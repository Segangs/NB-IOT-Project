from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import re
from typing import Callable, Mapping, Optional
from urllib.parse import urlsplit

from flask import Flask

from msg_send.catalog import TemplateCatalog
from msg_send.config import SupabaseConfig, TariffConfig
from msg_send.domain import ProviderResultMode
from msg_send.supabase_rest import SupabaseRestDatabase

try:
    from .device_temperature_settings import (
        DeviceTemperatureSettingsRepository,
        DeviceTemperatureSettingsService,
    )
    from .device_temperature_settings_adapter import (
        SupabaseDeviceTemperatureSettingsRepository,
    )
    from .device_temperature_settings_routes import (
        create_device_temperature_settings_blueprint,
    )
    from .limited_links import (
        InMemoryLimitedSessionStore,
        LimitedLinkService,
        LimitedSessionGrant,
        LinkTokenRepository,
    )
    from .message_adapters import (
        NonSendingCallbackRuntime,
        SupabaseLinkTokenRepository,
    )
    from .message_routes import CallbackRuntime, create_message_blueprint
except ImportError:
    from device_temperature_settings import (
        DeviceTemperatureSettingsRepository,
        DeviceTemperatureSettingsService,
    )
    from device_temperature_settings_adapter import (
        SupabaseDeviceTemperatureSettingsRepository,
    )
    from device_temperature_settings_routes import (
        create_device_temperature_settings_blueprint,
    )
    from limited_links import (
        InMemoryLimitedSessionStore,
        LimitedLinkService,
        LimitedSessionGrant,
        LinkTokenRepository,
    )
    from message_adapters import (
        NonSendingCallbackRuntime,
        SupabaseLinkTokenRepository,
    )
    from message_routes import CallbackRuntime, create_message_blueprint


class MessageWebConfigError(ValueError):
    """Raised when the optional public message routes are unsafe to enable."""


@dataclass(frozen=True)
class MessageWebConfig:
    callback_secret: str = field(repr=False)
    supabase_url: str
    supabase_secret_key: str = field(repr=False)
    tariffs: TariffConfig
    worker_id: str = "callback-web-1"
    provider_result_mode: ProviderResultMode = ProviderResultMode.CALLBACK


@dataclass(frozen=True)
class InstalledMessageServices:
    sessions: InMemoryLimitedSessionStore

    def resolve_limited_session(
        self,
        session_id: object,
    ) -> Optional[LimitedSessionGrant]:
        return self.sessions.resolve(session_id)


RepositoryFactory = Callable[[MessageWebConfig], LinkTokenRepository]
SettingsRepositoryFactory = Callable[
    [MessageWebConfig],
    DeviceTemperatureSettingsRepository,
]
CallbackRuntimeFactory = Callable[[MessageWebConfig], CallbackRuntime]


def install_message_routes(
    app: Flask,
    *,
    environ: Mapping[str, str],
    repository_factory: Optional[RepositoryFactory] = None,
    settings_repository_factory: Optional[
        SettingsRepositoryFactory
    ] = None,
    callback_runtime_factory: Optional[CallbackRuntimeFactory] = None,
    clock: Callable[[], datetime] = (
        lambda: datetime.now(timezone.utc)
    ),
) -> Optional[InstalledMessageServices]:
    if not _enabled(environ):
        return None
    config = _load_config(environ)
    repository = (
        repository_factory(config)
        if repository_factory is not None
        else _build_repository(config)
    )
    settings_repository = (
        settings_repository_factory(config)
        if settings_repository_factory is not None
        else _build_settings_repository(config)
    )
    callback_runtime = (
        callback_runtime_factory(config)
        if callback_runtime_factory is not None
        else _build_callback_runtime(config, clock)
    )
    sessions = InMemoryLimitedSessionStore(clock=clock)
    links = LimitedLinkService(
        repository=repository,
        sessions=sessions,
        clock=clock,
        max_session_seconds=900,
    )
    app.register_blueprint(
        create_message_blueprint(
            callback_runtime=callback_runtime,
            callback_secret=config.callback_secret,
            limited_links=links,
        )
    )
    app.register_blueprint(
        create_device_temperature_settings_blueprint(
            sessions=sessions,
            service=DeviceTemperatureSettingsService(
                settings_repository
            ),
        )
    )
    installed = InstalledMessageServices(sessions=sessions)
    app.extensions["message_delivery"] = installed
    return installed


def _enabled(environ: Mapping[str, str]) -> bool:
    raw = environ.get("MSG_SEND_WEB_ENABLED", "false").strip().lower()
    if raw == "false":
        return False
    if raw == "true":
        return True
    raise MessageWebConfigError(
        "MSG_SEND_WEB_ENABLED must be true or false"
    )


def _load_config(environ: Mapping[str, str]) -> MessageWebConfig:
    callback_secret = _required(
        environ,
        "MSG_SEND_CALLBACK_SECRET",
    )
    if re.fullmatch(
        r"[A-Za-z0-9_-]{32,256}",
        callback_secret,
    ) is None:
        raise MessageWebConfigError(
            "MSG_SEND_CALLBACK_SECRET must be URL-safe"
        )
    supabase_url = _required(environ, "SUPABASE_URL").rstrip("/")
    parsed = urlsplit(supabase_url)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or not parsed.hostname.endswith(".supabase.co")
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
    ):
        raise MessageWebConfigError("SUPABASE_URL must be an HTTPS origin")
    secret_key = _required(environ, "SUPABASE_SECRET_KEY")
    if not secret_key.startswith("sb_secret_"):
        raise MessageWebConfigError(
            "SUPABASE_SECRET_KEY must use the secret key format"
        )
    tariffs = TariffConfig(
        alimtalk=_decimal(environ, "BIZPPURIO_AT_COST_KRW"),
        sms=_decimal(environ, "BIZPPURIO_SMS_COST_KRW"),
    )
    worker_id = environ.get(
        "MSG_SEND_CALLBACK_WORKER_ID",
        "callback-web-1",
    ).strip()
    if re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", worker_id) is None:
        raise MessageWebConfigError(
            "MSG_SEND_CALLBACK_WORKER_ID is invalid"
        )
    try:
        provider_result_mode = ProviderResultMode(
            environ.get(
                "MSG_SEND_RESULT_MODE",
                ProviderResultMode.CALLBACK.value,
            ).strip().lower()
        )
    except ValueError as exc:
        raise MessageWebConfigError(
            "MSG_SEND_RESULT_MODE must be callback or "
            "provider_acceptance"
        ) from exc
    return MessageWebConfig(
        callback_secret=callback_secret,
        supabase_url=supabase_url,
        supabase_secret_key=secret_key,
        tariffs=tariffs,
        worker_id=worker_id,
        provider_result_mode=provider_result_mode,
    )


def _build_repository(
    config: MessageWebConfig,
) -> SupabaseLinkTokenRepository:
    from supabase import create_client

    client = create_client(
        config.supabase_url,
        config.supabase_secret_key,
    )
    return SupabaseLinkTokenRepository(client)


def _build_settings_repository(
    config: MessageWebConfig,
) -> SupabaseDeviceTemperatureSettingsRepository:
    from supabase import create_client

    client = create_client(
        config.supabase_url,
        config.supabase_secret_key,
    )
    return SupabaseDeviceTemperatureSettingsRepository(client)


def _build_callback_runtime(
    config: MessageWebConfig,
    clock: Callable[[], datetime],
) -> NonSendingCallbackRuntime:
    database = SupabaseRestDatabase(
        SupabaseConfig(
            url=config.supabase_url,
            secret_key=config.supabase_secret_key,
        ),
        TemplateCatalog.load_approved(),
    )
    return NonSendingCallbackRuntime(
        database=database,
        tariffs=config.tariffs,
        worker_id=config.worker_id,
        clock=clock,
        provider_result_mode=config.provider_result_mode,
    )


def _required(
    environ: Mapping[str, str],
    name: str,
) -> str:
    value = environ.get(name)
    if value is None or not value.strip():
        raise MessageWebConfigError(
            f"missing required configuration: {name}"
        )
    return value.strip()


def _decimal(
    environ: Mapping[str, str],
    name: str,
) -> Decimal:
    raw = _required(environ, name)
    try:
        return Decimal(raw)
    except InvalidOperation as exc:
        raise MessageWebConfigError(
            f"{name} must be a decimal"
        ) from exc
