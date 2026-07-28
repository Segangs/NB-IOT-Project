from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from threading import Lock
import re
from typing import Callable, Mapping, Protocol, Sequence

from .bizppurio import BizppurioAdapter
from .bizppurio_http import BizppurioHttpTransport
from .callback import parse_push_result
from .catalog import TemplateCatalog
from .config import RuntimeConfig
from .domain import (
    DeliveryChannel,
    MessageJob,
    ProviderResultMode,
    ProviderSubmission,
    ProviderSubmissionNotAttempted,
    PushAction,
    RetryPolicy,
)
from .supabase_rest import SupabaseRestDatabase
from .worker import (
    MessageDatabase,
    MessageProvider,
    MessageWorker,
    WorkerCycleResult,
    WorkerHealth,
)


class RuntimeSafetyError(RuntimeError):
    """Raised when a production execution gate cannot be opened safely."""


class OneShotDatabase(MessageDatabase, Protocol):
    def claim_exact_one_shot(
        self,
        expected_message_id: str,
        worker_id: str,
        now: datetime,
        lease_seconds: int,
    ) -> Sequence[MessageJob]:
        ...

    def is_callback_missing(self, provider_request_id: str) -> bool:
        ...


@dataclass(frozen=True)
class RuntimeRunResult:
    executed: bool
    success: bool
    reason: str
    cycle: WorkerCycleResult | None = None


@dataclass(frozen=True)
class RuntimeHealth:
    healthy: bool
    claim_enabled: bool
    send_enabled: bool
    worker: WorkerHealth


class RuntimeGates:
    def __init__(
        self,
        *,
        claim_enabled: bool,
        send_enabled: bool,
    ) -> None:
        self._lock = Lock()
        self._claim_enabled = claim_enabled
        self._send_enabled = send_enabled

    @property
    def claim_enabled(self) -> bool:
        with self._lock:
            return self._claim_enabled

    @property
    def send_enabled(self) -> bool:
        with self._lock:
            return self._send_enabled

    def unlock_one_shot(self) -> None:
        with self._lock:
            if self._claim_enabled or self._send_enabled:
                raise RuntimeSafetyError(
                    "one-shot requires both gates to start locked"
                )
            self._claim_enabled = True
            self._send_enabled = True

    def relock(self) -> None:
        with self._lock:
            self._claim_enabled = False
            self._send_enabled = False


class _GatedProvider:
    def __init__(
        self,
        provider: MessageProvider,
        gates: RuntimeGates,
    ) -> None:
        self._provider = provider
        self._gates = gates

    def submit(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> ProviderSubmission:
        if not self._gates.send_enabled:
            raise ProviderSubmissionNotAttempted()
        return self._provider.submit(job, channel)


class ProductionRuntime:
    def __init__(
        self,
        *,
        config: RuntimeConfig,
        database: OneShotDatabase,
        provider: MessageProvider,
        clock: Callable[[], datetime],
    ) -> None:
        if config.claim_enabled and not config.send_enabled:
            raise RuntimeSafetyError(
                "claim cannot be enabled while send is disabled"
            )
        if config.worker.max_concurrency != 1:
            raise RuntimeSafetyError("production concurrency must equal 1")
        self.config = config
        self.database = database
        self.gates = RuntimeGates(
            claim_enabled=config.claim_enabled,
            send_enabled=config.send_enabled,
        )
        self._clock = clock
        self._report_provider = provider
        gated_provider = _GatedProvider(provider, self.gates)
        self._worker = MessageWorker(
            database=database,
            provider=gated_provider,
            config=config.worker,
            retry_policy=RetryPolicy(
                max_attempts=5,
                base_delay_seconds=30,
                max_delay_seconds=3600,
            ),
            worker_id=config.worker_id,
            clock=clock,
            sms_fallback_enabled=config.sms_fallback_enabled,
            provider_result_mode=config.provider_result_mode,
            provider_acceptance_cost=config.tariffs.alimtalk,
            before_claim=lambda: database.drain_temperature_alert_outbox(16),
        )
        self._one_shot_worker = MessageWorker(
            database=database,
            provider=gated_provider,
            config=config.worker,
            retry_policy=RetryPolicy(
                max_attempts=1,
                base_delay_seconds=30,
                max_delay_seconds=30,
            ),
            worker_id=config.worker_id,
            clock=clock,
            sms_fallback_enabled=config.sms_fallback_enabled,
            provider_result_mode=config.provider_result_mode,
            provider_acceptance_cost=config.tariffs.alimtalk,
        )

    def health(self) -> RuntimeHealth:
        worker_health = self._worker.health_snapshot()
        return RuntimeHealth(
            healthy=worker_health.healthy,
            claim_enabled=self.gates.claim_enabled,
            send_enabled=self.gates.send_enabled,
            worker=worker_health,
        )

    def run_once(self) -> RuntimeRunResult:
        if not self.gates.claim_enabled:
            return RuntimeRunResult(
                executed=False,
                success=True,
                reason="claim_disabled",
            )
        if not self.gates.send_enabled:
            raise RuntimeSafetyError("send gate is disabled")
        cycle = self._worker.run_once()
        return RuntimeRunResult(
            executed=True,
            success=cycle.success,
            reason="completed",
            cycle=cycle,
        )

    def one_shot(
        self,
        expected_message_id: str,
    ) -> RuntimeRunResult:
        if not expected_message_id:
            raise RuntimeSafetyError(
                "one-shot expected message id is required"
            )
        if self.config.sms_fallback_enabled:
            raise RuntimeSafetyError(
                "one-shot requires SMS fallback disabled"
            )
        if self.gates.claim_enabled or self.gates.send_enabled:
            raise RuntimeSafetyError(
                "one-shot requires both gates locked"
            )
        self.gates.unlock_one_shot()
        try:
            claimed = tuple(
                self.database.claim_exact_one_shot(
                    expected_message_id=expected_message_id,
                    worker_id=self.config.worker_id,
                    now=self._clock(),
                    lease_seconds=self.config.worker.lease_seconds,
                )
            )
            if not claimed:
                return RuntimeRunResult(
                    executed=False,
                    success=True,
                    reason="queue_empty",
                )
            if len(claimed) != 1:
                raise RuntimeSafetyError(
                    "one-shot exact claim returned invalid cardinality"
                )
            job = claimed[0]
            if job.message_id != expected_message_id:
                raise RuntimeSafetyError(
                    "one-shot claim did not return the expected row"
                )
            if (
                job.attempt_count != 1
                or job.max_attempts != 1
                or job.delivery_channel
                    is not DeliveryChannel.ALIMTALK
                or job.policy.sms_fallback_approved
            ):
                raise RuntimeSafetyError(
                    "one-shot claimed row violated safety policy"
                )
            success = self._one_shot_worker.process_claimed_once(
                job,
                self._clock(),
            )
            return RuntimeRunResult(
                executed=True,
                success=success,
                reason="completed" if success else "processing_failed",
            )
        finally:
            self.gates.relock()

    def handle_callback_payload(
        self,
        payload: Mapping[str, object],
    ) -> PushAction:
        result = parse_push_result(payload, self.config.tariffs)
        if (
            self.config.provider_result_mode
            is ProviderResultMode.PROVIDER_ACCEPTANCE
            and result.channel is DeliveryChannel.ALIMTALK
        ):
            return PushAction.DUPLICATE
        return self._worker.handle_push_result(result)

    def handle_callback_payload_non_sending(
        self,
        payload: Mapping[str, object],
    ) -> PushAction:
        result = parse_push_result(payload, self.config.tariffs)
        if (
            self.config.provider_result_mode
            is ProviderResultMode.PROVIDER_ACCEPTANCE
            and result.channel is DeliveryChannel.ALIMTALK
        ):
            return PushAction.DUPLICATE
        return self._worker.handle_push_result(
            result,
            allow_fallback_submission=False,
            propagate_apply_error=True,
        )

    def reconcile_callback(
        self,
        provider_request_id: str,
    ) -> RuntimeRunResult:
        if (
            not isinstance(provider_request_id, str)
            or not re.fullmatch(
                r"[A-Za-z0-9._:-]{1,256}",
                provider_request_id,
            )
        ):
            raise RuntimeSafetyError(
                "provider request id is invalid"
            )
        try:
            callback_missing = self.database.is_callback_missing(
                provider_request_id
            )
        except Exception:
            return RuntimeRunResult(
                executed=False,
                success=False,
                reason="callback_lookup_failed",
            )
        if not callback_missing:
            return RuntimeRunResult(
                executed=False,
                success=True,
                reason="callback_not_missing",
            )
        request_report = getattr(
            self._report_provider,
            "request_report",
            None,
        )
        if not callable(request_report):
            return RuntimeRunResult(
                executed=False,
                success=False,
                reason="report_unavailable",
            )
        try:
            request_report(provider_request_id)
        except Exception:
            return RuntimeRunResult(
                executed=True,
                success=False,
                reason="report_request_failed",
            )
        return RuntimeRunResult(
            executed=True,
            success=True,
            reason="report_requested",
        )


def build_runtime(config: RuntimeConfig) -> ProductionRuntime:
    catalog = TemplateCatalog.load_approved()
    database = SupabaseRestDatabase(config.supabase, catalog)
    provider = BizppurioAdapter(
        BizppurioHttpTransport(config.bizppurio),
        clock=lambda: datetime.now(timezone.utc),
        catalog=catalog,
        resubmit_after_auth_expiry=False,
    )
    return ProductionRuntime(
        config=config,
        database=database,
        provider=provider,
        clock=lambda: datetime.now(timezone.utc),
    )
