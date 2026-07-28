from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timedelta
from enum import Enum
import hashlib
import hmac
import re
import secrets
from threading import Lock
from typing import Callable, Optional, Protocol
from urllib.parse import parse_qs, urlsplit


class LinkPurpose(str, Enum):
    TEMP_HISTORY = "temperature_history"
    SETTINGS = "device_settings"


@dataclass(frozen=True)
class LinkTokenRecord:
    token_id: int
    token_hash: bytes = field(repr=False)
    purpose: LinkPurpose
    target_path: str
    expires_at: datetime
    consumed_at: Optional[datetime]
    revoked_at: Optional[datetime]
    source_user_id: int
    source_device_id: int
    device_user_id: int
    workplace_id: int
    workplace_user_id: int
    device_workplace_id: int
    target_device_id: int
    sensor_ids: tuple[int, ...]
    sensor_device_ids: tuple[int, ...]


@dataclass(frozen=True)
class LimitedSessionGrant:
    purpose: LinkPurpose
    target_path: str
    user_id: int
    workplace_id: int
    device_id: int
    sensor_ids: tuple[int, ...]
    expires_at: datetime
    csrf_token: str = field(repr=False)


@dataclass(frozen=True)
class RedeemedLink:
    session_id: str = field(repr=False)
    target_path: str
    max_age_seconds: int


class LinkTokenRepository(Protocol):
    def lookup(self, token_hash: bytes) -> Optional[LinkTokenRecord]:
        ...

    def consume_if_current(
        self,
        record: LinkTokenRecord,
        now: datetime,
    ) -> bool:
        """Atomically recheck the proof and consume an unused token."""
        ...


class LimitedSessionStore(Protocol):
    def issue(self, grant: LimitedSessionGrant) -> str:
        """Persist a server-side limited session and return its opaque id."""
        ...

    def resolve(self, session_id: object) -> Optional[LimitedSessionGrant]:
        ...

    def discard(self, session_id: object) -> bool:
        ...


class InMemoryLimitedSessionStore:
    def __init__(
        self,
        *,
        clock: Callable[[], datetime],
        token_factory: Callable[[], str] = (
            lambda: secrets.token_urlsafe(32)
        ),
        max_entries: int = 4096,
    ) -> None:
        if not 1 <= max_entries <= 65536:
            raise ValueError(
                "limited session capacity must be between 1 and 65536"
            )
        self._clock = clock
        self._token_factory = token_factory
        self._max_entries = max_entries
        self._sessions: dict[str, LimitedSessionGrant] = {}
        self._lock = Lock()

    def issue(self, grant: LimitedSessionGrant) -> str:
        now = self._aware_now()
        if grant.expires_at <= now:
            raise ValueError("limited session grant is already expired")
        session_id = self._token_factory()
        if (
            not isinstance(session_id, str)
            or _SESSION_ID.fullmatch(session_id) is None
        ):
            raise ValueError("limited session id is invalid")
        with self._lock:
            self._purge_expired(now)
            if session_id in self._sessions:
                raise ValueError("limited session id collision")
            if len(self._sessions) >= self._max_entries:
                oldest_id = min(
                    self._sessions,
                    key=lambda key: self._sessions[key].expires_at,
                )
                self._sessions.pop(oldest_id, None)
            self._sessions[session_id] = grant
        return session_id

    def resolve(self, session_id: object) -> Optional[LimitedSessionGrant]:
        if (
            not isinstance(session_id, str)
            or _SESSION_ID.fullmatch(session_id) is None
        ):
            return None
        now = self._aware_now()
        with self._lock:
            self._purge_expired(now)
            return self._sessions.get(session_id)

    def discard(self, session_id: object) -> bool:
        if (
            not isinstance(session_id, str)
            or _SESSION_ID.fullmatch(session_id) is None
        ):
            return False
        now = self._aware_now()
        with self._lock:
            self._purge_expired(now)
            return self._sessions.pop(session_id, None) is not None

    def _aware_now(self) -> datetime:
        now = self._clock()
        if now.tzinfo is None or now.utcoffset() is None:
            raise ValueError("limited session clock must be timezone-aware")
        return now

    def _purge_expired(self, now: datetime) -> None:
        expired = [
            key
            for key, grant in self._sessions.items()
            if grant.expires_at <= now
        ]
        for key in expired:
            self._sessions.pop(key, None)


_RAW_TOKEN = re.compile(r"[A-Za-z0-9_-]{32,256}")
_SESSION_ID = re.compile(r"[A-Za-z0-9_-]{32,256}")
_CSRF_TOKEN = re.compile(r"[A-Za-z0-9_-]{32,256}")
_TEMP_HISTORY_PATH = re.compile(r"/device-temp-history/([1-9][0-9]*)")
_DEVICE_SETTINGS_PATH = re.compile(r"/device-settings/([1-9][0-9]*)")


class LimitedLinkService:
    def __init__(
        self,
        *,
        repository: LinkTokenRepository,
        sessions: LimitedSessionStore,
        clock: Callable[[], datetime],
        csrf_token_factory: Callable[[], str] = (
            lambda: secrets.token_urlsafe(32)
        ),
        max_session_seconds: int = 900,
    ) -> None:
        if not 1 <= max_session_seconds <= 3600:
            raise ValueError(
                "limited session lifetime must be between 1 and 3600"
            )
        self._repository = repository
        self._sessions = sessions
        self._clock = clock
        self._csrf_token_factory = csrf_token_factory
        self._max_session_seconds = max_session_seconds

    def redeem(
        self,
        raw_token: str,
    ) -> Optional[RedeemedLink]:
        if (
            not isinstance(raw_token, str)
            or _RAW_TOKEN.fullmatch(raw_token) is None
        ):
            return None
        now = self._clock()
        if now.tzinfo is None or now.utcoffset() is None:
            raise ValueError("limited link clock must be timezone-aware")
        token_hash = hashlib.sha256(
            raw_token.encode("ascii")
        ).digest()
        try:
            record = self._repository.lookup(token_hash)
        except Exception:
            return None
        if (
            record is None
            or not hmac.compare_digest(
                record.token_hash,
                token_hash,
            )
            or not self._record_is_current(record, now)
        ):
            return None
        authorized_target = self._authorized_target(record)
        if authorized_target is None:
            return None
        target_device_id, target_path = authorized_target
        csrf_token = self._csrf_token_factory()
        if (
            not isinstance(csrf_token, str)
            or _CSRF_TOKEN.fullmatch(csrf_token) is None
        ):
            return None
        try:
            consumed = self._repository.consume_if_current(
                record,
                now,
            )
        except Exception:
            return None
        if not consumed:
            return None
        seconds_remaining = int(
            (record.expires_at - now).total_seconds()
        )
        max_age_seconds = min(
            self._max_session_seconds,
            seconds_remaining,
        )
        if max_age_seconds < 1:
            return None
        grant = LimitedSessionGrant(
            purpose=record.purpose,
            target_path=target_path,
            user_id=record.source_user_id,
            workplace_id=record.workplace_id,
            device_id=target_device_id,
            sensor_ids=record.sensor_ids,
            expires_at=now + timedelta(seconds=max_age_seconds),
            csrf_token=csrf_token,
        )
        session_id = self._sessions.issue(grant)
        if (
            not isinstance(session_id, str)
            or _SESSION_ID.fullmatch(session_id) is None
        ):
            return None
        return RedeemedLink(
            session_id=session_id,
            target_path=target_path,
            max_age_seconds=max_age_seconds,
        )

    @staticmethod
    def _record_is_current(
        record: LinkTokenRecord,
        now: datetime,
    ) -> bool:
        return (
            _positive_identifier(record.token_id)
            and record.expires_at.tzinfo is not None
            and record.expires_at.utcoffset() is not None
            and record.expires_at > now
            and record.consumed_at is None
            and record.revoked_at is None
        )

    @staticmethod
    def _authorized_target(
        record: LinkTokenRecord,
    ) -> Optional[tuple[int, str]]:
        identities = (
            record.source_user_id,
            record.source_device_id,
            record.device_user_id,
            record.workplace_id,
            record.workplace_user_id,
            record.device_workplace_id,
            record.target_device_id,
        )
        if not all(_positive_identifier(value) for value in identities):
            return None
        if not (
            record.source_user_id
            == record.device_user_id
            == record.workplace_user_id
            and record.workplace_id == record.device_workplace_id
            and record.source_device_id == record.target_device_id
        ):
            return None

        if record.purpose is LinkPurpose.TEMP_HISTORY:
            target = _canonical_temp_history_target(record.target_path)
        elif record.purpose is LinkPurpose.SETTINGS:
            target = _canonical_device_settings_target(record.target_path)
        else:
            return None
        if target is None:
            return None
        path_device_id, target_path = target
        if (
            path_device_id != record.target_device_id
            or not isinstance(record.sensor_ids, tuple)
            or not isinstance(record.sensor_device_ids, tuple)
            or len(record.sensor_ids) != len(record.sensor_device_ids)
            or len(set(record.sensor_ids)) != len(record.sensor_ids)
            or not record.sensor_device_ids
            or not all(
                _positive_identifier(sensor_id)
                for sensor_id in record.sensor_ids
            )
            or not all(
                _positive_identifier(device_id)
                and device_id == record.target_device_id
                for device_id in record.sensor_device_ids
            )
        ):
            return None
        return path_device_id, target_path


def _positive_identifier(value: object) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and value > 0
    )


def _canonical_temp_history_target(
    target_path: object,
) -> Optional[tuple[int, str]]:
    if not isinstance(target_path, str):
        return None
    parsed = urlsplit(target_path)
    if parsed.scheme or parsed.netloc or parsed.fragment:
        return None

    match = _TEMP_HISTORY_PATH.fullmatch(parsed.path)
    if match is not None and not parsed.query:
        device_id = int(match.group(1))
        return device_id, parsed.path

    if parsed.path != "/history":
        return None
    try:
        query = parse_qs(
            parsed.query,
            keep_blank_values=True,
            strict_parsing=True,
        )
    except ValueError:
        return None
    if set(query) != {"deviceId"} or len(query["deviceId"]) != 1:
        return None
    raw_device_id = query["deviceId"][0]
    if re.fullmatch(r"[1-9][0-9]*", raw_device_id) is None:
        return None
    device_id = int(raw_device_id)
    return device_id, f"/device-temp-history/{device_id}"


def _canonical_device_settings_target(
    target_path: object,
) -> Optional[tuple[int, str]]:
    if not isinstance(target_path, str):
        return None
    parsed = urlsplit(target_path)
    if (
        parsed.scheme
        or parsed.netloc
        or parsed.query
        or parsed.fragment
    ):
        return None
    match = _DEVICE_SETTINGS_PATH.fullmatch(parsed.path)
    if match is None:
        return None
    return int(match.group(1)), parsed.path
