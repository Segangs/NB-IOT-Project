#include "command_journal_record_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {

using namespace boot_v2;

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

void test_binary_layout_and_crc_known_answer() noexcept
{
    CHECK(sizeof(CommandJournalRecordV1) == 64);
    CHECK(alignof(CommandJournalRecordV1) == 32);
    CHECK(std::is_standard_layout<CommandJournalRecordV1>::value);
    CHECK(std::is_trivially_copyable<CommandJournalRecordV1>::value);
    CHECK(offsetof(CommandJournalRecordV1, magic) == 0);
    CHECK(offsetof(CommandJournalRecordV1, version) == 4);
    CHECK(offsetof(CommandJournalRecordV1, size) == 6);
    CHECK(offsetof(CommandJournalRecordV1, sequence) == 8);
    CHECK(offsetof(CommandJournalRecordV1, cmd_id) == 12);
    CHECK(offsetof(CommandJournalRecordV1, job_ref) == 16);
    CHECK(offsetof(CommandJournalRecordV1, ttl_seconds) == 20);
    CHECK(
        offsetof(
            CommandJournalRecordV1,
            remaining_ttl_seconds) == 24);
    CHECK(
        offsetof(
            CommandJournalRecordV1,
            ttl_checkpoint_monotonic_seconds) == 28);
    CHECK(
        offsetof(
            CommandJournalRecordV1,
            ttl_checkpoint_unix_seconds) == 32);
    CHECK(
        offsetof(
            CommandJournalRecordV1,
            ttl_checkpoint_boot_sequence) == 36);
    CHECK(
        offsetof(
            CommandJournalRecordV1,
            boot_sequence_before_execute) == 40);
    CHECK(offsetof(CommandJournalRecordV1, state) == 44);
    CHECK(offsetof(CommandJournalRecordV1, opcode) == 45);
    CHECK(offsetof(CommandJournalRecordV1, phase) == 46);
    CHECK(offsetof(CommandJournalRecordV1, result) == 47);
    CHECK(offsetof(CommandJournalRecordV1, error) == 48);
    CHECK(offsetof(CommandJournalRecordV1, retry_count) == 49);
    CHECK(offsetof(CommandJournalRecordV1, expected_effect) == 50);
    CHECK(
        offsetof(
            CommandJournalRecordV1,
            ttl_checkpoint_clock_valid) == 51);
    CHECK(offsetof(CommandJournalRecordV1, dispatch_latched) == 52);
    CHECK(offsetof(CommandJournalRecordV1, reserved) == 53);
    CHECK(offsetof(CommandJournalRecordV1, crc32) == 60);

    const std::uint8_t input[] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };
    CHECK(
        command_journal_crc32(input, sizeof(input)) ==
        0xCBF43926u);
}

CommandJournalRecord canonical_runtime_record() noexcept
{
    CommandJournalRecord record{};
    record.state = CommandJournalState::AcceptedPersisted;
    record.opcode = CommandOpcode::Reboot;
    record.phase = CommandAckPhase::Accepted;
    record.result = CommandResult::Accepted;
    record.error = CommandError::None;
    record.retry_count = 3;
    record.expected_effect = CommandExpectedEffect::Reset;
    record.ttl_checkpoint_clock_valid = 1;
    record.dispatch_latched = 0;
    record.cmd_id = 17;
    record.job_id = 23;
    record.ttl_seconds = 600;
    record.remaining_ttl_seconds = 590;
    record.ttl_checkpoint_monotonic_seconds = 100;
    record.ttl_checkpoint_unix_seconds = 1720000000u;
    record.ttl_checkpoint_boot_sequence = 7;
    record.boot_sequence_before_execute = 7;
    return record;
}

bool same_runtime_record(
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

void test_encode_decode_and_little_endian_contract() noexcept
{
    const CommandJournalRecord runtime = canonical_runtime_record();
    CommandJournalRecordV1 record{};
    CHECK(command_journal_record_encode(runtime, 0x11223344u, record));
    CHECK(record.magic == COMMAND_JOURNAL_RECORD_MAGIC);
    CHECK(record.version == COMMAND_JOURNAL_RECORD_VERSION);
    CHECK(record.size == 64);
    CHECK(record.sequence == 0x11223344u);
    CHECK(record.cmd_id == 17);
    CHECK(record.job_ref == 23);
    CHECK(record.ttl_seconds == 600);
    CHECK(record.remaining_ttl_seconds == 590);
    CHECK(record.ttl_checkpoint_monotonic_seconds == 100);
    CHECK(record.ttl_checkpoint_unix_seconds == 1720000000u);
    CHECK(record.ttl_checkpoint_boot_sequence == 7);
    CHECK(record.boot_sequence_before_execute == 7);
    CHECK(record.state == CommandJournalState::AcceptedPersisted);
    CHECK(record.opcode == CommandOpcode::Reboot);
    CHECK(record.phase == CommandAckPhase::Accepted);
    CHECK(record.result == CommandResult::Accepted);
    CHECK(record.error == CommandError::None);
    CHECK(record.retry_count == 3);
    CHECK(record.expected_effect == CommandExpectedEffect::Reset);
    CHECK(record.ttl_checkpoint_clock_valid == 1);
    CHECK(record.dispatch_latched == 0);
    for (const std::uint8_t value : record.reserved) {
        CHECK(value == 0);
    }
    const auto *const bytes =
        reinterpret_cast<const std::uint8_t *>(&record);
    CHECK(bytes[8] == 0x44);
    CHECK(bytes[9] == 0x33);
    CHECK(bytes[10] == 0x22);
    CHECK(bytes[11] == 0x11);
    CHECK(record.crc32 == command_journal_record_crc32(record));
    CHECK(command_journal_record_valid(record));

    CommandJournalRecord decoded{};
    CHECK(command_journal_record_decode(record, decoded));
    CHECK(same_runtime_record(decoded, runtime));
}

void test_encode_decode_failure_keeps_output_unchanged() noexcept
{
    CommandJournalRecord runtime = canonical_runtime_record();
    CommandJournalRecordV1 output{};
    std::memset(&output, 0xA5, sizeof(output));
    const CommandJournalRecordV1 original_output = output;
    CHECK(!command_journal_record_encode(runtime, 0, output));
    CHECK(
        std::memcmp(&output, &original_output, sizeof(output)) == 0);

    runtime.remaining_ttl_seconds = runtime.ttl_seconds + 1;
    CHECK(!command_journal_record_encode(runtime, 1, output));
    CHECK(
        std::memcmp(&output, &original_output, sizeof(output)) == 0);

    CommandJournalRecordV1 invalid = original_output;
    CommandJournalRecord decoded = canonical_runtime_record();
    const CommandJournalRecord original_decoded = decoded;
    CHECK(!command_journal_record_decode(invalid, decoded));
    CHECK(same_runtime_record(decoded, original_decoded));
}

bool invalid_with_recomputed_crc(
    CommandJournalRecordV1 record) noexcept
{
    record.crc32 = command_journal_record_crc32(record);
    return !command_journal_record_valid(record);
}

CommandJournalRecord runtime_record_for_state(
    const CommandJournalState state) noexcept
{
    if (state == CommandJournalState::Empty) {
        return {};
    }
    CommandJournalRecord record = canonical_runtime_record();
    record.state = state;
    if (state <= CommandJournalState::AcceptedReceipted) {
        record.phase = CommandAckPhase::Accepted;
        record.result = CommandResult::Accepted;
        record.error = CommandError::None;
        record.dispatch_latched = 0;
    } else if (state == CommandJournalState::ExecuteMarked) {
        record.phase = CommandAckPhase::Accepted;
        record.result = CommandResult::Accepted;
        record.error = CommandError::None;
        record.dispatch_latched = 1;
    } else {
        record.phase = CommandAckPhase::Final;
        record.result = CommandResult::Executed;
        record.error = CommandError::None;
        record.dispatch_latched = 1;
    }
    return record;
}

void test_every_record_byte_is_crc_protected() noexcept
{
    CommandJournalRecordV1 valid{};
    CHECK(command_journal_record_encode(
        canonical_runtime_record(), 5, valid));
    auto *const bytes =
        reinterpret_cast<std::uint8_t *>(&valid);
    for (std::size_t offset = 0; offset < sizeof(valid); ++offset) {
        bytes[offset] ^= 1u;
        CHECK(!command_journal_record_valid(valid));
        bytes[offset] ^= 1u;
        CHECK(command_journal_record_valid(valid));
    }
}

void test_semantic_corruption_is_rejected_with_valid_crc() noexcept
{
    CommandJournalRecordV1 valid{};
    CHECK(command_journal_record_encode(
        canonical_runtime_record(), 5, valid));

    CommandJournalRecordV1 mutated = valid;
    mutated.magic ^= 1u;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.version = 2;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.size = 63;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.sequence = 0;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.state = static_cast<CommandJournalState>(0xFF);
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.opcode = static_cast<CommandOpcode>(0xFF);
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.phase = static_cast<CommandAckPhase>(0xFF);
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.result = static_cast<CommandResult>(0xFF);
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.error = static_cast<CommandError>(0xFF);
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.expected_effect =
        static_cast<CommandExpectedEffect>(0xFF);
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.reserved[3] = 1;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.remaining_ttl_seconds = mutated.ttl_seconds + 1;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.ttl_checkpoint_clock_valid = 2;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.ttl_checkpoint_clock_valid = 0;
    mutated.ttl_checkpoint_unix_seconds = 1;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.ttl_checkpoint_boot_sequence = 0;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.expected_effect = CommandExpectedEffect::PowerOff;
    CHECK(invalid_with_recomputed_crc(mutated));
    mutated = valid;
    mutated.dispatch_latched = 1;
    CHECK(invalid_with_recomputed_crc(mutated));

    CommandJournalRecord execute =
        runtime_record_for_state(CommandJournalState::ExecuteMarked);
    CHECK(command_journal_record_encode(execute, 6, mutated));
    mutated.dispatch_latched = 0;
    CHECK(invalid_with_recomputed_crc(mutated));

    CommandJournalRecord terminal =
        runtime_record_for_state(CommandJournalState::Executed);
    CHECK(command_journal_record_encode(terminal, 7, mutated));
    mutated.phase = CommandAckPhase::Accepted;
    CHECK(invalid_with_recomputed_crc(mutated));
}

void test_every_journal_state_and_terminal_result_matrix() noexcept
{
    const CommandJournalState states[] = {
        CommandJournalState::Empty,
        CommandJournalState::AcceptedPersisted,
        CommandJournalState::AcceptedPublishPending,
        CommandJournalState::AcceptedPuback,
        CommandJournalState::AcceptedReceipted,
        CommandJournalState::ExecuteMarked,
        CommandJournalState::Executed,
        CommandJournalState::FinalPersisted,
        CommandJournalState::FinalPublishPending,
        CommandJournalState::FinalPuback,
        CommandJournalState::FinalReceipted,
    };
    std::uint32_t sequence = 1;
    for (const CommandJournalState state : states) {
        const CommandJournalRecord runtime =
            runtime_record_for_state(state);
        CommandJournalRecordV1 persistent{};
        CHECK(command_journal_record_encode(
            runtime, sequence, persistent));
        CHECK(command_journal_record_valid(persistent));
        CommandJournalRecord decoded{};
        CHECK(command_journal_record_decode(
            persistent, decoded));
        CHECK(same_runtime_record(runtime, decoded));
        ++sequence;
    }

    CommandJournalRecord terminal =
        runtime_record_for_state(CommandJournalState::Executed);
    terminal.result = CommandResult::Failed;
    terminal.error = CommandError::Journal;
    CommandJournalRecordV1 persistent{};
    CHECK(command_journal_record_encode(terminal, 20, persistent));
    terminal.result = CommandResult::Expired;
    terminal.error = CommandError::Expired;
    CHECK(command_journal_record_encode(terminal, 21, persistent));
    terminal.result = CommandResult::Executed;
    CHECK(!command_journal_record_encode(terminal, 22, persistent));
}

void test_empty_tombstone_is_valid_but_zero_record_is_not() noexcept
{
    const CommandJournalRecord empty{};
    CommandJournalRecordV1 tombstone{};
    CHECK(command_journal_record_encode(empty, 9, tombstone));
    CHECK(command_journal_record_valid(tombstone));
    CHECK(tombstone.sequence == 9);
    CHECK(tombstone.state == CommandJournalState::Empty);
    CHECK(tombstone.cmd_id == 0);
    CHECK(tombstone.job_ref == 0);
    CHECK(tombstone.ttl_seconds == 0);
    CHECK(tombstone.remaining_ttl_seconds == 0);
    CHECK(tombstone.opcode == CommandOpcode::None);
    CHECK(tombstone.phase == CommandAckPhase::Invalid);
    CHECK(tombstone.result == CommandResult::None);
    CHECK(tombstone.error == CommandError::None);
    CHECK(tombstone.retry_count == 0);
    CHECK(
        tombstone.expected_effect ==
        CommandExpectedEffect::None);
    CHECK(tombstone.ttl_checkpoint_clock_valid == 0);
    CHECK(tombstone.dispatch_latched == 0);
    for (const std::uint8_t value : tombstone.reserved) {
        CHECK(value == 0);
    }
    CommandJournalRecord decoded = canonical_runtime_record();
    CHECK(command_journal_record_decode(tombstone, decoded));
    CHECK(same_runtime_record(decoded, empty));

    const CommandJournalRecordV1 all_zero{};
    CHECK(!command_journal_record_valid(all_zero));
}

void test_persisted_execute_marked_never_reexecutes() noexcept
{
    const CommandJournalRecord execute =
        runtime_record_for_state(CommandJournalState::ExecuteMarked);
    CommandJournalRecordV1 persistent{};
    CHECK(command_journal_record_encode(execute, 30, persistent));
    CommandJournalRecord decoded{};
    CHECK(command_journal_record_decode(persistent, decoded));

    CommandAckCore recovered;
    CHECK(recovered.restore_after_boot(decoded, 8, false) ==
          CommandTransitionResult::Accepted);
    CHECK(recovered.state() == CommandJournalState::Executed);
    CHECK(recovered.record().result == CommandResult::Failed);
    CHECK(recovered.record().error == CommandError::Journal);
    CHECK(recovered.mark_execute(200) ==
          CommandExecutionDecision::Rejected);
}

CommandJournalRecordV1 blank_persistent_record() noexcept
{
    CommandJournalRecordV1 record{};
    std::memset(&record, 0xFF, sizeof(record));
    return record;
}

CommandJournalRecordV1 encoded_record(
    const CommandJournalRecord &runtime,
    const std::uint32_t sequence) noexcept
{
    CommandJournalRecordV1 record{};
    CHECK(command_journal_record_encode(runtime, sequence, record));
    return record;
}

void check_selection(
    const CommandJournalRecordV1 &slot_a,
    const CommandJournalRecordV1 &slot_b,
    const CommandJournalSelectionStatus status,
    const CommandJournalSlot active,
    const CommandJournalSlot next) noexcept
{
    const CommandJournalSelection selected =
        command_journal_record_select(slot_a, slot_b);
    CHECK(selected.status == status);
    CHECK(selected.active_slot == active);
    CHECK(selected.next_write_slot == next);
}

void test_blank_valid_corrupt_selection_matrix() noexcept
{
    const CommandJournalRecordV1 blank = blank_persistent_record();
    const CommandJournalRecordV1 valid =
        encoded_record(canonical_runtime_record(), 10);
    const CommandJournalRecordV1 corrupt{};

    CHECK(command_journal_record_classify(blank) ==
          CommandJournalSlotState::Blank);
    CHECK(command_journal_record_classify(valid) ==
          CommandJournalSlotState::Valid);
    CHECK(command_journal_record_classify(corrupt) ==
          CommandJournalSlotState::Corrupt);

    check_selection(
        blank,
        blank,
        CommandJournalSelectionStatus::Blank,
        CommandJournalSlot::None,
        CommandJournalSlot::A);
    check_selection(
        valid,
        blank,
        CommandJournalSelectionStatus::Selected,
        CommandJournalSlot::A,
        CommandJournalSlot::B);
    check_selection(
        blank,
        valid,
        CommandJournalSelectionStatus::Selected,
        CommandJournalSlot::B,
        CommandJournalSlot::A);
    check_selection(
        valid,
        corrupt,
        CommandJournalSelectionStatus::Selected,
        CommandJournalSlot::A,
        CommandJournalSlot::B);
    check_selection(
        corrupt,
        valid,
        CommandJournalSelectionStatus::Selected,
        CommandJournalSlot::B,
        CommandJournalSlot::A);
    check_selection(
        corrupt,
        blank,
        CommandJournalSelectionStatus::Rejected,
        CommandJournalSlot::None,
        CommandJournalSlot::None);
    check_selection(
        blank,
        corrupt,
        CommandJournalSelectionStatus::Rejected,
        CommandJournalSlot::None,
        CommandJournalSlot::None);
    check_selection(
        corrupt,
        corrupt,
        CommandJournalSelectionStatus::Rejected,
        CommandJournalSlot::None,
        CommandJournalSlot::None);
}

void test_dual_valid_sequence_wrap_and_conflict() noexcept
{
    const CommandJournalRecord runtime = canonical_runtime_record();
    CommandJournalRecordV1 slot_a = encoded_record(runtime, 12);
    CommandJournalRecordV1 slot_b = encoded_record(runtime, 13);
    CommandJournalSelection selected =
        command_journal_record_select(slot_a, slot_b);
    CHECK(selected.status == CommandJournalSelectionStatus::Selected);
    CHECK(selected.active_slot == CommandJournalSlot::B);
    CHECK(selected.next_write_slot == CommandJournalSlot::A);
    CHECK(selected.active_sequence == 13);
    CHECK(selected.active_is_empty == 0);

    slot_a = encoded_record(runtime, 0xFFFFFFFFu);
    slot_b = encoded_record(runtime, 1);
    selected = command_journal_record_select(slot_a, slot_b);
    CHECK(selected.status == CommandJournalSelectionStatus::Selected);
    CHECK(selected.active_slot == CommandJournalSlot::B);
    CHECK(selected.active_sequence == 1);

    slot_a = encoded_record(runtime, 1);
    slot_b = encoded_record(runtime, 0x80000001u);
    selected = command_journal_record_select(slot_a, slot_b);
    CHECK(selected.status == CommandJournalSelectionStatus::Rejected);
    CHECK(selected.active_slot == CommandJournalSlot::None);
    CHECK(selected.next_write_slot == CommandJournalSlot::None);

    slot_a = encoded_record(runtime, 42);
    slot_b = slot_a;
    selected = command_journal_record_select(slot_a, slot_b);
    CHECK(selected.status == CommandJournalSelectionStatus::Selected);
    CHECK(selected.active_slot == CommandJournalSlot::A);
    CHECK(selected.next_write_slot == CommandJournalSlot::B);

    CommandJournalRecord divergent = runtime;
    divergent.cmd_id = 18;
    slot_b = encoded_record(divergent, 42);
    selected = command_journal_record_select(slot_a, slot_b);
    CHECK(selected.status == CommandJournalSelectionStatus::Rejected);
    CHECK(selected.active_slot == CommandJournalSlot::None);
    CHECK(selected.next_write_slot == CommandJournalSlot::None);

    CHECK(command_journal_next_sequence(0) == 1);
    CHECK(command_journal_next_sequence(1) == 2);
    CHECK(command_journal_next_sequence(0xFFFFFFFFu) == 1);
}

void test_partial_program_keeps_previous_record_until_complete() noexcept
{
    const CommandJournalRecord final_runtime =
        runtime_record_for_state(CommandJournalState::FinalReceipted);
    const CommandJournalRecordV1 previous =
        encoded_record(final_runtime, 20);
    const CommandJournalRecordV1 tombstone =
        encoded_record(CommandJournalRecord{}, 21);

    for (std::size_t length = 1; length < sizeof(tombstone); ++length) {
        CommandJournalRecordV1 partial = blank_persistent_record();
        std::memcpy(&partial, &tombstone, length);
        const CommandJournalSelection selected =
            command_journal_record_select(previous, partial);
        CHECK(selected.status ==
              CommandJournalSelectionStatus::Selected);
        CHECK(selected.active_slot == CommandJournalSlot::A);
        CHECK(selected.active_sequence == 20);
        CHECK(selected.active_is_empty == 0);
    }

    CommandJournalSelection selected =
        command_journal_record_select(previous, tombstone);
    CHECK(selected.status == CommandJournalSelectionStatus::Selected);
    CHECK(selected.active_slot == CommandJournalSlot::B);
    CHECK(selected.active_sequence == 21);
    CHECK(selected.active_is_empty == 1);

    CommandJournalRecord next_runtime = canonical_runtime_record();
    next_runtime.cmd_id = 99;
    const CommandJournalRecordV1 next =
        encoded_record(next_runtime, 31);
    const CommandJournalRecordV1 older =
        encoded_record(canonical_runtime_record(), 30);
    for (std::size_t length = 1; length < sizeof(next); ++length) {
        CommandJournalRecordV1 partial = blank_persistent_record();
        std::memcpy(&partial, &next, length);
        selected = command_journal_record_select(older, partial);
        CHECK(selected.status ==
              CommandJournalSelectionStatus::Selected);
        CHECK(selected.active_slot == CommandJournalSlot::A);
        CHECK(selected.active_sequence == 30);
    }
    selected = command_journal_record_select(older, next);
    CHECK(selected.active_slot == CommandJournalSlot::B);
    CHECK(selected.active_sequence == 31);
}

void test_retry_count_saturates_without_wrap() noexcept
{
    CommandJournalRecord record = canonical_runtime_record();
    record.retry_count = 0;
    CHECK(command_journal_retry_increment(record));
    CHECK(record.retry_count == 1);
    record.retry_count = 254;
    CHECK(command_journal_retry_increment(record));
    CHECK(record.retry_count == 255);
    CHECK(!command_journal_retry_increment(record));
    CHECK(record.retry_count == 255);

    CommandJournalRecord empty{};
    CHECK(!command_journal_retry_increment(empty));
    CHECK(empty.retry_count == 0);
    record = canonical_runtime_record();
    record.remaining_ttl_seconds = record.ttl_seconds + 1;
    CHECK(!command_journal_retry_increment(record));
}

} // namespace

int main()
{
    test_binary_layout_and_crc_known_answer();
    test_encode_decode_and_little_endian_contract();
    test_encode_decode_failure_keeps_output_unchanged();
    test_every_record_byte_is_crc_protected();
    test_semantic_corruption_is_rejected_with_valid_crc();
    test_every_journal_state_and_terminal_result_matrix();
    test_empty_tombstone_is_valid_but_zero_record_is_not();
    test_persisted_execute_marked_never_reexecutes();
    test_blank_valid_corrupt_selection_matrix();
    test_dual_valid_sequence_wrap_and_conflict();
    test_partial_program_keeps_previous_record_until_complete();
    test_retry_count_saturates_without_wrap();

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "command_journal_record_core_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "command_journal_record_core_test: %zu checks passed\n",
        g_checks);
    return 0;
}
