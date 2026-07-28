from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal
import unittest

from Segang.project.device_temperature_settings import (
    SensorTemperatureUpdate,
    SettingsRepositoryError,
)
from Segang.project.device_temperature_settings_adapter import (
    SupabaseDeviceTemperatureSettingsRepository,
)


OBSERVED_AT = datetime(2026, 7, 27, 9, 30, tzinfo=timezone.utc)


@dataclass
class FakeResponse:
    data: object


class FakeRpc:
    def __init__(self, client: "FakeClient", name: str, params: object) -> None:
        self.client = client
        self.name = name
        self.params = params

    def execute(self) -> FakeResponse:
        self.client.calls.append((self.name, self.params))
        return FakeResponse(self.client.responses.pop(0))


class FakeClient:
    def __init__(self, responses: list[object]) -> None:
        self.responses = responses
        self.calls: list[tuple[str, object]] = []

    def rpc(self, name: str, params: object) -> FakeRpc:
        return FakeRpc(self, name, params)


def rows() -> list[dict[str, object]]:
    return [
        {
            "user_sensor_pk": 7,
            "user_sensor_id": 1,
            "device_id": 42,
            "sensor_type": "TMP",
            "upper_limit": "-7.0",
            "max_notifications": 3,
            "latest_value": "-9.5",
            "latest_observed_at": OBSERVED_AT.isoformat(),
        },
        {
            "user_sensor_pk": 8,
            "user_sensor_id": 2,
            "device_id": 42,
            "sensor_type": "TMP",
            "upper_limit": "-10.0",
            "max_notifications": 2,
            "latest_value": None,
            "latest_observed_at": None,
        },
    ]


class SupabaseDeviceTemperatureSettingsRepositoryTests(
    unittest.TestCase
):
    def test_load_uses_exact_rpc_and_strictly_parses_rows(self) -> None:
        client = FakeClient([rows()])
        repository = SupabaseDeviceTemperatureSettingsRepository(client)

        loaded = repository.load(3, 8, 42)

        self.assertEqual(
            client.calls,
            [
                (
                    "get_device_temperature_settings",
                    {
                        "p_user_id": 3,
                        "p_workplace_id": 8,
                        "p_device_id": 42,
                    },
                )
            ],
        )
        self.assertEqual(
            [item.sensor_pk for item in loaded],
            [7, 8],
        )
        self.assertEqual(loaded[0].upper_limit, Decimal("-7.0"))
        self.assertEqual(loaded[0].latest_value, Decimal("-9.5"))
        self.assertEqual(loaded[0].latest_observed_at, OBSERVED_AT)
        self.assertIsNone(loaded[1].latest_value)
        self.assertIsNone(loaded[1].latest_observed_at)

    def test_update_uses_exact_rpc_and_json_shape(self) -> None:
        client = FakeClient([rows()])
        repository = SupabaseDeviceTemperatureSettingsRepository(client)
        updates = (
            SensorTemperatureUpdate.parse(
                sensor_pk=7,
                upper_limit="-6.5",
                max_notifications=2,
            ),
            SensorTemperatureUpdate.parse(
                sensor_pk=8,
                upper_limit="-9",
                max_notifications=1,
            ),
        )

        repository.update(3, 8, 42, updates)

        self.assertEqual(
            client.calls,
            [
                (
                    "update_device_temperature_settings",
                    {
                        "p_user_id": 3,
                        "p_workplace_id": 8,
                        "p_device_id": 42,
                        "p_updates": [
                            {
                                "sensor_pk": 7,
                                "upper_limit": "-6.5",
                                "max_notifications": 2,
                            },
                            {
                                "sensor_pk": 8,
                                "upper_limit": "-9.0",
                                "max_notifications": 1,
                            },
                        ],
                    },
                )
            ],
        )

    def test_duplicate_malformed_wrong_device_or_non_temp_rows_fail(
        self,
    ) -> None:
        invalid_sets = (
            [rows()[0], rows()[0]],
            [{**rows()[0], "device_id": 99}],
            [{**rows()[0], "sensor_type": "MIC"}],
            [{**rows()[0], "latest_value": "NaN"}],
            [{**rows()[0], "latest_observed_at": "not-a-time"}],
            {"not": "a list"},
        )
        for response in invalid_sets:
            with self.subTest(response=response):
                repository = SupabaseDeviceTemperatureSettingsRepository(
                    FakeClient([response])
                )
                with self.assertRaises(SettingsRepositoryError):
                    repository.load(3, 8, 42)


if __name__ == "__main__":
    unittest.main()
