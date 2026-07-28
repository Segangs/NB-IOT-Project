import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727190000"
STEM = "device_temperature_settings"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_{STEM}.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_{STEM}_down.sql"
PRECHECK = ROOT / "supabase" / "prechecks" / f"{VERSION}_{STEM}_precheck.sql"
VERIFY = ROOT / "supabase" / "verifications" / f"{VERSION}_{STEM}_verify.sql"
BEHAVIOR = ROOT / "supabase" / "rehearsals" / f"{VERSION}_{STEM}_behavior.sql"
EXPECTED = ROOT / "supabase" / f"{STEM}_expected.md"


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def compact(path: Path) -> str:
    value = re.sub(r"--[^\n]*", " ", source(path).lower())
    value = re.sub(r"\s+", " ", value).strip()
    value = re.sub(r"\(\s+", "(", value)
    value = re.sub(r"\s+\)", ")", value)
    return re.sub(r",\s+", ",", value)


class DeviceTemperatureSettingsMigrationContractTests(
    unittest.TestCase
):
    def test_all_lifecycle_artifacts_exist(self) -> None:
        for path in (
            UP,
            DOWN,
            PRECHECK,
            VERIFY,
            BEHAVIOR,
            EXPECTED,
        ):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_up_is_transactional_bounded_and_additive(self) -> None:
        sql = compact(UP)
        self.assertTrue(sql.startswith("begin;"))
        self.assertTrue(sql.endswith("commit;"))
        self.assertIn("set local lock_timeout = '5s'", sql)
        self.assertIn("set local statement_timeout = '30s'", sql)
        self.assertIn(
            "create table public.temperature_alert_preference",
            sql,
        )
        self.assertIn(
            "references public.\"user_sensor\" (\"id\") on delete cascade",
            sql,
        )
        self.assertIn(
            "check (max_notifications between 1 and 3)",
            sql,
        )
        self.assertNotIn("drop table public.\"user_sensor\"", sql)
        self.assertNotIn(
            "drop table public.temperature_alert_preference cascade",
            sql,
        )

    def test_get_and_update_rpcs_are_service_role_only_and_hardened(
        self,
    ) -> None:
        sql = compact(UP)
        for signature in (
            "public.get_device_temperature_settings(integer,integer,integer)",
            "public.update_device_temperature_settings(integer,integer,integer,jsonb)",
        ):
            self.assertIn(f"revoke all on function {signature}", sql)
            self.assertIn(f"grant execute on function {signature}", sql)
        self.assertGreaterEqual(sql.count("security definer"), 3)
        self.assertGreaterEqual(sql.count("set search_path = ''"), 3)
        self.assertIn("pg_advisory_xact_lock", sql)
        self.assertIn("jsonb_array_elements", sql)
        self.assertIn("jsonb_object_keys", sql)
        self.assertNotIn("jsonb_object_length", sql)
        self.assertIn("settmpuplimit", sql)
        self.assertIn(
            "on conflict on constraint "
            "temperature_alert_preference_pkey do update",
            sql,
        )
        self.assertNotIn("on conflict (user_sensor_pk) do update", sql)

    def test_settings_rpc_enforces_owner_temp_scope_and_atomic_validation(
        self,
    ) -> None:
        sql = compact(UP)
        for token in (
            "c.\"sensorctgytype\" = 'tmp'",
            "d.\"userid\" = p_user_id",
            "d.\"userworkplaceid\" = p_workplace_id",
            "jsonb_typeof(p_updates) <> 'array'",
            "upper limit must be -50..25 in 0.5 degree steps",
            "max notifications must be between 1 and 3",
            "duplicate sensor update",
            "sensor update escaped owner device scope",
        ):
            self.assertIn(token, sql)

    def test_trigger_uses_per_sensor_limit_with_default_three(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "coalesce(p.max_notifications,3)::smallint",
            sql,
        )
        self.assertIn(
            "v_state.high_notification_count < v_max_notifications",
            sql,
        )
        self.assertIn(
            "v_state.last_high_notification_at + interval '20 minutes'",
            sql,
        )
        self.assertNotIn(
            "v_state.high_notification_count < 3",
            sql,
        )

    def test_rls_acl_verify_and_precheck_fail_closed(self) -> None:
        precheck = compact(PRECHECK)
        verify = compact(VERIFY)
        self.assertIn("base temperature repeat contract is missing", precheck)
        self.assertIn("processing outbox must be zero", precheck)
        self.assertIn("target settings object already exists", precheck)
        for token in (
            "temperature_alert_preference",
            "relrowsecurity",
            "service_role",
            "has_function_privilege",
            "on conflict on constraint",
            "temperature_alert_preference_pkey",
            "v_max_notifications",
            "interval '20 minutes'",
            "max_notifications >= 1",
            "max_notifications <= 3",
        ):
            self.assertIn(token, verify)

    def test_behavior_proves_default_limits_atomicity_and_incident_change(
        self,
    ) -> None:
        behavior = compact(BEHAVIOR)
        self.assertTrue(behavior.startswith("begin;"))
        self.assertTrue(behavior.endswith("rollback;"))
        self.assertNotIn("commit;", behavior)
        for token in (
            "default max notifications must be three",
            "one-sensor update failed",
            "two-sensor update failed",
            "invalid second row did not roll back first row",
            "max one allowed an extra repeat",
            "increased max did not allow the next repeat",
            "interval '20 minutes'",
        ):
            self.assertIn(token, behavior)

    def test_down_restores_hardcoded_three_without_reverting_upper_values(
        self,
    ) -> None:
        down = compact(DOWN)
        expected = source(EXPECTED).lower()
        self.assertTrue(down.startswith("begin;"))
        self.assertTrue(down.endswith("commit;"))
        self.assertIn(
            "v_state.high_notification_count < 3",
            down,
        )
        self.assertIn(
            "drop table public.temperature_alert_preference",
            down,
        )
        self.assertNotIn("cascade", down)
        self.assertIn("상한 온도", expected)
        self.assertIn("자동 복원하지 않음", expected)


if __name__ == "__main__":
    unittest.main()
