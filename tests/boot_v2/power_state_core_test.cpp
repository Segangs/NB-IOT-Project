#include "power_state_core.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>

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

using boot_v2::PowerStateAction;
using boot_v2::PowerStateCore;
using boot_v2::PowerStateKind;
using boot_v2::power_state_action_submit_allowed;

void test_adapter_loss_requires_full_debounce()
{
    PowerStateCore core;

    CHECK(core.observe(true, 100).action == PowerStateAction::None);
    CHECK(core.observe(false, 200).action == PowerStateAction::None);
    CHECK(core.observe(false, 1199).action == PowerStateAction::None);

    const auto opened = core.observe(false, 1200);
    CHECK(opened.action == PowerStateAction::PublishAdapterRemoved);
    CHECK(opened.state == PowerStateKind::Grace);
    CHECK(opened.incident_id != 0);
    CHECK(opened.sequence == 1);
    CHECK(opened.elapsed_seconds == 0);
    CHECK(opened.remaining_seconds == 300);
    CHECK(opened.battery_grace_active == 1);
    CHECK(opened.shutdown_committed == 0);
}

void test_low_bounce_does_not_open_incident()
{
    PowerStateCore core;

    CHECK(core.observe(false, 10).action == PowerStateAction::None);
    CHECK(core.observe(true, 1009).action == PowerStateAction::None);
    CHECK(core.observe(false, 2000).action == PowerStateAction::None);
    CHECK(core.observe(false, 2999).action == PowerStateAction::None);
    CHECK(core.observe(true, 3000).state == PowerStateKind::ExternalPower);
}

void test_unacknowledged_removal_retries_and_ack_stops_duplicate()
{
    PowerStateCore core;

    CHECK(core.observe(false, 100).action == PowerStateAction::None);
    const auto first = core.observe(false, 1100);
    CHECK(first.action == PowerStateAction::PublishAdapterRemoved);
    CHECK(core.observe(false, 1200).action ==
          PowerStateAction::PublishAdapterRemoved);
    CHECK(core.acknowledge(PowerStateAction::PublishAdapterRemoved));
    CHECK(core.observe(false, 1300).action == PowerStateAction::None);
    CHECK(!core.acknowledge(PowerStateAction::PublishAdapterRemoved));
}

void test_restore_before_commit_cancels_with_same_incident()
{
    PowerStateCore core;

    (void)core.observe(false, 100);
    const auto opened = core.observe(false, 1100);
    const std::uint32_t incident = opened.incident_id;
    CHECK(core.acknowledge(PowerStateAction::PublishAdapterRemoved));

    CHECK(core.observe(true, 200000).action == PowerStateAction::None);
    CHECK(core.observe(true, 200999).action == PowerStateAction::None);
    const auto restored = core.observe(true, 201000);
    CHECK(restored.action == PowerStateAction::PublishAdapterRestored);
    CHECK(restored.state == PowerStateKind::ExternalPower);
    CHECK(restored.incident_id == incident);
    CHECK(restored.sequence == 2);
    CHECK(restored.battery_grace_active == 0);
    CHECK(restored.shutdown_committed == 0);
    CHECK(core.acknowledge(PowerStateAction::PublishAdapterRestored));
    CHECK(core.observe(true, 201001).action == PowerStateAction::None);
}

void test_commit_is_exactly_210_seconds_and_never_cancels()
{
    PowerStateCore core;

    (void)core.observe(false, 100);
    const auto opened = core.observe(false, 1100);
    const std::uint32_t incident = opened.incident_id;
    CHECK(core.acknowledge(PowerStateAction::PublishAdapterRemoved));

    CHECK(core.observe(false, 211099).action == PowerStateAction::None);
    const auto committed = core.observe(false, 211100);
    CHECK(committed.action == PowerStateAction::CommitShutdown);
    CHECK(committed.state == PowerStateKind::Committed);
    CHECK(committed.incident_id == incident);
    CHECK(committed.sequence == 2);
    CHECK(committed.elapsed_seconds == 210);
    CHECK(committed.remaining_seconds == 90);
    CHECK(committed.shutdown_committed == 1);
    CHECK(core.observe(true, 212100).action ==
          PowerStateAction::CommitShutdown);
    CHECK(core.acknowledge(PowerStateAction::CommitShutdown));
    CHECK(core.observe(true, 213100).state == PowerStateKind::Committed);
    CHECK(core.observe(true, 213100).action == PowerStateAction::None);
}

void test_unsigned_tick_wrap_preserves_boundaries()
{
    PowerStateCore core;
    constexpr std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    const std::uint32_t loss_started = maximum - 499;

    CHECK(core.observe(false, loss_started).action == PowerStateAction::None);
    CHECK(core.observe(false, 499).action == PowerStateAction::None);
    const auto opened = core.observe(false, 500);
    CHECK(opened.action == PowerStateAction::PublishAdapterRemoved);
    CHECK(core.acknowledge(PowerStateAction::PublishAdapterRemoved));

    CHECK(core.observe(false, 210499).action == PowerStateAction::None);
    const auto committed = core.observe(false, 210500);
    CHECK(committed.action == PowerStateAction::CommitShutdown);
    CHECK(committed.elapsed_seconds == 210);
    CHECK(committed.remaining_seconds == 90);
}

void test_publish_waits_for_runtime_but_shutdown_does_not()
{
    CHECK(!power_state_action_submit_allowed(
        PowerStateAction::None, false));
    CHECK(!power_state_action_submit_allowed(
        PowerStateAction::PublishAdapterRemoved, false));
    CHECK(!power_state_action_submit_allowed(
        PowerStateAction::PublishAdapterRestored, false));
    CHECK(power_state_action_submit_allowed(
        PowerStateAction::PublishAdapterRemoved, true));
    CHECK(power_state_action_submit_allowed(
        PowerStateAction::PublishAdapterRestored, true));
    CHECK(power_state_action_submit_allowed(
        PowerStateAction::CommitShutdown, false));
}

} // namespace

int main()
{
    test_adapter_loss_requires_full_debounce();
    test_low_bounce_does_not_open_incident();
    test_unacknowledged_removal_retries_and_ack_stops_duplicate();
    test_restore_before_commit_cancels_with_same_incident();
    test_commit_is_exactly_210_seconds_and_never_cancels();
    test_unsigned_tick_wrap_preserves_boundaries();
    test_publish_waits_for_runtime_but_shutdown_does_not();
    if (failures != 0) {
        std::printf("power_state_core_test: %d/%d failed\n",
                    failures, checks);
        return 1;
    }
    std::printf("power_state_core_test: %d checks passed\n", checks);
    return 0;
}
