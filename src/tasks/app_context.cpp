#include "app_context.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../config.h"
#include "../lib/log.hpp"
#include "../lib/mqtt_payload.hpp"
#include "tasks_modem.hpp"

// Global boot reason codes (0: Normal, 1: Cmd Reboot, 2: Watchdog Timeout, 3: Power Cut/Brown-out)
int g_boot_reason_code = 0;
int g_boot_cmd_id = 0;

SensorInfo g_sensors[4];
int g_sensor_count = 0;

void init_fixed_sensor_map()
{
    memset(g_sensors, 0, sizeof(g_sensors));
    for (int i = 0; i < 4; i++)
    {
        g_sensors[i].user_sensor_id = i + 1;
        g_sensors[i].sensor_ctgy_id = (i < 2) ? 1 : 2;
        strncpy(g_sensors[i].sensor_ctgy_type, (i < 2) ? "TMP" : "MIC", sizeof(g_sensors[i].sensor_ctgy_type) - 1);
        strncpy(g_sensors[i].sensor_ctgy_model, (i < 2) ? "DS18B20" : "SPH0645", sizeof(g_sensors[i].sensor_ctgy_model) - 1);
        g_sensors[i].temp_upper_limit = DEFAULT_TEMP_UPPER_LIMIT;
        g_sensors[i].temp_lower_limit = -999.0f;
    }
    g_sensor_count = 4;
    g_temp_upper_limit = DEFAULT_TEMP_UPPER_LIMIT;
    g_temp_upper_limit_ch0 = DEFAULT_TEMP_UPPER_LIMIT;
    g_temp_upper_limit_ch1 = DEFAULT_TEMP_UPPER_LIMIT;
    g_temp_lower_limit_ch0 = -999.0f;
    g_temp_lower_limit_ch1 = -999.0f;
}

static bool copy_json_string_value(const char *start, const char *key, char *out, size_t out_len)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    const char *ptr = strstr(start, search_key);
    if (ptr == nullptr) return false;
    ptr += strlen(search_key);
    while (*ptr == ' ' || *ptr == '\t') ptr++;
    if (*ptr != '"') return false;
    ptr++;
    const char *end = strchr(ptr, '"');
    if (end == nullptr) return false;
    size_t len = (size_t)(end - ptr);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, ptr, len);
    out[len] = '\0';
    return true;
}

// Helper to parse sensor JSON list returned by Supabase
int parse_sensors_json(const char *json, SensorInfo *sensors, int max_sensors) {
    int count = 0;
    const char *ptr = json;
    
    while (count < max_sensors) {
        ptr = strstr(ptr, "\"userSensorId\"");
        if (!ptr) break;
        
        ptr = strchr(ptr, ':');
        if (!ptr) break;
        ptr++; // Skip ':'
        
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        sensors[count].user_sensor_id = atoi(ptr);

        int ctgy = extract_json_int(ptr, "sensorCtgyId");
        sensors[count].sensor_ctgy_id = ctgy;
        copy_json_string_value(ptr, "sensorCtgyType", sensors[count].sensor_ctgy_type, sizeof(sensors[count].sensor_ctgy_type));
        copy_json_string_value(ptr, "sensorCtgyModel", sensors[count].sensor_ctgy_model, sizeof(sensors[count].sensor_ctgy_model));

        float upper = extract_json_float(ptr, "setTmpUpLimit");
        float lower = extract_json_float(ptr, "setTmpLowLimit");
        sensors[count].temp_upper_limit = (upper > -990.0f) ? upper : DEFAULT_TEMP_UPPER_LIMIT;
        sensors[count].temp_lower_limit = lower;
        
        count++;
    }
    return count;
}

// Buzzer Global Control Settings (Default threshold: -10.0C)
volatile float g_temp_upper_limit = DEFAULT_TEMP_UPPER_LIMIT;
volatile float g_temp_upper_limit_ch0 = DEFAULT_TEMP_UPPER_LIMIT;
volatile float g_temp_upper_limit_ch1 = DEFAULT_TEMP_UPPER_LIMIT;
volatile float g_temp_lower_limit_ch0 = -999.0f;
volatile float g_temp_lower_limit_ch1 = -999.0f;
volatile bool g_buzzer_trigger = false;
volatile bool g_buzzer_active = false;
volatile uint32_t g_temp_ch0_sample_seq = 0;
volatile uint32_t g_temp_ch1_sample_seq = 0;
volatile bool g_mic1_stream_active = false;
volatile bool g_mic2_stream_active = false;
volatile float g_boot_pico_temperature = 0.0f;
volatile int g_boot_flash_integrity = 1;

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

bool apply_mqtt_config_payload(const char *payload)
{
    char config[512];
    if (!mqtt_config_payload_extract_object(payload, config, sizeof(config))) {
        LOG("CONFIG_IGNORE\n");
        return false;
    }

    float temp1_upper = 0.0f;
    float temp2_upper = 0.0f;
    if (!mqtt_config_compact_limits_parse(
            config, &temp1_upper, &temp2_upper)) {
        LOG("CONFIG_IGNORE\n");
        return false;
    }

    g_sensors[0].temp_upper_limit = temp1_upper;
    g_sensors[1].temp_upper_limit = temp2_upper;
    g_temp_upper_limit_ch0 = temp1_upper;
    g_temp_upper_limit_ch1 = temp2_upper;
    g_temp_upper_limit = temp1_upper;
    LOG("CONFIG_LIMIT_OK %.1f,%.1f\n", temp1_upper, temp2_upper);
    return true;
}

// ====================================================================================
// Global Shared State
// ====================================================================================
LcdTaskParams lcd_params;
nb_iot modem;
