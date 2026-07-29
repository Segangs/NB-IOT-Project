#include "flash_operation_service.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "pico.h"
#include "pico/error.h"
#include "pico/flash.h"
#include "pico/time.h"

#include "../boot_v2/flash_partition_layout.hpp"

namespace boot_v2 {
namespace {

static_assert(flash_partition::sector_size == FLASH_SECTOR_SIZE);
static_assert(flash_partition::page_size == FLASH_PAGE_SIZE);
static_assert(flash_partition::total_size == PICO_FLASH_SIZE_BYTES);

StaticSemaphore_t g_flash_operation_mutex_storage{};
SemaphoreHandle_t const g_flash_operation_mutex =
    xSemaphoreCreateMutexStatic(&g_flash_operation_mutex_storage);
std::atomic_flag g_pre_scheduler_gate = ATOMIC_FLAG_INIT;

constexpr std::uint32_t MAXIMUM_FLASH_OPERATION_TIMEOUT_MS =
    60000u;

struct FlashRawOperation {
    std::uint32_t erase_offset{0};
    std::size_t erase_size{0};
    std::uint32_t program_offset{0};
    const std::uint8_t *program_data{nullptr};
    std::size_t program_size{0};
    bool callback_entered{false};
    bool callback_completed{false};
};

void __no_inline_not_in_flash_func(flash_operation_raw_callback)(
    void *parameter)
{
    auto *const operation =
        static_cast<FlashRawOperation *>(parameter);
    operation->callback_entered = true;
    if (operation->erase_size != 0u) {
        flash_range_erase(
            operation->erase_offset, operation->erase_size);
    }
    if (operation->program_size != 0u) {
        flash_range_program(
            operation->program_offset,
            operation->program_data,
            operation->program_size);
    }
    operation->callback_completed = true;
}

bool range_within(
    const std::uint32_t offset,
    const std::size_t size,
    const std::uint32_t partition_offset,
    const std::uint32_t partition_size) noexcept
{
    if (size == 0u ||
        offset < partition_offset ||
        offset >= flash_partition::total_size ||
        size > flash_partition::total_size - offset) {
        return false;
    }
    const std::uint32_t partition_end =
        partition_offset + partition_size;
    return offset < partition_end &&
           size <= partition_end - offset;
}

bool range_within_writable_partition(
    const std::uint32_t offset,
    const std::size_t size) noexcept
{
    return range_within(
               offset,
               size,
               flash_partition::command_journal_a_offset,
               flash_partition::command_journal_slot_size) ||
           range_within(
               offset,
               size,
               flash_partition::command_journal_b_offset,
               flash_partition::command_journal_slot_size) ||
           range_within(
               offset,
               size,
               flash_partition::sensor_log_offset,
               flash_partition::sensor_log_size) ||
           range_within(
               offset,
               size,
               flash_partition::shutdown_record_a_offset,
               flash_partition::shutdown_record_slot_size) ||
           range_within(
               offset,
               size,
               flash_partition::shutdown_record_b_offset,
               flash_partition::shutdown_record_slot_size);
}

bool valid_erase_range(
    const std::uint32_t offset,
    const std::size_t size) noexcept
{
    return size != 0u &&
           offset % flash_partition::sector_size == 0u &&
           size % flash_partition::sector_size == 0u &&
           range_within_writable_partition(offset, size);
}

bool valid_program_page(
    const std::uint32_t offset,
    const std::uint8_t *const page,
    const std::size_t size) noexcept
{
    return page != nullptr &&
           size == flash_partition::page_size &&
           offset % flash_partition::page_size == 0u &&
           reinterpret_cast<std::uintptr_t>(page) %
                   flash_partition::page_size ==
               0u &&
           range_within_writable_partition(offset, size);
}

std::uint32_t remaining_timeout_ms(
    const std::uint64_t deadline_us) noexcept
{
    const std::uint64_t now_us = time_us_64();
    if (now_us >= deadline_us) {
        return 0u;
    }
    const std::uint64_t remaining_us = deadline_us - now_us;
    return static_cast<std::uint32_t>(
        (remaining_us + 999u) / 1000u);
}

bool deadline_reached(
    const std::uint64_t deadline_us) noexcept
{
    return time_us_64() >= deadline_us;
}

bool timeout_to_ticks(
    const std::uint32_t timeout_ms,
    TickType_t &ticks) noexcept
{
    if (timeout_ms == 0u ||
        timeout_ms > MAXIMUM_FLASH_OPERATION_TIMEOUT_MS) {
        return false;
    }
    const std::uint64_t tick_count =
        static_cast<std::uint64_t>(timeout_ms) *
        configTICK_RATE_HZ / 1000u;
    if (tick_count == 0u ||
        tick_count >=
            std::numeric_limits<TickType_t>::max()) {
        return false;
    }
    ticks = static_cast<TickType_t>(tick_count);
    return true;
}

bool acquire_pre_scheduler_gate(
    const std::uint64_t deadline_us) noexcept
{
    while (time_us_64() < deadline_us) {
        if (!g_pre_scheduler_gate.test_and_set(
                std::memory_order_acquire)) {
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

void release_pre_scheduler_gate() noexcept
{
    g_pre_scheduler_gate.clear(std::memory_order_release);
}

} // namespace

FlashOperationTransaction::FlashOperationTransaction(
    const std::uint64_t deadline_us,
    const bool pre_scheduler) noexcept
    : deadline_us_(deadline_us),
      pre_scheduler_(pre_scheduler)
{
}

FlashOperationResult FlashOperationTransaction::read(
    const std::uint32_t offset,
    void *const output,
    const std::size_t size) noexcept
{
    if (output == nullptr || size == 0u) {
        return FlashOperationCode::InvalidArgument;
    }
    if (!range_within_writable_partition(offset, size)) {
        return FlashOperationCode::InvalidRange;
    }
    const bool deadline_exceeded =
        remaining_timeout_ms(deadline_us_) == 0u;
    if (deadline_exceeded &&
        !reconciliation_read_available_) {
        return {
            FlashOperationCode::TimedOut,
            FlashMutationDisposition::NotAttempted,
            true,
        };
    }

    const auto *const source =
        reinterpret_cast<const std::uint8_t *>(XIP_BASE + offset);
    std::memcpy(output, source, size);
    return {
        FlashOperationCode::Succeeded,
        FlashMutationDisposition::NotAttempted,
        deadline_exceeded,
    };
}

FlashOperationResult FlashOperationTransaction::erase_range(
    const std::uint32_t offset,
    const std::size_t size) noexcept
{
    if (size == 0u) {
        return FlashOperationCode::InvalidArgument;
    }
    if (offset % flash_partition::sector_size != 0u ||
        size % flash_partition::sector_size != 0u) {
        return FlashOperationCode::InvalidAlignment;
    }
    if (!valid_erase_range(offset, size)) {
        return FlashOperationCode::InvalidRange;
    }
    return execute_raw_operation(
        offset, size, 0u, nullptr, 0u);
}

FlashOperationResult FlashOperationTransaction::program_page(
    const std::uint32_t offset,
    const std::uint8_t *const page,
    const std::size_t size) noexcept
{
    if (page == nullptr || size == 0u) {
        return FlashOperationCode::InvalidArgument;
    }
    if (size != flash_partition::page_size ||
        offset % flash_partition::page_size != 0u ||
        reinterpret_cast<std::uintptr_t>(page) %
                flash_partition::page_size !=
            0u) {
        return FlashOperationCode::InvalidAlignment;
    }
    if (!valid_program_page(offset, page, size)) {
        return FlashOperationCode::InvalidRange;
    }
    return execute_raw_operation(
        0u, 0u, offset, page, size);
}

FlashOperationResult FlashOperationTransaction::replace_sector(
    const std::uint32_t sector_offset,
    const std::uint32_t page_offset,
    const std::uint8_t *const page,
    const std::size_t page_size) noexcept
{
    if (page == nullptr || page_size == 0u) {
        return FlashOperationCode::InvalidArgument;
    }
    if (sector_offset % flash_partition::sector_size != 0u ||
        page_offset % flash_partition::page_size != 0u ||
        page_size != flash_partition::page_size ||
        reinterpret_cast<std::uintptr_t>(page) %
                flash_partition::page_size !=
            0u) {
        return FlashOperationCode::InvalidAlignment;
    }
    if (!valid_erase_range(
            sector_offset, flash_partition::sector_size) ||
        !valid_program_page(page_offset, page, page_size) ||
        page_offset < sector_offset ||
        page_offset - sector_offset >
            flash_partition::sector_size -
                flash_partition::page_size) {
        return FlashOperationCode::InvalidRange;
    }
    return execute_raw_operation(
        sector_offset,
        flash_partition::sector_size,
        page_offset,
        page,
        page_size);
}

FlashOperationResult
FlashOperationTransaction::execute_raw_operation(
    const std::uint32_t erase_offset,
    const std::size_t erase_size,
    const std::uint32_t program_offset,
    const std::uint8_t *const program_data,
    const std::size_t program_size) noexcept
{
    const std::uint32_t remaining_ms =
        remaining_timeout_ms(deadline_us_);
    if (remaining_ms == 0u) {
        return {
            FlashOperationCode::TimedOut,
            FlashMutationDisposition::NotAttempted,
            true,
        };
    }
    FlashRawOperation operation{
        erase_offset,
        erase_size,
        program_offset,
        program_data,
        program_size,
    };

    if (pre_scheduler_) {
        if (get_core_num() != 0u ||
            xPortIsInsideInterrupt() != pdFALSE) {
            return FlashOperationCode::LockUnavailable;
        }
        const std::uint32_t saved_interrupt_state =
            save_and_disable_interrupts();
        flash_operation_raw_callback(&operation);
        restore_interrupts(saved_interrupt_state);
        const FlashMutationDisposition mutation =
            operation.callback_completed
                ? FlashMutationDisposition::Applied
                : operation.callback_entered
                      ? FlashMutationDisposition::Unknown
                      : FlashMutationDisposition::NotAttempted;
        reconciliation_read_available_ =
            reconciliation_read_available_ ||
            mutation != FlashMutationDisposition::NotAttempted;
        return {
            operation.callback_completed
                ? FlashOperationCode::Succeeded
                : FlashOperationCode::PlatformFailure,
            mutation,
            deadline_reached(deadline_us_),
        };
    }

    if (remaining_ms < 2u) {
        return {
            FlashOperationCode::TimedOut,
            FlashMutationDisposition::NotAttempted,
            true,
        };
    }
    const std::uint32_t coordination_phase_timeout_ms =
        remaining_ms / 2u;
    const int status = flash_safe_execute(
        flash_operation_raw_callback,
        &operation,
        coordination_phase_timeout_ms);

    const FlashMutationDisposition mutation =
        operation.callback_completed
            ? FlashMutationDisposition::Applied
            : operation.callback_entered &&
                      !operation.callback_completed
                  ? FlashMutationDisposition::Unknown
                  : FlashMutationDisposition::NotAttempted;
    reconciliation_read_available_ =
        reconciliation_read_available_ ||
        mutation != FlashMutationDisposition::NotAttempted;
    const bool deadline_exceeded =
        status == PICO_ERROR_TIMEOUT ||
        deadline_reached(deadline_us_);
    if (status == PICO_OK && operation.callback_completed) {
        return {
            FlashOperationCode::Succeeded,
            mutation,
            deadline_exceeded,
        };
    }
    if (status == PICO_ERROR_TIMEOUT) {
        return {
            FlashOperationCode::TimedOut,
            mutation,
            true,
        };
    }
    return {
        FlashOperationCode::PlatformFailure,
        mutation,
        deadline_exceeded,
    };
}

FlashOperationResult flash_operation_execute(
    const FlashOperationCallback callback,
    void *const context,
    const std::uint32_t timeout_ms) noexcept
{
    if (callback == nullptr ||
        timeout_ms == 0u ||
        timeout_ms == UINT32_MAX ||
        timeout_ms > MAXIMUM_FLASH_OPERATION_TIMEOUT_MS) {
        return FlashOperationCode::InvalidArgument;
    }
    const std::uint64_t start_us = time_us_64();
    const std::uint64_t timeout_us =
        static_cast<std::uint64_t>(timeout_ms) * 1000u;
    if (start_us >
        std::numeric_limits<std::uint64_t>::max() - timeout_us) {
        return FlashOperationCode::InvalidArgument;
    }
    const std::uint64_t deadline_us = start_us + timeout_us;

    if (xPortIsInsideInterrupt() != pdFALSE) {
        return FlashOperationCode::LockUnavailable;
    }

    const BaseType_t scheduler_state = xTaskGetSchedulerState();
    bool pre_scheduler_gate_acquired = false;
    bool mutex_acquired = false;
    if (scheduler_state == taskSCHEDULER_RUNNING) {
        if (g_flash_operation_mutex == nullptr) {
            return FlashOperationCode::LockUnavailable;
        }
        const std::uint32_t remaining_ms =
            remaining_timeout_ms(deadline_us);
        TickType_t wait_ticks = 0;
        if (!timeout_to_ticks(remaining_ms, wait_ticks)) {
            return {
                FlashOperationCode::TimedOut,
                FlashMutationDisposition::NotAttempted,
                true,
            };
        }
        if (xSemaphoreTake(
                g_flash_operation_mutex, wait_ticks) != pdTRUE) {
            return {
                FlashOperationCode::TimedOut,
                FlashMutationDisposition::NotAttempted,
                true,
            };
        }
        mutex_acquired = true;
    } else if (scheduler_state == taskSCHEDULER_NOT_STARTED) {
        if (get_core_num() != 0u) {
            return FlashOperationCode::LockUnavailable;
        }
        if (!acquire_pre_scheduler_gate(deadline_us)) {
            return {
                FlashOperationCode::TimedOut,
                FlashMutationDisposition::NotAttempted,
                true,
            };
        }
        pre_scheduler_gate_acquired = true;
    } else if (scheduler_state == taskSCHEDULER_SUSPENDED) {
        return FlashOperationCode::LockUnavailable;
    } else {
        return FlashOperationCode::LockUnavailable;
    }

    FlashOperationTransaction transaction(
        deadline_us, pre_scheduler_gate_acquired);
    FlashOperationResult result =
        callback(transaction, context);
    result.deadline_exceeded =
        result.deadline_exceeded ||
        deadline_reached(deadline_us);
    if (pre_scheduler_gate_acquired) {
        release_pre_scheduler_gate();
    } else if (
        mutex_acquired &&
        xSemaphoreGive(g_flash_operation_mutex) != pdTRUE) {
        result.code = FlashOperationCode::LockUnavailable;
    }
    return result;
}

} // namespace boot_v2
