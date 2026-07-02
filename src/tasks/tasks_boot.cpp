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
#include "app_context.hpp"

// ====================================================================================
// Core 0 Task: Boot Diagnostics Orchestration Task (Non-Blocking Thread)
// ====================================================================================
void vBootTask(void *pvParameters)
{
    printf("[BootTask] 🚀 비동기 부팅 자가 진단 태스크 구동 시작 (자가진단 복원)\n");
    
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
    
    printf("[Boot] Pico VSYS 전압: %.2fV (%s)\n", pico_voltage, vsys_stable ? "정상" : "이상");
    printf("[Boot] Pico 내부 온도: %.2fC (%s)\n", pico_temp, chip_temp_ok ? "정상" : "이상");
    printf("[Boot] Flash 체크섬 FNV-1a: 0x%08X\n", flash_checksum);
    printf("[Boot] RAM 무결성 테스트: %s (강제 정상)\n", ram_ok ? "통과" : "실패");

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
        
        // CEREG 확인 루프 돌리기 (QoS 1 기준 대기)
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
    
    float ntc_temp_ch0 = 0.0f;
    float ntc_temp_ch1 = 0.0f;
    int status_ch0 = 0;
    int status_ch1 = 0;
    check_ntc_status_dual(ntc_temp_ch0, status_ch0, ntc_temp_ch1, status_ch1);
    printf("[Boot] NTC 온도센서 Ch0 상태코드: %d, Ch1 상태코드: %d\n", status_ch0, status_ch1);

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

    char json_payload[128];
    snprintf(json_payload, sizeof(json_payload),
        "[%.1f,%.1f,%d,0,%d,%d,%d,%d,%d,%d,%d,%d]",
        pico_voltage, pico_temp, flash_integrity_val, at_status, cpin_status, csq_val,
        oper_num, status_ch0, status_ch1, g_boot_reason_code, g_boot_cmd_id);

    bool boot_report_success = false;
    bool config_received = false;
    char config_buffer[512];
    config_buffer[0] = '\0';

    char boot_topic[64];
    snprintf(boot_topic, sizeof(boot_topic), "devices/%s/boot", modem.get_imei());
    char config_topic[64];
    snprintf(config_topic, sizeof(config_topic), "devices/%s/config", modem.get_imei());

    printf("[Boot] EMQX MQTTS 브로커 연결 시도 (%s:%s)...\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    
    // IMEI(아이디) / IMSI(비밀번호) 접속 시도
    if (modem.modem_MqttOpen(MQTT_BROKER_HOST, MQTT_BROKER_PORT, modem.get_imei(), modem.get_imei(), modem.get_cimi())) {
        // 1. 단말 설정값을 받아오기 위해 config 토픽 구독
        printf("[Boot] 1. 단말 설정 정보 토픽 구독 등록...\n");
        if (modem.modem_MqttSubscribe(config_topic)) {
            // 2. 부팅 로그 메시지 발행
            printf("[Boot] 2. 부팅 자가 진단 로그 MQTTS 발행 중...\n");
            if (modem.modem_MqttPublish(boot_topic, json_payload)) {
                boot_report_success = true;
                printf("[Boot] 부팅 자가 진단 로그 MQTTS 발행 성공. 설정 대기 중...\n");
                
                // 3. URC 응답 대기 및 파싱 (최대 6초간 스캔)
                uint32_t wait_elapsed = 0;
                while (wait_elapsed < 6000) {
                    modem_sleep(100);
                    wait_elapsed += 100;
                    modem.modem_ReadResponse(0);
                    
                    if (strstr(modem.get_rx_buffer(), "+KMQTT_DATA:") != nullptr) {
                        const char *payload_start = strchr(modem.get_rx_buffer(), '{');
                        if (payload_start != nullptr) {
                            strncpy(config_buffer, payload_start, sizeof(config_buffer) - 1);
                            config_buffer[sizeof(config_buffer) - 1] = '\0';
                            
                            // JSON Payload 뒤의 닫는 따옴표나 기타 문자열 정리
                            char *json_end = strrchr(config_buffer, '}');
                            if (json_end != nullptr) {
                                *(json_end + 1) = '\0';
                            }
                            config_received = true;
                            printf("[Boot] 설정 데이터 URC 수신 성공: %s\n", config_buffer);
                            break;
                        }
                    }
                }
            } else {
                printf("[Boot] 에러: 부팅 로그 MQTTS 발행 실패.\n");
            }
        } else {
            printf("[Boot] 에러: 설정 토픽 구독 실패.\n");
        }
        
        // MQTTS 세션 정상 닫기
        modem.modem_MqttClose();
    } else {
        printf("[Boot] 에러: EMQX MQTTS 브로커 연결 실패.\n");
        if (modem.is_unauthenticated) {
            printf("[Boot] 🚨 인증 거부(Incorrect IMEI/IMSI) 상태 감지! LCD에 Unauth 에러 적용.\n");
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
        g_sensor_count = parse_sensors_json(config_buffer, g_sensors, 2);
        
        // usersettings 테이블의 tempUpperLimitValue를 파싱하여 적용
        float limit_val = extract_json_float(config_buffer, "tempUpperLimitValue");
        if (limit_val > -990.0f) {
            g_temp_upper_limit = limit_val;
            printf("[Boot] 수신된 온도 임계 상한값 캐시 갱신 완료: %.1f C\n", limit_val);
        }
    }

    // 센서 조회 실패했거나 매핑된 센서가 없을 때의 안전 폴백
    if (g_sensor_count == 0) {
        g_sensors[0].sensor_id = 1; // Default sensor id
        strcpy(g_sensors[0].sensor_type, "Temp");
        strcpy(g_sensors[0].sensor_memo, "Default");
        g_sensor_count = 1;
        printf("[Boot] DB 센서 매핑 조회 실패 또는 센서 없음. 기본값(ID:1)으로 폴백 설정 완료.\n");
    } else {
        printf("[Boot] DB 센서 매핑 정보 캐싱 완료 (총 %d개):\n", g_sensor_count);
        for (int i = 0; i < g_sensor_count; i++) {
            printf("  - Sensor %d: ID=%d, Type=%s, Memo=%s\n", i + 1, g_sensors[i].sensor_id, g_sensors[i].sensor_type, g_sensors[i].sensor_memo);
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
    lcd_params.current_temperature = (status_ch0 == 0) ? ntc_temp_ch0 : (-990.0f - (float)status_ch0);
    lcd_params.current_temperature_ch1 = (status_ch1 == 0) ? ntc_temp_ch1 : (-990.0f - (float)status_ch1);
    
    // 가로채기 방지 락 해제
    lcd_params.is_modem_busy = false;

    printf("[BootTask] 자가 진단 및 보고 스레드 완료 성공. 자가 소멸합니다.\n");
    vTaskDelete(NULL);
}


