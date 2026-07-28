from __future__ import annotations

from dataclasses import replace
from datetime import datetime, timedelta, timezone
import hashlib
import unittest

from Segang.project.limited_links import (
    InMemoryLimitedSessionStore,
    LimitedLinkService,
    LimitedSessionGrant,
    LinkPurpose,
    LinkTokenRecord,
)


NOW = datetime(2026, 7, 26, 12, 30, tzinfo=timezone.utc)
RAW_TOKEN = "A_secure_opaque_link_token_1234567890abcd"
SESSION_ID = "limited_session_reference_1234567890abcdef"
CSRF_TOKEN = "limited_csrf_reference_1234567890abcdefgh"


class InMemoryLinkRepository:
    def __init__(self, record: LinkTokenRecord | None) -> None:
        self.record = record
        self.consumed_at: datetime | None = None
        self.consume_calls = 0

    def lookup(self, token_hash: bytes) -> LinkTokenRecord | None:
        if self.record is None or self.record.token_hash != token_hash:
            return None
        if self.consumed_at is None:
            return self.record
        return replace(self.record, consumed_at=self.consumed_at)

    def consume_if_current(
        self,
        record: LinkTokenRecord,
        now: datetime,
    ) -> bool:
        self.consume_calls += 1
        if (
            self.record is None
            or self.record.token_id != record.token_id
            or self.record.token_hash != record.token_hash
            or self.consumed_at is not None
            or self.record.consumed_at is not None
            or self.record.revoked_at is not None
            or self.record.expires_at <= now
        ):
            return False
        self.consumed_at = now
        return True


class RecordingSessionStore:
    def __init__(self) -> None:
        self.grants: list[LimitedSessionGrant] = []

    def issue(self, grant: LimitedSessionGrant) -> str:
        self.grants.append(grant)
        return SESSION_ID


def valid_record(**changes: object) -> LinkTokenRecord:
    record = LinkTokenRecord(
        token_id=7,
        token_hash=hashlib.sha256(RAW_TOKEN.encode("ascii")).digest(),
        purpose=LinkPurpose.TEMP_HISTORY,
        target_path="/device-temp-history/42",
        expires_at=NOW + timedelta(minutes=30),
        consumed_at=None,
        revoked_at=None,
        source_user_id=3,
        source_device_id=42,
        device_user_id=3,
        workplace_id=8,
        workplace_user_id=3,
        device_workplace_id=8,
        target_device_id=42,
        sensor_ids=(7, 8),
        sensor_device_ids=(42, 42),
    )
    return replace(record, **changes)


def service_with(
    record: LinkTokenRecord | None,
) -> tuple[LimitedLinkService, InMemoryLinkRepository, RecordingSessionStore]:
    repository = InMemoryLinkRepository(record)
    sessions = RecordingSessionStore()
    service = LimitedLinkService(
        repository=repository,
        sessions=sessions,
        clock=lambda: NOW,
        csrf_token_factory=lambda: CSRF_TOKEN,
        max_session_seconds=900,
    )
    return service, repository, sessions


class LimitedLinkServiceTests(unittest.TestCase):
    def test_purpose_values_match_live_database_contract(self) -> None:
        self.assertEqual(
            LinkPurpose.TEMP_HISTORY.value,
            "temperature_history",
        )
        self.assertEqual(
            LinkPurpose.SETTINGS.value,
            "device_settings",
        )

    def test_valid_token_is_hashed_consumed_once_and_scoped(self) -> None:
        service, repository, sessions = service_with(valid_record())

        redeemed = service.redeem(RAW_TOKEN)

        self.assertIsNotNone(redeemed)
        assert redeemed is not None
        self.assertEqual(redeemed.session_id, SESSION_ID)
        self.assertEqual(
            redeemed.target_path,
            "/device-temp-history/42",
        )
        self.assertEqual(redeemed.max_age_seconds, 900)
        self.assertEqual(repository.consume_calls, 1)
        self.assertEqual(len(sessions.grants), 1)
        grant = sessions.grants[0]
        self.assertIs(grant.purpose, LinkPurpose.TEMP_HISTORY)
        self.assertEqual(grant.user_id, 3)
        self.assertEqual(grant.workplace_id, 8)
        self.assertEqual(grant.device_id, 42)
        self.assertEqual(grant.sensor_ids, (7, 8))
        self.assertEqual(grant.csrf_token, CSRF_TOKEN)
        self.assertNotIn(RAW_TOKEN, repr(grant))
        self.assertNotIn(CSRF_TOKEN, repr(grant))

    def test_live_history_target_shape_is_canonicalized_before_redirect(
        self,
    ) -> None:
        service, _, sessions = service_with(
            valid_record(target_path="/history?deviceId=42")
        )

        redeemed = service.redeem(RAW_TOKEN)

        self.assertIsNotNone(redeemed)
        assert redeemed is not None
        self.assertEqual(
            redeemed.target_path,
            "/device-temp-history/42",
        )
        self.assertEqual(sessions.grants[0].target_path, redeemed.target_path)

    def test_expired_revoked_and_preconsumed_tokens_fail_closed(self) -> None:
        cases = (
            valid_record(expires_at=NOW),
            valid_record(revoked_at=NOW - timedelta(seconds=1)),
            valid_record(consumed_at=NOW - timedelta(seconds=1)),
        )
        for record in cases:
            with self.subTest(record=record):
                service, repository, sessions = service_with(record)

                self.assertIsNone(service.redeem(RAW_TOKEN))

                self.assertEqual(repository.consume_calls, 0)
                self.assertEqual(sessions.grants, [])

    def test_same_token_cannot_be_reused_after_atomic_first_use(self) -> None:
        service, repository, sessions = service_with(valid_record())

        self.assertIsNotNone(service.redeem(RAW_TOKEN))
        self.assertIsNone(service.redeem(RAW_TOKEN))

        self.assertEqual(repository.consume_calls, 1)
        self.assertEqual(len(sessions.grants), 1)

    def test_wrong_purpose_target_pair_is_rejected_before_consumption(
        self,
    ) -> None:
        cases = (
            valid_record(
                purpose=LinkPurpose.TEMP_HISTORY,
                target_path="/device-settings/42",
            ),
            valid_record(
                purpose=LinkPurpose.SETTINGS,
                target_path="/device-temp-history/42",
                sensor_ids=(),
                sensor_device_ids=(),
            ),
            valid_record(target_path="/history?deviceId=42&extra=1"),
            valid_record(target_path="/history?deviceId=99"),
        )
        for record in cases:
            with self.subTest(record=record):
                service, repository, sessions = service_with(record)

                self.assertIsNone(service.redeem(RAW_TOKEN))

                self.assertEqual(repository.consume_calls, 0)
                self.assertEqual(sessions.grants, [])

    def test_valid_settings_token_is_consumed_once_and_scoped(self) -> None:
        service, repository, sessions = service_with(
            valid_record(
                purpose=LinkPurpose.SETTINGS,
                target_path="/device-settings/42",
            )
        )

        resolved = service.redeem(RAW_TOKEN)

        self.assertIsNotNone(resolved)
        assert resolved is not None
        self.assertEqual(resolved.target_path, "/device-settings/42")
        self.assertEqual(repository.consume_calls, 1)
        self.assertEqual(repository.consumed_at, NOW)
        self.assertEqual(len(sessions.grants), 1)
        grant = sessions.grants[0]
        self.assertIs(grant.purpose, LinkPurpose.SETTINGS)
        self.assertEqual(grant.device_id, 42)
        self.assertEqual(grant.sensor_ids, (7, 8))
        self.assertEqual(grant.csrf_token, CSRF_TOKEN)
        self.assertIsNone(service.redeem(RAW_TOKEN))

    def test_settings_token_with_wrong_device_fails_closed(self) -> None:
        service, repository, sessions = service_with(
            valid_record(
                purpose=LinkPurpose.SETTINGS,
                target_path="/device-settings/99",
            )
        )

        self.assertIsNone(service.redeem(RAW_TOKEN))
        self.assertEqual(repository.consume_calls, 0)
        self.assertEqual(sessions.grants, [])

    def test_unproven_user_workplace_device_or_sensor_ownership_fails_closed(
        self,
    ) -> None:
        cases = (
            valid_record(device_user_id=99),
            valid_record(workplace_user_id=99),
            valid_record(device_workplace_id=99),
            valid_record(source_device_id=99),
            valid_record(target_device_id=99),
            valid_record(sensor_ids=(7,)),
            valid_record(sensor_ids=(7, 7)),
            valid_record(sensor_ids=(7, 0)),
            valid_record(sensor_device_ids=(42, 99)),
            valid_record(sensor_device_ids=()),
        )
        for record in cases:
            with self.subTest(record=record):
                service, repository, sessions = service_with(record)

                self.assertIsNone(service.redeem(RAW_TOKEN))

                self.assertEqual(repository.consume_calls, 0)
                self.assertEqual(sessions.grants, [])

    def test_unknown_or_malformed_raw_token_never_reaches_consumption(
        self,
    ) -> None:
        service, repository, sessions = service_with(valid_record())

        for token in ("short", "contains/slash", "한글토큰", "B" * 257):
            with self.subTest(token=token):
                self.assertIsNone(service.redeem(token))
        unknown_service, unknown_repository, unknown_sessions = service_with(
            None
        )
        self.assertIsNone(unknown_service.redeem("B" * 43))

        self.assertEqual(repository.consume_calls, 0)
        self.assertEqual(sessions.grants, [])
        self.assertEqual(unknown_repository.consume_calls, 0)
        self.assertEqual(unknown_sessions.grants, [])


class InMemoryLimitedSessionStoreTests(unittest.TestCase):
    def test_session_is_server_side_bounded_and_expires_fail_closed(
        self,
    ) -> None:
        current = [NOW]
        session_ids = iter(
            (
                "limited_session_reference_1234567890abcdef",
                "limited_session_reference_2234567890abcdef",
            )
        )
        store = InMemoryLimitedSessionStore(
            clock=lambda: current[0],
            token_factory=lambda: next(session_ids),
            max_entries=1,
        )
        first = LimitedSessionGrant(
            purpose=LinkPurpose.TEMP_HISTORY,
            target_path="/device-temp-history/42",
            user_id=3,
            workplace_id=8,
            device_id=42,
            sensor_ids=(7, 8),
            expires_at=NOW + timedelta(minutes=15),
            csrf_token=CSRF_TOKEN,
        )
        second = replace(
            first,
            device_id=43,
            target_path="/device-temp-history/43",
            sensor_ids=(9,),
        )

        first_id = store.issue(first)
        self.assertEqual(store.resolve(first_id), first)
        second_id = store.issue(second)

        self.assertIsNone(store.resolve(first_id))
        self.assertEqual(store.resolve(second_id), second)
        current[0] = NOW + timedelta(minutes=15)
        self.assertIsNone(store.resolve(second_id))
        self.assertIsNone(store.resolve("short"))

    def test_discard_invalidates_only_the_exact_limited_session(self) -> None:
        session_ids = iter(
            (
                "limited_session_reference_1234567890abcdef",
                "limited_session_reference_2234567890abcdef",
            )
        )
        store = InMemoryLimitedSessionStore(
            clock=lambda: NOW,
            token_factory=lambda: next(session_ids),
        )
        grant = LimitedSessionGrant(
            purpose=LinkPurpose.SETTINGS,
            target_path="/device-settings/42",
            user_id=3,
            workplace_id=8,
            device_id=42,
            sensor_ids=(7, 8),
            expires_at=NOW + timedelta(minutes=15),
            csrf_token=CSRF_TOKEN,
        )
        first_id = store.issue(grant)
        second_id = store.issue(replace(grant, device_id=43))

        self.assertFalse(store.discard("short"))
        self.assertTrue(store.discard(first_id))
        self.assertFalse(store.discard(first_id))
        self.assertIsNone(store.resolve(first_id))
        self.assertIsNotNone(store.resolve(second_id))


if __name__ == "__main__":
    unittest.main()
