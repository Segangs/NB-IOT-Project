#ifndef FLASH_LOGGER_HPP
#define FLASH_LOGGER_HPP

#include <stdint.h>
#include <stdbool.h>

// Raspberry Pi Pico 2 W (4MB Flash 기준)
// 마지막 64KB (0x3F0000 ~ 0x3FFFFF)를 로깅 데이터 영역으로 활용합니다.
#define FLASH_LOG_OFFSET    0x3F0000
#define FLASH_LOG_SIZE      0x010000 // 64KB
#define FLASH_SECTOR_SIZE   4096     // Erase unit
#define FLASH_PAGE_SIZE     256      // Program unit

// 32-byte aligned Log Entry Structure (Fits exactly 8 entries in a 256-byte page)
struct __attribute__((packed)) FlashLogEntry {
    uint32_t timestamp;   // Seconds since boot (or RTC epoch if available)
    float temperature;    // Measured external temperature (Celsius)
    float vsys_voltage;   // VSYS Input voltage (Volts)
    uint8_t send_status;  // HTTPS Send status (1: Success, 0: Failed/Postponed)
    uint8_t temp_status;  // Temperature sensor error status (0: OK, others: specific fault code)
    int16_t modem_status; // Modem registration/HTTP response error code (e.g. -1, 403, 500)
    int16_t system_error; // General system diagnostics error code (0: OK, others: error)
    uint8_t boot_reason;  // Boot reason code (0: Normal, 1: Cmd, 2: Watchdog, 3: Brown-out)
    char padding[13];     // Padding to ensure exactly 32-byte alignment
};

#ifdef __cplusplus
extern "C" {
#endif

// Initialize log storage. Scans flash to locate the next free write pointer.
void flash_log_init(void);

// Write a new log entry to flash memory.
void flash_log_write(float temp, float vsys, uint8_t sent, uint8_t temp_err, int16_t modem_err, int16_t sys_err);

// Set the baseline Unix epoch boot time for real-time stamp calculation.
void flash_log_set_boot_epoch(uint32_t epoch_offset);

// Print all stored logs in CSV format to standard output.
void flash_log_dump_csv(void);

// Clear all logged entries from flash memory.
void flash_log_clear(void);

#ifdef __cplusplus
}
#endif

#endif // FLASH_LOGGER_HPP
