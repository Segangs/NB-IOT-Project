#ifndef APP_CONTEXT_HPP
#define APP_CONTEXT_HPP

#include <stdint.h>
#include "tasks_lcd.hpp"
#include "tasks_modem.hpp"

struct SensorInfo {
    int sensor_id = -1;
    char sensor_type[32] = {0};
    char sensor_memo[32] = {0};
};

extern int g_boot_reason_code;
extern int g_boot_cmd_id;
extern SensorInfo g_sensors[2];
extern int g_sensor_count;

extern volatile float g_temp_upper_limit;
extern volatile bool g_buzzer_trigger;
extern volatile bool g_buzzer_active;
extern volatile uint32_t g_temp_ch0_sample_seq;
extern volatile uint32_t g_temp_ch1_sample_seq;
extern volatile bool g_mic1_stream_active;
extern volatile bool g_mic2_stream_active;

extern LcdTaskParams lcd_params;
extern nb_iot modem;

int parse_sensors_json(const char *json, SensorInfo *sensors, int max_sensors);
int extract_json_int(const char *json, const char *key);
float extract_json_float(const char *json, const char *key);
void safe_reboot(int delay_ms, int cmd_id = 0);

#endif // APP_CONTEXT_HPP
