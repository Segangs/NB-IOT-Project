#include "runtime_owner_shutdown_record_core.hpp"

#include <cstddef>
#include <cstdint>

namespace boot_v2 {
namespace {

bool planned_action_is_valid(
    const RuntimeOwnerShutdownPlannedAction action) noexcept
{
    return action == RuntimeOwnerShutdownPlannedAction::WatchdogReboot ||
           action == RuntimeOwnerShutdownPlannedAction::Gp15Kill;
}

bool sequence_is_newer(
    const std::uint32_t candidate,
    const std::uint32_t reference) noexcept
{
    return static_cast<std::int32_t>(candidate - reference) > 0;
}

} // namespace

std::uint32_t runtime_owner_shutdown_record_crc(
    const RuntimeOwnerShutdownRecordV1 &record) noexcept
{
    const auto *const bytes =
        reinterpret_cast<const std::uint8_t *>(&record);
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0;
         index < offsetof(RuntimeOwnerShutdownRecordV1, crc32);
         ++index) {
        crc ^= bytes[index];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0u - static_cast<std::uint32_t>(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

RuntimeOwnerShutdownRecordV1 runtime_owner_shutdown_record_make(
    const RuntimeOwnerShutdownRecordInput input,
    const std::uint32_t sequence) noexcept
{
    RuntimeOwnerShutdownRecordV1 record{};
    record.magic = RUNTIME_OWNER_SHUTDOWN_RECORD_MAGIC;
    record.version = RUNTIME_OWNER_SHUTDOWN_RECORD_VERSION;
    record.size = sizeof(RuntimeOwnerShutdownRecordV1);
    record.sequence = sequence;
    record.producer_sequence = input.producer_sequence;
    record.incident_correlation_id = input.incident_correlation_id;
    record.elapsed_ms = input.elapsed_ms;
    record.reason = input.reason;
    record.initial_usb_present = input.initial_usb_present;
    record.planned_action = input.planned_action;
    record.hard_deadline_reached = input.hard_deadline_reached;
    record.cleanup_succeeded_mask = input.cleanup_succeeded_mask;
    record.cleanup_failed_mask = input.cleanup_failed_mask;
    record.cleanup_timed_out_mask = input.cleanup_timed_out_mask;
    record.cleanup_skipped_mask = input.cleanup_skipped_mask;
    record.hardware_reset_count = input.hardware_reset_count;
    record.crc32 = runtime_owner_shutdown_record_crc(record);
    return record;
}

bool runtime_owner_shutdown_record_valid(
    const RuntimeOwnerShutdownRecordV1 &record) noexcept
{
    const std::uint8_t classified = static_cast<std::uint8_t>(
        record.cleanup_succeeded_mask | record.cleanup_failed_mask |
        record.cleanup_timed_out_mask | record.cleanup_skipped_mask);
    const std::uint8_t overlap = static_cast<std::uint8_t>(
        (record.cleanup_succeeded_mask & record.cleanup_failed_mask) |
        (record.cleanup_succeeded_mask & record.cleanup_timed_out_mask) |
        (record.cleanup_succeeded_mask & record.cleanup_skipped_mask) |
        (record.cleanup_failed_mask & record.cleanup_timed_out_mask) |
        (record.cleanup_failed_mask & record.cleanup_skipped_mask) |
        (record.cleanup_timed_out_mask & record.cleanup_skipped_mask));
    return record.magic == RUNTIME_OWNER_SHUTDOWN_RECORD_MAGIC &&
           record.version == RUNTIME_OWNER_SHUTDOWN_RECORD_VERSION &&
           record.size == sizeof(RuntimeOwnerShutdownRecordV1) &&
           record.sequence != 0 && record.reason >= 1 && record.reason <= 3 &&
           record.initial_usb_present <= 1 &&
           planned_action_is_valid(record.planned_action) &&
           record.hard_deadline_reached <= 1 && classified == 0x7Fu &&
           overlap == 0 &&
           record.crc32 == runtime_owner_shutdown_record_crc(record);
}

const RuntimeOwnerShutdownRecordV1 *
runtime_owner_shutdown_record_select_latest(
    const RuntimeOwnerShutdownRecordV1 &slot_a,
    const RuntimeOwnerShutdownRecordV1 &slot_b) noexcept
{
    const bool a_valid = runtime_owner_shutdown_record_valid(slot_a);
    const bool b_valid = runtime_owner_shutdown_record_valid(slot_b);
    if (!a_valid) {
        return b_valid ? &slot_b : nullptr;
    }
    if (!b_valid) {
        return &slot_a;
    }
    return sequence_is_newer(slot_b.sequence, slot_a.sequence)
               ? &slot_b
               : &slot_a;
}

} // namespace boot_v2
