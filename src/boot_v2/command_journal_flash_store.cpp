#include "command_journal_flash_store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hardware/regs/addressmap.h"
#include "pico.h"

#include "flash_partition_layout.hpp"
#include "../lib/flash_operation_service.hpp"

namespace boot_v2 {
namespace {

static_assert(flash_partition::total_size == PICO_FLASH_SIZE_BYTES);
static_assert(
    flash_partition::command_journal_slot_size ==
    flash_partition::sector_size);
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
    bool verification_attempted{false};
    bool verified{false};
};

FlashOperationResult command_journal_flash_replace_transaction(
    FlashOperationTransaction &transaction,
    void *const parameter) noexcept
{
    auto *const write =
        static_cast<CommandJournalFlashWrite *>(parameter);
    if (write == nullptr) {
        return FlashOperationCode::InvalidArgument;
    }
    const FlashOperationResult result =
        transaction.replace_sector(
        write->offset,
        write->offset,
        write->page,
        flash_partition::page_size);
    if (result.mutation ==
        FlashMutationDisposition::NotAttempted) {
        return result;
    }

    alignas(flash_partition::page_size)
        std::uint8_t readback[flash_partition::page_size];
    const FlashOperationResult read_result = transaction.read(
        write->offset, readback, sizeof(readback));
    if (read_result == FlashOperationCode::Succeeded) {
        write->verification_attempted = true;
        write->verified =
            std::memcmp(
                readback,
                write->page,
                sizeof(readback)) == 0;
    }
    return result;
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
    const FlashOperationResult result =
        flash_operation_execute(
            command_journal_flash_replace_transaction,
            &write,
            timeout_ms);
    if (result.mutation ==
        FlashMutationDisposition::NotAttempted) {
        return false;
    }
    return write.verification_attempted &&
           write.verified;
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
