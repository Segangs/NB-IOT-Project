from __future__ import annotations

import unittest
from datetime import datetime, timedelta, timezone
from typing import List

from msg_send.bizppurio import (
    AccessToken,
    BizppurioAdapter,
    BizppurioRequest,
    BizppurioTransportResponse,
)
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
)


class MutableClock:
    def __init__(self, now: datetime) -> None:
        self.now = now

    def __call__(self) -> datetime:
        return self.now


class FakeTransport:
    def __init__(
        self,
        tokens: List[AccessToken],
        responses: List[BizppurioTransportResponse],
    ) -> None:
        self.tokens = list(tokens)
        self.responses = list(responses)
        self.token_fetches = 0
        self.requests: List[BizppurioRequest] = []
        self.access_values: List[str] = []
        self.report_requests: List[str] = []

    def fetch_access_token(self) -> AccessToken:
        self.token_fetches += 1
        return self.tokens.pop(0)

    def submit(
        self,
        access_token: str,
        request: BizppurioRequest,
    ) -> BizppurioTransportResponse:
        self.access_values.append(access_token)
        self.requests.append(request)
        return self.responses.pop(0)

    def request_report(
        self,
        access_token: str,
        provider_request_id: str,
    ) -> None:
        self.access_values.append(access_token)
        self.report_requests.append(provider_request_id)


def make_job(
    channel: DeliveryChannel = DeliveryChannel.ALIMTALK,
) -> MessageJob:
    return MessageJob(
        message_id="message-1",
        lease_token="lease-1",
        dedupe_key="dedupe-1",
        recipient="PRIVATE_RECIPIENT_SENTINEL",
        template_code="template-reference",
        template_body="상세 #{tempHistoryToken)}",
        history_url="https://service.invalid/s/opaque-reference",
        sms_body="SMS 상세 https://service.invalid/s/opaque-reference",
        attempt_count=1,
        policy=MessagePolicy(
            consent_granted=True,
            sms_fallback_approved=True,
            sms_cost_class="transactional",
        ),
        delivery_channel=channel,
        fallback_reason=(
            "alimtalk_final_failure"
            if channel is DeliveryChannel.SMS
            else None
        ),
    )


class BizppurioAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.now = datetime(2026, 7, 26, tzinfo=timezone.utc)

    def test_adapter_emits_only_alimtalk_and_sms_requests(self) -> None:
        transport = FakeTransport(
            tokens=[
                AccessToken(
                    value="PRIVATE_ACCESS_SENTINEL",
                    expires_at=self.now + timedelta(minutes=10),
                )
            ],
            responses=[
                BizppurioTransportResponse(
                    accepted=True,
                    request_id="provider-reference-at",
                ),
                BizppurioTransportResponse(
                    accepted=True,
                    request_id="provider-reference-sms",
                ),
            ],
        )
        adapter = BizppurioAdapter(transport, clock=lambda: self.now)

        alimtalk = adapter.submit(
            make_job(),
            DeliveryChannel.ALIMTALK,
        )
        sms = adapter.submit(
            make_job(DeliveryChannel.SMS),
            DeliveryChannel.SMS,
        )

        self.assertTrue(alimtalk.accepted)
        self.assertTrue(sms.accepted)
        self.assertEqual(
            [request.channel_code for request in transport.requests],
            ["at", "sms"],
        )
        self.assertEqual(
            [request.message_key for request in transport.requests],
            ["lease-1", "lease-1"],
        )
        self.assertEqual(
            transport.requests[0].body,
            "상세 https://service.invalid/s/opaque-reference",
        )
        self.assertNotIn(
            "opaque-reference)",
            transport.requests[0].body,
        )
        self.assertEqual(
            transport.requests[0].template_code,
            "template-reference",
        )
        self.assertIsNone(transport.requests[1].template_code)

        with self.assertRaises(ValueError):
            adapter.submit(make_job(), "friendtalk")  # type: ignore[arg-type]
        self.assertEqual(len(transport.requests), 2)

    def test_token_is_cached_then_refreshed_before_expiry(self) -> None:
        clock = MutableClock(self.now)
        transport = FakeTransport(
            tokens=[
                AccessToken(
                    value="PRIVATE_ACCESS_A",
                    expires_at=self.now + timedelta(seconds=120),
                ),
                AccessToken(
                    value="PRIVATE_ACCESS_B",
                    expires_at=self.now + timedelta(minutes=10),
                ),
            ],
            responses=[
                BizppurioTransportResponse(
                    accepted=True,
                    request_id="provider-reference-1",
                ),
                BizppurioTransportResponse(
                    accepted=True,
                    request_id="provider-reference-2",
                ),
                BizppurioTransportResponse(
                    accepted=True,
                    request_id="provider-reference-3",
                ),
            ],
        )
        adapter = BizppurioAdapter(
            transport,
            clock=clock,
            refresh_skew_seconds=30,
        )

        adapter.submit(make_job(), DeliveryChannel.ALIMTALK)
        clock.now = self.now + timedelta(seconds=60)
        adapter.submit(make_job(), DeliveryChannel.ALIMTALK)
        clock.now = self.now + timedelta(seconds=91)
        adapter.submit(make_job(), DeliveryChannel.ALIMTALK)

        self.assertEqual(transport.token_fetches, 2)
        self.assertEqual(
            transport.access_values,
            [
                "PRIVATE_ACCESS_A",
                "PRIVATE_ACCESS_A",
                "PRIVATE_ACCESS_B",
            ],
        )

    def test_auth_expiry_forces_one_token_refresh_and_resubmit(self) -> None:
        transport = FakeTransport(
            tokens=[
                AccessToken(
                    value="PRIVATE_ACCESS_OLD",
                    expires_at=self.now + timedelta(minutes=10),
                ),
                AccessToken(
                    value="PRIVATE_ACCESS_NEW",
                    expires_at=self.now + timedelta(minutes=10),
                ),
            ],
            responses=[
                BizppurioTransportResponse(
                    accepted=False,
                    auth_expired=True,
                    retryable=True,
                    failure_code="auth_expired",
                ),
                BizppurioTransportResponse(
                    accepted=True,
                    request_id="provider-reference",
                ),
            ],
        )
        adapter = BizppurioAdapter(transport, clock=lambda: self.now)

        result = adapter.submit(
            make_job(),
            DeliveryChannel.ALIMTALK,
        )

        self.assertTrue(result.accepted)
        self.assertEqual(transport.token_fetches, 2)
        self.assertEqual(len(transport.requests), 2)
        self.assertEqual(
            transport.access_values,
            ["PRIVATE_ACCESS_OLD", "PRIVATE_ACCESS_NEW"],
        )

    def test_malformed_accepted_response_is_not_treated_as_delivery(self) -> None:
        transport = FakeTransport(
            tokens=[
                AccessToken(
                    value="PRIVATE_ACCESS_SENTINEL",
                    expires_at=self.now + timedelta(minutes=10),
                )
            ],
            responses=[
                BizppurioTransportResponse(
                    accepted=True,
                    request_id=None,
                )
            ],
        )
        adapter = BizppurioAdapter(transport, clock=lambda: self.now)

        result = adapter.submit(
            make_job(),
            DeliveryChannel.ALIMTALK,
        )

        self.assertFalse(result.accepted)
        self.assertFalse(result.retryable)
        self.assertEqual(
            result.failure_code,
            "invalid_provider_response",
        )

    def test_second_auth_expiry_returns_bounded_retryable_failure(self) -> None:
        transport = FakeTransport(
            tokens=[
                AccessToken(
                    value="PRIVATE_ACCESS_A",
                    expires_at=self.now + timedelta(minutes=10),
                ),
                AccessToken(
                    value="PRIVATE_ACCESS_B",
                    expires_at=self.now + timedelta(minutes=10),
                ),
            ],
            responses=[
                BizppurioTransportResponse(
                    accepted=False,
                    auth_expired=True,
                ),
                BizppurioTransportResponse(
                    accepted=False,
                    auth_expired=True,
                ),
            ],
        )
        adapter = BizppurioAdapter(transport, clock=lambda: self.now)

        result = adapter.submit(
            make_job(),
            DeliveryChannel.ALIMTALK,
        )

        self.assertFalse(result.accepted)
        self.assertTrue(result.retryable)
        self.assertEqual(result.failure_code, "provider_auth_unavailable")
        self.assertEqual(transport.token_fetches, 2)
        self.assertEqual(len(transport.requests), 2)

    def test_report_reconciliation_uses_token_without_message_submit(
        self,
    ) -> None:
        transport = FakeTransport(
            tokens=[
                AccessToken(
                    value="PRIVATE_ACCESS_SENTINEL",
                    expires_at=self.now + timedelta(minutes=10),
                )
            ],
            responses=[],
        )
        adapter = BizppurioAdapter(transport, clock=lambda: self.now)

        adapter.request_report("provider-reference-1")

        self.assertEqual(transport.token_fetches, 1)
        self.assertEqual(
            transport.report_requests,
            ["provider-reference-1"],
        )
        self.assertEqual(transport.requests, [])


if __name__ == "__main__":
    unittest.main()
