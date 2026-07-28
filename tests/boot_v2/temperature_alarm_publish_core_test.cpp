#include "temperature_alarm_publish_core.hpp"

#include <cstdio>

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

void test_edges_and_retries()
{
    boot_v2::TemperatureAlarmPublishCore core;

    CHECK(core.observe(false, false).publish_required == 0);
    CHECK(core.observe(true, false).publish_required == 0);
    CHECK(core.observe(true, true).publish_required == 1);
    CHECK(core.observe(false, false).publish_required == 0);
    CHECK(core.observe(true, true).publish_required == 1);
    core.confirm_submitted();
    CHECK(core.observe(true, true).publish_required == 0);
    CHECK(core.observe(true, false).publish_required == 1);
    CHECK(core.observe(true, false).publish_required == 1);
    core.confirm_submitted();
    CHECK(core.observe(true, false).publish_required == 0);
    CHECK(core.observe(true, true).publish_required == 1);
    core.confirm_submitted();
    CHECK(core.observe(true, true).publish_required == 0);
}

void test_initial_high_is_not_lost()
{
    boot_v2::TemperatureAlarmPublishCore core;
    const auto first = core.observe(true, true);
    CHECK(first.publish_required == 1);
    CHECK(first.alarm_high == 1);
    const auto retry = core.observe(true, false);
    CHECK(retry.publish_required == 1);
    CHECK(retry.alarm_high == 1);
    core.confirm_submitted();
    const auto recovered = core.observe(true, false);
    CHECK(recovered.publish_required == 1);
    CHECK(recovered.alarm_high == 0);
}

} // namespace

int main()
{
    test_edges_and_retries();
    test_initial_high_is_not_lost();
    if (failures != 0) {
        std::printf("temperature_alarm_publish_core_test: %d/%d failed\n",
                    failures, checks);
        return 1;
    }
    std::printf("temperature_alarm_publish_core_test: %d checks passed\n",
                checks);
    return 0;
}
