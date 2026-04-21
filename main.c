#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"

#define UART_ID      uart0
#define BAUD_RATE    115200
#define UART_TX_PIN  0
#define UART_RX_PIN  1
#define PWR_PIN      14

void power_on_modem() {
    printf("[시스템] 모뎀 전원 버튼을 누릅니다...\n");
    gpio_init(PWR_PIN);
    gpio_set_dir(PWR_PIN, GPIO_OUT);

    // 2초간 누름
    gpio_put(PWR_PIN, 1); 
    sleep_ms(2000);
    gpio_put(PWR_PIN, 0);
    
    // 모뎀이 UART 응답을 할 수 있을 때까지만 살짝 대기 (너무 길면 기지국 찾다 죽음)
    printf("[시스템] 부팅 대기 중 (3초)...\n");
    sleep_ms(3000);
}

int main() {
    stdio_init_all();
    
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    sleep_ms(3000); 
    printf("\n=== Pico 2 W SIM7080G 세이프 모드 부팅 ===\n");

    // 1. 모뎀 전원 켜기
    power_on_modem();

    // 2. 비행기 모드 강제 진입 (여러 번 보내서 확실히 먹임)
    printf("[시스템] 비행기 모드(AT+CFUN=4) 강제 진입 시도...\n");
    for(int i = 0; i < 5; i++) {
        uart_puts(UART_ID, "AT+CFUN=4\r\n");
        sleep_ms(200); // 짧은 간격으로 연사
    }

    printf("[시스템] 설정 완료! 이제 수동으로 명령어를 입력하세요.\n");
    printf("[팁] 인터넷을 연결하려면 AT+CFUN=1 을 입력하세요 (전력 주의).\n\n");

    // 3. 패스스루 루프
    while (true) {
        // 맥북 -> 모뎀
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            uart_putc(UART_ID, (char)ch);
        }

        // 모뎀 -> 맥북
        if (uart_is_readable(UART_ID)) {
            char response = uart_getc(UART_ID);
            putchar(response);
        }
    }
}