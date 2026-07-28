#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../../src/boot_v2/command_ack_core.hpp"
#include "../../src/boot_v2/mqtt_command_codec.hpp"

namespace {

std::uint64_t checks = 0;

#define CHECK(expression)                                                       \
    do {                                                                        \
        ++checks;                                                               \
        if (!(expression)) {                                                    \
            std::cerr << "CHECK failed: " #expression << " at " << __FILE__   \
                      << ':' << __LINE__ << '\n';                                \
            std::exit(1);                                                       \
        }                                                                       \
    } while (false)

using boot_v2::CommandAcceptResult;
using boot_v2::CommandAckCore;
using boot_v2::CommandAckMessage;
using boot_v2::CommandAckPhase;
using boot_v2::CommandAckReceipt;
using boot_v2::CommandAckReceiptCode;
using boot_v2::CommandError;
using boot_v2::CommandExpectedEffect;
using boot_v2::CommandExecutionDecision;
using boot_v2::CommandJournalRecord;
using boot_v2::CommandJournalState;
using boot_v2::CommandOpcode;
using boot_v2::CommandResponse;
using boot_v2::CommandResult;
using boot_v2::CommandTransitionResult;

CommandResponse command(
    const std::uint32_t request_id,
    const std::uint32_t cmd_id,
    const CommandOpcode opcode = CommandOpcode::RequestStatus,
    const std::uint32_t ttl_seconds = 60) noexcept
{
    return {request_id, cmd_id, opcode, 0, ttl_seconds};
}

CommandAckReceipt receipt(
    const CommandAckMessage ack,
    const CommandAckReceiptCode code =
        CommandAckReceiptCode::Ingested,
    const std::uint8_t error = 0) noexcept
{
    return {
        ack.cmd_id,
        ack.phase,
        ack.result,
        code,
        error,
    };
}

void check_codec_round_trip_and_exact_shape()
{
    char payload[81]{};
    CHECK(boot_v2::mqtt_command_request_build(
        7, 3, payload, sizeof(payload)));
    CHECK(std::strcmp(payload, "[7,3]") == 0);

    CommandResponse parsed{};
    CHECK(boot_v2::mqtt_command_response_parse(
        "[7,9,3,0,60]", 7, parsed));
    CHECK(parsed.request_id == 7);
    CHECK(parsed.cmd_id == 9);
    CHECK(parsed.opcode == CommandOpcode::RequestStatus);
    CHECK(parsed.job_id == 0);
    CHECK(parsed.ttl_seconds == 60);

    CHECK(boot_v2::mqtt_command_ack_build(
        {9, CommandAckPhase::Accepted, CommandResult::Accepted,
         CommandError::None, 0, 0},
        payload,
        sizeof(payload)));
    CHECK(std::strcmp(payload, "[9,1,1,0,0,0]") == 0);

    CommandAckReceipt parsed_receipt{};
    CHECK(boot_v2::mqtt_command_ack_receipt_parse(
        "[9,1,1,1,0]", parsed_receipt));
    CHECK(parsed_receipt.cmd_id == 9);
    CHECK(parsed_receipt.phase == CommandAckPhase::Accepted);
    CHECK(parsed_receipt.result == CommandResult::Accepted);
    CHECK(parsed_receipt.receipt ==
          CommandAckReceiptCode::Ingested);
    CHECK(parsed_receipt.error == 0);

    const char *const frame =
        "\r\n+KMQTT_DATA: 1,\"devices/123/config\",\"[-7,-10]\"\r\n"
        "+KMQTT_DATA: 1,\"devices/123/cmd/response\","
        "\"[7,9,3,0,60]\"\r\n";
    std::size_t frame_bytes = 0;
    std::size_t payload_bytes = 0;
    CHECK(boot_v2::mqtt_command_topic_payload_extract(
        frame,
        "devices/123/cmd/response",
        payload,
        sizeof(payload),
        &frame_bytes,
        &payload_bytes));
    CHECK(std::strcmp(payload, "[7,9,3,0,60]") == 0);
    CHECK(payload_bytes == std::strlen(payload));
    CHECK(frame_bytes > payload_bytes);
    CHECK(!boot_v2::mqtt_command_topic_payload_extract(
        "+KMQTT_DATA: 1,\"devices/123/cmd/response\","
        "\"[7,9,3,0,60]\"",
        "devices/123/cmd/response",
        payload,
        sizeof(payload),
        nullptr,
        nullptr));
    CHECK(!boot_v2::mqtt_command_topic_payload_extract(
        frame,
        "devices/123/cmd/ack/receipt",
        payload,
        sizeof(payload),
        nullptr,
        nullptr));

    const char *const repeated_topic_frames =
        "+KMQTT_DATA: 1,\"devices/123/cmd/response\","
        "\"[6,8,3,0,60]\"\r\n"
        "+KMQTT_DATA: 1,\"devices/123/cmd/response\","
        "\"[7,9,3,0,60]\"\r\n";
    CHECK(boot_v2::mqtt_command_topic_payload_extract(
        repeated_topic_frames,
        "devices/123/cmd/response",
        payload,
        sizeof(payload),
        nullptr,
        nullptr));
    CHECK(std::strcmp(payload, "[7,9,3,0,60]") == 0);
}

void check_codec_rejects_malformed_or_relationally_invalid_payloads()
{
    const char *const invalid_responses[] = {
        "", "[]", "[1,2,3,4]", "[1,2,3,4,5,6]",
        "[1,2,3,4,-1]", "[1,2,3,4,01]", "[1,2,3,4, 5]",
        "[1,2,\"3\",4,5]", "[1,2,9,4,5]", "[1,0,3,0,60]",
        "[1,2,0,0,60]", "[1,2,3,0,0]", "[2,2,3,0,60]",
        "[4294967296,2,3,0,60]", "[1,2,3,0,86401]",
    };
    CommandResponse output{99, 99, CommandOpcode::PowerOff, 99, 99};
    for (const char *payload : invalid_responses) {
        CHECK(!boot_v2::mqtt_command_response_parse(payload, 1, output));
        CHECK(output.request_id == 99);
    }

    CHECK(boot_v2::mqtt_command_response_parse(
        "[1,0,0,0,0]", 1, output));
    CHECK(output.opcode == CommandOpcode::None);

    const char *const invalid_receipts[] = {
        "", "[]", "[1,1,1,1]", "[1,1,1,1,0,0]",
        "[1,1,1,0,0]", "[1,1,1,1,1]",
        "[1,1,1,2,0]", "[1,1,1,3,0]",
        "[1,2,1,1,0]", "[1,1,2,1,0]",
    };
    CommandAckReceipt receipt_output{};
    for (const char *payload : invalid_receipts) {
        CHECK(!boot_v2::mqtt_command_ack_receipt_parse(
            payload, receipt_output));
    }
}

void check_codec_accepts_only_canonical_receipt_error_pairs()
{
    CommandAckReceipt output{};
    CHECK(boot_v2::mqtt_command_ack_receipt_parse(
        "[9,1,1,1,0]", output));
    CHECK(output.receipt == CommandAckReceiptCode::Ingested);
    CHECK(output.error == 0);
    CHECK(boot_v2::mqtt_command_ack_receipt_parse(
        "[9,1,1,2,1]", output));
    CHECK(boot_v2::mqtt_command_ack_receipt_parse(
        "[9,1,1,2,2]", output));
    CHECK(output.receipt == CommandAckReceiptCode::Rejected);
    CHECK(output.error == 2);
    CHECK(boot_v2::mqtt_command_ack_receipt_parse(
        "[9,1,1,3,3]", output));
    CHECK(boot_v2::mqtt_command_ack_receipt_parse(
        "[9,1,1,3,4]", output));
    CHECK(!boot_v2::mqtt_command_ack_receipt_parse(
        "[9,1,1,3,2]", output));

    const char *const invalid_receipt_error_pairs[] = {
        "[9,1,1,1,1]",
        "[9,1,1,1,2]",
        "[9,1,1,1,3]",
        "[9,1,1,1,4]",
        "[9,1,1,1,5]",
        "[9,1,1,2,0]",
        "[9,1,1,2,3]",
        "[9,1,1,2,4]",
        "[9,1,1,2,5]",
        "[9,1,1,3,0]",
        "[9,1,1,3,1]",
        "[9,1,1,3,5]",
    };
    for (const char *payload : invalid_receipt_error_pairs) {
        CHECK(!boot_v2::mqtt_command_ack_receipt_parse(payload, output));
    }
}

void advance_accepted_receipt(
    CommandAckCore &core,
    CommandAckMessage &accepted) noexcept
{
    CHECK(core.prepare_ack(accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(accepted.phase == CommandAckPhase::Accepted);
    CHECK(core.record_puback(CommandAckPhase::Accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(core.record_receipt(receipt(accepted)) ==
          CommandTransitionResult::Accepted);
}

void advance_final_receipt(
    CommandAckCore &core,
    CommandAckMessage &final_ack) noexcept
{
    CHECK(core.prepare_ack(final_ack) ==
          CommandTransitionResult::Accepted);
    CHECK(final_ack.phase == CommandAckPhase::Final);
    CHECK(core.record_puback(CommandAckPhase::Final) ==
          CommandTransitionResult::Accepted);
    CHECK(core.record_receipt(receipt(final_ack)) ==
          CommandTransitionResult::Accepted);
    CHECK(core.state() == CommandJournalState::FinalReceipted);
}

void check_normal_command_flow_and_exact_receipts()
{
    CommandAckCore core;
    CHECK(core.begin_poll(11, 0));
    CHECK(core.accept_response(command(11, 41), 100, 7) ==
          CommandAcceptResult::Accepted);
    CHECK(core.state() == CommandJournalState::AcceptedPersisted);
    CHECK(core.record().retry_count == 0);
    CHECK(core.record().remaining_ttl_seconds == 60);
    CHECK(core.record().ttl_checkpoint_monotonic_seconds == 100);
    CHECK(core.record().ttl_checkpoint_unix_seconds == 0);
    CHECK(core.record().ttl_checkpoint_clock_valid == 0);
    CHECK(core.record().ttl_checkpoint_boot_sequence == 7);
    CHECK(core.record().boot_sequence_before_execute == 7);
    CHECK(core.record().expected_effect ==
          CommandExpectedEffect::None);

    CommandAckMessage accepted{};
    advance_accepted_receipt(core, accepted);
    CHECK(core.state() == CommandJournalState::AcceptedReceipted);
    CHECK(core.mark_execute(101) ==
          CommandExecutionDecision::Dispatch);
    CHECK(core.state() == CommandJournalState::ExecuteMarked);
    CHECK(core.record().dispatch_latched == 1);
    CHECK(core.complete_execution(true, CommandError::None) ==
          CommandTransitionResult::Accepted);

    CommandAckMessage final_ack{};
    advance_final_receipt(core, final_ack);
    CHECK(final_ack.result == CommandResult::Executed);
    CHECK(final_ack.error == CommandError::None);
    CHECK(core.clear_final_receipted() ==
          CommandTransitionResult::Accepted);
    CHECK(core.state() == CommandJournalState::Empty);
    CHECK(core.last_completed_cmd_id() == 41);
}

void check_expected_effect_and_ttl_checkpoint_contract()
{
    struct EffectCase {
        CommandOpcode opcode;
        CommandExpectedEffect effect;
    };
    const EffectCase cases[] = {
        {CommandOpcode::Reboot, CommandExpectedEffect::Reset},
        {CommandOpcode::PowerOff, CommandExpectedEffect::PowerOff},
        {CommandOpcode::RequestStatus, CommandExpectedEffect::None},
        {CommandOpcode::FotaPrepare, CommandExpectedEffect::None},
    };
    std::uint32_t request_id = 100;
    for (const EffectCase &entry : cases) {
        CommandAckCore core;
        CHECK(core.begin_poll(request_id, 0));
        CHECK(core.accept_response(
                  command(request_id, request_id, entry.opcode, 60),
                  321,
                  9) == CommandAcceptResult::Accepted);
        CHECK(core.record().expected_effect == entry.effect);
        CHECK(core.record().remaining_ttl_seconds == 60);
        CHECK(core.record().ttl_checkpoint_monotonic_seconds == 321);
        CHECK(core.record().ttl_checkpoint_unix_seconds == 0);
        CHECK(core.record().ttl_checkpoint_clock_valid == 0);
        CHECK(core.record().ttl_checkpoint_boot_sequence == 9);
        ++request_id;
    }

    CommandAckCore core;
    CHECK(core.begin_poll(200, 0));
    CHECK(core.accept_response(
              command(200, 200, CommandOpcode::Reboot, 60),
              500,
              10) == CommandAcceptResult::Accepted);
    const CommandJournalRecord valid = core.record();
    CHECK(boot_v2::command_journal_record_is_canonical(valid));

    CommandJournalRecord invalid = valid;
    invalid.remaining_ttl_seconds = invalid.ttl_seconds + 1;
    CHECK(!boot_v2::command_journal_record_is_canonical(invalid));

    invalid = valid;
    invalid.ttl_checkpoint_unix_seconds = 1;
    invalid.ttl_checkpoint_clock_valid = 0;
    CHECK(!boot_v2::command_journal_record_is_canonical(invalid));

    invalid = valid;
    invalid.ttl_checkpoint_boot_sequence = 0;
    CHECK(!boot_v2::command_journal_record_is_canonical(invalid));

    invalid = valid;
    invalid.expected_effect = CommandExpectedEffect::PowerOff;
    CHECK(!boot_v2::command_journal_record_is_canonical(invalid));
}

void check_duplicate_stale_and_no_command_paths()
{
    CommandAckCore core;
    CHECK(core.begin_poll(1, 8));
    CHECK(core.accept_response(command(2, 9), 0, 1) ==
          CommandAcceptResult::RejectedStaleRequest);
    CHECK(core.state() == CommandJournalState::Empty);
    CHECK(core.accept_response(
              {1, 0, CommandOpcode::None, 0, 0}, 0, 1) ==
          CommandAcceptResult::NoCommand);
    CHECK(core.state() == CommandJournalState::Empty);

    CHECK(core.begin_poll(3, 8));
    CHECK(core.accept_response(command(3, 8), 0, 1) ==
          CommandAcceptResult::RejectedDuplicate);
    CHECK(core.state() == CommandJournalState::Executed);
    CHECK(core.record().result == CommandResult::Failed);
    CHECK(core.record().error == CommandError::Duplicate);
    CHECK(core.record().dispatch_latched == 0);
}

void check_ttl_expiry_never_dispatches()
{
    CommandAckCore core;
    CHECK(core.begin_poll(9, 0));
    CHECK(core.accept_response(command(9, 22, CommandOpcode::PowerOff, 2),
                               100, 4) ==
          CommandAcceptResult::Accepted);
    CommandAckMessage accepted{};
    advance_accepted_receipt(core, accepted);
    CHECK(core.mark_execute(102) ==
          CommandExecutionDecision::Expired);
    CHECK(core.state() == CommandJournalState::Executed);
    CHECK(core.record().result == CommandResult::Expired);
    CHECK(core.record().error == CommandError::Expired);
    CHECK(core.record().dispatch_latched == 0);
}

void check_mismatch_and_timeout_keep_journal()
{
    CommandAckCore core;
    CHECK(core.begin_poll(10, 0));
    CHECK(core.accept_response(command(10, 55), 0, 1) ==
          CommandAcceptResult::Accepted);
    CommandAckMessage accepted{};
    CHECK(core.prepare_ack(accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(core.record_puback(CommandAckPhase::Accepted) ==
          CommandTransitionResult::Accepted);

    CommandAckReceipt mismatch = receipt(accepted);
    mismatch.cmd_id = 56;
    CHECK(core.record_receipt(mismatch) ==
          CommandTransitionResult::RejectedMismatch);
    CHECK(core.state() == CommandJournalState::AcceptedPuback);
    CHECK(core.prepare_ack(accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(core.state() == CommandJournalState::AcceptedPublishPending);
    CHECK(core.record_puback(CommandAckPhase::Accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(core.record_receipt(
              receipt(accepted, CommandAckReceiptCode::Rejected, 1)) ==
          CommandTransitionResult::RejectedRemote);
    CHECK(core.state() == CommandJournalState::AcceptedPuback);
}

void check_boot_recovery_never_reexecutes_execute_marked()
{
    CommandAckCore original;
    CHECK(original.begin_poll(4, 0));
    CHECK(original.accept_response(
              command(4, 99, CommandOpcode::PowerOff), 10, 2) ==
          CommandAcceptResult::Accepted);
    CommandAckMessage accepted{};
    advance_accepted_receipt(original, accepted);
    CHECK(original.mark_execute(11) ==
          CommandExecutionDecision::Dispatch);

    CommandAckCore recovered;
    CHECK(recovered.restore_after_boot(original.record(), 3, false) ==
          CommandTransitionResult::Accepted);
    CHECK(recovered.state() == CommandJournalState::Executed);
    CHECK(recovered.record().result == CommandResult::Failed);
    CHECK(recovered.record().error == CommandError::Journal);
    CHECK(recovered.mark_execute(12) ==
          CommandExecutionDecision::Rejected);

    CommandAckCore confirmed;
    CHECK(confirmed.restore_after_boot(original.record(), 3, true) ==
          CommandTransitionResult::Accepted);
    CHECK(confirmed.state() == CommandJournalState::Executed);
    CHECK(confirmed.record().result == CommandResult::Executed);
    CHECK(confirmed.record().error == CommandError::None);
}

} // namespace

int main()
{
    check_codec_round_trip_and_exact_shape();
    check_codec_rejects_malformed_or_relationally_invalid_payloads();
    check_codec_accepts_only_canonical_receipt_error_pairs();
    check_normal_command_flow_and_exact_receipts();
    check_expected_effect_and_ttl_checkpoint_contract();
    check_duplicate_stale_and_no_command_paths();
    check_ttl_expiry_never_dispatches();
    check_mismatch_and_timeout_keep_journal();
    check_boot_recovery_never_reexecutes_execute_marked();
    std::cout << "command_ack_core_test checks=" << checks << '\n';
    return 0;
}
