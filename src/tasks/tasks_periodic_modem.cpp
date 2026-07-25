#include "tasks_periodic_modem.hpp"

#include <cstdint>
#include <limits>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "../boot_v2/runtime_owner_producer_facade.hpp"
#include "../boot_v2/runtime_owner_rtos.hpp"
#include "../config.h"
#include "../lib/log.hpp"

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
    std::uint32_t last_config_ms = last_rssi_ms;
    std::uint32_t last_reconnect_ms = last_rssi_ms;
    std::uint32_t telemetry_revision = 0;
    bool first_telemetry_pending = true;

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
            increment_nonzero(telemetry_revision);
            (void)boot_v2::runtime_owner_periodic_publish_telemetry(
                1, telemetry_revision);
            first_telemetry_pending = false;
            last_telemetry_ms = now;
        } else if (status.runtime_ready != 0 && !first_telemetry_pending &&
                   now - last_telemetry_ms >=
                       SENSOR_TEMP_CHECK_INTERVAL_MIN * 60u * 1000u) {
            increment_nonzero(telemetry_revision);
            (void)boot_v2::runtime_owner_periodic_publish_telemetry(
                1, telemetry_revision);
            increment_nonzero(telemetry_revision);
            (void)boot_v2::runtime_owner_periodic_publish_telemetry(
                2, telemetry_revision);
            last_telemetry_ms = now;
        }

        if (status.runtime_ready != 0 && now - last_config_ms >= 60000) {
            (void)boot_v2::runtime_owner_periodic_pull_config();
            last_config_ms = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
