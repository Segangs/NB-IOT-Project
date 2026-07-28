import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727130000"
STEM = "power_event_alert_automation"
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


class PowerEventAlertAutomationMigrationTests(unittest.TestCase):
    def test_all_review_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY, EXPECTED):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_table_has_exact_idempotency_and_rls_boundary(self) -> None:
        sql = compact(UP)
        self.assertIn("create table public.device_power_event", sql)
        self.assertIn(
            "unique (source_device_id,source_origin,event_type,"
            "incident_id,event_sequence)",
            sql,
        )
        self.assertIn(
            "alter table public.device_power_event enable row level security",
            sql,
        )
        self.assertIn(
            "revoke all on table public.device_power_event "
            "from public,anon,authenticated",
            sql,
        )

    def test_rpc_validates_secret_identity_and_exact_wire_contract(self) -> None:
        sql = compact(UP)
        signature = (
            "create function public.ingest_device_power_event("
            "p_imei text,p_schema smallint,p_event_type smallint,"
            "p_incident_id bigint,p_sequence smallint,p_state smallint,"
            "p_value0 integer,p_value1 integer,p_unix_seconds bigint,"
            "p_clock_valid boolean,p_request_secret text)"
        )
        self.assertIn(signature, sql)
        self.assertIn("nb_iot_event_gateway_secret", sql)
        self.assertIn(
            "p_request_secret is distinct from v_expected_request_secret",
            sql,
        )
        self.assertIn('where d."deviceimei" = p_imei', sql)
        for canonical in (
            "p_event_type = 4 and p_sequence = 1 and p_state = 1 "
            "and p_value0 = 0 and p_value1 = 0",
            "p_event_type = 5 and p_sequence = 2 and p_state = 0 "
            "and p_value0 = 1 and p_value1 = 0",
            "p_event_type = 6 and p_sequence = 2 and p_state = 2 "
            "and p_value0 = 210 and p_value1 = 90",
        ):
            self.assertIn(canonical, sql)

    def test_exact_three_templates_and_two_links_are_enqueued(self) -> None:
        sql = compact(UP)
        for event_type, key, template_code in (
            (4, "adapter_removed", "bizp_2026071315030116762724896"),
            (5, "adapter_restored", "bizp_2026071315101276625891453"),
            (6, "power_shutdown", "bizp_2026071315073916762730996"),
        ):
            with self.subTest(event_type=event_type):
                self.assertIn(f"when {event_type} then '{key}'", sql)
                self.assertIn(template_code, sql)
        self.assertIn("insert into public.msg_send", sql)
        self.assertIn("insert into public.message_link_token", sql)
        self.assertIn("'temperature_history'", sql)
        self.assertIn("'device_settings'", sql)
        self.assertIn("'workplacename'", sql)
        self.assertIn("'usermachinename'", sql)
        self.assertIn("'eventtime'", sql)

    def test_duplicate_event_does_not_enqueue_again(self) -> None:
        sql = compact(UP)
        self.assertIn("on conflict on constraint device_power_event_idempotency_unique", sql)
        self.assertIn("if v_inserted then", sql)
        self.assertIn("'duplicate',not v_inserted", sql)
        self.assertIn(
            "'power_event:' || v_device_id::text || ':' "
            "|| p_event_type::text || ':' || p_incident_id::text "
            "|| ':' || p_sequence::text",
            sql,
        )

    def test_function_is_hardened_but_callable_by_emqx_anon_key(self) -> None:
        sql = compact(UP)
        self.assertIn("security definer set search_path = ''", sql)
        signature = (
            "public.ingest_device_power_event(text,smallint,smallint,bigint,"
            "smallint,smallint,integer,integer,bigint,boolean,text)"
        )
        self.assertIn(
            f"revoke all on function {signature} "
            "from public,anon,authenticated",
            sql,
        )
        self.assertIn(
            f"grant execute on function {signature} to anon,service_role",
            sql,
        )

    def test_precheck_verify_and_rollback_are_bounded(self) -> None:
        precheck = compact(PRECHECK)
        verify = compact(VERIFY)
        down = compact(DOWN)
        self.assertTrue(precheck.startswith("begin read only;"))
        self.assertTrue(precheck.endswith("rollback;"))
        self.assertIn("nb_iot_config_request_secret", precheck)
        self.assertIn("required power event relation is missing", precheck)
        self.assertTrue(verify.startswith("begin read only;"))
        self.assertTrue(verify.endswith("rollback;"))
        self.assertIn("device_power_event_idempotency_unique", verify)
        self.assertIn("has_function_privilege", verify)
        self.assertIn(
            "power event rows exist; explicit data decision required",
            down,
        )
        self.assertNotIn("cascade", down)

    def test_expected_result_records_provisional_hardware_gate(self) -> None:
        expected = source(EXPECTED).lower()
        self.assertIn("300초", expected)
        self.assertIn("provisional", expected)
        self.assertIn("실측", expected)
        self.assertIn("live apply", expected)
        self.assertIn("별도 승인", expected)


if __name__ == "__main__":
    unittest.main()
