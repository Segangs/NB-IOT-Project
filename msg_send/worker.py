from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime, timedelta
from decimal import Decimal
from threading import Lock
import time
from typing import Callable, Optional, Protocol, Sequence

from .domain import (
    DeliveryChannel,
    MessageJob,
    ProviderPushResult,
    ProviderResultMode,
    ProviderSubmission,
    ProviderSubmissionNotAttempted,
    ProviderSubmissionOutcomeUnknown,
    PushAction,
    PushApplication,
    RetryPolicy,
    SubmissionRegistration,
    WorkerConfig,
)


class MessageDatabase(Protocol):
    def recover_expired_leases(
        self,
        now: datetime,
        limit: int,
        max_attempts: int,
        base_delay_seconds: int,
        max_delay_seconds: int,
        waiting_result_timeout_seconds: int,
    ) -> int:
        """Recover expired queue, lease, and provider-result waits."""
        ...

    def claim_messages(
        self,
        worker_id: str,
        now: datetime,
        lease_seconds: int,
        limit: int,
    ) -> Sequence[MessageJob]:
        ...

    def suppress_message(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        ...

    def suppress_duplicate(
        self,
        job: MessageJob,
        now: datetime,
    ) -> None:
        ...

    def mark_submission_started(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> None:
        """Durably fence an attempt using the database clock."""
        ...

    def mark_submission_waiting_result(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
    ) -> SubmissionRegistration:
        ...

    def mark_submission_accepted_success(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
        cost_amount: Decimal,
    ) -> SubmissionRegistration:
        """Atomically register provider acceptance and finish the message."""
        ...

    def schedule_retry(
        self,
        job: MessageJob,
        next_attempt_at: datetime,
        reason_code: str,
        now: datetime,
    ) -> None:
        ...

    def mark_failed(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        ...

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
        ...

    def reject_sms_fallback(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        ...

    def oldest_waiting_result_at(self) -> Optional[datetime]:
        """Return the oldest result wait or unregistered submission."""
        ...


class MessageProvider(Protocol):
    def submit(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> ProviderSubmission:
        ...


@dataclass(frozen=True)
class WorkerCycleResult:
    success: bool
    recovered_leases: int
    claimed_messages: int
    processed_messages: int
    suppressed_duplicates: int


@dataclass(frozen=True)
class WorkerHealth:
    healthy: bool
    provider_result_lag_seconds: float
    consecutive_cycle_failures: int
    last_cycle_at: Optional[datetime]


class MessageWorker:
    def __init__(
        self,
        database: MessageDatabase,
        provider: MessageProvider,
        config: WorkerConfig,
        retry_policy: RetryPolicy,
        worker_id: str,
        clock: Callable[[], datetime],
        logger: Optional[logging.Logger] = None,
        sms_fallback_enabled: bool = True,
        provider_result_mode: ProviderResultMode = (
            ProviderResultMode.CALLBACK
        ),
        provider_acceptance_cost: Decimal = Decimal("0.0000"),
        before_claim: Optional[Callable[[], object]] = None,
        monotonic_clock: Callable[[], float] = time.monotonic,
    ) -> None:
        if not worker_id:
            raise ValueError("worker_id must not be empty")
        if (
            not isinstance(provider_result_mode, ProviderResultMode)
            or not isinstance(provider_acceptance_cost, Decimal)
            or not provider_acceptance_cost.is_finite()
            or provider_acceptance_cost < 0
        ):
            raise ValueError("invalid provider acceptance configuration")
        self._database = database
        self._provider = provider
        self._config = config
        self._retry_policy = retry_policy
        self._worker_id = worker_id
        self._clock = clock
        self._logger = logger or logging.getLogger(__name__)
        self._sms_fallback_enabled = sms_fallback_enabled
        self._provider_result_mode = provider_result_mode
        self._provider_acceptance_cost = provider_acceptance_cost
        self._before_claim = before_claim
        self._monotonic_clock = monotonic_clock
        self._health_lock = Lock()
        self._consecutive_cycle_failures = 0
        self._last_cycle_at: Optional[datetime] = None

    def run_once(
        self,
        now: Optional[datetime] = None,
    ) -> WorkerCycleResult:
        cycle_now = now or self._clock()
        cycle_started = self._monotonic_clock()
        try:
            recovered = self._database.recover_expired_leases(
                now=cycle_now,
                limit=self._config.recovery_batch_size,
                max_attempts=self._retry_policy.max_attempts,
                base_delay_seconds=(
                    self._retry_policy.base_delay_seconds
                ),
                max_delay_seconds=(
                    self._retry_policy.max_delay_seconds
                ),
                waiting_result_timeout_seconds=(
                    self._config.result_timeout_seconds
                ),
            )
            if self._before_claim is not None:
                self._before_claim()
        except Exception:
            self._record_cycle(False, cycle_now)
            self._logger.error(
                "message_worker_cycle_failed worker_id=%s stage=claim",
                self._worker_id,
            )
            return WorkerCycleResult(False, 0, 0, 0, 0)

        seen_dedupe_keys = set()
        claimed_messages = 0
        processed_messages = 0
        suppressed_duplicates = 0
        processing_success = True

        for _ in range(self._config.max_messages_per_run):
            if (
                self._monotonic_clock() - cycle_started
                >= self._config.max_cycle_seconds
            ):
                break
            try:
                claimed = tuple(
                    self._database.claim_messages(
                        worker_id=self._worker_id,
                        now=cycle_now,
                        lease_seconds=self._config.lease_seconds,
                        limit=1,
                    )
                )
            except Exception:
                self._record_cycle(False, cycle_now)
                self._logger.error(
                    "message_worker_cycle_failed "
                    "worker_id=%s stage=claim",
                    self._worker_id,
                )
                return WorkerCycleResult(
                    False,
                    recovered,
                    claimed_messages,
                    processed_messages,
                    suppressed_duplicates,
                )

            if not claimed:
                break

            claimed_messages += len(claimed)
            try:
                job = claimed[0]
                if job.dedupe_key in seen_dedupe_keys:
                    self._database.suppress_duplicate(job, cycle_now)
                    suppressed_duplicates += 1
                    continue
                seen_dedupe_keys.add(job.dedupe_key)
            except Exception:
                self._record_cycle(False, cycle_now)
                self._logger.error(
                    "message_worker_cycle_failed "
                    "worker_id=%s stage=dedupe",
                    self._worker_id,
                )
                return WorkerCycleResult(
                    False,
                    recovered,
                    claimed_messages,
                    processed_messages,
                    suppressed_duplicates,
                )

            processed_messages += 1
            processing_success = (
                self._process_job_safely(job, cycle_now)
                and processing_success
            )

        self._record_cycle(processing_success, cycle_now)
        return WorkerCycleResult(
            processing_success,
            recovered,
            claimed_messages,
            processed_messages,
            suppressed_duplicates,
        )

    def handle_push_result(
        self,
        result: ProviderPushResult,
        now: Optional[datetime] = None,
        *,
        allow_fallback_submission: bool = True,
        propagate_apply_error: bool = False,
    ) -> PushAction:
        result_now = now or self._clock()
        try:
            application = self._database.apply_push_result(
                result=result,
                worker_id=self._worker_id,
                now=result_now,
                lease_seconds=self._config.lease_seconds,
                max_attempts=self._retry_policy.max_attempts,
                base_delay_seconds=(
                    self._retry_policy.base_delay_seconds
                ),
                max_delay_seconds=(
                    self._retry_policy.max_delay_seconds
                ),
            )
        except Exception:
            self._logger.error(
                "push_result_apply_failed worker_id=%s",
                self._worker_id,
            )
            if propagate_apply_error:
                raise
            return PushAction.UNKNOWN

        if application.action is not PushAction.SMS_FALLBACK:
            return application.action

        fallback_job = application.message
        if (
            not self._sms_fallback_enabled
            or not allow_fallback_submission
        ):
            if fallback_job is not None:
                try:
                    self._database.reject_sms_fallback(
                        fallback_job,
                        (
                            "sms_fallback_disabled"
                            if not self._sms_fallback_enabled
                            else "sms_fallback_submission_disabled"
                        ),
                        result_now,
                    )
                except Exception:
                    self._logger.error(
                        "sms_fallback_reject_failed worker_id=%s",
                        self._worker_id,
                    )
                    if propagate_apply_error:
                        raise
            return PushAction.FAILED
        if (
            fallback_job is None
            or result.channel is not DeliveryChannel.ALIMTALK
            or result.delivered
            or fallback_job.delivery_channel
            is not DeliveryChannel.SMS
            or not fallback_job.fallback_reason
            or not fallback_job.policy.allows_sms_fallback()
        ):
            if fallback_job is not None:
                try:
                    self._database.reject_sms_fallback(
                        fallback_job,
                        "sms_fallback_not_approved",
                        result_now,
                    )
                except Exception:
                    self._logger.error(
                        "sms_fallback_reject_failed worker_id=%s",
                        self._worker_id,
                    )
            return PushAction.FAILED

        self._process_job_safely(fallback_job, result_now)
        return PushAction.SMS_FALLBACK

    def process_claimed_once(
        self,
        job: MessageJob,
        now: Optional[datetime] = None,
    ) -> bool:
        """Process one already-claimed, caller-prechecked message."""
        return self._process_job_safely(job, now or self._clock())

    def run(
        self,
        stop_requested: Callable[[], bool],
        wait_for_wakeup: Callable[[float], None],
    ) -> None:
        while not stop_requested():
            self.run_once()
            if stop_requested():
                break
            wait_for_wakeup(
                float(self._config.poll_interval_seconds)
            )

    def health_snapshot(
        self,
        now: Optional[datetime] = None,
    ) -> WorkerHealth:
        health_now = now or self._clock()
        try:
            oldest_waiting = (
                self._database.oldest_waiting_result_at()
            )
        except Exception:
            self._logger.error(
                "message_worker_health_failed worker_id=%s",
                self._worker_id,
            )
            with self._health_lock:
                return WorkerHealth(
                    healthy=False,
                    provider_result_lag_seconds=0.0,
                    consecutive_cycle_failures=(
                        self._consecutive_cycle_failures + 1
                    ),
                    last_cycle_at=self._last_cycle_at,
                )

        lag_seconds = 0.0
        if oldest_waiting is not None:
            lag_seconds = max(
                0.0,
                (health_now - oldest_waiting).total_seconds(),
            )
        with self._health_lock:
            failures = self._consecutive_cycle_failures
            last_cycle_at = self._last_cycle_at
        return WorkerHealth(
            healthy=(
                failures == 0
                and lag_seconds
                <= self._config.result_lag_threshold_seconds
            ),
            provider_result_lag_seconds=lag_seconds,
            consecutive_cycle_failures=failures,
            last_cycle_at=last_cycle_at,
        )

    def _process_job_safely(
        self,
        job: MessageJob,
        now: datetime,
    ) -> bool:
        try:
            self._process_job(job, now)
            return True
        except Exception:
            self._logger.error(
                "message_worker_job_failed message_id=%s",
                job.message_id,
            )
            return False

    def _process_job(
        self,
        job: MessageJob,
        now: datetime,
    ) -> None:
        if (
            job.delivery_channel is DeliveryChannel.SMS
            and not self._sms_fallback_enabled
        ):
            self._database.reject_sms_fallback(
                job,
                "sms_fallback_disabled",
                now,
            )
            return
        if not job.policy.allows_primary():
            self._database.suppress_message(
                job,
                "consent_not_granted",
                now,
            )
            return
        if (
            job.delivery_channel is DeliveryChannel.SMS
            and (
                not job.fallback_reason
                or not job.policy.allows_sms_fallback()
            )
        ):
            self._database.reject_sms_fallback(
                job,
                "sms_fallback_not_approved",
                now,
            )
            return

        self._database.mark_submission_started(
            job,
            job.delivery_channel,
        )

        try:
            submission = self._provider.submit(
                job,
                job.delivery_channel,
            )
        except ProviderSubmissionNotAttempted as exc:
            self._logger.warning(
                "provider_submit_failed outcome=not_attempted "
                "message_id=%s channel=%s",
                job.message_id,
                job.delivery_channel.value,
            )
            self._retry_or_fail(
                job,
                now,
                exc.failure_code,
            )
            return
        except ProviderSubmissionOutcomeUnknown:
            self._logger.warning(
                "provider_submit_failed outcome=ambiguous "
                "message_id=%s channel=%s",
                job.message_id,
                job.delivery_channel.value,
            )
            return
        except Exception:
            self._logger.warning(
                "provider_submit_failed outcome=ambiguous "
                "message_id=%s channel=%s",
                job.message_id,
                job.delivery_channel.value,
            )
            return

        if submission.accepted:
            provider_request_id = submission.request_id
            if provider_request_id is None:
                self._database.mark_failed(
                    job,
                    "invalid_provider_response",
                    now,
                )
                return
            if (
                self._provider_result_mode
                is ProviderResultMode.PROVIDER_ACCEPTANCE
                and job.delivery_channel is DeliveryChannel.ALIMTALK
            ):
                self._database.mark_submission_accepted_success(
                    job,
                    provider_request_id,
                    job.delivery_channel,
                    now,
                    self._provider_acceptance_cost,
                )
            else:
                self._database.mark_submission_waiting_result(
                    job,
                    provider_request_id,
                    job.delivery_channel,
                    now,
                )
            return

        if submission.retryable:
            self._retry_or_fail(
                job,
                now,
                "provider_submit_retryable",
            )
            return
        self._database.mark_failed(
            job,
            "provider_submit_rejected",
            now,
        )

    def _retry_or_fail(
        self,
        job: MessageJob,
        now: datetime,
        reason_code: str,
    ) -> None:
        try:
            delay = self._retry_policy.next_delay_seconds(
                job.attempt_count
            )
        except ValueError:
            delay = None
        if delay is None:
            self._database.mark_failed(
                job,
                "attempts_exhausted",
                now,
            )
            return
        self._database.schedule_retry(
            job,
            now + timedelta(seconds=delay),
            reason_code,
            now,
        )

    def _record_cycle(
        self,
        success: bool,
        now: datetime,
    ) -> None:
        with self._health_lock:
            self._last_cycle_at = now
            if success:
                self._consecutive_cycle_failures = 0
            else:
                self._consecutive_cycle_failures += 1
