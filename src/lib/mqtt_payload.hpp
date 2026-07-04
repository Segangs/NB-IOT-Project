#ifndef MQTT_PAYLOAD_HPP
#define MQTT_PAYLOAD_HPP

#include <stddef.h>

bool mqtt_config_payload_extract_object(const char *payload, char *out, size_t out_len);
bool mqtt_telemetry_payload_build(int sensor_id, float value, char *out, size_t out_len);
bool mqtt_telemetry_payload_validate(const char *payload, int *sensor_id, float *value);

#endif // MQTT_PAYLOAD_HPP
