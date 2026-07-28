from __future__ import annotations

from datetime import datetime, timezone
from decimal import Decimal
import unittest

from flask import Flask

from msg_send.domain import ProviderResultMode, PushAction
from Segang.project.device_temperature_settings import (
    SensorTemperatureSetting,
    SensorTemperatureUpdate,
)
from Segang.project.limited_links import LinkTokenRecord
from Segang.project.message_integration import (
    MessageWebConfigError,
    install_message_routes,
)


NOW = datetime(2026, 7, 26, 12, 30, tzinfo=timezone.utc)
CALLBACK_SECRET = "callback_secret_reference_1234567890abcd"


class EmptyRepository:
    def lookup(self, token_hash: bytes) -> LinkTokenRecord | None:
        del token_hash
        return None

    def consume_if_current(
        self,
        record: LinkTokenRecord,
        now: datetime,
    ) -> bool:
        del record, now
        return False


class StubCallbackRuntime:
    def handle_callback_payload_non_sending(
        self,
        payload: dict[str, object],
    ) -> PushAction:
        del payload
        return PushAction.DELIVERED


class EmptySettingsRepository:
    def load(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
    ) -> tuple[SensorTemperatureSetting, ...]:
        del user_id, workplace_id, device_id
        raise AssertionError("settings route was not requested")

    def update(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
        updates: tuple[SensorTemperatureUpdate, ...],
    ) -> tuple[SensorTemperatureSetting, ...]:
        del user_id, workplace_id, device_id, updates
        raise AssertionError("settings route was not requested")


def enabled_env() -> dict[str, str]:
    return {
        "MSG_SEND_WEB_ENABLED": "true",
        "MSG_SEND_CALLBACK_SECRET": CALLBACK_SECRET,
        "SUPABASE_URL": "https://project-ref.supabase.co",
        "SUPABASE_SECRET_KEY": "sb_secret_test_only_not_a_real_key",
        "BIZPPURIO_AT_COST_KRW": "8.0000",
        "BIZPPURIO_SMS_COST_KRW": "12.0000",
    }


class MessageIntegrationTests(unittest.TestCase):
    def test_disabled_install_has_no_routes_or_secret_requirements(
        self,
    ) -> None:
        app = Flask(__name__)

        installed = install_message_routes(
            app,
            environ={},
            repository_factory=lambda config: self.fail(
                f"unexpected repository config: {config}"
            ),
            settings_repository_factory=lambda config: self.fail(
                f"unexpected settings config: {config}"
            ),
            callback_runtime_factory=lambda config: self.fail(
                f"unexpected callback config: {config}"
            ),
            clock=lambda: NOW,
        )

        self.assertIsNone(installed)
        routes = {rule.rule for rule in app.url_map.iter_rules()}
        self.assertNotIn("/s/<token>", routes)

    def test_enabled_install_is_fail_fast_when_secret_is_missing(
        self,
    ) -> None:
        app = Flask(__name__)
        environment = enabled_env()
        environment.pop("MSG_SEND_CALLBACK_SECRET")

        with self.assertRaises(MessageWebConfigError):
            install_message_routes(
                app,
                environ=environment,
                repository_factory=lambda config: EmptyRepository(),
                settings_repository_factory=(
                    lambda config: EmptySettingsRepository()
                ),
                callback_runtime_factory=(
                    lambda config: StubCallbackRuntime()
                ),
                clock=lambda: NOW,
            )

    def test_enabled_install_rejects_non_supabase_secret_destination(
        self,
    ) -> None:
        app = Flask(__name__)
        environment = enabled_env()
        environment["SUPABASE_URL"] = "https://attacker.example"

        with self.assertRaisesRegex(
            MessageWebConfigError,
            "SUPABASE_URL",
        ):
            install_message_routes(
                app,
                environ=environment,
                repository_factory=lambda config: EmptyRepository(),
                settings_repository_factory=(
                    lambda config: EmptySettingsRepository()
                ),
                callback_runtime_factory=(
                    lambda config: StubCallbackRuntime()
                ),
                clock=lambda: NOW,
            )

    def test_enabled_install_registers_only_bounded_message_routes(
        self,
    ) -> None:
        app = Flask(__name__)
        captured = []

        installed = install_message_routes(
            app,
            environ=enabled_env(),
            repository_factory=lambda config: (
                captured.append(config),
                EmptyRepository(),
            )[1],
            settings_repository_factory=lambda config: (
                captured.append(config),
                EmptySettingsRepository(),
            )[1],
            callback_runtime_factory=lambda config: (
                captured.append(config),
                StubCallbackRuntime(),
            )[1],
            clock=lambda: NOW,
        )

        self.assertIsNotNone(installed)
        assert installed is not None
        self.assertEqual(len(captured), 3)
        self.assertEqual(captured[0].callback_secret, CALLBACK_SECRET)
        self.assertEqual(
            captured[0].tariffs.alimtalk,
            Decimal("8.0000"),
        )
        self.assertIs(
            captured[0].provider_result_mode,
            ProviderResultMode.CALLBACK,
        )
        routes = {rule.rule for rule in app.url_map.iter_rules()}
        self.assertIn(
            "/callbacks/bizppurio/<supplied_secret>",
            routes,
        )
        self.assertIn("/s/<token>", routes)
        self.assertIn(
            "/device-settings/<int:device_id>",
            routes,
        )
        self.assertIsNone(installed.resolve_limited_session("short"))

    def test_web_config_accepts_explicit_provider_acceptance_mode(
        self,
    ) -> None:
        app = Flask(__name__)
        captured = []
        environment = enabled_env()
        environment["MSG_SEND_RESULT_MODE"] = "provider_acceptance"

        install_message_routes(
            app,
            environ=environment,
            repository_factory=lambda config: EmptyRepository(),
            settings_repository_factory=(
                lambda config: EmptySettingsRepository()
            ),
            callback_runtime_factory=lambda config: (
                captured.append(config),
                StubCallbackRuntime(),
            )[1],
            clock=lambda: NOW,
        )

        self.assertIs(
            captured[0].provider_result_mode,
            ProviderResultMode.PROVIDER_ACCEPTANCE,
        )


if __name__ == "__main__":
    unittest.main()
