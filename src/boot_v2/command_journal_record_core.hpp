#ifndef NB_IOT_BOOT_V2_COMMAND_JOURNAL_RECORD_CORE_HPP
#define NB_IOT_BOOT_V2_COMMAND_JOURNAL_RECORD_CORE_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "command_ack_core.hpp"

namespace boot_v2 {

constexpr std::uint32_t COMMAND_JOURNAL_RECORD_MAGIC = 0x434A5231u;
constexpr std::uint16_t COMMAND_JOURNAL_RECORD_VERSION = 1u;

struct alignas(32) CommandJournalRecordV1 {
    std::uint32_t magic{0};
    std::uint16_t version{0};
    std::uint16_t size{0};
    std::uint32_t sequence{0};
    std::uint32_t cmd_id{0};
    std::uint32_t job_ref{0};
    std::uint32_t ttl_seconds{0};
    std::uint32_t remaining_ttl_seconds{0};
    std::uint32_t ttl_checkpoint_monotonic_seconds{0};
    std::uint32_t ttl_checkpoint_unix_seconds{0};
    std::uint32_t ttl_checkpoint_boot_sequence{0};
    std::uint32_t boot_sequence_before_execute{0};
    CommandJournalState state{CommandJournalState::Empty};
    CommandOpcode opcode{CommandOpcode::None};
    CommandAckPhase phase{CommandAckPhase::Invalid};
    CommandResult result{CommandResult::None};
    CommandError error{CommandError::None};
    std::uint8_t retry_count{0};
    CommandExpectedEffect expected_effect{CommandExpectedEffect::None};
    std::uint8_t ttl_checkpoint_clock_valid{0};
    std::uint8_t dispatch_latched{0};
    std::uint8_t reserved[7]{};
    std::uint32_t crc32{0};
};

enum class CommandJournalSlot : std::uint8_t {
    None = 0,
    A = 1,
    B = 2,
};

enum class CommandJournalSlotState : std::uint8_t {
    Blank = 0,
    Valid = 1,
    Corrupt = 2,
};

enum class CommandJournalSelectionStatus : std::uint8_t {
    Blank = 0,
    Selected = 1,
    Rejected = 2,
};

struct CommandJournalSelection {
    CommandJournalSelectionStatus status{
        CommandJournalSelectionStatus::Rejected};
    CommandJournalSlot active_slot{CommandJournalSlot::None};
    CommandJournalSlot next_write_slot{CommandJournalSlot::None};
    std::uint32_t active_sequence{0};
    std::uint8_t active_is_empty{0};
};

static_assert(sizeof(CommandJournalRecordV1) == 64);
static_assert(alignof(CommandJournalRecordV1) == 32);
static_assert(std::is_standard_layout<CommandJournalRecordV1>::value);
static_assert(std::is_trivially_copyable<CommandJournalRecordV1>::value);
static_assert(offsetof(CommandJournalRecordV1, magic) == 0);
static_assert(offsetof(CommandJournalRecordV1, version) == 4);
static_assert(offsetof(CommandJournalRecordV1, size) == 6);
static_assert(offsetof(CommandJournalRecordV1, sequence) == 8);
static_assert(offsetof(CommandJournalRecordV1, cmd_id) == 12);
static_assert(offsetof(CommandJournalRecordV1, job_ref) == 16);
static_assert(offsetof(CommandJournalRecordV1, ttl_seconds) == 20);
static_assert(
    offsetof(CommandJournalRecordV1, remaining_ttl_seconds) == 24);
static_assert(
    offsetof(
        CommandJournalRecordV1,
        ttl_checkpoint_monotonic_seconds) == 28);
static_assert(
    offsetof(CommandJournalRecordV1, ttl_checkpoint_unix_seconds) == 32);
static_assert(
    offsetof(CommandJournalRecordV1, ttl_checkpoint_boot_sequence) == 36);
static_assert(
    offsetof(CommandJournalRecordV1, boot_sequence_before_execute) == 40);
static_assert(offsetof(CommandJournalRecordV1, state) == 44);
static_assert(offsetof(CommandJournalRecordV1, opcode) == 45);
static_assert(offsetof(CommandJournalRecordV1, phase) == 46);
static_assert(offsetof(CommandJournalRecordV1, result) == 47);
static_assert(offsetof(CommandJournalRecordV1, error) == 48);
static_assert(offsetof(CommandJournalRecordV1, retry_count) == 49);
static_assert(offsetof(CommandJournalRecordV1, expected_effect) == 50);
static_assert(
    offsetof(CommandJournalRecordV1, ttl_checkpoint_clock_valid) == 51);
static_assert(offsetof(CommandJournalRecordV1, dispatch_latched) == 52);
static_assert(offsetof(CommandJournalRecordV1, reserved) == 53);
static_assert(offsetof(CommandJournalRecordV1, crc32) == 60);

[[nodiscard]] std::uint32_t command_journal_crc32(
    const std::uint8_t *data,
    std::size_t size) noexcept;

[[nodiscard]] std::uint32_t command_journal_record_crc32(
    const CommandJournalRecordV1 &record) noexcept;

[[nodiscard]] bool command_journal_record_encode(
    CommandJournalRecord runtime,
    std::uint32_t sequence,
    CommandJournalRecordV1 &output) noexcept;

[[nodiscard]] bool command_journal_record_valid(
    const CommandJournalRecordV1 &record) noexcept;

[[nodiscard]] bool command_journal_record_decode(
    const CommandJournalRecordV1 &record,
    CommandJournalRecord &output) noexcept;

[[nodiscard]] CommandJournalSlotState
command_journal_record_classify(
    const CommandJournalRecordV1 &record) noexcept;

[[nodiscard]] CommandJournalSelection command_journal_record_select(
    const CommandJournalRecordV1 &slot_a,
    const CommandJournalRecordV1 &slot_b) noexcept;

[[nodiscard]] std::uint32_t command_journal_next_sequence(
    std::uint32_t current) noexcept;

[[nodiscard]] bool command_journal_retry_increment(
    CommandJournalRecord &record) noexcept;

} // namespace boot_v2

#endif
