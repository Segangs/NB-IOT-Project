from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal, InvalidOperation
import os
import re
from typing import Mapping, Optional
from urllib.parse import urlsplit

from .domain import ProviderResultMode, WorkerConfig


class ConfigError(ValueError):
    """Raised when production configuration is unsafe or incomplete."""


@dataclass(frozen=True)
class TariffConfig:
    alimtalk: Decimal
    sms: Decimal

    def __post_init__(self) -> None:
        _validate_tariff(self.alimtalk, "BIZPPURIO_AT_COST_KRW")
        _validate_tariff(self.sms, "BIZPPURIO_SMS_COST_KRW")


@dataclass(frozen=True)
class SupabaseConfig:
    url: str
    secret_key: str = field(repr=False)
    timeout_seconds: float = 10.0


@dataclass(frozen=True)
class BizppurioConfig:
    base_url: str
    account: str
    password: str = field(repr=False)
    from_number: str = field(repr=False)
    sender_key: str
    timeout_seconds: float = 10.0


@dataclass(frozen=True)
class RuntimeConfig:
    supabase: SupabaseConfig
    bizppurio: BizppurioConfig
    tariffs: TariffConfig
    worker: WorkerConfig
    worker_id: str
    claim_enabled: bool = False
    send_enabled: bool = False
    sms_fallback_enabled: bool = False
    callback_apply_enabled: bool = False
    provider_result_mode: ProviderResultMode = ProviderResultMode.CALLBACK


def load_runtime_config(
    environ: Optional[Mapping[str, str]] = None,
) -> RuntimeConfig:
    values = os.environ if environ is None else environ
    claim_enabled = _boolean(
        values,
        "MSG_SEND_CLAIM_ENABLED",
        default=False,
    )
    send_enabled = _boolean(
        values,
        "MSG_SEND_SEND_ENABLED",
        default=False,
    )
    if claim_enabled and not send_enabled:
        raise ConfigError(
            "claim gate cannot be enabled while send gate is disabled"
        )
    sms_fallback_enabled = _boolean(
        values,
        "MSG_SEND_SMS_FALLBACK_ENABLED",
        default=False,
    )
    callback_apply_enabled = _boolean(
        values,
        "MSG_SEND_CALLBACK_APPLY_ENABLED",
        default=False,
    )
    provider_result_mode = _provider_result_mode(values)
    if (
        provider_result_mode
        is ProviderResultMode.PROVIDER_ACCEPTANCE
        and sms_fallback_enabled
    ):
        raise ConfigError(
            "provider_acceptance requires SMS fallback disabled"
        )
    concurrency = _integer(
        values,
        "MSG_SEND_MAX_CONCURRENCY",
        default=1,
    )
    if concurrency != 1:
        raise ConfigError("production concurrency must equal 1")
    max_messages_per_run = _integer(
        values,
        "MSG_SEND_MAX_MESSAGES_PER_RUN",
        default=20,
    )
    if not 1 <= max_messages_per_run <= 20:
        raise ConfigError(
            "MSG_SEND_MAX_MESSAGES_PER_RUN must be between 1 and 20"
        )
    max_cycle_seconds = _integer(
        values,
        "MSG_SEND_MAX_CYCLE_SECONDS",
        default=50,
    )
    if not 5 <= max_cycle_seconds <= 55:
        raise ConfigError(
            "MSG_SEND_MAX_CYCLE_SECONDS must be between 5 and 55"
        )

    supabase_url = _https_base_url(
        _required(values, "SUPABASE_URL"),
        "SUPABASE_URL",
        required_hostname_suffix=".supabase.co",
    )
    supabase_key = _required(values, "SUPABASE_SECRET_KEY")
    if not supabase_key.startswith("sb_secret_"):
        raise ConfigError(
            "SUPABASE_SECRET_KEY must use the sb_secret_ key format"
        )

    provider_url = _https_base_url(
        values.get(
            "BIZPPURIO_BASE_URL",
            "https://api.bizppurio.com",
        ),
        "BIZPPURIO_BASE_URL",
    )
    account = _bounded_text(
        _required(values, "BIZPPURIO_ACCOUNT"),
        "BIZPPURIO_ACCOUNT",
        128,
    )
    password = _bounded_text(
        _required(values, "BIZPPURIO_PASSWORD"),
        "BIZPPURIO_PASSWORD",
        256,
    )
    from_number = _required(values, "BIZPPURIO_FROM")
    if not re.fullmatch(r"[0-9]{8,15}", from_number):
        raise ConfigError("BIZPPURIO_FROM must be 8-15 digits")
    sender_key = _bounded_text(
        _required(values, "BIZPPURIO_SENDERKEY"),
        "BIZPPURIO_SENDERKEY",
        128,
    )
    timeout = _float(
        values,
        "MSG_SEND_HTTP_TIMEOUT_SECONDS",
        default=10.0,
    )
    if timeout < 1 or timeout > 30:
        raise ConfigError(
            "MSG_SEND_HTTP_TIMEOUT_SECONDS must be between 1 and 30"
        )
    worker_id = _bounded_text(
        values.get("MSG_SEND_WORKER_ID", "spaceship-msg-send-1"),
        "MSG_SEND_WORKER_ID",
        128,
    )

    tariffs = TariffConfig(
        alimtalk=_decimal(
            values,
            "BIZPPURIO_AT_COST_KRW",
        ),
        sms=_decimal(
            values,
            "BIZPPURIO_SMS_COST_KRW",
        ),
    )
    return RuntimeConfig(
        supabase=SupabaseConfig(
            url=supabase_url,
            secret_key=supabase_key,
            timeout_seconds=timeout,
        ),
        bizppurio=BizppurioConfig(
            base_url=provider_url,
            account=account,
            password=password,
            from_number=from_number,
            sender_key=sender_key,
            timeout_seconds=timeout,
        ),
        tariffs=tariffs,
        worker=WorkerConfig(
            max_concurrency=concurrency,
            max_messages_per_run=max_messages_per_run,
            max_cycle_seconds=max_cycle_seconds,
        ),
        worker_id=worker_id,
        claim_enabled=claim_enabled,
        send_enabled=send_enabled,
        sms_fallback_enabled=sms_fallback_enabled,
        callback_apply_enabled=callback_apply_enabled,
        provider_result_mode=provider_result_mode,
    )


def _provider_result_mode(
    values: Mapping[str, str],
) -> ProviderResultMode:
    raw = values.get(
        "MSG_SEND_RESULT_MODE",
        ProviderResultMode.CALLBACK.value,
    )
    try:
        return ProviderResultMode(raw.strip().lower())
    except ValueError as exc:
        raise ConfigError(
            "MSG_SEND_RESULT_MODE must be callback or "
            "provider_acceptance"
        ) from exc


def _required(values: Mapping[str, str], name: str) -> str:
    value = values.get(name)
    if value is None or not value.strip():
        raise ConfigError(f"missing required configuration: {name}")
    return value.strip()


def _boolean(
    values: Mapping[str, str],
    name: str,
    *,
    default: bool,
) -> bool:
    raw = values.get(name)
    if raw is None:
        return default
    normalized = raw.strip().lower()
    if normalized == "true":
        return True
    if normalized == "false":
        return False
    raise ConfigError(f"{name} must be true or false")


def _integer(
    values: Mapping[str, str],
    name: str,
    *,
    default: int,
) -> int:
    raw = values.get(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise ConfigError(f"{name} must be an integer") from exc


def _float(
    values: Mapping[str, str],
    name: str,
    *,
    default: float,
) -> float:
    raw = values.get(name)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise ConfigError(f"{name} must be numeric") from exc


def _decimal(values: Mapping[str, str], name: str) -> Decimal:
    raw = values.get(name)
    if raw is None or not raw.strip():
        raise ConfigError(f"missing required configuration: {name}")
    try:
        value = Decimal(raw)
    except InvalidOperation as exc:
        raise ConfigError(f"{name} must be a decimal tariff") from exc
    try:
        _validate_tariff(value, name)
    except ValueError as exc:
        raise ConfigError(str(exc)) from exc
    return value


def _validate_tariff(value: Decimal, name: str) -> None:
    if (
        not isinstance(value, Decimal)
        or not value.is_finite()
        or value < Decimal("0")
        or value > Decimal("9999999999.9999")
        or value != value.quantize(Decimal("0.0001"))
    ):
        raise ValueError(
            f"{name} must be a nonnegative decimal with at most 4 places"
        )


def _bounded_text(value: str, name: str, maximum: int) -> str:
    if not value or len(value) > maximum:
        raise ConfigError(f"{name} has an invalid length")
    return value


def _https_base_url(
    value: str,
    name: str,
    *,
    required_hostname_suffix: str = "",
) -> str:
    try:
        parsed = urlsplit(value)
    except ValueError as exc:
        raise ConfigError(f"{name} must be a valid HTTPS URL") from exc
    hostname = parsed.hostname or ""
    if (
        parsed.scheme != "https"
        or not hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
        or (
            required_hostname_suffix
            and not hostname.endswith(required_hostname_suffix)
        )
    ):
        raise ConfigError(f"{name} must be a valid HTTPS base URL")
    return value.rstrip("/")
