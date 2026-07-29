#include "temperature_alarm_publish_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

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

using boot_v2::TemperatureAlarmEdge;
using boot_v2::TemperatureAlarmPublishDecision;
using boot_v2::TemperatureAlarmPublishCore;
using boot_v2::TemperatureAlarmTerminalResult;

void check_idle(const TemperatureAlarmPublishDecision decision)
{
    CHECK(decision.snapshot_revision == 0);
    CHECK(decision.edge == TemperatureAlarmEdge::Invalid);
    CHECK(decision.publish_required == 0);
    CHECK(decision.value_deci_celsius == 0);
}

void check_publish(
    const TemperatureAlarmPublishDecision decision,
    const TemperatureAlarmEdge edge,
    const std::int16_t value_deci_celsius)
{
    CHECK(decision.snapshot_revision != 0);
    CHECK(decision.edge == edge);
    CHECK(decision.publish_required == 1);
    CHECK(decision.value_deci_celsius == value_deci_celsius);
}

void test_decision_abi_and_frozen_value_field()
{
    CHECK(sizeof(TemperatureAlarmPublishDecision) == 8);
    CHECK(alignof(TemperatureAlarmPublishDecision) == 4);
    CHECK(std::is_standard_layout<
          TemperatureAlarmPublishDecision>::value);
    CHECK(std::is_trivially_copyable<
          TemperatureAlarmPublishDecision>::value);
    CHECK(offsetof(TemperatureAlarmPublishDecision, snapshot_revision) == 0);
    CHECK(offsetof(TemperatureAlarmPublishDecision, edge) == 4);
    CHECK(offsetof(TemperatureAlarmPublishDecision, publish_required) == 5);
    CHECK(offsetof(
              TemperatureAlarmPublishDecision,
              value_deci_celsius) == 6);
    CHECK((std::is_same<
           decltype(TemperatureAlarmPublishDecision::value_deci_celsius),
           std::int16_t>::value));
}

void test_initial_baseline_and_stable_offer()
{
    TemperatureAlarmPublishCore core;

    check_idle(core.observe(false, true, 500));
    check_idle(core.observe(true, false, 200));

    const auto high = core.observe(true, true, 500);
    check_publish(high, TemperatureAlarmEdge::High, 500);

    const auto repeated = core.observe(true, true, 510);
    check_publish(repeated, TemperatureAlarmEdge::High, 500);
    CHECK(repeated.snapshot_revision == high.snapshot_revision);

    CHECK(!core.mark_enqueued(
        high.snapshot_revision + 1, TemperatureAlarmEdge::High));
    CHECK(!core.mark_enqueued(
        high.snapshot_revision, TemperatureAlarmEdge::Clear));
    CHECK(core.mark_enqueued(high.snapshot_revision, high.edge));
    CHECK(!core.mark_enqueued(high.snapshot_revision, high.edge));
}

void test_failed_high_retry_preserves_value_then_clear_captures_current()
{
    TemperatureAlarmPublishCore core;

    const auto high = core.observe(true, true, 500);
    check_publish(high, TemperatureAlarmEdge::High, 500);
    core.confirm_submitted();

    check_idle(core.observe(true, false, 200));
    CHECK(core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Failed));

    const auto high_retry = core.observe(true, false, 200);
    check_publish(high_retry, TemperatureAlarmEdge::High, 500);
    CHECK(high_retry.snapshot_revision != high.snapshot_revision);
    CHECK(core.mark_enqueued(
        high_retry.snapshot_revision, high_retry.edge));
    CHECK(core.apply_completion(
        high_retry.snapshot_revision,
        high_retry.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    const auto clear = core.observe(true, false, 200);
    check_publish(clear, TemperatureAlarmEdge::Clear, 200);
}

void test_only_exact_in_flight_success_confirms_delivery()
{
    TemperatureAlarmPublishCore core;

    const auto high = core.observe(true, true, 500);
    check_publish(high, TemperatureAlarmEdge::High, 500);
    CHECK(!core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Succeeded));
    CHECK(core.mark_enqueued(high.snapshot_revision, high.edge));

    CHECK(!core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Invalid));
    CHECK(!core.apply_completion(
        high.snapshot_revision + 1,
        high.edge,
        TemperatureAlarmTerminalResult::Succeeded));
    CHECK(!core.apply_completion(
        high.snapshot_revision,
        TemperatureAlarmEdge::Clear,
        TemperatureAlarmTerminalResult::Succeeded));

    check_idle(core.observe(true, false, 200));
    CHECK(core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Succeeded));
    CHECK(!core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    const auto clear = core.observe(true, false, 200);
    check_publish(clear, TemperatureAlarmEdge::Clear, 200);
    CHECK(clear.snapshot_revision != high.snapshot_revision);
    CHECK(core.mark_enqueued(clear.snapshot_revision, clear.edge));
    CHECK(core.apply_completion(
        clear.snapshot_revision,
        clear.edge,
        TemperatureAlarmTerminalResult::Succeeded));
    check_idle(core.observe(true, false, 200));
}

void check_terminal_result_is_retryable(
    const TemperatureAlarmTerminalResult result)
{
    TemperatureAlarmPublishCore core;

    const auto high = core.observe(true, true, 500);
    CHECK(core.mark_enqueued(high.snapshot_revision, high.edge));
    check_idle(core.observe(true, false, 200));

    CHECK(core.apply_completion(
        high.snapshot_revision, high.edge, result));

    const auto retry = core.observe(true, false, 200);
    check_publish(retry, TemperatureAlarmEdge::High, 500);
    CHECK(retry.snapshot_revision != high.snapshot_revision);

    CHECK(!core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Succeeded));
    CHECK(!core.apply_completion(
        retry.snapshot_revision,
        retry.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    CHECK(core.mark_enqueued(retry.snapshot_revision, retry.edge));
    CHECK(core.apply_completion(
        retry.snapshot_revision,
        retry.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    const auto clear = core.observe(true, false, 200);
    check_publish(clear, TemperatureAlarmEdge::Clear, 200);
}

void test_failure_timeout_and_cancel_are_retryable()
{
    check_terminal_result_is_retryable(
        TemperatureAlarmTerminalResult::Failed);
    check_terminal_result_is_retryable(
        TemperatureAlarmTerminalResult::TimedOut);
    check_terminal_result_is_retryable(
        TemperatureAlarmTerminalResult::Cancelled);
}

void test_failed_clear_retry_preserves_value_then_high_captures_current()
{
    TemperatureAlarmPublishCore core;

    const auto high = core.observe(true, true, 500);
    CHECK(core.mark_enqueued(high.snapshot_revision, high.edge));
    CHECK(core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    const auto clear = core.observe(true, false, 200);
    check_publish(clear, TemperatureAlarmEdge::Clear, 200);
    CHECK(core.mark_enqueued(clear.snapshot_revision, clear.edge));

    check_idle(core.observe(true, true, 550));
    CHECK(core.apply_completion(
        clear.snapshot_revision,
        clear.edge,
        TemperatureAlarmTerminalResult::Failed));

    const auto clear_retry = core.observe(true, true, 550);
    check_publish(clear_retry, TemperatureAlarmEdge::Clear, 200);
    CHECK(clear_retry.snapshot_revision != clear.snapshot_revision);
    CHECK(core.mark_enqueued(
        clear_retry.snapshot_revision, clear_retry.edge));
    CHECK(core.apply_completion(
        clear_retry.snapshot_revision,
        clear_retry.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    const auto next_high = core.observe(true, true, 550);
    check_publish(next_high, TemperatureAlarmEdge::High, 550);
}

void test_opposite_edges_remain_ordered()
{
    TemperatureAlarmPublishCore core;

    const auto high = core.observe(true, true, 500);
    const auto high_after_early_clear = core.observe(true, false, 200);
    check_publish(
        high_after_early_clear, TemperatureAlarmEdge::High, 500);
    CHECK(
        high_after_early_clear.snapshot_revision ==
        high.snapshot_revision);

    CHECK(core.mark_enqueued(high.snapshot_revision, high.edge));
    check_idle(core.observe(true, false, 200));
    CHECK(core.apply_completion(
        high.snapshot_revision,
        high.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    const auto clear = core.observe(true, false, 200);
    check_publish(clear, TemperatureAlarmEdge::Clear, 200);
    CHECK(core.mark_enqueued(clear.snapshot_revision, clear.edge));

    check_idle(core.observe(true, true, 550));
    CHECK(core.apply_completion(
        clear.snapshot_revision,
        clear.edge,
        TemperatureAlarmTerminalResult::Succeeded));

    const auto next_high = core.observe(true, true, 550);
    check_publish(next_high, TemperatureAlarmEdge::High, 550);
    CHECK(next_high.snapshot_revision != high.snapshot_revision);
    CHECK(next_high.snapshot_revision != clear.snapshot_revision);
}

} // namespace

int main()
{
    test_decision_abi_and_frozen_value_field();
    test_initial_baseline_and_stable_offer();
    test_failed_high_retry_preserves_value_then_clear_captures_current();
    test_only_exact_in_flight_success_confirms_delivery();
    test_failure_timeout_and_cancel_are_retryable();
    test_failed_clear_retry_preserves_value_then_high_captures_current();
    test_opposite_edges_remain_ordered();
    if (failures != 0) {
        std::printf("temperature_alarm_publish_core_test: %d/%d failed\n",
                    failures, checks);
        return 1;
    }
    std::printf("temperature_alarm_publish_core_test: %d checks passed\n",
                checks);
    return 0;
}
