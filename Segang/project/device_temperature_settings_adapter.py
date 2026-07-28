from __future__ import annotations

from collections.abc import Mapping, Sequence
from datetime import datetime
from decimal import Decimal, InvalidOperation
from typing import Protocol

try:
    from .device_temperature_settings import (
        SensorTemperatureSetting,
        SensorTemperatureUpdate,
        SettingsRepositoryError,
        SettingsValidationError,
        format_upper_limit,
    )
except ImportError:
    from device_temperature_settings import (
        SensorTemperatureSetting,
        SensorTemperatureUpdate,
        SettingsRepositoryError,
        SettingsValidationError,
        format_upper_limit,
    )


class SupabaseResponse(Protocol):
    data: object


class SupabaseRpcClient(Protocol):
    def rpc(self, name: str, params: object) -> object:
        ...


class SupabaseDeviceTemperatureSettingsRepository:
    def __init__(self, client: SupabaseRpcClient) -> None:
        self._client = client

    def load(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
    ) -> tuple[SensorTemperatureSetting, ...]:
        data = self._execute(
            "get_device_temperature_settings",
            {
                "p_user_id": user_id,
                "p_workplace_id": workplace_id,
                "p_device_id": device_id,
            },
        )
        return _parse_rows(data, expected_device_id=device_id)

    def update(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
        updates: tuple[SensorTemperatureUpdate, ...],
    ) -> tuple[SensorTemperatureSetting, ...]:
        data = self._execute(
            "update_device_temperature_settings",
            {
                "p_user_id": user_id,
                "p_workplace_id": workplace_id,
                "p_device_id": device_id,
                "p_updates": [
                    {
                        "sensor_pk": update.sensor_pk,
                        "upper_limit": format_upper_limit(
                            update.upper_limit
                        ),
                        "max_notifications": (
                            update.max_notifications
                        ),
                    }
                    for update in updates
                ],
            },
        )
        return _parse_rows(data, expected_device_id=device_id)

    def _execute(self, name: str, params: object) -> object:
        try:
            query = self._client.rpc(name, params)
            response = query.execute()  # type: ignore[attr-defined]
            return response.data  # type: ignore[attr-defined]
        except Exception as exc:
            if isinstance(exc, SettingsRepositoryError):
                raise
            raise SettingsRepositoryError(
                "temperature settings RPC failed"
            ) from None


def _parse_rows(
    data: object,
    *,
    expected_device_id: int,
) -> tuple[SensorTemperatureSetting, ...]:
    if (
        not isinstance(data, Sequence)
        or isinstance(data, (str, bytes, bytearray))
    ):
        raise SettingsRepositoryError(
            "temperature settings RPC returned a non-list"
        )
    settings: list[SensorTemperatureSetting] = []
    seen: set[int] = set()
    for row in data:
        if not isinstance(row, Mapping):
            raise SettingsRepositoryError(
                "temperature settings row is malformed"
            )
        try:
            sensor_pk = _positive_int(row.get("user_sensor_pk"))
            user_sensor_id = _positive_int(
                row.get("user_sensor_id")
            )
            device_id = _positive_int(row.get("device_id"))
            if (
                device_id != expected_device_id
                or row.get("sensor_type") != "TMP"
                or sensor_pk in seen
            ):
                raise SettingsRepositoryError(
                    "temperature settings row escaped scope"
                )
            update = SensorTemperatureUpdate.parse(
                sensor_pk=sensor_pk,
                upper_limit=row.get("upper_limit"),
                max_notifications=row.get("max_notifications"),
            )
            latest_value = _optional_decimal(
                row.get("latest_value")
            )
            latest_observed_at = _optional_aware_datetime(
                row.get("latest_observed_at")
            )
            if (latest_value is None) != (
                latest_observed_at is None
            ):
                raise SettingsRepositoryError(
                    "latest temperature pair is incomplete"
                )
            settings.append(
                SensorTemperatureSetting(
                    sensor_pk=sensor_pk,
                    user_sensor_id=user_sensor_id,
                    upper_limit=update.upper_limit,
                    max_notifications=update.max_notifications,
                    latest_value=latest_value,
                    latest_observed_at=latest_observed_at,
                )
            )
            seen.add(sensor_pk)
        except (SettingsValidationError, ValueError, TypeError):
            raise SettingsRepositoryError(
                "temperature settings row is malformed"
            ) from None
    if not settings:
        raise SettingsRepositoryError(
            "temperature settings RPC returned no sensors"
        )
    return tuple(settings)


def _positive_int(value: object) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < 1
    ):
        raise SettingsRepositoryError(
            "temperature settings identifier is invalid"
        )
    return value


def _optional_decimal(value: object) -> Decimal | None:
    if value is None:
        return None
    if isinstance(value, bool):
        raise SettingsRepositoryError(
            "latest temperature is invalid"
        )
    try:
        parsed = Decimal(str(value))
    except (InvalidOperation, ValueError):
        raise SettingsRepositoryError(
            "latest temperature is invalid"
        ) from None
    if not parsed.is_finite():
        raise SettingsRepositoryError(
            "latest temperature is invalid"
        )
    return parsed


def _optional_aware_datetime(value: object) -> datetime | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise SettingsRepositoryError(
            "latest observation time is invalid"
        )
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        raise SettingsRepositoryError(
            "latest observation time is invalid"
        ) from None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise SettingsRepositoryError(
            "latest observation time is invalid"
        )
    return parsed
