#ifndef NB_IOT_BOOT_V2_SENSOR_QUALITY_CORE_HPP
#define NB_IOT_BOOT_V2_SENSOR_QUALITY_CORE_HPP

#include <cstdint>

#include "runtime_snapshot_core.hpp"

namespace boot_v2 {

enum class SensorSampleFault : std::uint8_t {
    None = 0,
    NoPresence = 1,
    CrcMismatch = 2,
    OutOfRange = 3,
    Busy = 4,
    StuckLow = 5,
};

struct SensorSample {
    SensorSampleFault fault{SensorSampleFault::NoPresence};
    std::int16_t value_deci_celsius{0};
    std::uint8_t clock_valid{0};
    std::uint8_t reserved{0};
    std::uint32_t observed_at_monotonic_ms{0};
    std::uint32_t observed_at_unix_seconds{0};
};

struct SensorQualityDecision {
    SensorQualitySnapshotV1 snapshot{};
    std::uint8_t display_value_valid{0};
    std::uint8_t alarm_update_allowed{0};
    std::uint8_t reserved[2]{};
    std::int16_t display_value_deci_celsius{0};
};

class SensorQualityCore {
public:
    SensorQualityCore() noexcept;
    SensorQualityCore(const SensorQualityCore &) = delete;
    SensorQualityCore &operator=(const SensorQualityCore &) = delete;
    SensorQualityCore(SensorQualityCore &&) = delete;
    SensorQualityCore &operator=(SensorQualityCore &&) = delete;
    ~SensorQualityCore() noexcept = default;

    [[nodiscard]] bool observe(
        SensorSample sample,
        SensorQualityDecision &decision) noexcept;
    [[nodiscard]] const SensorQualityDecision &latest() const noexcept;

private:
    SensorQualityDecision latest_{};
    std::int16_t last_valid_value_{0};
    std::uint16_t consecutive_failures_{0};
    std::uint32_t last_valid_monotonic_ms_{0};
    std::uint32_t last_valid_unix_seconds_{0};
    std::uint8_t has_last_valid_{0};
    std::uint8_t last_valid_clock_{0};
};

[[nodiscard]] bool sensor_quality_allows_telemetry(
    const SensorQualitySnapshotV1 &snapshot) noexcept;

[[nodiscard]] SnapshotHealth combined_sensor_health(
    const SensorQualitySnapshotV1 &temp1,
    bool mic1_healthy,
    const SensorQualitySnapshotV1 &temp2,
    bool mic2_healthy) noexcept;

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_SENSOR_QUALITY_CORE_HPP
