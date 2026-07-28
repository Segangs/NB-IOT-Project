#include "command_journal_flash_store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include "pico/platform.h"

#include "flash_partition_layout.hpp"

namespace boot_v2 {
namespace {

static_assert(flash_partition::total_size == PICO_FLASH_SIZE_BYTES);
static_assert(
    flash_partition::command_journal_slot_size ==
    flash_partition::sector_size);
static_assert(flash_partition::sector_size == FLASH_SECTOR_SIZE);
static_assert(flash_partition::page_size == FLASH_PAGE_SIZE);
static_assert(
    sizeof(CommandJournalRecordV1) <= flash_partition::page_size);

bool command_journal_flash_slot_offset(
    const CommandJournalSlot slot,
    std::uint32_t &offset) noexcept
{
    switch (slot) {
    case CommandJournalSlot::A:
        offset = flash_partition::command_journal_a_offset;
        return true;
    case CommandJournalSlot::B:
        offset = flash_partition::command_journal_b_offset;
        return true;
    case CommandJournalSlot::None:
        return false;
    }
    return false;
}

bool command_journal_flash_read_slot(
    void *,
    const CommandJournalSlot slot,
    CommandJournalRecordV1 &output) noexcept
{
    std::uint32_t offset = 0;
    if (!command_journal_flash_slot_offset(slot, offset)) {
        return false;
    }

    const auto *const source =
        reinterpret_cast<const std::uint8_t *>(
            XIP_BASE + offset);
    std::memcpy(&output, source, sizeof(output));
    return true;
}

struct CommandJournalFlashWrite {
    std::uint32_t offset{0};
    const std::uint8_t *page{nullptr};
};

void __no_inline_not_in_flash_func(command_journal_flash_write_callback)(
    void *parameter)
{
    auto *const write =
        static_cast<CommandJournalFlashWrite *>(parameter);
    flash_range_erase(
        write->offset,
        flash_partition::command_journal_slot_size);
    flash_range_program(
        write->offset,
        write->page,
        flash_partition::page_size);
}

bool command_journal_flash_replace_slot(
    void *,
    const CommandJournalSlot slot,
    const std::uint8_t *const page,
    const std::size_t page_size,
    const std::uint32_t timeout_ms) noexcept
{
    if (page == nullptr ||
        page_size != flash_partition::page_size ||
        reinterpret_cast<std::uintptr_t>(page) %
                flash_partition::page_size !=
            0u) {
        return false;
    }

    std::uint32_t offset = 0;
    if (!command_journal_flash_slot_offset(slot, offset)) {
        return false;
    }

    CommandJournalFlashWrite write{offset, page};
    return flash_safe_execute(
               command_journal_flash_write_callback,
               &write,
               timeout_ms) == 0;
}

} // namespace

CommandJournalStorePort command_journal_flash_port() noexcept
{
    return {
        nullptr,
        command_journal_flash_read_slot,
        command_journal_flash_replace_slot,
    };
}

CommandJournalStoreResult command_journal_flash_load(
    CommandJournalRecord &output) noexcept
{
    return command_journal_store_load(
        command_journal_flash_port(), output);
}

CommandJournalStoreResult command_journal_flash_commit(
    const CommandJournalRecord record,
    const std::uint32_t timeout_ms) noexcept
{
    return command_journal_store_commit(
        command_journal_flash_port(), record, timeout_ms);
}

} // namespace boot_v2
