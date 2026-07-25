#include "runtime_owner_shutdown_record_store.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include "pico/platform.h"

#include "flash_partition_layout.hpp"
#include "../lib/log.hpp"

namespace boot_v2 {
namespace {

static_assert(
    flash_partition::shutdown_record_a_offset +
        flash_partition::shutdown_record_slot_size ==
    flash_partition::shutdown_record_b_offset);
static_assert(
    flash_partition::shutdown_record_b_offset +
        flash_partition::shutdown_record_slot_size ==
    flash_partition::total_size);
static_assert(flash_partition::total_size == PICO_FLASH_SIZE_BYTES);

const RuntimeOwnerShutdownRecordV1 &slot(
    const std::uint32_t offset) noexcept
{
    return *reinterpret_cast<const RuntimeOwnerShutdownRecordV1 *>(
        XIP_BASE + offset);
}

struct ShutdownRecordFlashWrite {
    std::uint32_t offset{0};
    const std::uint8_t *page{nullptr};
};

void __no_inline_not_in_flash_func(shutdown_record_flash_callback)(
    void *parameter)
{
    auto *const write =
        static_cast<ShutdownRecordFlashWrite *>(parameter);
    flash_range_erase(
        write->offset, flash_partition::shutdown_record_slot_size);
    flash_range_program(
        write->offset, write->page, flash_partition::page_size);
}

std::uint32_t next_sequence(
    const RuntimeOwnerShutdownRecordV1 *const latest) noexcept
{
    if (latest == nullptr ||
        latest->sequence == std::numeric_limits<std::uint32_t>::max()) {
        return 1;
    }
    const std::uint32_t next = latest->sequence + 1u;
    return next == 0 ? 1 : next;
}

} // namespace

const RuntimeOwnerShutdownRecordV1 *
runtime_owner_shutdown_record_current() noexcept
{
    const RuntimeOwnerShutdownRecordV1 &slot_a =
        slot(flash_partition::shutdown_record_a_offset);
    const RuntimeOwnerShutdownRecordV1 &slot_b =
        slot(flash_partition::shutdown_record_b_offset);
    return runtime_owner_shutdown_record_select_latest(slot_a, slot_b);
}

bool runtime_owner_shutdown_record_commit(
    const RuntimeOwnerShutdownRecordInput input,
    const std::uint32_t timeout_ms) noexcept
{
    if (timeout_ms == 0) {
        return false;
    }

    const RuntimeOwnerShutdownRecordV1 &slot_a =
        slot(flash_partition::shutdown_record_a_offset);
    const RuntimeOwnerShutdownRecordV1 &slot_b =
        slot(flash_partition::shutdown_record_b_offset);
    const RuntimeOwnerShutdownRecordV1 *const latest =
        runtime_owner_shutdown_record_select_latest(slot_a, slot_b);
    const std::uint32_t target_offset =
        latest == &slot_a ? flash_partition::shutdown_record_b_offset
                          : flash_partition::shutdown_record_a_offset;
    const RuntimeOwnerShutdownRecordV1 record =
        runtime_owner_shutdown_record_make(input, next_sequence(latest));
    if (!runtime_owner_shutdown_record_valid(record)) {
        return false;
    }

    alignas(flash_partition::page_size)
        std::uint8_t page[flash_partition::page_size];
    std::memset(page, 0xFF, sizeof(page));
    std::memcpy(page, &record, sizeof(record));
    ShutdownRecordFlashWrite write{target_offset, page};
    if (flash_safe_execute(
            shutdown_record_flash_callback, &write, timeout_ms) != 0) {
        return false;
    }

    const RuntimeOwnerShutdownRecordV1 &written = slot(target_offset);
    return runtime_owner_shutdown_record_valid(written) &&
           std::memcmp(&written, &record, sizeof(record)) == 0;
}

void runtime_owner_shutdown_record_log_current() noexcept
{
    const RuntimeOwnerShutdownRecordV1 *const record =
        runtime_owner_shutdown_record_current();
    if (record == nullptr) {
        LOG("LAST_SHUTDOWN result=NONE\n");
        return;
    }
    LOG(
        "LAST_SHUTDOWN SEQ=%lu REASON=%u USB=%u ACTION=%u "
        "OK=0x%02X FAIL=0x%02X TIMEOUT=0x%02X SKIP=0x%02X "
        "DEADLINE=%u ELAPSED=%lu RESET=%u\n",
        static_cast<unsigned long>(record->sequence),
        static_cast<unsigned>(record->reason),
        static_cast<unsigned>(record->initial_usb_present),
        static_cast<unsigned>(record->planned_action),
        static_cast<unsigned>(record->cleanup_succeeded_mask),
        static_cast<unsigned>(record->cleanup_failed_mask),
        static_cast<unsigned>(record->cleanup_timed_out_mask),
        static_cast<unsigned>(record->cleanup_skipped_mask),
        static_cast<unsigned>(record->hard_deadline_reached),
        static_cast<unsigned long>(record->elapsed_ms),
        static_cast<unsigned>(record->hardware_reset_count));
}

} // namespace boot_v2
