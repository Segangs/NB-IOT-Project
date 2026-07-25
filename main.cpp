#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/powman.h"
#include "hardware/regs/powman.h"
#include "src/boot_v2/runtime_owner_rtos.hpp"
#include "src/config.h"
#include "src/tasks/app_context.hpp"
#include "src/tasks/tasks_boot.hpp"
#include "src/tasks/tasks_buzzer.hpp"
#include "src/tasks/tasks_debug.hpp"
#include "src/tasks/tasks_led.hpp"
#include "src/tasks/tasks_lcd.hpp"
#include "src/tasks/tasks_periodic_modem.hpp"
#include "src/tasks/tasks_sensor.hpp"
#include "src/tasks/tasks_sensor_reader.hpp"
#include "src/lib/flash_logger.hpp"
#include "src/lib/log.hpp"

// FreeRTOS standard headers
#include "FreeRTOS.h" 
#include "task.h"

void detect_boot_reason() {
    if (watchdog_caused_reboot()) {
        uint32_t magic = watchdog_hw->scratch[2];
        uint32_t cmd_id = watchdog_hw->scratch[3];
        
        // Clear scratch registers immediately so they don't persist on next random reboot
        watchdog_hw->scratch[2] = 0;
        watchdog_hw->scratch[3] = 0;
        
        if (magic == 0x12345678) {
            g_boot_reason_code = 1; // Cmd전송으로 인한 재부팅
            g_boot_cmd_id = cmd_id;
        } else {
            g_boot_reason_code = 2; // 오류로 인한 워치독 재부팅
            g_boot_cmd_id = 0;
        }
    } else {
        // Check POWMAN chip reset register
        uint32_t reset_reason = powman_hw->chip_reset;
        
        // Clear the POWMAN reset register (Write-1-to-Clear) to avoid stale values next boot
        powman_hw->chip_reset = reset_reason;
        
        // LOG("RESET_RAW 0x%08X\n", reset_reason);
        
        // If brown-out (HAD_BOR) or glitch (HAD_GLITCH_DETECT) bits are set, classify as Power Cut/Glitch (3)
        if (reset_reason & (POWMAN_CHIP_RESET_HAD_BOR_BITS | POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS)) {
            g_boot_reason_code = 3; // 전원 끊김/브라운아웃 재부팅
        } else {
            g_boot_reason_code = 0; // 정상 파워 온
        }
        g_boot_cmd_id = 0;
    }
    // LOG("BOOT_REASON %d,%d\n", g_boot_reason_code, g_boot_cmd_id);
}

TaskHandle_t xBootTaskHandle = NULL;

// ====================================================================================
// Main Execution Block (Core 0 Startup)
// ====================================================================================
int main()
{
    // Detect boot reason immediately before registers are modified
    detect_boot_reason();

    app_log_init();
    
    // Initialize Flash log storage
    flash_log_init();
    
    // 1. Initialize serial monitoring
    stdio_init_all();


    // LOG("BOOT_BANNER\n");
    
    // 2. Configure shared state initial parameters immediately
    // Set 'is_searching_network' and 'is_booting' to true to start boot screen instantly!
    lcd_params.is_booting = true; // ACTIVATE BOOT SCREEN MODE
    strcpy(lcd_params.status_text, "Booting...");
    lcd_params.current_temperature = -991.0f;
    lcd_params.current_temperature_ch1 = -991.0f;
    lcd_params.status_ch0 = 1;
    lcd_params.status_ch1 = 1;
    lcd_params.current_csq = 99;
    lcd_params.is_searching_network = true; // RUN SEARCH SEQUENCE INSTANTLY
    lcd_params.is_transmitting = false;
    lcd_params.is_modem_busy = false;
    lcd_params.is_battery_mode = false;
    
    // 3. Initialize temperature GPIOs and MCU diagnostic ADCs
    sensor_init();

    const boot_v2::RuntimeOwnerStartResult runtime_owner_start =
        boot_v2::runtime_owner_rtos_start();
    if (runtime_owner_start == boot_v2::RuntimeOwnerStartResult::Failed) {
        LOG("FATAL_RUNTIME_OWNER_TASK\n");
        while (true) {}
    }
    
    // 4. Register FreeRTOS Tasks
    // LcdTask has priority 2 (High) to drive fluent, non-stuttering screen animations
    xTaskCreate(
        vLcdTask,
        "LcdTask",
        768,
        &lcd_params,
        2,
        NULL
    );

    // Boot task executes checks in background without freezing LcdTask
    xTaskCreate(
        vBootTask,
        "BootTask",
        3072,
        NULL,
        1,
        &xBootTaskHandle
    );

    xTaskCreate(
        vSensorTask,
        "SensorTask",
        1536,
        NULL,
        1,
        NULL
    );

    // 6. Register non-blocking PCB LED controller task (Priority 1)
    xTaskCreate(
        vStatusLedTask,
        "StatusLedTask",
        768,
        NULL,
        1,
        NULL
    );

    // 7. Register resource-locked Interactive AT Command Bypass thread (Priority 1)
    xTaskCreate(
        vDebugTask,
        "DebugTask",
        1536,
        NULL,
        1,
        NULL
    );

    // 8. Register periodic modem communication controller task (Priority 1)
    xTaskCreate(
        vPeriodicModemTask,
        "PeriodicModemTask",
        3072,
        NULL,
        1,
        NULL
    );

    // 9. Register buzzer melody control task (Priority 1)
    xTaskCreate(
        vBuzzerTask,
        "BuzzerTask",
        1536,
        NULL,
        1,
        NULL
    );

    // 10. Register single low-priority USB log output task.
    xTaskCreate(
        vLogTask,
        "LogTask",
        1536,
        NULL,
        0,
        NULL
    );

    const boot_v2::RuntimeOwnerAtomicCutoverResult runtime_owner_cutover =
        boot_v2::runtime_owner_rtos_activate_atomic();
    if (runtime_owner_cutover !=
        boot_v2::RuntimeOwnerAtomicCutoverResult::Activated) {
        LOG("FATAL_RUNTIME_OWNER_CUTOVER\n");
        while (true) {}
    }

    // 11. Ignite FreeRTOS Scheduler instantly!
    vTaskStartScheduler();

    // Loop fallback
    while (true) {}
    return 0;
}

// ====================================================================================
// FreeRTOS Malloc and Stack Hooks
// ====================================================================================
extern "C" {

void vApplicationMallocFailedHook(void)
{
    LOG("FATAL_MALLOC\n");
    while (true) {}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    LOG("FATAL_STACK %s\n", pcTaskName);
    while (true) {}
}

} // extern "C"
