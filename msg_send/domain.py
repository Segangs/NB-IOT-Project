from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from decimal import Decimal
from enum import Enum
from typing import Dict, FrozenSet, Mapping, Optional


class MessageStatus(str, Enum):
    PENDING = "pending"
    PROCESSING = "processing"
    ACCEPTED = "accepted"
    WAITING_RESULT = "waiting_result"
    RETRY_WAIT = "retry_wait"
    SENT = "sent"
    FAILED = "failed"
    SUPPRESSED = "suppressed"
    CANCELLED = "cancelled"


class DeliveryChannel(str, Enum):
    ALIMTALK = "alimtalk"
    SMS = "sms"


class ProviderResultMode(str, Enum):
    CALLBACK = "callback"
    PROVIDER_ACCEPTANCE = "provider_acceptance"


class PushAction(str, Enum):
    DUPLICATE = "duplicate"
    DELIVERED = "delivered"
    RETRY_SCHEDULED = "retry_scheduled"
    SMS_FALLBACK = "sms_fallback"
    FAILED = "failed"
    UNKNOWN = "unknown"


class InvalidStatusTransition(ValueError):
    """Raised when a message attempts a non-canonical state transition."""


class ProviderSubmissionNotAttempted(RuntimeError):
    """Raised when no provider message transport call was attempted."""

    failure_code = "provider_submit_not_attempted"

    def __init__(self) -> None:
        super().__init__(self.failure_code)


class ProviderSubmissionOutcomeUnknown(RuntimeError):
    """Raised when a provider transport call may have accepted a message."""

    failure_code = "provider_submit_outcome_unknown"

    def __init__(self) -> None:
        super().__init__(self.failure_code)


_ALLOWED_TRANSITIONS: Dict[MessageStatus, FrozenSet[MessageStatus]] = {
    MessageStatus.PENDING: frozenset(
        {
            MessageStatus.PROCESSING,
            MessageStatus.FAILED,
            MessageStatus.SUPPRESSED,
            MessageStatus.CANCELLED,
        }
    ),
    MessageStatus.PROCESSING: frozenset(
        {
            MessageStatus.ACCEPTED,
            MessageStatus.RETRY_WAIT,
            MessageStatus.FAILED,
            MessageStatus.SUPPRESSED,
            MessageStatus.CANCELLED,
        }
    ),
    MessageStatus.ACCEPTED: frozenset(
        {
            MessageStatus.WAITING_RESULT,
            MessageStatus.RETRY_WAIT,
            MessageStatus.FAILED,
            MessageStatus.CANCELLED,
        }
    ),
    MessageStatus.WAITING_RESULT: frozenset(
        {
            MessageStatus.SENT,
            MessageStatus.RETRY_WAIT,
            MessageStatus.FAILED,
            MessageStatus.CANCELLED,
        }
    ),
    MessageStatus.RETRY_WAIT: frozenset(
        {
            MessageStatus.PROCESSING,
            MessageStatus.FAILED,
            MessageStatus.CANCELLED,
        }
    ),
    MessageStatus.SENT: frozenset(),
    MessageStatus.FAILED: frozenset(),
    MessageStatus.SUPPRESSED: frozenset(),
    MessageStatus.CANCELLED: frozenset(),
}


def transition_status(
    current: MessageStatus,
    target: MessageStatus,
) -> MessageStatus:
    if target not in _ALLOWED_TRANSITIONS[current]:
        raise InvalidStatusTransition(
            "illegal message status transition: "
            f"{current.value} -> {target.value}"
        )
    return target


def render_history_links(template: str, final_url: str) -> str:
    if not final_url:
        raise ValueError("final_url must not be empty")
    return template.replace(
        "#{tempHistoryToken)}",
        final_url,
    ).replace(
        "#{tempHistoryToken}",
        final_url,
    )


@dataclass(frozen=True)
class RetryPolicy:
    max_attempts: int
    base_delay_seconds: int
    max_delay_seconds: int

    def __post_init__(self) -> None:
        if self.max_attempts < 1:
            raise ValueError("max_attempts must be positive")
        if self.base_delay_seconds < 1:
            raise ValueError("base_delay_seconds must be positive")
        if self.max_delay_seconds < self.base_delay_seconds:
            raise ValueError(
                "max_delay_seconds must be at least base_delay_seconds"
            )

    def next_delay_seconds(
        self,
        completed_attempts: int,
    ) -> Optional[int]:
        if completed_attempts < 1:
            raise ValueError("completed_attempts must be positive")
        if completed_attempts >= self.max_attempts:
            return None
        delay = self.base_delay_seconds * (2 ** (completed_attempts - 1))
        return min(delay, self.max_delay_seconds)


@dataclass(frozen=True)
class WorkerConfig:
    poll_interval_seconds: int = 45
    max_concurrency: int = 4
    max_messages_per_run: int = 20
    max_cycle_seconds: int = 50
    lease_seconds: int = 120
    recovery_batch_size: int = 64
    result_lag_threshold_seconds: int = 120
    result_timeout_seconds: int = 3600

    def __post_init__(self) -> None:
        if not 30 <= self.poll_interval_seconds <= 60:
            raise ValueError(
                "poll_interval_seconds must be between 30 and 60"
            )
        if not 1 <= self.max_concurrency <= 16:
            raise ValueError("max_concurrency must be between 1 and 16")
        if not 1 <= self.max_messages_per_run <= 20:
            raise ValueError(
                "max_messages_per_run must be between 1 and 20"
            )
        if not 5 <= self.max_cycle_seconds <= 55:
            raise ValueError(
                "max_cycle_seconds must be between 5 and 55"
            )
        if not 30 <= self.lease_seconds <= 300:
            raise ValueError("lease_seconds must be between 30 and 300")
        if not 1 <= self.recovery_batch_size <= 1000:
            raise ValueError(
                "recovery_batch_size must be between 1 and 1000"
            )
        if not 1 <= self.result_lag_threshold_seconds <= 86400:
            raise ValueError(
                "result_lag_threshold_seconds must be between 1 and 86400"
            )
        if not 60 <= self.result_timeout_seconds <= 86400:
            raise ValueError(
                "result_timeout_seconds must be between 60 and 86400"
            )


@dataclass(frozen=True)
class MessagePolicy:
    consent_granted: bool
    sms_fallback_approved: bool
    sms_cost_class: Optional[str] = None

    def allows_primary(self) -> bool:
        return self.consent_granted

    def allows_sms_fallback(self) -> bool:
        return (
            self.consent_granted
            and self.sms_fallback_approved
            and bool(self.sms_cost_class)
        )


@dataclass(frozen=True)
class ProviderSubmission:
    accepted: bool
    request_id: Optional[str] = None
    retryable: bool = False
    failure_code: Optional[str] = None

    def __post_init__(self) -> None:
        if self.accepted and not self.request_id:
            raise ValueError(
                "accepted provider submission requires request_id"
            )


@dataclass(frozen=True)
class MessageJob:
    message_id: str
    lease_token: str
    dedupe_key: str
    recipient: str = field(repr=False)
    template_code: str
    template_body: str = field(repr=False)
    history_url: str = field(repr=False)
    sms_body: str = field(repr=False)
    attempt_count: int
    policy: MessagePolicy
    delivery_channel: DeliveryChannel
    fallback_reason: Optional[str]
    settings_url: str = field(default="", repr=False)
    template_params: Mapping[str, object] = field(
        default_factory=dict,
        repr=False,
    )
    max_attempts: int = 1
    lock_owner: str = ""

    def __post_init__(self) -> None:
        if self.attempt_count < 0:
            raise ValueError("attempt_count must not be negative")
        if self.max_attempts < 1:
            raise ValueError("max_attempts must be positive")
        if (
            self.delivery_channel is DeliveryChannel.SMS
            and not self.fallback_reason
        ):
            raise ValueError(
                "SMS job requires an Alimtalk fallback reason"
            )


@dataclass(frozen=True)
class ProviderPushResult:
    result_id: str
    request_id: str
    submission_token: str
    channel: DeliveryChannel
    delivered: bool
    result_at: datetime
    cost_amount: Decimal
    retryable: bool = False
    failure_code: Optional[str] = None
    provider_result_code: Optional[str] = None

    def __post_init__(self) -> None:
        if not self.result_id:
            raise ValueError("result_id must not be empty")
        if not self.request_id:
            raise ValueError("request_id must not be empty")
        if not self.submission_token:
            raise ValueError("submission_token must not be empty")
        if (
            self.result_at.tzinfo is None
            or self.result_at.utcoffset() is None
        ):
            raise ValueError("result_at must be timezone-aware")
        if (
            not isinstance(self.cost_amount, Decimal)
            or not self.cost_amount.is_finite()
            or self.cost_amount < Decimal("0")
            or self.cost_amount > Decimal("9999999999.9999")
            or self.cost_amount
            != self.cost_amount.quantize(Decimal("0.0001"))
        ):
            raise ValueError(
                "cost_amount must fit canonical numeric(14,4)"
            )


@dataclass(frozen=True)
class SubmissionRegistration:
    applied: bool
    duplicate: bool
    current_status: MessageStatus

    def __post_init__(self) -> None:
        if self.applied == self.duplicate:
            raise ValueError(
                "submission registration must be applied or duplicate"
            )


@dataclass(frozen=True)
class PushApplication:
    action: PushAction
    message: Optional[MessageJob] = None

    def __post_init__(self) -> None:
        if (
            self.action is PushAction.SMS_FALLBACK
            and self.message is None
        ):
            raise ValueError(
                "SMS fallback action requires claimed message"
            )
