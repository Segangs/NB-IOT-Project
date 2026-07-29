#include "flash_logger.hpp"
#include "flash_operation_service.hpp"
#include "log.hpp"
#include <cstddef>
#include <cstdint>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/regs/addressmap.h"
#include <time.h>

#include "../boot_v2/flash_partition_layout.hpp"

// Access boot reason determined during main entry
extern int g_boot_reason_code;

// XIP_BASE is the memory mapped address for flash
namespace flash_partition = boot_v2::flash_partition;
static const uintptr_t flash_target_addr =
    XIP_BASE + flash_partition::sensor_log_offset;

static uint32_t g_write_offset = 0;
static bool g_initialized = false;
static uint32_t g_boot_epoch_offset = 0; // Baseline Unix time when system was booted
static constexpr int16_t POWER_ADAPTER_DIAGNOSTIC_MARKER = -707;
static constexpr uint32_t FLASH_OPERATION_TIMEOUT_MS = 2000u;

// Encodes Unix epoch time into decimal MMDDHHMMSS representation
static uint32_t epoch_to_mmddhhmmss(uint32_t epoch) {
    time_t rawtime = (time_t)epoch;
    struct tm *timeinfo = gmtime(&rawtime); // CCLK raw values parse straight to UTC tm
    if (timeinfo == nullptr) return 0;

    uint32_t month = timeinfo->tm_mon + 1;
    uint32_t day = timeinfo->tm_mday;
    uint32_t hour = timeinfo->tm_hour;
    uint32_t min = timeinfo->tm_min;
    uint32_t sec = timeinfo->tm_sec;

    return (month * 100000000) + (day * 1000000) + (hour * 10000) + (min * 100) + sec;
}

void flash_log_set_boot_epoch(uint32_t epoch_offset) {
    g_boot_epoch_offset = epoch_offset;
    LOG("FLASH_TIME_OK\n");
}

struct FlashLogInitContext {
    bool initialized_now{false};
    bool wrapped{false};
};

struct FlashLogScanResult {
    uint32_t next_write_offset{0};
    bool wrapped{true};
};

static boot_v2::FlashOperationResult scan_flash_log(
    boot_v2::FlashOperationTransaction &transaction,
    FlashLogScanResult &scan) noexcept
{
    const uint32_t max_entries =
        flash_partition::sensor_log_size / sizeof(FlashLogEntry);
    uint32_t next_write_offset = 0;
    bool wrapped = true;
    for (uint32_t i = 0; i < max_entries; ++i) {
        uint32_t timestamp = 0;
        const boot_v2::FlashOperationResult result =
            transaction.read(
                flash_partition::sensor_log_offset +
                    i * sizeof(FlashLogEntry),
                &timestamp,
                sizeof(timestamp));
        if (result != boot_v2::FlashOperationCode::Succeeded) {
            return result;
        }
        if (timestamp == UINT32_MAX) {
            next_write_offset = i * sizeof(FlashLogEntry);
            wrapped = false;
            break;
        }
    }

    scan.next_write_offset = next_write_offset;
    scan.wrapped = wrapped;
    return boot_v2::FlashOperationCode::Succeeded;
}

static boot_v2::FlashOperationResult flash_log_init_transaction(
    boot_v2::FlashOperationTransaction &transaction,
    void *const opaque_context) noexcept
{
    if (g_initialized) {
        return boot_v2::FlashOperationCode::Succeeded;
    }
    auto *const context =
        static_cast<FlashLogInitContext *>(opaque_context);
    if (context == nullptr) {
        return boot_v2::FlashOperationCode::InvalidArgument;
    }

    FlashLogScanResult scan{};
    const boot_v2::FlashOperationResult result =
        scan_flash_log(transaction, scan);
    if (result != boot_v2::FlashOperationCode::Succeeded) {
        return result;
    }

    g_write_offset = scan.next_write_offset;
    g_initialized = true;
    context->initialized_now = true;
    context->wrapped = scan.wrapped;
    return boot_v2::FlashOperationCode::Succeeded;
}

// Scan the flash to find the first unused slot (timestamp == 0xFFFFFFFF)
void flash_log_init(void) {
    FlashLogInitContext context{};
    const boot_v2::FlashOperationResult result =
        boot_v2::flash_operation_execute(
            flash_log_init_transaction,
            &context,
            FLASH_OPERATION_TIMEOUT_MS);
    if (result != boot_v2::FlashOperationCode::Succeeded) {
        LOG(
            "FLASH_LOG_INIT_FAIL code=%u mutation=%u deadline=%u\n",
            static_cast<unsigned>(result.code),
            static_cast<unsigned>(result.mutation),
            static_cast<unsigned>(result.deadline_exceeded));
        return;
    }
    if (context.initialized_now) {
        LOG(context.wrapped ? "FLASH_LOG_WRAP\n"
                            : "FLASH_LOG_READY\n");
    }
}

struct FlashLogWriteContext {
    FlashLogEntry entry{};
};

static boot_v2::FlashOperationResult flash_log_write_transaction(
    boot_v2::FlashOperationTransaction &transaction,
    void *const opaque_context) noexcept
{
    const auto *const context =
        static_cast<const FlashLogWriteContext *>(opaque_context);
    if (context == nullptr) {
        return boot_v2::FlashOperationCode::InvalidArgument;
    }

    uint32_t write_offset = g_write_offset;
    if (!g_initialized) {
        FlashLogScanResult scan{};
        const boot_v2::FlashOperationResult scan_result =
            scan_flash_log(transaction, scan);
        if (scan_result !=
            boot_v2::FlashOperationCode::Succeeded) {
            return scan_result;
        }
        write_offset = scan.next_write_offset;
    }
    if (write_offset >= flash_partition::sensor_log_size ||
        write_offset % sizeof(FlashLogEntry) != 0u) {
        write_offset = 0;
    }
    const uint32_t page_addr =
        write_offset & ~(flash_partition::page_size - 1u);
    const uint32_t entry_offset_in_page =
        write_offset - page_addr;
    const uint32_t flash_page_offset =
        flash_partition::sensor_log_offset + page_addr;

    alignas(flash_partition::page_size)
        uint8_t page_buffer[flash_partition::page_size];
    boot_v2::FlashOperationResult result =
        transaction.read(
            flash_page_offset,
            page_buffer,
            sizeof(page_buffer));
    if (result != boot_v2::FlashOperationCode::Succeeded) {
        return result;
    }

    const bool need_erase =
        write_offset % flash_partition::sector_size == 0u;
    if (need_erase) {
        memset(page_buffer, 0xFF, sizeof(page_buffer));
    }
    memcpy(
        page_buffer + entry_offset_in_page,
        &context->entry,
        sizeof(FlashLogEntry));

    if (need_erase) {
        LOG("FLASH_ERASE\n");
        result = transaction.replace_sector(
            flash_partition::sensor_log_offset + write_offset,
            flash_page_offset,
            page_buffer,
            sizeof(page_buffer));
    } else {
        result = transaction.program_page(
            flash_page_offset,
            page_buffer,
            sizeof(page_buffer));
    }
    if (result.mutation ==
        boot_v2::FlashMutationDisposition::Unknown) {
        g_initialized = false;
        return result;
    }
    if (result.mutation !=
        boot_v2::FlashMutationDisposition::Applied) {
        return result;
    }

    write_offset += sizeof(FlashLogEntry);
    if (write_offset >= flash_partition::sensor_log_size) {
        write_offset = 0;
    }
    g_initialized = true;
    g_write_offset = write_offset;
    return result;
}

void flash_log_write(float temp, float vsys, uint8_t sent, uint8_t temp_err, int16_t modem_err, int16_t sys_err) {
    FlashLogWriteContext context{};
    FlashLogEntry &new_entry = context.entry;
    uint32_t elapsed_sec = to_ms_since_boot(get_absolute_time()) / 1000;
    if (g_boot_epoch_offset > 0) {
        new_entry.timestamp = epoch_to_mmddhhmmss(g_boot_epoch_offset + elapsed_sec);
    } else {
        new_entry.timestamp = elapsed_sec; // Fallback to elapsed seconds before network sync
    }
    new_entry.temperature = temp;
    new_entry.vsys_voltage = vsys;
    new_entry.send_status = sent;
    new_entry.temp_status = temp_err;
    new_entry.modem_status = modem_err;
    new_entry.system_error = sys_err;
    new_entry.boot_reason = (uint8_t)g_boot_reason_code;
    memset(new_entry.padding, 0, sizeof(new_entry.padding));

    const boot_v2::FlashOperationResult result =
        boot_v2::flash_operation_execute(
            flash_log_write_transaction,
            &context,
            FLASH_OPERATION_TIMEOUT_MS);
    if (result != boot_v2::FlashOperationCode::Succeeded) {
        const char *const outcome =
            result.mutation ==
                    boot_v2::FlashMutationDisposition::Applied
                ? "APPLIED"
                : result.mutation ==
                          boot_v2::FlashMutationDisposition::Unknown
                      ? "UNCERTAIN"
                      : "FAIL";
        LOG(
            "FLASH_LOG_WRITE_%s code=%u mutation=%u deadline=%u\n",
            outcome,
            static_cast<unsigned>(result.code),
            static_cast<unsigned>(result.mutation),
            static_cast<unsigned>(result.deadline_exceeded));
    }
}

void flash_log_dump_csv(void) {
    LOG("\n=== START OF FLASH LOG CSV ===\n");
    LOG("DateTime(MMDDHHMMSS),Temperature(C),VSYS(V),SentStatus,TempStatus,ModemStatus,SystemError,BootReason\n");

    const FlashLogEntry *entries =
        (const FlashLogEntry *)flash_target_addr;
    uint32_t max_entries =
        flash_partition::sensor_log_size / sizeof(FlashLogEntry);
    uint32_t count = 0;

    for (uint32_t i = 0; i < max_entries; i++) {
        // Stop printing if we encounter unprogrammed flash (0xFFFFFFFF)
        if (entries[i].timestamp == 0xFFFFFFFF) {
            break;
        }

        LOG("%u,%.2f,%.2f,%u,%u,%d,%d,%u\n",
               entries[i].timestamp,
               entries[i].temperature,
               entries[i].vsys_voltage,
               entries[i].send_status,
               entries[i].temp_status,
               entries[i].modem_status,
               entries[i].system_error,
               entries[i].boot_reason);
        count++;
    }

    LOG("=== END OF FLASH LOG CSV (Total entries: %d) ===\n\n", count);
}

void flash_log_write_power_adapter_probe(
    const uint32_t falling_edges,
    const uint32_t rising_edges,
    const uint8_t flags,
    const int16_t recovery_us) {
    flash_log_write(
        static_cast<float>(falling_edges),
        static_cast<float>(rising_edges),
        1,
        flags,
        recovery_us,
        POWER_ADAPTER_DIAGNOSTIC_MARKER);
}

void flash_log_dump_power_adapter_probes(void) {
    const FlashLogEntry *entries =
        (const FlashLogEntry *)flash_target_addr;
    const uint32_t max_entries =
        flash_partition::sensor_log_size / sizeof(FlashLogEntry);
    uint32_t count = 0;

    LOG("POWER_PROBE_FLASH_BEGIN\n");
    for (uint32_t i = 0; i < max_entries; i++) {
        if (entries[i].timestamp == 0xFFFFFFFF) {
            break;
        }
        if (entries[i].system_error !=
            POWER_ADAPTER_DIAGNOSTIC_MARKER) {
            continue;
        }

        LOG(
            "POWER_PROBE_FLASH TS=%u FALL=%.0f RISE=%.0f "
            "FLAGS=0x%02X RECOVERY_US=%d BOOT=%u\n",
            entries[i].timestamp,
            entries[i].temperature,
            entries[i].vsys_voltage,
            entries[i].temp_status,
            entries[i].modem_status,
            entries[i].boot_reason);
        count++;
    }
    LOG("POWER_PROBE_FLASH_END COUNT=%u\n", count);
}

void flash_log_clear(void) {
    LOG("FLASH_CLEAR\n");

    const boot_v2::FlashOperationResult result =
        boot_v2::flash_operation_execute(
            [](boot_v2::FlashOperationTransaction &transaction,
               void *) noexcept {
                const boot_v2::FlashOperationResult erase_result =
                    transaction.erase_range(
                        flash_partition::sensor_log_offset,
                        flash_partition::sensor_log_size);
                if (erase_result.mutation ==
                    boot_v2::FlashMutationDisposition::Unknown) {
                    g_initialized = false;
                    return erase_result;
                }
                if (erase_result.mutation !=
                    boot_v2::FlashMutationDisposition::Applied) {
                    return erase_result;
                }
                g_write_offset = 0;
                g_initialized = true;
                return erase_result;
            },
            nullptr,
            FLASH_OPERATION_TIMEOUT_MS);
    if (result != boot_v2::FlashOperationCode::Succeeded) {
        const char *const outcome =
            result.mutation ==
                    boot_v2::FlashMutationDisposition::Applied
                ? "APPLIED"
                : result.mutation ==
                          boot_v2::FlashMutationDisposition::Unknown
                      ? "UNCERTAIN"
                      : "FAIL";
        LOG(
            "FLASH_CLEAR_%s code=%u mutation=%u deadline=%u\n",
            outcome,
            static_cast<unsigned>(result.code),
            static_cast<unsigned>(result.mutation),
            static_cast<unsigned>(result.deadline_exceeded));
        return;
    }
    LOG("FLASH_CLEAR_OK\n");
}
