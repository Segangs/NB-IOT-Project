#include "tasks_periodic_modem.hpp"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../config.h"
#include "tasks_modem.hpp"
#include "../lib/flash_logger.hpp"
#include "app_context.hpp"

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
                
                // 디버그 태스크의 시리얼 데이터 가로채기 방지 락 온
                lcd_params.is_modem_busy = true;
                vTaskDelay(pdMS_TO_TICKS(10));
                
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
                                // JSON 배열 페이로드 구조 [sensor_id,send_val]
                                char payload[64];
                                snprintf(payload, sizeof(payload), "[%d,%.1f]", sensor_id, send_val);
                                
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
                
                // 가로채기 방지 락 해제
                lcd_params.is_modem_busy = false;
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
                    // 디버그 태스크의 시리얼 데이터 가로채기 방지 락 온
                    lcd_params.is_modem_busy = true;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    
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
                    
                    // 가로채기 방지 락 해제
                    lcd_params.is_modem_busy = false;
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


