#include "tasks_boot.hpp"

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "../boot_v2/runtime_owner_producer_facade.hpp"
#include "../boot_v2/runtime_owner_rtos.hpp"
#include "../boot_v2/runtime_owner_shutdown_record_store.hpp"
#include "../lib/flash_logger.hpp"
#include "../lib/log.hpp"
#include "app_context.hpp"
#include "tasks_sensor.hpp"

namespace {

constexpr std::uint32_t kBootOwnerDeadlineMs = 300000;

bool submit_transport_request() noexcept
{
    const boot_v2::RuntimeOwnerIngressResult result =
        boot_v2::runtime_owner_boot_request_transport();
    return result ==
           boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery;
}

} // namespace

void vBootTask(void *)
{
    LOG("BOOT\n");
    init_fixed_sensor_map();

    std::strncpy(
        lcd_params.status_text,
        "Check Pico",
        sizeof(lcd_params.status_text) - 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    boot_v2::runtime_owner_shutdown_record_log_current();

    bool vsys_stable = false;
    const float pico_voltage = read_vsys_voltage(vsys_stable);
    lcd_params.current_vsys_voltage = pico_voltage;
    lcd_params.is_vsys_stable = vsys_stable;
    flash_log_write(0.0f, pico_voltage, 0, 0, 0, 99);

    bool chip_temp_ok = false;
    g_boot_pico_temperature = read_internal_temp(chip_temp_ok);
    std::uint32_t flash_checksum = 0;
    const bool flash_ok = check_flash_integrity(flash_checksum);
    g_boot_flash_integrity = flash_ok ? 0 : 1;
    LOG("SELFTEST %s\n",
        (vsys_stable && chip_temp_ok && flash_ok) ? "OK" : "WARN");

    if (!submit_transport_request()) {
        LOG("BOOT_OWNER_SUBMIT_FAIL\n");
        std::strncpy(
            lcd_params.status_text,
            "Boot Error",
            sizeof(lcd_params.status_text) - 1);
        lcd_params.is_booting = false;
        vTaskDelete(nullptr);
        return;
    }

    std::uint32_t elapsed_ms = 0;
    std::uint32_t retry_elapsed_ms = 0;
    while (elapsed_ms < kBootOwnerDeadlineMs) {
        const boot_v2::RuntimeOwnerRedactedStatus status =
            boot_v2::runtime_owner_redacted_status();
        if (status.runtime_ready != 0) {
            LOG("BOOT_DONE\n");
            vTaskDelete(nullptr);
            return;
        }
        if (status.state == boot_v2::RuntimeOwnerTaskState::Terminal) {
            break;
        }
        if (status.phase == boot_v2::RuntimeOwnerPhase::RecoveryPending &&
            retry_elapsed_ms >= 5000) {
            (void)submit_transport_request();
            retry_elapsed_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed_ms += 100;
        retry_elapsed_ms += 100;
    }

    LOG("BOOT_OWNER_TIMEOUT\n");
    std::strncpy(
        lcd_params.status_text,
        "Boot Error",
        sizeof(lcd_params.status_text) - 1);
    lcd_params.is_booting = false;
    vTaskDelete(nullptr);
}
