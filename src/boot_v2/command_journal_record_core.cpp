#include "command_journal_record_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace boot_v2 {
namespace {

CommandJournalRecord runtime_record_from(
    const CommandJournalRecordV1 &record) noexcept
{
    CommandJournalRecord runtime{};
    runtime.state = record.state;
    runtime.opcode = record.opcode;
    runtime.phase = record.phase;
    runtime.result = record.result;
    runtime.error = record.error;
    runtime.retry_count = record.retry_count;
    runtime.expected_effect = record.expected_effect;
    runtime.ttl_checkpoint_clock_valid =
        record.ttl_checkpoint_clock_valid;
    runtime.dispatch_latched = record.dispatch_latched;
    runtime.cmd_id = record.cmd_id;
    runtime.job_id = record.job_ref;
    runtime.ttl_seconds = record.ttl_seconds;
    runtime.remaining_ttl_seconds =
        record.remaining_ttl_seconds;
    runtime.ttl_checkpoint_monotonic_seconds =
        record.ttl_checkpoint_monotonic_seconds;
    runtime.ttl_checkpoint_unix_seconds =
        record.ttl_checkpoint_unix_seconds;
    runtime.ttl_checkpoint_boot_sequence =
        record.ttl_checkpoint_boot_sequence;
    runtime.boot_sequence_before_execute =
        record.boot_sequence_before_execute;
    return runtime;
}

bool reserved_is_zero(
    const CommandJournalRecordV1 &record) noexcept
{
    for (const std::uint8_t value : record.reserved) {
        if (value != 0) {
            return false;
        }
    }
    return true;
}

bool records_are_identical(
    const CommandJournalRecordV1 &left,
    const CommandJournalRecordV1 &right) noexcept
{
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

CommandJournalSelection selected_record(
    const CommandJournalSlot active,
    const CommandJournalSlot next,
    const CommandJournalRecordV1 &record) noexcept
{
    return {
        CommandJournalSelectionStatus::Selected,
        active,
        next,
        record.sequence,
        static_cast<std::uint8_t>(
            record.state == CommandJournalState::Empty ? 1 : 0),
    };
}

} // namespace

std::uint32_t command_journal_crc32(
    const std::uint8_t *const data,
    const std::size_t size) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0u - static_cast<std::uint32_t>(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::uint32_t command_journal_record_crc32(
    const CommandJournalRecordV1 &record) noexcept
{
    return command_journal_crc32(
        reinterpret_cast<const std::uint8_t *>(&record),
        offsetof(CommandJournalRecordV1, crc32));
}

bool command_journal_record_encode(
    const CommandJournalRecord runtime,
    const std::uint32_t sequence,
    CommandJournalRecordV1 &output) noexcept
{
    if (sequence == 0 ||
        !command_journal_record_is_canonical(runtime)) {
        return false;
    }

    CommandJournalRecordV1 record{};
    record.magic = COMMAND_JOURNAL_RECORD_MAGIC;
    record.version = COMMAND_JOURNAL_RECORD_VERSION;
    record.size =
        static_cast<std::uint16_t>(sizeof(CommandJournalRecordV1));
    record.sequence = sequence;
    record.cmd_id = runtime.cmd_id;
    record.job_ref = runtime.job_id;
    record.ttl_seconds = runtime.ttl_seconds;
    record.remaining_ttl_seconds = runtime.remaining_ttl_seconds;
    record.ttl_checkpoint_monotonic_seconds =
        runtime.ttl_checkpoint_monotonic_seconds;
    record.ttl_checkpoint_unix_seconds =
        runtime.ttl_checkpoint_unix_seconds;
    record.ttl_checkpoint_boot_sequence =
        runtime.ttl_checkpoint_boot_sequence;
    record.boot_sequence_before_execute =
        runtime.boot_sequence_before_execute;
    record.state = runtime.state;
    record.opcode = runtime.opcode;
    record.phase = runtime.phase;
    record.result = runtime.result;
    record.error = runtime.error;
    record.retry_count = runtime.retry_count;
    record.expected_effect = runtime.expected_effect;
    record.ttl_checkpoint_clock_valid =
        runtime.ttl_checkpoint_clock_valid;
    record.dispatch_latched = runtime.dispatch_latched;
    record.crc32 = command_journal_record_crc32(record);

    if (!command_journal_record_valid(record)) {
        return false;
    }
    output = record;
    return true;
}

bool command_journal_record_valid(
    const CommandJournalRecordV1 &record) noexcept
{
    if (record.magic != COMMAND_JOURNAL_RECORD_MAGIC ||
        record.version != COMMAND_JOURNAL_RECORD_VERSION ||
        record.size != sizeof(CommandJournalRecordV1) ||
        record.sequence == 0 || !reserved_is_zero(record) ||
        record.crc32 != command_journal_record_crc32(record)) {
        return false;
    }
    return command_journal_record_is_canonical(
        runtime_record_from(record));
}

bool command_journal_record_decode(
    const CommandJournalRecordV1 &record,
    CommandJournalRecord &output) noexcept
{
    if (!command_journal_record_valid(record)) {
        return false;
    }
    output = runtime_record_from(record);
    return true;
}

CommandJournalSlotState command_journal_record_classify(
    const CommandJournalRecordV1 &record) noexcept
{
    const auto *const bytes =
        reinterpret_cast<const std::uint8_t *>(&record);
    bool all_erased = true;
    for (std::size_t index = 0; index < sizeof(record); ++index) {
        if (bytes[index] != 0xFFu) {
            all_erased = false;
            break;
        }
    }
    if (all_erased) {
        return CommandJournalSlotState::Blank;
    }
    return command_journal_record_valid(record)
               ? CommandJournalSlotState::Valid
               : CommandJournalSlotState::Corrupt;
}

CommandJournalSelection command_journal_record_select(
    const CommandJournalRecordV1 &slot_a,
    const CommandJournalRecordV1 &slot_b) noexcept
{
    const CommandJournalSlotState a_state =
        command_journal_record_classify(slot_a);
    const CommandJournalSlotState b_state =
        command_journal_record_classify(slot_b);
    const bool a_valid = a_state == CommandJournalSlotState::Valid;
    const bool b_valid = b_state == CommandJournalSlotState::Valid;

    if (a_valid && !b_valid) {
        return selected_record(
            CommandJournalSlot::A,
            CommandJournalSlot::B,
            slot_a);
    }
    if (!a_valid && b_valid) {
        return selected_record(
            CommandJournalSlot::B,
            CommandJournalSlot::A,
            slot_b);
    }
    if (!a_valid && !b_valid) {
        if (a_state == CommandJournalSlotState::Blank &&
            b_state == CommandJournalSlotState::Blank) {
            return {
                CommandJournalSelectionStatus::Blank,
                CommandJournalSlot::None,
                CommandJournalSlot::A,
                0,
                0,
            };
        }
        return {};
    }

    if (slot_a.sequence == slot_b.sequence) {
        return records_are_identical(slot_a, slot_b)
                   ? selected_record(
                         CommandJournalSlot::A,
                         CommandJournalSlot::B,
                         slot_a)
                   : CommandJournalSelection{};
    }

    const std::uint32_t delta =
        slot_b.sequence - slot_a.sequence;
    if (delta == 0x80000000u) {
        return {};
    }
    return delta < 0x80000000u
               ? selected_record(
                     CommandJournalSlot::B,
                     CommandJournalSlot::A,
                     slot_b)
               : selected_record(
                     CommandJournalSlot::A,
                     CommandJournalSlot::B,
                     slot_a);
}

std::uint32_t command_journal_next_sequence(
    const std::uint32_t current) noexcept
{
    return current == 0 || current == 0xFFFFFFFFu
               ? 1u
               : current + 1u;
}

bool command_journal_retry_increment(
    CommandJournalRecord &record) noexcept
{
    if (!command_journal_record_is_canonical(record) ||
        record.state == CommandJournalState::Empty ||
        record.retry_count == 0xFFu) {
        return false;
    }
    ++record.retry_count;
    return true;
}

} // namespace boot_v2
