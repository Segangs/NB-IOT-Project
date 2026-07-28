from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timedelta
from threading import Lock
from typing import Callable, Optional, Protocol

from .catalog import TemplateCatalog
from .correlation import lease_uuid_to_refkey
from .domain import (
    DeliveryChannel,
    MessageJob,
    ProviderSubmission,
    ProviderSubmissionNotAttempted,
    ProviderSubmissionOutcomeUnknown,
    render_history_links,
)


@dataclass(frozen=True)
class AccessToken:
    value: str = field(repr=False)
    expires_at: datetime

    def __post_init__(self) -> None:
        if not self.value:
            raise ValueError("access token must not be empty")


@dataclass(frozen=True)
class BizppurioButton:
    name: str
    url_mobile: str = field(repr=False)


@dataclass(frozen=True)
class BizppurioRequest:
    message_key: str = field(repr=False)
    recipient: str = field(repr=False)
    channel_code: str
    body: str = field(repr=False)
    template_code: Optional[str]
    title: Optional[str] = None
    buttons: tuple[BizppurioButton, ...] = field(
        default=(),
        repr=False,
    )


@dataclass(frozen=True)
class BizppurioTransportResponse:
    accepted: bool
    request_id: Optional[str] = None
    retryable: bool = False
    failure_code: Optional[str] = None
    auth_expired: bool = False


class BizppurioTransport(Protocol):
    def fetch_access_token(self) -> AccessToken:
        ...

    def submit(
        self,
        access_token: str,
        request: BizppurioRequest,
    ) -> BizppurioTransportResponse:
        ...

    def request_report(
        self,
        access_token: str,
        provider_request_id: str,
    ) -> None:
        ...


class BizppurioAdapter:
    def __init__(
        self,
        transport: BizppurioTransport,
        clock: Callable[[], datetime],
        refresh_skew_seconds: int = 30,
        catalog: Optional[TemplateCatalog] = None,
        resubmit_after_auth_expiry: bool = True,
    ) -> None:
        if refresh_skew_seconds < 0:
            raise ValueError("refresh_skew_seconds must not be negative")
        self._transport = transport
        self._clock = clock
        self._refresh_skew = timedelta(
            seconds=refresh_skew_seconds,
        )
        self._cached_token: Optional[AccessToken] = None
        self._token_lock = Lock()
        self._catalog = catalog
        self._resubmit_after_auth_expiry = (
            resubmit_after_auth_expiry
        )

    def submit(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> ProviderSubmission:
        if not isinstance(channel, DeliveryChannel):
            raise ValueError("unsupported delivery channel")

        message_submit_started = False
        try:
            request = self._build_request(job, channel)
            token = self._access_token()
            message_submit_started = True
            response = self._transport.submit(token.value, request)

            if response.auth_expired:
                self._invalidate_token(token)
                if not self._resubmit_after_auth_expiry:
                    return ProviderSubmission(
                        accepted=False,
                        retryable=True,
                        failure_code="provider_auth_unavailable",
                    )
                refreshed = self._access_token()
                response = self._transport.submit(
                    refreshed.value,
                    request,
                )
                if response.auth_expired:
                    self._invalidate_token(refreshed)
                    return ProviderSubmission(
                        accepted=False,
                        retryable=True,
                        failure_code="provider_auth_unavailable",
                    )

            if response.accepted and not response.request_id:
                return ProviderSubmission(
                    accepted=False,
                    retryable=False,
                    failure_code="invalid_provider_response",
                )
            return ProviderSubmission(
                accepted=response.accepted,
                request_id=response.request_id,
                retryable=response.retryable,
                failure_code=response.failure_code,
            )
        except Exception as exc:
            if message_submit_started:
                raise ProviderSubmissionOutcomeUnknown() from exc
            raise ProviderSubmissionNotAttempted() from exc

    def request_report(self, provider_request_id: str) -> None:
        if (
            not isinstance(provider_request_id, str)
            or not provider_request_id
        ):
            raise ValueError("provider request id is required")
        token = self._access_token()
        self._transport.request_report(
            token.value,
            provider_request_id,
        )

    def _access_token(self) -> AccessToken:
        with self._token_lock:
            now = self._clock()
            if (
                self._cached_token is None
                or self._cached_token.expires_at
                <= now + self._refresh_skew
            ):
                self._cached_token = (
                    self._transport.fetch_access_token()
                )
            return self._cached_token

    def _invalidate_token(self, token: AccessToken) -> None:
        with self._token_lock:
            if self._cached_token == token:
                self._cached_token = None

    def _build_request(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> BizppurioRequest:
        if self._catalog is not None:
            lease_uuid_to_refkey(job.lease_token)
        if channel is DeliveryChannel.ALIMTALK:
            if self._catalog is not None:
                rendered = self._catalog.render(
                    job.template_code,
                    job.template_params,
                    history_url=job.history_url,
                    settings_url=job.settings_url,
                )
                return BizppurioRequest(
                    message_key=job.lease_token,
                    recipient=job.recipient,
                    channel_code="at",
                    body=rendered.body,
                    template_code=rendered.template_code,
                    title=rendered.title,
                    buttons=tuple(
                        BizppurioButton(
                            button.name,
                            button.url_mobile,
                        )
                        for button in rendered.buttons
                    ),
                )
            return BizppurioRequest(
                message_key=job.lease_token,
                recipient=job.recipient,
                channel_code="at",
                body=render_history_links(
                    job.template_body,
                    job.history_url,
                ),
                template_code=job.template_code,
            )
        if channel is DeliveryChannel.SMS:
            return BizppurioRequest(
                message_key=job.lease_token,
                recipient=job.recipient,
                channel_code="sms",
                body=job.sms_body,
                template_code=None,
            )
        raise ValueError("unsupported delivery channel")
