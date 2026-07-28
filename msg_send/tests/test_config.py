from __future__ import annotations

from decimal import Decimal
import unittest

from msg_send.config import (
    ConfigError,
    load_runtime_config,
)
from msg_send.domain import ProviderResultMode


def valid_env() -> dict[str, str]:
    return {
        "SUPABASE_URL": "https://project-ref.supabase.co",
        "SUPABASE_SECRET_KEY": "sb_secret_test_only_not_a_real_key",
        "BIZPPURIO_ACCOUNT": "provider-account",
        "BIZPPURIO_PASSWORD": "provider-password",
        "BIZPPURIO_FROM": "0212345678",
        "BIZPPURIO_SENDERKEY": "sender-profile-key",
        "BIZPPURIO_AT_COST_KRW": "8.5000",
        "BIZPPURIO_SMS_COST_KRW": "12.0000",
    }


class RuntimeConfigTests(unittest.TestCase):
    def test_safety_gates_default_locked_with_single_concurrency(self) -> None:
        config = load_runtime_config(valid_env())

        self.assertFalse(config.claim_enabled)
        self.assertFalse(config.send_enabled)
        self.assertFalse(config.sms_fallback_enabled)
        self.assertIs(
            config.provider_result_mode,
            ProviderResultMode.CALLBACK,
        )
        self.assertEqual(config.worker.max_concurrency, 1)
        self.assertEqual(config.worker.max_messages_per_run, 20)
        self.assertEqual(config.worker.max_cycle_seconds, 50)

    def test_sequential_drain_limits_are_configurable_and_bounded(
        self,
    ) -> None:
        environment = valid_env()
        environment["MSG_SEND_MAX_MESSAGES_PER_RUN"] = "7"
        environment["MSG_SEND_MAX_CYCLE_SECONDS"] = "35"

        config = load_runtime_config(environment)

        self.assertEqual(config.worker.max_messages_per_run, 7)
        self.assertEqual(config.worker.max_cycle_seconds, 35)

        invalid_values = (
            ("MSG_SEND_MAX_MESSAGES_PER_RUN", "0"),
            ("MSG_SEND_MAX_MESSAGES_PER_RUN", "21"),
            ("MSG_SEND_MAX_CYCLE_SECONDS", "4"),
            ("MSG_SEND_MAX_CYCLE_SECONDS", "56"),
        )
        for name, value in invalid_values:
            with self.subTest(name=name, value=value):
                invalid = valid_env()
                invalid[name] = value
                with self.assertRaisesRegex(ConfigError, name):
                    load_runtime_config(invalid)

    def test_provider_acceptance_result_mode_is_explicit(self) -> None:
        environment = valid_env()
        environment["MSG_SEND_RESULT_MODE"] = "provider_acceptance"

        config = load_runtime_config(environment)

        self.assertIs(
            config.provider_result_mode,
            ProviderResultMode.PROVIDER_ACCEPTANCE,
        )

    def test_unknown_provider_result_mode_is_rejected(self) -> None:
        environment = valid_env()
        environment["MSG_SEND_RESULT_MODE"] = "always_success"

        with self.assertRaisesRegex(ConfigError, "MSG_SEND_RESULT_MODE"):
            load_runtime_config(environment)

    def test_provider_acceptance_rejects_sms_fallback(self) -> None:
        environment = valid_env()
        environment["MSG_SEND_RESULT_MODE"] = "provider_acceptance"
        environment["MSG_SEND_SMS_FALLBACK_ENABLED"] = "true"

        with self.assertRaisesRegex(
            ConfigError,
            "provider_acceptance.*SMS fallback",
        ):
            load_runtime_config(environment)

    def test_claim_enabled_send_disabled_is_rejected(self) -> None:
        environment = valid_env()
        environment["MSG_SEND_CLAIM_ENABLED"] = "true"
        environment["MSG_SEND_SEND_ENABLED"] = "false"

        with self.assertRaisesRegex(ConfigError, "claim.*send"):
            load_runtime_config(environment)

    def test_concurrency_other_than_one_is_rejected(self) -> None:
        environment = valid_env()
        environment["MSG_SEND_MAX_CONCURRENCY"] = "2"

        with self.assertRaisesRegex(ConfigError, "concurrency"):
            load_runtime_config(environment)

    def test_new_supabase_secret_key_and_https_are_required(self) -> None:
        for key in (
            "legacy-service-role-jwt",
            "sb_publishable_not_allowed",
        ):
            with self.subTest(key=key):
                environment = valid_env()
                environment["SUPABASE_SECRET_KEY"] = key
                with self.assertRaisesRegex(ConfigError, "SUPABASE_SECRET_KEY"):
                    load_runtime_config(environment)

        environment = valid_env()
        environment["SUPABASE_URL"] = "http://project-ref.supabase.co"
        with self.assertRaisesRegex(ConfigError, "SUPABASE_URL"):
            load_runtime_config(environment)

    def test_callback_tariffs_are_explicit_exact_nonnegative_decimals(
        self,
    ) -> None:
        config = load_runtime_config(valid_env())
        self.assertEqual(config.tariffs.alimtalk, Decimal("8.5000"))
        self.assertEqual(config.tariffs.sms, Decimal("12.0000"))

        for value in ("", "-1", "NaN", "1.00001"):
            with self.subTest(value=value):
                environment = valid_env()
                environment["BIZPPURIO_AT_COST_KRW"] = value
                with self.assertRaisesRegex(ConfigError, "AT_COST"):
                    load_runtime_config(environment)

    def test_secret_values_are_absent_from_config_repr_and_errors(self) -> None:
        environment = valid_env()
        config = load_runtime_config(environment)
        rendered = repr(config)
        self.assertNotIn(environment["SUPABASE_SECRET_KEY"], rendered)
        self.assertNotIn(environment["BIZPPURIO_PASSWORD"], rendered)
        self.assertNotIn(environment["BIZPPURIO_FROM"], rendered)

        environment["BIZPPURIO_AT_COST_KRW"] = "secret-phone-01012345678"
        with self.assertRaises(ConfigError) as caught:
            load_runtime_config(environment)
        self.assertNotIn("01012345678", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
