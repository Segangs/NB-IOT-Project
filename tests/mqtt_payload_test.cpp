#include <assert.h>
#include <math.h>
#include <string.h>

#include "../src/lib/mqtt_payload.hpp"

int main()
{
    char object[192];

    assert(!mqtt_config_payload_extract_object("undefined", object, sizeof(object)));
    assert(!mqtt_config_payload_extract_object("", object, sizeof(object)));
    assert(!mqtt_config_payload_extract_object("   \t\r\n", object, sizeof(object)));
    assert(!mqtt_config_payload_extract_object("not-json", object, sizeof(object)));
    assert(!mqtt_config_payload_extract_object("null", object, sizeof(object)));
    assert(!mqtt_config_payload_extract_object("[]", object, sizeof(object)));
    assert(mqtt_config_payload_extract_object("{}", object, sizeof(object)));
    assert(strcmp(object, "{}") == 0);
    assert(mqtt_config_payload_extract_object("[{\"cmd\":null,\"cmdId\":null,\"setTmpUpLimit\":-10,\"setTmpLowLimit\":null}]", object, sizeof(object)));
    assert(strstr(object, "\"setTmpUpLimit\":-10") != nullptr);

    size_t frame_len = 99;
    size_t payload_len = 99;
    assert(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}",
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    assert(frame_len == 0);
    assert(payload_len == 0);
    assert(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"{\"userSensorId\":1",
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    assert(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}]",
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    assert(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}]\"",
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    const char *const complete_array_frame =
        "OK\r\n+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}]\"\r\n";
    assert(mqtt_kmqtt_data_extract_payload(
        complete_array_frame,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    assert(strcmp(object, "[{\"userSensorId\":1}]") == 0);
    assert(frame_len == strlen(strstr(complete_array_frame, "+KMQTT_DATA:")));
    assert(payload_len == strlen(object));
    assert(mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"{\"label\":\"[partial]\",\"value\":1}\"\r\n",
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    assert(strcmp(object, "{\"label\":\"[partial]\",\"value\":1}") == 0);
    assert(payload_len == strlen(object));

    const char *const compact_frame =
        "+KMQTT_DATA: 2,\"devices/x/config\",\"[-7,-10]\"\r\n";
    assert(mqtt_kmqtt_data_extract_payload(
        compact_frame,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    assert(strcmp(object, "[-7,-10]") == 0);
    assert(payload_len == 8);

    float temp1 = 123.0f;
    float temp2 = 456.0f;
    assert(mqtt_config_compact_limits_parse("[-7,-10]", &temp1, &temp2));
    assert(fabsf(temp1 - -7.0f) < 0.01f);
    assert(fabsf(temp2 - -10.0f) < 0.01f);

    assert(mqtt_config_compact_limits_parse("[-0,2.5]", &temp1, &temp2));
    assert(temp1 == 0.0f);
    assert(fabsf(temp2 - 2.5f) < 0.01f);
    assert(mqtt_config_compact_limits_parse("[-7e0,-1E+1]", &temp1, &temp2));
    assert(fabsf(temp1 - -7.0f) < 0.01f);
    assert(fabsf(temp2 - -10.0f) < 0.01f);

    temp1 = 123.0f;
    temp2 = 456.0f;
    assert(!mqtt_config_compact_limits_parse(nullptr, &temp1, &temp2));
    assert(temp1 == 123.0f);
    assert(temp2 == 456.0f);
    assert(!mqtt_config_compact_limits_parse("[-7,-10]", nullptr, &temp2));
    assert(temp2 == 456.0f);
    assert(!mqtt_config_compact_limits_parse("[-7,-10]", &temp1, nullptr));
    assert(temp1 == 123.0f);

    const char *const invalid[] = {
        "[]", "[-7]", "[-7,-10,0]", "[null,-10]", "[\"-7\",-10]",
        "[nan,-10]", "[-7,inf]", "[-7,-10]x", "[-7,-10",
        "[+1,2]", "[01,2]", "[.5,2]", "[1.,2]", "[0x1p0,2]"};
    for (const char *payload : invalid)
    {
        temp1 = 123.0f;
        temp2 = 456.0f;
        assert(!mqtt_config_compact_limits_parse(payload, &temp1, &temp2));
        assert(temp1 == 123.0f);
        assert(temp2 == 456.0f);
    }

    int sensor_id = 0;
    float value = 0.0f;
    assert(mqtt_telemetry_payload_build(1, -16.6f, object, sizeof(object)));
    assert(strcmp(object, "[1,-16.6]") == 0);
    assert(mqtt_telemetry_payload_validate(object, &sensor_id, &value));
    assert(sensor_id == 1);
    assert(fabsf(value - -16.6f) < 0.01f);
    assert(!mqtt_telemetry_payload_validate("[1,null]", nullptr, nullptr));
    assert(!mqtt_telemetry_payload_validate("[1,\"x\"]", nullptr, nullptr));
    assert(!mqtt_telemetry_payload_build(0, -16.6f, object, sizeof(object)));
    assert(!mqtt_telemetry_payload_build(1, -991.0f, object, sizeof(object)));

    return 0;
}
