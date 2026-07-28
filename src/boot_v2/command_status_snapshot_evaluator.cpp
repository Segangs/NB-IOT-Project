#include "command_status_snapshot_evaluator.hpp"

#include <cstdint>
#include <limits>

namespace boot_v2 {
namespace {

void increment_nonzero(std::uint32_t &value) noexcept
{
    if (value == std::numeric_limits<std::uint32_t>::max()) {
        value = 1;
        return;
    }
    ++value;
    if (value == 0) {
        value = 1;
    }
}

bool sample_flags_are_canonical(
    const CommandStatusSnapshotSample &sample) noexcept
{
    const bool known_command_result =
        sample.last_command_result >= RuntimeCommandResult::NoCommand &&
        sample.last_command_result <= RuntimeCommandResult::Rejected;
    const bool no_command =
        sample.last_command_result == RuntimeCommandResult::NoCommand;
    return known_command_result &&
           no_command == (sample.last_command_id == 0) &&
           sample.network_connected <= 1 &&
           sample.battery_mode <= 1 &&
           sample.alarm_active <= 1 &&
           sample.reserved == 0;
}

} // namespace

void RuntimeOwnerLastCommandState::reset_for_boot(
    const std::uint32_t command_id,
    const bool known_succeeded) noexcept
{
    value_ = {};
    if (command_id != 0 && known_succeeded) {
        value_.command_id = command_id;
        value_.result = RuntimeCommandResult::Succeeded;
    }
}

bool RuntimeOwnerLastCommandState::observe_terminal(
    const std::uint32_t command_id,
    const CommandResult result,
    const CommandError error) noexcept
{
    if (command_id == 0) {
        return false;
    }

    RuntimeCommandResult mapped = RuntimeCommandResult::Invalid;
    if (result == CommandResult::Executed &&
        error == CommandError::None) {
        mapped = RuntimeCommandResult::Succeeded;
    } else if (
        result == CommandResult::Failed &&
        (error == CommandError::Execution ||
         error == CommandError::Journal)) {
        mapped = RuntimeCommandResult::Failed;
    } else if (
        result == CommandResult::Failed &&
        (error == CommandError::InvalidOpcode ||
         error == CommandError::Duplicate)) {
        mapped = RuntimeCommandResult::Rejected;
    } else if (
        result == CommandResult::Expired &&
        error == CommandError::Expired) {
        mapped = RuntimeCommandResult::Rejected;
    } else {
        return false;
    }

    value_ = {command_id, mapped};
    return true;
}

RuntimeOwnerLastCommand
RuntimeOwnerLastCommandState::value() const noexcept
{
    return value_;
}

CommandStatusSnapshotEvaluator::CommandStatusSnapshotEvaluator(
    const CommandStatusSnapshotSourcePort source) noexcept
    : source_(source)
{
}

bool CommandStatusSnapshotEvaluator::validate_fresh(
    RuntimeStatusSnapshotV1 &output) noexcept
{
    if (source_.sample == nullptr) {
        return false;
    }
    CommandStatusSnapshotSample sample{};
    if (!source_.sample(source_.context, sample) ||
        !sample_flags_are_canonical(sample)) {
        return false;
    }

    increment_nonzero(revision_);
    RuntimeStatusSnapshotV1 snapshot{};
    snapshot.network_state =
        sample.network_connected != 0
            ? RuntimeNetworkState::Online
            : RuntimeNetworkState::Offline;
    snapshot.adapter_state =
        sample.battery_mode != 0
            ? RuntimeAdapterState::Absent
            : RuntimeAdapterState::Present;
    snapshot.power_state =
        sample.battery_mode != 0
            ? RuntimePowerState::Grace
            : RuntimePowerState::ExternalPower;
    snapshot.alarm_state =
        sample.alarm_active != 0
            ? RuntimeAlarmState::Active
            : RuntimeAlarmState::Clear;
    snapshot.ui_state =
        sample.owner.runtime_ready != 0
            ? RuntimeUiState::PeriodicReady
            : RuntimeUiState::Boot;
    snapshot.last_command_result = sample.last_command_result;
    snapshot.revision = revision_;
    snapshot.sensors = sample.sensors;
    snapshot.config_version =
        sample.config_version == 0 ? 1 : sample.config_version;
    snapshot.last_command_id = sample.last_command_id;
    snapshot.queue_summary =
        sample.metrics.control_processed_count +
        sample.metrics.normal_processed_count +
        sample.metrics.urgent_processed_count;
    snapshot.drop_summary =
        sample.metrics.dropped_invalid_count +
        sample.metrics.receive_fault_count;
    snapshot.recovery_summary =
        sample.owner.normal_cancelled_count +
        sample.owner.effect_cancelled_count;
    output = snapshot;

    const bool runtime_healthy =
        sample.owner.state == RuntimeOwnerTaskState::Active &&
        sample.owner.phase == RuntimeOwnerPhase::RuntimeReady &&
        sample.owner.runtime_ready == 1 &&
        sample.owner.shutdown_latched == 0 &&
        snapshot.network_state == RuntimeNetworkState::Online &&
        snapshot.power_state != RuntimePowerState::Off &&
        snapshot.ui_state == RuntimeUiState::PeriodicReady;
    return runtime_healthy &&
           runtime_status_snapshot_is_canonical(snapshot);
}

} // namespace boot_v2
