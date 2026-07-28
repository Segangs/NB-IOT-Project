from __future__ import annotations

from datetime import datetime, timedelta, timezone
from decimal import Decimal
import unittest

from flask import Flask
from werkzeug.datastructures import MultiDict

from Segang.project.device_temperature_settings import (
    SensorTemperatureSetting,
    SensorTemperatureUpdate,
    SettingsRepositoryError,
)
from Segang.project.device_temperature_settings_routes import (
    create_device_temperature_settings_blueprint,
)
from Segang.project.limited_links import LimitedSessionGrant, LinkPurpose


NOW = datetime(2026, 7, 27, 10, 0, tzinfo=timezone.utc)
SESSION_ID = "limited_session_reference_1234567890abcdef"
CSRF_TOKEN = "limited_csrf_reference_1234567890abcdefgh"
BASE_URL = "https://zxcx.io"


def grant(**changes: object) -> LimitedSessionGrant:
    values = {
        "purpose": LinkPurpose.SETTINGS,
        "target_path": "/device-settings/42",
        "user_id": 3,
        "workplace_id": 8,
        "device_id": 42,
        "sensor_ids": (7, 8),
        "expires_at": NOW + timedelta(minutes=15),
        "csrf_token": CSRF_TOKEN,
    }
    values.update(changes)
    return LimitedSessionGrant(**values)  # type: ignore[arg-type]


def settings() -> tuple[SensorTemperatureSetting, ...]:
    return (
        SensorTemperatureSetting(
            sensor_pk=7,
            user_sensor_id=1,
            upper_limit=Decimal("-7.0"),
            max_notifications=3,
            latest_value=Decimal("-8.5"),
            latest_observed_at=NOW,
        ),
        SensorTemperatureSetting(
            sensor_pk=8,
            user_sensor_id=2,
            upper_limit=Decimal("-10.0"),
            max_notifications=2,
            latest_value=None,
            latest_observed_at=None,
        ),
    )


class StubSessions:
    def __init__(
        self,
        resolved: LimitedSessionGrant | None,
    ) -> None:
        self.resolved = resolved
        self.resolve_calls: list[object] = []
        self.discard_calls: list[object] = []

    def resolve(self, session_id: object) -> LimitedSessionGrant | None:
        self.resolve_calls.append(session_id)
        if session_id != SESSION_ID:
            return None
        return self.resolved

    def discard(self, session_id: object) -> bool:
        self.discard_calls.append(session_id)
        if session_id != SESSION_ID or self.resolved is None:
            return False
        self.resolved = None
        return True


class StubService:
    def __init__(self) -> None:
        self.loaded = settings()
        self.load_calls: list[LimitedSessionGrant] = []
        self.update_calls: list[
            tuple[
                LimitedSessionGrant,
                tuple[SensorTemperatureUpdate, ...],
            ]
        ] = []
        self.load_error: Exception | None = None
        self.update_error: Exception | None = None

    def load(
        self,
        limited_grant: LimitedSessionGrant,
    ) -> tuple[SensorTemperatureSetting, ...]:
        self.load_calls.append(limited_grant)
        if self.load_error is not None:
            raise self.load_error
        return self.loaded

    def update(
        self,
        limited_grant: LimitedSessionGrant,
        updates: tuple[SensorTemperatureUpdate, ...],
    ) -> tuple[SensorTemperatureSetting, ...]:
        self.update_calls.append((limited_grant, updates))
        if self.update_error is not None:
            raise self.update_error
        return self.loaded


def build_client(
    resolved: LimitedSessionGrant | None = None,
) -> tuple[object, StubSessions, StubService]:
    sessions = StubSessions(resolved)
    service = StubService()
    app = Flask(
        __name__,
        template_folder="../templates",
        static_folder="../static",
    )
    app.config.update(TESTING=True, SECRET_KEY="settings-route-test")
    app.register_blueprint(
        create_device_temperature_settings_blueprint(
            sessions=sessions,
            service=service,
        )
    )
    client = app.test_client()
    if resolved is not None:
        client.set_cookie(
            "__Host-limited_session",
            SESSION_ID,
            domain="zxcx.io",
            secure=True,
            httponly=True,
            samesite="Lax",
        )
    return client, sessions, service


class DeviceTemperatureSettingsRouteTests(unittest.TestCase):
    def test_get_without_cookie_or_resolved_session_is_not_found(self) -> None:
        for resolved in (None, grant()):
            with self.subTest(resolved=resolved):
                client, sessions, service = build_client(resolved)
                if resolved is not None:
                    client.delete_cookie(
                        "__Host-limited_session",
                        domain="zxcx.io",
                    )

                response = client.get(
                    "/device-settings/42",
                    base_url=BASE_URL,
                )

                self.assertEqual(response.status_code, 404)
                self.assertEqual(service.load_calls, [])

    def test_get_wrong_purpose_or_device_is_not_found(self) -> None:
        cases = (
            grant(purpose=LinkPurpose.TEMP_HISTORY),
            grant(device_id=99, target_path="/device-settings/99"),
        )
        for resolved in cases:
            with self.subTest(resolved=resolved):
                client, _, service = build_client(resolved)

                response = client.get(
                    "/device-settings/42",
                    base_url=BASE_URL,
                )

                self.assertEqual(response.status_code, 404)
                self.assertEqual(service.load_calls, [])

    def test_get_renders_each_sensor_current_values_and_fixed_interval(
        self,
    ) -> None:
        client, _, service = build_client(grant())

        response = client.get(
            "/device-settings/42",
            base_url=BASE_URL,
        )

        self.assertEqual(response.status_code, 200)
        self.assertIn("센서별 온도 알림 설정".encode("utf-8"), response.data)
        self.assertIn(b"TEMP1", response.data)
        self.assertIn(b"TEMP2", response.data)
        self.assertIn(b'value="-7.0"', response.data)
        self.assertIn(b'value="-10.0"', response.data)
        self.assertIn("20분 고정".encode("utf-8"), response.data)
        self.assertIn("측정값 없음".encode("utf-8"), response.data)
        self.assertEqual(service.load_calls, [grant()])
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        self.assertEqual(response.headers["Referrer-Policy"], "no-referrer")

    def test_post_rejects_cross_origin_before_service_write(self) -> None:
        client, sessions, service = build_client(grant())

        response = client.post(
            "/device-settings/42",
            base_url=BASE_URL,
            headers={"Origin": "https://attacker.example"},
            data={
                "csrf_token": CSRF_TOKEN,
                "upper_limit_7": "-6.5",
                "max_notifications_7": "2",
            },
        )

        self.assertEqual(response.status_code, 403)
        self.assertEqual(service.update_calls, [])
        self.assertEqual(sessions.discard_calls, [])

    def test_post_rejects_csrf_missing_pair_duplicate_and_out_of_scope(
        self,
    ) -> None:
        cases = (
            MultiDict(
                [
                    ("csrf_token", "wrong_csrf_reference_1234567890abcd"),
                    ("upper_limit_7", "-6.5"),
                    ("max_notifications_7", "2"),
                ]
            ),
            MultiDict(
                [
                    ("csrf_token", CSRF_TOKEN),
                    ("upper_limit_7", "-6.5"),
                ]
            ),
            MultiDict(
                [
                    ("csrf_token", CSRF_TOKEN),
                    ("upper_limit_7", "-6.5"),
                    ("upper_limit_7", "-6.0"),
                    ("max_notifications_7", "2"),
                ]
            ),
            MultiDict(
                [
                    ("csrf_token", CSRF_TOKEN),
                    ("upper_limit_7", "-6.25"),
                    ("max_notifications_7", "2"),
                ]
            ),
            MultiDict(
                [
                    ("csrf_token", CSRF_TOKEN),
                    ("upper_limit_99", "-6.5"),
                    ("max_notifications_99", "2"),
                ]
            ),
        )
        for form_data in cases:
            with self.subTest(form_data=form_data):
                client, sessions, service = build_client(grant())

                response = client.post(
                    "/device-settings/42",
                    base_url=BASE_URL,
                    headers={"Origin": BASE_URL},
                    data=form_data,
                )

                self.assertEqual(response.status_code, 400)
                self.assertEqual(service.update_calls, [])
                self.assertEqual(sessions.discard_calls, [])

    def test_successful_post_updates_once_then_discards_session_cookie(
        self,
    ) -> None:
        client, sessions, service = build_client(grant())

        response = client.post(
            "/device-settings/42",
            base_url=BASE_URL,
            headers={"Origin": BASE_URL},
            data={
                "csrf_token": CSRF_TOKEN,
                "upper_limit_7": "-6.5",
                "max_notifications_7": "2",
                "upper_limit_8": "-9",
                "max_notifications_8": "1",
            },
        )

        self.assertEqual(response.status_code, 200)
        self.assertIn("설정 저장 완료".encode("utf-8"), response.data)
        self.assertEqual(len(service.update_calls), 1)
        _, updates = service.update_calls[0]
        self.assertEqual(
            [
                (
                    update.sensor_pk,
                    update.upper_limit,
                    update.max_notifications,
                )
                for update in updates
            ],
            [
                (7, Decimal("-6.5"), 2),
                (8, Decimal("-9.0"), 1),
            ],
        )
        self.assertEqual(sessions.discard_calls, [SESSION_ID])
        cookie = response.headers["Set-Cookie"]
        self.assertIn("__Host-limited_session=", cookie)
        self.assertIn("Max-Age=0", cookie)
        self.assertEqual(response.headers["Cache-Control"], "no-store")

        second = client.get(
            "/device-settings/42",
            base_url=BASE_URL,
        )
        self.assertEqual(second.status_code, 404)

    def test_repository_failure_keeps_session_for_safe_retry(self) -> None:
        client, sessions, service = build_client(grant())
        service.update_error = SettingsRepositoryError("private detail")

        response = client.post(
            "/device-settings/42",
            base_url=BASE_URL,
            headers={"Origin": BASE_URL},
            data={
                "csrf_token": CSRF_TOKEN,
                "upper_limit_7": "-6.5",
                "max_notifications_7": "2",
            },
        )

        self.assertEqual(response.status_code, 503)
        self.assertEqual(sessions.discard_calls, [])
        self.assertNotIn(b"private detail", response.data)


if __name__ == "__main__":
    unittest.main()
