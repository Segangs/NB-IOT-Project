import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260726193000"
STEM = "message_acceptance_enqueue"
UP = ROOT / "supabase" / "migrations" / f"{VERSION}_{STEM}.sql"
DOWN = ROOT / "supabase" / "rollbacks" / f"{VERSION}_{STEM}_down.sql"
PRECHECK = (
    ROOT / "supabase" / "prechecks" / f"{VERSION}_{STEM}_precheck.sql"
)
VERIFY = (
    ROOT / "supabase" / "verifications" / f"{VERSION}_{STEM}_verify.sql"
)


def source(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def compact(path: Path) -> str:
    without_comments = re.sub(r"--[^\n]*", " ", source(path).lower())
    value = re.sub(r"\s+", " ", without_comments).strip()
    value = re.sub(r"\(\s+", "(", value)
    value = re.sub(r"\s+\)", ")", value)
    return re.sub(r",\s+", ",", value)


class MessageAcceptanceEnqueueMigrationContractTests(unittest.TestCase):
    def test_all_additive_review_artifacts_exist(self) -> None:
        for path in (UP, DOWN, PRECHECK, VERIFY):
            with self.subTest(path=path):
                self.assertTrue(path.is_file(), f"missing artifact: {path}")

    def test_up_adds_only_two_service_role_functions(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "create function public.finalize_msg_send_provider_acceptance(",
            sql,
        )
        self.assertIn(
            "create function public.enqueue_temp_alert_msg_send(",
            sql,
        )
        self.assertNotRegex(sql, r"\b(create|alter|drop|truncate)\s+table\b")
        self.assertNotIn("create or replace function", sql)
        self.assertEqual(sql.count("security definer"), 2)
        self.assertEqual(sql.count("set search_path = ''"), 2)

    def test_special_sql_expressions_are_not_schema_qualified(self) -> None:
        for path in (UP, PRECHECK, VERIFY, DOWN):
            with self.subTest(path=path):
                self.assertNotRegex(
                    source(path).lower(),
                    r"\bpg_catalog\."
                    r"(coalesce|greatest|least|nullif|position)\s*\(",
                )

    def test_acceptance_finish_reuses_existing_atomic_fences(self) -> None:
        sql = compact(UP)
        start = sql.index(
            "create function public.finalize_msg_send_provider_acceptance("
        )
        end = sql.index(
            "create function public.enqueue_temp_alert_msg_send("
        )
        body = sql[start:end]
        self.assertIn(
            "public.mark_msg_send_submission_waiting_result(",
            body,
        )
        self.assertIn("public.record_msg_send_push_result(", body)
        self.assertIn("'acceptance:' || p_lease_token::text", body)
        self.assertIn("'1000'", body)
        self.assertIn("p_delivered => true", body)
        self.assertIn("p_retryable => false", body)
        self.assertIn("p_channel is distinct from 'alimtalk'", body)

    def test_enqueue_is_temp_only_and_creates_both_links(self) -> None:
        sql = compact(UP)
        body = sql[
            sql.index("create function public.enqueue_temp_alert_msg_send(") :
        ]
        self.assertIn("'temperature_high'", body)
        self.assertIn("'temperature_recovered'", body)
        self.assertIn("bizp_2026070914231016762370714", body)
        self.assertIn("bizp_2026071314514616762878062", body)
        self.assertIn("insert into public.msg_send", body)
        self.assertIn("insert into public.message_link_token", body)
        self.assertIn("'temperature_history'", body)
        self.assertIn("'device_settings'", body)
        self.assertIn("public.device", body)
        self.assertIn('public."user_sensor"', body)
        self.assertIn('public."sensor_ctgy"', body)
        self.assertIn("consent_status is distinct from 'granted'", body)
        self.assertIn("allow_sms_fallback", body)
        self.assertIn("pg_catalog.pg_advisory_xact_lock", body)

    def test_enqueue_counts_json_object_keys_with_postgres_function(
        self,
    ) -> None:
        sql = compact(UP)
        self.assertNotIn("jsonb_object_length", sql)
        self.assertEqual(
            sql.count(
                "select pg_catalog.count(*) from "
                "pg_catalog.jsonb_object_keys(p_template_params)"
            ),
            2,
        )

    def test_enqueue_never_returns_raw_tokens(self) -> None:
        sql = compact(UP)
        signature = sql[
            sql.index("create function public.enqueue_temp_alert_msg_send(") :
            sql.index("language plpgsql", sql.index(
                "create function public.enqueue_temp_alert_msg_send("
            ))
        ]
        self.assertIn("returns bigint", signature)
        self.assertNotIn("returns table", signature)
        self.assertIn("return v_message_id", sql)
        self.assertNotIn("return v_history_token", sql)
        self.assertNotIn("return v_settings_token", sql)
        self.assertIn("extensions.digest(v_history_token,'sha256')", sql)
        self.assertIn("extensions.digest(v_settings_token,'sha256')", sql)

    def test_exact_replay_requires_matching_message_and_two_links(self) -> None:
        sql = compact(UP)
        self.assertIn("where m.dedupe_key = p_dedupe_key", sql)
        self.assertIn(
            "v_existing.template_params "
            "- 'temphistorytoken' - 'settingstoken'",
            sql,
        )
        self.assertIn("count(*) filter", sql)
        self.assertIn("temperature_history", sql)
        self.assertIn("device_settings", sql)
        self.assertIn(
            "extensions.digest(v_existing.template_params "
            "->> 'temphistorytoken','sha256')",
            sql,
        )
        self.assertIn(
            "extensions.digest(v_existing.template_params "
            "->> 'settingstoken','sha256')",
            sql,
        )
        self.assertIn("enqueue replay mismatch", sql)

    def test_enqueue_locks_authorization_rows_through_insert(self) -> None:
        sql = compact(UP)
        self.assertIn(
            "where c.alert_contact_id = p_alert_contact_id "
            "for share of c",
            sql,
        )
        self.assertIn(
            "where p.message_policy_id = p_message_policy_id "
            "for share of p",
            sql,
        )
        self.assertIn(
            'where d."deviceid" = p_source_device_id for share of d',
            sql,
        )
        self.assertIn("for share of w", sql)
        self.assertIn("for share of s,c", sql)

    def test_functions_are_service_role_only(self) -> None:
        sql = compact(UP)
        signatures = (
            "public.finalize_msg_send_provider_acceptance"
            "(bigint,text,uuid,text,text,timestamptz,numeric)",
            "public.enqueue_temp_alert_msg_send"
            "(bigint,bigint,integer,integer,text,bigint,bigint,text,"
            "jsonb,text,timestamptz,smallint)",
        )
        for signature in signatures:
            with self.subTest(signature=signature):
                self.assertIn(
                    f"revoke all on function {signature} "
                    "from public,anon,authenticated",
                    sql,
                )
                self.assertIn(
                    f"grant execute on function {signature} to service_role",
                    sql,
                )

    def test_precheck_and_verify_guard_the_live_baseline(self) -> None:
        precheck = compact(PRECHECK)
        verify = compact(VERIFY)
        for relation in (
            "public.msg_send",
            "public.message_link_token",
            "public.alert_contact",
            "public.message_policy",
            "public.device",
            'public."user_sensor"',
            'public."sensor_ctgy"',
        ):
            with self.subTest(relation=relation):
                self.assertIn(relation, precheck)
        self.assertIn(
            "public.mark_msg_send_submission_waiting_result",
            precheck,
        )
        self.assertIn("public.record_msg_send_push_result", precheck)
        self.assertIn("extensions.digest(text,text)", precheck)
        self.assertIn("raise exception", precheck)
        self.assertIn("prosecdef", verify)
        self.assertIn("proconfig", verify)
        self.assertIn("has_function_privilege", verify)
        self.assertIn("service_role", verify)
        self.assertIn("anon", verify)
        self.assertIn("authenticated", verify)

    def test_verify_accepts_postgres_empty_search_path_catalog_forms(
        self,
    ) -> None:
        verify = compact(VERIFY)
        self.assertIn("unnest(", verify)
        self.assertIn(
            "cfg ~ '^search_path=(|\\\"\\\")$'",
            verify,
        )
        self.assertNotIn(
            "proconfig is distinct from array['search_path=']",
            verify,
        )

    def test_rollback_removes_only_the_two_new_functions(self) -> None:
        sql = compact(DOWN)
        self.assertEqual(sql.count("drop function"), 2)
        self.assertNotRegex(sql, r"\b(drop|alter|truncate)\s+table\b")
        self.assertIn(
            "drop function public.enqueue_temp_alert_msg_send",
            sql,
        )
        self.assertIn(
            "drop function public.finalize_msg_send_provider_acceptance",
            sql,
        )


if __name__ == "__main__":
    unittest.main()
