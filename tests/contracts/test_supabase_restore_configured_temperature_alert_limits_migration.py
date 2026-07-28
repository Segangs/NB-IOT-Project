import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260729011815"
STEM = "restore_configured_temperature_alert_limits"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_{STEM}.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_{STEM}_down.sql"
PRECHECK = ROOT / "supabase" / "prechecks" / f"{VERSION}_{STEM}_precheck.sql"
VERIFY = ROOT / "supabase" / "verifications" / f"{VERSION}_{STEM}_verify.sql"
BEHAVIOR = ROOT / "supabase" / "rehearsals" / f"{VERSION}_{STEM}_behavior.sql"


def compact(path: Path) -> str:
    value = path.read_text(encoding="utf-8").lower() if path.is_file() else ""
    value = re.sub(r"--[^\n]*", " ", value)
    value = re.sub(r"\s+", " ", value).strip()
    value = re.sub(r"\(\s+", "(", value)
    value = re.sub(r"\s+\)", ")", value)
    return re.sub(r",\s+", ",", value)


class RestoreConfiguredTemperatureAlertLimitsMigrationTests(
    unittest.TestCase
):
    def test_all_lifecycle_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY, BEHAVIOR):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_up_restores_configured_limit_without_rewriting_history(
        self,
    ) -> None:
        sql = compact(UP)
        self.assertIn(
            "v_state.high_notification_count < v_max_notifications",
            sql,
        )
        self.assertIn(
            "v_state.high_notification_count < 32767",
            sql,
        )
        self.assertNotIn("delete from", sql)
        self.assertNotIn("update public.temperature_alert_state", sql)
        self.assertNotIn("alter table", sql)

    def test_verification_preserves_fourth_notification_history(
        self,
    ) -> None:
        verify = compact(VERIFY)
        self.assertIn("high_notification_count <= 32767", verify)
        self.assertIn("notification_ordinal <= 32767", verify)
        self.assertIn("high_notification_count > 3", verify)
        self.assertIn("max_notifications not between 1 and 3", verify)
        self.assertTrue(verify.startswith("begin read only;"))
        self.assertTrue(verify.endswith("rollback;"))

    def test_rollback_and_behavior_are_non_destructive(self) -> None:
        down = compact(DOWN)
        behavior = compact(BEHAVIOR)
        self.assertIn(
            "v_state.high_notification_count < 32767",
            down,
        )
        self.assertNotIn("delete from", down)
        self.assertNotIn("update public.temperature_alert_state", down)
        self.assertIn("v_after_count <> v_before_count", behavior)
        self.assertIn("v_after_outbox <> v_before_outbox", behavior)
        self.assertTrue(behavior.endswith("rollback;"))


if __name__ == "__main__":
    unittest.main()
