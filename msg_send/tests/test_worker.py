from __future__ import annotations

import io
import logging
import threading
import unittest
from dataclasses import dataclass, replace
from datetime import datetime, timedelta, timezone
from decimal import Decimal
from typing import (
    Callable,
    Dict,
    List,
    Optional,
    Sequence,
    Set,
    Union,
)

from msg_send.bizppurio import (
    AccessToken,
    BizppurioAdapter,
    BizppurioRequest,
    BizppurioTransportResponse,
)
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
    MessageStatus,
    ProviderPushResult,
    ProviderResultMode,
    ProviderSubmission,
    PushAction,
    PushApplication,
    RetryPolicy,
    WorkerConfig,
    transition_status,
)
from msg_send.worker import MessageWorker


@dataclass
class StoredMessage:
    job: MessageJob
    status: MessageStatus = MessageStatus.PENDING
    expires_at: Optional[datetime] = None
    lease_expires_at: Optional[datetime] = None
    next_attempt_at: Optional[datetime] = None
    provider_request_id: Optional[str] = None
    submission_token: Optional[str] = None
    submission_started_at: Optional[datetime] = None
    waiting_since: Optional[datetime] = None
    provider_result_at: Optional[datetime] = None
    provider_cost: Optional[Decimal] = None
    sent_count: int = 0
    failure_code: Optional[str] = None


class FakeDatabase:
    def __init__(self) -> None:
        self.rows: Dict[str, StoredMessage] = {}
        self.claim_limits: List[int] = []
        self.recovery_calls: List[Dict[str, int]] = []
        self.applied_result_ids: Set[str] = set()
        self.applied_request_ids: Set[str] = set()
        self.fallback_rejections: List[str] = []
        self.submission_registrations: List[
            Dict[str, object]
        ] = []
        self.acceptance_successes: List[Dict[str, object]] = []
        self.submission_starts: List[Dict[str, object]] = []
        self.submission_waiting_hook: Optional[
            Callable[[], None]
        ] = None
        self.fail_submission_waiting_once = False
        self.fail_submission_start_once = False
        self.fail_submission_start_after_commit_once = False
        self.fail_recovery_once = False
        self.database_now: Optional[datetime] = None
        self._lock = threading.Lock()

    def add(
        self,
        job: MessageJob,
        status: MessageStatus = MessageStatus.PENDING,
        lease_expires_at: Optional[datetime] = None,
        expires_at: Optional[datetime] = None,
    ) -> None:
        self.rows[job.message_id] = StoredMessage(
            job=job,
            status=status,
            expires_at=(
                expires_at
                if expires_at is not None
                else datetime.max.replace(tzinfo=timezone.utc)
            ),
            lease_expires_at=lease_expires_at,
        )

    def recover_expired_leases(
        self,
        now: datetime,
        limit: int,
        max_attempts: int,
        base_delay_seconds: int,
        max_delay_seconds: int,
        waiting_result_timeout_seconds: Optional[int] = None,
    ) -> int:
        self.database_now = now
        if self.fail_recovery_once:
            self.fail_recovery_once = False
            raise RuntimeError("injected recovery failure")
        self.recovery_calls.append(
            {
                "limit": limit,
                "max_attempts": max_attempts,
                "base_delay_seconds": base_delay_seconds,
                "max_delay_seconds": max_delay_seconds,
                "waiting_result_timeout_seconds": (
                    waiting_result_timeout_seconds
                ),
            }
        )
        result_timeout = (
            waiting_result_timeout_seconds
            if waiting_result_timeout_seconds is not None
            else 3600
        )
        retry_policy = RetryPolicy(
            max_attempts=max_attempts,
            base_delay_seconds=base_delay_seconds,
            max_delay_seconds=max_delay_seconds,
        )
        recovered = 0
        for row in self.rows.values():
            if recovered >= limit:
                break
            if (
                row.status
                in (MessageStatus.PENDING, MessageStatus.RETRY_WAIT)
                and row.expires_at is not None
                and row.expires_at <= now
            ):
                row.status = MessageStatus.FAILED
                row.failure_code = "queue_expired"
                recovered += 1
                continue
            if (
                row.status is MessageStatus.WAITING_RESULT
                and (
                    (
                        row.waiting_since is not None
                        and row.waiting_since
                        <= now - timedelta(seconds=result_timeout)
                    )
                    or (
                        row.expires_at is not None
                        and row.expires_at <= now
                    )
                )
            ):
                row.status = transition_status(
                    row.status,
                    MessageStatus.FAILED,
                )
                row.failure_code = "provider_result_timeout"
                recovered += 1
                continue
            if (
                row.status is MessageStatus.ACCEPTED
                and row.submission_started_at is not None
            ):
                if (
                    row.expires_at is not None
                    and row.expires_at <= now
                ) or (
                    row.submission_started_at
                    <= now - timedelta(seconds=result_timeout)
                ):
                    row.status = transition_status(
                        row.status,
                        MessageStatus.FAILED,
                    )
                    row.failure_code = (
                        "provider_submission_ambiguous_timeout"
                    )
                    row.lease_expires_at = None
                    recovered += 1
                continue
            if (
                row.status
                not in (
                    MessageStatus.PROCESSING,
                    MessageStatus.ACCEPTED,
                )
                or row.lease_expires_at is None
                or row.lease_expires_at > now
            ):
                continue
            delay = retry_policy.next_delay_seconds(
                row.job.attempt_count
            )
            if delay is None:
                row.status = transition_status(
                    row.status,
                    MessageStatus.FAILED,
                )
                row.failure_code = "lease_attempts_exhausted"
            else:
                row.status = transition_status(
                    row.status,
                    MessageStatus.RETRY_WAIT,
                )
                row.next_attempt_at = now + timedelta(seconds=delay)
            recovered += 1
        return recovered

    def claim_messages(
        self,
        worker_id: str,
        now: datetime,
        lease_seconds: int,
        limit: int,
    ) -> Sequence[MessageJob]:
        del worker_id
        self.claim_limits.append(limit)
        claimed: List[MessageJob] = []
        with self._lock:
            for row in self.rows.values():
                due = (
                    row.status is MessageStatus.PENDING
                    or (
                        row.status is MessageStatus.RETRY_WAIT
                        and row.next_attempt_at is not None
                        and row.next_attempt_at <= now
                    )
                )
                if not due or len(claimed) >= limit:
                    continue
                row.status = transition_status(
                    row.status,
                    MessageStatus.PROCESSING,
                )
                next_attempt = row.job.attempt_count + 1
                row.job = replace(
                    row.job,
                    attempt_count=next_attempt,
                    lease_token=(
                        f"lease-{row.job.message_id}-"
                        f"attempt-{next_attempt}"
                    ),
                )
                row.lease_expires_at = now + timedelta(
                    seconds=lease_seconds
                )
                claimed.append(row.job)
        return claimed

    def suppress_message(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        with self._lock:
            row = self.rows[job.message_id]
            row.status = transition_status(
                row.status,
                MessageStatus.SUPPRESSED,
            )
            row.failure_code = reason_code

    def suppress_duplicate(
        self,
        job: MessageJob,
        now: datetime,
    ) -> None:
        self.suppress_message(job, "duplicate_dedupe_key", now)

    def mark_submission_started(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> None:
        with self._lock:
            if self.fail_submission_start_once:
                self.fail_submission_start_once = False
                raise RuntimeError(
                    "injected submission start failure"
                )
            row = self.rows[job.message_id]
            if (
                row.status is not MessageStatus.PROCESSING
                or row.job.lease_token != job.lease_token
                or row.job.delivery_channel is not channel
            ):
                raise RuntimeError("stale submission start")
            if self.database_now is None:
                raise RuntimeError("database clock is unavailable")
            row.status = transition_status(
                row.status,
                MessageStatus.ACCEPTED,
            )
            row.submission_token = job.lease_token
            row.submission_started_at = self.database_now
            self.submission_starts.append(
                {
                    "message_id": job.message_id,
                    "submission_token": job.lease_token,
                    "channel": channel,
                    "started_at": self.database_now,
                }
            )
            if self.fail_submission_start_after_commit_once:
                self.fail_submission_start_after_commit_once = False
                raise RuntimeError(
                    "injected submission start response loss"
                )

    def mark_submission_waiting_result(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
    ) -> Dict[str, object]:
        with self._lock:
            if self.fail_submission_waiting_once:
                self.fail_submission_waiting_once = False
                raise RuntimeError(
                    "injected submission registration failure"
                )
            row = self.rows[job.message_id]
            if (
                row.provider_request_id == provider_request_id
                and row.submission_token == job.lease_token
                and row.status
                in (
                    MessageStatus.WAITING_RESULT,
                    MessageStatus.SENT,
                    MessageStatus.FAILED,
                    MessageStatus.RETRY_WAIT,
                    MessageStatus.PROCESSING,
                    MessageStatus.ACCEPTED,
                )
            ):
                result = {
                    "applied": False,
                    "duplicate": True,
                    "current_status": row.status,
                }
                self.submission_registrations.append(result)
                return result
            if (
                row.status is not MessageStatus.ACCEPTED
                or row.job.lease_token != job.lease_token
                or row.submission_token != job.lease_token
                or row.submission_started_at is None
            ):
                raise RuntimeError("stale submission registration")
            row.status = transition_status(
                row.status,
                MessageStatus.WAITING_RESULT,
            )
            row.provider_request_id = provider_request_id
            row.submission_token = job.lease_token
            row.job = replace(row.job, delivery_channel=channel)
            row.waiting_since = now
            result = {
                "applied": True,
                "duplicate": False,
                "current_status": row.status,
            }
            self.submission_registrations.append(result)
        if self.submission_waiting_hook is not None:
            self.submission_waiting_hook()
        return result

    def mark_submission_accepted_success(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
        cost_amount: Decimal,
    ) -> Dict[str, object]:
        with self._lock:
            row = self.rows[job.message_id]
            if (
                row.status is not MessageStatus.ACCEPTED
                or row.job.lease_token != job.lease_token
                or row.submission_token != job.lease_token
                or row.submission_started_at is None
            ):
                raise RuntimeError("stale acceptance success")
            row.status = transition_status(
                row.status,
                MessageStatus.WAITING_RESULT,
            )
            row.status = transition_status(
                row.status,
                MessageStatus.SENT,
            )
            row.provider_request_id = provider_request_id
            row.provider_result_at = now
            row.provider_cost = cost_amount
            row.sent_count += 1
            result = {
                "applied": True,
                "duplicate": False,
                "current_status": row.status,
                "provider_result_code": "1000",
                "cost_amount": cost_amount,
                "channel": channel,
            }
            self.acceptance_successes.append(result)
            return result

    def schedule_retry(
        self,
        job: MessageJob,
        next_attempt_at: datetime,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        with self._lock:
            row = self.rows[job.message_id]
            row.status = transition_status(
                row.status,
                MessageStatus.RETRY_WAIT,
            )
            row.next_attempt_at = next_attempt_at
            row.failure_code = reason_code

    def mark_failed(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        with self._lock:
            row = self.rows[job.message_id]
            row.status = transition_status(
                row.status,
                MessageStatus.FAILED,
            )
            row.failure_code = reason_code

    def apply_push_result(
        self,
        result: ProviderPushResult,
        worker_id: str,
        now: datetime,
        lease_seconds: int,
        max_attempts: int,
        base_delay_seconds: int,
        max_delay_seconds: int,
    ) -> PushApplication:
        del worker_id
        self.database_now = now
        retry_policy = RetryPolicy(
            max_attempts=max_attempts,
            base_delay_seconds=base_delay_seconds,
            max_delay_seconds=max_delay_seconds,
        )
        with self._lock:
            if (
                result.result_id in self.applied_result_ids
            ):
                return PushApplication(PushAction.DUPLICATE)
            row = next(
                (
                    candidate
                    for candidate in self.rows.values()
                    if (
                        candidate.submission_token
                        == result.submission_token
                        and candidate.provider_request_id
                        == result.request_id
                    )
                    or (
                        candidate.status is MessageStatus.ACCEPTED
                        and candidate.submission_token
                        == result.submission_token
                        and candidate.job.delivery_channel
                        is result.channel
                    )
                ),
                None,
            )
            if row is None:
                return PushApplication(PushAction.UNKNOWN)
            if (
                row.status is not MessageStatus.ACCEPTED
                and row.provider_request_id is not None
                and row.provider_request_id != result.request_id
            ):
                return PushApplication(PushAction.UNKNOWN)
            if row.status is MessageStatus.ACCEPTED:
                row.status = transition_status(
                    row.status,
                    MessageStatus.WAITING_RESULT,
                )
                row.provider_request_id = result.request_id
                row.submission_token = result.submission_token
                row.waiting_since = result.result_at
                row.lease_expires_at = None

            self.applied_result_ids.add(result.result_id)
            self.applied_request_ids.add(result.request_id)
            row.provider_result_at = result.result_at
            row.provider_cost = result.cost_amount

            if result.delivered:
                row.status = transition_status(
                    row.status,
                    MessageStatus.SENT,
                )
                row.sent_count += 1
                return PushApplication(PushAction.DELIVERED)

            delay = retry_policy.next_delay_seconds(
                row.job.attempt_count
            )
            if result.retryable and delay is not None:
                row.status = transition_status(
                    row.status,
                    MessageStatus.RETRY_WAIT,
                )
                row.next_attempt_at = now + timedelta(seconds=delay)
                return PushApplication(PushAction.RETRY_SCHEDULED)

            if (
                result.channel is DeliveryChannel.ALIMTALK
                and row.job.policy.allows_sms_fallback()
            ):
                row.status = transition_status(
                    row.status,
                    MessageStatus.RETRY_WAIT,
                )
                row.status = transition_status(
                    row.status,
                    MessageStatus.PROCESSING,
                )
                fallback_attempt = row.job.attempt_count + 1
                row.job = replace(
                    row.job,
                    attempt_count=fallback_attempt,
                    lease_token=(
                        f"lease-{row.job.message_id}-"
                        f"sms-attempt-{fallback_attempt}"
                    ),
                    delivery_channel=DeliveryChannel.SMS,
                    fallback_reason=(
                        result.failure_code
                        or "alimtalk_final_failure"
                    ),
                )
                row.lease_expires_at = now + timedelta(
                    seconds=lease_seconds
                )
                return PushApplication(
                    PushAction.SMS_FALLBACK,
                    row.job,
                )

            row.status = transition_status(
                row.status,
                MessageStatus.FAILED,
            )
            row.failure_code = (
                result.failure_code or "provider_delivery_failed"
            )
            return PushApplication(PushAction.FAILED)

    def reject_sms_fallback(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        self.fallback_rejections.append(job.message_id)
        self.mark_failed(job, reason_code, now)

    def oldest_waiting_result_at(self) -> Optional[datetime]:
        waiting = [
            (
                row.waiting_since
                if row.status is MessageStatus.WAITING_RESULT
                else row.submission_started_at
            )
            for row in self.rows.values()
            if (
                (
                    row.status is MessageStatus.WAITING_RESULT
                    and row.waiting_since is not None
                )
                or (
                    row.status is MessageStatus.ACCEPTED
                    and row.submission_started_at is not None
                )
            )
        ]
        return min(waiting) if waiting else None


class FakeProvider:
    def __init__(
        self,
        responses: Sequence[Union[ProviderSubmission, Exception]],
    ) -> None:
        self.responses = list(responses)
        self.calls: List[DeliveryChannel] = []
        self.call_tokens: List[str] = []
        self.before_return: Optional[
            Callable[[MessageJob, ProviderSubmission], None]
        ] = None
        self._lock = threading.Lock()

    def submit(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> ProviderSubmission:
        with self._lock:
            self.calls.append(channel)
            self.call_tokens.append(job.lease_token)
            response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        if self.before_return is not None:
            self.before_return(job, response)
        return response


class BoundaryTransport:
    def __init__(
        self,
        now: datetime,
        *,
        token_error: Optional[Exception] = None,
        submit_error: Optional[Exception] = None,
    ) -> None:
        self.now = now
        self.token_error = token_error
        self.submit_error = submit_error
        self.token_fetches = 0
        self.requests: List[BizppurioRequest] = []

    def fetch_access_token(self) -> AccessToken:
        self.token_fetches += 1
        if self.token_error is not None:
            raise self.token_error
        return AccessToken(
            value="PRIVATE_ACCESS_SENTINEL",
            expires_at=self.now + timedelta(minutes=10),
        )

    def submit(
        self,
        access_token: str,
        request: BizppurioRequest,
    ) -> BizppurioTransportResponse:
        del access_token
        self.requests.append(request)
        if self.submit_error is not None:
            raise self.submit_error
        return BizppurioTransportResponse(
            accepted=True,
            request_id="provider-reference",
        )


def make_job(
    message_id: str = "message-1",
    dedupe_key: str = "dedupe-1",
    consent_granted: bool = True,
    sms_fallback_approved: bool = False,
    attempt_count: int = 0,
) -> MessageJob:
    return MessageJob(
        message_id=message_id,
        lease_token=f"lease-{message_id}",
        dedupe_key=dedupe_key,
        recipient="PRIVATE_RECIPIENT_SENTINEL",
        template_code="template-reference",
        template_body="상세 #{tempHistoryToken}",
        history_url="https://service.invalid/s/opaque-reference",
        sms_body="SMS 상세 https://service.invalid/s/opaque-reference",
        attempt_count=attempt_count,
        policy=MessagePolicy(
            consent_granted=consent_granted,
            sms_fallback_approved=sms_fallback_approved,
            sms_cost_class=(
                "transactional"
                if sms_fallback_approved
                else None
            ),
        ),
        delivery_channel=DeliveryChannel.ALIMTALK,
        fallback_reason=None,
    )


def make_push_result(
    *,
    result_id: str,
    request_id: str,
    submission_token: str,
    channel: DeliveryChannel,
    delivered: bool,
    retryable: bool = False,
    failure_code: Optional[str] = None,
    result_at: Optional[datetime] = None,
    cost_amount: Decimal = Decimal("0.0100"),
) -> ProviderPushResult:
    try:
        return ProviderPushResult(
            result_id=result_id,
            request_id=request_id,
            submission_token=submission_token,
            channel=channel,
            delivered=delivered,
            retryable=retryable,
            failure_code=failure_code,
            result_at=(
                result_at
                if result_at is not None
                else datetime(2026, 7, 26, tzinfo=timezone.utc)
            ),
            cost_amount=cost_amount,
        )
    except TypeError:
        raise AssertionError(
            "ProviderPushResult must expose attempt token, result time, "
            "and canonical cost"
        ) from None


class MessageWorkerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.now = datetime(2026, 7, 26, tzinfo=timezone.utc)
        self.config = WorkerConfig(
            poll_interval_seconds=45,
            max_concurrency=4,
            lease_seconds=90,
            recovery_batch_size=8,
            result_lag_threshold_seconds=120,
        )
        self.retry_policy = RetryPolicy(
            max_attempts=3,
            base_delay_seconds=5,
            max_delay_seconds=20,
        )

    def worker(
        self,
        database: FakeDatabase,
        provider: FakeProvider,
        logger: Optional[logging.Logger] = None,
        *,
        provider_result_mode: ProviderResultMode = (
            ProviderResultMode.CALLBACK
        ),
    ) -> MessageWorker:
        return MessageWorker(
            database=database,
            provider=provider,
            config=self.config,
            retry_policy=self.retry_policy,
            worker_id="worker-1",
            clock=lambda: self.now,
            logger=logger,
            provider_result_mode=provider_result_mode,
            provider_acceptance_cost=Decimal("8.5000"),
        )

    def test_api_acceptance_waits_for_push_delivery(self) -> None:
        database = FakeDatabase()
        database.add(make_job())
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )

        self.worker(database, provider).run_once()

        row = database.rows["message-1"]
        self.assertEqual(row.status, MessageStatus.WAITING_RESULT)
        self.assertEqual(row.sent_count, 0)
        self.assertEqual(
            row.job.delivery_channel,
            DeliveryChannel.ALIMTALK,
        )

    def test_provider_acceptance_mode_atomically_marks_success(self) -> None:
        database = FakeDatabase()
        database.add(make_job())
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )

        cycle = self.worker(
            database,
            provider,
            provider_result_mode=(
                ProviderResultMode.PROVIDER_ACCEPTANCE
            ),
        ).run_once()

        row = database.rows["message-1"]
        self.assertTrue(cycle.success)
        self.assertEqual(row.status, MessageStatus.SENT)
        self.assertEqual(row.sent_count, 1)
        self.assertEqual(database.submission_registrations, [])
        self.assertEqual(len(database.acceptance_successes), 1)
        success = database.acceptance_successes[0]
        self.assertEqual(success["provider_result_code"], "1000")
        self.assertEqual(success["cost_amount"], Decimal("8.5000"))
        self.assertIs(success["channel"], DeliveryChannel.ALIMTALK)

    def test_push_success_is_idempotent(self) -> None:
        database = FakeDatabase()
        database.add(make_job())
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        worker = self.worker(database, provider)
        worker.run_once()
        result = make_push_result(
            result_id="result-reference",
            request_id="provider-reference",
            submission_token=database.rows["message-1"].job.lease_token,
            channel=DeliveryChannel.ALIMTALK,
            delivered=True,
            result_at=self.now,
        )

        first = worker.handle_push_result(result)
        second = worker.handle_push_result(result)

        row = database.rows["message-1"]
        self.assertEqual(first, PushAction.DELIVERED)
        self.assertEqual(second, PushAction.DUPLICATE)
        self.assertEqual(row.status, MessageStatus.SENT)
        self.assertEqual(row.sent_count, 1)

    def test_atomic_submission_registration_closes_fast_push_race(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        worker = self.worker(database, provider)
        fast_actions: List[PushAction] = []
        fast_results: List[ProviderPushResult] = []

        def deliver_after_registration() -> None:
            result = make_push_result(
                result_id="fast-result-reference",
                request_id="provider-reference",
                submission_token=(
                    database.rows["message-1"].job.lease_token
                ),
                channel=DeliveryChannel.ALIMTALK,
                delivered=True,
                result_at=self.now,
            )
            fast_results.append(result)
            fast_actions.append(worker.handle_push_result(result))

        database.submission_waiting_hook = (
            deliver_after_registration
        )

        cycle = worker.run_once()

        row = database.rows["message-1"]
        self.assertTrue(cycle.success)
        self.assertEqual(fast_actions, [PushAction.DELIVERED])
        self.assertEqual(row.status, MessageStatus.SENT)
        self.assertEqual(row.sent_count, 1)
        self.assertEqual(
            worker.handle_push_result(fast_results[0]),
            PushAction.DUPLICATE,
        )
        self.assertEqual(row.sent_count, 1)

    def test_callback_inside_provider_submit_is_applied_exactly_once(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        worker = self.worker(database, provider)
        callback_actions: List[PushAction] = []
        callback_results: List[ProviderPushResult] = []

        def deliver_before_submit_returns(
            job: MessageJob,
            submission: ProviderSubmission,
        ) -> None:
            self.assertIsNotNone(submission.request_id)
            result = make_push_result(
                result_id="inside-submit-result",
                request_id=submission.request_id or "",
                submission_token=job.lease_token,
                channel=job.delivery_channel,
                delivered=True,
                result_at=self.now,
                cost_amount=Decimal("0.0100"),
            )
            callback_results.append(result)
            callback_actions.append(worker.handle_push_result(result))

        provider.before_return = deliver_before_submit_returns

        cycle = worker.run_once()

        row = database.rows["message-1"]
        self.assertTrue(cycle.success)
        self.assertEqual(callback_actions, [PushAction.DELIVERED])
        self.assertEqual(row.status, MessageStatus.SENT)
        self.assertEqual(row.sent_count, 1)
        self.assertEqual(row.provider_result_at, self.now)
        self.assertEqual(row.provider_cost, Decimal("0.0100"))
        self.assertEqual(
            database.submission_registrations,
            [
                {
                    "applied": False,
                    "duplicate": True,
                    "current_status": MessageStatus.SENT,
                }
            ],
        )
        self.assertEqual(
            worker.handle_push_result(callback_results[0]),
            PushAction.DUPLICATE,
        )
        self.assertEqual(row.sent_count, 1)

    def test_unknown_or_mismatched_submission_token_fails_closed(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        worker = self.worker(database, provider)
        worker.run_once()
        row = database.rows["message-1"]
        original_status = row.status
        original_token = row.submission_token

        action = worker.handle_push_result(
            make_push_result(
                result_id="mismatched-result",
                request_id="provider-reference",
                submission_token="unrelated-attempt-token",
                channel=DeliveryChannel.ALIMTALK,
                delivered=True,
                result_at=self.now,
            )
        )

        self.assertEqual(action, PushAction.UNKNOWN)
        self.assertEqual(row.status, original_status)
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(row.sent_count, 0)

    def test_registration_failure_after_accept_never_resubmits(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        database.fail_submission_waiting_once = True
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        logger = logging.getLogger(
            f"msg_send_atomic_failure_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)

        failed_cycle = worker.run_once()

        row = database.rows["message-1"]
        self.assertFalse(failed_cycle.success)
        self.assertEqual(row.status, MessageStatus.ACCEPTED)
        original_token = row.job.lease_token
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(row.submission_started_at, self.now)
        self.assertIsNone(row.provider_request_id)
        self.assertIsNone(row.waiting_since)
        self.assertEqual(provider.calls, [DeliveryChannel.ALIMTALK])
        self.assertEqual(provider.call_tokens, [original_token])

        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovered_cycle = worker.run_once(now=recovery_now)

        self.assertEqual(recovered_cycle.recovered_leases, 0)
        self.assertEqual(row.status, MessageStatus.ACCEPTED)
        self.assertEqual(row.job.lease_token, original_token)
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(provider.calls, [DeliveryChannel.ALIMTALK])
        self.assertEqual(provider.call_tokens, [original_token])

        timeout_now = self.now + timedelta(
            seconds=self.config.result_timeout_seconds + 1
        )
        timeout_cycle = worker.run_once(now=timeout_now)

        self.assertEqual(timeout_cycle.recovered_leases, 1)
        self.assertEqual(row.status, MessageStatus.FAILED)
        self.assertEqual(
            row.failure_code,
            "provider_submission_ambiguous_timeout",
        )
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(provider.calls, [DeliveryChannel.ALIMTALK])

    def test_submission_start_failure_calls_provider_zero_times(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        database.fail_submission_start_once = True
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        logger = logging.getLogger(
            f"msg_send_start_failure_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False

        cycle = self.worker(
            database,
            provider,
            logger=logger,
        ).run_once()

        row = database.rows["message-1"]
        self.assertFalse(cycle.success)
        self.assertEqual(row.status, MessageStatus.PROCESSING)
        self.assertIsNone(row.submission_token)
        self.assertIsNone(row.submission_started_at)
        self.assertEqual(database.submission_starts, [])
        self.assertEqual(provider.calls, [])
        self.assertEqual(provider.call_tokens, [])

    def test_submission_start_response_loss_never_rewinds_or_sends(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        database.fail_submission_start_after_commit_once = True
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        logger = logging.getLogger(
            f"msg_send_start_response_loss_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)

        failed_cycle = worker.run_once()
        row = database.rows["message-1"]
        original_token = row.job.lease_token
        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovery_cycle = worker.run_once(now=recovery_now)

        self.assertFalse(failed_cycle.success)
        self.assertEqual(recovery_cycle.recovered_leases, 0)
        self.assertEqual(row.status, MessageStatus.ACCEPTED)
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(row.submission_started_at, self.now)
        self.assertEqual(provider.calls, [])
        self.assertEqual(provider.call_tokens, [])

    def test_late_callback_after_lease_expiry_applies_exactly_once(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        database.fail_submission_waiting_once = True
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )
        logger = logging.getLogger(
            f"msg_send_late_callback_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)

        failed_cycle = worker.run_once()
        row = database.rows["message-1"]
        original_token = row.job.lease_token
        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovered_cycle = worker.run_once(now=recovery_now)
        result = make_push_result(
            result_id="late-result-reference",
            request_id="provider-reference",
            submission_token=original_token,
            channel=DeliveryChannel.ALIMTALK,
            delivered=True,
            result_at=recovery_now,
        )

        first = worker.handle_push_result(result, now=recovery_now)
        second = worker.handle_push_result(result, now=recovery_now)
        replay = database.mark_submission_waiting_result(
            row.job,
            "provider-reference",
            DeliveryChannel.ALIMTALK,
            recovery_now,
        )

        self.assertFalse(failed_cycle.success)
        self.assertEqual(recovered_cycle.recovered_leases, 0)
        self.assertEqual(first, PushAction.DELIVERED)
        self.assertEqual(second, PushAction.DUPLICATE)
        self.assertTrue(replay["duplicate"])
        self.assertEqual(
            replay["current_status"],
            MessageStatus.SENT,
        )
        self.assertEqual(row.status, MessageStatus.SENT)
        self.assertEqual(row.sent_count, 1)
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(provider.calls, [DeliveryChannel.ALIMTALK])
        self.assertEqual(provider.call_tokens, [original_token])

    def test_provider_response_loss_remains_ambiguous_without_retry(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        provider = FakeProvider(
            [RuntimeError("provider response lost")]
        )
        logger = logging.getLogger(
            f"msg_send_response_loss_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)

        first_cycle = worker.run_once()
        row = database.rows["message-1"]
        original_token = row.job.lease_token
        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovery_cycle = worker.run_once(now=recovery_now)

        self.assertTrue(first_cycle.success)
        self.assertEqual(recovery_cycle.recovered_leases, 0)
        self.assertEqual(row.status, MessageStatus.ACCEPTED)
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(row.submission_started_at, self.now)
        self.assertEqual(provider.calls, [DeliveryChannel.ALIMTALK])
        self.assertEqual(provider.call_tokens, [original_token])

        timeout_now = self.now + timedelta(
            seconds=self.config.result_timeout_seconds + 1
        )
        timeout_cycle = worker.run_once(now=timeout_now)

        self.assertEqual(timeout_cycle.recovered_leases, 1)
        self.assertEqual(row.status, MessageStatus.FAILED)
        self.assertEqual(
            row.failure_code,
            "provider_submission_ambiguous_timeout",
        )
        self.assertEqual(provider.calls, [DeliveryChannel.ALIMTALK])

        late = worker.handle_push_result(
            make_push_result(
                result_id="too-late-result",
                request_id="unknown-provider-reference",
                submission_token=original_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=True,
                result_at=timeout_now,
            ),
            now=timeout_now,
        )

        self.assertEqual(late, PushAction.UNKNOWN)
        self.assertEqual(row.status, MessageStatus.FAILED)
        self.assertEqual(row.sent_count, 0)
        self.assertIsNone(row.provider_result_at)

    def test_token_endpoint_failure_before_submit_is_bounded_retry(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        transport = BoundaryTransport(
            self.now,
            token_error=RuntimeError("token endpoint unavailable"),
        )
        provider = BizppurioAdapter(
            transport,
            clock=lambda: self.now,
        )
        logger = logging.getLogger(
            f"msg_send_token_failure_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False

        cycle = self.worker(
            database,
            provider,
            logger=logger,
        ).run_once()

        row = database.rows["message-1"]
        self.assertTrue(cycle.success)
        self.assertEqual(row.status, MessageStatus.RETRY_WAIT)
        self.assertEqual(
            row.next_attempt_at,
            self.now + timedelta(seconds=5),
        )
        self.assertEqual(
            row.failure_code,
            "provider_submit_not_attempted",
        )
        self.assertEqual(transport.token_fetches, 1)
        self.assertEqual(transport.requests, [])

    def test_transport_response_loss_after_submit_stays_ambiguous(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job())
        transport = BoundaryTransport(
            self.now,
            submit_error=RuntimeError("provider response lost"),
        )
        provider = BizppurioAdapter(
            transport,
            clock=lambda: self.now,
        )
        logger = logging.getLogger(
            f"msg_send_transport_loss_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)

        first_cycle = worker.run_once()
        row = database.rows["message-1"]
        original_token = row.job.lease_token
        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovery_cycle = worker.run_once(now=recovery_now)

        self.assertTrue(first_cycle.success)
        self.assertEqual(recovery_cycle.recovered_leases, 0)
        self.assertEqual(row.status, MessageStatus.ACCEPTED)
        self.assertEqual(row.submission_token, original_token)
        self.assertEqual(len(transport.requests), 1)
        self.assertEqual(
            transport.requests[0].message_key,
            original_token,
        )

    def test_retry_backoff_stops_at_max_attempts(self) -> None:
        retrying_db = FakeDatabase()
        retrying_db.add(make_job(message_id="retrying"))
        retrying_provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=False,
                    retryable=True,
                    failure_code="temporary_rejection",
                )
            ]
        )
        self.worker(retrying_db, retrying_provider).run_once()

        retrying = retrying_db.rows["retrying"]
        self.assertEqual(retrying.status, MessageStatus.RETRY_WAIT)
        self.assertEqual(
            retrying.next_attempt_at,
            self.now + timedelta(seconds=5),
        )

        exhausted_db = FakeDatabase()
        exhausted_db.add(
            make_job(
                message_id="exhausted",
                attempt_count=2,
            )
        )
        exhausted_provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=False,
                    retryable=True,
                    failure_code="temporary_rejection",
                )
            ]
        )
        self.worker(exhausted_db, exhausted_provider).run_once()

        exhausted = exhausted_db.rows["exhausted"]
        self.assertEqual(exhausted.status, MessageStatus.FAILED)
        self.assertIsNone(exhausted.next_attempt_at)

    def test_expired_lease_recovery_honors_retry_policy(self) -> None:
        database = FakeDatabase()
        database.add(
            make_job(
                message_id="expired",
                attempt_count=1,
            ),
            status=MessageStatus.PROCESSING,
            lease_expires_at=self.now - timedelta(seconds=1),
        )
        provider = FakeProvider([])

        cycle = self.worker(database, provider).run_once()

        row = database.rows["expired"]
        self.assertEqual(cycle.recovered_leases, 1)
        self.assertEqual(row.status, MessageStatus.RETRY_WAIT)
        self.assertEqual(
            row.next_attempt_at,
            self.now + timedelta(seconds=5),
        )
        self.assertEqual(provider.calls, [])
        self.assertEqual(
            database.recovery_calls[0],
            {
                "limit": 8,
                "max_attempts": 3,
                "base_delay_seconds": 5,
                "max_delay_seconds": 20,
                "waiting_result_timeout_seconds": 3600,
            },
        )

    def test_expired_pending_and_retry_wait_are_bounded_and_idempotent(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(
            make_job(
                message_id="expired-pending",
                dedupe_key="dedupe-expired-pending",
            ),
            expires_at=self.now - timedelta(seconds=1),
        )
        database.add(
            make_job(
                message_id="expired-retry",
                dedupe_key="dedupe-expired-retry",
                attempt_count=1,
            ),
            status=MessageStatus.RETRY_WAIT,
            expires_at=self.now - timedelta(seconds=1),
        )
        database.rows[
            "expired-retry"
        ].next_attempt_at = self.now + timedelta(hours=1)
        provider = FakeProvider([])
        config = replace(self.config, recovery_batch_size=1)
        worker = MessageWorker(
            database=database,
            provider=provider,
            config=config,
            retry_policy=self.retry_policy,
            worker_id="worker-1",
            clock=lambda: self.now,
        )

        first = worker.run_once()
        second = worker.run_once()
        repeated = worker.run_once()

        self.assertEqual(first.recovered_leases, 1)
        self.assertEqual(second.recovered_leases, 1)
        self.assertEqual(repeated.recovered_leases, 0)
        self.assertEqual(
            [
                database.rows["expired-pending"].status,
                database.rows["expired-retry"].status,
            ],
            [MessageStatus.FAILED, MessageStatus.FAILED],
        )
        self.assertEqual(
            [
                database.rows["expired-pending"].failure_code,
                database.rows["expired-retry"].failure_code,
            ],
            ["queue_expired", "queue_expired"],
        )
        self.assertEqual(provider.calls, [])

    def test_waiting_result_timeout_terminalizes_without_resubmit(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(
            make_job(),
            status=MessageStatus.WAITING_RESULT,
            expires_at=self.now + timedelta(hours=2),
        )
        row = database.rows["message-1"]
        row.provider_request_id = "provider-reference"
        row.submission_token = row.job.lease_token
        row.waiting_since = self.now - timedelta(seconds=3601)
        provider = FakeProvider([])

        cycle = self.worker(database, provider).run_once()
        repeated = self.worker(database, provider).run_once()

        self.assertEqual(cycle.recovered_leases, 1)
        self.assertEqual(repeated.recovered_leases, 0)
        self.assertEqual(row.status, MessageStatus.FAILED)
        self.assertEqual(row.failure_code, "provider_result_timeout")
        self.assertEqual(row.provider_request_id, "provider-reference")
        self.assertEqual(row.submission_token, row.job.lease_token)
        self.assertEqual(provider.calls, [])
        self.assertEqual(
            database.recovery_calls[0][
                "waiting_result_timeout_seconds"
            ],
            3600,
        )

    def test_expired_accepted_recovery_is_bounded_and_idempotent(
        self,
    ) -> None:
        database = FakeDatabase()
        for message_id in ("accepted-a", "accepted-b"):
            database.add(
                make_job(
                    message_id=message_id,
                    dedupe_key=f"dedupe-{message_id}",
                    attempt_count=1,
                ),
                status=MessageStatus.ACCEPTED,
                lease_expires_at=self.now - timedelta(seconds=1),
            )
            row = database.rows[message_id]
            row.submission_token = row.job.lease_token
            row.submission_started_at = self.now - timedelta(
                seconds=self.config.result_timeout_seconds + 1
            )
        database.fail_recovery_once = True
        provider = FakeProvider([])
        config = replace(self.config, recovery_batch_size=1)
        logger = logging.getLogger(
            f"msg_send_recovery_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = MessageWorker(
            database=database,
            provider=provider,
            config=config,
            retry_policy=self.retry_policy,
            worker_id="worker-1",
            clock=lambda: self.now,
            logger=logger,
        )

        failed_cycle = worker.run_once()
        first_recovery = worker.run_once()
        second_recovery = worker.run_once()
        idempotent_cycle = worker.run_once()

        self.assertFalse(failed_cycle.success)
        self.assertEqual(first_recovery.recovered_leases, 1)
        self.assertEqual(second_recovery.recovered_leases, 1)
        self.assertEqual(idempotent_cycle.recovered_leases, 0)
        self.assertEqual(
            [
                row.status
                for row in database.rows.values()
            ],
            [
                MessageStatus.FAILED,
                MessageStatus.FAILED,
            ],
        )
        self.assertTrue(
            all(
                row.failure_code
                == "provider_submission_ambiguous_timeout"
                for row in database.rows.values()
            )
        )
        self.assertEqual(provider.calls, [])

    def test_sms_fallback_requires_final_failure_and_policy_approval(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(
            make_job(sms_fallback_approved=True)
        )
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="alimtalk-reference",
                ),
                ProviderSubmission(
                    accepted=True,
                    request_id="sms-reference",
                ),
            ]
        )
        worker = self.worker(database, provider)
        worker.run_once()

        action = worker.handle_push_result(
            make_push_result(
                result_id="alimtalk-result",
                request_id="alimtalk-reference",
                submission_token=database.rows[
                    "message-1"
                ].job.lease_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=False,
                retryable=False,
                failure_code="alimtalk_final_failure",
                result_at=self.now,
            )
        )

        row = database.rows["message-1"]
        self.assertEqual(action, PushAction.SMS_FALLBACK)
        self.assertEqual(
            provider.calls,
            [DeliveryChannel.ALIMTALK, DeliveryChannel.SMS],
        )
        self.assertEqual(row.status, MessageStatus.WAITING_RESULT)
        self.assertEqual(
            row.job.delivery_channel,
            DeliveryChannel.SMS,
        )
        self.assertEqual(
            row.job.fallback_reason,
            "alimtalk_final_failure",
        )
        self.assertEqual(row.sent_count, 0)

    def test_sms_registration_failure_never_resubmits(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job(sms_fallback_approved=True))
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="alimtalk-reference",
                ),
                ProviderSubmission(
                    accepted=True,
                    request_id="sms-reference",
                ),
            ]
        )
        logger = logging.getLogger(
            f"msg_send_sms_registration_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)
        worker.run_once()
        row = database.rows["message-1"]
        primary_token = row.job.lease_token
        database.fail_submission_waiting_once = True

        action = worker.handle_push_result(
            make_push_result(
                result_id="alimtalk-failure-result",
                request_id="alimtalk-reference",
                submission_token=primary_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=False,
                retryable=False,
                failure_code="alimtalk_final_failure",
                result_at=self.now,
            )
        )
        fallback_token = row.job.lease_token
        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovery_cycle = worker.run_once(now=recovery_now)

        self.assertEqual(action, PushAction.SMS_FALLBACK)
        self.assertEqual(row.status, MessageStatus.ACCEPTED)
        self.assertEqual(
            row.job.delivery_channel,
            DeliveryChannel.SMS,
        )
        self.assertNotEqual(fallback_token, primary_token)
        self.assertEqual(row.submission_token, fallback_token)
        self.assertEqual(recovery_cycle.recovered_leases, 0)
        self.assertEqual(
            provider.calls,
            [DeliveryChannel.ALIMTALK, DeliveryChannel.SMS],
        )
        self.assertEqual(
            provider.call_tokens,
            [primary_token, fallback_token],
        )

        timeout_now = self.now + timedelta(
            seconds=self.config.result_timeout_seconds + 1
        )
        timeout_cycle = worker.run_once(now=timeout_now)

        self.assertEqual(timeout_cycle.recovered_leases, 1)
        self.assertEqual(row.status, MessageStatus.FAILED)
        self.assertEqual(
            row.failure_code,
            "provider_submission_ambiguous_timeout",
        )
        self.assertEqual(len(provider.calls), 2)

    def test_sms_response_loss_stays_ambiguous_without_resend(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job(sms_fallback_approved=True))
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="alimtalk-reference",
                ),
                RuntimeError("SMS provider response lost"),
            ]
        )
        logger = logging.getLogger(
            f"msg_send_sms_response_loss_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)
        worker.run_once()
        row = database.rows["message-1"]
        primary_token = row.job.lease_token

        action = worker.handle_push_result(
            make_push_result(
                result_id="alimtalk-failure-result",
                request_id="alimtalk-reference",
                submission_token=primary_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=False,
                retryable=False,
                failure_code="alimtalk_final_failure",
                result_at=self.now,
            )
        )
        fallback_token = row.job.lease_token
        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovery_cycle = worker.run_once(now=recovery_now)

        self.assertEqual(action, PushAction.SMS_FALLBACK)
        self.assertEqual(recovery_cycle.recovered_leases, 0)
        self.assertEqual(row.status, MessageStatus.ACCEPTED)
        self.assertNotEqual(fallback_token, primary_token)
        self.assertEqual(row.submission_token, fallback_token)
        self.assertEqual(
            provider.calls,
            [DeliveryChannel.ALIMTALK, DeliveryChannel.SMS],
        )
        self.assertEqual(
            provider.call_tokens,
            [primary_token, fallback_token],
        )

    def test_sms_late_callback_after_lease_expiry_is_exactly_once(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job(sms_fallback_approved=True))
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="alimtalk-reference",
                ),
                ProviderSubmission(
                    accepted=True,
                    request_id="sms-reference",
                ),
            ]
        )
        logger = logging.getLogger(
            f"msg_send_sms_late_callback_test_{id(database)}"
        )
        logger.handlers = [logging.NullHandler()]
        logger.propagate = False
        worker = self.worker(database, provider, logger=logger)
        worker.run_once()
        row = database.rows["message-1"]
        primary_token = row.job.lease_token
        database.fail_submission_waiting_once = True
        worker.handle_push_result(
            make_push_result(
                result_id="alimtalk-failure-result",
                request_id="alimtalk-reference",
                submission_token=primary_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=False,
                retryable=False,
                failure_code="alimtalk_final_failure",
                result_at=self.now,
            )
        )
        fallback_token = row.job.lease_token
        recovery_now = self.now + timedelta(
            seconds=self.config.lease_seconds + 1
        )
        recovery_cycle = worker.run_once(now=recovery_now)
        result = make_push_result(
            result_id="late-sms-result",
            request_id="sms-reference",
            submission_token=fallback_token,
            channel=DeliveryChannel.SMS,
            delivered=True,
            result_at=recovery_now,
        )

        first = worker.handle_push_result(result, now=recovery_now)
        second = worker.handle_push_result(result, now=recovery_now)
        replay = database.mark_submission_waiting_result(
            row.job,
            "sms-reference",
            DeliveryChannel.SMS,
            recovery_now,
        )

        self.assertEqual(recovery_cycle.recovered_leases, 0)
        self.assertEqual(first, PushAction.DELIVERED)
        self.assertEqual(second, PushAction.DUPLICATE)
        self.assertTrue(replay["duplicate"])
        self.assertEqual(
            replay["current_status"],
            MessageStatus.SENT,
        )
        self.assertEqual(row.status, MessageStatus.SENT)
        self.assertEqual(row.sent_count, 1)
        self.assertEqual(row.submission_token, fallback_token)
        self.assertEqual(len(provider.calls), 2)

    def test_retryable_alimtalk_push_retries_before_sms_fallback(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job(sms_fallback_approved=True))
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="alimtalk-reference",
                )
            ]
        )
        worker = self.worker(database, provider)
        worker.run_once()

        action = worker.handle_push_result(
            make_push_result(
                result_id="alimtalk-result",
                request_id="alimtalk-reference",
                submission_token=database.rows[
                    "message-1"
                ].job.lease_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=False,
                retryable=True,
                failure_code="temporary_delivery_failure",
                result_at=self.now,
            )
        )

        row = database.rows["message-1"]
        self.assertEqual(action, PushAction.RETRY_SCHEDULED)
        self.assertEqual(
            provider.calls,
            [DeliveryChannel.ALIMTALK],
        )
        self.assertEqual(row.status, MessageStatus.RETRY_WAIT)
        self.assertEqual(
            row.job.delivery_channel,
            DeliveryChannel.ALIMTALK,
        )
        self.assertIsNone(row.job.fallback_reason)
        self.assertEqual(
            row.next_attempt_at,
            self.now + timedelta(seconds=5),
        )

    def test_exhausted_retryable_alimtalk_push_uses_approved_sms(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(
            make_job(
                sms_fallback_approved=True,
                attempt_count=2,
            )
        )
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="alimtalk-reference",
                ),
                ProviderSubmission(
                    accepted=True,
                    request_id="sms-reference",
                ),
            ]
        )
        worker = self.worker(database, provider)
        worker.run_once()

        action = worker.handle_push_result(
            make_push_result(
                result_id="alimtalk-result",
                request_id="alimtalk-reference",
                submission_token=database.rows[
                    "message-1"
                ].job.lease_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=False,
                retryable=True,
                failure_code="retry_budget_exhausted",
                result_at=self.now,
            )
        )

        row = database.rows["message-1"]
        self.assertEqual(action, PushAction.SMS_FALLBACK)
        self.assertEqual(
            provider.calls,
            [DeliveryChannel.ALIMTALK, DeliveryChannel.SMS],
        )
        self.assertEqual(row.status, MessageStatus.WAITING_RESULT)
        self.assertEqual(
            row.job.delivery_channel,
            DeliveryChannel.SMS,
        )
        self.assertEqual(
            row.job.fallback_reason,
            "retry_budget_exhausted",
        )

    def test_unapproved_policy_never_sends_sms(self) -> None:
        database = FakeDatabase()
        database.add(make_job(sms_fallback_approved=False))
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="alimtalk-reference",
                )
            ]
        )
        worker = self.worker(database, provider)
        worker.run_once()

        action = worker.handle_push_result(
            make_push_result(
                result_id="alimtalk-result",
                request_id="alimtalk-reference",
                submission_token=database.rows[
                    "message-1"
                ].job.lease_token,
                channel=DeliveryChannel.ALIMTALK,
                delivered=False,
                retryable=False,
                failure_code="alimtalk_final_failure",
                result_at=self.now,
            )
        )

        self.assertEqual(action, PushAction.FAILED)
        self.assertEqual(
            provider.calls,
            [DeliveryChannel.ALIMTALK],
        )
        self.assertEqual(
            database.rows["message-1"].status,
            MessageStatus.FAILED,
        )

    def test_api_rejection_does_not_trigger_sms_fallback(self) -> None:
        database = FakeDatabase()
        database.add(make_job(sms_fallback_approved=True))
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=False,
                    retryable=False,
                    failure_code="api_rejected",
                )
            ]
        )

        self.worker(database, provider).run_once()

        self.assertEqual(
            provider.calls,
            [DeliveryChannel.ALIMTALK],
        )
        self.assertEqual(
            database.rows["message-1"].status,
            MessageStatus.FAILED,
        )

    def test_dedupe_key_is_submitted_once_per_claim_batch(self) -> None:
        database = FakeDatabase()
        database.add(
            make_job(
                message_id="message-a",
                dedupe_key="same-dedupe",
            )
        )
        database.add(
            make_job(
                message_id="message-b",
                dedupe_key="same-dedupe",
            )
        )
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id="provider-reference",
                )
            ]
        )

        cycle = self.worker(database, provider).run_once()

        statuses = {
            row.status for row in database.rows.values()
        }
        self.assertEqual(cycle.processed_messages, 1)
        self.assertEqual(cycle.suppressed_duplicates, 1)
        self.assertEqual(len(provider.calls), 1)
        self.assertEqual(
            statuses,
            {
                MessageStatus.WAITING_RESULT,
                MessageStatus.SUPPRESSED,
            },
        )

    def test_provider_errors_do_not_log_private_fields(self) -> None:
        stream = io.StringIO()
        logger = logging.getLogger(
            f"msg_send_privacy_test_{id(stream)}"
        )
        logger.handlers = []
        logger.propagate = False
        logger.setLevel(logging.DEBUG)
        logger.addHandler(logging.StreamHandler(stream))

        database = FakeDatabase()
        private_job = replace(
            make_job(),
            recipient="PRIVATE_RECIPIENT_SENTINEL",
            template_body="PRIVATE_TEMPLATE_PAYLOAD",
            history_url="https://service.invalid/s/PRIVATE_TOKEN_SENTINEL",
        )
        database.add(private_job)
        provider = FakeProvider(
            [
                RuntimeError(
                    "PRIVATE_PROVIDER_PAYLOAD "
                    "PRIVATE_RECIPIENT_SENTINEL "
                    "PRIVATE_TOKEN_SENTINEL"
                )
            ]
        )

        self.worker(database, provider, logger=logger).run_once()

        logs = stream.getvalue()
        self.assertNotIn("PRIVATE_PROVIDER_PAYLOAD", logs)
        self.assertNotIn("PRIVATE_RECIPIENT_SENTINEL", logs)
        self.assertNotIn("PRIVATE_TEMPLATE_PAYLOAD", logs)
        self.assertNotIn("PRIVATE_TOKEN_SENTINEL", logs)
        self.assertIn("provider_submit_failed", logs)

    def test_polling_fallback_uses_configured_interval(self) -> None:
        database = FakeDatabase()
        provider = FakeProvider([])
        worker = self.worker(database, provider)
        stopped = {"value": False}
        waits: List[float] = []

        def wait_for_wakeup(timeout_seconds: float) -> None:
            waits.append(timeout_seconds)
            stopped["value"] = True

        worker.run(
            stop_requested=lambda: stopped["value"],
            wait_for_wakeup=wait_for_wakeup,
        )

        self.assertEqual(waits, [45.0])

    def test_run_once_drains_at_most_twenty_messages_sequentially(
        self,
    ) -> None:
        database = FakeDatabase()
        for index in range(21):
            database.add(
                make_job(
                    message_id=f"message-{index}",
                    dedupe_key=f"dedupe-{index}",
                )
            )
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id=f"provider-{index}",
                )
                for index in range(20)
            ]
        )
        config = replace(
            self.config,
            max_concurrency=1,
            max_messages_per_run=20,
            max_cycle_seconds=50,
        )
        worker = MessageWorker(
            database=database,
            provider=provider,
            config=config,
            retry_policy=self.retry_policy,
            worker_id="worker-1",
            clock=lambda: self.now,
        )

        cycle = worker.run_once()

        self.assertEqual(database.claim_limits, [1] * 20)
        self.assertEqual(cycle.claimed_messages, 20)
        self.assertEqual(cycle.processed_messages, 20)
        self.assertEqual(len(provider.calls), 20)
        self.assertEqual(
            database.rows["message-20"].status,
            MessageStatus.PENDING,
        )

    def test_run_once_stops_claiming_after_cycle_start_budget(
        self,
    ) -> None:
        database = FakeDatabase()
        for index in range(3):
            database.add(
                make_job(
                    message_id=f"message-{index}",
                    dedupe_key=f"dedupe-{index}",
                )
            )
        provider = FakeProvider(
            [
                ProviderSubmission(
                    accepted=True,
                    request_id=f"provider-{index}",
                )
                for index in range(2)
            ]
        )
        elapsed = {"seconds": 0.0}

        def advance_after_submit(
            job: MessageJob,
            submission: ProviderSubmission,
        ) -> None:
            del job, submission
            elapsed["seconds"] += 26.0

        provider.before_return = advance_after_submit
        config = replace(
            self.config,
            max_concurrency=1,
            max_messages_per_run=20,
            max_cycle_seconds=50,
        )
        worker = MessageWorker(
            database=database,
            provider=provider,
            config=config,
            retry_policy=self.retry_policy,
            worker_id="worker-1",
            clock=lambda: self.now,
            monotonic_clock=lambda: elapsed["seconds"],
        )

        cycle = worker.run_once()

        self.assertEqual(database.claim_limits, [1, 1])
        self.assertEqual(cycle.claimed_messages, 2)
        self.assertEqual(cycle.processed_messages, 2)
        self.assertEqual(
            database.rows["message-2"].status,
            MessageStatus.PENDING,
        )

    def test_health_reports_provider_result_lag(self) -> None:
        database = FakeDatabase()
        database.add(make_job())
        row = database.rows["message-1"]
        row.status = MessageStatus.WAITING_RESULT
        row.waiting_since = self.now - timedelta(seconds=121)
        provider = FakeProvider([])
        worker = self.worker(database, provider)

        lagged = worker.health_snapshot()
        row.waiting_since = self.now - timedelta(seconds=20)
        healthy = worker.health_snapshot()

        self.assertFalse(lagged.healthy)
        self.assertEqual(lagged.provider_result_lag_seconds, 121.0)
        self.assertTrue(healthy.healthy)
        self.assertEqual(healthy.provider_result_lag_seconds, 20.0)

    def test_health_includes_unregistered_submission_ambiguity_lag(
        self,
    ) -> None:
        database = FakeDatabase()
        database.add(make_job(), status=MessageStatus.ACCEPTED)
        row = database.rows["message-1"]
        row.submission_started_at = self.now - timedelta(
            seconds=121
        )
        row.submission_token = row.job.lease_token
        provider = FakeProvider([])

        health = self.worker(database, provider).health_snapshot()

        self.assertFalse(health.healthy)
        self.assertEqual(
            health.provider_result_lag_seconds,
            121.0,
        )


if __name__ == "__main__":
    unittest.main()
