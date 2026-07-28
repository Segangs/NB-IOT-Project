from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal, InvalidOperation
import re
from typing import Protocol, Sequence

try:
    from .limited_links import LimitedSessionGrant, LinkPurpose
except ImportError:
    from limited_links import LimitedSessionGrant, LinkPurpose


class SettingsValidationError(ValueError):
    pass


class SettingsScopeError(ValueError):
    pass


class SettingsRepositoryError(RuntimeError):
    pass


_SETTINGS_PATH = re.compile(r"/device-settings/([1-9][0-9]*)")
_CSRF_TOKEN = re.compile(r"[A-Za-z0-9_-]{32,256}")
_HALF_DEGREE = Decimal("0.5")
_MIN_UPPER_LIMIT = Decimal("-50.0")
_MAX_UPPER_LIMIT = Decimal("25.0")


@dataclass(frozen=True)
class SensorTemperatureSetting:
    sensor_pk: int
    user_sensor_id: int
    upper_limit: Decimal
    max_notifications: int
    latest_value: Decimal | None
    latest_observed_at: datetime | None

    def __post_init__(self) -> None:
        _positive_int(self.sensor_pk, "sensor")
        _positive_int(self.user_sensor_id, "user sensor")
        _validate_upper_limit(self.upper_limit)
        _notification_count(self.max_notifications)
        if self.latest_value is not None:
            _finite_decimal(self.latest_value, "latest value")
        if self.latest_observed_at is not None and (
            self.latest_observed_at.tzinfo is None
            or self.latest_observed_at.utcoffset() is None
        ):
            raise SettingsValidationError(
                "latest observation must be timezone-aware"
            )


@dataclass(frozen=True)
class SensorTemperatureUpdate:
    sensor_pk: int
    upper_limit: Decimal
    max_notifications: int

    def __post_init__(self) -> None:
        _positive_int(self.sensor_pk, "sensor")
        _validate_upper_limit(self.upper_limit)
        _notification_count(self.max_notifications)

    @classmethod
    def parse(
        cls,
        *,
        sensor_pk: object,
        upper_limit: object,
        max_notifications: object,
    ) -> "SensorTemperatureUpdate":
        return cls(
            sensor_pk=_positive_int(sensor_pk, "sensor"),
            upper_limit=_validate_upper_limit(upper_limit),
            max_notifications=_notification_count(
                max_notifications
            ),
        )


class DeviceTemperatureSettingsRepository(Protocol):
    def load(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
    ) -> Sequence[SensorTemperatureSetting]:
        ...

    def update(
        self,
        user_id: int,
        workplace_id: int,
        device_id: int,
        updates: tuple[SensorTemperatureUpdate, ...],
    ) -> Sequence[SensorTemperatureSetting]:
        ...


class DeviceTemperatureSettingsService:
    def __init__(
        self,
        repository: DeviceTemperatureSettingsRepository,
    ) -> None:
        self._repository = repository

    def load(
        self,
        grant: LimitedSessionGrant,
    ) -> tuple[SensorTemperatureSetting, ...]:
        self._validate_grant(grant)
        loaded = tuple(
            self._repository.load(
                grant.user_id,
                grant.workplace_id,
                grant.device_id,
            )
        )
        self._validate_loaded_scope(grant, loaded)
        return loaded

    def update(
        self,
        grant: LimitedSessionGrant,
        updates: Sequence[SensorTemperatureUpdate],
    ) -> tuple[SensorTemperatureSetting, ...]:
        self._validate_grant(grant)
        normalized = tuple(updates)
        if not all(
            isinstance(update, SensorTemperatureUpdate)
            for update in normalized
        ):
            raise SettingsScopeError(
                "temperature updates are outside the limited scope"
            )
        update_sensor_ids = tuple(
            update.sensor_pk for update in normalized
        )
        if (
            not normalized
            or len(set(update_sensor_ids)) != len(update_sensor_ids)
            or not set(update_sensor_ids).issubset(grant.sensor_ids)
        ):
            raise SettingsScopeError(
                "temperature updates are outside the limited scope"
            )
        loaded = tuple(
            self._repository.update(
                grant.user_id,
                grant.workplace_id,
                grant.device_id,
                normalized,
            )
        )
        self._validate_loaded_scope(grant, loaded)
        return loaded

    @staticmethod
    def _validate_grant(grant: LimitedSessionGrant) -> None:
        if not isinstance(grant, LimitedSessionGrant):
            raise SettingsScopeError("limited settings grant is required")
        try:
            device_id = _positive_int(grant.device_id, "device")
            _positive_int(grant.user_id, "user")
            _positive_int(grant.workplace_id, "workplace")
            sensor_ids_are_positive = all(
                _positive_int(sensor_id, "sensor")
                for sensor_id in grant.sensor_ids
            )
        except (SettingsValidationError, TypeError):
            raise SettingsScopeError(
                "limited settings grant is outside the device scope"
            ) from None
        match = (
            _SETTINGS_PATH.fullmatch(grant.target_path)
            if isinstance(grant.target_path, str)
            else None
        )
        if (
            grant.purpose is not LinkPurpose.SETTINGS
            or match is None
            or int(match.group(1)) != device_id
            or not isinstance(grant.sensor_ids, tuple)
            or not grant.sensor_ids
            or len(set(grant.sensor_ids)) != len(grant.sensor_ids)
            or not sensor_ids_are_positive
            or not isinstance(grant.csrf_token, str)
            or _CSRF_TOKEN.fullmatch(grant.csrf_token) is None
        ):
            raise SettingsScopeError(
                "limited settings grant is outside the device scope"
            )

    @staticmethod
    def _validate_loaded_scope(
        grant: LimitedSessionGrant,
        loaded: tuple[SensorTemperatureSetting, ...],
    ) -> None:
        if (
            not loaded
            or not all(
                isinstance(item, SensorTemperatureSetting)
                for item in loaded
            )
        ):
            raise SettingsScopeError(
                "temperature settings result is malformed"
            )
        sensor_ids = tuple(item.sensor_pk for item in loaded)
        if (
            len(set(sensor_ids)) != len(sensor_ids)
            or set(sensor_ids) != set(grant.sensor_ids)
        ):
            raise SettingsScopeError(
                "temperature settings result escaped the sensor scope"
            )


def format_upper_limit(value: Decimal) -> str:
    return f"{_validate_upper_limit(value):.1f}"


def _positive_int(value: object, name: str) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < 1
    ):
        raise SettingsValidationError(f"{name} must be a positive integer")
    return value


def _notification_count(value: object) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < 1
        or value > 3
    ):
        raise SettingsValidationError(
            "max notifications must be between 1 and 3"
        )
    return value


def _finite_decimal(value: object, name: str) -> Decimal:
    if isinstance(value, bool):
        raise SettingsValidationError(f"{name} must be numeric")
    try:
        decimal_value = Decimal(str(value))
    except (InvalidOperation, ValueError):
        raise SettingsValidationError(
            f"{name} must be numeric"
        ) from None
    if not decimal_value.is_finite():
        raise SettingsValidationError(f"{name} must be finite")
    return decimal_value


def _validate_upper_limit(value: object) -> Decimal:
    decimal_value = _finite_decimal(value, "upper limit")
    if (
        decimal_value < _MIN_UPPER_LIMIT
        or decimal_value > _MAX_UPPER_LIMIT
        or decimal_value % _HALF_DEGREE != 0
    ):
        raise SettingsValidationError(
            "upper limit must be -50..25 in 0.5 degree steps"
        )
    return decimal_value.quantize(Decimal("0.1"))
