#ifndef APP_CONTEXT_HPP
#define APP_CONTEXT_HPP

#include <stdint.h>
#include "../config.h"
#include "tasks_lcd.hpp"

struct SensorInfo {
    int user_sensor_id = -1;
    int sensor_ctgy_id = -1;
    char sensor_ctgy_type[8] = {0};
    char sensor_ctgy_model[32] = {0};
    float temp_upper_limit = DEFAULT_TEMP_UPPER_LIMIT;
    float temp_lower_limit = -999.0f;
};

extern int g_boot_reason_code;
extern int g_boot_cmd_id;
extern SensorInfo g_sensors[4];
extern int g_sensor_count;

extern volatile float g_temp_upper_limit;
extern volatile float g_temp_upper_limit_ch0;
extern volatile float g_temp_upper_limit_ch1;
extern volatile float g_temp_lower_limit_ch0;
extern volatile float g_temp_lower_limit_ch1;
extern volatile bool g_buzzer_trigger;
extern volatile bool g_buzzer_active;
extern volatile uint32_t g_temp_ch0_sample_seq;
extern volatile uint32_t g_temp_ch1_sample_seq;
extern volatile bool g_mic1_stream_active;
extern volatile bool g_mic2_stream_active;
extern volatile float g_boot_pico_temperature;
extern volatile int g_boot_flash_integrity;

extern LcdTaskParams lcd_params;

int parse_sensors_json(const char *json, SensorInfo *sensors, int max_sensors);
void init_fixed_sensor_map();
int extract_json_int(const char *json, const char *key);
float extract_json_float(const char *json, const char *key);
bool apply_mqtt_config_payload(const char *payload);

#endif // APP_CONTEXT_HPP
