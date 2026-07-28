#include "command_ack_core.hpp"

namespace boot_v2 {
namespace {

bool known_state(const CommandJournalState state) noexcept
{
    return state >= CommandJournalState::Empty &&
           state <= CommandJournalState::FinalReceipted;
}

bool known_opcode(const CommandOpcode opcode) noexcept
{
    return opcode >= CommandOpcode::None &&
           opcode <= CommandOpcode::FotaPrepare;
}

bool known_result(const CommandResult result) noexcept
{
    return result >= CommandResult::None &&
           result <= CommandResult::Expired;
}

bool known_error(const CommandError error) noexcept
{
    return error >= CommandError::None &&
           error <= CommandError::Expired;
}

bool known_expected_effect(
    const CommandExpectedEffect effect) noexcept
{
    return effect >= CommandExpectedEffect::None &&
           effect <= CommandExpectedEffect::PowerOff;
}

CommandExpectedEffect expected_effect_for_opcode(
    const CommandOpcode opcode) noexcept
{
    switch (opcode) {
    case CommandOpcode::None:
    case CommandOpcode::RequestStatus:
    case CommandOpcode::FotaPrepare:
        return CommandExpectedEffect::None;
    case CommandOpcode::Reboot:
        return CommandExpectedEffect::Reset;
    case CommandOpcode::PowerOff:
        return CommandExpectedEffect::PowerOff;
    }
    return CommandExpectedEffect::None;
}

CommandAckMessage accepted_ack(
    const CommandJournalRecord &record) noexcept
{
    return {
        record.cmd_id,
        CommandAckPhase::Accepted,
        CommandResult::Accepted,
        CommandError::None,
        0,
        0,
    };
}

CommandAckMessage final_ack(
    const CommandJournalRecord &record) noexcept
{
    return {
        record.cmd_id,
        CommandAckPhase::Final,
        record.result,
        record.error,
        0,
        0,
    };
}

bool receipt_matches(
    const CommandAckReceipt receipt,
    const CommandAckMessage expected) noexcept
{
    return receipt.cmd_id == expected.cmd_id &&
           receipt.phase == expected.phase &&
           receipt.result == expected.result;
}

} // namespace

bool command_journal_record_is_canonical(
    const CommandJournalRecord record) noexcept
{
    if (record.schema_version != 1 || !known_state(record.state) ||
        !known_opcode(record.opcode) || !known_result(record.result) ||
        !known_error(record.error) ||
        !known_expected_effect(record.expected_effect) ||
        record.ttl_checkpoint_clock_valid > 1 ||
        record.dispatch_latched > 1 || record.reserved != 0) {
        return false;
    }
    if (record.state == CommandJournalState::Empty) {
        return record.opcode == CommandOpcode::None &&
               record.phase == CommandAckPhase::Invalid &&
               record.result == CommandResult::None &&
               record.error == CommandError::None &&
               record.retry_count == 0 &&
               record.expected_effect == CommandExpectedEffect::None &&
               record.ttl_checkpoint_clock_valid == 0 &&
               record.dispatch_latched == 0 && record.cmd_id == 0 &&
               record.job_id == 0 && record.ttl_seconds == 0 &&
               record.remaining_ttl_seconds == 0 &&
               record.ttl_checkpoint_monotonic_seconds == 0 &&
               record.ttl_checkpoint_unix_seconds == 0 &&
               record.ttl_checkpoint_boot_sequence == 0 &&
               record.boot_sequence_before_execute == 0;
    }
    if (record.cmd_id == 0 || record.opcode == CommandOpcode::None ||
        record.ttl_seconds == 0 || record.ttl_seconds > 86400 ||
        record.remaining_ttl_seconds > record.ttl_seconds ||
        record.ttl_checkpoint_boot_sequence == 0 ||
        (record.ttl_checkpoint_clock_valid == 0 &&
         record.ttl_checkpoint_unix_seconds != 0) ||
        record.expected_effect !=
            expected_effect_for_opcode(record.opcode) ||
        record.boot_sequence_before_execute == 0) {
        return false;
    }
    if (record.state <= CommandJournalState::AcceptedReceipted) {
        return record.phase == CommandAckPhase::Accepted &&
               record.result == CommandResult::Accepted &&
               record.error == CommandError::None &&
               record.dispatch_latched == 0;
    }
    if (record.state == CommandJournalState::ExecuteMarked) {
        return record.phase == CommandAckPhase::Accepted &&
               record.result == CommandResult::Accepted &&
               record.error == CommandError::None &&
               record.dispatch_latched == 1;
    }
    return record.phase == CommandAckPhase::Final &&
           record.result >= CommandResult::Executed &&
           record.result <= CommandResult::Expired &&
           ((record.result == CommandResult::Executed &&
             record.error == CommandError::None) ||
            (record.result == CommandResult::Failed &&
             record.error >= CommandError::InvalidOpcode &&
             record.error <= CommandError::Journal) ||
            (record.result == CommandResult::Expired &&
             record.error == CommandError::Expired));
}

bool CommandAckCore::begin_poll(
    const std::uint32_t request_id,
    const std::uint32_t last_cmd_id) noexcept
{
    if (request_id == 0 || record_.state != CommandJournalState::Empty) {
        return false;
    }
    pending_request_id_ = request_id;
    last_completed_cmd_id_ = last_cmd_id;
    return true;
}

CommandAcceptResult CommandAckCore::accept_response(
    const CommandResponse response,
    const std::uint32_t received_at_monotonic_seconds,
    const std::uint32_t boot_sequence) noexcept
{
    if (pending_request_id_ == 0) {
        return CommandAcceptResult::RejectedNoPoll;
    }
    if (response.request_id != pending_request_id_) {
        return CommandAcceptResult::RejectedStaleRequest;
    }
    if (record_.state != CommandJournalState::Empty ||
        boot_sequence == 0) {
        return CommandAcceptResult::RejectedInvalid;
    }
    pending_request_id_ = 0;
    if (response.opcode == CommandOpcode::None) {
        return response.cmd_id == 0 && response.job_id == 0 &&
                       response.ttl_seconds == 0
                   ? CommandAcceptResult::NoCommand
                   : CommandAcceptResult::RejectedInvalid;
    }
    if (response.cmd_id == 0 || response.ttl_seconds == 0 ||
        response.ttl_seconds > 86400 ||
        response.opcode < CommandOpcode::Reboot ||
        response.opcode > CommandOpcode::FotaPrepare) {
        return CommandAcceptResult::RejectedInvalid;
    }

    record_ = {};
    record_.opcode = response.opcode;
    record_.cmd_id = response.cmd_id;
    record_.job_id = response.job_id;
    record_.ttl_seconds = response.ttl_seconds;
    record_.remaining_ttl_seconds = response.ttl_seconds;
    record_.ttl_checkpoint_monotonic_seconds =
        received_at_monotonic_seconds;
    record_.ttl_checkpoint_boot_sequence = boot_sequence;
    record_.boot_sequence_before_execute = boot_sequence;
    record_.expected_effect =
        expected_effect_for_opcode(response.opcode);
    record_.phase = CommandAckPhase::Accepted;

    if (response.cmd_id <= last_completed_cmd_id_) {
        record_.state = CommandJournalState::Executed;
        record_.phase = CommandAckPhase::Final;
        record_.result = CommandResult::Failed;
        record_.error = CommandError::Duplicate;
        return CommandAcceptResult::RejectedDuplicate;
    }

    record_.state = CommandJournalState::AcceptedPersisted;
    record_.result = CommandResult::Accepted;
    return CommandAcceptResult::Accepted;
}

CommandTransitionResult CommandAckCore::prepare_ack(
    CommandAckMessage &message) noexcept
{
    if (record_.state == CommandJournalState::AcceptedPersisted ||
        record_.state == CommandJournalState::AcceptedPublishPending ||
        record_.state == CommandJournalState::AcceptedPuback) {
        message = accepted_ack(record_);
        record_.state = CommandJournalState::AcceptedPublishPending;
        return CommandTransitionResult::Accepted;
    }
    if (record_.state == CommandJournalState::Executed ||
        record_.state == CommandJournalState::FinalPersisted ||
        record_.state == CommandJournalState::FinalPublishPending ||
        record_.state == CommandJournalState::FinalPuback) {
        record_.phase = CommandAckPhase::Final;
        if (record_.state == CommandJournalState::Executed) {
            record_.state = CommandJournalState::FinalPersisted;
        }
        message = final_ack(record_);
        record_.state = CommandJournalState::FinalPublishPending;
        return CommandTransitionResult::Accepted;
    }
    return CommandTransitionResult::RejectedState;
}

CommandTransitionResult CommandAckCore::record_puback(
    const CommandAckPhase phase) noexcept
{
    if (phase == CommandAckPhase::Accepted &&
        record_.state == CommandJournalState::AcceptedPublishPending) {
        record_.state = CommandJournalState::AcceptedPuback;
        return CommandTransitionResult::Accepted;
    }
    if (phase == CommandAckPhase::Final &&
        record_.state == CommandJournalState::FinalPublishPending) {
        record_.state = CommandJournalState::FinalPuback;
        return CommandTransitionResult::Accepted;
    }
    return CommandTransitionResult::RejectedState;
}

CommandTransitionResult CommandAckCore::record_receipt(
    const CommandAckReceipt receipt_value) noexcept
{
    CommandAckMessage expected{};
    if (record_.state == CommandJournalState::AcceptedPuback) {
        expected = accepted_ack(record_);
    } else if (record_.state == CommandJournalState::FinalPuback) {
        expected = final_ack(record_);
    } else {
        return CommandTransitionResult::RejectedState;
    }
    if (!receipt_matches(receipt_value, expected)) {
        return CommandTransitionResult::RejectedMismatch;
    }
    if (receipt_value.receipt == CommandAckReceiptCode::Rejected) {
        return CommandTransitionResult::RejectedRemote;
    }
    if (receipt_value.receipt != CommandAckReceiptCode::Ingested) {
        return CommandTransitionResult::RejectedMismatch;
    }
    record_.state =
        expected.phase == CommandAckPhase::Accepted
            ? CommandJournalState::AcceptedReceipted
            : CommandJournalState::FinalReceipted;
    return CommandTransitionResult::Accepted;
}

CommandExecutionDecision CommandAckCore::mark_execute(
    const std::uint32_t current_monotonic_seconds) noexcept
{
    if (record_.state != CommandJournalState::AcceptedReceipted) {
        return CommandExecutionDecision::Rejected;
    }
    const std::uint32_t elapsed =
        current_monotonic_seconds -
        record_.ttl_checkpoint_monotonic_seconds;
    if (elapsed >= record_.remaining_ttl_seconds) {
        record_.state = CommandJournalState::Executed;
        record_.phase = CommandAckPhase::Final;
        record_.result = CommandResult::Expired;
        record_.error = CommandError::Expired;
        return CommandExecutionDecision::Expired;
    }
    record_.state = CommandJournalState::ExecuteMarked;
    record_.dispatch_latched = 1;
    return CommandExecutionDecision::Dispatch;
}

CommandTransitionResult CommandAckCore::complete_execution(
    const bool succeeded,
    const CommandError error) noexcept
{
    if (record_.state != CommandJournalState::ExecuteMarked) {
        return CommandTransitionResult::RejectedState;
    }
    if ((succeeded && error != CommandError::None) ||
        (!succeeded &&
         (error < CommandError::InvalidOpcode ||
          error > CommandError::Journal))) {
        return CommandTransitionResult::RejectedInvalid;
    }
    record_.state = CommandJournalState::Executed;
    record_.phase = CommandAckPhase::Final;
    record_.result =
        succeeded ? CommandResult::Executed : CommandResult::Failed;
    record_.error = succeeded ? CommandError::None : error;
    return CommandTransitionResult::Accepted;
}

CommandTransitionResult CommandAckCore::clear_final_receipted() noexcept
{
    if (record_.state != CommandJournalState::FinalReceipted) {
        return CommandTransitionResult::RejectedState;
    }
    last_completed_cmd_id_ = record_.cmd_id;
    record_ = {};
    return CommandTransitionResult::Accepted;
}

CommandTransitionResult CommandAckCore::restore_after_boot(
    CommandJournalRecord record,
    const std::uint32_t current_boot_sequence,
    const bool effect_confirmed) noexcept
{
    if (!command_journal_record_is_canonical(record) ||
        record.state == CommandJournalState::Empty ||
        current_boot_sequence == 0) {
        return CommandTransitionResult::RejectedInvalid;
    }
    if (record.state == CommandJournalState::ExecuteMarked) {
        record.state = CommandJournalState::Executed;
        record.phase = CommandAckPhase::Final;
        record.result = effect_confirmed
                            ? CommandResult::Executed
                            : CommandResult::Failed;
        record.error = effect_confirmed
                           ? CommandError::None
                           : CommandError::Journal;
    } else if (record.state <= CommandJournalState::AcceptedReceipted &&
               current_boot_sequence !=
                   record.boot_sequence_before_execute) {
        record.state = CommandJournalState::Executed;
        record.phase = CommandAckPhase::Final;
        record.result = CommandResult::Expired;
        record.error = CommandError::Expired;
    }
    record_ = record;
    return CommandTransitionResult::Accepted;
}

bool CommandAckCore::synchronize_from_journal(
    const CommandJournalRecord record) noexcept
{
    if (!command_journal_record_is_canonical(record)) {
        return false;
    }
    pending_request_id_ = 0;
    record_ = record;
    return true;
}

CommandJournalState CommandAckCore::state() const noexcept
{
    return record_.state;
}

const CommandJournalRecord &CommandAckCore::record() const noexcept
{
    return record_;
}

std::uint32_t CommandAckCore::last_completed_cmd_id() const noexcept
{
    return last_completed_cmd_id_;
}

} // namespace boot_v2
