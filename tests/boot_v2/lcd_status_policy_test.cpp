#include "lcd_status_policy.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

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

boot_v2::SensorSample fresh_sample(
    const std::int16_t value_deci_celsius,
    const std::uint32_t observed_at_ms) noexcept
{
    boot_v2::SensorSample sample{};
    sample.fault = boot_v2::SensorSampleFault::None;
    sample.value_deci_celsius = value_deci_celsius;
    sample.observed_at_monotonic_ms = observed_at_ms;
    return sample;
}

boot_v2::SensorSample fault_sample(
    const boot_v2::SensorSampleFault fault,
    const std::uint32_t observed_at_ms) noexcept
{
    boot_v2::SensorSample sample{};
    sample.fault = fault;
    sample.observed_at_monotonic_ms = observed_at_ms;
    return sample;
}

std::string read_source(const char *path)
{
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void test_battery_mode_uses_batt_label_on_top_row()
{
    CHECK(std::strcmp(
              boot_v2::lcd_normal_status_line(true, "Ready"),
              "BATT MODE") == 0);
}

void test_external_power_keeps_runtime_status_on_top_row()
{
    CHECK(std::strcmp(
              boot_v2::lcd_normal_status_line(false, "Ready"),
              "Ready") == 0);
}

void test_crc_fallback_with_valid_display_value_is_visible()
{
    CHECK(boot_v2::lcd_temperature_value_visible(2, true));
}

void test_missing_or_expired_display_value_is_hidden()
{
    CHECK(!boot_v2::lcd_temperature_value_visible(1, false));
    CHECK(!boot_v2::lcd_temperature_value_visible(2, false));
}

void test_raw_success_status_does_not_override_display_validity()
{
    CHECK(!boot_v2::lcd_temperature_value_visible(0, false));
}

void test_zero_initialized_producer_display_state_is_invalid()
{
    const boot_v2::LcdSensorDisplayState state{};

    CHECK(state.channel0.raw_status == 0);
    CHECK(!state.channel0.value_valid);
    CHECK(state.channel0.value_celsius == 0.0f);
    CHECK(state.channel1.raw_status == 0);
    CHECK(!state.channel1.value_valid);
    CHECK(state.channel1.value_celsius == 0.0f);
}

void test_fresh_and_crc_fallback_preserve_raw_status_and_value()
{
    boot_v2::SensorQualityCore quality;
    boot_v2::SensorQualityDecision fresh{};
    boot_v2::SensorQualityDecision crc_fallback{};

    CHECK(quality.observe(fresh_sample(-75, 1000), fresh));
    const auto fresh_state =
        boot_v2::make_lcd_temperature_display_state(0, fresh);
    CHECK(fresh_state.raw_status == 0);
    CHECK(fresh_state.value_valid);
    CHECK(fresh_state.value_celsius == -7.5f);

    CHECK(quality.observe(
        fault_sample(boot_v2::SensorSampleFault::CrcMismatch, 2000),
        crc_fallback));
    const auto fallback_state =
        boot_v2::make_lcd_temperature_display_state(2, crc_fallback);
    CHECK(fallback_state.raw_status == 2);
    CHECK(fallback_state.value_valid);
    CHECK(fallback_state.value_celsius == -7.5f);
}

void test_no_presence_and_expired_fallback_remain_invalid()
{
    boot_v2::SensorQualityCore no_presence_quality;
    boot_v2::SensorQualityDecision no_presence{};
    CHECK(no_presence_quality.observe(fresh_sample(250, 1000), no_presence));
    CHECK(no_presence_quality.observe(
        fault_sample(boot_v2::SensorSampleFault::NoPresence, 2000),
        no_presence));
    const auto missing_state =
        boot_v2::make_lcd_temperature_display_state(1, no_presence);
    CHECK(missing_state.raw_status == 1);
    CHECK(!missing_state.value_valid);
    CHECK(missing_state.value_celsius == -991.0f);

    boot_v2::SensorQualityCore expired_quality;
    boot_v2::SensorQualityDecision expired{};
    CHECK(expired_quality.observe(fresh_sample(250, 1000), expired));
    CHECK(expired_quality.observe(
        fault_sample(boot_v2::SensorSampleFault::CrcMismatch, 31001),
        expired));
    const auto expired_state =
        boot_v2::make_lcd_temperature_display_state(2, expired);
    CHECK(expired_state.raw_status == 2);
    CHECK(!expired_state.value_valid);
    CHECK(expired_state.value_celsius == -992.0f);
}

void test_invalid_decision_does_not_become_visible_from_raw_success()
{
    const boot_v2::SensorQualityDecision invalid{};
    const auto state =
        boot_v2::make_lcd_temperature_display_state(0, invalid);

    CHECK(state.raw_status == 0);
    CHECK(!state.value_valid);
    CHECK(state.value_celsius == -990.0f);
}

void test_two_channel_mapping_applies_each_channel_independently()
{
    boot_v2::SensorQualityCore first_quality;
    boot_v2::SensorQualityCore second_quality;
    boot_v2::SensorQualityDecision first{};
    boot_v2::SensorQualityDecision second{};
    CHECK(first_quality.observe(fresh_sample(123, 1000), first));
    CHECK(first_quality.observe(
        fault_sample(boot_v2::SensorSampleFault::CrcMismatch, 2000),
        first));
    CHECK(second_quality.observe(fresh_sample(456, 1000), second));
    CHECK(second_quality.observe(
        fault_sample(boot_v2::SensorSampleFault::NoPresence, 2000),
        second));

    const auto state = boot_v2::make_lcd_sensor_display_state(
        2, first, 1, second);

    CHECK(state.channel0.raw_status == 2);
    CHECK(state.channel0.value_valid);
    CHECK(state.channel0.value_celsius == 12.3f);
    CHECK(state.channel1.raw_status == 1);
    CHECK(!state.channel1.value_valid);
    CHECK(state.channel1.value_celsius == -991.0f);
}

void test_battery_mode_does_not_replace_temperature_row()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_lcd.cpp");
    CHECK(!source.empty());
    CHECK(source.find(
              "lcd_normal_status_line(\n"
              "                    params->is_battery_mode,") !=
          std::string::npos);
    CHECK(source.find(
              "if (params->is_battery_mode) {\n"
              "                snprintf(temp_str") ==
          std::string::npos);
    CHECK(source.find("\"BATT MODE\"") == std::string::npos);
}

} // namespace

int main()
{
    test_battery_mode_uses_batt_label_on_top_row();
    test_external_power_keeps_runtime_status_on_top_row();
    test_crc_fallback_with_valid_display_value_is_visible();
    test_missing_or_expired_display_value_is_hidden();
    test_raw_success_status_does_not_override_display_validity();
    test_zero_initialized_producer_display_state_is_invalid();
    test_fresh_and_crc_fallback_preserve_raw_status_and_value();
    test_no_presence_and_expired_fallback_remain_invalid();
    test_invalid_decision_does_not_become_visible_from_raw_success();
    test_two_channel_mapping_applies_each_channel_independently();
    test_battery_mode_does_not_replace_temperature_row();
    if (failures != 0) {
        std::printf(
            "lcd_status_policy_test: %d/%d failed\n",
            failures,
            checks);
        return 1;
    }
    std::printf(
        "lcd_status_policy_test: %d checks passed\n",
        checks);
    return 0;
}
