import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727071000"
STEM = "temperature_alert_repeat_msg_send"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_{STEM}.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_{STEM}_down.sql"
PRECHECK = ROOT / "supabase" / "prechecks" / f"{VERSION}_{STEM}_precheck.sql"
VERIFY = ROOT / "supabase" / "verifications" / f"{VERSION}_{STEM}_verify.sql"


def compact(path: Path) -> str:
    value = path.read_text(encoding="utf-8").lower() if path.is_file() else ""
    value = re.sub(r"--[^\n]*", " ", value)
    value = re.sub(r"\s+", " ", value).strip()
    value = re.sub(r"\(\s+", "(", value)
    value = re.sub(r"\s+\)", ")", value)
    return re.sub(r",\s+", ",", value)


class TemperatureAlertRepeatMsgSendMigrationTests(unittest.TestCase):
    def test_all_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_unique_keeps_event_identity_but_allows_repeat_stage(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "drop constraint msg_send_incident_template_stage_unique",
            sql,
        )
        self.assertIn(
            "unique (source_device_id,incident_id,template_code,"
            "template_stage,alert_contact_id,source_event_sequence)",
            sql,
        )
        self.assertNotIn("drop constraint msg_send_dedupe_key_unique", sql)
        self.assertNotIn("drop index msg_send_dedupe_key_unique", sql)

    def test_precheck_and_rollback_are_fail_closed(self) -> None:
        precheck = compact(PRECHECK)
        down = compact(DOWN)
        self.assertIn(
            "msg_send_incident_template_stage_unique drift",
            precheck,
        )
        self.assertIn("processing msg_send must be zero", precheck)
        self.assertIn(
            "repeat msg_send data exists; explicit data decision required",
            down,
        )
        self.assertIn(
            "group by source_device_id,incident_id,template_code,"
            "template_stage,alert_contact_id",
            down,
        )
        self.assertNotIn("cascade", down)

    def test_verify_checks_new_unique_and_dedupe_survival(self) -> None:
        verify = compact(VERIFY)
        self.assertIn(
            "unique(source_device_id,incident_id,template_code,"
            "template_stage,alert_contact_id,source_event_sequence)",
            verify,
        )
        self.assertIn("msg_send_dedupe_key_unique", verify)
        self.assertIn("has_table_privilege", verify)
        self.assertIn("relrowsecurity", verify)
        self.assertTrue(verify.startswith("begin read only;"))
        self.assertTrue(verify.endswith("rollback;"))


if __name__ == "__main__":
    unittest.main()
