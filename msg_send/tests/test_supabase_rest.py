from __future__ import annotations

from datetime import datetime, timezone
from decimal import Decimal
import json
import unittest
from urllib.parse import parse_qs, urlsplit

from msg_send.catalog import TemplateCatalog
from msg_send.config import SupabaseConfig
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
    MessageStatus,
    ProviderPushResult,
    PushAction,
)
from msg_send.http_transport import HttpRequest, HttpResponse
from msg_send.supabase_rest import (
    SupabaseRestDatabase,
    SupabaseRestError,
)


LEASE_UUID = "12345678-1234-4abc-8def-1234567890ab"
NOW = datetime(2026, 7, 26, 12, 30, tzinfo=timezone.utc)


class RecordingTransport:
    def __init__(self, responses: list[HttpResponse]) -> None:
        self.responses = list(responses)
        self.requests: list[HttpRequest] = []

    def request(self, request: HttpRequest) -> HttpResponse:
        self.requests.append(request)
        if not self.responses:
            raise AssertionError("unexpected HTTP request")
        return self.responses.pop(0)


def response(payload: object, status: int = 200) -> HttpResponse:
    return HttpResponse(
        status=status,
        headers={"content-type": "application/json"},
        body=json.dumps(payload).encode("utf-8"),
    )


def claimed_row() -> dict[str, object]:
    return {
        "msg_send_id": 42,
        "alert_contact_id": 7,
        "message_policy_id": 9,
        "lease_token": LEASE_UUID,
        "dedupe_key": "dedupe-42",
        "template_code": "bizp_2026071315003676625784727",
        "template_params": {
            "workplaceName": "서울 1호점",
            "userMachineName": "냉동고 A",
            "eventTime": "2026-07-26 12:30",
            "tempHistoryToken": "history_token_123456",
            "SettingsToken": "settings_token_123456",
        },
        "attempt_count": 1,
        "max_attempts": 1,
        "active_channel": "alimtalk",
        "fallback_reason": None,
    }


def job_for_rpc() -> MessageJob:
    return MessageJob(
        message_id="42",
        lease_token=LEASE_UUID,
        dedupe_key="dedupe-42",
        recipient="01012345678",
        template_code="bizp_2026071315003676625784727",
        template_body="source template body",
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
        lock_owner="worker-1",
    )


def client_with(
    responses: list[HttpResponse],
) -> tuple[SupabaseRestDatabase, RecordingTransport]:
    transport = RecordingTransport(responses)
    client = SupabaseRestDatabase(
        SupabaseConfig(
            url="https://project-ref.supabase.co",
            secret_key="sb_secret_test_only_not_a_real_key",
            timeout_seconds=7.5,
        ),
        TemplateCatalog.load_approved(),
        transport=transport,
    )
    return client, transport


class SupabaseRestContractTests(unittest.TestCase):
    def test_temperature_outbox_drain_rpc_is_bounded_and_compact(self) -> None:
        client, transport = client_with(
            [
                response(
                    [
                        {
                            "temperature_alert_outbox_id": 7,
                            "msg_send_id": 42,
                            "result": "enqueued",
                        },
                        {
                            "temperature_alert_outbox_id": 8,
                            "msg_send_id": None,
                            "result": "contact_cardinality",
                        },
                    ]
                )
            ]
        )

        self.assertEqual(
            client.drain_temperature_alert_outbox(16),
            ("42",),
        )
        request = transport.requests[0]
        self.assertEqual(
            urlsplit(request.url).path,
            "/rest/v1/rpc/drain_temperature_alert_outbox",
        )
        self.assertEqual(
            json.loads(request.body.decode("utf-8")),
            {"p_limit": 16},
        )
        with self.assertRaises(ValueError):
            client.drain_temperature_alert_outbox(0)
        with self.assertRaises(ValueError):
            client.drain_temperature_alert_outbox(33)

    def test_claim_rpc_hydrates_contact_policy_template_and_links(self) -> None:
        client, transport = client_with(
            [
                response([claimed_row()]),
                response(
                    [
                        {
                            "alert_contact_id": 7,
                            "destination_e164": "+821012345678",
                            "consent_status": "granted",
                            "is_active": True,
                        }
                    ]
                ),
                response(
                    [
                        {
                            "message_policy_id": 9,
                            "allow_sms_fallback": False,
                            "fallback_cost_class": None,
                            "is_active": True,
                        }
                    ]
                ),
            ]
        )

        jobs = client.claim_messages(
            worker_id="worker-1",
            now=NOW,
            lease_seconds=60,
            limit=1,
        )

        self.assertEqual(len(jobs), 1)
        job = jobs[0]
        self.assertEqual(job.message_id, "42")
        self.assertEqual(job.recipient, "01012345678")
        self.assertEqual(
            job.history_url,
            "https://zxcx.io/s/history_token_123456",
        )
        self.assertEqual(
            job.settings_url,
            "https://zxcx.io/s/settings_token_123456",
        )
        self.assertEqual(
            job.template_params,
            {
                "workplaceName": "서울 1호점",
                "userMachineName": "냉동고 A",
                "eventTime": "2026-07-26 12:30",
            },
        )
        self.assertFalse(job.policy.sms_fallback_approved)
        self.assertEqual(job.max_attempts, 1)

        rpc = transport.requests[0]
        self.assertEqual(
            urlsplit(rpc.url).path,
            "/rest/v1/rpc/claim_msg_send",
        )
        self.assertEqual(
            json.loads(rpc.body.decode("utf-8")),
            {
                "p_batch_size": 1,
                "p_lease_seconds": 60,
                "p_lock_owner": "worker-1",
            },
        )
        self.assertEqual(
            urlsplit(transport.requests[1].url).path,
            "/rest/v1/alert_contact",
        )
        self.assertEqual(
            urlsplit(transport.requests[2].url).path,
            "/rest/v1/message_policy",
        )

    def test_secret_key_uses_apikey_header_only_for_rpc_and_tables(
        self,
    ) -> None:
        client, transport = client_with(
            [
                response([]),
                response(
                    [
                        {
                            "userId": 1,
                            "userPhoneNumber": "010-1234-5678",
                        }
                    ]
                ),
            ]
        )
        client.claim_messages("worker-1", NOW, 60, 1)
        self.assertEqual(client.lookup_admin_phone(), "01012345678")

        for request in transport.requests:
            lowered = {key.lower(): value for key, value in request.headers.items()}
            self.assertEqual(
                lowered["apikey"],
                "sb_secret_test_only_not_a_real_key",
            )
            self.assertNotIn("authorization", lowered)
            self.assertEqual(lowered["connection"], "close")
            self.assertEqual(request.timeout_seconds, 7.5)

        admin_query = parse_qs(urlsplit(transport.requests[-1].url).query)
        self.assertEqual(admin_query["select"], ["userId,userPhoneNumber"])
        self.assertEqual(admin_query["userAccountId"], ["eq.admin"])
        self.assertEqual(admin_query["limit"], ["2"])

    def test_current_six_rpc_argument_contracts_are_exact(self) -> None:
        client, transport = client_with(
            [
                response({"status": "accepted"}),
                response(
                    [
                        {
                            "applied": True,
                            "duplicate": False,
                            "message": {"status": "waiting_result"},
                        }
                    ]
                ),
                response({"status": "failed"}),
                response(1),
                response(
                    [
                        {
                            "applied": False,
                            "duplicate": True,
                            "action": "duplicate",
                            "message": None,
                        }
                    ]
                ),
            ]
        )
        job = job_for_rpc()
        client.mark_submission_started(job, DeliveryChannel.ALIMTALK)
        registration = client.mark_submission_waiting_result(
            job,
            "provider-request-1",
            DeliveryChannel.ALIMTALK,
            NOW,
        )
        self.assertTrue(registration.applied)
        self.assertIs(registration.current_status, MessageStatus.WAITING_RESULT)
        client.mark_failed(job, "terminal-test", NOW)
        self.assertEqual(
            client.recover_expired_leases(
                now=NOW,
                limit=64,
                max_attempts=5,
                base_delay_seconds=30,
                max_delay_seconds=3600,
                waiting_result_timeout_seconds=3600,
            ),
            1,
        )
        action = client.apply_push_result(
            ProviderPushResult(
                result_id="provider-result-1",
                request_id="provider-request-1",
                submission_token=LEASE_UUID,
                channel=DeliveryChannel.ALIMTALK,
                delivered=True,
                retryable=False,
                failure_code=None,
                result_at=NOW,
                cost_amount=Decimal("8.5000"),
            ),
            worker_id="worker-1",
            now=NOW,
            lease_seconds=60,
            max_attempts=5,
            base_delay_seconds=30,
            max_delay_seconds=3600,
        )
        self.assertIs(action.action, PushAction.DUPLICATE)

        calls = {
            urlsplit(request.url).path.rsplit("/", 1)[-1]:
            json.loads(request.body.decode("utf-8"))
            for request in transport.requests
        }
        self.assertEqual(
            calls["mark_msg_send_submission_started"],
            {
                "p_channel": "alimtalk",
                "p_lease_token": LEASE_UUID,
                "p_lock_owner": "worker-1",
                "p_msg_send_id": 42,
            },
        )
        self.assertNotIn(
            "p_started_at",
            calls["mark_msg_send_submission_started"],
        )
        self.assertEqual(
            set(calls["mark_msg_send_submission_waiting_result"]),
            {
                "p_accepted_at",
                "p_channel",
                "p_lease_token",
                "p_lock_owner",
                "p_msg_send_id",
                "p_provider_request_id",
            },
        )
        self.assertEqual(
            set(calls["complete_msg_send_claim"]),
            {
                "p_action",
                "p_available_at",
                "p_error_code",
                "p_lease_token",
                "p_lock_owner",
                "p_msg_send_id",
            },
        )
        self.assertEqual(
            set(calls["recover_msg_send_leases"]),
            {
                "p_batch_size",
                "p_max_attempts",
                "p_retry_base_seconds",
                "p_retry_max_seconds",
                "p_waiting_result_timeout_seconds",
            },
        )
        self.assertEqual(
            set(calls["record_msg_send_push_result"]),
            {
                "p_channel",
                "p_cost_amount",
                "p_delivered",
                "p_lease_seconds",
                "p_lock_owner",
                "p_max_attempts",
                "p_provider_request_id",
                "p_provider_result_id",
                "p_result_at",
                "p_result_code",
                "p_retry_base_seconds",
                "p_retry_max_seconds",
                "p_retryable",
                "p_submission_lease_token",
            },
        )

    def test_provider_acceptance_success_rpc_contract_is_exact(self) -> None:
        client, transport = client_with(
            [
                response(
                    [
                        {
                            "applied": True,
                            "duplicate": False,
                            "message": {"status": "sent"},
                        }
                    ]
                )
            ]
        )

        registration = client.mark_submission_accepted_success(
            job_for_rpc(),
            "provider-request-1",
            DeliveryChannel.ALIMTALK,
            NOW,
            Decimal("8.5000"),
        )

        self.assertTrue(registration.applied)
        self.assertFalse(registration.duplicate)
        self.assertIs(registration.current_status, MessageStatus.SENT)
        request = transport.requests[0]
        self.assertEqual(
            urlsplit(request.url).path,
            "/rest/v1/rpc/finalize_msg_send_provider_acceptance",
        )
        self.assertEqual(
            json.loads(request.body.decode("utf-8")),
            {
                "p_accepted_at": "2026-07-26T12:30:00+00:00",
                "p_channel": "alimtalk",
                "p_cost_amount": "8.5000",
                "p_lease_token": LEASE_UUID,
                "p_lock_owner": "worker-1",
                "p_msg_send_id": 42,
                "p_provider_request_id": "provider-request-1",
            },
        )

    def test_health_query_includes_unregistered_accepted_ambiguity(self) -> None:
        client, transport = client_with(
            [
                response(
                    [
                        {
                            "status": "accepted",
                            "active_channel": "alimtalk",
                            "provider_submission_started_at":
                                "2026-07-26T12:00:00+00:00",
                            "fallback_submission_started_at": None,
                            "provider_accepted_at": None,
                            "fallback_provider_accepted_at": None,
                        },
                        {
                            "status": "waiting_result",
                            "active_channel": "alimtalk",
                            "provider_submission_started_at":
                                "2026-07-26T12:10:00+00:00",
                            "fallback_submission_started_at": None,
                            "provider_accepted_at":
                                "2026-07-26T12:11:00+00:00",
                            "fallback_provider_accepted_at": None,
                        },
                    ]
                )
            ]
        )
        oldest = client.oldest_waiting_result_at()
        self.assertEqual(
            oldest,
            datetime(2026, 7, 26, 12, 0, tzinfo=timezone.utc),
        )
        query = parse_qs(urlsplit(transport.requests[0].url).query)
        self.assertEqual(
            query["status"],
            ["in.(accepted,waiting_result)"],
        )

    def test_one_shot_claim_uses_exact_atomic_rpc_and_hydrates_result(
        self,
    ) -> None:
        client, transport = client_with(
            [
                response([claimed_row()]),
                response(
                    [
                        {
                            "alert_contact_id": 7,
                            "destination_e164": "+821012345678",
                            "consent_status": "granted",
                            "is_active": True,
                        }
                    ]
                ),
                response(
                    [
                        {
                            "message_policy_id": 9,
                            "allow_sms_fallback": False,
                            "fallback_cost_class": None,
                            "is_active": True,
                        }
                    ]
                ),
            ]
        )
        jobs = client.claim_exact_one_shot(
            expected_message_id="42",
            worker_id="worker-1",
            now=NOW,
            lease_seconds=60,
        )
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0].message_id, "42")
        self.assertEqual(jobs[0].attempt_count, 1)
        self.assertEqual(jobs[0].max_attempts, 1)
        self.assertFalse(jobs[0].policy.sms_fallback_approved)

        self.assertEqual(
            urlsplit(transport.requests[0].url).path,
            "/rest/v1/rpc/claim_exact_one_shot_msg_send",
        )
        self.assertEqual(
            json.loads(transport.requests[0].body.decode("utf-8")),
            {
                "p_expected_msg_send_id": 42,
                "p_lease_seconds": 60,
                "p_lock_owner": "worker-1",
            },
        )
        self.assertEqual(
            urlsplit(transport.requests[1].url).path,
            "/rest/v1/alert_contact",
        )
        self.assertEqual(
            urlsplit(transport.requests[2].url).path,
            "/rest/v1/message_policy",
        )

    def test_admin_lookup_ambiguity_and_http_errors_hide_private_data(
        self,
    ) -> None:
        client, _ = client_with(
            [
                response(
                    [
                        {"userId": 1, "userPhoneNumber": "01012345678"},
                        {"userId": 2, "userPhoneNumber": "01099998888"},
                    ]
                )
            ]
        )
        with self.assertRaises(SupabaseRestError) as ambiguity:
            client.lookup_admin_phone()
        self.assertNotIn("01012345678", str(ambiguity.exception))
        self.assertNotIn("01099998888", str(ambiguity.exception))

        client, _ = client_with(
            [
                response(
                    {
                        "message": "sb_secret_leaked 01012345678",
                    },
                    status=400,
                )
            ]
        )
        with self.assertRaises(SupabaseRestError) as http_error:
            client.lookup_admin_phone()
        self.assertNotIn("sb_secret_leaked", str(http_error.exception))
        self.assertNotIn("01012345678", str(http_error.exception))

    def test_callback_missing_lookup_requires_exact_waiting_result_identity(
        self,
    ) -> None:
        client, transport = client_with(
            [
                response(
                    [
                        {
                            "status": "waiting_result",
                            "active_channel": "alimtalk",
                            "provider_request_id": "provider-request-1",
                            "provider_result_id": None,
                        }
                    ]
                )
            ]
        )

        self.assertTrue(
            client.is_callback_missing("provider-request-1")
        )
        request = transport.requests[0]
        query = parse_qs(urlsplit(request.url).query)
        self.assertEqual(
            urlsplit(request.url).path,
            "/rest/v1/msg_send",
        )
        self.assertEqual(
            query["provider_request_id"],
            ["eq.provider-request-1"],
        )
        self.assertEqual(query["limit"], ["2"])

        client, transport = client_with(
            [
                response([]),
                response(
                    [
                        {
                            "status": "waiting_result",
                            "active_channel": "sms",
                            "fallback_provider_request_id":
                                "provider-request-sms",
                            "fallback_provider_result_id": None,
                        }
                    ]
                ),
            ]
        )
        self.assertTrue(
            client.is_callback_missing("provider-request-sms")
        )
        fallback_query = parse_qs(
            urlsplit(transport.requests[1].url).query
        )
        self.assertEqual(
            fallback_query["fallback_provider_request_id"],
            ["eq.provider-request-sms"],
        )

    def test_callback_missing_lookup_fails_closed_on_unknown_or_mismatch(
        self,
    ) -> None:
        cases = (
            [
                {
                    "status": "sent",
                    "active_channel": "alimtalk",
                    "provider_request_id": "provider-request-1",
                    "provider_result_id": "provider-result-1",
                }
            ],
            [
                {
                    "status": "waiting_result",
                    "active_channel": "sms",
                    "provider_request_id": "provider-request-1",
                    "provider_result_id": None,
                }
            ],
            [
                {
                    "status": "waiting_result",
                    "active_channel": "alimtalk",
                    "provider_request_id": "provider-request-1",
                    "provider_result_id": None,
                },
                {
                    "status": "waiting_result",
                    "active_channel": "alimtalk",
                    "provider_request_id": "provider-request-1",
                    "provider_result_id": None,
                },
            ],
        )
        for rows in cases:
            with self.subTest(rows=rows):
                client, _ = client_with([response(rows)])
                self.assertFalse(
                    client.is_callback_missing("provider-request-1")
                )

        client, _ = client_with([response([]), response([])])
        self.assertFalse(client.is_callback_missing("unknown-request"))


if __name__ == "__main__":
    unittest.main()
