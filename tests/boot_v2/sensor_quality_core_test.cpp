#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "../../src/boot_v2/sensor_quality_core.hpp"

namespace {

std::uint64_t checks = 0;

#define CHECK(expression)                                                       \
    do {                                                                        \
        ++checks;                                                               \
        if (!(expression)) {                                                    \
            std::cerr << "CHECK failed: " #expression << " at " << __FILE__   \
                      << ':' << __LINE__ << '\n';                                \
            std::exit(1);                                                       \
        }                                                                       \
    } while (false)

using boot_v2::SensorQualityCore;
using boot_v2::SensorQualityDecision;
using boot_v2::SensorSample;
using boot_v2::SensorSampleFault;
using boot_v2::SensorValueSource;
using boot_v2::SnapshotHealth;

SensorSample fresh(
    const std::int16_t value,
    const std::uint32_t monotonic_ms,
    const std::uint32_t unix_seconds = 0,
    const std::uint8_t clock_valid = 0) noexcept
{
    return {
        SensorSampleFault::None,
        value,
        clock_valid,
        0,
        monotonic_ms,
        unix_seconds,
    };
}

SensorSample fault(
    const SensorSampleFault kind,
    const std::uint32_t monotonic_ms) noexcept
{
    return {kind, 0, 0, 0, monotonic_ms, 0};
}

void check_default_state_is_canonical_failed()
{
    SensorQualityCore core;
    const SensorQualityDecision &decision = core.latest();
    CHECK(decision.snapshot.health == SnapshotHealth::Failed);
    CHECK(decision.snapshot.has_value == 0);
    CHECK(decision.snapshot.value_source == SensorValueSource::None);
    CHECK(decision.snapshot.stale == 1);
    CHECK(decision.display_value_valid == 0);
    CHECK(decision.alarm_update_allowed == 0);
}

void check_fresh_and_clock_contract()
{
    SensorQualityCore core;
    SensorQualityDecision decision{};
    CHECK(core.observe(fresh(-75, 1000, 123456, 1), decision));
    CHECK(decision.snapshot.health == SnapshotHealth::Pass);
    CHECK(decision.snapshot.has_value == 1);
    CHECK(decision.snapshot.value_source == SensorValueSource::Fresh);
    CHECK(decision.snapshot.stale == 0);
    CHECK(decision.snapshot.consecutive_failures == 0);
    CHECK(decision.snapshot.value_deci_celsius == -75);
    CHECK(decision.snapshot.clock_valid == 1);
    CHECK(decision.snapshot.last_valid_at_unix_seconds == 123456);
    CHECK(decision.display_value_valid == 1);
    CHECK(decision.display_value_deci_celsius == -75);
    CHECK(decision.alarm_update_allowed == 1);
    CHECK(boot_v2::sensor_quality_allows_telemetry(decision.snapshot));
}

void check_crc_fallback_is_bounded_and_not_telemetry()
{
    SensorQualityCore core;
    SensorQualityDecision decision{};
    CHECK(core.observe(fresh(250, 1000), decision));

    for (std::uint32_t count = 1; count <= 3; ++count) {
        CHECK(core.observe(
            fault(SensorSampleFault::CrcMismatch, 1000 + count * 5000),
            decision));
        CHECK(decision.snapshot.health == SnapshotHealth::Degraded);
        CHECK(decision.snapshot.has_value == 1);
        CHECK(decision.snapshot.value_source ==
              SensorValueSource::CrcFallback);
        CHECK(decision.snapshot.stale == 1);
        CHECK(decision.snapshot.consecutive_failures == count);
        CHECK(decision.snapshot.value_deci_celsius == 250);
        CHECK(decision.display_value_valid == 1);
        CHECK(decision.display_value_deci_celsius == 250);
        CHECK(decision.alarm_update_allowed == 0);
        CHECK(!boot_v2::sensor_quality_allows_telemetry(decision.snapshot));
    }

    CHECK(core.observe(
        fault(SensorSampleFault::CrcMismatch, 17000), decision));
    CHECK(decision.snapshot.health == SnapshotHealth::Failed);
    CHECK(decision.snapshot.has_value == 0);
    CHECK(decision.snapshot.value_source == SensorValueSource::None);
    CHECK(decision.snapshot.stale == 1);
    CHECK(decision.snapshot.consecutive_failures == 4);
    CHECK(decision.display_value_valid == 0);
    CHECK(decision.alarm_update_allowed == 0);
}

void check_crc_age_and_history_boundaries()
{
    SensorQualityCore core;
    SensorQualityDecision decision{};
    CHECK(core.observe(
        fault(SensorSampleFault::CrcMismatch, 100), decision));
    CHECK(decision.snapshot.health == SnapshotHealth::Failed);
    CHECK(decision.display_value_valid == 0);

    CHECK(core.observe(fresh(10, 1000), decision));
    CHECK(core.observe(
        fault(SensorSampleFault::CrcMismatch, 31000), decision));
    CHECK(decision.snapshot.health == SnapshotHealth::Degraded);
    CHECK(decision.display_value_valid == 1);

    SensorQualityCore expired;
    CHECK(expired.observe(fresh(10, 1000), decision));
    CHECK(expired.observe(
        fault(SensorSampleFault::CrcMismatch, 31001), decision));
    CHECK(decision.snapshot.health == SnapshotHealth::Failed);
    CHECK(decision.display_value_valid == 0);
}

void check_non_crc_faults_never_use_fallback()
{
    const SensorSampleFault faults[] = {
        SensorSampleFault::NoPresence,
        SensorSampleFault::OutOfRange,
        SensorSampleFault::Busy,
        SensorSampleFault::StuckLow,
    };
    for (const SensorSampleFault value : faults) {
        SensorQualityCore core;
        SensorQualityDecision decision{};
        CHECK(core.observe(fresh(210, 1000), decision));
        CHECK(core.observe(fault(value, 1100), decision));
        CHECK(decision.snapshot.health == SnapshotHealth::Failed);
        CHECK(decision.snapshot.has_value == 0);
        CHECK(decision.snapshot.value_source == SensorValueSource::None);
        CHECK(decision.display_value_valid == 0);
        CHECK(decision.alarm_update_allowed == 0);
    }
}

void check_invalid_input_is_rejected_without_state_change()
{
    SensorQualityCore core;
    SensorQualityDecision decision{};
    CHECK(core.observe(fresh(100, 1000), decision));
    const auto before = decision.snapshot;

    SensorSample invalid = fresh(1300, 2000);
    CHECK(!core.observe(invalid, decision));
    CHECK(boot_v2::sensor_quality_snapshots_equal(before, decision.snapshot));

    invalid = fresh(100, 2000);
    invalid.clock_valid = 2;
    CHECK(!core.observe(invalid, decision));
    CHECK(boot_v2::sensor_quality_snapshots_equal(before, decision.snapshot));

    invalid = fresh(100, 2000);
    invalid.reserved = 1;
    CHECK(!core.observe(invalid, decision));
    CHECK(boot_v2::sensor_quality_snapshots_equal(before, decision.snapshot));
}

void check_pair_health_policy()
{
    SensorQualityCore first;
    SensorQualityCore second;
    SensorQualityDecision temp1{};
    SensorQualityDecision temp2{};
    CHECK(first.observe(fresh(100, 1000), temp1));
    CHECK(second.observe(
        fault(SensorSampleFault::NoPresence, 1000), temp2));

    CHECK(boot_v2::combined_sensor_health(
              temp1.snapshot, true, temp2.snapshot, false) ==
          SnapshotHealth::Pass);
    CHECK(boot_v2::combined_sensor_health(
              temp1.snapshot, false, temp2.snapshot, false) ==
          SnapshotHealth::Degraded);
    CHECK(boot_v2::combined_sensor_health(
              temp1.snapshot, false, temp2.snapshot, true) ==
          SnapshotHealth::Degraded);
}

} // namespace

int main()
{
    check_default_state_is_canonical_failed();
    check_fresh_and_clock_contract();
    check_crc_fallback_is_bounded_and_not_telemetry();
    check_crc_age_and_history_boundaries();
    check_non_crc_faults_never_use_fallback();
    check_invalid_input_is_rejected_without_state_change();
    check_pair_health_policy();
    std::cout << "sensor_quality_core_test checks=" << checks << '\n';
    return 0;
}
