from __future__ import annotations

import base64
from dataclasses import replace
from datetime import datetime, timedelta, timezone
import json
import unittest
from urllib.parse import urlsplit

from msg_send.bizppurio import (
    AccessToken,
    BizppurioAdapter,
    BizppurioButton,
    BizppurioRequest,
)
from msg_send.bizppurio_http import (
    BizppurioHttpError,
    BizppurioHttpTransport,
)
from msg_send.catalog import TemplateCatalog
from msg_send.config import BizppurioConfig
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
    ProviderSubmissionNotAttempted,
    ProviderSubmissionOutcomeUnknown,
)
from msg_send.http_transport import HttpRequest, HttpResponse


LEASE_UUID = "12345678-1234-4abc-8def-1234567890ab"
REFKEY = "1234567812344abc8def1234567890ab"


class RecordingTransport:
    def __init__(
        self,
        responses: list[HttpResponse | Exception],
    ) -> None:
        self.responses = list(responses)
        self.requests: list[HttpRequest] = []

    def request(self, request: HttpRequest) -> HttpResponse:
        self.requests.append(request)
        if not self.responses:
            raise AssertionError("unexpected HTTP request")
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response


def response(payload: object, status: int = 200) -> HttpResponse:
    return HttpResponse(
        status=status,
        headers={"content-type": "application/json"},
        body=json.dumps(payload).encode("utf-8"),
    )


def provider_config() -> BizppurioConfig:
    return BizppurioConfig(
        base_url="https://api.bizppurio.com",
        account="provider-account",
        password="provider-password",
        from_number="0212345678",
        sender_key="sender-profile-key",
        timeout_seconds=8.0,
    )


def production_request() -> BizppurioRequest:
    return BizppurioRequest(
        message_key=LEASE_UUID,
        recipient="01012345678",
        channel_code="at",
        body=(
            "기기가 정상적으로 서버와 연결을 시작했습니다.\n\n"
            "- 사업장명 : 서울 1호점\n"
            "- 기기명 : 냉동고 A\n"
            "- 부팅시간 : 2026-07-26 12:30"
        ),
        template_code="bizp_2026071315003676625784727",
        title="기기 켜짐 알림",
        buttons=(
            BizppurioButton(
                "온도 이력 확인",
                "https://zxcx.io/s/history_token_123456",
            ),
            BizppurioButton(
                "설정 변경",
                "https://zxcx.io/s/settings_token_123456",
            ),
        ),
    )


class BizppurioHttpTests(unittest.TestCase):
    def test_request_and_job_repr_redact_phone_body_and_access_token(
        self,
    ) -> None:
        request_repr = repr(production_request())
        self.assertNotIn("01012345678", request_repr)
        self.assertNotIn("서울 1호점", request_repr)

        token_repr = repr(
            AccessToken(
                "private-access-token",
                datetime(
                    2026,
                    7,
                    26,
                    13,
                    0,
                    tzinfo=timezone.utc,
                ),
            )
        )
        self.assertNotIn("private-access-token", token_repr)

    def test_token_request_uses_basic_auth_close_and_timeout(self) -> None:
        recording = RecordingTransport(
            [
                response(
                    {
                        "accesstoken": "access-token-value",
                        "expired": "2026-07-26T13:00:00+00:00",
                    }
                )
            ]
        )
        transport = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        )
        token = transport.fetch_access_token()

        self.assertEqual(token.value, "access-token-value")
        self.assertEqual(
            token.expires_at,
            datetime(2026, 7, 26, 13, 0, tzinfo=timezone.utc),
        )
        request = recording.requests[0]
        self.assertEqual(urlsplit(request.url).path, "/v1/token")
        self.assertEqual(request.method, "POST")
        expected_basic = base64.b64encode(
            b"provider-account:provider-password"
        ).decode("ascii")
        self.assertEqual(
            request.headers["Authorization"],
            f"Basic {expected_basic}",
        )
        self.assertEqual(request.headers["Connection"], "close")
        self.assertEqual(request.timeout_seconds, 8.0)

    def test_compact_provider_expiry_uses_korea_fixed_offset(self) -> None:
        recording = RecordingTransport(
            [
                response(
                    {
                        "accesstoken": "access-token-value",
                        "expired": "20260726130000",
                    }
                )
            ]
        )
        token = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        ).fetch_access_token()
        self.assertEqual(
            token.expires_at,
            datetime(
                2026,
                7,
                26,
                13,
                0,
                tzinfo=timezone(timedelta(hours=9)),
            ),
        )

    def test_exact_at_request_json_uses_title_buttons_and_hex_refkey(
        self,
    ) -> None:
        recording = RecordingTransport(
            [
                response(
                    {
                        "code": 1000,
                        "messagekey": "provider-request-1",
                        "refkey": REFKEY,
                    }
                )
            ]
        )
        transport = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        )
        result = transport.submit(
            "access-token-value",
            production_request(),
        )

        self.assertTrue(result.accepted)
        self.assertEqual(result.request_id, "provider-request-1")
        request = recording.requests[0]
        self.assertEqual(urlsplit(request.url).path, "/v3/message")
        self.assertEqual(request.headers["Authorization"], "Bearer access-token-value")
        self.assertEqual(request.headers["Connection"], "close")
        self.assertEqual(
            json.loads(request.body.decode("utf-8")),
            {
                "account": "provider-account",
                "refkey": REFKEY,
                "type": "at",
                "from": "0212345678",
                "to": "01012345678",
                "content": {
                    "at": {
                        "senderkey": "sender-profile-key",
                        "templatecode":
                            "bizp_2026071315003676625784727",
                        "message": (
                            "기기가 정상적으로 서버와 연결을 시작했습니다.\n\n"
                            "- 사업장명 : 서울 1호점\n"
                            "- 기기명 : 냉동고 A\n"
                            "- 부팅시간 : 2026-07-26 12:30"
                        ),
                        "title": "기기 켜짐 알림",
                        "button": [
                            {
                                "name": "온도 이력 확인",
                                "type": "WL",
                                "url_mobile":
                                    "https://zxcx.io/s/history_token_123456",
                                "url_pc":
                                    "https://zxcx.io/s/history_token_123456",
                            },
                            {
                                "name": "설정 변경",
                                "type": "WL",
                                "url_mobile":
                                    "https://zxcx.io/s/settings_token_123456",
                                "url_pc":
                                    "https://zxcx.io/s/settings_token_123456",
                            },
                        ],
                    }
                },
            },
        )

    def test_only_code_1000_with_messagekey_is_accepted(self) -> None:
        recording = RecordingTransport(
            [
                response({"code": 1001, "messagekey": "not-accepted"}),
                response({"code": 1000}),
            ]
        )
        transport = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        )
        rejected = transport.submit("token", production_request())
        self.assertFalse(rejected.accepted)
        self.assertEqual(rejected.failure_code, "provider_code_1001")
        with self.assertRaises(BizppurioHttpError):
            transport.submit("token", production_request())

    def test_success_code_rejects_lossy_or_noncanonical_values(self) -> None:
        for invalid_code in (1000.9, " 1000 ", "01000", True):
            with self.subTest(code=invalid_code):
                recording = RecordingTransport(
                    [
                        response(
                            {
                                "code": invalid_code,
                                "messagekey": "provider-request-1",
                            }
                        )
                    ]
                )
                transport = BizppurioHttpTransport(
                    provider_config(),
                    transport=recording,
                )
                with self.assertRaises(BizppurioHttpError):
                    transport.submit("token", production_request())

    def test_canonical_string_success_code_is_accepted(self) -> None:
        recording = RecordingTransport(
            [
                response(
                    {
                        "code": "1000",
                        "messagekey": "provider-request-1",
                    }
                )
            ]
        )
        result = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        ).submit("token", production_request())
        self.assertTrue(result.accepted)

    def test_sms_request_uses_sms_content_without_alimtalk_fields(
        self,
    ) -> None:
        recording = RecordingTransport(
            [
                response(
                    {
                        "code": 1000,
                        "messagekey": "provider-request-sms",
                        "refkey": REFKEY,
                    }
                )
            ]
        )
        transport = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        )
        result = transport.submit(
            "access-token-value",
            BizppurioRequest(
                message_key=LEASE_UUID,
                recipient="01012345678",
                channel_code="sms",
                body="SMS fallback body",
                template_code=None,
            ),
        )
        self.assertTrue(result.accepted)
        self.assertEqual(
            json.loads(recording.requests[0].body.decode("utf-8")),
            {
                "account": "provider-account",
                "refkey": REFKEY,
                "type": "sms",
                "from": "0212345678",
                "to": "01012345678",
                "content": {
                    "sms": {"message": "SMS fallback body"}
                },
            },
        )

    def test_report_request_is_non_send_and_never_calls_message_path(
        self,
    ) -> None:
        recording = RecordingTransport([response({"code": 1000})])
        transport = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        )
        transport.request_report(
            "access-token-value",
            "provider-request-1",
        )

        self.assertEqual(len(recording.requests), 1)
        self.assertEqual(
            urlsplit(recording.requests[0].url).path,
            "/v2/report",
        )
        self.assertNotIn(
            "/v3/message",
            recording.requests[0].url,
        )
        self.assertEqual(
            json.loads(recording.requests[0].body.decode("utf-8")),
            {
                "account": "provider-account",
                "messagekey": "provider-request-1",
            },
        )

    def test_errors_never_include_secret_phone_or_response_body(self) -> None:
        recording = RecordingTransport(
            [
                response(
                    {
                        "message":
                            "provider-password 01012345678 leaked-body",
                    },
                    status=500,
                )
            ]
        )
        transport = BizppurioHttpTransport(
            provider_config(),
            transport=recording,
        )
        with self.assertRaises(BizppurioHttpError) as caught:
            transport.fetch_access_token()
        rendered = str(caught.exception)
        self.assertNotIn("provider-password", rendered)
        self.assertNotIn("01012345678", rendered)
        self.assertNotIn("leaked-body", rendered)

    def test_token_and_pre_submit_errors_are_not_attempted_but_response_loss_is_ambiguous(
        self,
    ) -> None:
        job = MessageJob(
            message_id="42",
            lease_token=LEASE_UUID,
            dedupe_key="dedupe-42",
            recipient="01012345678",
            template_code="bizp_2026071315003676625784727",
            template_body="unused-production-source",
            history_url="https://zxcx.io/s/history_token_123456",
            settings_url="https://zxcx.io/s/settings_token_123456",
            sms_body="fallback disabled",
            attempt_count=1,
            policy=MessagePolicy(
                consent_granted=True,
                sms_fallback_approved=False,
            ),
            delivery_channel=DeliveryChannel.ALIMTALK,
            fallback_reason=None,
            template_params={
                "workplaceName": "서울 1호점",
                "userMachineName": "냉동고 A",
                "eventTime": "2026-07-26 12:30",
            },
            max_attempts=1,
        )

        token_failure = RecordingTransport([OSError("token failure")])
        adapter = BizppurioAdapter(
            BizppurioHttpTransport(
                provider_config(),
                transport=token_failure,
            ),
            clock=lambda: datetime(
                2026,
                7,
                26,
                12,
                0,
                tzinfo=timezone.utc,
            ),
            catalog=TemplateCatalog.load_approved(),
        )
        with self.assertRaises(ProviderSubmissionNotAttempted):
            adapter.submit(job, DeliveryChannel.ALIMTALK)
        self.assertEqual(
            [urlsplit(item.url).path for item in token_failure.requests],
            ["/v1/token"],
        )

        submit_loss = RecordingTransport(
            [
                response(
                    {
                        "accesstoken": "access-token-value",
                        "expired": "2026-07-26T13:00:00+00:00",
                    }
                ),
                OSError("response lost"),
            ]
        )
        adapter = BizppurioAdapter(
            BizppurioHttpTransport(
                provider_config(),
                transport=submit_loss,
            ),
            clock=lambda: datetime(
                2026,
                7,
                26,
                12,
                0,
                tzinfo=timezone.utc,
            ),
            catalog=TemplateCatalog.load_approved(),
        )
        with self.assertRaises(ProviderSubmissionOutcomeUnknown):
            adapter.submit(job, DeliveryChannel.ALIMTALK)
        self.assertEqual(
            [urlsplit(item.url).path for item in submit_loss.requests],
            ["/v1/token", "/v3/message"],
        )

    def test_invalid_lease_correlation_fails_before_token_or_message_http(
        self,
    ) -> None:
        recording = RecordingTransport([])
        adapter = BizppurioAdapter(
            BizppurioHttpTransport(
                provider_config(),
                transport=recording,
            ),
            clock=lambda: datetime(
                2026,
                7,
                26,
                12,
                0,
                tzinfo=timezone.utc,
            ),
            catalog=TemplateCatalog.load_approved(),
        )
        with self.assertRaises(ProviderSubmissionNotAttempted):
            adapter.submit(
                replace(
                    MessageJob(
                        message_id="42",
                        lease_token=LEASE_UUID,
                        dedupe_key="dedupe-42",
                        recipient="01012345678",
                        template_code=
                            "bizp_2026071315003676625784727",
                        template_body="unused",
                        history_url=
                            "https://zxcx.io/s/history_token_123456",
                        settings_url=
                            "https://zxcx.io/s/settings_token_123456",
                        sms_body="fallback disabled",
                        attempt_count=1,
                        policy=MessagePolicy(True, False),
                        delivery_channel=DeliveryChannel.ALIMTALK,
                        fallback_reason=None,
                        template_params={
                            "workplaceName": "서울 1호점",
                            "userMachineName": "냉동고 A",
                            "eventTime": "2026-07-26 12:30",
                        },
                    ),
                    lease_token="not-a-uuid",
                ),
                DeliveryChannel.ALIMTALK,
            )
        self.assertEqual(recording.requests, [])

    def test_production_auth_rejection_never_retries_message_http_inline(
        self,
    ) -> None:
        recording = RecordingTransport(
            [
                response(
                    {
                        "accesstoken": "access-token-value",
                        "expired": "2026-07-26T13:00:00+00:00",
                    }
                ),
                response({"code": 401}, status=401),
            ]
        )
        adapter = BizppurioAdapter(
            BizppurioHttpTransport(
                provider_config(),
                transport=recording,
            ),
            clock=lambda: datetime(
                2026,
                7,
                26,
                12,
                0,
                tzinfo=timezone.utc,
            ),
            catalog=TemplateCatalog.load_approved(),
            resubmit_after_auth_expiry=False,
        )
        job = MessageJob(
            message_id="42",
            lease_token=LEASE_UUID,
            dedupe_key="dedupe-42",
            recipient="01012345678",
            template_code="bizp_2026071315003676625784727",
            template_body="unused",
            history_url="https://zxcx.io/s/history_token_123456",
            settings_url="https://zxcx.io/s/settings_token_123456",
            sms_body="fallback disabled",
            attempt_count=1,
            policy=MessagePolicy(True, False),
            delivery_channel=DeliveryChannel.ALIMTALK,
            fallback_reason=None,
            template_params={
                "workplaceName": "서울 1호점",
                "userMachineName": "냉동고 A",
                "eventTime": "2026-07-26 12:30",
            },
        )
        result = adapter.submit(job, DeliveryChannel.ALIMTALK)
        self.assertFalse(result.accepted)
        self.assertTrue(result.retryable)
        self.assertEqual(
            [urlsplit(request.url).path for request in recording.requests],
            ["/v1/token", "/v3/message"],
        )


if __name__ == "__main__":
    unittest.main()
