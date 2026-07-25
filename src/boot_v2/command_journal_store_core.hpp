#ifndef NB_IOT_BOOT_V2_COMMAND_JOURNAL_STORE_CORE_HPP
#define NB_IOT_BOOT_V2_COMMAND_JOURNAL_STORE_CORE_HPP

#include <cstddef>
#include <cstdint>

#include "command_journal_record_core.hpp"

namespace boot_v2 {

enum class CommandJournalStoreStatus : std::uint8_t {
    Blank = 0,
    Loaded = 1,
    Committed = 2,
    Rejected = 3,
    InvalidInput = 4,
    ReadFailed = 5,
    WriteFailed = 6,
    VerifyFailed = 7,
};

using CommandJournalReadSlotFn = bool (*)(
    void *,
    CommandJournalSlot,
    CommandJournalRecordV1 &) noexcept;

using CommandJournalReplaceSlotFn = bool (*)(
    void *,
    CommandJournalSlot,
    const std::uint8_t *,
    std::size_t,
    std::uint32_t) noexcept;

struct CommandJournalStorePort {
    void *context{nullptr};
    CommandJournalReadSlotFn read_slot{nullptr};
    CommandJournalReplaceSlotFn replace_slot{nullptr};
};

struct CommandJournalStoreResult {
    CommandJournalStoreStatus status{
        CommandJournalStoreStatus::Rejected};
    CommandJournalSlot active_slot{CommandJournalSlot::None};
    std::uint32_t sequence{0};
};

[[nodiscard]] CommandJournalStoreResult command_journal_store_load(
    CommandJournalStorePort port,
    CommandJournalRecord &output) noexcept;

[[nodiscard]] CommandJournalStoreResult command_journal_store_commit(
    CommandJournalStorePort port,
    CommandJournalRecord record,
    std::uint32_t timeout_ms) noexcept;

} // namespace boot_v2

#endif
