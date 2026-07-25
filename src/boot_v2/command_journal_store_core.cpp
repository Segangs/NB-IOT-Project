#include "command_journal_store_core.hpp"

#include <cstdint>
#include <cstring>

namespace boot_v2 {
namespace {

CommandJournalStoreResult store_result(
    const CommandJournalStoreStatus status,
    const CommandJournalSlot active_slot = CommandJournalSlot::None,
    const std::uint32_t sequence = 0) noexcept
{
    return {status, active_slot, sequence};
}

} // namespace

CommandJournalStoreResult command_journal_store_load(
    const CommandJournalStorePort port,
    CommandJournalRecord &output) noexcept
{
    if (port.read_slot == nullptr ||
        port.replace_slot == nullptr) {
        return store_result(CommandJournalStoreStatus::InvalidInput);
    }

    CommandJournalRecordV1 slot_a{};
    CommandJournalRecordV1 slot_b{};
    if (!port.read_slot(
            port.context, CommandJournalSlot::A, slot_a) ||
        !port.read_slot(
            port.context, CommandJournalSlot::B, slot_b)) {
        return store_result(CommandJournalStoreStatus::ReadFailed);
    }

    const CommandJournalSelection selection =
        command_journal_record_select(slot_a, slot_b);
    if (selection.status == CommandJournalSelectionStatus::Blank) {
        return store_result(CommandJournalStoreStatus::Blank);
    }
    if (selection.status != CommandJournalSelectionStatus::Selected) {
        return store_result(CommandJournalStoreStatus::Rejected);
    }

    const CommandJournalRecordV1 *selected = nullptr;
    if (selection.active_slot == CommandJournalSlot::A) {
        selected = &slot_a;
    } else if (selection.active_slot == CommandJournalSlot::B) {
        selected = &slot_b;
    }
    if (selected == nullptr) {
        return store_result(CommandJournalStoreStatus::Rejected);
    }

    CommandJournalRecord decoded{};
    if (!command_journal_record_decode(*selected, decoded)) {
        return store_result(CommandJournalStoreStatus::Rejected);
    }

    output = decoded;
    return store_result(
        CommandJournalStoreStatus::Loaded,
        selection.active_slot,
        selection.active_sequence);
}

CommandJournalStoreResult command_journal_store_commit(
    const CommandJournalStorePort port,
    const CommandJournalRecord record,
    const std::uint32_t timeout_ms) noexcept
{
    if (port.read_slot == nullptr ||
        port.replace_slot == nullptr ||
        timeout_ms == 0 ||
        !command_journal_record_is_canonical(record)) {
        return store_result(CommandJournalStoreStatus::InvalidInput);
    }

    CommandJournalRecordV1 slot_a{};
    CommandJournalRecordV1 slot_b{};
    if (!port.read_slot(
            port.context, CommandJournalSlot::A, slot_a) ||
        !port.read_slot(
            port.context, CommandJournalSlot::B, slot_b)) {
        return store_result(CommandJournalStoreStatus::ReadFailed);
    }

    const CommandJournalSelection selection =
        command_journal_record_select(slot_a, slot_b);
    CommandJournalSlot target_slot = CommandJournalSlot::None;
    std::uint32_t sequence = 0;
    if (selection.status == CommandJournalSelectionStatus::Blank) {
        target_slot = CommandJournalSlot::A;
        sequence = 1;
    } else if (
        selection.status ==
        CommandJournalSelectionStatus::Selected) {
        target_slot = selection.next_write_slot;
        sequence =
            command_journal_next_sequence(selection.active_sequence);
    } else {
        return store_result(CommandJournalStoreStatus::Rejected);
    }
    if (target_slot != CommandJournalSlot::A &&
        target_slot != CommandJournalSlot::B) {
        return store_result(CommandJournalStoreStatus::Rejected);
    }

    CommandJournalRecordV1 encoded{};
    if (!command_journal_record_encode(record, sequence, encoded)) {
        return store_result(CommandJournalStoreStatus::Rejected);
    }

    alignas(256) std::uint8_t page[256];
    std::memset(page, 0xFF, sizeof(page));
    std::memcpy(page, &encoded, sizeof(encoded));
    if (!port.replace_slot(
            port.context,
            target_slot,
            page,
            sizeof(page),
            timeout_ms)) {
        return store_result(CommandJournalStoreStatus::WriteFailed);
    }

    CommandJournalRecordV1 verified{};
    if (!port.read_slot(
            port.context, target_slot, verified) ||
        std::memcmp(&verified, &encoded, sizeof(encoded)) != 0) {
        return store_result(CommandJournalStoreStatus::VerifyFailed);
    }

    return store_result(
        CommandJournalStoreStatus::Committed,
        target_slot,
        sequence);
}

} // namespace boot_v2
