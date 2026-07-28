#include "tasks_sensor_reader.hpp"

#include <atomic>
#include <cmath>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tasks_sensor.hpp"
#include "../config.h"
#include "../boot_v2/sensor_quality_core.hpp"
#include "../boot_v2/temperature_alarm_publish_core.hpp"
#include "../boot_v2/runtime_owner_producer_facade.hpp"
#include "../lib/log.hpp"
#include "app_context.hpp"
#include "pico/stdlib.h"

namespace {

std::atomic_flag g_sensor_quality_lock = ATOMIC_FLAG_INIT;
boot_v2::SensorQualitySnapshotV1 g_sensor_quality_snapshots[2]{};

boot_v2::SensorSampleFault sample_fault(const int status) noexcept
{
    switch (status) {
    case 0:
        return boot_v2::SensorSampleFault::None;
    case 1:
        return boot_v2::SensorSampleFault::NoPresence;
    case 2:
        return boot_v2::SensorSampleFault::CrcMismatch;
    case 3:
        return boot_v2::SensorSampleFault::OutOfRange;
    case 4:
        return boot_v2::SensorSampleFault::Busy;
    case 5:
        return boot_v2::SensorSampleFault::StuckLow;
    default:
        return boot_v2::SensorSampleFault::OutOfRange;
    }
}

std::int16_t temperature_deci(const float value) noexcept
{
    return static_cast<std::int16_t>(std::lround(value * 10.0f));
}

boot_v2::SensorSample sample(
    const float value,
    const int status,
    const std::uint32_t observed_at_ms) noexcept
{
    boot_v2::SensorSample result{};
    result.fault = sample_fault(status);
    result.observed_at_monotonic_ms = observed_at_ms;
    if (status == 0) {
        result.value_deci_celsius = temperature_deci(value);
    }
    return result;
}

void publish_sensor_quality(
    const boot_v2::SensorQualitySnapshotV1 first,
    const boot_v2::SensorQualitySnapshotV1 second) noexcept
{
    while (g_sensor_quality_lock.test_and_set(std::memory_order_acquire)) {
    }
    g_sensor_quality_snapshots[0] = first;
    g_sensor_quality_snapshots[1] = second;
    g_sensor_quality_lock.clear(std::memory_order_release);
}

} // namespace

bool copy_sensor_quality_snapshot(
    const std::size_t channel,
    boot_v2::SensorQualitySnapshotV1 &snapshot) noexcept
{
    if (channel >= 2 ||
        g_sensor_quality_lock.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    snapshot = g_sensor_quality_snapshots[channel];
    g_sensor_quality_lock.clear(std::memory_order_release);
    return snapshot.health != boot_v2::SnapshotHealth::Unknown;
}

// ====================================================================================
// Core 0 Task: Sensor Reader Task (Core 0)
// ====================================================================================
void vSensorTask(void *pvParameters)
{
    uint32_t serial_print_counter = 0;
    boot_v2::SensorQualityCore quality_ch0;
    boot_v2::SensorQualityCore quality_ch1;
    boot_v2::TemperatureAlarmPublishCore alert_publish_ch0;
    boot_v2::TemperatureAlarmPublishCore alert_publish_ch1;
    bool alarm_ch0 = false;
    bool alarm_ch1 = false;

    while (true)
    {
        // Core 1의 ADC 직접 간섭을 예방하기 위해 Core 0에서 전압도 수집하여 전역 공유합니다.
        bool vsys_stable = false;
        float vsys_vol = read_vsys_voltage(vsys_stable);
        lcd_params.current_vsys_voltage = vsys_vol;
        lcd_params.is_vsys_stable = vsys_stable;

        float temp_ch0 = -999.0f;
        float temp_ch1 = -999.0f;
        int status_ch0 = 1;
        int status_ch1 = 1;

        bool sensor_read_enabled =
            !lcd_params.is_booting &&
            to_ms_since_boot(get_absolute_time()) >= DS18B20_BOOT_DELAY_MS;

        if (sensor_read_enabled) {
            check_temperature_status_dual(temp_ch0, status_ch0, temp_ch1, status_ch1);
        }

        const std::uint32_t observed_at_ms =
            to_ms_since_boot(get_absolute_time());
        boot_v2::SensorQualityDecision quality0{};
        boot_v2::SensorQualityDecision quality1{};
        const bool quality0_updated = quality_ch0.observe(
            sample(temp_ch0, status_ch0, observed_at_ms), quality0);
        const bool quality1_updated = quality_ch1.observe(
            sample(temp_ch1, status_ch1, observed_at_ms), quality1);
        if (!quality0_updated || !quality1_updated) {
            LOG("SENSOR_QUALITY_REJECT\n");
        }
        publish_sensor_quality(quality0.snapshot, quality1.snapshot);

        if (quality0.display_value_valid != 0) {
            temp_ch0 =
                static_cast<float>(quality0.display_value_deci_celsius) /
                10.0f;
            lcd_params.current_temperature = temp_ch0;
        } else {
            lcd_params.current_temperature = -990.0f - (float)status_ch0;
        }
        if (quality0.alarm_update_allowed != 0) {
            g_temp_ch0_sample_seq++;
            if (g_temp_ch0_sample_seq == 0) {
                g_temp_ch0_sample_seq = 1;
            }
            alarm_ch0 = temp_ch0 > g_temp_upper_limit_ch0;
        }
        const auto alert0 = alert_publish_ch0.observe(
            quality0.alarm_update_allowed != 0, alarm_ch0);
        if (alert0.publish_required != 0 &&
            boot_v2::runtime_owner_sensor_publish_telemetry(
                1, g_temp_ch0_sample_seq) ==
                boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery) {
            alert_publish_ch0.confirm_submitted();
        }
        lcd_params.status_ch0 = status_ch0;

        if (quality1.display_value_valid != 0) {
            temp_ch1 =
                static_cast<float>(quality1.display_value_deci_celsius) /
                10.0f;
            lcd_params.current_temperature_ch1 = temp_ch1;
        } else {
            lcd_params.current_temperature_ch1 = -990.0f - (float)status_ch1;
        }
        if (quality1.alarm_update_allowed != 0) {
            g_temp_ch1_sample_seq++;
            if (g_temp_ch1_sample_seq == 0) {
                g_temp_ch1_sample_seq = 1;
            }
            alarm_ch1 = temp_ch1 > g_temp_upper_limit_ch1;
        }
        const auto alert1 = alert_publish_ch1.observe(
            quality1.alarm_update_allowed != 0, alarm_ch1);
        if (alert1.publish_required != 0 &&
            boot_v2::runtime_owner_sensor_publish_telemetry(
                2, g_temp_ch1_sample_seq) ==
                boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery) {
            alert_publish_ch1.confirm_submitted();
        }
        lcd_params.status_ch1 = status_ch1;

        // Stale/fault samples neither raise nor clear an existing alarm.
        g_buzzer_trigger = alarm_ch0 || alarm_ch1;

        // 시리얼 로그 출력 (1분 주기)
        if (serial_print_counter % 60 == 0) {
            if (g_sensor_count >= 2) {
                LOG("SENSOR_SAMPLE %.1f,%d %.1f,%d VSYS=%.2f PWR=%d ALARM=%d LIMIT=%.1f,%.1f\n",
                       temp_ch0, status_ch0, temp_ch1, status_ch1, vsys_vol, vsys_stable, g_buzzer_trigger,
                       g_temp_upper_limit_ch0, g_temp_upper_limit_ch1);
            } else {
                LOG("SENSOR_SAMPLE %.1f,%d VSYS=%.2f PWR=%d ALARM=%d LIMIT=%.1f\n",
                       temp_ch0, status_ch0, vsys_vol, vsys_stable, g_buzzer_trigger, g_temp_upper_limit_ch0);
            }
        }

        serial_print_counter++;
        vTaskDelay(pdMS_TO_TICKS(DS18B20_SAMPLE_INTERVAL_MS));
    }
}
