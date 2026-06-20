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
        
        printf("[System] POWMAN chip_reset raw register: 0x%08X\n", reset_reason);
        
        // If brown-out (HAD_BOR) or glitch (HAD_GLITCH_DETECT) bits are set, classify as Power Cut/Glitch (3)
        if (reset_reason & (POWMAN_CHIP_RESET_HAD_BOR_BITS | POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS)) {
            g_boot_reason_code = 3; // 전원 끊김/브라운아웃 재부팅
        } else {
            g_boot_reason_code = 0; // 정상 파워 온
        }
        g_boot_cmd_id = 0;
    }
    printf("[System] Boot reason detected: %d (cmdId: %d)\n", g_boot_reason_code, g_boot_cmd_id);
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
            if (lcd_params.current_temperature > -990.0f)
            {
                printf("[PeriodicModemTask] 주기적 온도 HTTPS 전송 시도...\n");
                
                // 💡 [핵심] 망 등록 상태(CEREG) 및 신호 세기(CSQ)를 1회 즉각 확인하여 통신 상태가 좋은지 판별합니다.
                int cereg_val = modem.check_network_registration();
                int csq_val = modem.check_rssi_csq();
                lcd_params.current_csq = csq_val;
                lcd_params.is_searching_network = (csq_val == 99 || csq_val == 0);
                
                bool network_good = ((cereg_val == 1 || cereg_val == 5) && (csq_val != 99 && csq_val > 0));
                bool send_success = false;
                char response_buf[256];
                response_buf[0] = '\0';

                if (network_good)
                {
                    printf("[PeriodicModemTask] 통신 상태 양호 (CEREG: %d, CSQ: %d). HTTPS 전송 시작...\n", cereg_val, csq_val);
                    lcd_params.is_transmitting = true;
                    vTaskDelay(pdMS_TO_TICKS(1000)); // 물리 안정화 지연
                    
                    char payload[256];
                    snprintf(payload, sizeof(payload), 
                        "{\"p_imei\":\"%s\",\"p_value\":%.2f}", 
                        modem.get_imei(), lcd_params.current_temperature);
                    if (modem.modem_HttpOpen(SUPABASE_HOST, SUPABASE_PORT))
                    {
                        if (modem.modem_HttpPost("/rest/v1/rpc/t", payload, response_buf, sizeof(response_buf)))
                        {
                            send_success = true;
                        }
                        modem.modem_HttpClose();
                    }
                    else
                    {
                        modem.modem_HttpClose(); 
                    }
                    lcd_params.is_transmitting = false;
                }
                else
                {
                    printf("[PeriodicModemTask] 통신 상태 불량 (CEREG: %d, CSQ: %d). 전송 보류.\n", cereg_val, csq_val);
                }

                if (send_success)
                {
                    printf("[PeriodicModemTask] 온도 데이터 전송 성공. 다음 주기까지 20분 대기.\n");
                    
                    // Parse command and command ID from response
                    if (response_buf[0] != '\0')
                    {
                        int cmd = extract_json_int(response_buf, "cmd");
                        int cmdId = extract_json_int(response_buf, "cmdId");
                        if (cmd != -1 && cmdId != -1)
                        {
                            printf("[PeriodicModemTask] 기기 제어 명령 감지: cmd=%d, cmdId=%d\n", cmd, cmdId);
                            if (cmd == 10)
                            {
                                printf("[PeriodicModemTask] ⚠️ 'reboot' 명령 실행 (100ms 후 리셋)...\n");
                                vTaskDelay(pdMS_TO_TICKS(500)); // 로그 출력 보장을 위한 500ms 지연
                                safe_reboot(100);
                            }
                            else
                            {
                                printf("[PeriodicModemTask] 지원되지 않는 명령 코드: %d\n", cmd);
                            }
                        }
                    }
                    
                    // 로컬 플래시에 전송 성공 로그 기록 (HTTP 응답 성공코드: 200)
                    flash_log_write(lcd_params.current_temperature, lcd_params.current_vsys_voltage, 1, 0, 200, 0);
                    
                    last_temp_send_time_ms = current_time_ms; // 성공 시에만 20분 타이머 리셋
                }
                else
                {
                    printf("[PeriodicModemTask] 온도 데이터 전송 실패/보류. 1분 뒤 재시도 예약.\n");
                    
                    // 로컬 플래시에 전송 실패 로그 기록 (-1: 망 없음, -2: HTTP 포스트 실패)
                    int16_t modem_err = network_good ? -2 : -1;
                    flash_log_write(lcd_params.current_temperature, lcd_params.current_vsys_voltage, 0, 0, modem_err, 0);
                    
                    // 실패/보류 시 다음 시도를 1분 뒤로 지연시킵니다.
                    last_temp_send_time_ms = current_time_ms - (SENSOR_TEMP_CHECK_INTERVAL_MIN * 60 * 1000) + (60 * 1000);
                }
            }
            else
            {
                // 온도가 비정상인 경우는 전송하지 않고 다음 20분 주기로 넘어감
                printf("[PeriodicModemTask] 온도 비정상(센서 결함)으로 전송 생략. 에러 로그 기록.\n");
                int ntc_err = (lcd_params.current_temperature <= -990.0f) ? (int)(-990.0f - lcd_params.current_temperature) : 99;
                flash_log_write(lcd_params.current_temperature, lcd_params.current_vsys_voltage, 0, ntc_err, 0, 101); // 101: 센서 고장 코드
                
                last_temp_send_time_ms = current_time_ms;
            }
        }

        // 3. System Error Diagnostic Telemetry (Every 5 minutes)
        if (current_time_ms - last_error_check_time_ms >= (5 * 60 * 1000))
        {
            printf("[PeriodicModemTask] 시스템 이상 진단 체크 실행...\n");
            
            bool vsys_stable = lcd_params.is_vsys_stable;
            float temp_ntc = lcd_params.current_temperature;
            bool ntc_fault = (temp_ntc <= -990.0f);
            
            if (!vsys_stable || ntc_fault)
            {
                printf("[PeriodicModemTask] 🚨 시스템 이상 동작 검출! 긴급 HTTPS 알림 전송 시도...\n");
                
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
                    
                    char err_payload[256];
                    snprintf(err_payload, sizeof(err_payload), 
                        "{\"p_imei\":\"%s\"}", 
                        modem.get_imei());
                    
                    if (modem.modem_HttpOpen(SUPABASE_HOST, SUPABASE_PORT))
                    {
                        if (modem.modem_HttpPost("/rest/v1/rpc/a", err_payload))
                        {
                            err_send_success = true;
                        }
                        modem.modem_HttpClose();
                    }
                    else
                    {
                        modem.modem_HttpClose();
                    }
                    lcd_params.is_transmitting = false;
                }

                if (err_send_success)
                {
                    printf("[PeriodicModemTask] 이상 검출 경보 HTTPS 송출 완료. 다음 주기 5분 대기.\n");
                    last_error_check_time_ms = current_time_ms;
                }
                else
                {
                    printf("[PeriodicModemTask] 이상 검출 경보 HTTPS 송출 실패/보류. 1분 뒤 재시도 예약.\n");
                    last_error_check_time_ms = current_time_ms - (5 * 60 * 1000) + (60 * 1000);
                }
            }
            else
            {
                // 이상이 없을 때는 그냥 다음 5분 주기로 세팅
                last_error_check_time_ms = current_time_ms;
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

        float ntc_temp = 0.0f;
        int status = check_ntc_status(ntc_temp);
        
        if (status == 0) {
            lcd_params.current_temperature = ntc_temp;
            
            // Check if temperature exceeds the upper limit (-9.0C)
            if (ntc_temp > g_temp_upper_limit) {
                g_buzzer_trigger = true;
            } else {
                g_buzzer_trigger = false;
            }
            
            // Print temperature reading strictly every 60 seconds (1s loop * 60 cycles)
            if (serial_print_counter % 60 == 0) {
                printf("[Sensor] 현재 측정 온도: %.2f °C | VSYS: %.2fV (%s) | Trigger: %d | Limit: %.1f C\n", 
                       ntc_temp, vsys_vol, vsys_stable ? "정상" : "이상", g_buzzer_trigger, g_temp_upper_limit);
            }
        } else {
            lcd_params.current_temperature = -990.0f - (float)status;
            g_buzzer_trigger = false; // Mute alert if sensor fails
            
            if (serial_print_counter % 300 == 0) {
                printf("[Sensor] 센서 회로 결함 감지! 상태코드: %d | VSYS: %.2fV (%s)\n", 
                       status, vsys_vol, vsys_stable ? "정상" : "이상");
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
    printf("[BootTask] 비동기 부팅 자가 진단 태스크 구동 시작.\n");
    
    // ====================================================================================
    // Phase 1: Pico MCU Self-Diagnostics
    // ====================================================================================
    strcpy(lcd_params.status_text, "Check Pico");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    bool vsys_stable = false;
    float pico_voltage = read_vsys_voltage(vsys_stable);
    lcd_params.current_vsys_voltage = pico_voltage;
    lcd_params.is_vsys_stable = vsys_stable;
    
    // 로컬 플래시에 부팅 시점 이벤트 기록 (시스템오류 코드: 99로 부팅 성공 알림)
    flash_log_write(0.0f, pico_voltage, 0, 0, 0, 99);
    
    bool chip_temp_ok = false;
    float pico_temp = read_internal_temp(chip_temp_ok);
    
    uint32_t flash_checksum = 0;
    bool flash_ok = check_flash_integrity(flash_checksum);
    int flash_integrity_val = flash_ok ? 0 : 1;
    
    bool ram_ok = test_ram_integrity(); 
    int ram_test_val = ram_ok ? 0 : 1;
    
    printf("[Boot] Pico VSYS 전압: %.2fV (%s)\n", pico_voltage, vsys_stable ? "정상" : "이상");
    printf("[Boot] Pico 내부 온도: %.2fC (%s)\n", pico_temp, chip_temp_ok ? "정상" : "이상");
    printf("[Boot] Flash 체크섬 FNV-1a: 0x%08X\n", flash_checksum);
    printf("[Boot] RAM 무결성 테스트: %s\n", ram_ok ? "통과" : "실패");

    // ====================================================================================
    // Phase 2: Modem Power On & AT Initialization
    // ====================================================================================
    strcpy(lcd_params.status_text, "Power Modem");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    int at_status = 1;
    int cpin_status = 1;
    
    bool modem_ok = modem.modem_init(at_status, cpin_status);
    
    printf("[Boot] 모뎀 AT 얼라이브: %s\n", (at_status == 0) ? "정상(OK)" : "이상");
    printf("[Boot] SIM 카드 상태: %s\n", (cpin_status == 0) ? "정상(READY)" : "이상");

    int cereg_val = -1;
    int csq_val = 99;
    
    if (at_status == 0 && cpin_status == 0) {
        printf("[Boot] 기지국 신호 감지 및 LTE 망 등록 대기 시작...\n");
        
        // Loop and yield to check CEREG every 2 seconds for up to 90 seconds
        for (int retry = 1; retry <= 45; retry++) {
            cereg_val = modem.check_network_registration();
            csq_val = modem.check_rssi_csq();
            
            snprintf(lcd_params.status_text, sizeof(lcd_params.status_text), "LTE Conn %ds", retry * 2);
            
            if (cereg_val == 1 || cereg_val == 5) {
                printf("[Boot] LTE 망 연결 성공! 등록 코드: %d, CSQ: %d (대기시간: %d초)\n", cereg_val, csq_val, retry * 2);
                
                // NITZ 시간 동기화 및 부팅 에포크 보정
                uint32_t net_time = modem.retrieve_network_time();
                if (net_time > 0) {
                    uint32_t elapsed = to_ms_since_boot(get_absolute_time()) / 1000;
                    flash_log_set_boot_epoch(net_time - elapsed);
                    // 시간 동기화 성공 이벤트 로그 (코드 98) 기록 (sent=1, ntc=0, modem=0, sys=98)
                    flash_log_write(0.0f, pico_voltage, 1, 0, 0, 98);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    // Update operators details
    char oper_name[32];
    modem.check_operator_name(oper_name, sizeof(oper_name));
    
    printf("[Boot] CSQ 신호 세기: %d (0-31, 99)\n", csq_val);
    printf("[Boot] LTE망 등록 코드: %d\n", cereg_val);
    printf("[Boot] 통신사 명칭: %s\n", oper_name);
    printf("[Boot] IMEI 고유번호: %s\n", modem.get_imei());
    printf("[Boot] CIMI 고유번호: %s\n", modem.get_cimi());

    // ====================================================================================
    // Phase 3: External NTC Sensor Checks
    // ====================================================================================
    strcpy(lcd_params.status_text, "Check Sensor");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    float ntc_temp = 0.0f;
    int ntc_status = check_ntc_status(ntc_temp);
    printf("[Boot] NTC 온도센서 상태코드: %d\n", ntc_status);

    // ====================================================================================
    // Phase 4: Construct JSON & HTTPS Transmit
    // ====================================================================================
    strcpy(lcd_params.status_text, "Report HTTP");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    lcd_params.is_transmitting = true;
    
    char json_payload[512];
    snprintf(json_payload, sizeof(json_payload),
        "{\"p_imei\":\"%s\",\"p_cimi\":\"%s\",\"p_voltage\":%.2f,\"p_temp\":%.2f,"
        "\"p_flash\":%d,\"p_ram\":%d,\"p_at\":%d,\"p_cpin\":%d,\"p_csq\":%d,"
        "\"p_carrier\":\"%s\",\"p_temp_status\":%d,\"p_boot_reason\":%d,\"p_cmd_id\":%d}",
        modem.get_imei(), modem.get_cimi(), pico_voltage, pico_temp,
        flash_integrity_val, ram_test_val, at_status, cpin_status, csq_val,
        oper_name, ntc_status, g_boot_reason_code, g_boot_cmd_id);

    if (modem.modem_HttpOpen(SUPABASE_HOST, SUPABASE_PORT)) {
        modem.modem_HttpPost("/rest/v1/rpc/b", json_payload);
        modem.modem_HttpClose();
        printf("[Boot] 자가 진단 보고 데이터 HTTPS 송출 완료.\n");
    } else {
        printf("[Boot] 에러: 자가 진단 HTTPS 데이터 송출 실패.\n");
        modem.modem_HttpClose();
    }
    
    lcd_params.is_transmitting = false;

    // ====================================================================================
    // Phase 5: Launch Core 1 Background Thread & Clean Up
    // ====================================================================================
    // 1. Configure the normal operational state parameter for LCD
    lcd_params.is_booting = false; // Disable boot display state (starts showing temperature)
    strcpy(lcd_params.status_text, "Ready");
    lcd_params.current_csq = csq_val;
    lcd_params.is_searching_network = (csq_val == 99 || csq_val == 0);
    lcd_params.current_temperature = (ntc_status == 0) ? ntc_temp : (-990.0f - (float)ntc_status);
    
    printf("[BootTask] 자가 진단 및 보고 스레드 완료 성공. 자가 소멸합니다.\n");
    
    // 3. Delete this boot task cleanly to free RAM resources
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
    
    printf("\n==================================================\n");
    printf("❄️ Pico 2 W 부팅 자가 진단 및 데이터 수집 클라이언트\n");
    printf("==================================================\n");
    
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
        256,
        NULL,
        1,
        NULL
    );

    // 6. Register resource-locked Interactive AT Command Bypass thread (Priority 1)
    xTaskCreate(
        vDebugTask,
        "DebugTask",
        512,
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
        512,
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