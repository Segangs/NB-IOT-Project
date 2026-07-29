#include "tasks_periodic_modem.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "../boot_v2/command_periodic_schedule_core.hpp"
#include "../boot_v2/runtime_owner_producer_facade.hpp"
#include "../boot_v2/runtime_owner_rtos.hpp"
#include "../boot_v2/sensor_quality_core.hpp"
#include "../config.h"
#include "../lib/log.hpp"
#include "tasks_sensor_reader.hpp"

namespace {

void increment_nonzero(std::uint32_t &value) noexcept
{
    if (value == std::numeric_limits<std::uint32_t>::max()) {
        value = 1;
    } else {
        ++value;
        if (value == 0) {
            value = 1;
        }
    }
}

bool publish_periodic_sensor_snapshot(
    const std::uint32_t sensor_id,
    const std::size_t channel,
    std::uint32_t &telemetry_revision) noexcept
{
    boot_v2::SensorQualitySnapshotV1 snapshot{};
    if (!copy_sensor_quality_snapshot(channel, snapshot) ||
        !boot_v2::sensor_quality_allows_telemetry(snapshot)) {
        return false;
    }

    increment_nonzero(telemetry_revision);
    return boot_v2::runtime_owner_periodic_publish_telemetry(
               sensor_id,
               telemetry_revision,
               snapshot.value_deci_celsius) ==
           boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery;
}

bool pull_periodic_config(void *) noexcept
{
    return boot_v2::runtime_owner_periodic_pull_config() ==
           boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery;
}

bool pull_periodic_command(void *) noexcept
{
    return boot_v2::runtime_owner_periodic_pull_command() ==
           boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery;
}

} // namespace

void vPeriodicModemTask(void *)
{
    while (boot_v2::runtime_owner_redacted_status().runtime_ready == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    LOG("PERIODIC_READY\n");

    constexpr std::uint32_t kPostConfigFirstTelemetryDelayMs = 30000;
    const std::uint32_t periodic_ready_ms =
        to_ms_since_boot(get_absolute_time());
    std::uint32_t last_rssi_ms = periodic_ready_ms;
    std::uint32_t last_telemetry_ms = last_rssi_ms;
    std::uint32_t last_reconnect_ms = last_rssi_ms;
    std::uint32_t telemetry_revision = 0;
    bool first_telemetry_pending = true;
    boot_v2::CommandPeriodicStepper command_stepper{
        periodic_ready_ms};
    const boot_v2::CommandPeriodicStepPort command_port{
        nullptr, pull_periodic_config, pull_periodic_command};

    for (;;) {
        const std::uint32_t now = to_ms_since_boot(get_absolute_time());
        const boot_v2::RuntimeOwnerRedactedStatus status =
            boot_v2::runtime_owner_redacted_status();

        if (status.phase == boot_v2::RuntimeOwnerPhase::RecoveryPending &&
            now - last_reconnect_ms >= 5000) {
            (void)boot_v2::runtime_owner_periodic_request_transport();
            last_reconnect_ms = now;
        }

        if (status.runtime_ready != 0 &&
            now - last_rssi_ms >=
                MODEM_RSSI_CHECK_INTERVAL_MIN * 60u * 1000u) {
            (void)boot_v2::runtime_owner_periodic_refresh_rssi();
            last_rssi_ms = now;
        }

        const bool first_telemetry_due =
            first_telemetry_pending &&
            now - periodic_ready_ms >= kPostConfigFirstTelemetryDelayMs;
        if (status.runtime_ready != 0 && first_telemetry_due) {
            LOG("PERIODIC_FIRST_TELEMETRY\n");
            (void)publish_periodic_sensor_snapshot(
                1, 0, telemetry_revision);
            first_telemetry_pending = false;
            last_telemetry_ms = now;
        } else if (status.runtime_ready != 0 && !first_telemetry_pending &&
                   now - last_telemetry_ms >=
                       SENSOR_TEMP_CHECK_INTERVAL_MIN * 60u * 1000u) {
            (void)publish_periodic_sensor_snapshot(
                1, 0, telemetry_revision);
            (void)publish_periodic_sensor_snapshot(
                2, 1, telemetry_revision);
            last_telemetry_ms = now;
        }

        (void)command_stepper.step(
            {now,
             status.runtime_ready,
             static_cast<std::uint8_t>(
                 status.phase ==
                 boot_v2::RuntimeOwnerPhase::RecoveryPending),
             {}},
            command_port);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
