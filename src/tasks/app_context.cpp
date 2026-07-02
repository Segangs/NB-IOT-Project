#include "app_context.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/watchdog.h"
#include "../config.h"

// Global boot reason codes (0: Normal, 1: Cmd Reboot, 2: Watchdog Timeout, 3: Power Cut/Brown-out)
int g_boot_reason_code = 0;
int g_boot_cmd_id = 0;

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
volatile uint32_t g_temp_ch0_sample_seq = 0;
volatile uint32_t g_temp_ch1_sample_seq = 0;
volatile bool g_mic1_stream_active = false;
volatile bool g_mic2_stream_active = false;

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
void safe_reboot(int delay_ms, int cmd_id) {
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

