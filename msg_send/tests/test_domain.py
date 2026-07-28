from __future__ import annotations

import unittest
from datetime import datetime, timezone
from decimal import Decimal

import msg_send.domain as domain_module
from msg_send.domain import (
    DeliveryChannel,
    InvalidStatusTransition,
    MessageJob,
    MessagePolicy,
    MessageStatus,
    ProviderPushResult,
    ProviderSubmission,
    RetryPolicy,
    WorkerConfig,
    render_history_links,
    transition_status,
)


class StatusTransitionTests(unittest.TestCase):
    def test_canonical_delivery_path_is_allowed(self) -> None:
        path = (
            MessageStatus.PENDING,
            MessageStatus.PROCESSING,
            MessageStatus.ACCEPTED,
            MessageStatus.WAITING_RESULT,
            MessageStatus.SENT,
        )

        for current, target in zip(path, path[1:]):
            with self.subTest(current=current, target=target):
                self.assertEqual(transition_status(current, target), target)

    def test_retry_path_is_allowed_from_each_provider_stage(self) -> None:
        for current in (
            MessageStatus.PROCESSING,
            MessageStatus.ACCEPTED,
            MessageStatus.WAITING_RESULT,
        ):
            with self.subTest(current=current):
                self.assertEqual(
                    transition_status(current, MessageStatus.RETRY_WAIT),
                    MessageStatus.RETRY_WAIT,
                )
        self.assertEqual(
            transition_status(
                MessageStatus.RETRY_WAIT,
                MessageStatus.PROCESSING,
            ),
            MessageStatus.PROCESSING,
        )

    def test_expired_unclaimed_message_can_terminalize(self) -> None:
        try:
            result = transition_status(
                MessageStatus.PENDING,
                MessageStatus.FAILED,
            )
        except InvalidStatusTransition:
            self.fail(
                "expired pending message must be allowed to terminalize"
            )
            return
        self.assertEqual(result, MessageStatus.FAILED)

    def test_api_acceptance_cannot_skip_push_result(self) -> None:
        with self.assertRaises(InvalidStatusTransition):
            transition_status(
                MessageStatus.ACCEPTED,
                MessageStatus.SENT,
            )

    def test_illegal_and_terminal_transitions_are_rejected(self) -> None:
        pairs = (
            (MessageStatus.PENDING, MessageStatus.SENT),
            (MessageStatus.WAITING_RESULT, MessageStatus.ACCEPTED),
            (MessageStatus.SENT, MessageStatus.PROCESSING),
            (MessageStatus.FAILED, MessageStatus.RETRY_WAIT),
            (MessageStatus.SUPPRESSED, MessageStatus.PROCESSING),
            (MessageStatus.CANCELLED, MessageStatus.PROCESSING),
        )

        for current, target in pairs:
            with self.subTest(current=current, target=target):
                with self.assertRaises(InvalidStatusTransition):
                    transition_status(current, target)


class TemplateCompatibilityTests(unittest.TestCase):
    def test_normal_and_malformed_history_markers_render_without_parenthesis(
        self,
    ) -> None:
        final_url = "https://service.invalid/s/opaque-reference"
        template = (
            "normal=#{tempHistoryToken} "
            "legacy=#{tempHistoryToken)}"
        )

        rendered = render_history_links(template, final_url)

        self.assertEqual(
            rendered,
            (
                "normal=https://service.invalid/s/opaque-reference "
                "legacy=https://service.invalid/s/opaque-reference"
            ),
        )
        self.assertNotIn("opaque-reference)", rendered)


class RetryAndConfigurationTests(unittest.TestCase):
    def test_retry_backoff_is_exponential_capped_and_bounded(self) -> None:
        policy = RetryPolicy(
            max_attempts=4,
            base_delay_seconds=5,
            max_delay_seconds=12,
        )

        self.assertEqual(policy.next_delay_seconds(1), 5)
        self.assertEqual(policy.next_delay_seconds(2), 10)
        self.assertEqual(policy.next_delay_seconds(3), 12)
        self.assertIsNone(policy.next_delay_seconds(4))

    def test_worker_configuration_enforces_poll_and_concurrency_bounds(
        self,
    ) -> None:
        self.assertEqual(
            WorkerConfig(
                poll_interval_seconds=30,
                max_concurrency=1,
            ).poll_interval_seconds,
            30,
        )
        self.assertEqual(
            WorkerConfig(
                poll_interval_seconds=60,
                max_concurrency=16,
            ).max_concurrency,
            16,
        )

        invalid_values = (
            {"poll_interval_seconds": 29, "max_concurrency": 1},
            {"poll_interval_seconds": 61, "max_concurrency": 1},
            {"poll_interval_seconds": 45, "max_concurrency": 0},
            {"poll_interval_seconds": 45, "max_concurrency": 17},
        )
        for values in invalid_values:
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    WorkerConfig(**values)

    def test_worker_lease_matches_database_maximum(self) -> None:
        self.assertEqual(
            WorkerConfig(lease_seconds=300).lease_seconds,
            300,
        )
        with self.assertRaises(ValueError):
            WorkerConfig(lease_seconds=301)

    def test_worker_drain_limits_are_bounded(self) -> None:
        config = WorkerConfig(
            max_messages_per_run=20,
            max_cycle_seconds=50,
        )

        self.assertEqual(config.max_messages_per_run, 20)
        self.assertEqual(config.max_cycle_seconds, 50)

        invalid_values = (
            {"max_messages_per_run": 0},
            {"max_messages_per_run": 21},
            {"max_cycle_seconds": 4},
            {"max_cycle_seconds": 56},
        )
        for values in invalid_values:
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    WorkerConfig(**values)

    def test_waiting_result_timeout_is_bounded(self) -> None:
        try:
            minimum = WorkerConfig(result_timeout_seconds=60)
            maximum = WorkerConfig(result_timeout_seconds=86400)
        except TypeError:
            self.fail(
                "WorkerConfig must expose bounded result_timeout_seconds"
            )
            return

        self.assertEqual(minimum.result_timeout_seconds, 60)
        self.assertEqual(maximum.result_timeout_seconds, 86400)
        with self.assertRaises(ValueError):
            WorkerConfig(result_timeout_seconds=59)
        with self.assertRaises(ValueError):
            WorkerConfig(result_timeout_seconds=86401)


class PolicyAndProviderStateTests(unittest.TestCase):
    def test_sms_fallback_requires_consent_approval_and_cost_class(
        self,
    ) -> None:
        approved = MessagePolicy(
            consent_granted=True,
            sms_fallback_approved=True,
            sms_cost_class="transactional",
        )
        no_consent = MessagePolicy(
            consent_granted=False,
            sms_fallback_approved=True,
            sms_cost_class="transactional",
        )
        no_approval = MessagePolicy(
            consent_granted=True,
            sms_fallback_approved=False,
            sms_cost_class="transactional",
        )
        no_cost_class = MessagePolicy(
            consent_granted=True,
            sms_fallback_approved=True,
            sms_cost_class=None,
        )

        self.assertTrue(approved.allows_primary())
        self.assertTrue(approved.allows_sms_fallback())
        self.assertFalse(no_consent.allows_primary())
        self.assertFalse(no_consent.allows_sms_fallback())
        self.assertFalse(no_approval.allows_sms_fallback())
        self.assertFalse(no_cost_class.allows_sms_fallback())

    def test_accepted_submission_requires_provider_request_id(self) -> None:
        accepted = ProviderSubmission(
            accepted=True,
            request_id="provider-reference",
        )

        self.assertTrue(accepted.accepted)
        with self.assertRaises(ValueError):
            ProviderSubmission(accepted=True, request_id=None)

    def test_push_result_carries_attempt_correlation_time_and_cost(
        self,
    ) -> None:
        result_at = datetime(2026, 7, 26, tzinfo=timezone.utc)
        try:
            result = ProviderPushResult(
                result_id="result-reference",
                request_id="provider-reference",
                submission_token="attempt-reference",
                channel=DeliveryChannel.ALIMTALK,
                delivered=True,
                result_at=result_at,
                cost_amount=Decimal("12.3400"),
            )
        except TypeError:
            self.fail(
                "ProviderPushResult must carry submission_token, "
                "result_at, and canonical cost_amount"
            )
            return

        self.assertEqual(result.submission_token, "attempt-reference")
        self.assertEqual(result.result_at, result_at)
        self.assertEqual(result.cost_amount, Decimal("12.3400"))

        invalid_costs = (
            Decimal("-0.0001"),
            Decimal("1.00001"),
            Decimal("10000000000.0000"),
            Decimal("NaN"),
        )
        for invalid_cost in invalid_costs:
            with self.subTest(cost=invalid_cost):
                with self.assertRaises(ValueError):
                    ProviderPushResult(
                        result_id="result-reference",
                        request_id="provider-reference",
                        submission_token="attempt-reference",
                        channel=DeliveryChannel.ALIMTALK,
                        delivered=True,
                        result_at=result_at,
                        cost_amount=invalid_cost,
                    )

    def test_submission_registration_reports_current_state(self) -> None:
        if not hasattr(domain_module, "SubmissionRegistration"):
            self.fail(
                "repository registration contract must expose "
                "SubmissionRegistration"
            )
            return
        registration_type = domain_module.SubmissionRegistration
        registration = registration_type(
            applied=False,
            duplicate=True,
            current_status=MessageStatus.SENT,
        )

        self.assertFalse(registration.applied)
        self.assertTrue(registration.duplicate)
        self.assertEqual(
            registration.current_status,
            MessageStatus.SENT,
        )
        with self.assertRaises(ValueError):
            registration_type(
                applied=True,
                duplicate=True,
                current_status=MessageStatus.WAITING_RESULT,
            )

    def test_direct_sms_job_requires_fallback_provenance(self) -> None:
        common = {
            "message_id": "message-1",
            "lease_token": "lease-1",
            "dedupe_key": "dedupe-1",
            "recipient": "PRIVATE_RECIPIENT_SENTINEL",
            "template_code": "template-reference",
            "template_body": "body",
            "history_url": "https://service.invalid/s/reference",
            "sms_body": "sms body",
            "attempt_count": 1,
            "policy": MessagePolicy(
                consent_granted=True,
                sms_fallback_approved=True,
                sms_cost_class="transactional",
            ),
        }

        with self.assertRaises(ValueError):
            MessageJob(
                **common,
                delivery_channel=DeliveryChannel.SMS,
                fallback_reason=None,
            )

        fallback = MessageJob(
            **common,
            delivery_channel=DeliveryChannel.SMS,
            fallback_reason="alimtalk_final_failure",
        )
        self.assertEqual(fallback.delivery_channel, DeliveryChannel.SMS)


if __name__ == "__main__":
    unittest.main()
