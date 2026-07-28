from __future__ import annotations

import base64
from datetime import datetime, timedelta, timezone
import json
from typing import Mapping, Optional

from .bizppurio import (
    AccessToken,
    BizppurioRequest,
    BizppurioTransportResponse,
)
from .config import BizppurioConfig
from .correlation import lease_uuid_to_refkey
from .http_transport import (
    HttpRequest,
    HttpResponse,
    HttpTransport,
    StdlibHttpTransport,
)


class BizppurioHttpError(RuntimeError):
    """Sanitized provider HTTP or response-contract failure."""


class BizppurioHttpTransport:
    def __init__(
        self,
        config: BizppurioConfig,
        *,
        transport: Optional[HttpTransport] = None,
    ) -> None:
        self._config = config
        self._transport = transport or StdlibHttpTransport()

    def fetch_access_token(self) -> AccessToken:
        credential = base64.b64encode(
            (
                self._config.account
                + ":"
                + self._config.password
            ).encode("utf-8")
        ).decode("ascii")
        response = self._transport.request(
            HttpRequest(
                method="POST",
                url=self._config.base_url + "/v1/token",
                headers={
                    "Accept": "application/json",
                    "Authorization": f"Basic {credential}",
                    "Connection": "close",
                    "Content-Type": "application/json; charset=utf-8",
                },
                body=b"{}",
                timeout_seconds=self._config.timeout_seconds,
            )
        )
        payload = self._response_json(response, "token")
        token = payload.get("accesstoken")
        expiry = payload.get("expired")
        if not isinstance(token, str) or not token:
            raise BizppurioHttpError(
                "bizppurio token response contract failed"
            )
        return AccessToken(
            value=token,
            expires_at=_parse_expiry(expiry),
        )

    def submit(
        self,
        access_token: str,
        request: BizppurioRequest,
    ) -> BizppurioTransportResponse:
        payload, refkey = self._message_payload(request)
        response = self._transport.request(
            HttpRequest(
                method="POST",
                url=self._config.base_url + "/v3/message",
                headers={
                    "Accept": "application/json",
                    "Authorization": f"Bearer {access_token}",
                    "Connection": "close",
                    "Content-Type": "application/json; charset=utf-8",
                },
                body=_json_bytes(payload),
                timeout_seconds=self._config.timeout_seconds,
            )
        )
        if response.status == 401:
            return BizppurioTransportResponse(
                accepted=False,
                retryable=True,
                failure_code="provider_auth_expired",
                auth_expired=True,
            )
        response_payload = self._response_json(response, "message")
        code = _provider_code(response_payload.get("code"))
        if code != 1000:
            return BizppurioTransportResponse(
                accepted=False,
                retryable=False,
                failure_code=f"provider_code_{code}",
            )
        request_id = response_payload.get("messagekey")
        response_refkey = response_payload.get("refkey")
        if (
            not isinstance(request_id, str)
            or not request_id
            or (
                response_refkey is not None
                and response_refkey != refkey
            )
        ):
            raise BizppurioHttpError(
                "bizppurio message response contract failed"
            )
        return BizppurioTransportResponse(
            accepted=True,
            request_id=request_id,
        )

    def request_report(
        self,
        access_token: str,
        provider_request_id: str,
    ) -> None:
        if not isinstance(provider_request_id, str) or not provider_request_id:
            raise BizppurioHttpError("invalid report identity")
        response = self._transport.request(
            HttpRequest(
                method="POST",
                url=self._config.base_url + "/v2/report",
                headers={
                    "Accept": "application/json",
                    "Authorization": f"Bearer {access_token}",
                    "Connection": "close",
                    "Content-Type": "application/json; charset=utf-8",
                },
                body=_json_bytes(
                    {
                        "account": self._config.account,
                        "messagekey": provider_request_id,
                    }
                ),
                timeout_seconds=self._config.timeout_seconds,
            )
        )
        response_payload = self._response_json(response, "report")
        if _provider_code(response_payload.get("code")) != 1000:
            raise BizppurioHttpError(
                "bizppurio report request was rejected"
            )

    def _message_payload(
        self,
        request: BizppurioRequest,
    ) -> tuple[dict[str, object], str]:
        try:
            refkey = lease_uuid_to_refkey(request.message_key)
        except ValueError as exc:
            raise BizppurioHttpError("invalid message correlation") from exc
        if (
            not request.recipient.isdigit()
            or not 8 <= len(request.recipient) <= 15
            or not request.body
        ):
            raise BizppurioHttpError("invalid message request")
        common: dict[str, object] = {
            "account": self._config.account,
            "refkey": refkey,
            "type": request.channel_code,
            "from": self._config.from_number,
            "to": request.recipient,
        }
        if request.channel_code == "at":
            if (
                not request.template_code
                or not request.title
                or len(request.title) > 50
                or len(request.body) > 1300
                or len(request.buttons) != 2
            ):
                raise BizppurioHttpError("invalid Alimtalk request")
            common["content"] = {
                "at": {
                    "senderkey": self._config.sender_key,
                    "templatecode": request.template_code,
                    "message": request.body,
                    "title": request.title,
                    "button": [
                        {
                            "name": button.name,
                            "type": "WL",
                            "url_mobile": button.url_mobile,
                            "url_pc": button.url_mobile,
                        }
                        for button in request.buttons
                    ],
                }
            }
        elif request.channel_code == "sms":
            common["content"] = {"sms": {"message": request.body}}
        else:
            raise BizppurioHttpError("unsupported message channel")
        return common, refkey

    @staticmethod
    def _response_json(
        response: HttpResponse,
        operation: str,
    ) -> Mapping[str, object]:
        if response.status < 200 or response.status >= 300:
            raise BizppurioHttpError(
                f"bizppurio {operation} request failed: "
                f"status={response.status}"
            )
        try:
            payload = json.loads(response.body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise BizppurioHttpError(
                f"bizppurio {operation} response was invalid"
            ) from exc
        if not isinstance(payload, dict):
            raise BizppurioHttpError(
                f"bizppurio {operation} response was invalid"
            )
        return payload


def _json_bytes(payload: Mapping[str, object]) -> bytes:
    return json.dumps(
        payload,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _provider_code(value: object) -> int:
    if isinstance(value, bool):
        raise BizppurioHttpError("invalid provider response code")
    if isinstance(value, int):
        return value
    if (
        isinstance(value, str)
        and 1 <= len(value) <= 16
        and value.isascii()
        and value.isdigit()
        and (value == "0" or not value.startswith("0"))
    ):
        return int(value)
    raise BizppurioHttpError("invalid provider response code")


def _parse_expiry(value: object) -> datetime:
    if not isinstance(value, str) or not value:
        raise BizppurioHttpError(
            "bizppurio token response contract failed"
        )
    try:
        if len(value) == 14 and value.isdigit():
            parsed = datetime.strptime(value, "%Y%m%d%H%M%S").replace(
                tzinfo=timezone(timedelta(hours=9))
            )
        else:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise BizppurioHttpError(
            "bizppurio token response contract failed"
        ) from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise BizppurioHttpError(
            "bizppurio token expiry must be timezone-aware"
        )
    return parsed
