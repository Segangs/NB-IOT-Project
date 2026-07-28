from __future__ import annotations

from datetime import datetime
from decimal import Decimal
import json
import re
from typing import Mapping, Optional, Sequence
from urllib.parse import urlencode

from .catalog import TemplateCatalog
from .config import SupabaseConfig
from .domain import (
    DeliveryChannel,
    MessageJob,
    MessagePolicy,
    MessageStatus,
    ProviderPushResult,
    PushAction,
    PushApplication,
    SubmissionRegistration,
)
from .http_transport import (
    HttpRequest,
    HttpResponse,
    HttpTransport,
    StdlibHttpTransport,
)


class SupabaseRestError(RuntimeError):
    """Sanitized Supabase Data API or hydration failure."""


class SupabaseRestDatabase:
    def __init__(
        self,
        config: SupabaseConfig,
        catalog: TemplateCatalog,
        *,
        transport: Optional[HttpTransport] = None,
    ) -> None:
        self._config = config
        self._catalog = catalog
        self._transport = transport or StdlibHttpTransport()

    def recover_expired_leases(
        self,
        now: datetime,
        limit: int,
        max_attempts: int,
        base_delay_seconds: int,
        max_delay_seconds: int,
        waiting_result_timeout_seconds: int,
    ) -> int:
        del now
        payload = self._rpc(
            "recover_msg_send_leases",
            {
                "p_batch_size": limit,
                "p_max_attempts": max_attempts,
                "p_retry_base_seconds": base_delay_seconds,
                "p_retry_max_seconds": max_delay_seconds,
                "p_waiting_result_timeout_seconds":
                    waiting_result_timeout_seconds,
            },
        )
        if isinstance(payload, bool) or not isinstance(payload, int):
            raise SupabaseRestError("invalid recovery RPC response")
        return payload

    def drain_temperature_alert_outbox(
        self,
        limit: int = 16,
    ) -> tuple[str, ...]:
        if isinstance(limit, bool) or not isinstance(limit, int):
            raise ValueError("temperature outbox limit must be an integer")
        if limit < 1 or limit > 32:
            raise ValueError("temperature outbox limit must be 1..32")
        payload = self._rpc(
            "drain_temperature_alert_outbox",
            {"p_limit": limit},
        )
        rows = _rows(payload, "temperature outbox drain")
        if len(rows) > limit:
            raise SupabaseRestError(
                "invalid temperature outbox drain cardinality"
            )
        enqueued = []
        for row in rows:
            result = row.get("result")
            message_id = row.get("msg_send_id")
            if result == "enqueued":
                if (
                    isinstance(message_id, bool)
                    or not isinstance(message_id, int)
                    or message_id <= 0
                ):
                    raise SupabaseRestError(
                        "invalid temperature outbox drain response"
                    )
                enqueued.append(str(message_id))
            elif result not in (
                "contact_cardinality",
                "policy_cardinality",
                "ownership_missing",
                "enqueue_failed",
            ):
                raise SupabaseRestError(
                    "invalid temperature outbox drain result"
                )
        return tuple(enqueued)

    def claim_messages(
        self,
        worker_id: str,
        now: datetime,
        lease_seconds: int,
        limit: int,
    ) -> Sequence[MessageJob]:
        del now
        payload = self._rpc(
            "claim_msg_send",
            {
                "p_lock_owner": worker_id,
                "p_batch_size": limit,
                "p_lease_seconds": lease_seconds,
            },
        )
        rows = _rows(payload, "claim")
        claimed_rows = []
        for raw in rows:
            row = dict(raw)
            row["lock_owner"] = worker_id
            claimed_rows.append(row)
        return self._hydrate_rows(claimed_rows)

    def suppress_message(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        self._complete(job, "suppressed", None, reason_code)

    def suppress_duplicate(
        self,
        job: MessageJob,
        now: datetime,
    ) -> None:
        del now
        self._complete(
            job,
            "suppressed",
            None,
            "duplicate_claim_batch",
        )

    def mark_submission_started(
        self,
        job: MessageJob,
        channel: DeliveryChannel,
    ) -> None:
        self._rpc(
            "mark_msg_send_submission_started",
            {
                "p_msg_send_id": _message_id(job),
                "p_lock_owner": _lock_owner(job),
                "p_lease_token": job.lease_token,
                "p_channel": channel.value,
            },
        )

    def mark_submission_waiting_result(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
    ) -> SubmissionRegistration:
        payload = self._rpc(
            "mark_msg_send_submission_waiting_result",
            {
                "p_msg_send_id": _message_id(job),
                "p_lock_owner": _lock_owner(job),
                "p_lease_token": job.lease_token,
                "p_channel": channel.value,
                "p_provider_request_id": provider_request_id,
                "p_accepted_at": _timestamp(now),
            },
        )
        row = _single_rpc_row(payload, "submission registration")
        message = row.get("message")
        if not isinstance(message, Mapping):
            raise SupabaseRestError(
                "invalid submission registration response"
            )
        try:
            status = MessageStatus(str(message["status"]))
            applied = _strict_bool(row.get("applied"))
            duplicate = _strict_bool(row.get("duplicate"))
        except (KeyError, ValueError) as exc:
            raise SupabaseRestError(
                "invalid submission registration response"
            ) from exc
        return SubmissionRegistration(
            applied=applied,
            duplicate=duplicate,
            current_status=status,
        )

    def mark_submission_accepted_success(
        self,
        job: MessageJob,
        provider_request_id: str,
        channel: DeliveryChannel,
        now: datetime,
        cost_amount: Decimal,
    ) -> SubmissionRegistration:
        payload = self._rpc(
            "finalize_msg_send_provider_acceptance",
            {
                "p_msg_send_id": _message_id(job),
                "p_lock_owner": _lock_owner(job),
                "p_lease_token": job.lease_token,
                "p_channel": channel.value,
                "p_provider_request_id": provider_request_id,
                "p_accepted_at": _timestamp(now),
                "p_cost_amount": format(cost_amount, "f"),
            },
        )
        row = _single_rpc_row(payload, "provider acceptance")
        message = row.get("message")
        if not isinstance(message, Mapping):
            raise SupabaseRestError(
                "invalid provider acceptance response"
            )
        try:
            status = MessageStatus(str(message["status"]))
            applied = _strict_bool(row.get("applied"))
            duplicate = _strict_bool(row.get("duplicate"))
        except (KeyError, ValueError) as exc:
            raise SupabaseRestError(
                "invalid provider acceptance response"
            ) from exc
        return SubmissionRegistration(
            applied=applied,
            duplicate=duplicate,
            current_status=status,
        )

    def schedule_retry(
        self,
        job: MessageJob,
        next_attempt_at: datetime,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        self._complete(
            job,
            "retry_wait",
            next_attempt_at,
            reason_code,
        )

    def mark_failed(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        self._complete(job, "failed", None, reason_code)

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
        del now
        payload = self._rpc(
            "record_msg_send_push_result",
            {
                "p_provider_request_id": result.request_id,
                "p_submission_lease_token":
                    result.submission_token,
                "p_provider_result_id": result.result_id,
                "p_channel": result.channel.value,
                "p_delivered": result.delivered,
                "p_retryable": result.retryable,
                "p_result_at": _timestamp(result.result_at),
                "p_result_code": (
                    result.provider_result_code
                    or result.failure_code
                    or (
                        "delivered"
                        if result.delivered
                        else "provider_delivery_failed"
                    )
                ),
                "p_cost_amount": format(result.cost_amount, "f"),
                "p_lock_owner": worker_id,
                "p_lease_seconds": lease_seconds,
                "p_max_attempts": max_attempts,
                "p_retry_base_seconds": base_delay_seconds,
                "p_retry_max_seconds": max_delay_seconds,
            },
        )
        row = _single_rpc_row(payload, "push result")
        try:
            action = PushAction(str(row["action"]))
        except (KeyError, ValueError) as exc:
            raise SupabaseRestError("invalid push result response") from exc
        message = None
        if action is PushAction.SMS_FALLBACK:
            raw_message = row.get("message")
            if not isinstance(raw_message, Mapping):
                raise SupabaseRestError(
                    "SMS fallback response has no message"
                )
            hydrated = self._hydrate_rows([raw_message])
            if len(hydrated) != 1:
                raise SupabaseRestError(
                    "SMS fallback hydration failed"
                )
            message = hydrated[0]
        return PushApplication(action=action, message=message)

    def reject_sms_fallback(
        self,
        job: MessageJob,
        reason_code: str,
        now: datetime,
    ) -> None:
        del now
        self._complete(job, "failed", None, reason_code)

    def oldest_waiting_result_at(self) -> Optional[datetime]:
        rows = self._table(
            "msg_send",
            {
                "select": (
                    "status,active_channel,"
                    "provider_submission_started_at,"
                    "fallback_submission_started_at,"
                    "provider_accepted_at,"
                    "fallback_provider_accepted_at"
                ),
                "status": "in.(accepted,waiting_result)",
                "order": "updated_at.asc",
                "limit": "1000",
            },
        )
        timestamps = []
        for row in rows:
            status = row.get("status")
            channel = row.get("active_channel")
            if status == "accepted":
                field = (
                    "provider_submission_started_at"
                    if channel == "alimtalk"
                    else "fallback_submission_started_at"
                )
            elif status == "waiting_result":
                field = (
                    "provider_accepted_at"
                    if channel == "alimtalk"
                    else "fallback_provider_accepted_at"
                )
            else:
                raise SupabaseRestError("invalid health query response")
            raw_timestamp = row.get(field)
            if raw_timestamp is not None:
                timestamps.append(_parse_timestamp(raw_timestamp))
        return min(timestamps) if timestamps else None

    def lookup_admin_phone(self) -> str:
        rows = self._table(
            "users",
            {
                "select": "userId,userPhoneNumber",
                "userAccountId": "eq.admin",
                "limit": "2",
            },
        )
        if len(rows) != 1:
            raise SupabaseRestError(
                "admin recipient lookup must return exactly one row"
            )
        phone = rows[0].get("userPhoneNumber")
        if not isinstance(phone, str):
            raise SupabaseRestError("admin recipient is invalid")
        return _normalize_phone(phone)

    def is_callback_missing(self, provider_request_id: str) -> bool:
        request_id = _provider_request_id(provider_request_id)
        select = (
            "status,active_channel,"
            "provider_request_id,provider_result_id,"
            "fallback_provider_request_id,"
            "fallback_provider_result_id"
        )
        primary = self._table(
            "msg_send",
            {
                "select": select,
                "provider_request_id": "eq." + request_id,
                "limit": "2",
            },
        )
        if primary:
            return _is_missing_callback_row(
                primary,
                request_id,
                DeliveryChannel.ALIMTALK,
            )
        fallback = self._table(
            "msg_send",
            {
                "select": select,
                "fallback_provider_request_id":
                    "eq." + request_id,
                "limit": "2",
            },
        )
        return _is_missing_callback_row(
            fallback,
            request_id,
            DeliveryChannel.SMS,
        )

    def claim_exact_one_shot(
        self,
        expected_message_id: str,
        worker_id: str,
        now: datetime,
        lease_seconds: int,
    ) -> tuple[MessageJob, ...]:
        del now
        payload = self._rpc(
            "claim_exact_one_shot_msg_send",
            {
                "p_expected_msg_send_id": _positive_int(
                    expected_message_id,
                    "expected message",
                ),
                "p_lock_owner": worker_id,
                "p_lease_seconds": lease_seconds,
            },
        )
        rows = _rows(payload, "exact one-shot claim")
        if len(rows) != 1:
            raise SupabaseRestError(
                "invalid exact one-shot claim response cardinality"
            )
        row = dict(rows[0])
        row["lock_owner"] = worker_id
        return self._hydrate_rows([row])

    def _complete(
        self,
        job: MessageJob,
        action: str,
        available_at: Optional[datetime],
        reason_code: str,
    ) -> None:
        self._rpc(
            "complete_msg_send_claim",
            {
                "p_msg_send_id": _message_id(job),
                "p_lock_owner": _lock_owner(job),
                "p_lease_token": job.lease_token,
                "p_action": action,
                "p_available_at": (
                    _timestamp(available_at)
                    if available_at is not None
                    else None
                ),
                "p_error_code": reason_code,
            },
        )

    def _hydrate_rows(
        self,
        rows: Sequence[Mapping[str, object]],
    ) -> tuple[MessageJob, ...]:
        if not rows:
            return ()
        contact_ids = {
            _positive_int(row.get("alert_contact_id"), "alert contact")
            for row in rows
        }
        policy_ids = {
            _positive_int(row.get("message_policy_id"), "message policy")
            for row in rows
        }
        contacts = _index_rows(
            self._table(
                "alert_contact",
                {
                    "select": (
                        "alert_contact_id,destination_e164,"
                        "consent_status,is_active"
                    ),
                    "alert_contact_id":
                        _in_filter(contact_ids),
                },
            ),
            "alert_contact_id",
        )
        policies = _index_rows(
            self._table(
                "message_policy",
                {
                    "select": (
                        "message_policy_id,allow_sms_fallback,"
                        "fallback_cost_class,is_active"
                    ),
                    "message_policy_id":
                        _in_filter(policy_ids),
                },
            ),
            "message_policy_id",
        )
        hydrated = []
        for row in rows:
            contact_id = _positive_int(
                row.get("alert_contact_id"),
                "alert contact",
            )
            policy_id = _positive_int(
                row.get("message_policy_id"),
                "message policy",
            )
            try:
                contact = contacts[contact_id]
                policy = policies[policy_id]
            except KeyError as exc:
                raise SupabaseRestError(
                    "claimed message hydration is incomplete"
                ) from exc
            hydrated.append(
                self._hydrate_message(row, contact, policy)
            )
        return tuple(hydrated)

    def _hydrate_message(
        self,
        row: Mapping[str, object],
        contact: Mapping[str, object],
        policy: Mapping[str, object],
    ) -> MessageJob:
        template_code = row.get("template_code")
        if not isinstance(template_code, str):
            raise SupabaseRestError("claimed template code is invalid")
        template = self._catalog.by_code(template_code)
        raw_params = row.get("template_params")
        if not isinstance(raw_params, Mapping):
            raise SupabaseRestError("claimed template parameters are invalid")
        params = dict(raw_params)
        history_url = _link_from_parameter(
            params.pop("tempHistoryToken", None)
        )
        settings_url = _link_from_parameter(
            params.pop("SettingsToken", None)
        )
        sms_body = params.pop("smsBody", "")
        if not isinstance(sms_body, str):
            raise SupabaseRestError("claimed SMS body is invalid")

        destination = contact.get("destination_e164")
        if not isinstance(destination, str):
            raise SupabaseRestError("claimed recipient is invalid")
        contact_active = contact.get("is_active") is True
        policy_active = policy.get("is_active") is True
        consent_granted = (
            contact_active
            and contact.get("consent_status") == "granted"
        )
        allow_fallback = (
            policy_active
            and policy.get("allow_sms_fallback") is True
        )
        fallback_cost_class = policy.get("fallback_cost_class")
        if fallback_cost_class is not None and not isinstance(
            fallback_cost_class,
            str,
        ):
            raise SupabaseRestError("claimed policy is invalid")
        try:
            channel = DeliveryChannel(str(row.get("active_channel")))
        except ValueError as exc:
            raise SupabaseRestError("claimed channel is invalid") from exc
        fallback_reason = row.get("fallback_reason")
        if fallback_reason is not None and not isinstance(
            fallback_reason,
            str,
        ):
            raise SupabaseRestError("claimed fallback reason is invalid")
        lease_token = row.get("lease_token")
        lock_owner = row.get("lock_owner")
        dedupe_key = row.get("dedupe_key")
        if not all(
            isinstance(value, str) and value
            for value in (lease_token, lock_owner, dedupe_key)
        ):
            raise SupabaseRestError("claimed fence is invalid")
        return MessageJob(
            message_id=str(
                _positive_int(row.get("msg_send_id"), "message")
            ),
            lease_token=lease_token,
            dedupe_key=dedupe_key,
            recipient=_normalize_phone(destination),
            template_code=template_code,
            template_body=template.body,
            history_url=history_url,
            settings_url=settings_url,
            sms_body=sms_body,
            attempt_count=_nonnegative_int(
                row.get("attempt_count"),
                "attempt count",
            ),
            policy=MessagePolicy(
                consent_granted=consent_granted,
                sms_fallback_approved=allow_fallback,
                sms_cost_class=(
                    fallback_cost_class
                    if allow_fallback
                    else None
                ),
            ),
            delivery_channel=channel,
            fallback_reason=fallback_reason,
            template_params=params,
            max_attempts=_positive_int(
                row.get("max_attempts"),
                "max attempts",
            ),
            lock_owner=lock_owner,
        )

    def _rpc(
        self,
        name: str,
        body: Mapping[str, object],
    ) -> object:
        return self._request_json(
            HttpRequest(
                method="POST",
                url=(
                    self._config.url
                    + "/rest/v1/rpc/"
                    + name
                ),
                headers=self._headers(json_body=True),
                body=json.dumps(
                    body,
                    ensure_ascii=False,
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode("utf-8"),
                timeout_seconds=self._config.timeout_seconds,
            ),
            operation="RPC",
        )

    def _table(
        self,
        table: str,
        query: Mapping[str, str],
    ) -> tuple[Mapping[str, object], ...]:
        payload = self._request_json(
            HttpRequest(
                method="GET",
                url=(
                    self._config.url
                    + "/rest/v1/"
                    + table
                    + "?"
                    + urlencode(query)
                ),
                headers=self._headers(json_body=False),
                timeout_seconds=self._config.timeout_seconds,
            ),
            operation="table",
        )
        return tuple(_rows(payload, "table"))

    def _headers(self, *, json_body: bool) -> dict[str, str]:
        headers = {
            "Accept": "application/json",
            "apikey": self._config.secret_key,
            "Connection": "close",
        }
        if json_body:
            headers["Content-Type"] = "application/json"
        return headers

    def _request_json(
        self,
        request: HttpRequest,
        *,
        operation: str,
    ) -> object:
        response = self._transport.request(request)
        if response.status < 200 or response.status >= 300:
            raise SupabaseRestError(
                f"Supabase {operation} failed: status={response.status}"
            )
        try:
            return json.loads(response.body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise SupabaseRestError(
                f"Supabase {operation} response was invalid"
            ) from exc


def _rows(
    payload: object,
    operation: str,
) -> list[Mapping[str, object]]:
    if not isinstance(payload, list) or not all(
        isinstance(row, Mapping) for row in payload
    ):
        raise SupabaseRestError(
            f"invalid {operation} response shape"
        )
    return list(payload)


def _single_rpc_row(
    payload: object,
    operation: str,
) -> Mapping[str, object]:
    rows = _rows(payload, operation)
    if len(rows) != 1:
        raise SupabaseRestError(
            f"invalid {operation} response cardinality"
        )
    return rows[0]


def _message_id(job: MessageJob) -> int:
    return _positive_int(job.message_id, "message")


def _lock_owner(job: MessageJob) -> str:
    if not job.lock_owner:
        raise SupabaseRestError("message claim owner is missing")
    return job.lock_owner


def _timestamp(value: datetime) -> str:
    if value.tzinfo is None or value.utcoffset() is None:
        raise SupabaseRestError("timestamp must be timezone-aware")
    return value.isoformat()


def _parse_timestamp(value: object) -> datetime:
    if not isinstance(value, str):
        raise SupabaseRestError("invalid timestamp response")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise SupabaseRestError("invalid timestamp response") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise SupabaseRestError("invalid timestamp response")
    return parsed


def _strict_bool(value: object) -> bool:
    if not isinstance(value, bool):
        raise ValueError("expected boolean")
    return value


def _positive_int(value: object, name: str) -> int:
    if isinstance(value, bool):
        raise SupabaseRestError(f"invalid {name} identifier")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise SupabaseRestError(f"invalid {name} identifier") from exc
    if parsed <= 0:
        raise SupabaseRestError(f"invalid {name} identifier")
    return parsed


def _nonnegative_int(value: object, name: str) -> int:
    if isinstance(value, bool):
        raise SupabaseRestError(f"invalid {name}")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise SupabaseRestError(f"invalid {name}") from exc
    if parsed < 0:
        raise SupabaseRestError(f"invalid {name}")
    return parsed


def _index_rows(
    rows: Sequence[Mapping[str, object]],
    key: str,
) -> dict[int, Mapping[str, object]]:
    indexed = {}
    for row in rows:
        identifier = _positive_int(row.get(key), key)
        if identifier in indexed:
            raise SupabaseRestError("duplicate hydration row")
        indexed[identifier] = row
    return indexed


def _in_filter(values: set[int]) -> str:
    return "in.(" + ",".join(str(value) for value in sorted(values)) + ")"


def _link_from_parameter(value: object) -> str:
    if not isinstance(value, str) or not value:
        raise SupabaseRestError("opaque link parameter is missing")
    if value.startswith("https://"):
        return value
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,256}", value):
        raise SupabaseRestError("opaque link parameter is invalid")
    return "https://zxcx.io/s/" + value


def _normalize_phone(value: str) -> str:
    compact = re.sub(r"[- ]", "", value)
    if compact.startswith("+82"):
        compact = "0" + compact[3:]
    if not re.fullmatch(r"0[0-9]{8,10}", compact):
        raise SupabaseRestError("recipient phone format is invalid")
    return compact


def _provider_request_id(value: object) -> str:
    if (
        not isinstance(value, str)
        or not re.fullmatch(r"[A-Za-z0-9._:-]{1,256}", value)
    ):
        raise SupabaseRestError("provider request id is invalid")
    return value


def _is_missing_callback_row(
    rows: Sequence[Mapping[str, object]],
    request_id: str,
    channel: DeliveryChannel,
) -> bool:
    if len(rows) != 1:
        return False
    row = rows[0]
    if (
        row.get("status") != MessageStatus.WAITING_RESULT.value
        or row.get("active_channel") != channel.value
    ):
        return False
    if channel is DeliveryChannel.ALIMTALK:
        return (
            row.get("provider_request_id") == request_id
            and row.get("provider_result_id") is None
        )
    return (
        row.get("fallback_provider_request_id") == request_id
        and row.get("fallback_provider_result_id") is None
    )
