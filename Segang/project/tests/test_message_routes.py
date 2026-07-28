from __future__ import annotations

from datetime import datetime, timezone
import unittest

from flask import Flask

from msg_send.config import load_runtime_config
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
    ProviderSubmission,
    PushAction,
    PushApplication,
)
from msg_send.runtime import ProductionRuntime
from Segang.project.limited_links import (
    RedeemedLink,
)
from Segang.project.message_routes import create_message_blueprint


NOW = datetime(2026, 7, 26, 12, 30, tzinfo=timezone.utc)
LEASE_UUID = "12345678-1234-4abc-8def-1234567890ab"
REFKEY = "1234567812344abc8def1234567890ab"
CALLBACK_SECRET = "callback_secret_reference_1234567890abcd"
CALLBACK_PATH = f"/callbacks/bizppurio/{CALLBACK_SECRET}"
RAW_TOKEN = "A_secure_opaque_link_token_1234567890abcd"
SESSION_ID = "limited_session_reference_1234567890abcdef"


def config_env() -> dict[str, str]:
    return {
        "SUPABASE_URL": "https://project-ref.supabase.co",
        "SUPABASE_SECRET_KEY": "sb_secret_test_only_not_a_real_key",
        "BIZPPURIO_ACCOUNT": "provider-account",
        "BIZPPURIO_PASSWORD": "provider-password",
        "BIZPPURIO_FROM": "0212345678",
        "BIZPPURIO_SENDERKEY": "sender-profile-key",
        "BIZPPURIO_AT_COST_KRW": "8.5000",
        "BIZPPURIO_SMS_COST_KRW": "12.0000",
        "MSG_SEND_CLAIM_ENABLED": "true",
        "MSG_SEND_SEND_ENABLED": "true",
        "MSG_SEND_SMS_FALLBACK_ENABLED": "true",
    }


def callback_payload() -> dict[str, object]:
    return {
        "CMSGID": "PRIVATE_PROVIDER_REQUEST_SENTINEL",
        "REFKEY": REFKEY,
        "MSGID": "PRIVATE_PROVIDER_RESULT_SENTINEL",
        "UNIXTIME": "1785038400",
        "RESULT": "7000",
        "MEDIA": "AT",
    }


def fallback_job() -> MessageJob:
    return MessageJob(
        message_id="42",
        lease_token=LEASE_UUID,
        dedupe_key="dedupe-42",
        recipient="01012345678",
        template_code="template-reference",
        template_body="private body",
        history_url="https://zxcx.io/s/history_reference",
        settings_url="https://zxcx.io/s/settings_reference",
        sms_body="private fallback body",
        attempt_count=2,
        policy=MessagePolicy(
            consent_granted=True,
            sms_fallback_approved=True,
            sms_cost_class="sms",
        ),
        delivery_channel=DeliveryChannel.SMS,
        fallback_reason="7001",
        max_attempts=1,
        lock_owner="worker-1",
    )


class CallbackDatabase:
    def __init__(self, application: PushApplication) -> None:
        self.application = application
        self.results = []
        self.fallback_rejections = 0

    def apply_push_result(self, result: object, **kwargs: object) -> PushApplication:
        del kwargs
        self.results.append(result)
        return self.application

    def reject_sms_fallback(self, *args: object, **kwargs: object) -> None:
        del args, kwargs
        self.fallback_rejections += 1


class NeverSendProvider:
    def __init__(self) -> None:
        self.calls = 0

    def submit(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> ProviderSubmission:
        del job, channel
        self.calls += 1
        raise AssertionError("callback ingress must never send")


class StubLinks:
    def __init__(
        self,
        redeemed: RedeemedLink | None,
    ) -> None:
        self.redeemed = redeemed
        self.tokens: list[str] = []

    def redeem(
        self,
        token: str,
    ) -> RedeemedLink | None:
        self.tokens.append(token)
        return self.redeemed


def build_client(
    *,
    application: PushApplication = PushApplication(PushAction.DELIVERED),
    redeemed: RedeemedLink | None = None,
) -> tuple[object, CallbackDatabase, NeverSendProvider, StubLinks]:
    database = CallbackDatabase(application)
    provider = NeverSendProvider()
    runtime = ProductionRuntime(
        config=load_runtime_config(config_env()),
        database=database,  # type: ignore[arg-type]
        provider=provider,
        clock=lambda: NOW,
    )
    links = StubLinks(redeemed)
    app = Flask(__name__)
    app.register_blueprint(
        create_message_blueprint(
            callback_runtime=runtime,
            callback_secret=CALLBACK_SECRET,
            limited_links=links,
        )
    )
    return app.test_client(), database, provider, links


class BizppurioCallbackRouteTests(unittest.TestCase):
    def test_callback_secret_rejects_non_url_safe_text(self) -> None:
        with self.assertRaises(ValueError):
            create_message_blueprint(
                callback_runtime=object(),  # type: ignore[arg-type]
                callback_secret="not/path-safe" * 4,
                limited_links=StubLinks(None),  # type: ignore[arg-type]
            )

    def test_callback_requires_url_secret_before_parsing_or_database_rpc(
        self,
    ) -> None:
        client, database, provider, _ = build_client()

        response = client.post(
            "/callbacks/bizppurio/wrong_secret_reference_1234567890abcdef",
            json=callback_payload(),
        )

        self.assertEqual(response.status_code, 404)
        self.assertEqual(database.results, [])
        self.assertEqual(provider.calls, 0)
        self.assertNotIn(
            b"PRIVATE_PROVIDER_REQUEST_SENTINEL",
            response.data,
        )

    def test_malformed_json_and_payload_shape_are_rejected_without_rpc(
        self,
    ) -> None:
        client, database, provider, _ = build_client()

        malformed = client.post(
            CALLBACK_PATH,
            data=b"{",
            content_type="application/json",
        )
        non_object = client.post(
            CALLBACK_PATH,
            json=["not", "an", "object"],
        )
        missing = client.post(
            CALLBACK_PATH,
            json={"RESULT": "7000"},
        )

        self.assertEqual(
            [malformed.status_code, non_object.status_code, missing.status_code],
            [400, 400, 400],
        )
        self.assertEqual(database.results, [])
        self.assertEqual(provider.calls, 0)

    def test_duplicate_and_unknown_results_are_sanitized_idempotent_2xx(
        self,
    ) -> None:
        for action in (
            PushAction.DUPLICATE,
            PushAction.UNKNOWN,
        ):
            with self.subTest(action=action):
                client, database, provider, _ = build_client(
                    application=PushApplication(action)
                )

                response = client.post(
                    CALLBACK_PATH,
                    json=callback_payload(),
                )

                self.assertEqual(response.status_code, 200)
                self.assertEqual(len(database.results), 1)
                self.assertEqual(provider.calls, 0)
                rendered_headers = repr(tuple(response.headers.items()))
                self.assertNotIn(
                    "PRIVATE_PROVIDER_REQUEST_SENTINEL",
                    rendered_headers,
                )
                self.assertNotIn(
                    "PRIVATE_PROVIDER_RESULT_SENTINEL",
                    rendered_headers,
                )

    def test_callback_uses_rpc_only_even_when_sms_fallback_is_enabled(
        self,
    ) -> None:
        client, database, provider, _ = build_client(
            application=PushApplication(
                PushAction.SMS_FALLBACK,
                fallback_job(),
            )
        )
        payload = callback_payload()
        payload["RESULT"] = "7001"

        response = client.post(
            CALLBACK_PATH,
            json=payload,
        )

        self.assertEqual(response.status_code, 200)
        self.assertEqual(len(database.results), 1)
        self.assertEqual(database.fallback_rejections, 1)
        self.assertEqual(provider.calls, 0)


class ScopedLinkRouteTests(unittest.TestCase):
    def test_token_is_replaced_by_secure_limited_cookie_and_clean_redirect(
        self,
    ) -> None:
        client, _, _, links = build_client(
            redeemed=RedeemedLink(
                session_id=SESSION_ID,
                target_path="/device-temp-history/42",
                max_age_seconds=900,
            )
        )

        response = client.get(f"/s/{RAW_TOKEN}")

        self.assertEqual(response.status_code, 303)
        self.assertEqual(
            response.headers["Location"],
            "/device-temp-history/42",
        )
        cookie = response.headers["Set-Cookie"]
        self.assertIn(
            f"__Host-limited_session={SESSION_ID}",
            cookie,
        )
        self.assertIn("HttpOnly", cookie)
        self.assertIn("Secure", cookie)
        self.assertIn("SameSite=Lax", cookie)
        self.assertIn("Path=/", cookie)
        self.assertIn("Max-Age=900", cookie)
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        self.assertEqual(response.headers["Referrer-Policy"], "no-referrer")
        self.assertNotIn(RAW_TOKEN, response.headers["Location"])
        self.assertNotIn(RAW_TOKEN, cookie)
        self.assertNotIn(RAW_TOKEN.encode("ascii"), response.data)
        self.assertEqual(links.tokens, [RAW_TOKEN])

    def test_rejected_token_returns_no_cookie_or_redirect_target(self) -> None:
        client, _, _, links = build_client(redeemed=None)

        response = client.get(f"/s/{RAW_TOKEN}")

        self.assertEqual(response.status_code, 404)
        self.assertNotIn("Set-Cookie", response.headers)
        self.assertNotIn("Location", response.headers)
        self.assertNotIn(RAW_TOKEN.encode("ascii"), response.data)
        self.assertEqual(links.tokens, [RAW_TOKEN])

    def test_valid_settings_token_gets_secure_cookie_and_clean_redirect(
        self,
    ) -> None:
        client, _, _, links = build_client(
            redeemed=RedeemedLink(
                session_id=SESSION_ID,
                target_path="/device-settings/42",
                max_age_seconds=900,
            )
        )

        response = client.get(f"/s/{RAW_TOKEN}")

        self.assertEqual(response.status_code, 303)
        self.assertEqual(response.headers["Location"], "/device-settings/42")
        cookie = response.headers["Set-Cookie"]
        self.assertIn(
            f"__Host-limited_session={SESSION_ID}",
            cookie,
        )
        self.assertIn("HttpOnly", cookie)
        self.assertIn("Secure", cookie)
        self.assertIn("SameSite=Lax", cookie)
        self.assertNotIn(RAW_TOKEN.encode("ascii"), response.data)
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        self.assertEqual(response.headers["Referrer-Policy"], "no-referrer")
        self.assertEqual(links.tokens, [RAW_TOKEN])


if __name__ == "__main__":
    unittest.main()
