#include "tasks_sensor_reader.hpp"

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "tasks_sensor.hpp"
#include "app_context.hpp"

// ====================================================================================
// Core 0 Task: Sensor Reader Task (Core 0)
// ====================================================================================
void vSensorTask(void *pvParameters)
{
    uint32_t serial_print_counter = 0;
    
    while (true)
    {
        // Core 1의 ADC 직접 간섭을 예방하기 위해 Core 0에서 전압도 수집하여 전역 공유합니다.
        bool vsys_stable = false;
        float vsys_vol = read_vsys_voltage(vsys_stable);
        lcd_params.current_vsys_voltage = vsys_vol;
        lcd_params.is_vsys_stable = vsys_stable;

        float ntc_temp_ch0 = 0.0f;
        float ntc_temp_ch1 = 0.0f;
        int status_ch0 = 0;
        int status_ch1 = 0;
        
        check_ntc_status_dual(ntc_temp_ch0, status_ch0, ntc_temp_ch1, status_ch1);
        
        // Ch0 파라미터 매핑
        if (status_ch0 == 0) {
            lcd_params.current_temperature = ntc_temp_ch0;
            g_temp_ch0_sample_seq++;
        } else {
            lcd_params.current_temperature = -990.0f - (float)status_ch0;
        }
        lcd_params.status_ch0 = status_ch0;

        // Ch1 파라미터 매핑
        if (status_ch1 == 0) {
            lcd_params.current_temperature_ch1 = ntc_temp_ch1;
            g_temp_ch1_sample_seq++;
        } else {
            lcd_params.current_temperature_ch1 = -990.0f - (float)status_ch1;
        }
        lcd_params.status_ch1 = status_ch1;

        // 경보 여부 판단 (활성화된 센서 중 하나라도 임계치를 넘으면 발생)
        bool alarm_active = false;
        if (status_ch0 == 0 && ntc_temp_ch0 > g_temp_upper_limit) {
            alarm_active = true;
        }
        if (g_sensor_count >= 2 && status_ch1 == 0 && ntc_temp_ch1 > g_temp_upper_limit) {
            alarm_active = true;
        }
        g_buzzer_trigger = alarm_active;

        // 시리얼 로그 출력 (1분 주기)
        if (serial_print_counter % 60 == 0) {
            if (g_sensor_count >= 2) {
                printf("[Sensor] Ch0: %.2f °C (St:%d) | Ch1: %.2f °C (St:%d) | VSYS: %.2fV (%s) | Alarm: %d | Limit: %.1f C\n", 
                       ntc_temp_ch0, status_ch0, ntc_temp_ch1, status_ch1, vsys_vol, vsys_stable ? "정상" : "이상", g_buzzer_trigger, g_temp_upper_limit);
            } else {
                printf("[Sensor] Ch0: %.2f °C (St:%d) | VSYS: %.2fV (%s) | Alarm: %d | Limit: %.1f C\n", 
                       ntc_temp_ch0, status_ch0, vsys_vol, vsys_stable ? "정상" : "이상", g_buzzer_trigger, g_temp_upper_limit);
            }
        }
        
        serial_print_counter++;
        vTaskDelay(pdMS_TO_TICKS(1000)); // Sample sensor every 1 second
    }
}


