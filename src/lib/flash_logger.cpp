#include "flash_logger.hpp"
#include "log.hpp"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include <time.h>

// FreeRTOS headers
#include "FreeRTOS.h"
#include "task.h"

// Access boot reason determined during main entry
extern int g_boot_reason_code;

// XIP_BASE is the memory mapped address for flash
#define FLASH_TARGET_ADDR   (XIP_BASE + FLASH_LOG_OFFSET)

static uint32_t g_write_offset = 0;
static bool g_initialized = false;
static uint32_t g_boot_epoch_offset = 0; // Baseline Unix time when system was booted

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

// 💡 flash_safe_execute 콜백용 파라미터 구조체 및 RAM 실행 함수
// flash_safe_execute()는 양쪽 코어를 안전하게 동기화한 뒤 콜백을 실행합니다.
// (기존 save_and_disable_interrupts는 현재 코어만 보호하여 듀얼코어 XIP 충돌 유발)
struct FlashOpParams {
    uint32_t erase_offset;
    size_t erase_size;
    bool do_erase;
    uint32_t program_offset;
    const uint8_t *program_data;
    size_t program_size;
};

static void __no_inline_not_in_flash_func(flash_op_callback)(void *param) {
    FlashOpParams *p = (FlashOpParams *)param;
    if (p->do_erase) {
        flash_range_erase(p->erase_offset, p->erase_size);
    }
    if (p->program_size > 0 && p->program_data != nullptr) {
        flash_range_program(p->program_offset, p->program_data, p->program_size);
    }
}

// Scan the flash to find the first unused slot (timestamp == 0xFFFFFFFF)
void flash_log_init(void) {
    if (g_initialized) return;

    g_write_offset = 0;
    const FlashLogEntry *entries = (const FlashLogEntry *)FLASH_TARGET_ADDR;
    uint32_t max_entries = FLASH_LOG_SIZE / sizeof(FlashLogEntry);

    for (uint32_t i = 0; i < max_entries; i++) {
        if (entries[i].timestamp == 0xFFFFFFFF) {
            g_write_offset = i * sizeof(FlashLogEntry);
            g_initialized = true;
            LOG("FLASH_LOG_READY\n");
            return;
        }
    }

    // If no free slot found, wrap around to 0
    g_write_offset = 0;
    g_initialized = true;
    LOG("FLASH_LOG_WRAP\n");
}

void flash_log_write(float temp, float vsys, uint8_t sent, uint8_t temp_err, int16_t modem_err, int16_t sys_err) {
    if (!g_initialized) {
        flash_log_init();
    }

    // Safety check for offset alignment
    if (g_write_offset >= FLASH_LOG_SIZE || (g_write_offset % sizeof(FlashLogEntry)) != 0) {
        g_write_offset = 0;
    }

    // Prepare the new log entry
    FlashLogEntry new_entry;
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

    // Page-buffered Write (flash_range_program requires 256-byte aligned writes)
    uint32_t page_addr = g_write_offset & ~(FLASH_PAGE_SIZE - 1);
    uint32_t entry_offset_in_page = g_write_offset - page_addr;

    uint8_t page_buffer[FLASH_PAGE_SIZE];
    // Copy existing flash content of this page to buffer
    memcpy(page_buffer, (const void *)(FLASH_TARGET_ADDR + page_addr), FLASH_PAGE_SIZE);
    // Overwrite the specific log entry slot in the buffer
    memcpy(page_buffer + entry_offset_in_page, &new_entry, sizeof(FlashLogEntry));

    // 🚨 flash_safe_execute로 양쪽 코어를 안전하게 동기화한 뒤 플래시 I/O 수행
    bool need_erase = (g_write_offset % FLASH_SECTOR_SIZE) == 0;
    if (need_erase) {
        LOG("FLASH_ERASE\n");
    }

    FlashOpParams wp;
    wp.erase_offset = FLASH_LOG_OFFSET + g_write_offset;
    wp.erase_size = FLASH_SECTOR_SIZE;
    wp.do_erase = need_erase;
    wp.program_offset = FLASH_LOG_OFFSET + page_addr;
    wp.program_data = page_buffer;
    wp.program_size = FLASH_PAGE_SIZE;

    flash_safe_execute(flash_op_callback, &wp, UINT32_MAX);

    // Advance write offset
    g_write_offset += sizeof(FlashLogEntry);
    if (g_write_offset >= FLASH_LOG_SIZE) {
        g_write_offset = 0; // Wrap around (circular buffer)
    }
}

void flash_log_dump_csv(void) {
    LOG("\n=== START OF FLASH LOG CSV ===\n");
    LOG("DateTime(MMDDHHMMSS),Temperature(C),VSYS(V),SentStatus,TempStatus,ModemStatus,SystemError,BootReason\n");

    const FlashLogEntry *entries = (const FlashLogEntry *)FLASH_TARGET_ADDR;
    uint32_t max_entries = FLASH_LOG_SIZE / sizeof(FlashLogEntry);
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

void flash_log_clear(void) {
    LOG("FLASH_CLEAR\n");

    FlashOpParams wp;
    wp.erase_offset = FLASH_LOG_OFFSET;
    wp.erase_size = FLASH_LOG_SIZE;
    wp.do_erase = true;
    wp.program_offset = 0;
    wp.program_data = nullptr;
    wp.program_size = 0;

    flash_safe_execute(flash_op_callback, &wp, UINT32_MAX);

    g_write_offset = 0;
    LOG("FLASH_CLEAR_OK\n");
}
