#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "command_periodic_schedule_core.hpp"

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(
    const bool condition,
    const char *const expression,
    const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(
            stderr,
            "CHECK failed: %s:%d: %s\n",
            __FILE__,
            line,
            expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

using namespace boot_v2;

struct FakeIngress {
    std::size_t config_calls{0};
    std::size_t command_calls{0};
    bool config_accepted{true};
    bool command_accepted{true};
};

bool pull_config(void *const context) noexcept
{
    if (context == nullptr) {
        return false;
    }
    auto &ingress = *static_cast<FakeIngress *>(context);
    ++ingress.config_calls;
    return ingress.config_accepted;
}

bool pull_command(void *const context) noexcept
{
    if (context == nullptr) {
        return false;
    }
    auto &ingress = *static_cast<FakeIngress *>(context);
    ++ingress.command_calls;
    return ingress.command_accepted;
}

void test_runtime_ready_and_recovery_gate_command_pull() noexcept
{
    CommandPeriodicScheduleInput input{};
    input.now_ms = COMMAND_PULL_INTERVAL_MS;
    input.last_config_ms = input.now_ms;
    input.last_command_ms = 0;

    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::Idle);

    input.runtime_ready = 1;
    input.recovery_pending = 1;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::Idle);

    input.recovery_pending = 0;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::PullCommand);
}

void test_command_is_due_only_at_the_twenty_minute_boundary() noexcept
{
    CommandPeriodicScheduleInput input{};
    input.runtime_ready = 1;
    input.last_command_ms = 0;

    input.now_ms = COMMAND_PULL_INTERVAL_MS - 1u;
    input.last_config_ms = input.now_ms;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::Idle);

    input.now_ms = COMMAND_PULL_INTERVAL_MS;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::PullCommand);
    CHECK(COMMAND_PULL_INTERVAL_MS == 20u * 60u * 1000u);
}

void test_config_is_due_only_at_the_twenty_minute_boundary() noexcept
{
    CommandPeriodicScheduleInput input{};
    input.runtime_ready = 1;
    input.last_config_ms = 0;
    input.last_command_ms = 0;

    input.now_ms = 60u * 1000u;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::Idle);

    input.now_ms = 20u * 60u * 1000u;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::PullConfig);
    CHECK(COMMAND_CONFIG_PULL_INTERVAL_MS ==
          20u * 60u * 1000u);
}

void test_config_has_priority_without_same_cycle_double_submit() noexcept
{
    CommandPeriodicScheduleInput input{};
    input.runtime_ready = 1;
    input.now_ms = COMMAND_PULL_INTERVAL_MS;
    input.last_config_ms = 0;
    input.last_command_ms = 0;

    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::PullConfig);

    input.last_config_ms = input.now_ms;
    ++input.now_ms;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::PullCommand);

    input.last_command_ms = input.now_ms;
    ++input.now_ms;
    CHECK(command_periodic_schedule(input) ==
          CommandPeriodicAction::Idle);
}

void test_stateful_step_config_then_command_callback_counts() noexcept
{
    FakeIngress ingress{};
    CommandPeriodicStepper stepper{0};
    const CommandPeriodicStepPort port{
        &ingress, pull_config, pull_command};

    CHECK(stepper.step(
              {COMMAND_PULL_INTERVAL_MS, 1, 0, {}}, port) ==
          CommandPeriodicStepResult::ConfigAccepted);
    CHECK(ingress.config_calls == 1);
    CHECK(ingress.command_calls == 0);

    CHECK(stepper.step(
              {COMMAND_PULL_INTERVAL_MS + 1u, 1, 0, {}}, port) ==
          CommandPeriodicStepResult::CommandAccepted);
    CHECK(ingress.config_calls == 1);
    CHECK(ingress.command_calls == 1);
}

void test_rejected_ingress_does_not_advance_due_timestamp() noexcept
{
    FakeIngress ingress{};
    ingress.config_accepted = false;
    CommandPeriodicStepper stepper{0};
    const CommandPeriodicStepPort port{
        &ingress, pull_config, pull_command};

    CHECK(stepper.step(
              {COMMAND_CONFIG_PULL_INTERVAL_MS, 1, 0, {}}, port) ==
          CommandPeriodicStepResult::ConfigRejected);
    CHECK(stepper.last_config_ms() == 0);
    CHECK(stepper.step(
              {COMMAND_CONFIG_PULL_INTERVAL_MS + 1u, 1, 0, {}}, port) ==
          CommandPeriodicStepResult::ConfigRejected);
    CHECK(ingress.config_calls == 2);
    CHECK(ingress.command_calls == 0);
}

void test_accepted_command_cannot_repeat_before_twenty_minutes() noexcept
{
    FakeIngress ingress{};
    CommandPeriodicStepper stepper{0};
    const CommandPeriodicStepPort port{
        &ingress, pull_config, pull_command};

    CHECK(stepper.step(
              {COMMAND_PULL_INTERVAL_MS, 1, 0, {}}, port) ==
          CommandPeriodicStepResult::ConfigAccepted);
    CHECK(stepper.step(
              {COMMAND_PULL_INTERVAL_MS + 1u, 1, 0, {}}, port) ==
          CommandPeriodicStepResult::CommandAccepted);
    const std::uint32_t accepted_at = stepper.last_command_ms();

    CHECK(stepper.step(
              {accepted_at + COMMAND_PULL_INTERVAL_MS - 1u,
               1,
               0,
               {}},
              port) != CommandPeriodicStepResult::CommandAccepted);
    CHECK(ingress.command_calls == 1);
}

void test_runtime_and_recovery_gates_invoke_no_callback() noexcept
{
    FakeIngress ingress{};
    CommandPeriodicStepper stepper{0};
    const CommandPeriodicStepPort port{
        &ingress, pull_config, pull_command};

    CHECK(stepper.step(
              {COMMAND_PULL_INTERVAL_MS, 0, 0, {}}, port) ==
          CommandPeriodicStepResult::Idle);
    CHECK(stepper.step(
              {COMMAND_PULL_INTERVAL_MS, 1, 1, {}}, port) ==
          CommandPeriodicStepResult::Idle);
    CHECK(ingress.config_calls == 0);
    CHECK(ingress.command_calls == 0);
}

} // namespace

int main()
{
    test_runtime_ready_and_recovery_gate_command_pull();
    test_command_is_due_only_at_the_twenty_minute_boundary();
    test_config_is_due_only_at_the_twenty_minute_boundary();
    test_config_has_priority_without_same_cycle_double_submit();
    test_stateful_step_config_then_command_callback_counts();
    test_rejected_ingress_does_not_advance_due_timestamp();
    test_accepted_command_cannot_repeat_before_twenty_minutes();
    test_runtime_and_recovery_gates_invoke_no_callback();

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "command_periodic_schedule_core_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "command_periodic_schedule_core_test: %zu checks passed\n",
        g_checks);
    return 0;
}
