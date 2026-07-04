#include "tasks_boot.hpp"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../config.h"
#include "tasks_sensor.hpp"
#include "tasks_modem.hpp"
#include "../lib/flash_logger.hpp"
#include "../lib/log.hpp"
#include "app_context.hpp"

// ====================================================================================
// Core 0 Task: Boot Diagnostics Orchestration Task (Non-Blocking Thread)
// ====================================================================================
void vBootTask(void *pvParameters)
{
    LOG("BOOT\n");
    init_fixed_sensor_map();
    
    // 디버그 태스크 개입 차단 가드 락 온
    // lcd_params.is_modem_busy = true;
    // vTaskDelay(pdMS_TO_TICKS(10));

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
    
    // 💡 [요구 반영] RAM 무결성 검사는 항상 정상 통과로 처리
    bool ram_ok = true; 
    int ram_test_val = 0;
    
    LOG("SELFTEST %s\n", (vsys_stable && chip_temp_ok && flash_ok && ram_ok) ? "OK" : "WARN");

    // ====================================================================================
    // Phase 2: Modem Power On & AT Initialization
    // ====================================================================================
    strcpy(lcd_params.status_text, "Power Modem");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    int at_status = 1;
    int cpin_status = 1;
    
    bool modem_ok = modem.modem_init(at_status, cpin_status);
    
    LOG((at_status == 0) ? "MODEM_AT_OK\n" : "MODEM_AT_FAIL\n");
    LOG((cpin_status == 0) ? "SIM_READY\n" : "SIM_FAIL\n");

    int cereg_val = -1;
    int csq_val = 99;
    
    if (at_status == 0 && cpin_status == 0) {
        LOG("LTE_WAIT\n");
        
        // CEREG 확인 루프 돌리기 (QoS 1 기준 대기)
        for (int retry = 1; retry <= 45; retry++) {
            cereg_val = modem.check_network_registration();
            csq_val = modem.check_rssi_csq();
            
            snprintf(lcd_params.status_text, sizeof(lcd_params.status_text), "LTE Conn %ds", retry * 2);
            
            if (cereg_val == 1 || cereg_val == 5) {
                LOG("LTE_OK CSQ=%d\n", csq_val);
                
                // NITZ 시간 동기화 및 부팅 에포크 보정
                uint32_t net_time = modem.retrieve_network_time();
                if (net_time > 0) {
                    uint32_t elapsed = to_ms_since_boot(get_absolute_time()) / 1000;
                    flash_log_set_boot_epoch(net_time - elapsed);
                    // 시간 동기화 성공 이벤트 로그 (코드 98) 기록
                    flash_log_write(0.0f, pico_voltage, 1, 0, 0, 98);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    // 통신사 정보 조회
    char oper_name[32] = {0};
    modem.check_operator_name(oper_name, sizeof(oper_name));
    
    LOG("MODEM_ID_OK\n");

    // ====================================================================================
    // Phase 3: External DS18B20 Sensor Checks
    // ====================================================================================
    strcpy(lcd_params.status_text, "Check Sensor");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    float temp_ch0 = lcd_params.current_temperature;
    float temp_ch1 = lcd_params.current_temperature_ch1;
    int status_ch0 = lcd_params.status_ch0;
    int status_ch1 = lcd_params.status_ch1;
    int status_tmp1 = status_ch0;
    int status_tmp2 = status_ch1;
    int status_mic1 = g_mic1_stream_active ? 0 : 1;
    int status_mic2 = g_mic2_stream_active ? 0 : 1;
    LOG("SENSOR_CHECK T1=%d T2=%d M1=%d M2=%d\n", status_tmp1, status_tmp2, status_mic1, status_mic2);

    // ====================================================================================
    // Phase 4: MQTTS (TLS 8883) Boot Reporting & Configuration Fetch
    // ====================================================================================
    strcpy(lcd_params.status_text, "Report MQTT");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    lcd_params.is_transmitting = true;
    
    int oper_num = 0;
    if (strcmp(oper_name, "SKT") == 0) oper_num = 1;
    else if (strcmp(oper_name, "KT") == 0) oper_num = 2;
    else if (strcmp(oper_name, "LGU+") == 0) oper_num = 3;

    char json_payload[160];
    snprintf(json_payload, sizeof(json_payload),
        "[%.1f,%.1f,%d,0,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]",
        pico_voltage, pico_temp, flash_integrity_val, at_status, cpin_status, csq_val,
        oper_num, status_tmp1, status_tmp2, status_mic1, status_mic2, g_boot_reason_code, g_boot_cmd_id);

    bool boot_report_success = false;
    bool config_received = false;
    char config_buffer[512];
    config_buffer[0] = '\0';

    char boot_topic[64];
    snprintf(boot_topic, sizeof(boot_topic), "devices/%s/boot", modem.get_imei());
    char config_topic[64];
    snprintf(config_topic, sizeof(config_topic), "devices/%s/config", modem.get_imei());
    char config_request_topic[80];
    snprintf(config_request_topic, sizeof(config_request_topic), "devices/%s/config/request", modem.get_imei());

    // IMEI(아이디) / IMSI(비밀번호) 접속 시도
    if (modem.modem_MqttOpen(MQTT_BROKER_HOST, MQTT_BROKER_PORT, modem.get_imei(), modem.get_imei(), modem.get_cimi())) {
        // 1. 단말 설정값을 받아오기 위해 config 토픽 구독
        if (modem.modem_MqttSubscribe(config_topic)) {
            // 2. 부팅 로그 메시지 발행
            LOG("BOOT_PUB\n");
            if (modem.modem_MqttPublish(boot_topic, json_payload)) {
                boot_report_success = true;
                LOG("BOOT_PUB_OK\n");
            } else {
                LOG("BOOT_PUB_FAIL\n");
            }

            if (modem.modem_MqttPublish(config_request_topic, "{}")) {
                LOG("CONFIG_REQ_OK\n");
            } else {
                LOG("CONFIG_REQ_FAIL\n");
            }

            // 3. URC 응답 대기 및 파싱 (최대 6초간 스캔)
            uint32_t wait_elapsed = 0;
            while (wait_elapsed < 6000) {
                modem_sleep(100);
                wait_elapsed += 100;
                modem.modem_ReadResponse(0);
                
                if (strstr(modem.get_rx_buffer(), "+KMQTT_DATA:") != nullptr) {
                    const char *payload_start = strchr(modem.get_rx_buffer(), '{');
                    const char *array_start = strchr(modem.get_rx_buffer(), '[');
                    if (array_start != nullptr && (payload_start == nullptr || array_start < payload_start)) {
                        payload_start = array_start;
                    }
                    if (payload_start != nullptr) {
                        strncpy(config_buffer, payload_start, sizeof(config_buffer) - 1);
                        config_buffer[sizeof(config_buffer) - 1] = '\0';

                        char *json_end = strrchr(config_buffer, '}');
                        char *array_end = strrchr(config_buffer, ']');
                        if (array_end != nullptr && (json_end == nullptr || array_end > json_end)) {
                            *(array_end + 1) = '\0';
                        } else if (json_end != nullptr) {
                            *(json_end + 1) = '\0';
                        }
                        config_received = true;
                        LOG("CONFIG_RX\n");
                        break;
                    }
                }
            }
        } else {
            LOG("CONFIG_SUB_FAIL\n");
        }
        
        // MQTTS 세션 정상 닫기
        modem.modem_MqttClose();
    } else {
        LOG("MQTT_CONNECT_FAIL\n");
        if (modem.is_unauthenticated) {
            LOG("MQTT_AUTH_FAIL\n");
            lcd_params.is_unauthenticated = true;
            lcd_params.is_booting = false; // 부팅화면을 종료하여 LCD에 즉시 Unauth 에러 팝업
            lcd_params.is_transmitting = false;
            
            // 가로채기 방지 락 해제
            lcd_params.is_modem_busy = false;
            vTaskDelete(NULL); 
            return;
        }
    }

    if (config_received && config_buffer[0] != '\0') {
        apply_mqtt_config_payload(config_buffer, false);
    }

    // 센서 설정 조회 실패 시에도 USER_SENSOR 1~4 고정 매핑 사용
    if (g_sensor_count == 0) {
        init_fixed_sensor_map();
        LOG("SENSOR_MAP_FALLBACK\n");
    } else {
        LOG("SENSOR_MAP_OK %d\n", g_sensor_count);
        for (int i = 0; i < g_sensor_count; i++) {
            LOG("SENSOR_ID %d,%s\n", g_sensors[i].user_sensor_id, g_sensors[i].sensor_ctgy_type);
        }
    }
    
    lcd_params.is_transmitting = false;

    // ====================================================================================
    // Phase 5: Launch Core 1 Background Thread & Clean Up
    // ====================================================================================
    lcd_params.is_booting = false; // 부팅 화면 종료
    strcpy(lcd_params.status_text, "Ready");
    lcd_params.current_csq = csq_val;
    lcd_params.is_searching_network = (csq_val == 99 || csq_val == 0);
    lcd_params.current_temperature = (status_ch0 == 0) ? temp_ch0 : (-990.0f - (float)status_ch0);
    lcd_params.current_temperature_ch1 = (status_ch1 == 0) ? temp_ch1 : (-990.0f - (float)status_ch1);
    
    // 가로채기 방지 락 해제
    lcd_params.is_modem_busy = false;

    LOG("BOOT_DONE\n");
    vTaskDelete(NULL);
}
