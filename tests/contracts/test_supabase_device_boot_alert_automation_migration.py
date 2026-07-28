import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727110000"
STEM = "device_boot_alert_automation"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_{STEM}.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_{STEM}_down.sql"
PRECHECK = ROOT / "supabase" / "prechecks" / f"{VERSION}_{STEM}_precheck.sql"
VERIFY = ROOT / "supabase" / "verifications" / f"{VERSION}_{STEM}_verify.sql"
EXPECTED = ROOT / "supabase" / f"{STEM}_expected.md"


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def compact(path: Path) -> str:
    value = re.sub(r"--[^\n]*", " ", source(path).lower())
    value = re.sub(r"\s+", " ", value).strip()
    value = re.sub(r"\(\s+", "(", value)
    value = re.sub(r"\s+\)", ")", value)
    return re.sub(r",\s+", ",", value)


class DeviceBootAlertAutomationMigrationTests(unittest.TestCase):
    def test_all_review_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY, EXPECTED):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_trigger_accepts_only_normal_rpc_boots(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "create function public.capture_device_boot_alert()",
            sql,
        )
        self.assertIn(
            "create trigger trg_capture_device_boot_alert "
            "after insert on public.device_boot_logs",
            sql,
        )
        for status_column in (
            "flash_integrity",
            "ram_test",
            "at_status",
            "cpin_status",
        ):
            self.assertIn(f"new.{status_column} is distinct from 0", sql)
        self.assertIn(
            "nullif(current_setting('request.path',true),'') "
            "is distinct from '/rpc/b'",
            sql,
        )
        self.assertIn(
            "nullif(current_setting('request.method',true),'') "
            "is distinct from 'post'",
            sql,
        )

    def test_trigger_builds_exact_boot_message_and_two_links(self) -> None:
        sql = compact(UP)
        self.assertIn("bizp_2026071315003676625784727", sql)
        self.assertIn("'device_boot'", sql)
        for parameter in (
            "workplaceName",
            "userMachineName",
            "eventTime",
            "tempHistoryToken",
            "SettingsToken",
        ):
            self.assertIn(parameter.lower(), sql)
        self.assertIn("insert into public.msg_send", sql)
        self.assertIn("insert into public.message_link_token", sql)
        self.assertIn("extensions.digest(v_history_token,'sha256')", sql)
        self.assertIn("extensions.digest(v_settings_token,'sha256')", sql)
        self.assertIn("'temperature_history'", sql)
        self.assertIn("'device_settings'", sql)

    def test_device_lock_and_five_minute_cooldown_prevent_repeats(self) -> None:
        sql = compact(UP)
        self.assertIn("pg_catalog.pg_advisory_xact_lock(", sql)
        self.assertIn("interval '5 minutes'", sql)
        self.assertIn("m.source_event_type = 'device_boot'", sql)
        self.assertIn("m.source_device_id = new.\"deviceid\"", sql)
        self.assertIn(
            "'device_boot:' || new.\"deviceid\"::text "
            "|| ':' || new.id::text",
            sql,
        )

    def test_enqueue_failure_never_rejects_the_boot_log(self) -> None:
        sql = compact(UP)
        self.assertIn("exception when others then", sql)
        self.assertIn(
            "raise warning 'device boot alert enqueue failed [%]',sqlstate",
            sql,
        )
        self.assertGreaterEqual(sql.count("return new"), 3)

    def test_migration_is_additive_without_backfill_or_legacy_acl_changes(
        self,
    ) -> None:
        sql = compact(UP)
        self.assertNotIn("alter table public.device_boot_logs", sql)
        self.assertNotIn("grant insert on public.device_boot_logs", sql)
        self.assertNotIn("revoke insert on public.device_boot_logs", sql)
        self.assertNotIn("from public.device_boot_logs", sql)
        self.assertNotIn("for each statement", sql)
        self.assertIn("for each row", sql)

    def test_function_is_hardened_and_not_publicly_callable(self) -> None:
        sql = compact(UP)
        self.assertIn("security definer", sql)
        self.assertIn("set search_path = ''", sql)
        self.assertIn(
            "revoke all on function public.capture_device_boot_alert() "
            "from public,anon,authenticated",
            sql,
        )
        self.assertIn(
            "grant execute on function public.capture_device_boot_alert() "
            "to service_role",
            sql,
        )

    def test_precheck_verification_and_rollback_are_bounded(self) -> None:
        precheck = compact(PRECHECK)
        verify = compact(VERIFY)
        down = compact(DOWN)
        self.assertTrue(precheck.startswith("begin read only;"))
        self.assertTrue(precheck.endswith("rollback;"))
        self.assertIn("expected exactly one active alimtalk-only policy", precheck)
        self.assertIn("required device boot alert relation is missing", precheck)
        self.assertTrue(verify.startswith("begin read only;"))
        self.assertTrue(verify.endswith("rollback;"))
        self.assertIn("trg_capture_device_boot_alert", verify)
        self.assertIn("has_function_privilege", verify)
        self.assertEqual(down.count("drop trigger"), 1)
        self.assertEqual(down.count("drop function"), 1)
        self.assertNotIn("cascade", down)

    def test_expected_result_records_scope_and_known_security_followup(
        self,
    ) -> None:
        expected = source(EXPECTED).lower()
        self.assertIn("5분", expected)
        self.assertIn("backfill", expected)
        self.assertIn("request.path", expected)
        self.assertIn("rls", expected)
        self.assertIn("별도 승인", expected)


if __name__ == "__main__":
    unittest.main()
