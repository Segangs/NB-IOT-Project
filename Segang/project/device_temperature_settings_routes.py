from __future__ import annotations

from collections.abc import Sequence
import hmac
import re
from typing import Protocol
from urllib.parse import urlsplit

from flask import (
    Blueprint,
    Response,
    make_response,
    render_template,
    request,
)

try:
    from .device_temperature_settings import (
        DeviceTemperatureSettingsService,
        SensorTemperatureSetting,
        SensorTemperatureUpdate,
        SettingsRepositoryError,
        SettingsScopeError,
        SettingsValidationError,
        format_upper_limit,
    )
    from .limited_links import LimitedSessionGrant, LinkPurpose
except ImportError:
    from device_temperature_settings import (
        DeviceTemperatureSettingsService,
        SensorTemperatureSetting,
        SensorTemperatureUpdate,
        SettingsRepositoryError,
        SettingsScopeError,
        SettingsValidationError,
        format_upper_limit,
    )
    from limited_links import LimitedSessionGrant, LinkPurpose


class LimitedSessionResolver(Protocol):
    def resolve(self, session_id: object) -> LimitedSessionGrant | None:
        ...

    def discard(self, session_id: object) -> bool:
        ...


_SESSION_COOKIE = "__Host-limited_session"
_MAX_FORM_BYTES = 16 * 1024
_COUNT = re.compile(r"[1-3]")


def create_device_temperature_settings_blueprint(
    *,
    sessions: LimitedSessionResolver,
    service: DeviceTemperatureSettingsService,
) -> Blueprint:
    blueprint = Blueprint("device_temperature_settings", __name__)

    @blueprint.after_request
    def protect_settings_response(response: Response) -> Response:
        response.headers["Cache-Control"] = "no-store"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Content-Security-Policy"] = (
            "default-src 'none'; style-src 'unsafe-inline' 'self'; "
            "font-src 'self'; form-action 'self'; "
            "frame-ancestors 'none'; base-uri 'none'"
        )
        return response

    @blueprint.route(
        "/device-settings/<int:device_id>",
        methods=("GET", "POST"),
    )
    def device_settings(device_id: int) -> Response:
        session_id = request.cookies.get(_SESSION_COOKIE)
        limited_grant = _settings_grant(
            sessions.resolve(session_id),
            device_id,
        )
        if limited_grant is None:
            return _empty_response(404)

        if request.method == "GET":
            try:
                sensor_settings = service.load(limited_grant)
            except SettingsScopeError:
                return _empty_response(404)
            except SettingsRepositoryError:
                return _empty_response(503)
            return _render_settings(
                limited_grant,
                sensor_settings,
                saved=False,
            )

        if not _same_origin_request():
            return _empty_response(403)
        if (
            request.content_length is not None
            and request.content_length > _MAX_FORM_BYTES
        ):
            return _empty_response(400)
        if not _valid_csrf(limited_grant.csrf_token):
            return _empty_response(400)
        try:
            updates = _parse_updates(
                request.form,
                limited_grant.sensor_ids,
            )
        except (SettingsValidationError, SettingsScopeError):
            return _empty_response(400)

        try:
            updated = service.update(limited_grant, updates)
        except SettingsScopeError:
            return _empty_response(404)
        except SettingsRepositoryError:
            return _empty_response(503)

        sessions.discard(session_id)
        response = _render_settings(
            limited_grant,
            updated,
            saved=True,
        )
        response.delete_cookie(
            _SESSION_COOKIE,
            path="/",
            secure=True,
            httponly=True,
            samesite="Lax",
        )
        return response

    return blueprint


def _settings_grant(
    grant: LimitedSessionGrant | None,
    device_id: int,
) -> LimitedSessionGrant | None:
    if (
        grant is None
        or grant.purpose is not LinkPurpose.SETTINGS
        or grant.device_id != device_id
        or grant.target_path != f"/device-settings/{device_id}"
    ):
        return None
    return grant


def _same_origin_request() -> bool:
    fetch_site = request.headers.get("Sec-Fetch-Site")
    if fetch_site not in (None, "same-origin", "none"):
        return False
    origin = request.headers.get("Origin")
    if not origin:
        return False
    parsed = urlsplit(origin)
    return (
        parsed.scheme == "https"
        and parsed.netloc == request.host
        and parsed.path in ("", "/")
        and not parsed.query
        and not parsed.fragment
    )


def _valid_csrf(expected: str) -> bool:
    values = request.form.getlist("csrf_token")
    return (
        len(values) == 1
        and isinstance(expected, str)
        and hmac.compare_digest(values[0], expected)
    )


def _parse_updates(
    form: object,
    sensor_ids: tuple[int, ...],
) -> tuple[SensorTemperatureUpdate, ...]:
    if not hasattr(form, "keys") or not hasattr(form, "getlist"):
        raise SettingsValidationError("settings form is malformed")
    expected_fields = {"csrf_token"}
    for sensor_id in sensor_ids:
        expected_fields.add(f"upper_limit_{sensor_id}")
        expected_fields.add(f"max_notifications_{sensor_id}")
    if not set(form.keys()).issubset(expected_fields):  # type: ignore[attr-defined]
        raise SettingsScopeError("settings form escaped sensor scope")

    updates = []
    for sensor_id in sensor_ids:
        upper_values = form.getlist(  # type: ignore[attr-defined]
            f"upper_limit_{sensor_id}"
        )
        count_values = form.getlist(  # type: ignore[attr-defined]
            f"max_notifications_{sensor_id}"
        )
        if not upper_values and not count_values:
            continue
        if len(upper_values) != 1 or len(count_values) != 1:
            raise SettingsValidationError(
                "each sensor setting must occur exactly once"
            )
        count_text = count_values[0]
        if not isinstance(count_text, str) or _COUNT.fullmatch(
            count_text
        ) is None:
            raise SettingsValidationError(
                "notification count is invalid"
            )
        updates.append(
            SensorTemperatureUpdate.parse(
                sensor_pk=sensor_id,
                upper_limit=upper_values[0],
                max_notifications=int(count_text),
            )
        )
    if not updates:
        raise SettingsValidationError(
            "at least one sensor setting is required"
        )
    return tuple(updates)


def _render_settings(
    grant: LimitedSessionGrant,
    settings: Sequence[SensorTemperatureSetting],
    *,
    saved: bool,
) -> Response:
    rows = [
        {
            "sensor_pk": item.sensor_pk,
            "label": f"TEMP{item.user_sensor_id}",
            "upper_limit": format_upper_limit(item.upper_limit),
            "max_notifications": item.max_notifications,
            "latest_value": (
                f"{item.latest_value:.1f}"
                if item.latest_value is not None
                else None
            ),
            "latest_observed_at": item.latest_observed_at,
        }
        for item in settings
    ]
    return make_response(
        render_template(
            "device_temperature_settings.html",
            csrf_token=grant.csrf_token,
            device_id=grant.device_id,
            saved=saved,
            sensors=rows,
        ),
        200,
    )


def _empty_response(status: int) -> Response:
    return Response(status=status)
