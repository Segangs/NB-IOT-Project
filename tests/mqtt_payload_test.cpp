#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/lib/mqtt_payload.hpp"

static size_t g_failures = 0;

static void check(
    const bool condition,
    const char *const expression,
    const int line)
{
    if (!condition)
    {
        ++g_failures;
        fprintf(
            stderr,
            "CHECK failed: %s:%d: %s\n",
            __FILE__,
            line,
            expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

int main()
{
    char object[192];
    const char *const config_topic = "devices/x/config";

    CHECK(!mqtt_config_payload_extract_object("undefined", object, sizeof(object)));
    CHECK(!mqtt_config_payload_extract_object("", object, sizeof(object)));
    CHECK(!mqtt_config_payload_extract_object("   \t\r\n", object, sizeof(object)));
    CHECK(!mqtt_config_payload_extract_object("not-json", object, sizeof(object)));
    CHECK(!mqtt_config_payload_extract_object("null", object, sizeof(object)));
    CHECK(!mqtt_config_payload_extract_object("[]", object, sizeof(object)));
    CHECK(mqtt_config_payload_extract_object("{}", object, sizeof(object)));
    CHECK(strcmp(object, "{}") == 0);
    CHECK(mqtt_config_payload_extract_object("[{\"cmd\":null,\"cmdId\":null,\"setTmpUpLimit\":-10,\"setTmpLowLimit\":null}]", object, sizeof(object)));
    CHECK(strstr(object, "\"setTmpUpLimit\":-10") != nullptr);

    size_t frame_len = 99;
    size_t payload_len = 99;

    const char *const wrong_then_exact_config_frame =
        "+KMQTT_DATA: 1,\"devices/x/cmd/response\",\"[91,92]\"\r\n"
        "+KMQTT_DATA: 1,\"devices/x/config\",\"[-7.5,-10.25]\"\r\n";
    CHECK(mqtt_kmqtt_data_extract_payload(
        wrong_then_exact_config_frame,
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(strcmp(object, "[-7.5,-10.25]") == 0);
    CHECK(payload_len == strlen(object));

    memset(object, 'x', sizeof(object));
    frame_len = 99;
    payload_len = 99;
    CHECK(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/cmd/response\",\"[91,92]\"\r\n",
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(object[0] == '\0');
    CHECK(frame_len == 0);
    CHECK(payload_len == 0);

    CHECK(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}",
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(frame_len == 0);
    CHECK(payload_len == 0);
    CHECK(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"{\"userSensorId\":1",
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}]",
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(!mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}]\"",
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    const char *const complete_array_frame =
        "OK\r\n+KMQTT_DATA: 1,\"devices/x/config\",\"[{\"userSensorId\":1}]\"\r\n";
    CHECK(mqtt_kmqtt_data_extract_payload(
        complete_array_frame,
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(strcmp(object, "[{\"userSensorId\":1}]") == 0);
    CHECK(frame_len == strlen(strstr(complete_array_frame, "+KMQTT_DATA:")));
    CHECK(payload_len == strlen(object));
    CHECK(mqtt_kmqtt_data_extract_payload(
        "+KMQTT_DATA: 1,\"devices/x/config\",\"{\"label\":\"[partial]\",\"value\":1}\"\r\n",
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(strcmp(object, "{\"label\":\"[partial]\",\"value\":1}") == 0);
    CHECK(payload_len == strlen(object));

    const char *const compact_frame =
        "+KMQTT_DATA: 2,\"devices/x/config\",\"[-7,-10]\"\r\n";
    CHECK(mqtt_kmqtt_data_extract_payload(
        compact_frame,
        config_topic,
        object,
        sizeof(object),
        &frame_len,
        &payload_len));
    CHECK(strcmp(object, "[-7,-10]") == 0);
    CHECK(payload_len == 8);

    float temp1 = 123.0f;
    float temp2 = 456.0f;
    CHECK(mqtt_config_compact_limits_parse("[-7,-10]", &temp1, &temp2));
    CHECK(fabsf(temp1 - -7.0f) < 0.01f);
    CHECK(fabsf(temp2 - -10.0f) < 0.01f);

    CHECK(mqtt_config_compact_limits_parse("[-0,2.5]", &temp1, &temp2));
    CHECK(temp1 == 0.0f);
    CHECK(fabsf(temp2 - 2.5f) < 0.01f);
    CHECK(mqtt_config_compact_limits_parse("[-7e0,-1E+1]", &temp1, &temp2));
    CHECK(fabsf(temp1 - -7.0f) < 0.01f);
    CHECK(fabsf(temp2 - -10.0f) < 0.01f);

    temp1 = 123.0f;
    temp2 = 456.0f;
    CHECK(!mqtt_config_compact_limits_parse(nullptr, &temp1, &temp2));
    CHECK(temp1 == 123.0f);
    CHECK(temp2 == 456.0f);
    CHECK(!mqtt_config_compact_limits_parse("[-7,-10]", nullptr, &temp2));
    CHECK(temp2 == 456.0f);
    CHECK(!mqtt_config_compact_limits_parse("[-7,-10]", &temp1, nullptr));
    CHECK(temp1 == 123.0f);

    const char *const invalid[] = {
        "[]", "[-7]", "[-7,-10,0]", "[null,-10]", "[\"-7\",-10]",
        "[nan,-10]", "[-7,inf]", "[-7,-10]x", "[-7,-10",
        "[+1,2]", "[01,2]", "[.5,2]", "[1.,2]", "[0x1p0,2]"};
    for (const char *payload : invalid)
    {
        temp1 = 123.0f;
        temp2 = 456.0f;
        CHECK(!mqtt_config_compact_limits_parse(payload, &temp1, &temp2));
        CHECK(temp1 == 123.0f);
        CHECK(temp2 == 456.0f);
    }

    int sensor_id = 0;
    float value = 0.0f;
    CHECK(mqtt_telemetry_payload_build(1, -16.6f, object, sizeof(object)));
    CHECK(strcmp(object, "[1,-16.6]") == 0);
    CHECK(mqtt_telemetry_payload_validate(object, &sensor_id, &value));
    CHECK(sensor_id == 1);
    CHECK(fabsf(value - -16.6f) < 0.01f);
    CHECK(!mqtt_telemetry_payload_validate("[1,null]", nullptr, nullptr));
    CHECK(!mqtt_telemetry_payload_validate("[1,\"x\"]", nullptr, nullptr));
    CHECK(!mqtt_telemetry_payload_build(0, -16.6f, object, sizeof(object)));
    CHECK(!mqtt_telemetry_payload_build(1, -991.0f, object, sizeof(object)));

    if (g_failures != 0)
    {
        fprintf(stderr, "mqtt_payload_test: %zu checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
