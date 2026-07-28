#include "status_led_policy.hpp"

#include <cstdint>
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

boot_v2::StatusLedPolicyInput make_input(
    const std::uint32_t now_ms,
    const bool booting = false,
    const bool power_button_shutdown = false,
    const bool battery_grace = false,
    const bool modem_tx_active = false)
{
    boot_v2::StatusLedPolicyInput input{};
    input.now_ms = now_ms;
    input.booting = booting;
    input.power_button_shutdown = power_button_shutdown;
    input.battery_grace = battery_grace;
    input.modem_tx_active = modem_tx_active;
    return input;
}

void test_ready_is_green_and_modem_tx_is_normally_on()
{
    const auto at_zero =
        boot_v2::status_led_outputs(make_input(0));
    CHECK(at_zero.red == 0);
    CHECK(at_zero.green == 1);
    CHECK(at_zero.modem_tx == 1);

    const auto later =
        boot_v2::status_led_outputs(make_input(1350));
    CHECK(later.red == 0);
    CHECK(later.green == 1);
    CHECK(later.modem_tx == 1);
}

void test_booting_blinks_red_every_500ms()
{
    const auto before =
        boot_v2::status_led_outputs(make_input(499, true));
    CHECK(before.red == 0);
    CHECK(before.green == 0);

    const auto first_on =
        boot_v2::status_led_outputs(make_input(500, true));
    CHECK(first_on.red == 1);
    CHECK(first_on.green == 0);

    const auto second_off =
        boot_v2::status_led_outputs(make_input(1000, true));
    CHECK(second_off.red == 0);
    CHECK(second_off.green == 0);
}

void test_power_button_shutdown_blinks_until_power_off()
{
    const auto first_on =
        boot_v2::status_led_outputs(make_input(500, false, true));
    CHECK(first_on.red == 1);
    CHECK(first_on.green == 0);

    const auto second_off =
        boot_v2::status_led_outputs(make_input(1000, false, true));
    CHECK(second_off.red == 0);
    CHECK(second_off.green == 0);
}

void test_adapter_grace_blinks_and_restore_returns_green()
{
    const auto battery =
        boot_v2::status_led_outputs(
            make_input(500, false, false, true));
    CHECK(battery.red == 1);
    CHECK(battery.green == 0);

    const auto restored =
        boot_v2::status_led_outputs(make_input(600));
    CHECK(restored.red == 0);
    CHECK(restored.green == 1);
}

void test_actual_modem_tx_pulse_turns_normally_on_led_off()
{
    const auto active_at_zero =
        boot_v2::status_led_outputs(
            make_input(0, false, false, false, true));
    CHECK(active_at_zero.modem_tx == 0);

    const auto active_later =
        boot_v2::status_led_outputs(
            make_input(100, false, false, false, true));
    CHECK(active_later.modem_tx == 0);

    const auto idle_again =
        boot_v2::status_led_outputs(
            make_input(101, false, false, false, false));
    CHECK(idle_again.modem_tx == 1);
    CHECK(idle_again.red == 0);
    CHECK(idle_again.green == 1);
}

} // namespace

int main()
{
    test_ready_is_green_and_modem_tx_is_normally_on();
    test_booting_blinks_red_every_500ms();
    test_power_button_shutdown_blinks_until_power_off();
    test_adapter_grace_blinks_and_restore_returns_green();
    test_actual_modem_tx_pulse_turns_normally_on_led_off();
    if (failures != 0) {
        std::printf(
            "status_led_policy_test: %d/%d failed\n",
            failures,
            checks);
        return 1;
    }
    std::printf(
        "status_led_policy_test: %d checks passed\n",
        checks);
    return 0;
}
