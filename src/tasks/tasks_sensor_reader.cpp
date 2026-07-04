#include "tasks_sensor_reader.hpp"

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tasks_sensor.hpp"
#include "../config.h"
#include "../lib/log.hpp"
#include "app_context.hpp"
#include "pico/stdlib.h"

// ====================================================================================
// Core 0 Task: Sensor Reader Task (Core 0)
// ====================================================================================
void vSensorTask(void *pvParameters)
{
    uint32_t serial_print_counter = 0;
    float last_valid_ch0 = -999.0f;
    float last_valid_ch1 = -999.0f;
    int last_status_ch0 = 1;
    int last_status_ch1 = 1;
    uint8_t fail_count_ch0 = 0;
    uint8_t fail_count_ch1 = 0;

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

        // Ch0 파라미터 매핑
        if (status_ch0 == 0) {
            last_valid_ch0 = temp_ch0;
            last_status_ch0 = 0;
            fail_count_ch0 = 0;
            lcd_params.current_temperature = temp_ch0;
            g_temp_ch0_sample_seq++;
        } else if (last_status_ch0 == 0 && fail_count_ch0 < 3) {
            fail_count_ch0++;
            temp_ch0 = last_valid_ch0;
            status_ch0 = 0;
            lcd_params.current_temperature = last_valid_ch0;
        } else {
            last_status_ch0 = status_ch0;
            lcd_params.current_temperature = -990.0f - (float)status_ch0;
        }
        lcd_params.status_ch0 = status_ch0;

        // Ch1 파라미터 매핑
        if (status_ch1 == 0) {
            last_valid_ch1 = temp_ch1;
            last_status_ch1 = 0;
            fail_count_ch1 = 0;
            lcd_params.current_temperature_ch1 = temp_ch1;
            g_temp_ch1_sample_seq++;
        } else if (last_status_ch1 == 0 && fail_count_ch1 < 3) {
            fail_count_ch1++;
            temp_ch1 = last_valid_ch1;
            status_ch1 = 0;
            lcd_params.current_temperature_ch1 = last_valid_ch1;
        } else {
            last_status_ch1 = status_ch1;
            lcd_params.current_temperature_ch1 = -990.0f - (float)status_ch1;
        }
        lcd_params.status_ch1 = status_ch1;

        // 경보 여부 판단 (활성화된 센서 중 하나라도 임계치를 넘으면 발생)
        bool alarm_active = false;
        if (status_ch0 == 0 && temp_ch0 > g_temp_upper_limit_ch0) {
            alarm_active = true;
        }
        if (status_ch1 == 0 && temp_ch1 > g_temp_upper_limit_ch1) {
            alarm_active = true;
        }
        g_buzzer_trigger = alarm_active;

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
