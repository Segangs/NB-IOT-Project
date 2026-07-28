import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727232840"
STEM = "temporary_unlimited_temperature_alerts"
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


class TemporaryUnlimitedTemperatureAlertsMigrationTests(unittest.TestCase):
    def test_all_lifecycle_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY, BEHAVIOR):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_up_removes_only_count_cap_and_keeps_twenty_minute_cadence(
        self,
    ) -> None:
        sql = compact(UP)
        self.assertTrue(sql.startswith("begin;"))
        self.assertTrue(sql.endswith("commit;"))
        self.assertIn(
            "v_state.high_notification_count < 32767",
            sql,
        )
        self.assertIn("high_notification_count between 0 and 32767", sql)
        self.assertIn("notification_ordinal between 1 and 32767", sql)
        self.assertNotIn("interval '20 minutes'", sql)
        self.assertNotIn("drop table", sql)
        self.assertNotIn("delete from", sql)

    def test_rollback_is_fail_closed_after_fourth_notification(self) -> None:
        down = compact(DOWN)
        self.assertIn("high_notification_count > 3", down)
        self.assertIn("notification_ordinal > 3", down)
        self.assertIn(
            "explicit data decision required",
            down,
        )
        self.assertIn(
            "v_state.high_notification_count < v_max_notifications",
            down,
        )

    def test_verify_and_behavior_cover_fourth_notification(self) -> None:
        verify = compact(VERIFY)
        behavior = compact(BEHAVIOR)
        self.assertTrue(verify.startswith("begin read only;"))
        self.assertTrue(verify.endswith("rollback;"))
        self.assertIn("interval ''20 minutes''", verify)
        self.assertIn("notification_ordinal = 4", behavior)
        self.assertIn("v_after_count <> 4", behavior)
        self.assertTrue(behavior.endswith("rollback;"))


if __name__ == "__main__":
    unittest.main()
