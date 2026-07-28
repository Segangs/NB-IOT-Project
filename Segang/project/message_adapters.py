from __future__ import annotations

from datetime import datetime
import re
from typing import Callable, Mapping, Optional, Protocol, Sequence

from msg_send.callback import parse_push_result
from msg_send.config import TariffConfig
from msg_send.domain import (
    DeliveryChannel,
    MessageJob,
    ProviderPushResult,
    ProviderResultMode,
    PushAction,
    PushApplication,
)

try:
    from .limited_links import LinkPurpose, LinkTokenRecord
except ImportError:
    from limited_links import LinkPurpose, LinkTokenRecord


class SupabaseResponse(Protocol):
    data: object


class SupabaseTableClient(Protocol):
    def table(self, name: str) -> object:
        ...


class CallbackDatabase(Protocol):
    def apply_push_result(
        self,
        result: ProviderPushResult,
        worker_id: str,
        now: datetime,
        lease_seconds: int,
        max_attempts: int,
        base_delay_seconds: int,
        max_delay_seconds: int,
    ) -> PushApplication:
        ...

    def reject_sms_fallback(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        ...


class NonSendingCallbackRuntime:
    def __init__(
        self,
        *,
        database: CallbackDatabase,
        tariffs: TariffConfig,
        worker_id: str,
        clock: Callable[[], datetime],
        lease_seconds: int = 120,
        max_attempts: int = 5,
        base_delay_seconds: int = 30,
        max_delay_seconds: int = 3600,
        provider_result_mode: ProviderResultMode = (
            ProviderResultMode.CALLBACK
        ),
    ) -> None:
        if re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", worker_id) is None:
            raise ValueError("callback worker id is invalid")
        if not 30 <= lease_seconds <= 300:
            raise ValueError("callback lease seconds are invalid")
        if not 1 <= max_attempts <= 100:
            raise ValueError("callback max attempts are invalid")
        if (
            base_delay_seconds < 1
            or max_delay_seconds < base_delay_seconds
        ):
            raise ValueError("callback retry delay is invalid")
        if not isinstance(provider_result_mode, ProviderResultMode):
            raise ValueError("callback provider result mode is invalid")
        self._database = database
        self._tariffs = tariffs
        self._worker_id = worker_id
        self._clock = clock
        self._lease_seconds = lease_seconds
        self._max_attempts = max_attempts
        self._base_delay_seconds = base_delay_seconds
        self._max_delay_seconds = max_delay_seconds
        self._provider_result_mode = provider_result_mode

    def handle_callback_payload_non_sending(
        self,
        payload: Mapping[str, object],
    ) -> PushAction:
        result = parse_push_result(payload, self._tariffs)
        if (
            self._provider_result_mode
            is ProviderResultMode.PROVIDER_ACCEPTANCE
            and result.channel is DeliveryChannel.ALIMTALK
        ):
            return PushAction.DUPLICATE
        now = self._clock()
        if now.tzinfo is None or now.utcoffset() is None:
            raise ValueError("callback clock must be timezone-aware")
        application = self._database.apply_push_result(
            result=result,
            worker_id=self._worker_id,
            now=now,
            lease_seconds=self._lease_seconds,
            max_attempts=self._max_attempts,
            base_delay_seconds=self._base_delay_seconds,
            max_delay_seconds=self._max_delay_seconds,
        )
        if application.action is not PushAction.SMS_FALLBACK:
            return application.action
        fallback_job = application.message
        if fallback_job is None:
            raise ValueError("SMS fallback result has no message")
        self._database.reject_sms_fallback(
            fallback_job,
            "sms_fallback_submission_disabled",
            now,
        )
        return PushAction.FAILED


class SupabaseLinkTokenRepository:
    def __init__(self, client: SupabaseTableClient) -> None:
        self._client = client

    def lookup(self, token_hash: bytes) -> Optional[LinkTokenRecord]:
        if not isinstance(token_hash, bytes) or len(token_hash) != 32:
            return None
        token_literal = _bytea_literal(token_hash)
        link = _optional_single(
            _execute(
                self._client.table("message_link_token")
                .select(
                    "message_link_token_id,msg_send_id,token_hash,"
                    "token_hash_algorithm,purpose,target_path,"
                    "expires_at,consumed_at,revoked_at"
                )
                .eq("token_hash", token_literal)
                .limit(2)
            ),
            "link token",
        )
        if link is None:
            return None
        if (
            link.get("token_hash_algorithm") != "sha256"
            or _parse_bytea(link.get("token_hash")) != token_hash
        ):
            return None
        purpose = _purpose(link.get("purpose"))

        message_id = _positive_int(link.get("msg_send_id"), "message")
        message = _required_single(
            _execute(
                self._client.table("msg_send")
                .select(
                    "msg_send_id,source_user_id,source_device_id"
                )
                .eq("msg_send_id", message_id)
                .limit(2)
            ),
            "message",
        )
        source_user_id = _positive_int(
            message.get("source_user_id"),
            "source user",
        )
        source_device_id = _positive_int(
            message.get("source_device_id"),
            "source device",
        )
        device = _required_single(
            _execute(
                self._client.table("device")
                .select("deviceId,userId,userWorkplaceId")
                .eq("deviceId", source_device_id)
                .limit(2)
            ),
            "device",
        )
        device_user_id = _positive_int(
            device.get("userId"),
            "device user",
        )
        workplace_id = _positive_int(
            device.get("userWorkplaceId"),
            "device workplace",
        )
        workplace = _required_single(
            _execute(
                self._client.table("userworkplace")
                .select("userWorkplaceId,userId")
                .eq("userWorkplaceId", workplace_id)
                .limit(2)
            ),
            "workplace",
        )
        sensor_ids: tuple[int, ...] = ()
        sensor_device_ids: tuple[int, ...] = ()
        if purpose in (LinkPurpose.TEMP_HISTORY, LinkPurpose.SETTINGS):
            sensors = _rows(
                _execute(
                    self._client.table("USER_SENSOR")
                    .select("Id,deviceId,sensorCtgyId")
                    .eq("deviceId", source_device_id)
                    .order("Id")
                ),
                "sensor",
            )
            categories = _rows(
                _execute(
                    self._client.table("SENSOR_CTGY")
                    .select("sensorCtgyId,sensorCtgyType")
                    .order("sensorCtgyId")
                ),
                "sensor category",
            )
            temp_category_ids = {
                _positive_int(
                    category.get("sensorCtgyId"),
                    "sensor category",
                )
                for category in categories
                if category.get("sensorCtgyType") == "TMP"
            }
            sensors = [
                sensor
                for sensor in sensors
                if _positive_int(
                    sensor.get("sensorCtgyId"),
                    "sensor category",
                )
                in temp_category_ids
            ]
            if not sensors:
                raise ValueError(
                    "link target has no temperature sensors"
                )
            sensor_ids = tuple(
                _positive_int(sensor.get("Id"), "sensor")
                for sensor in sensors
            )
            sensor_device_ids = tuple(
                _positive_int(
                    sensor.get("deviceId"),
                    "sensor device",
                )
                for sensor in sensors
            )
        return LinkTokenRecord(
            token_id=_positive_int(
                link.get("message_link_token_id"),
                "link token",
            ),
            token_hash=token_hash,
            purpose=purpose,
            target_path=_required_text(
                link.get("target_path"),
                "target path",
            ),
            expires_at=_timestamp(link.get("expires_at")),
            consumed_at=_optional_timestamp(link.get("consumed_at")),
            revoked_at=_optional_timestamp(link.get("revoked_at")),
            source_user_id=source_user_id,
            source_device_id=source_device_id,
            device_user_id=device_user_id,
            workplace_id=workplace_id,
            workplace_user_id=_positive_int(
                workplace.get("userId"),
                "workplace user",
            ),
            device_workplace_id=_positive_int(
                workplace.get("userWorkplaceId"),
                "device workplace",
            ),
            target_device_id=_positive_int(
                device.get("deviceId"),
                "target device",
            ),
            sensor_ids=sensor_ids,
            sensor_device_ids=sensor_device_ids,
        )

    def consume_if_current(
        self,
        record: LinkTokenRecord,
        now: datetime,
    ) -> bool:
        if now.tzinfo is None or now.utcoffset() is None:
            raise ValueError("link consume time must be timezone-aware")
        rows = _rows(
            _execute(
                self._client.table("message_link_token")
                .update({"consumed_at": now.isoformat()})
                .eq("message_link_token_id", record.token_id)
                .eq("token_hash", _bytea_literal(record.token_hash))
                .is_("consumed_at", "null")
                .is_("revoked_at", "null")
                .gt("expires_at", now.isoformat())
                .select("message_link_token_id")
            ),
            "link consume",
        )
        if len(rows) > 1:
            raise ValueError("link consume returned duplicate rows")
        return (
            len(rows) == 1
            and _positive_int(
                rows[0].get("message_link_token_id"),
                "link token",
            )
            == record.token_id
        )


def _execute(query: object) -> SupabaseResponse:
    execute = getattr(query, "execute", None)
    if not callable(execute):
        raise ValueError("Supabase query is not executable")
    return execute()


def _rows(
    response: SupabaseResponse,
    operation: str,
) -> list[Mapping[str, object]]:
    data = getattr(response, "data", None)
    if not isinstance(data, list) or not all(
        isinstance(row, Mapping) for row in data
    ):
        raise ValueError(f"{operation} returned invalid rows")
    return list(data)


def _optional_single(
    response: SupabaseResponse,
    operation: str,
) -> Optional[Mapping[str, object]]:
    rows = _rows(response, operation)
    if not rows:
        return None
    if len(rows) != 1:
        raise ValueError(f"{operation} returned duplicate rows")
    return rows[0]


def _required_single(
    response: SupabaseResponse,
    operation: str,
) -> Mapping[str, object]:
    row = _optional_single(response, operation)
    if row is None:
        raise ValueError(f"{operation} row is missing")
    return row


def _positive_int(value: object, name: str) -> int:
    if isinstance(value, bool):
        raise ValueError(f"{name} identifier is invalid")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} identifier is invalid") from exc
    if parsed <= 0:
        raise ValueError(f"{name} identifier is invalid")
    return parsed


def _required_text(value: object, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{name} is invalid")
    return value


def _timestamp(value: object) -> datetime:
    if not isinstance(value, str):
        raise ValueError("timestamp is invalid")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValueError("timestamp is invalid") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise ValueError("timestamp is invalid")
    return parsed


def _optional_timestamp(value: object) -> Optional[datetime]:
    return None if value is None else _timestamp(value)


def _purpose(value: object) -> LinkPurpose:
    try:
        return LinkPurpose(str(value))
    except ValueError as exc:
        raise ValueError("link purpose is invalid") from exc


def _bytea_literal(value: bytes) -> str:
    if len(value) != 32:
        raise ValueError("token hash is invalid")
    return "\\x" + value.hex()


def _parse_bytea(value: object) -> bytes:
    if isinstance(value, bytes):
        parsed = value
    elif isinstance(value, str) and re.fullmatch(
        r"\\x[0-9A-Fa-f]{64}",
        value,
    ):
        parsed = bytes.fromhex(value[2:])
    else:
        raise ValueError("token hash response is invalid")
    if len(parsed) != 32:
        raise ValueError("token hash response is invalid")
    return parsed
