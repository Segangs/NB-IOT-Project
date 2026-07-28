#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "command_status_snapshot_evaluator.hpp"

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

SensorQualitySnapshotV1 sensor(
    const std::int16_t value,
    const std::uint32_t sampled_at) noexcept
{
    SensorQualitySnapshotV1 snapshot{};
    snapshot.health = SnapshotHealth::Pass;
    snapshot.has_value = 1;
    snapshot.value_source = SensorValueSource::Fresh;
    snapshot.clock_valid = 1;
    snapshot.value_deci_celsius = value;
    snapshot.last_valid_at_unix_seconds = sampled_at;
    return snapshot;
}

CommandStatusSnapshotSample healthy_sample() noexcept
{
    CommandStatusSnapshotSample sample{};
    sample.owner.state = RuntimeOwnerTaskState::Active;
    sample.owner.phase = RuntimeOwnerPhase::RuntimeReady;
    sample.owner.runtime_ready = 1;
    sample.owner.normal_cancelled_count = 13;
    sample.owner.effect_cancelled_count = 17;
    sample.metrics.urgent_processed_count = 2;
    sample.metrics.control_processed_count = 3;
    sample.metrics.normal_processed_count = 5;
    sample.metrics.dropped_invalid_count = 7;
    sample.metrics.receive_fault_count = 11;
    sample.sensors = {sensor(215, 1001), sensor(287, 1002)};
    sample.config_version = 19;
    sample.last_command_id = 23;
    sample.last_command_result = RuntimeCommandResult::Succeeded;
    sample.network_connected = 1;
    sample.alarm_active = 1;
    return sample;
}

struct FakeSampler {
    CommandStatusSnapshotSample sample{healthy_sample()};
    std::size_t calls{0};
    bool succeeds{true};
};

bool sample_status(
    void *const context,
    CommandStatusSnapshotSample &output) noexcept
{
    if (context == nullptr) {
        return false;
    }
    auto &sampler = *static_cast<FakeSampler *>(context);
    ++sampler.calls;
    if (!sampler.succeeds) {
        return false;
    }
    output = sampler.sample;
    return true;
}

void test_owner_metrics_and_sensor_mapping() noexcept
{
    FakeSampler sampler{};
    CommandStatusSnapshotEvaluator evaluator{
        {&sampler, sample_status}};
    RuntimeStatusSnapshotV1 snapshot{};

    CHECK(evaluator.validate_fresh(snapshot));
    CHECK(sampler.calls == 1);
    CHECK(snapshot.network_state == RuntimeNetworkState::Online);
    CHECK(snapshot.adapter_state == RuntimeAdapterState::Present);
    CHECK(snapshot.power_state == RuntimePowerState::ExternalPower);
    CHECK(snapshot.alarm_state == RuntimeAlarmState::Active);
    CHECK(snapshot.ui_state == RuntimeUiState::PeriodicReady);
    CHECK(snapshot.last_command_result ==
          RuntimeCommandResult::Succeeded);
    CHECK(snapshot.revision == 1);
    CHECK(snapshot.sensors[0].value_deci_celsius == 215);
    CHECK(snapshot.sensors[1].value_deci_celsius == 287);
    CHECK(snapshot.config_version == 19);
    CHECK(snapshot.last_command_id == 23);
    CHECK(snapshot.queue_summary == 10);
    CHECK(snapshot.drop_summary == 18);
    CHECK(snapshot.recovery_summary == 30);
}

void test_each_validation_takes_a_fresh_sample() noexcept
{
    FakeSampler sampler{};
    CommandStatusSnapshotEvaluator evaluator{
        {&sampler, sample_status}};
    RuntimeStatusSnapshotV1 first{};
    RuntimeStatusSnapshotV1 second{};

    CHECK(evaluator.validate_fresh(first));
    sampler.sample.sensors[0] = sensor(333, 2001);
    sampler.sample.metrics.normal_processed_count = 29;
    CHECK(evaluator.validate_fresh(second));

    CHECK(sampler.calls == 2);
    CHECK(first.revision == 1);
    CHECK(second.revision == 2);
    CHECK(first.sensors[0].value_deci_celsius == 215);
    CHECK(second.sensors[0].value_deci_celsius == 333);
    CHECK(first.queue_summary == 10);
    CHECK(second.queue_summary == 34);
}

void test_sampling_and_canonical_failure_fail_closed() noexcept
{
    {
        FakeSampler sampler{};
        sampler.succeeds = false;
        CommandStatusSnapshotEvaluator evaluator{
            {&sampler, sample_status}};
        RuntimeStatusSnapshotV1 snapshot{};
        CHECK(!evaluator.validate_fresh(snapshot));
        CHECK(sampler.calls == 1);
    }

    {
        FakeSampler sampler{};
        sampler.sample.sensors[1].reserved[0] = 1;
        CommandStatusSnapshotEvaluator evaluator{
            {&sampler, sample_status}};
        RuntimeStatusSnapshotV1 snapshot{};
        CHECK(!evaluator.validate_fresh(snapshot));
        CHECK(sampler.calls == 1);
    }

    {
        FakeSampler sampler{};
        sampler.sample.owner.phase = RuntimeOwnerPhase::RecoveryPending;
        sampler.sample.owner.runtime_ready = 0;
        CommandStatusSnapshotEvaluator evaluator{
            {&sampler, sample_status}};
        RuntimeStatusSnapshotV1 snapshot{};
        CHECK(!evaluator.validate_fresh(snapshot));
        CHECK(sampler.calls == 1);
    }

    {
        FakeSampler sampler{};
        sampler.sample.last_command_id = 0;
        sampler.sample.last_command_result =
            RuntimeCommandResult::Succeeded;
        CommandStatusSnapshotEvaluator evaluator{
            {&sampler, sample_status}};
        RuntimeStatusSnapshotV1 snapshot{};
        CHECK(!evaluator.validate_fresh(snapshot));
    }

    {
        FakeSampler sampler{};
        sampler.sample.last_command_id = 23;
        sampler.sample.last_command_result =
            RuntimeCommandResult::NoCommand;
        CommandStatusSnapshotEvaluator evaluator{
            {&sampler, sample_status}};
        RuntimeStatusSnapshotV1 snapshot{};
        CHECK(!evaluator.validate_fresh(snapshot));
    }
}

void test_runtime_owner_last_command_state_maps_explicit_results() noexcept
{
    {
        RuntimeOwnerLastCommandState state{};
        state.reset_for_boot(0, false);
        CHECK(state.value().command_id == 0);
        CHECK(state.value().result == RuntimeCommandResult::NoCommand);

        state.reset_for_boot(91, false);
        CHECK(state.value().command_id == 0);
        CHECK(state.value().result == RuntimeCommandResult::NoCommand);

        state.reset_for_boot(92, true);
        CHECK(state.value().command_id == 92);
        CHECK(state.value().result == RuntimeCommandResult::Succeeded);
    }

    struct Case {
        CommandResult command_result;
        CommandError command_error;
        RuntimeCommandResult expected;
    };
    constexpr Case cases[] = {
        {
            CommandResult::Executed,
            CommandError::None,
            RuntimeCommandResult::Succeeded,
        },
        {
            CommandResult::Failed,
            CommandError::Execution,
            RuntimeCommandResult::Failed,
        },
        {
            CommandResult::Failed,
            CommandError::Journal,
            RuntimeCommandResult::Failed,
        },
        {
            CommandResult::Failed,
            CommandError::InvalidOpcode,
            RuntimeCommandResult::Rejected,
        },
        {
            CommandResult::Failed,
            CommandError::Duplicate,
            RuntimeCommandResult::Rejected,
        },
        {
            CommandResult::Expired,
            CommandError::Expired,
            RuntimeCommandResult::Rejected,
        },
    };
    std::uint32_t command_id = 100;
    for (const Case &item : cases) {
        RuntimeOwnerLastCommandState state{};
        CHECK(state.observe_terminal(
            command_id,
            item.command_result,
            item.command_error));
        CHECK(state.value().command_id == command_id);
        CHECK(state.value().result == item.expected);
        ++command_id;
    }

    RuntimeOwnerLastCommandState state{};
    state.reset_for_boot(77, true);
    CHECK(!state.observe_terminal(
        78,
        CommandResult::Accepted,
        CommandError::None));
    CHECK(state.value().command_id == 77);
    CHECK(state.value().result == RuntimeCommandResult::Succeeded);
}

} // namespace

int main()
{
    test_owner_metrics_and_sensor_mapping();
    test_each_validation_takes_a_fresh_sample();
    test_sampling_and_canonical_failure_fail_closed();
    test_runtime_owner_last_command_state_maps_explicit_results();

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "command_status_snapshot_evaluator_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "command_status_snapshot_evaluator_test: %zu checks passed\n",
        g_checks);
    return 0;
}
