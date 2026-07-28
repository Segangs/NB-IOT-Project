#include "lcd_status_policy.hpp"

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
