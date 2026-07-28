from __future__ import annotations

from dataclasses import replace
from datetime import datetime, timezone
from decimal import Decimal
import unittest

from msg_send.callback import parse_push_result
from msg_send.config import load_runtime_config
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
    MessageStatus,
    ProviderPushResult,
    ProviderResultMode,
    ProviderSubmission,
    ProviderSubmissionOutcomeUnknown,
    PushAction,
    PushApplication,
    SubmissionRegistration,
)
from msg_send.runtime import (
    ProductionRuntime,
    RuntimeSafetyError,
)


NOW = datetime(2026, 7, 26, 12, 30, tzinfo=timezone.utc)
LEASE_UUID = "12345678-1234-4abc-8def-1234567890ab"
REFKEY = "1234567812344abc8def1234567890ab"


def config_env() -> dict[str, str]:
    return {
        "SUPABASE_URL": "https://project-ref.supabase.co",
        "SUPABASE_SECRET_KEY": "sb_secret_test_only_not_a_real_key",
        "BIZPPURIO_ACCOUNT": "provider-account",
        "BIZPPURIO_PASSWORD": "provider-password",
        "BIZPPURIO_FROM": "0212345678",
        "BIZPPURIO_SENDERKEY": "sender-profile-key",
        "BIZPPURIO_AT_COST_KRW": "8.5000",
        "BIZPPURIO_SMS_COST_KRW": "12.0000",
    }


def one_shot_job(
    *,
    message_id: str = "42",
    max_attempts: int = 1,
    fallback: bool = False,
) -> MessageJob:
    return MessageJob(
        message_id=message_id,
        lease_token=LEASE_UUID,
        dedupe_key=f"dedupe-{message_id}",
        recipient="01012345678",
        template_code="bizp_2026071315003676625784727",
        template_body="source body",
        history_url="https://zxcx.io/s/history_token_123456",
        settings_url="https://zxcx.io/s/settings_token_123456",
        sms_body="fallback disabled",
        attempt_count=1,
        policy=MessagePolicy(
            consent_granted=True,
            sms_fallback_approved=fallback,
            sms_cost_class="sms" if fallback else None,
        ),
        delivery_channel=DeliveryChannel.ALIMTALK,
        fallback_reason=None,
        template_params={
            "workplaceName": "서울 1호점",
            "userMachineName": "냉동고 A",
            "eventTime": "2026-07-26 12:30",
        },
        max_attempts=max_attempts,
        lock_owner="spaceship-msg-send-1",
    )


class FakeDatabase:
    def __init__(
        self,
        claims: tuple[MessageJob, ...] = (),
    ) -> None:
        self.claims = claims
        self.exact_claim_calls = 0
        self.recoveries = 0
        self.claim_calls = 0
        self.started = 0
        self.waiting = 0
        self.acceptance_successes: list[Decimal] = []
        self.pushes = 0
        self.push_error = False
        self.push_application = PushApplication(PushAction.DELIVERED)
        self.expect_fallback_rejection = False
        self.fallback_reject_error = False
        self.fallback_rejections = 0
        self.callback_missing = False
        self.callback_missing_queries: list[str] = []
        self.drain_calls = 0
        self.drain_error = False
        self.events: list[str] = []

    def drain_temperature_alert_outbox(
        self,
        limit: int = 16,
    ) -> tuple[str, ...]:
        self.drain_calls += 1
        self.events.append(f"drain:{limit}")
        if self.drain_error:
            raise RuntimeError("sanitized drain failure")
        return ()

    def recover_expired_leases(self, **kwargs: object) -> int:
        del kwargs
        self.recoveries += 1
        self.events.append("recover")
        return 0

    def claim_messages(self, **kwargs: object) -> tuple[MessageJob, ...]:
        limit = int(kwargs.get("limit", len(self.claims)))
        self.claim_calls += 1
        self.events.append("claim")
        claimed = self.claims[:limit]
        self.claims = self.claims[limit:]
        return claimed

    def claim_exact_one_shot(
        self,
        **kwargs: object,
    ) -> tuple[MessageJob, ...]:
        del kwargs
        self.exact_claim_calls += 1
        return self.claims

    def mark_submission_started(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> None:
        del job, channel
        self.started += 1

    def mark_submission_waiting_result(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
    ) -> SubmissionRegistration:
        del job, provider_request_id, channel, now
        self.waiting += 1
        return SubmissionRegistration(
            applied=True,
            duplicate=False,
            current_status=MessageStatus.WAITING_RESULT,
        )

    def mark_submission_accepted_success(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
        cost_amount: Decimal,
    ) -> SubmissionRegistration:
        del job, provider_request_id, channel, now
        self.acceptance_successes.append(cost_amount)
        return SubmissionRegistration(
            applied=True,
            duplicate=False,
            current_status=MessageStatus.SENT,
        )

    def oldest_waiting_result_at(self) -> None:
        return None

    def is_callback_missing(self, provider_request_id: str) -> bool:
        self.callback_missing_queries.append(provider_request_id)
        return self.callback_missing

    def apply_push_result(
        self,
        result: ProviderPushResult,
        **kwargs: object,
    ) -> PushApplication:
        del result, kwargs
        self.pushes += 1
        if self.push_error:
            raise RuntimeError("sanitized push apply failure")
        return self.push_application

    def suppress_message(self, *args: object, **kwargs: object) -> None:
        raise AssertionError("unexpected suppression")

    def suppress_duplicate(self, *args: object, **kwargs: object) -> None:
        raise AssertionError("unexpected duplicate")

    def schedule_retry(self, *args: object, **kwargs: object) -> None:
        raise AssertionError("unexpected retry")

    def mark_failed(self, *args: object, **kwargs: object) -> None:
        raise AssertionError("unexpected failure")

    def reject_sms_fallback(
        self,
        *args: object,
        **kwargs: object,
    ) -> None:
        del args, kwargs
        if not self.expect_fallback_rejection:
            raise AssertionError("unexpected fallback")
        if self.fallback_reject_error:
            raise RuntimeError("sanitized fallback reject failure")
        self.fallback_rejections += 1


class FakeProvider:
    def __init__(
        self,
        outcome: ProviderSubmission | Exception | None = None,
    ) -> None:
        self.outcome = outcome or ProviderSubmission(
            accepted=True,
            request_id="provider-request-1",
        )
        self.calls = 0
        self.report_requests: list[str] = []
        self.report_error: Exception | None = None

    def submit(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> ProviderSubmission:
        del job, channel
        self.calls += 1
        if isinstance(self.outcome, Exception):
            raise self.outcome
        return self.outcome

    def request_report(self, provider_request_id: str) -> None:
        self.report_requests.append(provider_request_id)
        if self.report_error is not None:
            raise self.report_error


def runtime_with(
    database: FakeDatabase,
    provider: FakeProvider,
    environment: dict[str, str] | None = None,
) -> ProductionRuntime:
    return ProductionRuntime(
        config=load_runtime_config(environment or config_env()),
        database=database,
        provider=provider,
        clock=lambda: NOW,
    )


class ProductionRuntimeSafetyTests(unittest.TestCase):
    def test_run_once_is_locked_by_default_without_recovery_or_claim(self) -> None:
        database = FakeDatabase()
        provider = FakeProvider()
        runtime = runtime_with(database, provider)

        result = runtime.run_once()

        self.assertFalse(result.executed)
        self.assertEqual(result.reason, "claim_disabled")
        self.assertEqual(database.recoveries, 0)
        self.assertEqual(database.drain_calls, 0)
        self.assertEqual(database.claim_calls, 0)
        self.assertEqual(provider.calls, 0)
        self.assertFalse(runtime.gates.claim_enabled)
        self.assertFalse(runtime.gates.send_enabled)

    def test_enabled_run_once_recovers_then_drains_then_claims(self) -> None:
        environment = config_env()
        environment["MSG_SEND_CLAIM_ENABLED"] = "true"
        environment["MSG_SEND_SEND_ENABLED"] = "true"
        database = FakeDatabase()
        runtime = runtime_with(database, FakeProvider(), environment)

        result = runtime.run_once()

        self.assertTrue(result.success)
        self.assertEqual(database.events, ["recover", "drain:16", "claim"])

    def test_drain_failure_stops_before_claim_and_provider(self) -> None:
        environment = config_env()
        environment["MSG_SEND_CLAIM_ENABLED"] = "true"
        environment["MSG_SEND_SEND_ENABLED"] = "true"
        database = FakeDatabase((one_shot_job(),))
        database.drain_error = True
        provider = FakeProvider()
        runtime = runtime_with(database, provider, environment)

        result = runtime.run_once()

        self.assertTrue(result.executed)
        self.assertFalse(result.success)
        self.assertEqual(database.events, ["recover", "drain:16"])
        self.assertEqual(database.claim_calls, 0)
        self.assertEqual(provider.calls, 0)

    def test_one_shot_accepts_only_one_expected_attempt_one_no_fallback_row(
        self,
    ) -> None:
        job = one_shot_job()
        database = FakeDatabase((job,))
        provider = FakeProvider()
        runtime = runtime_with(database, provider)

        result = runtime.one_shot("42")

        self.assertTrue(result.executed)
        self.assertTrue(result.success)
        self.assertEqual(database.exact_claim_calls, 1)
        self.assertEqual(database.recoveries, 0)
        self.assertEqual(database.claim_calls, 0)
        self.assertEqual(database.started, 1)
        self.assertEqual(database.waiting, 1)
        self.assertEqual(provider.calls, 1)
        self.assertFalse(runtime.gates.claim_enabled)
        self.assertFalse(runtime.gates.send_enabled)

    def test_one_shot_honors_provider_acceptance_result_mode(self) -> None:
        environment = config_env()
        environment["MSG_SEND_RESULT_MODE"] = "provider_acceptance"
        database = FakeDatabase((one_shot_job(),))
        runtime = runtime_with(
            database,
            FakeProvider(),
            environment,
        )

        result = runtime.one_shot("42")

        self.assertTrue(result.success)
        self.assertEqual(database.waiting, 0)
        self.assertEqual(
            database.acceptance_successes,
            [Decimal("8.5000")],
        )

    def test_one_shot_uses_only_atomic_exact_claim_rpc(self) -> None:
        database = FakeDatabase(claims=(one_shot_job(),))
        provider = FakeProvider()
        runtime = runtime_with(database, provider)

        result = runtime.one_shot("42")

        self.assertTrue(result.success)
        self.assertEqual(database.exact_claim_calls, 1)
        self.assertEqual(database.claim_calls, 0)
        self.assertEqual(provider.calls, 1)

    def test_one_shot_rejects_ambiguous_queue_policy_and_wrong_claim(
        self,
    ) -> None:
        cases = (
            (one_shot_job(), one_shot_job()),
            (one_shot_job(max_attempts=2),),
            (one_shot_job(fallback=True),),
            (one_shot_job(message_id="99"),),
            (replace(one_shot_job(), attempt_count=0),),
        )
        for claims in cases:
            with self.subTest(claims=claims):
                database = FakeDatabase(claims)
                provider = FakeProvider()
                runtime = runtime_with(database, provider)
                with self.assertRaises(RuntimeSafetyError):
                    runtime.one_shot("42")
                self.assertEqual(provider.calls, 0)
                self.assertFalse(runtime.gates.claim_enabled)
                self.assertFalse(runtime.gates.send_enabled)

    def test_one_shot_empty_queue_is_safe_noop(self) -> None:
        database = FakeDatabase()
        provider = FakeProvider()
        runtime = runtime_with(database, provider)
        result = runtime.one_shot("42")
        self.assertFalse(result.executed)
        self.assertEqual(result.reason, "queue_empty")
        self.assertEqual(database.exact_claim_calls, 1)
        self.assertEqual(database.claim_calls, 0)
        self.assertEqual(provider.calls, 0)

    def test_provider_ambiguity_still_relocks_one_shot_gates(self) -> None:
        database = FakeDatabase((one_shot_job(),))
        provider = FakeProvider(ProviderSubmissionOutcomeUnknown())
        runtime = runtime_with(database, provider)

        with self.assertLogs("msg_send.worker", level="WARNING"):
            result = runtime.one_shot("42")

        self.assertTrue(result.executed)
        self.assertTrue(result.success)
        self.assertEqual(provider.calls, 1)
        self.assertFalse(runtime.gates.claim_enabled)
        self.assertFalse(runtime.gates.send_enabled)

    def test_synthetic_callback_is_not_blocked_by_claim_send_gates(self) -> None:
        database = FakeDatabase()
        runtime = runtime_with(database, FakeProvider())
        action = runtime.handle_callback_payload(
            {
                "CMSGID": "provider-request-1",
                "REFKEY": REFKEY,
                "MSGID": "provider-result-1",
                "UNIXTIME": "1785038400",
                "RESULT": "7000",
                "MEDIA": "AT",
            }
        )
        self.assertIs(action, PushAction.DELIVERED)
        self.assertEqual(database.pushes, 1)
        self.assertFalse(runtime.gates.claim_enabled)
        self.assertFalse(runtime.gates.send_enabled)

    def test_provider_acceptance_mode_ignores_late_callback(self) -> None:
        environment = config_env()
        environment["MSG_SEND_RESULT_MODE"] = "provider_acceptance"
        database = FakeDatabase()
        runtime = runtime_with(
            database,
            FakeProvider(),
            environment,
        )

        action = runtime.handle_callback_payload_non_sending(
            {
                "CMSGID": "provider-request-1",
                "REFKEY": REFKEY,
                "MSGID": "late-provider-result-1",
                "UNIXTIME": "1785038400",
                "RESULT": "7000",
                "MEDIA": "AT",
            }
        )

        self.assertIs(action, PushAction.DUPLICATE)
        self.assertEqual(database.pushes, 0)
        self.assertIs(
            runtime.config.provider_result_mode,
            ProviderResultMode.PROVIDER_ACCEPTANCE,
        )

    def test_non_sending_callback_propagates_database_failure(
        self,
    ) -> None:
        database = FakeDatabase()
        database.push_error = True
        runtime = runtime_with(database, FakeProvider())

        with self.assertRaisesRegex(
            RuntimeError,
            "sanitized push apply failure",
        ):
            runtime.handle_callback_payload_non_sending(
                {
                    "CMSGID": "provider-request-1",
                    "REFKEY": REFKEY,
                    "MSGID": "provider-result-1",
                    "UNIXTIME": "1785038400",
                    "RESULT": "7000",
                    "MEDIA": "AT",
                }
            )

    def test_non_sending_callback_propagates_fallback_reject_failure(
        self,
    ) -> None:
        database = FakeDatabase()
        database.push_application = PushApplication(
            PushAction.SMS_FALLBACK,
            replace(
                one_shot_job(fallback=True),
                delivery_channel=DeliveryChannel.SMS,
                fallback_reason="7204",
            ),
        )
        database.expect_fallback_rejection = True
        database.fallback_reject_error = True
        runtime = runtime_with(database, FakeProvider())

        with self.assertRaisesRegex(
            RuntimeError,
            "sanitized fallback reject failure",
        ):
            runtime.handle_callback_payload_non_sending(
                {
                    "CMSGID": "provider-request-1",
                    "REFKEY": REFKEY,
                    "MSGID": "provider-result-1",
                    "UNIXTIME": "1785038400",
                    "RESULT": "7204",
                    "MEDIA": "AT",
                }
            )

    def test_provider_acceptance_mode_does_not_ignore_sms_result(self) -> None:
        environment = config_env()
        environment["MSG_SEND_RESULT_MODE"] = "provider_acceptance"
        database = FakeDatabase()
        runtime = runtime_with(
            database,
            FakeProvider(),
            environment,
        )
        payload = {
            "CMSGID": "provider-request-1",
            "REFKEY": REFKEY,
            "MSGID": "legacy-sms-result-1",
            "UNIXTIME": "1785038400",
            "RESULT": "7000",
            "MEDIA": "SMS",
        }

        action = runtime.handle_callback_payload_non_sending(payload)

        self.assertIs(action, PushAction.DELIVERED)
        self.assertEqual(database.pushes, 1)

    def test_disabled_global_fallback_terminalizes_callback_without_send(
        self,
    ) -> None:
        environment = config_env()
        environment["MSG_SEND_CLAIM_ENABLED"] = "true"
        environment["MSG_SEND_SEND_ENABLED"] = "true"
        database = FakeDatabase()
        database.push_application = PushApplication(
            PushAction.SMS_FALLBACK,
            replace(
                one_shot_job(fallback=True),
                delivery_channel=DeliveryChannel.SMS,
                fallback_reason="7001",
            ),
        )
        database.expect_fallback_rejection = True
        provider = FakeProvider()
        runtime = ProductionRuntime(
            config=load_runtime_config(environment),
            database=database,
            provider=provider,
            clock=lambda: NOW,
        )

        action = runtime.handle_callback_payload(
            {
                "CMSGID": "provider-request-1",
                "REFKEY": REFKEY,
                "MSGID": "provider-result-1",
                "UNIXTIME": "1785038400",
                "RESULT": "7001",
                "MEDIA": "AT",
            }
        )

        self.assertIs(action, PushAction.FAILED)
        self.assertEqual(database.fallback_rejections, 1)
        self.assertEqual(database.started, 0)
        self.assertEqual(provider.calls, 0)

    def test_disabled_global_fallback_rejects_claimed_sms_without_send(
        self,
    ) -> None:
        environment = config_env()
        environment["MSG_SEND_CLAIM_ENABLED"] = "true"
        environment["MSG_SEND_SEND_ENABLED"] = "true"
        fallback_job = replace(
            one_shot_job(fallback=True),
            delivery_channel=DeliveryChannel.SMS,
            fallback_reason="7001",
        )
        database = FakeDatabase(claims=(fallback_job,))
        database.expect_fallback_rejection = True
        provider = FakeProvider()
        runtime = ProductionRuntime(
            config=load_runtime_config(environment),
            database=database,
            provider=provider,
            clock=lambda: NOW,
        )

        result = runtime.run_once()

        self.assertTrue(result.executed)
        self.assertTrue(result.success)
        self.assertEqual(database.fallback_rejections, 1)
        self.assertEqual(database.started, 0)
        self.assertEqual(provider.calls, 0)

    def test_report_reconciliation_only_queries_callback_missing_request(
        self,
    ) -> None:
        database = FakeDatabase()
        database.callback_missing = True
        provider = FakeProvider()
        runtime = runtime_with(database, provider)

        result = runtime.reconcile_callback("provider-request-1")

        self.assertTrue(result.executed)
        self.assertTrue(result.success)
        self.assertEqual(result.reason, "report_requested")
        self.assertEqual(
            database.callback_missing_queries,
            ["provider-request-1"],
        )
        self.assertEqual(provider.report_requests, ["provider-request-1"])
        self.assertEqual(provider.calls, 0)

        database.callback_missing = False
        skipped = runtime.reconcile_callback("provider-request-2")
        self.assertFalse(skipped.executed)
        self.assertTrue(skipped.success)
        self.assertEqual(skipped.reason, "callback_not_missing")
        self.assertEqual(provider.report_requests, ["provider-request-1"])
        self.assertEqual(provider.calls, 0)

    def test_ambiguous_report_transport_never_resends_message(self) -> None:
        database = FakeDatabase()
        database.callback_missing = True
        provider = FakeProvider()
        provider.report_error = OSError("PRIVATE_PROVIDER_BODY_SENTINEL")
        runtime = runtime_with(database, provider)

        result = runtime.reconcile_callback("provider-request-1")

        self.assertTrue(result.executed)
        self.assertFalse(result.success)
        self.assertEqual(result.reason, "report_request_failed")
        self.assertEqual(provider.report_requests, ["provider-request-1"])
        self.assertEqual(provider.calls, 0)


if __name__ == "__main__":
    unittest.main()
