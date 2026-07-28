from __future__ import annotations

from datetime import datetime, timedelta, timezone
from decimal import Decimal
import unittest

from Segang.project.device_temperature_settings import (
    DeviceTemperatureSettingsService,
    SensorTemperatureSetting,
    SensorTemperatureUpdate,
    SettingsScopeError,
    SettingsValidationError,
)
from Segang.project.limited_links import LimitedSessionGrant, LinkPurpose


NOW = datetime(2026, 7, 27, 10, 0, tzinfo=timezone.utc)
CSRF_TOKEN = "limited_csrf_reference_1234567890abcdefgh"


def grant(**changes: object) -> LimitedSessionGrant:
    values = {
        "purpose": LinkPurpose.SETTINGS,
        "target_path": "/device-settings/42",
        "user_id": 3,
        "workplace_id": 8,
        "device_id": 42,
        "sensor_ids": (7, 8),
        "expires_at": NOW + timedelta(minutes=15),
        "csrf_token": CSRF_TOKEN,
    }
    values.update(changes)
    return LimitedSessionGrant(**values)  # type: ignore[arg-type]


def setting(
    sensor_pk: int,
    *,
    upper_limit: str = "-7.0",
    max_notifications: int = 3,
) -> SensorTemperatureSetting:
    return SensorTemperatureSetting(
        sensor_pk=sensor_pk,
        user_sensor_id=1 if sensor_pk == 7 else 2,
        upper_limit=Decimal(upper_limit),
        max_notifications=max_notifications,
        latest_value=Decimal("-9.5"),
        latest_observed_at=NOW,
    )


class RecordingRepository:
    def __init__(
        self,
        *,
        loaded: tuple[SensorTemperatureSetting, ...] = (
            setting(7),
            setting(8, upper_limit="-10.0", max_notifications=2),
        ),
    ) -> None:
        self.loaded = loaded
        self.load_calls: list[tuple[int, int, int]] = []
        self.update_calls: list[
            tuple[int, int, int, tuple[SensorTemperatureUpdate, ...]]
        ] = []

    def load(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
    ) -> tuple[SensorTemperatureSetting, ...]:
        self.load_calls.append((user_id, workplace_id, device_id))
        return self.loaded

    def update(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
        updates: tuple[SensorTemperatureUpdate, ...],
    ) -> tuple[SensorTemperatureSetting, ...]:
        self.update_calls.append(
            (user_id, workplace_id, device_id, updates)
        )
        return self.loaded


class SensorTemperatureUpdateTests(unittest.TestCase):
    def test_accepts_literal_boundaries_half_steps_and_counts(self) -> None:
        accepted = (
            (7, "-50", 1, Decimal("-50.0")),
            (7, "-49.5", 2, Decimal("-49.5")),
            (8, "24.5", 3, Decimal("24.5")),
            (8, "25", 1, Decimal("25.0")),
        )
        for sensor_pk, upper_limit, count, expected in accepted:
            with self.subTest(
                upper_limit=upper_limit,
                count=count,
            ):
                update = SensorTemperatureUpdate.parse(
                    sensor_pk=sensor_pk,
                    upper_limit=upper_limit,
                    max_notifications=count,
                )
                self.assertEqual(update.upper_limit, expected)
                self.assertEqual(update.max_notifications, count)

    def test_rejects_non_finite_quarter_step_bool_and_out_of_range(self) -> None:
        rejected = (
            (7, "NaN", 1),
            (7, "Infinity", 1),
            (7, "-50.5", 1),
            (7, "25.5", 1),
            (7, "-7.25", 1),
            (7, "-7", 0),
            (7, "-7", 4),
            (True, "-7", 1),
            (7, True, 1),
            (7, "-7", True),
        )
        for sensor_pk, upper_limit, count in rejected:
            with self.subTest(
                sensor_pk=sensor_pk,
                upper_limit=upper_limit,
                count=count,
            ):
                with self.assertRaises(SettingsValidationError):
                    SensorTemperatureUpdate.parse(
                        sensor_pk=sensor_pk,
                        upper_limit=upper_limit,
                        max_notifications=count,
                    )

    def test_direct_construction_cannot_bypass_validation(self) -> None:
        invalid_values = (
            (0, Decimal("-7.0"), 1),
            (7, Decimal("-7.25"), 1),
            (7, Decimal("-7.0"), 4),
        )
        for sensor_pk, upper_limit, count in invalid_values:
            with self.subTest(
                sensor_pk=sensor_pk,
                upper_limit=upper_limit,
                count=count,
            ):
                with self.assertRaises(SettingsValidationError):
                    SensorTemperatureUpdate(
                        sensor_pk=sensor_pk,
                        upper_limit=upper_limit,
                        max_notifications=count,
                    )


class DeviceTemperatureSettingsServiceTests(unittest.TestCase):
    def test_load_passes_exact_scope_and_returns_both_mapped_sensors(
        self,
    ) -> None:
        repository = RecordingRepository()
        service = DeviceTemperatureSettingsService(repository)

        loaded = service.load(grant())

        self.assertEqual(loaded, repository.loaded)
        self.assertEqual(repository.load_calls, [(3, 8, 42)])

    def test_wrong_purpose_path_device_or_sensor_result_fails_closed(
        self,
    ) -> None:
        cases = (
            (
                grant(purpose=LinkPurpose.TEMP_HISTORY),
                RecordingRepository(),
            ),
            (
                grant(target_path="/device-settings/99"),
                RecordingRepository(),
            ),
            (
                grant(device_id=99),
                RecordingRepository(),
            ),
            (
                grant(sensor_ids=(7, 7)),
                RecordingRepository(),
            ),
            (
                grant(sensor_ids=(0, 7)),
                RecordingRepository(),
            ),
            (
                grant(),
                RecordingRepository(loaded=(setting(7), setting(9))),
            ),
        )
        for scoped_grant, repository in cases:
            with self.subTest(scoped_grant=scoped_grant):
                service = DeviceTemperatureSettingsService(repository)
                with self.assertRaises(SettingsScopeError):
                    service.load(scoped_grant)

    def test_update_rejects_duplicate_or_out_of_scope_before_repository(
        self,
    ) -> None:
        repository = RecordingRepository()
        service = DeviceTemperatureSettingsService(repository)
        sensor_7 = SensorTemperatureUpdate.parse(
            sensor_pk=7,
            upper_limit="-6.5",
            max_notifications=2,
        )
        sensor_9 = SensorTemperatureUpdate.parse(
            sensor_pk=9,
            upper_limit="-6.5",
            max_notifications=2,
        )

        for updates in ((sensor_7, sensor_7), (sensor_9,), ()):
            with self.subTest(updates=updates):
                with self.assertRaises(SettingsScopeError):
                    service.update(grant(), updates)

        self.assertEqual(repository.update_calls, [])

    def test_update_rejects_malformed_item_before_repository(self) -> None:
        repository = RecordingRepository()
        service = DeviceTemperatureSettingsService(repository)

        with self.assertRaises(SettingsScopeError):
            service.update(
                grant(),
                (object(),),  # type: ignore[arg-type]
            )

        self.assertEqual(repository.update_calls, [])

    def test_update_accepts_one_scoped_sensor_in_one_repository_call(
        self,
    ) -> None:
        repository = RecordingRepository()
        service = DeviceTemperatureSettingsService(repository)
        update = SensorTemperatureUpdate.parse(
            sensor_pk=8,
            upper_limit="-8.5",
            max_notifications=1,
        )

        loaded = service.update(grant(), (update,))

        self.assertEqual(loaded, repository.loaded)
        self.assertEqual(
            repository.update_calls,
            [(3, 8, 42, (update,))],
        )


if __name__ == "__main__":
    unittest.main()
