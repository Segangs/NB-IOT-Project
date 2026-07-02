#include "tasks_debug.hpp"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../lib/flash_logger.hpp"
#include "app_context.hpp"

// ====================================================================================
// Core 0 Task: Resource-Locked Interactive AT Command Bypass (Core 0)
// ====================================================================================
void vDebugTask(void *pvParameters)
{
    printf("[DebugTask] 디버깅용 실시간 AT 바이패스 스레드 가동 완료.\n");
    printf("[DebugTask] 로그 관리 명령어: 'dump_csv' (CSV 출력), 'clear_csv' (로그 청소)\n");
    
    char cmd_buf[64];
    int cmd_idx = 0;
    memset(cmd_buf, 0, sizeof(cmd_buf));
    
    while (true)
    {
        // 🚨 시리얼 가로채기 차단: 모뎀이 부팅 중, 통신 중, 또는 소켓 연결 중일 때는 
        // 전송 및 수신 버퍼 Race Condition 방지를 위해 대화식 디버그를 일시 중지(Yield)합니다.
        if (lcd_params.is_booting || lcd_params.is_transmitting || modem.is_connected() || lcd_params.is_modem_busy) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Yield for 100ms
            continue;
        }
        
        // 시리얼 입력 비동기 1문자 획득
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            char c = (char)ch;
            
            // 화면 에코백 (사용자 편의성 제공)
            putchar(c);
            
            if (c == '\r' || c == '\n') {
                if (cmd_idx > 0) {
                    cmd_buf[cmd_idx] = '\0';
                    
                    // 디버그 쉘 명령어 분기
                    if (strcmp(cmd_buf, "dump_csv") == 0) {
                        flash_log_dump_csv();
                    } else if (strcmp(cmd_buf, "clear_csv") == 0) {
                        flash_log_clear();
                    } else {
                        // 일반 AT 명령어일 경우 모뎀 UART에 전달
                        strcat(cmd_buf, "\r\n");
                        modem.modem_PacedWrite(cmd_buf);
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
        
        // 모뎀의 실시간 출력 결과 파이프
        modem.modem_ReadResponse(0);
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Hyper-responsive 10ms polling interval
    }
}


