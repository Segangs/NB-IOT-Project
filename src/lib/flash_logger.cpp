#include "flash_logger.hpp"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
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
    printf("[FlashLogger] Boot epoch offset initialized to: %u\n", g_boot_epoch_offset);
}

// 💡 RAM에서 직접 실행되어야 하는 안전한 저수준 플래시 I/O 래퍼 함수들
// XIP 모드 하에서 플래시 쓰기/지우기 시 명령 페치 충돌을 방지합니다.
static void __no_inline_not_in_flash_func(safe_flash_erase)(uint32_t offset, size_t size) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, size);
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(safe_flash_program)(uint32_t offset, const uint8_t *data, size_t size) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, size);
    restore_interrupts(ints);
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
            printf("[FlashLogger] Initialized. Next write offset: 0x%08X (Index: %d)\n", g_write_offset, i);
            return;
        }
    }

    // If no free slot found, wrap around to 0
    g_write_offset = 0;
    g_initialized = true;
    printf("[FlashLogger] Initialized (Full). Next write offset wrapped to 0x00000000\n");
}

void flash_log_write(float temp, float vsys, uint8_t sent, uint8_t ntc_err, int16_t modem_err, int16_t sys_err) {
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
    new_entry.ntc_status = ntc_err;
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

    // 🚨 FreeRTOS 스케줄러 일시 정지하여 컨텍스트 스위칭 완전 차단
    vTaskSuspendAll();

    // Check if we are at the start of a sector. If so, erase the sector (4KB) first.
    if ((g_write_offset % FLASH_SECTOR_SIZE) == 0) {
        printf("[FlashLogger] Erasing sector at offset: 0x%08X\n", g_write_offset);
        safe_flash_erase(FLASH_LOG_OFFSET + g_write_offset, FLASH_SECTOR_SIZE);
    }

    // Program the page back to Flash (Uses safe RAM-based wrapper)
    safe_flash_program(FLASH_LOG_OFFSET + page_addr, page_buffer, FLASH_PAGE_SIZE);

    // FreeRTOS 스케줄러 복구
    xTaskResumeAll();

    // Advance write offset
    g_write_offset += sizeof(FlashLogEntry);
    if (g_write_offset >= FLASH_LOG_SIZE) {
        g_write_offset = 0; // Wrap around (circular buffer)
    }
}

void flash_log_dump_csv(void) {
    printf("\n=== START OF FLASH LOG CSV ===\n");
    printf("DateTime(MMDDHHMMSS),Temperature(C),VSYS(V),SentStatus,NtcStatus,ModemStatus,SystemError,BootReason\n");

    const FlashLogEntry *entries = (const FlashLogEntry *)FLASH_TARGET_ADDR;
    uint32_t max_entries = FLASH_LOG_SIZE / sizeof(FlashLogEntry);
    uint32_t count = 0;

    for (uint32_t i = 0; i < max_entries; i++) {
        // Stop printing if we encounter unprogrammed flash (0xFFFFFFFF)
        if (entries[i].timestamp == 0xFFFFFFFF) {
            break;
        }

        printf("%u,%.2f,%.2f,%u,%u,%d,%d,%u\n", 
               entries[i].timestamp,
               entries[i].temperature,
               entries[i].vsys_voltage,
               entries[i].send_status,
               entries[i].ntc_status,
               entries[i].modem_status,
               entries[i].system_error,
               entries[i].boot_reason);
        count++;
    }

    printf("=== END OF FLASH LOG CSV (Total entries: %d) ===\n\n", count);
}

void flash_log_clear(void) {
    printf("[FlashLogger] Clearing all logs...\n");
    
    // FreeRTOS 스케줄러 일시 정지
    vTaskSuspendAll();
    
    safe_flash_erase(FLASH_LOG_OFFSET, FLASH_LOG_SIZE);
    
    // FreeRTOS 스케줄러 복구
    xTaskResumeAll();

    g_write_offset = 0;
    printf("[FlashLogger] Logs cleared successfully.\n");
}
