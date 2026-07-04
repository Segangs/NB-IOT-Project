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
