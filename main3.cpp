#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

// 핀 정의
#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define PWR_ON_PIN 28

int main() {
    stdio_init_all(); // USB 시리얼 모니터링 초기화

    // 1. UART 설정
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // 2. PWR_ON 핀 설정
    gpio_init(PWR_ON_PIN);
    gpio_set_dir(PWR_ON_PIN, GPIO_OUT);
    gpio_put(PWR_ON_PIN, 1); // 평소엔 High 유지
    sleep_ms(500);

    // 3. 모뎀 전원 켜기 펄스 (Low 신호 인가)
    printf("HL7810 전원 켜는 중...\n");
    gpio_put(PWR_ON_PIN, 0);
    sleep_ms(1000); // 데이터시트 권장 시간 확인 필요
    gpio_put(PWR_ON_PIN, 1);
    
    sleep_ms(2000); // 모뎀 부팅 대기
    gpio_put(PWR_ON_PIN, 0);

    sleep_ms(10000); // 모뎀 부팅 대기

while (true) {
        // A. PC(USB) -> 모뎀(UART) 전달
        int pc_char = getchar_timeout_us(0); // PC 입력 확인 (Non-blocking)
        if (pc_char != PICO_ERROR_TIMEOUT) {
            uart_putc(UART_ID, (char)pc_char);
        }

        // B. 모뎀(UART) -> PC(USB) 전달
        if (uart_is_readable(UART_ID)) {
            char modem_char = uart_getc(UART_ID);
            putchar(modem_char);
        }
    }
}