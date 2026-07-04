#include "tasks_debug.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../lib/flash_logger.hpp"
#include "../lib/log.hpp"
#include "app_context.hpp"

static void debug_trim_command(char *cmd)
{
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t' ||
                       cmd[len - 1] == '\r' || cmd[len - 1] == '\n'))
    {
        cmd[--len] = '\0';
    }
}

static bool debug_command_equals(const char *cmd, const char *expected)
{
    while (*cmd != '\0' && *expected != '\0')
    {
        if (tolower((unsigned char)*cmd) != tolower((unsigned char)*expected))
        {
            return false;
        }
        cmd++;
        expected++;
    }
    return *cmd == '\0' && *expected == '\0';
}

// ====================================================================================
// Core 0 Task: Resource-Locked Interactive AT Command Bypass (Core 0)
// ====================================================================================
void vDebugTask(void *pvParameters)
{
    LOG("DEBUG_READY\n");
    
    char cmd_buf[64];
    int cmd_idx = 0;
    memset(cmd_buf, 0, sizeof(cmd_buf));
    
    while (true)
    {
        // 시리얼 입력 비동기 1문자 획득
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            char c = (char)ch;
            
            // 화면 에코백 (사용자 편의성 제공)
            putchar(c);
            
            if (c == '\r' || c == '\n') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';
                    debug_trim_command(cmd_buf);
                    
                    // 디버그 쉘 명령어 분기
                    if (cmd_buf[0] == '\0') {
                        // Ignore whitespace-only input.
                    } else if (debug_command_equals(cmd_buf, "dump_csv")) {
                        flash_log_dump_csv();
                    } else if (debug_command_equals(cmd_buf, "clear_csv")) {
                        flash_log_clear();
                    } else if (debug_command_equals(cmd_buf, "reboot")) {
                        safe_reboot(100);
                    } else if (debug_command_equals(cmd_buf, "power off") ||
                               debug_command_equals(cmd_buf, "poweroff") ||
                               debug_command_equals(cmd_buf, "power_off")) {
                        safe_power_off();
                    } else {
                        bool modem_busy = lcd_params.is_booting ||
                                          lcd_params.is_transmitting ||
                                          modem.is_connected() ||
                                          lcd_params.is_modem_busy;
                        if (modem_busy) {
                            LOG("AT_BUSY\n");
                        } else {
                            // 일반 AT 명령어일 경우 모뎀 UART에 전달
                            strcat(cmd_buf, "\r\n");
                            modem.modem_PacedWrite(cmd_buf);
                        }
                    }
                    cmd_idx = 0;
                    memset(cmd_buf, 0, sizeof(cmd_buf));
                }
            } else if (c == '\b' || ch == 127) { // 백스페이스
                if (cmd_idx > 0) {
                    cmd_idx--;
                    cmd_buf[cmd_idx] = '\0';
                }
            } else {
                if (cmd_idx < (int)sizeof(cmd_buf) - 2) {
                    cmd_buf[cmd_idx++] = c;
                }
            }
        }
        
        bool modem_busy = lcd_params.is_booting ||
                          lcd_params.is_transmitting ||
                          modem.is_connected() ||
                          lcd_params.is_modem_busy;
        if (!modem_busy) {
            // 모뎀의 실시간 출력 결과 파이프
            modem.modem_ReadResponse(0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Hyper-responsive 10ms polling interval
    }
}
