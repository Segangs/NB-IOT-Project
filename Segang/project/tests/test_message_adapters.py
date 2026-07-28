from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from decimal import Decimal
import hashlib
import unittest

from msg_send.config import TariffConfig
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    ProviderResultMode,
    MessagePolicy,
    PushAction,
    PushApplication,
)
from Segang.project.limited_links import LinkPurpose
from Segang.project.message_adapters import (
    NonSendingCallbackRuntime,
    SupabaseLinkTokenRepository,
)


NOW = datetime(2026, 7, 26, 12, 30, tzinfo=timezone.utc)
RAW_TOKEN = "A_secure_opaque_link_token_1234567890abcd"
TOKEN_HASH = hashlib.sha256(RAW_TOKEN.encode("ascii")).digest()


@dataclass
class FakeResponse:
    data: list[dict[str, object]]


class FakeQuery:
    def __init__(
        self,
        client: "FakeSupabase",
        table: str,
    ) -> None:
        self.client = client
        self.table = table
        self.operation = "select"
        self.arguments: list[tuple[str, object, object]] = []

    def select(self, columns: str) -> "FakeQuery":
        self.arguments.append(("select", columns, None))
        return self

    def update(self, values: dict[str, object]) -> "FakeQuery":
        self.operation = "update"
        self.arguments.append(("update", values, None))
        return self

    def eq(self, column: str, value: object) -> "FakeQuery":
        self.arguments.append(("eq", column, value))
        return self

    def is_(self, column: str, value: object) -> "FakeQuery":
        self.arguments.append(("is", column, value))
        return self

    def gt(self, column: str, value: object) -> "FakeQuery":
        self.arguments.append(("gt", column, value))
        return self

    def limit(self, count: int) -> "FakeQuery":
        self.arguments.append(("limit", count, None))
        return self

    def order(self, column: str) -> "FakeQuery":
        self.arguments.append(("order", column, None))
        return self

    def execute(self) -> FakeResponse:
        self.client.calls.append(
            (self.table, self.operation, tuple(self.arguments))
        )
        return FakeResponse(self.client.rows[self.table].pop(0))


class FakeSupabase:
    def __init__(
        self,
        rows: dict[str, list[list[dict[str, object]]]],
    ) -> None:
        self.rows = rows
        self.calls: list[
            tuple[str, str, tuple[tuple[str, object, object], ...]]
        ] = []

    def table(self, name: str) -> FakeQuery:
        return FakeQuery(self, name)


def valid_rows() -> dict[str, list[list[dict[str, object]]]]:
    return {
        "message_link_token": [
            [
                {
                    "message_link_token_id": 7,
                    "msg_send_id": 4,
                    "token_hash": "\\x" + TOKEN_HASH.hex(),
                    "token_hash_algorithm": "sha256",
                    "purpose": "temperature_history",
                    "target_path": "/history?deviceId=42",
                    "expires_at": (
                        NOW + timedelta(minutes=30)
                    ).isoformat(),
                    "consumed_at": None,
                    "revoked_at": None,
                }
            ],
            [{"message_link_token_id": 7}],
        ],
        "msg_send": [
            [
                {
                    "msg_send_id": 4,
                    "source_user_id": 3,
                    "source_device_id": 42,
                }
            ]
        ],
        "device": [
            [
                {
                    "deviceId": 42,
                    "userId": 3,
                    "userWorkplaceId": 8,
                }
            ]
        ],
        "userworkplace": [
            [{"userWorkplaceId": 8, "userId": 3}]
        ],
        "USER_SENSOR": [
            [
                {"Id": 7, "deviceId": 42, "sensorCtgyId": 1},
                {"Id": 8, "deviceId": 42, "sensorCtgyId": 1},
                {"Id": 9, "deviceId": 42, "sensorCtgyId": 2},
            ]
        ],
        "SENSOR_CTGY": [
            [
                {"sensorCtgyId": 1, "sensorCtgyType": "TMP"},
                {"sensorCtgyId": 2, "sensorCtgyType": "MIC"},
            ]
        ],
    }


class SupabaseLinkTokenRepositoryTests(unittest.TestCase):
    def test_lookup_hydrates_exact_live_ownership_proof(self) -> None:
        client = FakeSupabase(valid_rows())
        repository = SupabaseLinkTokenRepository(client)

        record = repository.lookup(TOKEN_HASH)

        self.assertIsNotNone(record)
        assert record is not None
        self.assertEqual(record.token_id, 7)
        self.assertEqual(record.token_hash, TOKEN_HASH)
        self.assertIs(record.purpose, LinkPurpose.TEMP_HISTORY)
        self.assertEqual(record.target_path, "/history?deviceId=42")
        self.assertEqual(record.source_user_id, 3)
        self.assertEqual(record.target_device_id, 42)
        self.assertEqual(record.sensor_ids, (7, 8))
        self.assertEqual(record.sensor_device_ids, (42, 42))
        self.assertEqual(
            [call[0] for call in client.calls],
            [
                "message_link_token",
                "msg_send",
                "device",
                "userworkplace",
                "USER_SENSOR",
                "SENSOR_CTGY",
            ],
        )

    def test_settings_lookup_hydrates_only_temperature_sensor_scope(self) -> None:
        rows = valid_rows()
        link = rows["message_link_token"][0][0]
        link["purpose"] = "device_settings"
        link["target_path"] = "/device-settings/42"
        client = FakeSupabase(rows)
        repository = SupabaseLinkTokenRepository(client)

        record = repository.lookup(TOKEN_HASH)

        self.assertIsNotNone(record)
        assert record is not None
        self.assertIs(record.purpose, LinkPurpose.SETTINGS)
        self.assertEqual(record.sensor_ids, (7, 8))
        self.assertEqual(record.sensor_device_ids, (42, 42))
        self.assertEqual(
            [call[0] for call in client.calls],
            [
                "message_link_token",
                "msg_send",
                "device",
                "userworkplace",
                "USER_SENSOR",
                "SENSOR_CTGY",
            ],
        )

    def test_consume_uses_atomic_current_row_filters(self) -> None:
        client = FakeSupabase(valid_rows())
        repository = SupabaseLinkTokenRepository(client)
        record = repository.lookup(TOKEN_HASH)
        assert record is not None

        consumed = repository.consume_if_current(record, NOW)

        self.assertTrue(consumed)
        update_call = client.calls[-1]
        self.assertEqual(update_call[0:2], ("message_link_token", "update"))
        arguments = update_call[2]
        self.assertIn(("eq", "message_link_token_id", 7), arguments)
        self.assertIn(
            ("eq", "token_hash", "\\x" + TOKEN_HASH.hex()),
            arguments,
        )
        self.assertIn(("is", "consumed_at", "null"), arguments)
        self.assertIn(("is", "revoked_at", "null"), arguments)
        self.assertIn(("gt", "expires_at", NOW.isoformat()), arguments)


def callback_payload() -> dict[str, object]:
    return {
        "CMSGID": "provider-request-reference",
        "REFKEY": "1234567812344abc8def1234567890ab",
        "MSGID": "provider-result-reference",
        "UNIXTIME": "1785038400",
        "RESULT": "7000",
        "MEDIA": "AT",
    }


class CallbackDatabase:
    def __init__(self, application: PushApplication) -> None:
        self.application = application
        self.applied: list[dict[str, object]] = []
        self.rejected: list[tuple[MessageJob, str]] = []

    def apply_push_result(self, **kwargs: object) -> PushApplication:
        self.applied.append(dict(kwargs))
        return self.application

    def reject_sms_fallback(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        self.rejected.append((job, reason_code))


def fallback_job() -> MessageJob:
    return MessageJob(
        message_id="42",
        lease_token="12345678-1234-4abc-8def-1234567890ab",
        dedupe_key="dedupe-42",
        recipient="01012345678",
        template_code="template-reference",
        template_body="private",
        history_url="https://zxcx.io/s/history",
        settings_url="https://zxcx.io/s/settings",
        sms_body="private",
        attempt_count=2,
        policy=MessagePolicy(True, True, "sms"),
        delivery_channel=DeliveryChannel.SMS,
        fallback_reason="7001",
        max_attempts=1,
        lock_owner="callback-web-1",
    )


class NonSendingCallbackRuntimeTests(unittest.TestCase):
    def test_delivered_result_is_applied_without_provider_dependency(
        self,
    ) -> None:
        database = CallbackDatabase(
            PushApplication(PushAction.DELIVERED)
        )
        runtime = NonSendingCallbackRuntime(
            database=database,
            tariffs=TariffConfig(
                alimtalk=Decimal("8.0000"),
                sms=Decimal("12.0000"),
            ),
            worker_id="callback-web-1",
            clock=lambda: NOW,
        )

        action = runtime.handle_callback_payload_non_sending(
            callback_payload()
        )

        self.assertIs(action, PushAction.DELIVERED)
        self.assertEqual(len(database.applied), 1)
        self.assertEqual(database.rejected, [])

    def test_sms_fallback_action_is_closed_without_sending(self) -> None:
        job = fallback_job()
        database = CallbackDatabase(
            PushApplication(PushAction.SMS_FALLBACK, job)
        )
        runtime = NonSendingCallbackRuntime(
            database=database,
            tariffs=TariffConfig(
                alimtalk=Decimal("8.0000"),
                sms=Decimal("12.0000"),
            ),
            worker_id="callback-web-1",
            clock=lambda: NOW,
        )
        payload = callback_payload()
        payload["RESULT"] = "7001"

        action = runtime.handle_callback_payload_non_sending(payload)

        self.assertIs(action, PushAction.FAILED)
        self.assertEqual(
            database.rejected,
            [(job, "sms_fallback_submission_disabled")],
        )

    def test_provider_acceptance_mode_ignores_late_callback(self) -> None:
        database = CallbackDatabase(
            PushApplication(PushAction.DELIVERED)
        )
        runtime = NonSendingCallbackRuntime(
            database=database,
            tariffs=TariffConfig(
                alimtalk=Decimal("8.0000"),
                sms=Decimal("12.0000"),
            ),
            worker_id="callback-web-1",
            clock=lambda: NOW,
            provider_result_mode=(
                ProviderResultMode.PROVIDER_ACCEPTANCE
            ),
        )

        action = runtime.handle_callback_payload_non_sending(
            callback_payload()
        )

        self.assertIs(action, PushAction.DUPLICATE)
        self.assertEqual(database.applied, [])
        self.assertEqual(database.rejected, [])

    def test_provider_acceptance_mode_keeps_sms_callback_path(self) -> None:
        database = CallbackDatabase(
            PushApplication(PushAction.DELIVERED)
        )
        runtime = NonSendingCallbackRuntime(
            database=database,
            tariffs=TariffConfig(
                alimtalk=Decimal("8.0000"),
                sms=Decimal("12.0000"),
            ),
            worker_id="callback-web-1",
            clock=lambda: NOW,
            provider_result_mode=(
                ProviderResultMode.PROVIDER_ACCEPTANCE
            ),
        )
        payload = callback_payload()
        payload["MEDIA"] = "SMS"
        payload["RESULT"] = "4100"

        action = runtime.handle_callback_payload_non_sending(payload)

        self.assertIs(action, PushAction.DELIVERED)
        self.assertEqual(len(database.applied), 1)
        self.assertEqual(database.rejected, [])


if __name__ == "__main__":
    unittest.main()
