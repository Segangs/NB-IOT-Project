import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260727170000"
STEM = "message_queue_throughput_ordering"
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


class MessageQueueThroughputOrderingMigrationTests(unittest.TestCase):
    def test_all_review_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY, EXPECTED):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_claim_function_keeps_atomic_fenced_selection(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "create or replace function public.claim_msg_send("
            "p_lock_owner text,p_batch_size integer,"
            "p_lease_seconds integer)",
            sql,
        )
        self.assertIn("returns setof public.msg_send", sql)
        self.assertIn("security definer set search_path = ''", sql)
        self.assertIn("for update skip locked", sql)
        self.assertIn("limit p_batch_size", sql)
        self.assertIn("lease_generation = m.lease_generation + 1", sql)
        self.assertIn("lease_token = pg_catalog.gen_random_uuid()", sql)

    def test_power_event_successor_waits_for_active_predecessor(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "m.source_event_origin <> 'device_power_event' or not exists",
            sql,
        )
        for equality in (
            "p.source_event_origin = 'device_power_event'",
            "p.source_device_id = m.source_device_id",
            "p.incident_id = m.incident_id",
            "p.source_event_sequence < m.source_event_sequence",
        ):
            self.assertIn(equality, sql)
        self.assertIn(
            "p.status in ('pending','processing','accepted',"
            "'waiting_result','retry_wait')",
            sql,
        )
        self.assertNotIn(
            "p.status in ('sent','failed','suppressed','cancelled')",
            sql,
        )

    def test_partial_index_matches_predecessor_gate(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "create index idx_msg_send_power_incident_order "
            "on public.msg_send (source_device_id,incident_id,"
            "source_event_sequence,msg_send_id)",
            sql,
        )
        self.assertIn(
            "where source_event_origin = 'device_power_event' "
            "and status in ('pending','processing','accepted',"
            "'waiting_result','retry_wait')",
            sql,
        )

    def test_function_privileges_remain_service_role_only(self) -> None:
        sql = compact(UP)
        signature = "public.claim_msg_send(text,integer,integer)"
        self.assertIn(
            f"revoke all on function {signature} "
            "from public,anon,authenticated",
            sql,
        )
        self.assertIn(
            f"grant execute on function {signature} to service_role",
            sql,
        )

    def test_precheck_and_verify_are_read_only_and_bounded(self) -> None:
        precheck = compact(PRECHECK)
        verify = compact(VERIFY)
        self.assertTrue(precheck.startswith("begin read only;"))
        self.assertTrue(precheck.endswith("rollback;"))
        self.assertIn("claim_msg_send(text,integer,integer)", precheck)
        self.assertIn("required msg_send column contract drift", precheck)
        self.assertIn("idx_msg_send_power_incident_order", precheck)
        self.assertTrue(verify.startswith("begin read only;"))
        self.assertTrue(verify.endswith("rollback;"))
        self.assertIn("pg_get_functiondef", verify)
        self.assertIn("regexp_replace", verify)
        self.assertIn("has_function_privilege", verify)
        self.assertIn("idx_msg_send_power_incident_order", verify)

    def test_rollback_restores_previous_claim_contract(self) -> None:
        sql = compact(DOWN)
        self.assertIn(
            "drop index public.idx_msg_send_power_incident_order",
            sql,
        )
        self.assertIn(
            "create or replace function public.claim_msg_send",
            sql,
        )
        self.assertIn(
            "where m.status in ('pending','retry_wait') "
            "and m.available_at <= v_now",
            sql,
        )
        self.assertNotIn("p.source_event_sequence", sql)
        self.assertNotIn("cascade", sql)

    def test_expected_result_records_speed_order_and_rollback(self) -> None:
        expected = source(EXPECTED)
        self.assertIn("1분", expected)
        self.assertIn("최대 20건", expected)
        self.assertIn("50초", expected)
        self.assertIn("같은 전원 사건", expected)
        self.assertIn("rollback", expected.lower())

    def test_sql_transactions_and_function_quotes_are_balanced(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY):
            with self.subTest(path=path):
                sql = source(path).lower()
                self.assertTrue(sql.strip().startswith("begin"))
                self.assertTrue(
                    sql.strip().endswith("commit;")
                    or sql.strip().endswith("rollback;")
                )
                self.assertEqual(sql.count("$function$") % 2, 0)


if __name__ == "__main__":
    unittest.main()
