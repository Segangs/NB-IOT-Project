#include "command_runtime_coordinator.hpp"

#include <cstdint>
#include <limits>

namespace boot_v2 {
namespace {

constexpr std::uint32_t kCommandJournalCommitTimeoutMs = 2000;

bool records_equal(
    const CommandJournalRecord &left,
    const CommandJournalRecord &right) noexcept
{
    return left.schema_version == right.schema_version &&
           left.state == right.state &&
           left.opcode == right.opcode &&
           left.phase == right.phase &&
           left.result == right.result &&
           left.error == right.error &&
           left.retry_count == right.retry_count &&
           left.expected_effect == right.expected_effect &&
           left.ttl_checkpoint_clock_valid ==
               right.ttl_checkpoint_clock_valid &&
           left.dispatch_latched == right.dispatch_latched &&
           left.reserved == right.reserved &&
           left.cmd_id == right.cmd_id &&
           left.job_id == right.job_id &&
           left.ttl_seconds == right.ttl_seconds &&
           left.remaining_ttl_seconds ==
               right.remaining_ttl_seconds &&
           left.ttl_checkpoint_monotonic_seconds ==
               right.ttl_checkpoint_monotonic_seconds &&
           left.ttl_checkpoint_unix_seconds ==
               right.ttl_checkpoint_unix_seconds &&
           left.ttl_checkpoint_boot_sequence ==
               right.ttl_checkpoint_boot_sequence &&
           left.boot_sequence_before_execute ==
               right.boot_sequence_before_execute;
}

std::uint32_t next_boot_sequence(
    const CommandJournalRecord &loaded) noexcept
{
    if (loaded.state == CommandJournalState::Empty ||
        loaded.boot_sequence_before_execute == 0 ||
        loaded.boot_sequence_before_execute ==
            std::numeric_limits<std::uint32_t>::max()) {
        return 1;
    }
    return loaded.boot_sequence_before_execute + 1u;
}

} // namespace

CommandRuntimeCoordinator::CommandRuntimeCoordinator(
    const CommandJournalStorePort store) noexcept
    : store_(store)
{
}

CommandRuntimePrepareResult CommandRuntimeCoordinator::prepare(
    const CommandBootEffectEvidence &evidence) noexcept
{
    ready_ = 0;
    current_boot_sequence_ = 0;
    last_terminal_command_ = {};

    CommandJournalRecord loaded{};
    const CommandJournalStoreResult load =
        command_journal_store_load(store_, loaded);
    if (load.status == CommandJournalStoreStatus::Blank) {
        if (!core_.synchronize_from_journal({})) {
            return CommandRuntimePrepareResult::FailedClosed;
        }
        current_boot_sequence_ = 1;
        ready_ = 1;
        return CommandRuntimePrepareResult::ReadyBlank;
    }
    if (load.status != CommandJournalStoreStatus::Loaded ||
        !command_journal_record_is_canonical(loaded) ||
        !core_.synchronize_from_journal(loaded)) {
        return CommandRuntimePrepareResult::FailedClosed;
    }

    current_boot_sequence_ = next_boot_sequence(loaded);
    if (loaded.state == CommandJournalState::Empty) {
        ready_ = 1;
        return CommandRuntimePrepareResult::ReadyLoaded;
    }
    const bool effect_confirmed =
        loaded.state == CommandJournalState::ExecuteMarked &&
        command_boot_effect_matches(
            loaded, current_boot_sequence_, evidence);
    if (core_.restore_after_boot(
            loaded, current_boot_sequence_, effect_confirmed) !=
        CommandTransitionResult::Accepted) {
        return CommandRuntimePrepareResult::FailedClosed;
    }
    if (records_equal(loaded, core_.record())) {
        capture_terminal_command(core_.record());
        ready_ = 1;
        return CommandRuntimePrepareResult::ReadyLoaded;
    }

    const CommandJournalRecord recovered = core_.record();
    const CommandJournalStoreResult committed =
        command_journal_store_commit(
            store_, recovered, kCommandJournalCommitTimeoutMs);
    if (committed.status != CommandJournalStoreStatus::Committed) {
        (void)reload_after_failed_commit(loaded, recovered);
        ready_ = 0;
        return CommandRuntimePrepareResult::FailedClosed;
    }

    capture_terminal_command(recovered);
    ready_ = 1;
    return CommandRuntimePrepareResult::ReadyRecovered;
}

bool CommandRuntimeCoordinator::ready() const noexcept
{
    return ready_ != 0;
}

bool CommandRuntimeCoordinator::pending() const noexcept
{
    return ready_ != 0 &&
           core_.state() != CommandJournalState::Empty;
}

std::uint32_t
CommandRuntimeCoordinator::current_boot_sequence() const noexcept
{
    return current_boot_sequence_;
}

bool CommandRuntimeCoordinator::begin_poll(
    const std::uint32_t request_id,
    const std::uint32_t last_cmd_id) noexcept
{
    return ready_ != 0 &&
           core_.begin_poll(request_id, last_cmd_id);
}

CommandAcceptResult CommandRuntimeCoordinator::accept_response(
    const CommandResponse response,
    const std::uint32_t received_at_monotonic_seconds) noexcept
{
    if (ready_ == 0 || current_boot_sequence_ == 0) {
        return CommandAcceptResult::RejectedInvalid;
    }
    const CommandJournalRecord previous = core_.record();
    const CommandAcceptResult accepted = core_.accept_response(
        response,
        received_at_monotonic_seconds,
        current_boot_sequence_);
    if (accepted != CommandAcceptResult::Accepted &&
        accepted != CommandAcceptResult::RejectedDuplicate) {
        return accepted;
    }
    return persist_transition(previous)
               ? accepted
               : CommandAcceptResult::RejectedInvalid;
}

CommandTransitionResult CommandRuntimeCoordinator::prepare_ack(
    CommandAckMessage &message) noexcept
{
    if (ready_ == 0) {
        return CommandTransitionResult::RejectedState;
    }
    const CommandJournalRecord previous = core_.record();
    if (core_.prepare_ack(message) !=
        CommandTransitionResult::Accepted) {
        return CommandTransitionResult::RejectedState;
    }
    return persist_transition(previous)
               ? CommandTransitionResult::Accepted
               : CommandTransitionResult::RejectedState;
}

CommandTransitionResult CommandRuntimeCoordinator::record_puback(
    const CommandAckPhase phase) noexcept
{
    if (ready_ == 0) {
        return CommandTransitionResult::RejectedState;
    }
    const CommandJournalRecord previous = core_.record();
    if (core_.record_puback(phase) !=
        CommandTransitionResult::Accepted) {
        return CommandTransitionResult::RejectedState;
    }
    return persist_transition(previous)
               ? CommandTransitionResult::Accepted
               : CommandTransitionResult::RejectedState;
}

CommandTransitionResult CommandRuntimeCoordinator::record_receipt(
    const CommandAckReceipt receipt) noexcept
{
    if (ready_ == 0) {
        return CommandTransitionResult::RejectedState;
    }
    const CommandJournalRecord previous = core_.record();
    const CommandTransitionResult transition =
        core_.record_receipt(receipt);
    if (transition != CommandTransitionResult::Accepted) {
        return transition;
    }
    return persist_transition(previous)
               ? CommandTransitionResult::Accepted
               : CommandTransitionResult::RejectedState;
}

CommandRuntimeExecutionResult CommandRuntimeCoordinator::execute_pending(
    const std::uint32_t current_monotonic_seconds,
    const CommandStatusSnapshotPort status_snapshot,
    const CommandShutdownDispatchPort shutdown) noexcept
{
    if (ready_ == 0) {
        return CommandRuntimeExecutionResult::Rejected;
    }
    bool marked_this_call = false;
    if (core_.state() == CommandJournalState::AcceptedReceipted) {
        const CommandJournalRecord previous = core_.record();
        const CommandExecutionDecision decision =
            core_.mark_execute(current_monotonic_seconds);
        if (decision == CommandExecutionDecision::Rejected) {
            return CommandRuntimeExecutionResult::Rejected;
        }
        if (!persist_transition(previous)) {
            return CommandRuntimeExecutionResult::Deferred;
        }
        if (decision == CommandExecutionDecision::Expired) {
            return CommandRuntimeExecutionResult::Expired;
        }
        marked_this_call = true;
    }
    if (core_.state() != CommandJournalState::ExecuteMarked) {
        return CommandRuntimeExecutionResult::Rejected;
    }

    const CommandOpcode opcode = core_.record().opcode;
    if (!marked_this_call) {
        return CommandRuntimeExecutionResult::AwaitingBootEffect;
    }

    if (opcode == CommandOpcode::FotaPrepare) {
        const CommandJournalRecord previous = core_.record();
        if (core_.complete_execution(
                false, CommandError::InvalidOpcode) !=
            CommandTransitionResult::Accepted) {
            return CommandRuntimeExecutionResult::Rejected;
        }
        return persist_transition(previous)
                   ? CommandRuntimeExecutionResult::TerminalFailed
                   : CommandRuntimeExecutionResult::Deferred;
    }

    if (opcode == CommandOpcode::Reboot ||
        opcode == CommandOpcode::PowerOff) {
        if (shutdown.dispatch != nullptr &&
            shutdown.dispatch(
                shutdown.context,
                opcode,
                core_.record().cmd_id) ==
                CommandShutdownDispatchResult::Accepted) {
            return CommandRuntimeExecutionResult::ShutdownDispatched;
        }
        const CommandJournalRecord previous = core_.record();
        if (core_.complete_execution(
                false, CommandError::Execution) !=
            CommandTransitionResult::Accepted) {
            return CommandRuntimeExecutionResult::Rejected;
        }
        return persist_transition(previous)
                   ? CommandRuntimeExecutionResult::TerminalFailed
                   : CommandRuntimeExecutionResult::Deferred;
    }

    const bool request_status =
        opcode == CommandOpcode::RequestStatus;
    const bool status_succeeded =
        request_status && status_snapshot.validate_fresh != nullptr &&
        status_snapshot.validate_fresh(status_snapshot.context);
    const CommandJournalRecord previous = core_.record();
    if (core_.complete_execution(
            status_succeeded,
            status_succeeded ? CommandError::None
                             : CommandError::Execution) !=
        CommandTransitionResult::Accepted) {
        return CommandRuntimeExecutionResult::Rejected;
    }
    if (!persist_transition(previous)) {
        return CommandRuntimeExecutionResult::Deferred;
    }
    return status_succeeded
               ? CommandRuntimeExecutionResult::StatusSucceeded
               : CommandRuntimeExecutionResult::TerminalFailed;
}

CommandTransitionResult
CommandRuntimeCoordinator::clear_final_receipted() noexcept
{
    if (ready_ == 0) {
        return CommandTransitionResult::RejectedState;
    }
    const CommandJournalRecord previous = core_.record();
    if (core_.clear_final_receipted() !=
        CommandTransitionResult::Accepted) {
        return CommandTransitionResult::RejectedState;
    }
    return persist_transition(previous)
               ? CommandTransitionResult::Accepted
               : CommandTransitionResult::RejectedState;
}

CommandJournalState CommandRuntimeCoordinator::state() const noexcept
{
    return core_.state();
}

const CommandJournalRecord &
CommandRuntimeCoordinator::record() const noexcept
{
    return core_.record();
}

std::uint32_t
CommandRuntimeCoordinator::last_completed_cmd_id() const noexcept
{
    return core_.last_completed_cmd_id();
}

CommandRuntimeTerminalCommand
CommandRuntimeCoordinator::last_terminal_command() const noexcept
{
    return last_terminal_command_;
}

void CommandRuntimeCoordinator::capture_terminal_command(
    const CommandJournalRecord &record) noexcept
{
    if (record.state < CommandJournalState::Executed ||
        record.cmd_id == 0) {
        return;
    }
    const bool succeeded =
        record.result == CommandResult::Executed &&
        record.error == CommandError::None;
    const bool failed =
        record.result == CommandResult::Failed &&
        record.error >= CommandError::InvalidOpcode &&
        record.error <= CommandError::Journal;
    const bool expired =
        record.result == CommandResult::Expired &&
        record.error == CommandError::Expired;
    if (!succeeded && !failed && !expired) {
        return;
    }
    last_terminal_command_ = {
        record.cmd_id,
        record.result,
        record.error,
    };
}

bool CommandRuntimeCoordinator::persist_transition(
    const CommandJournalRecord previous) noexcept
{
    const CommandJournalRecord intended = core_.record();
    if (records_equal(previous, intended)) {
        capture_terminal_command(intended);
        return true;
    }
    const CommandJournalStoreResult committed =
        command_journal_store_commit(
            store_, intended, kCommandJournalCommitTimeoutMs);
    if (committed.status == CommandJournalStoreStatus::Committed) {
        if (intended.state == CommandJournalState::Empty) {
            capture_terminal_command(previous);
        } else {
            capture_terminal_command(intended);
        }
        return true;
    }
    (void)reload_after_failed_commit(previous, intended);
    return false;
}

bool CommandRuntimeCoordinator::reload_after_failed_commit(
    const CommandJournalRecord previous,
    const CommandJournalRecord intended) noexcept
{
    CommandJournalRecord loaded{};
    const CommandJournalStoreResult reloaded =
        command_journal_store_load(store_, loaded);
    if (reloaded.status == CommandJournalStoreStatus::Blank) {
        if (previous.state == CommandJournalState::Empty &&
            core_.synchronize_from_journal(previous)) {
            return true;
        }
        ready_ = 0;
        return false;
    }
    if (reloaded.status != CommandJournalStoreStatus::Loaded ||
        (!records_equal(loaded, intended) &&
         !records_equal(loaded, previous)) ||
        !core_.synchronize_from_journal(loaded)) {
        ready_ = 0;
        return false;
    }
    if (loaded.state == CommandJournalState::Empty) {
        capture_terminal_command(previous);
    } else {
        capture_terminal_command(loaded);
    }
    return true;
}

} // namespace boot_v2
