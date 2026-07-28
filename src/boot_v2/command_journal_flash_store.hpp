#ifndef NB_IOT_BOOT_V2_COMMAND_JOURNAL_FLASH_STORE_HPP
#define NB_IOT_BOOT_V2_COMMAND_JOURNAL_FLASH_STORE_HPP

#include <cstdint>

#include "command_journal_store_core.hpp"

namespace boot_v2 {

[[nodiscard]] CommandJournalStorePort
command_journal_flash_port() noexcept;

[[nodiscard]] CommandJournalStoreResult command_journal_flash_load(
    CommandJournalRecord &output) noexcept;

[[nodiscard]] CommandJournalStoreResult command_journal_flash_commit(
    CommandJournalRecord record,
    std::uint32_t timeout_ms) noexcept;

} // namespace boot_v2

#endif
