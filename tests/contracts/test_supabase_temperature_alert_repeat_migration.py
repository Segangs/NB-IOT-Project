import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727070000"
STEM = "temperature_alert_repeat"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_{STEM}.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_{STEM}_down.sql"
PRECHECK = ROOT / "supabase" / "prechecks" / f"{VERSION}_{STEM}_precheck.sql"
VERIFY = ROOT / "supabase" / "verifications" / f"{VERSION}_{STEM}_verify.sql"
REHEARSAL = (
    ROOT / "supabase" / "rehearsals" / f"{VERSION}_{STEM}_behavior.sql"
)


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def compact(path: Path) -> str:
    value = re.sub(r"--[^\n]*", " ", source(path).lower())
    value = re.sub(r"\s+", " ", value).strip()
    value = re.sub(r"\(\s+", "(", value)
    value = re.sub(r"\s+\)", ")", value)
    return re.sub(r",\s+", ",", value)


class TemperatureAlertRepeatMigrationContractTests(unittest.TestCase):
    def test_all_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY, REHEARSAL):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_state_and_outbox_identify_three_notifications(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "add column high_notification_count smallint",
            sql,
        )
        self.assertIn(
            "add column last_high_notification_at timestamptz",
            sql,
        )
        self.assertIn(
            "add column notification_ordinal smallint not null default 1",
            sql,
        )
        self.assertIn(
            "unique (user_sensor_pk,incident_id,event_key,"
            "notification_ordinal)",
            sql,
        )
        self.assertIn("high_notification_count between 0 and 3", sql)
        self.assertIn("notification_ordinal between 1 and 3", sql)

    def test_repeat_transition_uses_database_twenty_minute_gate(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "v_state.high_notification_count < 3",
            sql,
        )
        self.assertIn(
            "v_state.last_high_notification_at + interval '20 minutes'",
            sql,
        )
        self.assertIn(
            "v_notification_ordinal := "
            "v_state.high_notification_count + 1",
            sql,
        )
        self.assertNotIn(
            'new."sensorvaluetime" + interval \'20 minutes\'',
            sql,
        )

    def test_existing_incident_is_backfilled_without_new_outbox(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "count(*) filter (where o.event_key = 'temperature_high')",
            sql,
        )
        self.assertIn(
            "max(o.created_at) filter "
            "(where o.event_key = 'temperature_high')",
            sql,
        )
        backfill_end = sql.index(
            "create or replace function "
            "public.capture_temperature_alert_transition("
        )
        self.assertNotIn(
            "insert into public.temperature_alert_outbox",
            sql[:backfill_end],
        )

    def test_drain_dedupe_includes_notification_ordinal(self) -> None:
        sql = compact(UP)
        marker = (
            "create or replace function "
            "public.drain_temperature_alert_outbox("
        )
        self.assertIn(marker, sql)
        drain = sql[sql.index(marker) :]
        self.assertIn(
            "|| ':' || v_item.notification_ordinal::text",
            drain,
        )
        self.assertIn(
            "public.enqueue_temp_alert_msg_send(",
            drain,
        )
        self.assertIn("if v_item.event_key = 'temperature_high'", drain)

    def test_precheck_and_rollback_are_fail_closed(self) -> None:
        precheck = compact(PRECHECK)
        down = compact(DOWN)
        self.assertIn("base temperature alert contract is missing", precheck)
        self.assertIn("processing outbox must be zero", precheck)
        self.assertIn(
            "temperature_alert_outbox_incident_event_unique",
            precheck,
        )
        self.assertIn("notification_ordinal > 1", down)
        self.assertIn("high_notification_count > 1", down)
        self.assertIn(
            "explicit data decision required",
            down,
        )
        self.assertNotIn("cascade", down)

    def test_verify_and_rehearsal_cover_behavior_and_security(self) -> None:
        verify = compact(VERIFY)
        rehearsal = compact(REHEARSAL)
        for token in (
            "high_notification_count",
            "last_high_notification_at",
            "notification_ordinal",
            "interval '20 minutes'",
            "service_role",
            "relrowsecurity",
            "has_function_privilege",
        ):
            self.assertIn(token, verify)
        self.assertTrue(rehearsal.startswith("begin;"))
        self.assertTrue(rehearsal.endswith("rollback;"))
        self.assertNotIn("commit;", rehearsal)
        self.assertIn("interval '20 minutes'", rehearsal)
        self.assertIn("notification_ordinal = 3", rehearsal)
        self.assertIn("temperature_recovered", rehearsal)


if __name__ == "__main__":
    unittest.main()
