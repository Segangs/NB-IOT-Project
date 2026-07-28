#include "mqtt_power_event_codec.hpp"

#include <cstdio>
#include <cstring>

namespace {

int checks = 0;
int failures = 0;

void check(const bool condition, const char *expression, const int line)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using boot_v2::MqttPowerEvent;

MqttPowerEvent removed()
{
    return {4, 42, 1, 1, 0, 0, 0, 0};
}

MqttPowerEvent restored()
{
    return {5, 42, 2, 0, 1, 0, 0, 0};
}

MqttPowerEvent dying()
{
    return {6, 42, 2, 2, 210, 90, 0, 0};
}

void check_payload(
    const MqttPowerEvent event,
    const char *expected)
{
    char output[81]{};
    CHECK(boot_v2::mqtt_power_event_is_canonical(event));
    CHECK(boot_v2::mqtt_power_event_build(
        event, output, sizeof(output)));
    CHECK(std::strcmp(output, expected) == 0);
    CHECK(std::strlen(output) <= 80);
    CHECK(output[std::strlen(output)] == '\0');
}

void test_golden_payloads()
{
    check_payload(removed(), "[1,4,42,1,1,0,0,0,0]");
    check_payload(restored(), "[1,5,42,2,0,1,0,0,0]");
    check_payload(dying(), "[1,6,42,2,2,210,90,0,0]");
}

void test_maximum_payload_stays_within_modem_limit()
{
    const MqttPowerEvent maximum{
        6,
        4294967295u,
        4294967295u,
        2,
        300,
        300,
        4294967295u,
        1};
    char output[81]{};
    CHECK(boot_v2::mqtt_power_event_build(
        maximum, output, sizeof(output)));
    CHECK(std::strcmp(
              output,
              "[1,6,4294967295,4294967295,2,300,300,4294967295,1]") ==
          0);
    CHECK(std::strlen(output) == 50);
}

void test_invalid_fields_are_rejected()
{
    MqttPowerEvent value = removed();

    value.event_type = 3;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = removed();
    value.incident_id = 0;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = removed();
    value.sequence = 0;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = removed();
    value.state_code = 0;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = removed();
    value.value0 = 1;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = restored();
    value.value1 = 1;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = dying();
    value.state_code = 1;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = dying();
    value.value0 = 301;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = dying();
    value.value1 = -1;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = dying();
    value.clock_valid = 2;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
    value = dying();
    value.unix_seconds = 1;
    CHECK(!boot_v2::mqtt_power_event_is_canonical(value));
}

void test_output_bounds_are_fail_closed()
{
    char exact[25]{};
    char short_output[20]{};
    CHECK(!boot_v2::mqtt_power_event_build(
        removed(), nullptr, sizeof(exact)));
    CHECK(!boot_v2::mqtt_power_event_build(removed(), exact, 0));
    CHECK(!boot_v2::mqtt_power_event_build(
        removed(), short_output, sizeof(short_output)));
    CHECK(short_output[sizeof(short_output) - 1] == '\0');
    CHECK(boot_v2::mqtt_power_event_build(
        removed(), exact, sizeof(exact)));
    CHECK(std::strcmp(exact, "[1,4,42,1,1,0,0,0,0]") == 0);
}

} // namespace

int main()
{
    test_golden_payloads();
    test_maximum_payload_stays_within_modem_limit();
    test_invalid_fields_are_rejected();
    test_output_bounds_are_fail_closed();
    if (failures != 0) {
        std::printf("mqtt_power_event_codec_test: %d/%d failed\n",
                    failures, checks);
        return 1;
    }
    std::printf("mqtt_power_event_codec_test: %d checks passed\n",
                checks);
    return 0;
}
