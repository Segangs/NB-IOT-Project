from __future__ import annotations

from datetime import datetime, timezone
from typing import Mapping

from .config import TariffConfig
from .correlation import refkey_to_lease_uuid
from .domain import (
    DeliveryChannel,
    ProviderPushResult,
)


class CallbackContractError(ValueError):
    """Raised when a Bizppurio PUSH payload cannot be trusted."""


_CHANNELS = {
    "AT": DeliveryChannel.ALIMTALK,
    "SMS": DeliveryChannel.SMS,
}
_SUCCESS_CODES = {
    DeliveryChannel.ALIMTALK: "7000",
    DeliveryChannel.SMS: "4100",
}
_REQUIRED_KEYS = frozenset(
    {
        "CMSGID",
        "REFKEY",
        "MSGID",
        "UNIXTIME",
        "RESULT",
        "MEDIA",
    }
)


def parse_push_result(
    payload: Mapping[str, object],
    tariffs: TariffConfig,
) -> ProviderPushResult:
    missing = _REQUIRED_KEYS - set(payload)
    if missing:
        raise CallbackContractError("missing uppercase PUSH fields")
    media = payload["MEDIA"]
    if not isinstance(media, str) or media not in _CHANNELS:
        raise CallbackContractError("unsupported PUSH MEDIA")
    channel = _CHANNELS[media]
    request_id = _identity(payload["CMSGID"], "CMSGID")
    result_id = _identity(payload["MSGID"], "MSGID")
    result_code = _result_code(payload["RESULT"])
    result_at = _result_time(payload["UNIXTIME"])
    refkey = payload["REFKEY"]
    if not isinstance(refkey, str):
        raise CallbackContractError("invalid PUSH REFKEY")
    try:
        submission_token = refkey_to_lease_uuid(refkey)
    except ValueError as exc:
        raise CallbackContractError("invalid PUSH REFKEY") from exc
    delivered = result_code == _SUCCESS_CODES[channel]
    return ProviderPushResult(
        result_id=result_id,
        request_id=request_id,
        submission_token=submission_token,
        channel=channel,
        delivered=delivered,
        retryable=False,
        failure_code=None if delivered else result_code,
        provider_result_code=result_code,
        result_at=result_at,
        cost_amount=(
            tariffs.alimtalk
            if channel is DeliveryChannel.ALIMTALK
            else tariffs.sms
        ),
    )


def _identity(value: object, field_name: str) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= 256:
        raise CallbackContractError(f"invalid PUSH {field_name}")
    return value


def _result_code(value: object) -> str:
    if isinstance(value, bool):
        raise CallbackContractError("invalid PUSH RESULT")
    if isinstance(value, int):
        rendered = str(value)
    elif isinstance(value, str):
        rendered = value
    else:
        raise CallbackContractError("invalid PUSH RESULT")
    if not rendered.isdigit() or not 1 <= len(rendered) <= 16:
        raise CallbackContractError("invalid PUSH RESULT")
    return rendered


def _result_time(value: object) -> datetime:
    if isinstance(value, bool):
        raise CallbackContractError("invalid PUSH UNIXTIME")
    try:
        timestamp = int(value)
    except (TypeError, ValueError) as exc:
        raise CallbackContractError("invalid PUSH UNIXTIME") from exc
    if timestamp < 1 or timestamp > 4102444800:
        raise CallbackContractError("invalid PUSH UNIXTIME")
    return datetime.fromtimestamp(timestamp, tz=timezone.utc)
