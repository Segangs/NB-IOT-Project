#ifndef MQTT_PAYLOAD_HPP
#define MQTT_PAYLOAD_HPP

#include <stddef.h>

bool mqtt_config_payload_extract_object(const char *payload, char *out, size_t out_len);
bool mqtt_config_compact_limits_parse(
    const char *payload,
    float *temp1_upper,
    float *temp2_upper);
bool mqtt_kmqtt_data_extract_payload(
    const char *frame,
    const char *expected_topic,
    char *out,
    size_t out_len,
    size_t *frame_len,
    size_t *payload_len);
bool mqtt_telemetry_payload_build(int sensor_id, float value, char *out, size_t out_len);
bool mqtt_telemetry_payload_validate(const char *payload, int *sensor_id, float *value);

#endif // MQTT_PAYLOAD_HPP
