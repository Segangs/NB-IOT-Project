#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/structs/powman.h"
#include "hardware/regs/powman.h"
#include "lib/LCD_I2C.hpp"
#include "src/config.h"
#include "src/tasks/tasks_sensor.hpp"
#include "src/tasks/tasks_lcd.hpp"
#include "src/tasks/tasks_modem.hpp"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "src/lib/flash_logger.hpp"

// FreeRTOS standard headers
#include "FreeRTOS.h" 
#include "task.h"

// Global boot reason codes (0: Normal, 1: Cmd Reboot, 2: Watchdog Timeout, 3: Power Cut/Brown-out)
int g_boot_reason_code = 0;
int g_boot_cmd_id = 0;

// Sensor cache structure and variables
struct SensorInfo {
    int sensor_id = -1;
    char sensor_type[32] = {0};
    char sensor_memo[32] = {0};
};

SensorInfo g_sensors[2];
int g_sensor_count = 0;

// Helper to parse sensor JSON list returned by Supabase
int parse_sensors_json(const char *json, SensorInfo *sensors, int max_sensors) {
    int count = 0;
    const char *ptr = json;
    
    while (count < max_sensors) {
        ptr = strstr(ptr, "\"sensorId\"");
        if (!ptr) break;
        
        ptr = strchr(ptr, ':');
        if (!ptr) break;
        ptr++; // Skip ':'
        
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        sensors[count].sensor_id = atoi(ptr);
        
        ptr = strstr(ptr, "\"sensorType\"");
        if (!ptr) break;
        
        ptr = strchr(ptr, ':');
        if (!ptr) break;
        ptr++;
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        
        if (*ptr == '"') {
            ptr++;
            const char *end = strchr(ptr, '"');
            if (end) {
                int len = end - ptr;
                if (len > 31) len = 31;
                strncpy(sensors[count].sensor_type, ptr, len);
                sensors[count].sensor_type[len] = '\0';
                ptr = end + 1;
            }
        } else if (strncmp(ptr, "null", 4) == 0) {
            strcpy(sensors[count].sensor_type, "null");
            ptr += 4;
        }
        
        ptr = strstr(ptr, "\"sensorMemo\"");
        if (!ptr) break;
        
        ptr = strchr(ptr, ':');
        if (!ptr) break;
        ptr++;
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        
        if (*ptr == '"') {
            ptr++;
            const char *end = strchr(ptr, '"');
            if (end) {
                int len = end - ptr;
                if (len > 31) len = 31;
                strncpy(sensors[count].sensor_memo, ptr, len);
                sensors[count].sensor_memo[len] = '\0';
                ptr = end + 1;
            }
        } else if (strncmp(ptr, "null", 4) == 0) {
            strcpy(sensors[count].sensor_memo, "null");
            ptr += 4;
        }
        
        count++;
    }
    return count;
}

// Buzzer Global Control Settings (Default threshold: -10.0C)
volatile float g_temp_upper_limit = DEFAULT_TEMP_UPPER_LIMIT;
volatile bool g_buzzer_trigger = false;
volatile bool g_buzzer_active = false;

void detect_boot_reason() {
    if (watchdog_caused_reboot()) {
        uint32_t magic = watchdog_hw->scratch[2];
        uint32_t cmd_id = watchdog_hw->scratch[3];
        
        // Clear scratch registers immediately so they don't persist on next random reboot
        watchdog_hw->scratch[2] = 0;
        watchdog_hw->scratch[3] = 0;
        
        if (magic == 0x12345678) {
            g_boot_reason_code = 1; // Cmd전송으로 인한 재부팅
            g_boot_cmd_id = cmd_id;
        } else {
            g_boot_reason_code = 2; // 오류로 인한 워치독 재부팅
            g_boot_cmd_id = 0;
        }
    } else {
        // Check POWMAN chip reset register
        uint32_t reset_reason = powman_hw->chip_reset;
        
        // Clear the POWMAN reset register (Write-1-to-Clear) to avoid stale values next boot
        powman_hw->chip_reset = reset_reason;
        
        // printf("[System] POWMAN chip_reset raw register: 0x%08X\n", reset_reason);
        
        // If brown-out (HAD_BOR) or glitch (HAD_GLITCH_DETECT) bits are set, classify as Power Cut/Glitch (3)
        if (reset_reason & (POWMAN_CHIP_RESET_HAD_BOR_BITS | POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS)) {
            g_boot_reason_code = 3; // 전원 끊김/브라운아웃 재부팅
        } else {
            g_boot_reason_code = 0; // 정상 파워 온
        }
        g_boot_cmd_id = 0;
    }
    // printf("[System] Boot reason detected: %d (cmdId: %d)\n", g_boot_reason_code, g_boot_cmd_id);
}

// Helper function to extract integer value from JSON response
int extract_json_int(const char *json, const char *key) {
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    const char *pos = strstr(json, search_key);
    if (pos == nullptr) {
        snprintf(search_key, sizeof(search_key), "\"%s\" :", key);
        pos = strstr(json, search_key);
    }
    if (pos != nullptr) {
        const char *val_start = pos + strlen(search_key);
        while (*val_start == ' ' || *val_start == '\t') {
            val_start++;
        }
        if (strncmp(val_start, "null", 4) == 0) {
            return -1;
        }
        return atoi(val_start);
    }
    return -1;
}

// Helper function to extract float value from JSON response
float extract_json_float(const char *json, const char *key) {
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    const char *pos = strstr(json, search_key);
    if (pos == nullptr) {
        snprintf(search_key, sizeof(search_key), "\"%s\" :", key);
        pos = strstr(json, search_key);
    }
    if (pos != nullptr) {
        const char *val_start = pos + strlen(search_key);
        while (*val_start == ' ' || *val_start == '\t') {
            val_start++;
        }
        if (strncmp(val_start, "null", 4) == 0) {
            return -999.0f;
        }
        return (float)atof(val_start);
    }
    return -999.0f;
}

// Safe reboot using Pico SDK Hardware Watchdog
void safe_reboot(int delay_ms, int cmd_id = 0) {
    printf("[System] 🚨 Reboot command received! Halting FreeRTOS and resetting system via hardware watchdog...\n");
    // FreeRTOS 스케줄러 및 인터럽트 차단하여 타 테스크와 꼬임 완벽 방지
    taskENTER_CRITICAL();
    
    // Save magic code and cmd_id to scratch registers before rebooting
    watchdog_hw->scratch[2] = 0x12345678;
    watchdog_hw->scratch[3] = cmd_id;
    
    // 하드웨어 워치독 재부팅 타이밍 로드 (0, 0은 일반 부팅 경로)
    watchdog_reboot(0, 0, delay_ms);
    
    // 리셋 발생 전까지 무한 루프로 대기
    while (true) {
        // Spin
    }
}

// ====================================================================================
// Global Shared State
// ====================================================================================
LcdTaskParams lcd_params;
nb_iot modem;

// Task handle for cleanup
TaskHandle_t xBootTaskHandle = NULL;

// ====================================================================================
// Core 0 Task: Dedicated Modem Communication Controller (Periodic Telemetry & RSSI)
// ====================================================================================
void vPeriodicModemTask(void *pvParameters)
{
    // 부팅 자가 진단이 완료될 때까지 안전하게 대기
    while (lcd_params.is_booting) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("[PeriodicModemTask] 백그라운드 모뎀 전용 태스크 가동.\n");
    
    uint32_t last_rssi_time_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_temp_send_time_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_error_check_time_ms = to_ms_since_boot(get_absolute_time());

    while (true)
    {
        uint32_t current_time_ms = to_ms_since_boot(get_absolute_time());

        // 1. RSSI Signal check (Every 5 minutes)
        if (current_time_ms - last_rssi_time_ms >= (MODEM_RSSI_CHECK_INTERVAL_MIN * 60 * 1000))
        {
            printf("[PeriodicModemTask] 주기적 CSQ RSSI 체크 실행...\n");
            lcd_params.is_modem_busy = true;
            vTaskDelay(pdMS_TO_TICKS(10)); // 락 전파를 위한 지연
            
            int csq = modem.check_rssi_csq();
            lcd_params.current_csq = csq;
            lcd_params.is_searching_network = (csq == 99 || csq == 0);
            
            lcd_params.is_modem_busy = false;
            last_rssi_time_ms = current_time_ms;
        }

        // 2. Transmit Temperature Telemetry JSON (Every 20 minutes as configured)
        if (current_time_ms - last_temp_send_time_ms >= (SENSOR_TEMP_CHECK_INTERVAL_MIN * 60 * 1000))
        {
            if (lcd_params.is_unauthenticated)
            {
                printf("[PeriodicModemTask] 🚨 인증 거부 상태입니다. MQTTS 전송을 시도하지 않습니다.\n");
                last_temp_send_time_ms = current_time_ms; // 주기 타이머만 리셋
            }
            else
            {
                printf("[PeriodicModemTask] 주기적 온도 MQTTS 전송 시도...\n");
                
                int cereg_val = modem.check_network_registration();
                int csq_val = modem.check_rssi_csq();
                lcd_params.current_csq = csq_val;
                lcd_params.is_searching_network = (csq_val == 99 || csq_val == 0);
                
                bool network_good = ((cereg_val == 1 || cereg_val == 5) && (csq_val != 99 && csq_val > 0));
                
                if (network_good)
                {
                    printf("[PeriodicModemTask] 통신 상태 양호 (CEREG: %d, CSQ: %d). MQTTS 연결 시작...\n", cereg_val, csq_val);
                    lcd_params.is_transmitting = true;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    
                    bool all_sends_success = true;
                    char config_topic[64];
                    snprintf(config_topic, sizeof(config_topic), "devices/%s/config", modem.get_imei());
                    char telemetry_topic[64];
                    snprintf(telemetry_topic, sizeof(telemetry_topic), "devices/%s/telemetry", modem.get_imei());

                    if (modem.modem_MqttOpen(MQTT_BROKER_HOST, MQTT_BROKER_PORT, modem.get_imei(), modem.get_imei(), modem.get_cimi()))
                    {
                        // 제어 명령 수신을 위한 구독 설정
                        modem.modem_MqttSubscribe(config_topic);

                        for (int i = 0; i < g_sensor_count; i++)
                        {
                            float send_val = (i == 0) ? lcd_params.current_temperature : lcd_params.current_temperature_ch1;
                            int sensor_id = g_sensors[i].sensor_id;
                            
                            if (send_val > -990.0f)
                            {
                                // 💡 [피드백 반영] 80바이트 제한 극복용 압축 JSON 페이로드 구조
                                char payload[64];
                                snprintf(payload, sizeof(payload), "{\"id\":%d,\"v\":%.2f}", sensor_id, send_val);
                                
                                if (modem.modem_MqttPublish(telemetry_topic, payload))
                                {
                                    printf("[PeriodicModemTask] 센서 ID %d 데이터 MQTTS 발행 성공.\n", sensor_id);
                                    flash_log_write(send_val, lcd_params.current_vsys_voltage, 1, 0, 200, 0);
                                }
                                else
                                {
                                    printf("[PeriodicModemTask] 센서 ID %d 데이터 MQTTS 발행 실패.\n", sensor_id);
                                    all_sends_success = false;
                                    flash_log_write(send_val, lcd_params.current_vsys_voltage, 0, 0, -2, 0);
                                }
                            }
                            else
                            {
                                printf("[PeriodicModemTask] 센서 ID %d 온도 비정상으로 발행 생략.\n", sensor_id);
                                int ntc_err = (send_val <= -990.0f) ? (int)(-990.0f - send_val) : 99;
                                flash_log_write(send_val, lcd_params.current_vsys_voltage, 0, ntc_err, 0, 101);
                            }
                        }

                        // 발행 직후 약 3초 동안 서버의 config 토픽 응답 수신 대기 (제어 및 임계치 동기화)
                        uint32_t wait_elapsed = 0;
                        while (wait_elapsed < 3000) {
                            modem_sleep(100);
                            wait_elapsed += 100;
                            modem.modem_ReadResponse(0);
                            
                            if (strstr(modem.get_rx_buffer(), "+KMQTT_DATA:") != nullptr) {
                                const char *payload_start = strchr(modem.get_rx_buffer(), '{');
                                if (payload_start != nullptr) {
                                    char cmd_buffer[256];
                                    strncpy(cmd_buffer, payload_start, sizeof(cmd_buffer) - 1);
                                    cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
                                    
                                    char *json_end = strrchr(cmd_buffer, '}');
                                    if (json_end != nullptr) *(json_end + 1) = '\0';

                                    int cmd = extract_json_int(cmd_buffer, "cmd");
                                    int cmdId = extract_json_int(cmd_buffer, "cmdId");
                                    
                                    if (cmd != -1 && cmdId != -1) {
                                        printf("[PeriodicModemTask] MQTTS 제어 명령 수신: cmd=%d, cmdId=%d\n", cmd, cmdId);
                                        if (cmd == 10) {
                                            printf("[PeriodicModemTask] ⚠️ 리부트 명령(cmd=10) 실행 중...\n");
                                            modem.modem_MqttClose();
                                            vTaskDelay(pdMS_TO_TICKS(500));
                                            safe_reboot(100);
                                        }
                                    }
                                    
                                    // 💡 [피드백 반영] 실시간 임계 상한 온도 갱신 동기화
                                    float limit_val = extract_json_float(cmd_buffer, "tempUpperLimitValue");
                                    if (limit_val > -990.0f) {
                                        g_temp_upper_limit = limit_val;
                                        printf("[PeriodicModemTask] 실시간 임계 온도값 갱신 적용: %.1f C\n", limit_val);
                                    }
                                    break;
                                }
                            }
                        }

                        // 세션 종료
                        modem.modem_MqttClose();
                    }
                    else
                    {
                        printf("[PeriodicModemTask] 에러: MQTTS 브로커 연결 실패\n");
                        all_sends_success = false;
                        if (modem.is_unauthenticated) {
                            printf("[PeriodicModemTask] 🚨 인증 실패 상태 감지! LCD에 상태 표기 적용.\n");
                            lcd_params.is_unauthenticated = true;
                        }
                    }

                    lcd_params.is_transmitting = false;
                    
                    if (all_sends_success)
                    {
                        printf("[PeriodicModemTask] 모든 센서 데이터 전송 완료. 다음 주기까지 20분 대기.\n");
                        last_temp_send_time_ms = current_time_ms;
                    }
                    else
                    {
                        printf("[PeriodicModemTask] 일부 전송 실패. 1분 뒤 재시도 예약.\n");
                        last_temp_send_time_ms = current_time_ms - (SENSOR_TEMP_CHECK_INTERVAL_MIN * 60 * 1000) + (60 * 1000);
                    }
                }
                else
                {
                    printf("[PeriodicModemTask] 통신망 상태 불량. 전송 보류. 1분 뒤 재시도 예약.\n");
                    flash_log_write(0.0f, lcd_params.current_vsys_voltage, 0, 0, -1, 0);
                    last_temp_send_time_ms = current_time_ms - (SENSOR_TEMP_CHECK_INTERVAL_MIN * 60 * 1000) + (60 * 1000);
                }
            }
        }

        // 3. System Error Diagnostic Telemetry (Every 5 minutes)
        if (current_time_ms - last_error_check_time_ms >= (5 * 60 * 1000))
        {
            if (lcd_params.is_unauthenticated)
            {
                last_error_check_time_ms = current_time_ms; // 인증 거부 시 경보 생략
            }
            else
            {
                printf("[PeriodicModemTask] 시스템 이상 진단 체크 실행...\n");
                
                bool vsys_stable = lcd_params.is_vsys_stable;
                float temp_ntc = lcd_params.current_temperature;
                bool ntc_fault = (temp_ntc <= -990.0f);
                
                if (!vsys_stable || ntc_fault)
                {
                    printf("[PeriodicModemTask] 🚨 시스템 이상 동작 검출! 긴급 MQTTS 알림 전송 시도...\n");
                    
                    int cereg_val = modem.check_network_registration();
                    int csq_val = modem.check_rssi_csq();
                    lcd_params.current_csq = csq_val;
                    lcd_params.is_searching_network = (csq_val == 99 || csq_val == 0);
                    
                    bool network_good = ((cereg_val == 1 || cereg_val == 5) && (csq_val != 99 && csq_val > 0));
                    bool err_send_success = false;

                    if (network_good)
                    {
                        lcd_params.is_transmitting = true;
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        
                        char alert_topic[64];
                        snprintf(alert_topic, sizeof(alert_topic), "devices/%s/alert", modem.get_imei());
                        
                        if (modem.modem_MqttOpen(MQTT_BROKER_HOST, MQTT_BROKER_PORT, modem.get_imei(), modem.get_imei(), modem.get_cimi()))
                        {
                            if (modem.modem_MqttPublish(alert_topic, "{\"alert\":1}"))
                            {
                                err_send_success = true;
                            }
                            modem.modem_MqttClose();
                        }
                        lcd_params.is_transmitting = false;
                    }

                    if (err_send_success)
                    {
                        printf("[PeriodicModemTask] 이상 검출 경보 MQTTS 송출 완료. 다음 주기 5분 대기.\n");
                        last_error_check_time_ms = current_time_ms;
                    }
                    else
                    {
                        printf("[PeriodicModemTask] 이상 검출 경보 MQTTS 송출 실패. 1분 뒤 재시도 예약.\n");
                        last_error_check_time_ms = current_time_ms - (5 * 60 * 1000) + (60 * 1000);
                    }
                }
                else
                {
                    last_error_check_time_ms = current_time_ms;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Yield to other tasks
    }
}

// ====================================================================================
// Buzzer Control Helpers (Passive Buzzer on GP16 using PWM)
// ====================================================================================
void buzzer_stop(uint pin)
{
    uint slice_num = pwm_gpio_to_slice_num(pin);
    pwm_set_enabled(slice_num, false);
    
    // Reconfigure pin as standard GPIO output and force low (0V) to suppress static noise
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
}

void buzzer_set_frequency(uint pin, uint32_t frequency)
{
    if (frequency == 0)
    {
        buzzer_stop(pin);
        return;
    }

    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin);
    uint chan = pwm_gpio_to_channel(pin);

    // Get system clock frequency safely
    uint32_t sys_clk = clock_get_hz(clk_sys);
    if (sys_clk == 0) {
        sys_clk = 150000000; // Fallback to RP2350 standard 150MHz
    }

    float div = 125.0f;
    uint32_t wrap = sys_clk / (div * frequency);
    if (wrap > 65535) wrap = 65535;

    pwm_set_clkdiv(slice_num, div);
    pwm_set_wrap(slice_num, wrap);
    pwm_set_chan_level(slice_num, chan, wrap / 10); // 10% duty cycle to significantly reduce current draw
    pwm_set_enabled(slice_num, true);
}

struct Note {
    uint32_t freq;
    uint32_t duration;
};

// ====================================================================================
// Core 0 Task: Buzzer Controller Task (Core 0)
// ====================================================================================
void vBuzzerTask(void *pvParameters)
{
    // Initialize GP16 as simple GPIO to start in off state
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);

    // Ding-Dong (Mi-Do) Melody Configuration
    // Ding (E5): 659 Hz, Dong (C5): 523 Hz (Lower than Mi)
    const Note ding_dong[] = {
        {659, 200},  // Ding (E5)
        {0,   50},   // Brief gap between Ding and Dong
        {523, 400},  // Dong (C5) - Lower pitch
        {0,   600}   // Silent delay before next Ding-Dong
    };
    const int num_notes = sizeof(ding_dong) / sizeof(ding_dong[0]);

    printf("[BuzzerTask] Passive Buzzer 'Ding-Dong' 5-repetition test task started.\n");

    while (true)
    {
        // Wait until temperature exceeds the upper limit (g_buzzer_trigger == true)
        if (!g_buzzer_trigger)
        {
            vTaskDelay(pdMS_TO_TICKS(100)); // Poll every 100ms
            continue;
        }

        printf("[BuzzerTask] Temp upper limit exceeded! Playing 'Ding-Dong' (Mi-Do) 5 times...\n");
        g_buzzer_active = true;

        for (int repeat = 0; repeat < 5; repeat++)
        {
            for (int i = 0; i < num_notes; i++)
            {
                // Play note using hardware PWM
                buzzer_set_frequency(BUZZER_PIN, ding_dong[i].freq);
                vTaskDelay(pdMS_TO_TICKS(ding_dong[i].duration));

                // Very brief gap to distinguish rapid same notes
                buzzer_set_frequency(BUZZER_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }

        g_buzzer_active = false;
        printf("[BuzzerTask] Done playing 5 times. Entering 1-minute silent interval...\n");
        buzzer_stop(BUZZER_PIN);

        // Sleep for 1 minute (60 seconds = 60,000 ms) before checking trigger status again
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

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
        } else {
            lcd_params.current_temperature = -990.0f - (float)status_ch0;
        }
        lcd_params.status_ch0 = status_ch0;

        // Ch1 파라미터 매핑
        if (status_ch1 == 0) {
            lcd_params.current_temperature_ch1 = ntc_temp_ch1;
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

// ====================================================================================
// Core 0 Task: Resource-Locked Interactive AT Command Bypass (Core 0)
// ====================================================================================
void vDebugTask(void *pvParameters)
{
    printf("[DebugTask] 디버깅용 실시간 AT 바이패스 스레드 가동 완료.\n");
    printf("[DebugTask] 로그 관리 명령어: 'dump_csv' (CSV 출력), 'clear_csv' (로그 청소)\n");
    
    char cmd_buf[64];
    int cmd_idx = 0;
    memset(cmd_buf, 0, sizeof(cmd_buf));
    
    while (true)
    {
        // 🚨 시리얼 가로채기 차단: 모뎀이 부팅 중, 통신 중, 또는 소켓 연결 중일 때는 
        // 전송 및 수신 버퍼 Race Condition 방지를 위해 대화식 디버그를 일시 중지(Yield)합니다.
        if (lcd_params.is_booting || lcd_params.is_transmitting || modem.is_connected() || lcd_params.is_modem_busy) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Yield for 100ms
            continue;
        }
        
        // 시리얼 입력 비동기 1문자 획득
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            char c = (char)ch;
            
            // 화면 에코백 (사용자 편의성 제공)
            putchar(c);
            
            if (c == '\r' || c == '\n') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';
                    
                    // 디버그 쉘 명령어 분기
                    if (strcmp(cmd_buf, "dump_csv") == 0) {
                        flash_log_dump_csv();
                    } else if (strcmp(cmd_buf, "clear_csv") == 0) {
                        flash_log_clear();
                    } else {
                        // 일반 AT 명령어일 경우 모뎀 UART에 전달
                        strcat(cmd_buf, "\r\n");
                        modem.modem_PacedWrite(cmd_buf);
                    }
                    cmd_idx = 0;
                    memset(cmd_buf, 0, sizeof(cmd_buf));
                }
            } else if (c == '\b' || ch == 127) { // 백스페이스
                if (cmd_idx > 0) {
                    cmd_idx--;
                    cmd_buf[cmd_idx] = '\0';
                }
            } else {
                if (cmd_idx < (int)sizeof(cmd_buf) - 2) {
                    cmd_buf[cmd_idx++] = c;
                }
            }
        }
        
        // 모뎀의 실시간 출력 결과 파이프
        modem.modem_ReadResponse(0);
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Hyper-responsive 10ms polling interval
    }
}

// ====================================================================================
// Core 0 Task: Boot Diagnostics Orchestration Task (Non-Blocking Thread)
// ====================================================================================
void vBootTask(void *pvParameters)
{
    printf("[BootTask] 🚀 단순화된 비동기 부팅 태스크 구동 시작 (Bypass 모드)\n");
    
    // ====================================================================================
    // LCD 즉시 부팅 해제 및 정상 화면 전환 (멈춤 현상 원천 예방)
    // ====================================================================================
    lcd_params.is_booting = false; // 부팅 화면 즉시 종료
    strcpy(lcd_params.status_text, "Ready");
    lcd_params.current_vsys_voltage = 5.0f;
    lcd_params.is_vsys_stable = true;
    
    // 센서 조회 실패를 대비한 기본 센서 세팅 폴백
    g_sensors[0].sensor_id = 1;
    strcpy(g_sensors[0].sensor_type, "Temp");
    strcpy(g_sensors[0].sensor_memo, "Default");
    g_sensor_count = 1;

    // ====================================================================================
    // 백그라운드 모뎀 초기화 및 MQTTS 부팅 보고 송출
    // ====================================================================================
    int at_status = 1;
    int cpin_status = 1;
    
    // 모뎀의 하드웨어 전원 켜기 및 초기화 시도
    bool modem_ok = modem.modem_init(at_status, cpin_status);
    
    if (modem_ok && at_status == 0 && cpin_status == 0) {
        printf("[BootTask] 모뎀 초기화 성공. 기지국 신호 확인 중...\n");
        int csq_val = modem.check_rssi_csq();
        lcd_params.current_csq = csq_val;
        lcd_params.is_searching_network = (csq_val == 99 || csq_val == 0);
        
        int cereg_val = modem.check_network_registration();
        
        // 최대 10초 동안 기지국 신호 획득 대기 (비동기로 돌기 때문에 LCD는 멈추지 않음)
        for (int retry = 1; retry <= 5; retry++) {
            if (cereg_val == 1 || cereg_val == 5) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            cereg_val = modem.check_network_registration();
        }
        
        char oper_name[32] = {0};
        modem.check_operator_name(oper_name, sizeof(oper_name));
        
        // MQTTS 부팅 보고 송출 패킷 구성
        char json_payload[128];
        snprintf(json_payload, sizeof(json_payload),
            "{\"v\":5.00,\"t\":25.00,\"f\":0,\"r\":0,\"a\":0,\"c\":0,\"q\":%d,\"o\":\"%.8s\","
            "\"ts0\":0,\"ts1\":0,\"b\":%d,\"i\":%d}",
            csq_val, oper_name, g_boot_reason_code, g_boot_cmd_id);
            
        char boot_topic[64];
        snprintf(boot_topic, sizeof(boot_topic), "devices/%s/boot", modem.get_imei());
        
        printf("[BootTask] 부팅 자가 진단 로그 MQTTS 발행 시도...\n");
        
        // MQTTS 연결 및 발송 시도
        if (modem.modem_MqttOpen(MQTT_BROKER_HOST, MQTT_BROKER_PORT, modem.get_imei(), modem.get_imei(), modem.get_cimi())) {
            modem.modem_MqttPublish(boot_topic, json_payload);
            modem.modem_MqttClose();
            printf("[BootTask] 부팅 자가 진단 로그 MQTTS 발행 성공.\n");
        } else {
            printf("[BootTask] 에러: 부팅 로그 MQTTS 발행 실패 (브로커 연결 거부)\n");
            if (modem.is_unauthenticated) {
                printf("[BootTask] 🚨 인증 거부 상태 감지! LCD에 Unauth 팝업.\n");
                lcd_params.is_unauthenticated = true;
            }
        }
    } else {
        printf("[BootTask] 에러: 모뎀 초기화 실패. 망 등록 및 보고 생략.\n");
    }
    
    printf("[BootTask] 비동기 부팅 태스크 완료. 자가 소멸합니다.\n");
    vTaskDelete(NULL);
}

// ====================================================================================
// Main Execution Block (Core 0 Startup)
// ====================================================================================
int main()
{
    // Detect boot reason immediately before registers are modified
    detect_boot_reason();
    
    // Initialize Flash log storage
    flash_log_init();
    
    // 1. Initialize serial monitoring
    stdio_init_all();
    
    // printf("\n==================================================\n");
    // printf("❄️ Pico 2 W 부팅 자가 진단 및 데이터 수집 클라이언트\n");
    // printf("==================================================\n");
    
    // 2. Configure shared state initial parameters immediately
    // Set 'is_searching_network' and 'is_booting' to true to start boot screen instantly!
    lcd_params.is_booting = true; // ACTIVATE BOOT SCREEN MODE
    strcpy(lcd_params.status_text, "Booting...");
    lcd_params.current_temperature = 25.0f; 
    lcd_params.current_csq = 99;
    lcd_params.is_searching_network = true; // RUN SEARCH SEQUENCE INSTANTLY
    lcd_params.is_transmitting = false;
    lcd_params.is_modem_busy = false;
    
    // 3. Initialize LCD hardware (takes ~100ms)
    static LCD_I2C lcd(LCD_ADDR, 16, 2, I2C_PORT, SDA_PIN, SCL_PIN);
    lcd_params.lcd = &lcd;
    
    // 4. Initialize MCU ADCs
    sensor_init();
    
    // 5. Register FreeRTOS Tasks
    // LcdTask has priority 2 (High) to drive fluent, non-stuttering screen animations
    xTaskCreate(
        vLcdTask,
        "LcdTask",
        512,
        &lcd_params,
        2,
        NULL
    );

    // Boot task executes checks in background without freezing LcdTask
    xTaskCreate(
        vBootTask,
        "BootTask",
        2048,
        NULL,
        1,
        &xBootTaskHandle
    );

    xTaskCreate(
        vSensorTask,
        "SensorTask",
        1024,
        NULL,
        1,
        NULL
    );

    // 6. Register resource-locked Interactive AT Command Bypass thread (Priority 1)
    xTaskCreate(
        vDebugTask,
        "DebugTask",
        1024,
        NULL,
        1,
        NULL
    );

    // 7. Register periodic modem communication controller task (Priority 1)
    xTaskCreate(
        vPeriodicModemTask,
        "PeriodicModemTask",
        2048,
        NULL,
        1,
        NULL
    );

    // 8. Register buzzer melody control task (Priority 1)
    xTaskCreate(
        vBuzzerTask,
        "BuzzerTask",
        1024,
        NULL,
        1,
        NULL
    );

    // 8. Ignite FreeRTOS Scheduler instantly!
    vTaskStartScheduler();

    // Loop fallback
    while (true) {}
    return 0;
}

// ====================================================================================
// FreeRTOS Malloc and Stack Hooks
// ====================================================================================
extern "C" {

void vApplicationMallocFailedHook(void)
{
    printf("[Fatal] FreeRTOS Malloc Failed!\n");
    while (true) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("[Fatal] FreeRTOS Stack Overflow in task: %s\n", pcTaskName);
    while (true) {}
}

} // extern "C"