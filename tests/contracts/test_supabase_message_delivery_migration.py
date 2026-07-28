import re
import unittest
from pathlib import Path
from typing import Optional


ROOT = Path(__file__).resolve().parents[2]
VERSION = "20260726005145"
UP = (
    ROOT
    / "supabase"
    / "migrations"
    / f"{VERSION}_message_delivery_queue.sql"
)
DOWN = (
    ROOT
    / "supabase"
    / "rollbacks"
    / f"{VERSION}_message_delivery_queue_down.sql"
)
PRECHECK = (
    ROOT
    / "supabase"
    / "prechecks"
    / f"{VERSION}_message_delivery_queue_precheck.sql"
)
VERIFY = (
    ROOT
    / "supabase"
    / "verifications"
    / f"{VERSION}_message_delivery_queue_verify.sql"
)
EXPECTED = ROOT / "supabase" / "message_delivery_queue_expected.md"


def source(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8")


def compact_sql(value: str) -> str:
    without_line_comments = re.sub(r"--[^\n]*", " ", value.lower())
    compact = re.sub(r"\s+", " ", without_line_comments).strip()
    compact = re.sub(r"\(\s+", "(", compact)
    return re.sub(r"\s+\)", ")", compact)


def function_body(
    value: str, name: str, next_name: Optional[str] = None
) -> str:
    lowered = value.lower()
    anchor = f"create function public.{name}("
    start = lowered.find(anchor)
    if start < 0:
        return ""
    if next_name is None:
        return lowered[start:]
    end = lowered.find(f"create function public.{next_name}(", start + 1)
    return lowered[start:] if end < 0 else lowered[start:end]


class SupabaseMessageDeliveryMigrationContractTest(unittest.TestCase):
    def test_all_review_artifacts_exist(self):
        for path in (UP, DOWN, PRECHECK, VERIFY, EXPECTED):
            with self.subTest(path=path):
                self.assertTrue(
                    path.is_file(), f"missing message migration artifact: {path}"
                )

    def test_migration_adds_only_lowercase_message_objects(self):
        sql = compact_sql(source(UP))
        for table in (
            "alert_contact",
            "message_policy",
            "msg_send",
            "message_link_token",
        ):
            with self.subTest(table=table):
                self.assertIn(f"create table public.{table}", sql)

        self.assertIn("create function public.claim_msg_send(", sql)
        self.assertIn(
            "create function public.claim_exact_one_shot_msg_send(",
            sql,
        )
        self.assertIn(
            "create function public.mark_msg_send_submission_started(",
            sql,
        )
        self.assertIn(
            "create function public.mark_msg_send_submission_waiting_result(",
            sql,
        )
        self.assertIn(
            "create function public.complete_msg_send_claim(",
            sql,
        )
        self.assertIn(
            "create function public.recover_msg_send_leases(",
            sql,
        )
        self.assertIn(
            "create function public.record_msg_send_push_result(", sql
        )
        self.assertNotRegex(sql, r'create\s+(table|function)\s+public\."')
        self.assertNotRegex(
            sql,
            r"\b(alter|drop|truncate)\s+table\s+public\."
            r"(device|users|alertsend|devicecmds)\b",
        )
        self.assertNotRegex(
            sql,
            r"\b(create\s+or\s+replace|alter|drop)\s+function\s+"
            r"public\.(assign_device_command|ingest_device_event)\b",
        )

    def test_special_conditional_expressions_are_never_schema_qualified(self):
        for path in (UP, PRECHECK, VERIFY, DOWN):
            with self.subTest(path=path):
                self.assertNotRegex(
                    source(path).lower(),
                    r"\bpg_catalog\."
                    r"(coalesce|greatest|least|nullif|position)\s*\(",
                )

    def test_existing_user_and_device_ids_are_provenance_without_foreign_keys(
        self,
    ):
        sql = compact_sql(source(UP))
        for column in ("user_id", "source_user_id", "source_device_id"):
            with self.subTest(column=column):
                self.assertIn(f"{column} integer not null", sql)
        self.assertNotRegex(
            sql,
            r"references\s+public\.(users|device)\b",
        )

    def test_queue_states_and_timestamps_are_exact(self):
        sql = compact_sql(source(UP))
        expected_states = (
            "status in ('pending', 'processing', 'accepted', "
            "'waiting_result', 'retry_wait', 'sent', 'failed', "
            "'suppressed', 'cancelled')"
        )
        self.assertIn(expected_states, sql)
        self.assertIn(
            "active_channel in ('alimtalk', 'sms')",
            sql,
        )
        self.assertIn(
            "delivered_channel is null or "
            "delivered_channel in ('alimtalk', 'sms')",
            sql,
        )
        self.assertGreaterEqual(sql.count("timestamptz"), 24)
        self.assertNotRegex(sql, r"\btimestamp\s+without\s+time\s+zone\b")
        self.assertIn("msg_send_lease_state_check", sql)
        self.assertIn("msg_send_terminal_state_check", sql)
        self.assertIn("msg_send_provider_result_check", sql)
        self.assertIn("msg_send_fallback_result_check", sql)
        self.assertIn(
            "active_channel = 'sms' and "
            "fallback_reason is not null and "
            "pg_catalog.length(fallback_reason) between 1 and 128 and "
            "provider_result_status is not distinct from 'failure'",
            sql,
        )

    def test_queue_state_constraints_are_null_safe_and_channel_consistent(
        self,
    ):
        sql = compact_sql(source(UP))
        self.assertIn(
            "attempt_count between 0 and max_attempts + case "
            "when active_channel = 'sms' then 1 else 0 end",
            sql,
        )
        self.assertIn(
            "status in ('processing', 'accepted') and "
            "lock_owner is not null and lease_token is not null",
            sql,
        )
        self.assertIn(
            "status not in ('processing', 'accepted') and "
            "lock_owner is null and lease_token is null",
            sql,
        )
        self.assertIn(
            "provider_submission_started_at is null and "
            "provider_submission_lease_token is null and "
            "provider_request_id is null and "
            "provider_accepted_at is null",
            sql,
        )
        self.assertIn(
            "fallback_submission_started_at is null and "
            "fallback_submission_lease_token is null and "
            "fallback_provider_request_id is null and "
            "fallback_provider_accepted_at is null",
            sql,
        )
        self.assertIn("provider_result_retryable is null", sql)
        self.assertIn("provider_result_retryable is not null", sql)
        self.assertIn("fallback_provider_result_retryable is null", sql)
        self.assertIn("fallback_provider_result_retryable is not null", sql)
        self.assertIn("constraint msg_send_submission_state_check", sql)
        self.assertIn(
            "status = 'accepted'",
            sql,
        )
        self.assertIn(
            "active_channel = 'alimtalk' and "
            "provider_submission_started_at is not null and "
            "provider_submission_lease_token is not null and "
            "provider_request_id is null and "
            "provider_accepted_at is null",
            sql,
        )
        self.assertIn(
            "active_channel = 'sms' and "
            "fallback_submission_started_at is not null and "
            "fallback_submission_lease_token is not null and "
            "fallback_provider_request_id is null and "
            "fallback_provider_accepted_at is null",
            sql,
        )
        self.assertIn(
            "status = 'waiting_result'",
            sql,
        )
        self.assertIn(
            "active_channel = 'alimtalk' and "
            "provider_submission_started_at is not null and "
            "provider_request_id is not null and "
            "provider_accepted_at is not null and "
            "provider_submission_lease_token is not null",
            sql,
        )
        self.assertIn(
            "active_channel = 'sms' and "
            "fallback_submission_started_at is not null and "
            "fallback_provider_request_id is not null and "
            "fallback_provider_accepted_at is not null and "
            "fallback_submission_lease_token is not null",
            sql,
        )
        self.assertIn(
            "fallback_reason is not null and "
            "pg_catalog.length(fallback_reason) between 1 and 128",
            sql,
        )
        self.assertIn(
            "provider_result_status is not distinct from 'failure'",
            sql,
        )
        self.assertIn("constraint msg_send_delivery_state_check", sql)
        self.assertIn(
            "delivered_channel = 'alimtalk' and "
            "active_channel = 'alimtalk' and "
            "provider_result_status is not distinct from 'success'",
            sql,
        )
        self.assertIn(
            "delivered_channel = 'sms' and "
            "active_channel = 'sms' and "
            "fallback_provider_result_status is not distinct from 'success'",
            sql,
        )

    def test_queue_records_provenance_dedupe_incident_attempt_and_cost(self):
        sql = compact_sql(source(UP))
        for column in (
            "source_user_id",
            "source_device_id",
            "source_event_origin",
            "source_event_type",
            "source_event_sequence",
            "incident_id",
            "incident_kind",
            "dedupe_key",
            "lock_owner",
            "lease_token",
            "lease_generation",
            "lease_expires_at",
            "attempt_count",
            "max_attempts",
            "provider_request_id",
            "provider_submission_started_at",
            "provider_submission_lease_token",
            "provider_result_id",
            "provider_result_status",
            "fallback_provider_request_id",
            "fallback_submission_started_at",
            "fallback_submission_lease_token",
            "fallback_provider_result_id",
            "fallback_reason",
            "cost_class",
            "estimated_cost",
            "final_cost",
        ):
            with self.subTest(column=column):
                self.assertRegex(sql, rf"\b{column}\b")

        self.assertIn("constraint msg_send_dedupe_key_unique unique (dedupe_key)", sql)
        self.assertIn(
            "constraint msg_send_incident_template_stage_unique unique "
            "(source_device_id, incident_id, template_code, template_stage, "
            "alert_contact_id)",
            sql,
        )
        self.assertIn("idx_msg_send_claim", sql)
        self.assertIn("where status in ('pending', 'retry_wait')", sql)
        self.assertIn("idx_msg_send_lease", sql)
        self.assertIn(
            "where status in ('processing', 'accepted')",
            sql,
        )
        self.assertIn(
            "create unique index uq_msg_send_provider_request "
            "on public.msg_send (provider, provider_request_id) "
            "where provider_request_id is not null",
            sql,
        )
        self.assertIn(
            "create unique index uq_msg_send_fallback_provider_request "
            "on public.msg_send "
            "(fallback_provider, fallback_provider_request_id) "
            "where fallback_provider_request_id is not null",
            sql,
        )

    def test_contact_and_policy_capture_consent_provenance_fallback_and_cost(self):
        sql = compact_sql(source(UP))
        for token in (
            "destination_e164",
            "consent_status",
            "consent_scope",
            "consent_source",
            "consent_recorded_at",
            "consent_revoked_at",
            "provenance_source",
            "allow_sms_fallback",
            "max_attempts",
            "lease_seconds",
            "retry_base_seconds",
            "retry_max_seconds",
            "monthly_cost_cap",
            "cost_warning_ratio",
            "primary_cost_class",
            "fallback_cost_class",
        ):
            with self.subTest(token=token):
                self.assertRegex(sql, rf"\b{token}\b")
        self.assertIn("alert_contact_consent_state_check", sql)
        self.assertIn("message_policy_retry_bounds_check", sql)
        self.assertIn("message_policy_cost_check", sql)

    def test_claim_is_atomic_skip_locked_and_bounded(self):
        sql = source(UP)
        claim = compact_sql(
            function_body(
                sql,
                "claim_msg_send",
                "claim_exact_one_shot_msg_send",
            )
        )
        self.assertTrue(claim, "claim_msg_send function is missing")
        self.assertIn("security definer", claim)
        self.assertIn("set search_path = ''", claim)
        self.assertIn("p_batch_size not between 1 and 50", claim)
        self.assertIn("p_lease_seconds not between 15 and 300", claim)
        self.assertIn("for update skip locked", claim)
        self.assertIn("limit p_batch_size", claim)
        self.assertIn("update public.msg_send as m", claim)
        self.assertIn("status = 'processing'", claim)
        self.assertIn("lock_owner = p_lock_owner", claim)
        self.assertIn("lease_token = pg_catalog.gen_random_uuid()", claim)
        self.assertIn(
            "lease_generation = m.lease_generation + 1",
            claim,
        )
        self.assertIn(
            "lease_expires_at = v_now + pg_catalog.make_interval("
            "secs => p_lease_seconds)",
            claim,
        )
        self.assertIn("attempt_count = m.attempt_count + 1", claim)
        self.assertIn("m.attempt_count < m.max_attempts", claim)
        self.assertIn("m.status in ('pending', 'retry_wait')", claim)
        self.assertNotIn("m.status = 'processing'", claim)
        self.assertLess(
            claim.index("for update skip locked"),
            claim.index("update public.msg_send as m"),
        )

    def test_exact_one_shot_claim_locks_validates_and_claims_only_target(
        self,
    ):
        exact_claim = compact_sql(
            function_body(
                source(UP),
                "claim_exact_one_shot_msg_send",
                "mark_msg_send_submission_started",
            )
        )
        self.assertTrue(
            exact_claim,
            "claim_exact_one_shot_msg_send function is missing",
        )
        self.assertIn("security definer", exact_claim)
        self.assertIn("set search_path = ''", exact_claim)
        self.assertIn(
            "lock table public.msg_send in share row exclusive mode",
            exact_claim,
        )
        self.assertLess(
            exact_claim.index(
                "lock table public.msg_send in share row exclusive mode"
            ),
            exact_claim.index("v_now := pg_catalog.clock_timestamp()"),
        )
        self.assertIn(
            "m.status in ('pending', 'retry_wait', 'processing', "
            "'accepted', 'waiting_result')",
            exact_claim,
        )
        self.assertIn("v_active_count <> 1", exact_claim)
        self.assertIn(
            "v_active_msg_send_id is distinct from p_expected_msg_send_id",
            exact_claim,
        )
        self.assertIn("m.status in ('pending', 'retry_wait')", exact_claim)
        self.assertIn("m.attempt_count = 0", exact_claim)
        self.assertIn("m.max_attempts = 1", exact_claim)
        self.assertIn("m.active_channel = 'alimtalk'", exact_claim)
        self.assertIn("p.is_active", exact_claim)
        self.assertIn("not p.allow_sms_fallback", exact_claim)
        self.assertIn("p.fallback_cost_class is null", exact_claim)
        self.assertIn("for update of m, p", exact_claim)
        self.assertIn("status = 'processing'", exact_claim)
        self.assertIn("attempt_count = m.attempt_count + 1", exact_claim)
        self.assertIn(
            "where m.msg_send_id = p_expected_msg_send_id",
            exact_claim,
        )

    def test_submission_start_is_atomic_channel_specific_and_fenced(self):
        start = compact_sql(
            function_body(
                source(UP),
                "mark_msg_send_submission_started",
                "mark_msg_send_submission_waiting_result",
            )
        )
        self.assertTrue(start)
        self.assertIn("security definer", start)
        self.assertIn("set search_path = ''", start)
        self.assertIn("from public.msg_send as m", start)
        self.assertIn("for update", start)
        self.assertIn(
            "v_message.status is distinct from 'processing'",
            start,
        )
        self.assertNotIn(
            "if v_message.status = 'accepted'",
            start,
        )
        self.assertIn(
            "v_message.lock_owner is distinct from p_lock_owner",
            start,
        )
        self.assertIn(
            "v_message.lease_token is distinct from p_lease_token",
            start,
        )
        self.assertIn(
            "v_message.lease_expires_at <= v_now",
            start,
        )
        self.assertIn(
            "v_message.active_channel is distinct from p_channel",
            start,
        )
        self.assertIn("status = 'accepted'", start)
        self.assertNotIn("p_started_at", start)
        self.assertIn(
            "provider_submission_started_at = v_now",
            start,
        )
        self.assertIn(
            "provider_submission_lease_token = p_lease_token",
            start,
        )
        self.assertIn(
            "fallback_submission_started_at = v_now",
            start,
        )
        self.assertIn(
            "fallback_submission_lease_token = p_lease_token",
            start,
        )
        submit_call = start.find("status = 'accepted'")
        self.assertGreater(submit_call, start.find("for update"))

    def test_submission_registration_is_atomic_idempotent_and_fenced(self):
        sql = source(UP)
        registration = compact_sql(
            function_body(
                sql,
                "mark_msg_send_submission_waiting_result",
                "complete_msg_send_claim",
            )
        )
        self.assertTrue(registration)
        self.assertIn("returns table (", registration)
        self.assertIn("applied boolean", registration)
        self.assertIn("duplicate boolean", registration)
        self.assertIn("message jsonb", registration)
        self.assertIn("from public.msg_send as m", registration)
        self.assertIn("for update", registration)
        self.assertIn(
            "v_message.lock_owner is distinct from p_lock_owner",
            registration,
        )
        self.assertIn(
            "v_message.lease_token is distinct from p_lease_token",
            registration,
        )
        self.assertNotIn("lease_expires_at <= v_now", registration)
        self.assertIn(
            "v_message.status is distinct from 'accepted'",
            registration,
        )
        self.assertIn(
            "v_message.active_channel is distinct from p_channel",
            registration,
        )
        self.assertIn(
            "v_message.provider_submission_started_at is null",
            registration,
        )
        self.assertIn(
            "v_message.fallback_submission_started_at is null",
            registration,
        )
        self.assertIn("status = 'waiting_result'", registration)
        self.assertIn(
            "provider_request_id = p_provider_request_id",
            registration,
        )
        self.assertIn(
            "provider_accepted_at = p_accepted_at",
            registration,
        )
        self.assertIn(
            "provider_submission_lease_token = p_lease_token",
            registration,
        )
        self.assertIn(
            "fallback_provider_request_id = p_provider_request_id",
            registration,
        )
        self.assertIn(
            "fallback_submission_lease_token = p_lease_token",
            registration,
        )
        self.assertIn("lock_owner = null", registration)
        self.assertIn("lease_token = null", registration)
        self.assertIn("lease_expires_at = null", registration)
        self.assertIn("'submission request mismatch'", registration)
        self.assertIn("'late submission registration'", registration)

    def test_submission_replay_survives_fast_callback_without_state_rewind(
        self,
    ):
        registration = compact_sql(
            function_body(
                source(UP),
                "mark_msg_send_submission_waiting_result",
                "complete_msg_send_claim",
            )
        )
        replay_guard = (
            "if v_message.status in ('waiting_result', 'sent', 'failed', "
            "'retry_wait', 'processing', 'accepted') and ("
        )
        self.assertIn(replay_guard, registration)
        self.assertIn(
            "p_channel = 'alimtalk' and "
            "v_message.provider_request_id is not distinct from "
            "p_provider_request_id and "
            "v_message.provider_submission_lease_token is not distinct "
            "from p_lease_token",
            registration,
        )
        self.assertIn(
            "p_channel = 'sms' and "
            "v_message.fallback_provider_request_id is not distinct from "
            "p_provider_request_id and "
            "v_message.fallback_submission_lease_token is not distinct "
            "from p_lease_token",
            registration,
        )
        replay_offset = registration.index(replay_guard)
        duplicate_offset = registration.index(
            "false, true, pg_catalog.to_jsonb(v_message)",
            replay_offset,
        )
        mismatch_offset = registration.index(
            "'submission request mismatch'",
            duplicate_offset,
        )
        late_offset = registration.index(
            "'late submission registration'",
            duplicate_offset,
        )
        stale_offset = registration.index(
            "'stale submission start fence'",
            duplicate_offset,
        )
        update_offset = registration.index("update public.msg_send as m")
        self.assertLess(duplicate_offset, mismatch_offset)
        self.assertLess(duplicate_offset, late_offset)
        self.assertLess(duplicate_offset, stale_offset)
        self.assertLess(duplicate_offset, update_offset)
        self.assertNotIn(
            "update public.msg_send as m",
            registration[replay_offset:duplicate_offset],
        )
        self.assertIn(
            "if v_message.status = 'waiting_result' then "
            "raise exception 'submission request mismatch'",
            registration,
        )

    def test_claim_completion_rejects_stale_or_expired_fence(self):
        sql = source(UP)
        completion = compact_sql(
            function_body(
                sql,
                "complete_msg_send_claim",
                "recover_msg_send_leases",
            )
        )
        self.assertTrue(completion)
        self.assertIn(
            "p_action not in ('retry_wait', 'failed', "
            "'suppressed', 'cancelled')",
            completion,
        )
        self.assertIn("from public.msg_send as m", completion)
        self.assertIn("for update", completion)
        self.assertIn(
            "v_message.lock_owner is distinct from p_lock_owner",
            completion,
        )
        self.assertIn(
            "v_message.lease_token is distinct from p_lease_token",
            completion,
        )
        self.assertIn("v_message.status = 'processing'", completion)
        self.assertIn("v_message.status = 'accepted'", completion)
        self.assertIn("v_message.lease_expires_at <= v_now", completion)
        self.assertIn(
            "v_message.provider_submission_lease_token is distinct from "
            "p_lease_token",
            completion,
        )
        self.assertIn(
            "v_message.fallback_submission_lease_token is distinct from "
            "p_lease_token",
            completion,
        )
        self.assertIn("lock_owner = null", completion)
        self.assertIn("lease_token = null", completion)
        self.assertIn("lease_expires_at = null", completion)

    def test_expired_lease_recovery_is_bounded_skip_locked_and_terminalizes(
        self,
    ):
        sql = source(UP)
        recovery = compact_sql(
            function_body(
                sql,
                "recover_msg_send_leases",
                "record_msg_send_push_result",
            )
        )
        self.assertTrue(recovery)
        self.assertIn("p_batch_size not between 1 and 1000", recovery)
        self.assertIn(
            "m.status = 'processing'",
            recovery,
        )
        self.assertIn(
            "m.status in ('pending', 'retry_wait') and "
            "m.expires_at <= v_now",
            recovery,
        )
        self.assertIn(
            "m.status = 'waiting_result'",
            recovery,
        )
        self.assertIn("m.status = 'accepted'", recovery)
        self.assertIn(
            "m.provider_submission_started_at <= "
            "v_now - pg_catalog.make_interval("
            "secs => p_waiting_result_timeout_seconds)",
            recovery,
        )
        self.assertIn(
            "m.fallback_submission_started_at <= "
            "v_now - pg_catalog.make_interval("
            "secs => p_waiting_result_timeout_seconds)",
            recovery,
        )
        self.assertIn(
            "p_waiting_result_timeout_seconds",
            recovery,
        )
        self.assertIn("m.lease_expires_at <= v_now", recovery)
        self.assertIn("m.expires_at <= v_now", recovery)
        self.assertIn("limit p_batch_size", recovery)
        self.assertIn("for update of m skip locked", recovery)
        self.assertIn("status = case", recovery)
        self.assertIn("then 'failed'", recovery)
        self.assertIn("else 'retry_wait'", recovery)
        self.assertIn("'lease_attempts_exhausted'", recovery)
        self.assertIn("'queue_expired'", recovery)
        self.assertIn("'provider_result_timeout'", recovery)
        self.assertIn(
            "'provider_submission_ambiguous_timeout'",
            recovery,
        )
        self.assertNotIn(
            "m.status in ('processing', 'accepted') and "
            "m.lease_expires_at <= v_now",
            recovery,
        )
        self.assertIn("lock_owner = null", recovery)
        self.assertIn("lease_token = null", recovery)
        self.assertIn("lease_expires_at = null", recovery)

    def test_push_result_correlates_durable_start_after_lease_expiry(self):
        result = compact_sql(
            function_body(source(UP), "record_msg_send_push_result")
        )
        durable_correlation = (
            "m.status = 'accepted' and "
            "m.active_channel = p_channel and "
            "((p_channel = 'alimtalk' and "
            "m.provider_submission_started_at is not null and "
            "m.provider_submission_lease_token = "
            "p_submission_lease_token) or "
            "(p_channel = 'sms' and "
            "m.fallback_submission_started_at is not null and "
            "m.fallback_submission_lease_token = "
            "p_submission_lease_token))"
        )
        self.assertIn(durable_correlation, result)
        self.assertNotIn(
            "m.status = 'processing' and "
            "m.active_channel = p_channel and "
            "m.lease_token = p_submission_lease_token",
            result,
        )
        self.assertNotIn(
            "m.lease_expires_at > v_now",
            result,
        )
        self.assertIn(
            "m.provider_submission_lease_token = "
            "p_submission_lease_token",
            result,
        )
        self.assertIn(
            "m.fallback_submission_lease_token = "
            "p_submission_lease_token",
            result,
        )
        self.assertIn(
            "provider_request_id = p_provider_request_id",
            result,
        )
        self.assertIn(
            "fallback_provider_request_id = p_provider_request_id",
            result,
        )
        correlation_offset = result.index(durable_correlation)
        registration_offset = result.index(
            "provider_request_id = p_provider_request_id",
            correlation_offset,
        )
        waiting_guard_offset = result.index(
            "if v_message.status is distinct from 'waiting_result'",
            registration_offset,
        )
        self.assertLess(correlation_offset, registration_offset)
        self.assertLess(registration_offset, waiting_guard_offset)
        self.assertIn(
            "select false, false, 'unknown', null::jsonb",
            result,
        )

    def test_push_result_rpc_returns_typed_worker_actions_and_fenced_fallback(
        self,
    ):
        sql = source(UP)
        result = compact_sql(
            function_body(sql, "record_msg_send_push_result")
        )
        self.assertTrue(
            result, "record_msg_send_push_result function is missing"
        )
        self.assertIn("security definer", result)
        self.assertIn("set search_path = ''", result)
        self.assertIn("returns table (", result)
        self.assertIn("applied boolean", result)
        self.assertIn("duplicate boolean", result)
        self.assertIn("action text", result)
        self.assertIn("message jsonb", result)
        self.assertIn("from public.msg_send as m", result)
        self.assertIn("for update", result)
        self.assertIn("p_channel not in ('alimtalk', 'sms')", result)
        self.assertIn("p_delivered is null", result)
        self.assertIn("p_retryable is null", result)
        self.assertIn("p_submission_lease_token", result)
        self.assertIn("provider_result_id is not null", result)
        self.assertIn("fallback_provider_result_id is not null", result)
        self.assertGreaterEqual(result.count("is distinct from"), 8)
        self.assertIn("duplicate provider push result mismatch", result)
        self.assertIn("false, true, 'duplicate'", result)
        self.assertIn("true, false, 'delivered'", result)
        self.assertIn("true, false, 'retry_scheduled'", result)
        self.assertIn("true, false, 'sms_fallback'", result)
        self.assertIn("true, false, 'failed'", result)
        duplicate_offset = result.index("false, true, 'duplicate'")
        retry_offset = result.index("true, false, 'retry_scheduled'")
        fallback_offset = result.index("true, false, 'sms_fallback'")
        self.assertLess(duplicate_offset, fallback_offset)
        self.assertLess(retry_offset, fallback_offset)
        self.assertIn("allow_sms_fallback", result)
        self.assertIn("status = 'retry_wait'", result)
        self.assertIn("status = 'processing'", result)
        self.assertIn("active_channel = 'sms'", result)
        self.assertIn("fallback_reason = v_result_code", result)
        self.assertIn("lock_owner = p_lock_owner", result)
        self.assertIn("lease_token = pg_catalog.gen_random_uuid()", result)
        self.assertIn(
            "lease_generation = m.lease_generation + 1",
            result,
        )
        self.assertIn(
            "lease_expires_at = v_now + pg_catalog.make_interval("
            "secs => p_lease_seconds)",
            result,
        )
        self.assertIn("attempt_count = m.attempt_count + 1", result)
        self.assertIn("status = 'sent'", result)
        self.assertIn("delivered_channel = p_channel", result)
        self.assertIn("status = 'failed'", result)

    def test_push_result_cost_is_validated_and_canonicalized_once(self):
        result = compact_sql(
            function_body(source(UP), "record_msg_send_push_result")
        )
        self.assertIn("v_cost numeric(14, 4)", result)
        self.assertIn("p_cost_amount > 9999999999.9999", result)
        self.assertIn(
            "p_cost_amount <> pg_catalog.round(p_cost_amount, 4)",
            result,
        )
        self.assertEqual(
            result.count(
                "v_cost := p_cost_amount::numeric(14, 4)"
            ),
            1,
        )
        self.assertNotIn(
            "provider_cost is distinct from p_cost_amount",
            result,
        )
        self.assertNotIn(
            "fallback_provider_cost is distinct from p_cost_amount",
            result,
        )
        self.assertIn(
            "provider_cost is distinct from v_cost",
            result,
        )
        self.assertIn(
            "fallback_provider_cost is distinct from v_cost",
            result,
        )
        self.assertIn(
            "provider_cost = case when p_channel = 'alimtalk' "
            "then v_cost",
            result,
        )
        self.assertIn(
            "fallback_provider_cost = case when p_channel = 'sms' "
            "then v_cost",
            result,
        )
        self.assertIn("provider_cost = v_cost", result)
        self.assertNotIn("provider_cost = p_cost_amount", result)
        self.assertNotIn("fallback_provider_cost = p_cost_amount", result)

    def test_link_token_is_hash_only_and_expiring(self):
        sql = compact_sql(source(UP))
        token_start = sql.find("create table public.message_link_token")
        self.assertGreaterEqual(token_start, 0)
        token_table = sql[token_start:]
        self.assertIn("token_hash bytea not null", token_table)
        self.assertIn("token_hash_algorithm text not null default 'sha256'", token_table)
        self.assertIn("pg_catalog.octet_length(token_hash) = 32", token_table)
        self.assertIn("expires_at timestamptz not null", token_table)
        self.assertIn("constraint message_link_token_token_hash_unique", token_table)
        self.assertNotRegex(
            token_table,
            r"\b(raw_token|plain_token|plaintext_token|token_plaintext|"
            r"token_value|opaque_token)\b",
        )

    def test_rls_and_privileges_are_fail_closed(self):
        sql = compact_sql(source(UP))
        table_grants = {
            "alert_contact": "select, insert, update",
            "message_policy": "select, insert, update",
            "msg_send": "select, insert",
            "message_link_token": "select, insert, update",
        }
        for table, privileges in table_grants.items():
            with self.subTest(table=table):
                self.assertIn(
                    f"alter table public.{table} enable row level security",
                    sql,
                )
                self.assertIn(
                    f"revoke all on table public.{table} "
                    "from public, anon, authenticated, service_role",
                    sql,
                )
                self.assertIn(
                    f"grant {privileges} on table public.{table} "
                    "to service_role",
                    sql,
                )
        self.assertNotIn(
            "grant select, insert, update on table public.msg_send",
            sql,
        )
        self.assertNotRegex(
            sql,
            r"grant\s+.+\s+on\s+(table|sequence|function)\s+.+\s+"
            r"to\s+(public|anon|authenticated)\b",
        )

        sequences = (
            "alert_contact_alert_contact_id_seq",
            "message_policy_message_policy_id_seq",
            "msg_send_msg_send_id_seq",
            "message_link_token_message_link_token_id_seq",
        )
        for sequence in sequences:
            with self.subTest(sequence=sequence):
                self.assertIn(
                    f"revoke all on sequence public.{sequence} "
                    "from public, anon, authenticated, service_role",
                    sql,
                )
                self.assertIn(
                    f"grant usage, select on sequence public.{sequence} "
                    "to service_role",
                    sql,
                )

        normalized = re.sub(r"\s*,\s*", ",", sql)
        signatures = (
            "public.claim_msg_send(text,integer,integer)",
            "public.claim_exact_one_shot_msg_send(bigint,text,integer)",
            "public.mark_msg_send_submission_started("
            "bigint,text,uuid,text)",
            "public.mark_msg_send_submission_waiting_result("
            "bigint,text,uuid,text,text,timestamptz)",
            "public.complete_msg_send_claim("
            "bigint,text,uuid,text,timestamptz,text)",
            "public.recover_msg_send_leases("
            "integer,integer,integer,integer,integer)",
            "public.record_msg_send_push_result("
            "text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,"
            "text,integer,integer,integer,integer)",
        )
        for signature in signatures:
            with self.subTest(signature=signature):
                self.assertIn(
                    f"revoke all on function {signature} "
                    "from public,anon,authenticated",
                    normalized,
                )
                self.assertIn(
                    f"grant execute on function {signature} to service_role",
                    normalized,
                )

        self.assertEqual(sql.count("security definer"), 7)
        self.assertEqual(sql.count("set search_path = ''"), 7)
        self.assertNotIn("security invoker", sql)

    def test_precheck_is_read_only_and_documents_existing_dependencies(self):
        sql = compact_sql(source(PRECHECK))
        self.assertTrue(sql.startswith("begin read only"))
        self.assertIn("current_setting('server_version_num')", sql)
        self.assertIn("to_regclass('public.device')", sql)
        self.assertIn("to_regclass('public.users')", sql)
        self.assertIn("column_name = 'deviceId'".lower(), sql)
        self.assertIn("column_name = 'userId'".lower(), sql)
        self.assertIn("rolname = 'service_role'", sql)
        for target in (
            "public.alert_contact",
            "public.message_policy",
            "public.msg_send",
            "public.message_link_token",
        ):
            with self.subTest(target=target):
                self.assertIn(f"to_regclass('{target}')", sql)
        self.assertIn(
            "to_regprocedure('public.claim_msg_send(text,integer,integer)')",
            sql,
        )
        for signature in (
            "public.claim_exact_one_shot_msg_send(bigint,text,integer)",
            "public.mark_msg_send_submission_started("
            "bigint,text,uuid,text)",
            "public.mark_msg_send_submission_waiting_result("
            "bigint,text,uuid,text,text,timestamptz)",
            "public.complete_msg_send_claim("
            "bigint,text,uuid,text,timestamptz,text)",
            "public.recover_msg_send_leases("
            "integer,integer,integer,integer,integer)",
            "public.record_msg_send_push_result("
            "text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,"
            "text,integer,integer,integer,integer)",
        ):
            with self.subTest(signature=signature):
                self.assertIn(
                    f"to_regprocedure('{signature}')",
                    re.sub(r"\s*,\s*", ",", sql),
                )
        for relation in (
            "public.alert_contact_alert_contact_id_seq",
            "public.message_policy_message_policy_id_seq",
            "public.msg_send_msg_send_id_seq",
            "public.message_link_token_message_link_token_id_seq",
            "public.idx_msg_send_claim",
            "public.idx_msg_send_lease",
            "public.uq_msg_send_provider_request",
            "public.uq_msg_send_fallback_provider_request",
            "public.uq_msg_send_provider_result",
            "public.uq_msg_send_fallback_provider_result",
            "public.idx_message_link_token_expiry",
        ):
            with self.subTest(relation=relation):
                self.assertIn(f"to_regclass('{relation}')", sql)
        self.assertIn("raise exception", sql)
        self.assertTrue(sql.endswith("rollback;"))
        self.assertNotRegex(
            sql,
            r"\b(create|alter|drop|truncate|insert|update|delete)\s+"
            r"(table|function|into|public\.)",
        )

    def test_verification_checks_schema_security_and_hash_only_contract(self):
        sql = compact_sql(source(VERIFY))
        for token in (
            "relrowsecurity",
            "pg_catalog.pg_policy",
            "information_schema.columns",
            "pg_catalog.pg_constraint",
            "pg_get_constraintdef",
            "has_table_privilege",
            "has_sequence_privilege",
            "has_function_privilege",
            "prosecdef",
            "proconfig",
            "idx_msg_send_claim",
            "msg_send_state_check",
            "message_link_token_hash_length_check",
            "unexpected plaintext token column",
        ):
            with self.subTest(token=token):
                self.assertIn(token, sql)
        self.assertTrue(sql.startswith("begin read only"))
        self.assertTrue(sql.endswith("rollback;"))
        self.assertIn("raise exception", sql)
        for privilege in ("select", "insert"):
            with self.subTest(privilege=privilege):
                self.assertIn(
                    "not has_table_privilege("
                    "'service_role', expected.relation_name, "
                    f"'{privilege}')",
                    sql,
                )
        service_error = (
            "raise exception 'service_role message table privilege mismatch'"
        )
        service_end = sql.index(service_error)
        service_start = sql.rfind("if exists (", 0, service_end)
        service_check = sql[service_start:service_end]
        self.assertIn(
            "values ('public.alert_contact', true), "
            "('public.message_policy', true), "
            "('public.msg_send', false), "
            "('public.message_link_token', true)) "
            "as expected(relation_name, require_update)",
            service_check,
        )
        self.assertNotIn(
            "as expected(relation_name) where",
            service_check,
        )
        self.assertEqual(service_check.count("expected.require_update"), 2)
        self.assertIn(
            "expected.require_update and not has_table_privilege("
            "'service_role', expected.relation_name, 'update')",
            service_check,
        )
        self.assertIn(
            "not expected.require_update and has_table_privilege("
            "'service_role', expected.relation_name, 'update')",
            service_check,
        )
        for privilege in ("delete", "truncate", "references", "trigger"):
            with self.subTest(privilege=privilege):
                self.assertIn(
                    "has_table_privilege("
                    "'service_role', expected.relation_name, "
                    f"'{privilege}')",
                    sql,
                )
        for privilege in ("usage", "select"):
            with self.subTest(privilege=privilege):
                self.assertIn(
                    "not has_sequence_privilege("
                    "'service_role', expected.relation_name, "
                    f"'{privilege}')",
                    sql,
                )
        self.assertIn(
            "has_sequence_privilege("
            "'service_role', expected.relation_name, 'update')",
            sql,
        )
        normalized = re.sub(r"\s*,\s*", ",", sql)
        for signature in (
            "public.claim_msg_send(text,integer,integer)",
            "public.claim_exact_one_shot_msg_send(bigint,text,integer)",
            "public.mark_msg_send_submission_started("
            "bigint,text,uuid,text)",
            "public.mark_msg_send_submission_waiting_result("
            "bigint,text,uuid,text,text,timestamptz)",
            "public.complete_msg_send_claim("
            "bigint,text,uuid,text,timestamptz,text)",
            "public.recover_msg_send_leases("
            "integer,integer,integer,integer,integer)",
            "public.record_msg_send_push_result("
            "text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,"
            "text,integer,integer,integer,integer)",
        ):
            with self.subTest(signature=signature):
                self.assertIn(f"'{signature}'::regprocedure", normalized)
        for constraint in (
            "msg_send_submission_state_check",
            "msg_send_channel_state_check",
            "msg_send_delivery_state_check",
        ):
            with self.subTest(constraint=constraint):
                self.assertIn(constraint, sql)
        self.assertIn(
            "('msg_send_channel_state_check', 'is distinct from')",
            sql,
        )
        self.assertNotIn(
            "('msg_send_channel_state_check', 'is not distinct from')",
            sql,
        )
        for index in (
            "uq_msg_send_provider_request",
            "uq_msg_send_fallback_provider_request",
        ):
            with self.subTest(index=index):
                self.assertIn(index, sql)

    def test_rollback_drops_only_new_objects_in_dependency_order(self):
        sql = compact_sql(source(DOWN))
        normalized = re.sub(r"\s*,\s*", ",", sql)
        expected_order = (
            "drop function public.record_msg_send_push_result"
            "(text,uuid,text,text,boolean,boolean,timestamptz,text,numeric,"
            "text,integer,integer,integer,integer)",
            "drop function public.recover_msg_send_leases"
            "(integer,integer,integer,integer,integer)",
            "drop function public.complete_msg_send_claim"
            "(bigint,text,uuid,text,timestamptz,text)",
            "drop function public.mark_msg_send_submission_waiting_result"
            "(bigint,text,uuid,text,text,timestamptz)",
            "drop function public.mark_msg_send_submission_started"
            "(bigint,text,uuid,text)",
            "drop function public.claim_exact_one_shot_msg_send"
            "(bigint,text,integer)",
            "drop function public.claim_msg_send(text,integer,integer)",
            "drop table public.message_link_token",
            "drop table public.msg_send",
            "drop table public.message_policy",
            "drop table public.alert_contact",
        )
        offsets = []
        for token in expected_order:
            with self.subTest(token=token):
                self.assertIn(token, normalized)
            offsets.append(normalized.find(token))
        self.assertEqual(offsets, sorted(offsets))
        self.assertNotIn("cascade", sql)
        self.assertNotRegex(
            sql,
            r"drop\s+(table|function)\s+public\."
            r"(device|users|alertsend|devicecmds|assign_device_command)\b",
        )

    def test_expected_result_states_live_and_rollback_boundaries(self):
        text = source(EXPECTED)
        self.assertIn("live apply: 0", text)
        self.assertIn("기존 object 변경: 0", text)
        self.assertIn("FOR UPDATE SKIP LOCKED", text)
        self.assertIn("atomic exact one-shot claim", text)
        self.assertIn("hash만 저장", text)
        self.assertIn("service_role", text)
        self.assertIn("PUSH result", text)
        self.assertIn("provider_submission_ambiguous_timeout", text)
        self.assertIn("submission start", text)
        self.assertIn("신규 message queue 데이터 삭제", text)
        self.assertIn("별도 승인", text)

    def test_sql_artifacts_have_balanced_transaction_and_dollar_quotes(self):
        expected_boundaries = {
            UP: ("begin;", "commit;"),
            DOWN: ("begin;", "commit;"),
            PRECHECK: ("begin read only;", "rollback;"),
            VERIFY: ("begin read only;", "rollback;"),
        }
        for path, (begin, end) in expected_boundaries.items():
            text = source(path)
            compact = compact_sql(text)
            with self.subTest(path=path):
                self.assertTrue(compact.startswith(begin))
                self.assertTrue(compact.endswith(end))
                self.assertFalse(text.lstrip().startswith("\\"))
                tags = set(re.findall(r"\$[a-z_][a-z0-9_]*\$", text.lower()))
                for tag in tags:
                    self.assertGreaterEqual(
                        text.lower().count(tag),
                        2,
                        f"missing dollar quote pair {tag} in {path}",
                    )
                    self.assertEqual(
                        text.lower().count(tag) % 2,
                        0,
                        f"unbalanced dollar quote {tag} in {path}",
                    )


if __name__ == "__main__":
    unittest.main()
