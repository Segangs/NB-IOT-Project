from __future__ import annotations

from collections.abc import Mapping
import hmac
import re
from typing import Protocol
from urllib.parse import urlsplit

from flask import Blueprint, Response, redirect, request

from msg_send.callback import CallbackContractError
from msg_send.domain import PushAction

try:
    from .limited_links import (
        LimitedLinkService,
        RedeemedLink,
    )
except ImportError:
    from limited_links import (
        LimitedLinkService,
        RedeemedLink,
    )


class CallbackRuntime(Protocol):
    def handle_callback_payload_non_sending(
        self,
        payload: Mapping[str, object],
    ) -> PushAction:
        ...


_CALLBACK_SECRET = re.compile(r"[A-Za-z0-9_-]{32,256}")
_SESSION_ID = re.compile(r"[A-Za-z0-9_-]{32,256}")
_MAX_CALLBACK_BYTES = 16 * 1024


def create_message_blueprint(
    *,
    callback_runtime: CallbackRuntime,
    callback_secret: str,
    limited_links: LimitedLinkService,
) -> Blueprint:
    if (
        not isinstance(callback_secret, str)
        or _CALLBACK_SECRET.fullmatch(callback_secret) is None
    ):
        raise ValueError(
            "callback secret must contain 32-256 characters"
        )
    blueprint = Blueprint("message_delivery", __name__)

    @blueprint.post("/callbacks/bizppurio/<supplied_secret>")
    def bizppurio_callback(supplied_secret: str) -> Response:
        if not hmac.compare_digest(
            supplied_secret,
            callback_secret,
        ):
            return _empty_response(404)
        if (
            not request.is_json
            or (
                request.content_length is not None
                and request.content_length > _MAX_CALLBACK_BYTES
            )
        ):
            return _empty_response(400)
        payload = request.get_json(silent=True)
        if not isinstance(payload, Mapping):
            return _empty_response(400)
        try:
            action = (
                callback_runtime.handle_callback_payload_non_sending(
                    payload
                )
            )
        except CallbackContractError:
            return _empty_response(400)
        except Exception:
            return _empty_response(503)
        if not isinstance(action, PushAction):
            return _empty_response(503)
        return _empty_response(200)

    @blueprint.get("/s/<token>")
    def redeem_message_link(token: str) -> Response:
        try:
            redeemed = limited_links.redeem(token)
        except Exception:
            return _empty_response(503)
        if redeemed is None or not _safe_redeemed_link(redeemed):
            return _empty_response(404)
        response = redirect(redeemed.target_path, code=303)
        response.set_cookie(
            "__Host-limited_session",
            redeemed.session_id,
            max_age=redeemed.max_age_seconds,
            secure=True,
            httponly=True,
            samesite="Lax",
            path="/",
        )
        response.headers["Cache-Control"] = "no-store"
        response.headers["Referrer-Policy"] = "no-referrer"
        return response

    return blueprint


def _empty_response(status: int) -> Response:
    response = Response(status=status)
    response.headers["Cache-Control"] = "no-store"
    response.headers["Referrer-Policy"] = "no-referrer"
    return response


def _safe_redeemed_link(redeemed: RedeemedLink) -> bool:
    target = urlsplit(redeemed.target_path)
    return (
        isinstance(redeemed.session_id, str)
        and _SESSION_ID.fullmatch(redeemed.session_id) is not None
        and isinstance(redeemed.max_age_seconds, int)
        and not isinstance(redeemed.max_age_seconds, bool)
        and 1 <= redeemed.max_age_seconds <= 3600
        and target.scheme == ""
        and target.netloc == ""
        and target.query == ""
        and target.fragment == ""
        and target.path.startswith("/")
        and not target.path.startswith("//")
    )
