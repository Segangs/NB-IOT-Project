import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727060000"
STEM = "temperature_alert_automation"
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


class TemperatureAlertAutomationMigrationContractTests(unittest.TestCase):
    def test_all_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY, EXPECTED):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_state_and_outbox_are_service_role_only(self) -> None:
        sql = compact(UP)
        self.assertIn("create table public.temperature_alert_state", sql)
        self.assertIn("create table public.temperature_alert_outbox", sql)
        self.assertEqual(sql.count("enable row level security"), 2)
        self.assertNotIn(
            "grant select on table public.temperature_alert_",
            sql,
        )
        self.assertEqual(
            sql.count("grant select,insert,update,delete on table"),
            2,
        )
        self.assertIn("to service_role", sql)

    def test_transition_is_temp_only_and_edge_only(self) -> None:
        sql = compact(UP)
        body = sql[
            sql.index(
                "create function public.capture_temperature_alert_transition("
            ) :
            sql.index(
                "create function public.drain_temperature_alert_outbox("
            )
        ]
        self.assertIn('"sensorctgytype" = \'tmp\'', body)
        self.assertIn('new."sensorvalueid"', body)
        self.assertIn("temperature_high", body)
        self.assertIn("temperature_recovered", body)
        self.assertIn("on conflict do nothing", body)
        self.assertNotIn("enqueue_temp_alert_msg_send", body)

    def test_drain_reuses_existing_enqueue_boundary(self) -> None:
        sql = compact(UP)
        body = sql[
            sql.index(
                "create function public.drain_temperature_alert_outbox("
            ) :
        ]
        self.assertIn("public.enqueue_temp_alert_msg_send(", body)
        self.assertIn("for update skip locked", body)
        self.assertIn("contact_cardinality", body)
        self.assertIn("policy_cardinality", body)
        self.assertIn("ownership_missing", body)
        self.assertIn("enqueue_failed", body)
        self.assertIn("temperature:<deviceid>", source(EXPECTED).lower())

    def test_precheck_guards_live_dependencies(self) -> None:
        sql = compact(PRECHECK)
        self.assertIn(
            "to_regclass('public.temperature_alert_state') is not null",
            sql,
        )
        self.assertIn(
            "to_regprocedure('public.enqueue_temp_alert_msg_send"
            "(bigint,bigint,integer,integer,text,bigint,bigint,text,"
            "jsonb,text,timestamp with time zone,smallint)') is null",
            sql,
        )
        for relation in (
            "public.sensorvalue",
            'public."user_sensor"',
            'public."sensor_ctgy"',
            "public.device",
            "public.userworkplace",
            "public.usermachine",
            "public.alert_contact",
            "public.message_policy",
        ):
            self.assertIn(relation, sql)
        self.assertIn("active consented contact", sql)
        self.assertIn("active alimtalk-only policy", sql)

    def test_functions_are_hardened_and_service_role_only(self) -> None:
        sql = compact(UP)
        self.assertEqual(sql.count("security definer"), 2)
        self.assertEqual(sql.count("set search_path = ''"), 2)
        self.assertIn(
            "revoke all on function "
            "public.drain_temperature_alert_outbox(integer) "
            "from public,anon,authenticated",
            sql,
        )
        self.assertIn(
            "grant execute on function "
            "public.drain_temperature_alert_outbox(integer) "
            "to service_role",
            sql,
        )

    def test_verification_and_rollback_are_bounded(self) -> None:
        verify = compact(VERIFY)
        down = compact(DOWN)
        self.assertIn("relrowsecurity", verify)
        self.assertIn("relowner", verify)
        self.assertIn("pg_policies", verify)
        self.assertIn("has_table_privilege", verify)
        self.assertIn("temperature_alert_outbox_claim_idx", verify)
        self.assertIn("has_function_privilege", verify)
        self.assertIn("service_role", verify)
        self.assertIn("anon", verify)
        self.assertEqual(down.count("drop trigger"), 1)
        self.assertEqual(down.count("drop function"), 2)
        self.assertEqual(down.count("drop table"), 2)
        self.assertEqual(down.count("drop sequence"), 1)
        self.assertNotIn("cascade", down)


if __name__ == "__main__":
    unittest.main()
