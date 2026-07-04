#include "tasks_periodic_modem.hpp"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../config.h"
#include "tasks_modem.hpp"
#include "../lib/flash_logger.hpp"
#include "../lib/log.hpp"
#include "../lib/mqtt_payload.hpp"
#include "app_context.hpp"

static uint32_t mqtt_backoff_ms(int failure_count)
{
    if (failure_count <= 1) return 3000;
    if (failure_count == 2) return 5000;
    if (failure_count == 3) return 10000;
    return 60000;
}

static void handle_config_urc()
{
    const char *rx = modem.get_rx_buffer();
    if (strstr(rx, "+KMQTT_DATA:") == nullptr) {
        return;
    }

    const char *payload_start = strchr(rx, '{');
    const char *array_start = strchr(rx, '[');
    if (array_start != nullptr && (payload_start == nullptr || array_start < payload_start)) {
        payload_start = array_start;
    }
    if (payload_start == nullptr) {
        LOG("CONFIG_IGNORE\n");
        return;
    }

    char payload[512];
    strncpy(payload, payload_start, sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';

    char *json_end = strrchr(payload, '}');
    char *array_end = strrchr(payload, ']');
    if (array_end != nullptr && (json_end == nullptr || array_end > json_end)) {
        *(array_end + 1) = '\0';
    } else if (json_end != nullptr) {
        *(json_end + 1) = '\0';
    }

    apply_mqtt_config_payload(payload, true);
}

static bool publish_temperature_payload(const char *topic, int user_sensor_id, float value)
{
    char payload[64];
    if (!mqtt_telemetry_payload_build(user_sensor_id, value, payload, sizeof(payload)) ||
        !mqtt_telemetry_payload_validate(payload, nullptr, nullptr)) {
        LOG("TELEMETRY_SKIP_BAD %d\n", user_sensor_id);
        return false;
    }

    return modem.modem_MqttPublish(topic, payload);
}

// ====================================================================================
// Core 0 Task: Dedicated Modem Communication Controller (Periodic Telemetry & RSSI)
// ====================================================================================
void vPeriodicModemTask(void *pvParameters)
{
    // 부팅 자가 진단이 완료될 때까지 안전하게 대기
    while (lcd_params.is_booting) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    LOG("PERIODIC_READY\n");
    
    uint32_t last_rssi_time_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_temp_send_time_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_error_check_time_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_config_request_time_ms = 0;
    uint32_t next_mqtt_reconnect_time_ms = 0;
    int mqtt_failure_count = 0;
    bool mqtt_config_subscribed = false;

    while (true)
    {
        uint32_t current_time_ms = to_ms_since_boot(get_absolute_time());

        if (lcd_params.is_unauthenticated) {
            if (modem.is_connected()) {
                lcd_params.is_modem_busy = true;
                modem.modem_MqttClose();
                lcd_params.is_modem_busy = false;
            }
        } else if (!modem.is_connected() && current_time_ms >= next_mqtt_reconnect_time_ms) {
            lcd_params.is_modem_busy = true;
            vTaskDelay(pdMS_TO_TICKS(10));

            int cereg_val = modem.check_network_registration();
            int csq_val = modem.check_rssi_csq();
            lcd_params.current_csq = csq_val;
            lcd_params.is_searching_network = (csq_val == 99 || csq_val == 0);
            bool network_good = ((cereg_val == 1 || cereg_val == 5) && (csq_val != 99 && csq_val > 0));

            if (network_good &&
                modem.modem_MqttOpen(MQTT_BROKER_HOST, MQTT_BROKER_PORT, modem.get_imei(), modem.get_imei(), modem.get_cimi())) {
                char config_topic[64];
                snprintf(config_topic, sizeof(config_topic), "devices/%s/config", modem.get_imei());
                mqtt_config_subscribed = modem.modem_MqttSubscribe(config_topic);
                if (mqtt_config_subscribed) {
                    LOG("MQTT_READY\n");
                    mqtt_failure_count = 0;
                    last_config_request_time_ms = 0;
                } else {
                    mqtt_failure_count++;
                    next_mqtt_reconnect_time_ms = current_time_ms + mqtt_backoff_ms(mqtt_failure_count);
                    modem.modem_MqttClose();
                }
            } else {
                mqtt_failure_count++;
                next_mqtt_reconnect_time_ms = current_time_ms + mqtt_backoff_ms(mqtt_failure_count);
                if (modem.is_unauthenticated) {
                    lcd_params.is_unauthenticated = true;
                }
                LOG("MQTT_RECONNECT_WAIT %lu\n", (unsigned long)mqtt_backoff_ms(mqtt_failure_count));
            }

            lcd_params.is_modem_busy = false;
        }

        if (modem.is_connected()) {
            lcd_params.is_modem_busy = true;
            if (!modem.modem_MqttPoll(50)) {
                mqtt_config_subscribed = false;
                mqtt_failure_count++;
                next_mqtt_reconnect_time_ms = current_time_ms + mqtt_backoff_ms(mqtt_failure_count);
            } else {
                handle_config_urc();
            }

            if (mqtt_config_subscribed &&
                (last_config_request_time_ms == 0 || current_time_ms - last_config_request_time_ms >= 60000)) {
                char config_request_topic[80];
                snprintf(config_request_topic, sizeof(config_request_topic), "devices/%s/config/request", modem.get_imei());
                if (modem.modem_MqttPublish(config_request_topic, "{}")) {
                    LOG("CONFIG_REQ_OK\n");
                    last_config_request_time_ms = current_time_ms;
                } else {
                    LOG("CONFIG_REQ_FAIL\n");
                    mqtt_config_subscribed = false;
                    mqtt_failure_count++;
                    next_mqtt_reconnect_time_ms = current_time_ms + mqtt_backoff_ms(mqtt_failure_count);
                }
            }
            lcd_params.is_modem_busy = false;
        }

        // 1. RSSI Signal check (Every 5 minutes)
        if (current_time_ms - last_rssi_time_ms >= (MODEM_RSSI_CHECK_INTERVAL_MIN * 60 * 1000))
        {
            LOG("RSSI_CHECK\n");
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
                LOG("MQTT_AUTH_BLOCK\n");
                last_temp_send_time_ms = current_time_ms; // 주기 타이머만 리셋
            }
            else
            {
                LOG("TELEMETRY_START\n");
                
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
                    LOG("NETWORK_OK CEREG=%d CSQ=%d\n", cereg_val, csq_val);
                    lcd_params.is_transmitting = true;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    
                    bool all_sends_success = true;
                    char telemetry_topic[64];
                    snprintf(telemetry_topic, sizeof(telemetry_topic), "devices/%s/telemetry", modem.get_imei());

                    if (modem.is_connected())
                    {
                        for (int user_sensor_id = 1; user_sensor_id <= 2; user_sensor_id++)
                        {
                            float send_val = (user_sensor_id == 1) ? lcd_params.current_temperature : lcd_params.current_temperature_ch1;
                            
                            if (send_val > -990.0f)
                            {
                                if (publish_temperature_payload(telemetry_topic, user_sensor_id, send_val))
                                {
                                    LOG("TELEMETRY_OK %d\n", user_sensor_id);
                                    flash_log_write(send_val, lcd_params.current_vsys_voltage, 1, 0, 200, 0);
                                }
                                else
                                {
                                    LOG("TELEMETRY_FAIL %d\n", user_sensor_id);
                                    all_sends_success = false;
                                    flash_log_write(send_val, lcd_params.current_vsys_voltage, 0, 0, -2, 0);
                                }
                            }
                            else
                            {
                                LOG("SENSOR_SKIP %d\n", user_sensor_id);
                                int temp_sensor_err = (send_val <= -990.0f) ? (int)(-990.0f - send_val) : 99;
                                flash_log_write(send_val, lcd_params.current_vsys_voltage, 0, temp_sensor_err, 0, 101);
                            }
                        }

                        uint32_t wait_elapsed = 0;
                        while (wait_elapsed < 3000) {
                            modem_sleep(100);
                            wait_elapsed += 100;
                            if (!modem.modem_MqttPoll(50)) break;
                            handle_config_urc();
                        }
                    }
                    else
                    {
                        LOG("MQTT_CONNECT_FAIL\n");
                        all_sends_success = false;
                        if (modem.is_unauthenticated) {
                            LOG("MQTT_AUTH_FAIL\n");
                            lcd_params.is_unauthenticated = true;
                        }
                    }

                    lcd_params.is_transmitting = false;
                    
                    if (all_sends_success)
                    {
                        LOG("TELEMETRY_DONE\n");
                        last_temp_send_time_ms = current_time_ms;
                    }
                    else
                    {
                        LOG("TELEMETRY_RETRY\n");
                        last_temp_send_time_ms = current_time_ms - (SENSOR_TEMP_CHECK_INTERVAL_MIN * 60 * 1000) + (60 * 1000);
                    }
                }
                else
                {
                    LOG("NETWORK_BAD\n");
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
                bool vsys_stable = lcd_params.is_vsys_stable;
                float temp_sensor = lcd_params.current_temperature;
                bool temp_sensor_fault = (temp_sensor <= -990.0f);
                
                if (!vsys_stable || temp_sensor_fault)
                {
                    // 디버그 태스크의 시리얼 데이터 가로채기 방지 락 온
                    lcd_params.is_modem_busy = true;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    
                    LOG("ALERT_START\n");
                    
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
                        
                        if (modem.is_connected())
                        {
                            if (modem.modem_MqttPublish(alert_topic, "{\"alert\":1}"))
                            {
                                err_send_success = true;
                            }
                        }
                        lcd_params.is_transmitting = false;
                    }

                    if (err_send_success)
                    {
                        LOG("ALERT_OK\n");
                        last_error_check_time_ms = current_time_ms;
                    }
                    else
                    {
                        LOG("ALERT_RETRY\n");
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
