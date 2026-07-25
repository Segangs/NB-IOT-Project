#include "sensor_quality_core.hpp"

#include <limits>

namespace boot_v2 {
namespace {

constexpr std::int16_t kMinimumTemperatureDeciCelsius = -550;
constexpr std::int16_t kMaximumTemperatureDeciCelsius = 1250;
constexpr std::uint16_t kCrcFallbackMaxConsecutive = 3;
constexpr std::uint32_t kCrcFallbackMaxAgeMs = 30000;

bool known_fault(const SensorSampleFault fault) noexcept
{
    return fault >= SensorSampleFault::None &&
           fault <= SensorSampleFault::StuckLow;
}

bool valid_sample(const SensorSample sample) noexcept
{
    if (!known_fault(sample.fault) || sample.clock_valid > 1 ||
        sample.reserved != 0) {
        return false;
    }
    if (sample.clock_valid == 0 &&
        sample.observed_at_unix_seconds != 0) {
        return false;
    }
    if (sample.fault == SensorSampleFault::None) {
        return sample.value_deci_celsius >=
                   kMinimumTemperatureDeciCelsius &&
               sample.value_deci_celsius <=
                   kMaximumTemperatureDeciCelsius;
    }
    return sample.value_deci_celsius == 0 &&
           sample.observed_at_unix_seconds == 0;
}

std::uint16_t increment_saturating(const std::uint16_t value) noexcept
{
    return value == std::numeric_limits<std::uint16_t>::max()
               ? value
               : static_cast<std::uint16_t>(value + 1);
}

SensorQualitySnapshotV1 failed_snapshot(
    const std::uint16_t consecutive_failures,
    const std::uint8_t last_valid_clock,
    const std::uint32_t last_valid_unix_seconds) noexcept
{
    SensorQualitySnapshotV1 snapshot{};
    snapshot.health = SnapshotHealth::Failed;
    snapshot.stale = 1;
    snapshot.consecutive_failures = consecutive_failures;
    snapshot.clock_valid = last_valid_clock;
    snapshot.last_valid_at_unix_seconds =
        last_valid_clock != 0 ? last_valid_unix_seconds : 0;
    return snapshot;
}

bool complete_pair_temperature(
    const SensorQualitySnapshotV1 &snapshot) noexcept
{
    return snapshot.health == SnapshotHealth::Pass &&
           snapshot.has_value == 1 &&
           snapshot.value_source == SensorValueSource::Fresh &&
           snapshot.stale == 0;
}

} // namespace

SensorQualityCore::SensorQualityCore() noexcept
{
    latest_.snapshot.health = SnapshotHealth::Failed;
    latest_.snapshot.stale = 1;
}

bool SensorQualityCore::observe(
    const SensorSample sample,
    SensorQualityDecision &decision) noexcept
{
    if (!valid_sample(sample)) {
        decision = latest_;
        return false;
    }

    SensorQualityDecision next{};
    if (sample.fault == SensorSampleFault::None) {
        has_last_valid_ = 1;
        last_valid_value_ = sample.value_deci_celsius;
        last_valid_monotonic_ms_ = sample.observed_at_monotonic_ms;
        last_valid_clock_ = sample.clock_valid;
        last_valid_unix_seconds_ =
            sample.clock_valid != 0
                ? sample.observed_at_unix_seconds
                : 0;
        consecutive_failures_ = 0;

        next.snapshot.health = SnapshotHealth::Pass;
        next.snapshot.has_value = 1;
        next.snapshot.value_source = SensorValueSource::Fresh;
        next.snapshot.value_deci_celsius =
            sample.value_deci_celsius;
        next.snapshot.clock_valid = sample.clock_valid;
        next.snapshot.last_valid_at_unix_seconds =
            last_valid_unix_seconds_;
        next.display_value_valid = 1;
        next.alarm_update_allowed = 1;
        next.display_value_deci_celsius =
            sample.value_deci_celsius;
    } else {
        consecutive_failures_ =
            increment_saturating(consecutive_failures_);
        const std::uint32_t age_ms =
            sample.observed_at_monotonic_ms -
            last_valid_monotonic_ms_;
        const bool crc_fallback =
            sample.fault == SensorSampleFault::CrcMismatch &&
            has_last_valid_ != 0 &&
            consecutive_failures_ <= kCrcFallbackMaxConsecutive &&
            age_ms <= kCrcFallbackMaxAgeMs;

        if (crc_fallback) {
            next.snapshot.health = SnapshotHealth::Degraded;
            next.snapshot.has_value = 1;
            next.snapshot.value_source =
                SensorValueSource::CrcFallback;
            next.snapshot.stale = 1;
            next.snapshot.clock_valid = last_valid_clock_;
            next.snapshot.value_deci_celsius =
                last_valid_value_;
            next.snapshot.consecutive_failures =
                consecutive_failures_;
            next.snapshot.last_valid_at_unix_seconds =
                last_valid_clock_ != 0
                    ? last_valid_unix_seconds_
                    : 0;
            next.display_value_valid = 1;
            next.display_value_deci_celsius =
                last_valid_value_;
        } else {
            next.snapshot = failed_snapshot(
                consecutive_failures_,
                last_valid_clock_,
                last_valid_unix_seconds_);
        }
    }

    latest_ = next;
    decision = next;
    return true;
}

const SensorQualityDecision &SensorQualityCore::latest() const noexcept
{
    return latest_;
}

bool sensor_quality_allows_telemetry(
    const SensorQualitySnapshotV1 &snapshot) noexcept
{
    return complete_pair_temperature(snapshot);
}

SnapshotHealth combined_sensor_health(
    const SensorQualitySnapshotV1 &temp1,
    const bool mic1_healthy,
    const SensorQualitySnapshotV1 &temp2,
    const bool mic2_healthy) noexcept
{
    const bool port1_complete =
        complete_pair_temperature(temp1) && mic1_healthy;
    const bool port2_complete =
        complete_pair_temperature(temp2) && mic2_healthy;
    return port1_complete || port2_complete
               ? SnapshotHealth::Pass
               : SnapshotHealth::Degraded;
}

} // namespace boot_v2
