#include "tasks_modem.hpp" 
#include "../config.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// FreeRTOS support
#include "FreeRTOS.h"
#include "task.h"

// EMQX Default Root CA certificate for self-signed broker verification (885 bytes DER)
static const uint8_t EMQX_ROOT_CA_CERT_DER[] = {
    0x30, 0x82, 0x03, 0x71, 0x30, 0x82, 0x02, 0x59, 0xA0, 0x03, 0x02, 0x01, 
    0x02, 0x02, 0x14, 0x65, 0x04, 0x89, 0x16, 0xC3, 0xEF, 0x31, 0x23, 0xCE, 
    0x1A, 0xD5, 0x16, 0xD2, 0x91, 0x13, 0xF1, 0x4C, 0x65, 0xA3, 0xAD, 0x30, 
    0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B, 
    0x05, 0x00, 0x30, 0x40, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 
    0x06, 0x13, 0x02, 0x53, 0x45, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 
    0x04, 0x08, 0x0C, 0x09, 0x53, 0x74, 0x6F, 0x63, 0x6B, 0x68, 0x6F, 0x6C, 
    0x6D, 0x31, 0x0C, 0x30, 0x0A, 0x06, 0x03, 0x55, 0x04, 0x0A, 0x0C, 0x03, 
    0x45, 0x4D, 0x51, 0x31, 0x0F, 0x30, 0x0D, 0x06, 0x03, 0x55, 0x04, 0x03, 
    0x0C, 0x06, 0x52, 0x6F, 0x6F, 0x74, 0x43, 0x41, 0x30, 0x1E, 0x17, 0x0D, 
    0x32, 0x36, 0x30, 0x33, 0x30, 0x38, 0x30, 0x39, 0x33, 0x38, 0x34, 0x36, 
    0x5A, 0x17, 0x0D, 0x33, 0x31, 0x30, 0x33, 0x30, 0x37, 0x30, 0x39, 0x33, 
    0x38, 0x34, 0x36, 0x5A, 0x30, 0x40, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03, 
    0x55, 0x04, 0x06, 0x13, 0x02, 0x53, 0x45, 0x31, 0x12, 0x30, 0x10, 0x06, 
    0x03, 0x55, 0x04, 0x08, 0x0C, 0x09, 0x53, 0x74, 0x6F, 0x63, 0x6B, 0x68, 
    0x6F, 0x6C, 0x6D, 0x31, 0x0C, 0x30, 0x0A, 0x06, 0x03, 0x55, 0x04, 0x0A, 
    0x0C, 0x03, 0x45, 0x4D, 0x51, 0x31, 0x0F, 0x30, 0x0D, 0x06, 0x03, 0x55, 
    0x04, 0x03, 0x0C, 0x06, 0x52, 0x6F, 0x6F, 0x74, 0x43, 0x41, 0x30, 0x82, 
    0x01, 0x22, 0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 
    0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x82, 0x01, 0x0F, 0x00, 0x30, 0x82, 
    0x01, 0x0A, 0x02, 0x82, 0x01, 0x01, 0x00, 0xF0, 0x43, 0xC6, 0xAA, 0x9B, 
    0xF3, 0x76, 0xF8, 0xDC, 0x3B, 0x58, 0xA5, 0x73, 0xCC, 0x42, 0x6B, 0x79, 
    0x6D, 0x4E, 0xA2, 0xC4, 0x8B, 0xEC, 0x56, 0x89, 0xAF, 0xB3, 0x91, 0x3F, 
    0xFF, 0xEC, 0x2A, 0x49, 0xF8, 0xE9, 0xB0, 0xC9, 0x9E, 0xEE, 0xD8, 0xF5, 
    0xBC, 0x2D, 0x0D, 0xFE, 0xA8, 0x8C, 0xDD, 0x98, 0xE5, 0xA3, 0x63, 0x59, 
    0xBC, 0xCC, 0x78, 0xE1, 0x42, 0xA5, 0xEC, 0x7E, 0xA6, 0xC6, 0xC4, 0xE5, 
    0x8B, 0xBC, 0x87, 0x3C, 0xEA, 0xEE, 0x14, 0xF5, 0x28, 0x0C, 0x2F, 0x1E, 
    0xB2, 0xDE, 0x91, 0xDE, 0x28, 0xFA, 0x9A, 0x63, 0x84, 0x51, 0xAB, 0xDD, 
    0x3D, 0x1D, 0xD7, 0x0D, 0x79, 0xEB, 0x5A, 0xDD, 0xDA, 0xB5, 0xC4, 0x02, 
    0xC4, 0xBF, 0x60, 0x13, 0x44, 0x78, 0x9E, 0x07, 0x3D, 0x99, 0xA6, 0xBC, 
    0x28, 0x80, 0x0A, 0x4E, 0xEB, 0x95, 0x3D, 0xDA, 0x55, 0x62, 0x3A, 0xBE, 
    0x69, 0x87, 0x96, 0xCD, 0x88, 0xDB, 0x44, 0xE8, 0x8A, 0xD1, 0xD3, 0xB7, 
    0xD9, 0x2B, 0x06, 0x20, 0x7D, 0xE9, 0x98, 0xC4, 0x53, 0xBA, 0x0E, 0x2B, 
    0x2E, 0xD0, 0x19, 0x10, 0x3F, 0xDE, 0x1A, 0x24, 0xDF, 0x77, 0x03, 0x3D, 
    0xFD, 0xF0, 0x29, 0xA6, 0xA8, 0x02, 0xF0, 0x6B, 0x7A, 0xD8, 0x8E, 0xDD, 
    0x93, 0x77, 0x2A, 0x68, 0xDF, 0x4F, 0xA4, 0x52, 0x0A, 0x1D, 0x0D, 0xCB, 
    0xDB, 0x82, 0xC0, 0xAE, 0x2A, 0x48, 0x1A, 0xBC, 0x80, 0xF1, 0x78, 0xAF, 
    0x61, 0x85, 0x62, 0xBE, 0x39, 0xD9, 0x94, 0xC9, 0xDF, 0x29, 0xD1, 0xD6, 
    0x26, 0x10, 0x59, 0x3C, 0x07, 0xF4, 0x33, 0x1B, 0x38, 0xD0, 0x81, 0xD4, 
    0xFB, 0xA9, 0x0E, 0x05, 0x48, 0xDF, 0xD1, 0x4C, 0x66, 0xA8, 0x00, 0x89, 
    0x45, 0xF4, 0x37, 0x2A, 0x60, 0xFB, 0x23, 0xAE, 0xA4, 0x0F, 0x36, 0xB1, 
    0x36, 0x76, 0xF2, 0xDE, 0x3F, 0x22, 0xC3, 0x8B, 0x59, 0xFA, 0x35, 0x02, 
    0x03, 0x01, 0x00, 0x01, 0xA3, 0x63, 0x30, 0x61, 0x30, 0x1D, 0x06, 0x03, 
    0x55, 0x1D, 0x0E, 0x04, 0x16, 0x04, 0x14, 0x8A, 0x42, 0x56, 0x4F, 0x30, 
    0x12, 0xBD, 0xE1, 0x94, 0x2B, 0x29, 0xD9, 0x23, 0xE6, 0x69, 0x07, 0xD5, 
    0x01, 0x0B, 0x5B, 0x30, 0x1F, 0x06, 0x03, 0x55, 0x1D, 0x23, 0x04, 0x18, 
    0x30, 0x16, 0x80, 0x14, 0x8A, 0x42, 0x56, 0x4F, 0x30, 0x12, 0xBD, 0xE1, 
    0x94, 0x2B, 0x29, 0xD9, 0x23, 0xE6, 0x69, 0x07, 0xD5, 0x01, 0x0B, 0x5B, 
    0x30, 0x0F, 0x06, 0x03, 0x55, 0x1D, 0x13, 0x01, 0x01, 0xFF, 0x04, 0x05, 
    0x30, 0x03, 0x01, 0x01, 0xFF, 0x30, 0x0E, 0x06, 0x03, 0x55, 0x1D, 0x0F, 
    0x01, 0x01, 0xFF, 0x04, 0x04, 0x03, 0x02, 0x01, 0x06, 0x30, 0x0D, 0x06, 
    0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B, 0x05, 0x00, 
    0x03, 0x82, 0x01, 0x01, 0x00, 0x10, 0x90, 0x34, 0x65, 0x16, 0xD2, 0xEA, 
    0x49, 0xDE, 0x16, 0x01, 0x72, 0xF6, 0xDC, 0xDE, 0x1B, 0x8B, 0xB9, 0xA5, 
    0x80, 0x84, 0x52, 0x31, 0xA2, 0xDE, 0xAB, 0x86, 0xBA, 0xAB, 0x87, 0xBB, 
    0x83, 0x71, 0x59, 0x58, 0xFE, 0x09, 0xFE, 0x68, 0xB9, 0xD6, 0xE4, 0x11, 
    0x4A, 0xD6, 0xA9, 0x7F, 0xB4, 0xF6, 0xB3, 0x67, 0x42, 0x2A, 0x0B, 0x30, 
    0x2A, 0x9C, 0xF4, 0x8B, 0x01, 0x93, 0xD4, 0xC8, 0x68, 0x3E, 0xCA, 0x22, 
    0x7F, 0x80, 0x41, 0x54, 0x0C, 0x2C, 0xA1, 0xE2, 0x3D, 0x63, 0xDA, 0xAE, 
    0x8E, 0xF3, 0xDE, 0x21, 0x0F, 0xE0, 0x8B, 0xF5, 0x84, 0x7A, 0xD3, 0xCE, 
    0x1C, 0x42, 0xFB, 0xC3, 0x49, 0x62, 0x9E, 0xFC, 0xEF, 0xF9, 0x8E, 0x60, 
    0x82, 0x43, 0xD2, 0x6B, 0x36, 0xF8, 0x18, 0x97, 0x31, 0x64, 0x81, 0xC0, 
    0x38, 0x12, 0xE8, 0xA3, 0xEE, 0xA7, 0x4D, 0x2F, 0xEF, 0x98, 0xFA, 0xDE, 
    0x83, 0xA6, 0xE5, 0xAA, 0xF1, 0x5A, 0x8A, 0x93, 0xF5, 0x9A, 0xDF, 0x2F, 
    0x0E, 0x9C, 0x58, 0x86, 0xB0, 0x70, 0xAF, 0x19, 0x30, 0x0F, 0xE3, 0x6C, 
    0x91, 0x5D, 0xF1, 0x69, 0x36, 0x21, 0xA7, 0x5E, 0x8A, 0x76, 0x03, 0xF4, 
    0xA5, 0x6C, 0x21, 0x47, 0xC2, 0x47, 0x54, 0x4B, 0x0B, 0xB8, 0x37, 0x20, 
    0xF1, 0x92, 0x38, 0x20, 0x23, 0xAB, 0xA5, 0x1B, 0x1E, 0xBB, 0x25, 0x4B, 
    0x79, 0x5F, 0x61, 0x09, 0x22, 0x6F, 0x46, 0x34, 0x24, 0x09, 0x23, 0x03, 
    0x46, 0x13, 0x33, 0x97, 0xA8, 0xFF, 0x96, 0xD0, 0x4F, 0x4F, 0x4B, 0x51, 
    0x3B, 0xC9, 0x01, 0x13, 0x93, 0xA0, 0xE5, 0x4A, 0x99, 0x26, 0xD1, 0x42, 
    0xA6, 0xD9, 0x47, 0xBE, 0x65, 0x8F, 0x97, 0xA0, 0xD5, 0xCD, 0x3D, 0x13, 
    0x4F, 0xD3, 0x26, 0xDA, 0xE0, 0xC8, 0x3E, 0xBF, 0x12, 0x78, 0x32, 0xF6, 
    0x28, 0xB5, 0xE2, 0x8F, 0xA1, 0x81, 0xA8, 0x1C, 0x84
};

// ====================================================================================
// Cooperative Sleep Helper: Performs non-blocking yields when FreeRTOS is active
// ====================================================================================
void modem_sleep(uint32_t ms)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
    {
        sleep_ms(ms);
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

nb_iot::nb_iot()
{
    buffer_idx = 0;
    is_socket_open = false;
    last_csq = 99;
    last_cereg = -1;
    http_session_id = 1;
    mqtt_session_id = 0;
    is_unauthenticated = false;

    memset(rx_buffer, 0, sizeof(rx_buffer));
    strcpy(device_imei, "0");
    strcpy(device_cimi, "0");
    strcpy(carrier_name, "Unknown");

    // Initialize UART0 using standard Pico SDK driver (proven 100% robust)
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // 강제 내부 풀업(Pull-up) 설정을 통해 플로팅 노이즈에 의한 모뎀 무한 ERROR 피드백 방지
    gpio_pull_up(UART_TX_PIN);
    gpio_pull_up(UART_RX_PIN);

    // Disable hardware flow control
    uart_set_hw_flow(UART_ID, false, false);

    // Initialize PWR_ON control pin
    gpio_init(PWR_ON_PIN);
    gpio_set_dir(PWR_ON_PIN, GPIO_OUT);
    gpio_put(PWR_ON_PIN, 1); // Set HIGH (inactive)
}

nb_iot::~nb_iot()
{
    uart_deinit(UART_ID);
}

void nb_iot::modem_ClearRxBuffer()
{
    // UART 하드웨어 FIFO 버퍼에 대기 중인 잔여 바이트들을 완전히 읽어내어 버림으로써 이전 명령의 URC/에러 찌꺼기 유입을 차단합니다.
    while (uart_is_readable(UART_ID))
    {
        (void)uart_getc(UART_ID);
    }
    memset(this->rx_buffer, 0, sizeof(this->rx_buffer));
    this->buffer_idx = 0;
}

void nb_iot::modem_SendCmd(const char *cmd)
{
    this->modem_ClearRxBuffer(); // Clear RX buffer before character write

    // Send AT command
    uart_puts(UART_ID, cmd);
    uart_puts(UART_ID, "\r");

    printf("[AT Tx] %s\n", cmd);
}

void nb_iot::modem_PacedWrite(const char *data)
{
    int len = strlen(data);
    for (int i = 0; i < len; i++)
    {
        uart_putc(UART_ID, data[i]);
        sleep_us(2000); // 2ms 정밀 페이싱 (UART 하드웨어 버퍼 오버런 완벽 차단)
    }
}

void nb_iot::modem_SendCmdUserInput()
{
    int pc_char = getchar_timeout_us(0); // PC 입력 확인 (Non-blocking)
    if (pc_char != PICO_ERROR_TIMEOUT)
    {
        uart_putc(UART_ID, (char)pc_char);
    }
}

// ====================================================================================
// [수정 포인트 1] 노이즈 필터링 위치 보정
// 문자열 수집 중 간헐적으로 튀는 하드웨어 Null(0x00) 및 0xFF 상시 필터링
// ====================================================================================
void nb_iot::modem_ReadResponse(int check)
{
    int max_bytes = 256; // 무한 루프 방지를 위해 한 번에 최대 256바이트만 수집
    int count = 0;

    while (uart_is_readable(UART_ID) && count < max_bytes)
    {
        count++;
        if (this->buffer_idx < 1023)
        {
            char response = uart_getc(UART_ID);

            // 인덱스 위치와 상관없이 시리얼 노이즈 데이터 상시 필터링
            if (response == 0x00 || (unsigned char)response == 0xFF)
            {
                continue;
            }

            // 버퍼가 비어있을 때 들어오는 선행 개행문자만 스킵
            if (this->buffer_idx == 0 && (response == '\r' || response == '\n'))
            {
                continue;
            }

            if (!check)
            {
                putchar(response);
            }

            this->rx_buffer[this->buffer_idx] = response;
            this->buffer_idx++;
            this->rx_buffer[this->buffer_idx] = '\0';
        }
        else
        {
            this->buffer_idx = 0; // Prevent buffer overrun
        }
    }
}

bool nb_iot::modem_SendCmdWaitResponse(const char *cmd, const char *expected_resp1, const char *expected_resp2, uint32_t timeout_ms)
{
    this->modem_SendCmd(cmd);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;

    while (elapsed < timeout_ms)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;

        this->modem_ReadResponse(0); // Echo to serial monitor

        if (expected_resp1 && strstr(this->rx_buffer, expected_resp1) != nullptr)
        {
            return true;
        }
        if (expected_resp2 && strstr(this->rx_buffer, expected_resp2) != nullptr)
        {
            return true;
        }

        // Fail early on generic ERROR unless the caller explicitly expects it
        if (strstr(this->rx_buffer, "ERROR") != nullptr)
        {
            if ((expected_resp1 && strstr(expected_resp1, "ERROR") != nullptr) ||
                (expected_resp2 && strstr(expected_resp2, "ERROR") != nullptr) ||
                (expected_resp1 && strstr(expected_resp1, "910") != nullptr) ||
                (expected_resp2 && strstr(expected_resp2, "910") != nullptr))
            {
                return true;
            }
            return false;
        }
    }
    return false;
}

bool nb_iot::modem_SendCmdWaitOK(const char *cmd, uint32_t timeout_ms, uint32_t post_delay_ms)
{
    return this->modem_SendCmdWaitResponse(cmd, "OK", nullptr, timeout_ms);
}

void nb_iot::modem_hw_power_on()
{
    printf("[System] 모뎀 하드웨어 부팅 시퀀스 개시...\n");

    // 1. 초기 상태: 확실히 HIGH(비활성)로 유지
    gpio_put(PWR_ON_PIN, 1);
    modem_sleep(1000);

    // 2. LOW로 1.5초(1500ms) 동안 유지하여 모뎀 켜기 트리거
    gpio_put(PWR_ON_PIN, 0);
    modem_sleep(1500);

    // 3. 다시 HIGH(비활성)로 복구하여 릴리즈
    gpio_put(PWR_ON_PIN, 1);
    
    printf("[System] 모뎀 OS 부팅 완료 대기 중 (30초)...\n");
    modem_sleep(30000);

    printf("[System] 모뎀 하드웨어 부팅 시퀀스 완료.\n");
}

bool nb_iot::modem_init(int &at_status, int &cpin_status)
{
    modem_hw_power_on();

    // ====================================================================================
    // [Baudrate Auto-Negotiation & 115200bps 상향 고정 시퀀스]
    // 115200bps 또는 9600bps 중 모뎀이 현재 어떤 속도로 설정되어 있든 감지해 115200bps로 통신을 동기화합니다.
    // ====================================================================================
    bool alive = false;
    
    // 1. 115200bps 접속 시도 (이미 모뎀이 115200bps로 설정되어 있는 경우 대비)
    printf("[System] 115200bps 속도로 모뎀 접속 테스트 시도...\n");
    uart_set_baudrate(UART_ID, 115200);
    this->modem_ClearRxBuffer();
    
    for (int i = 0; i < 2; i++)
    {
        if (check_at_alive())
        {
            alive = true;
            printf("[System] 모뎀이 115200bps로 응답했습니다. 정상 동기화 완료.\n");
            break;
        }
        modem_sleep(1000);
    }
    
    // 2. 115200bps 응답이 없는 경우 9600bps로 접속한 뒤 115200bps로 상향 재지정
    if (!alive)
    {
        printf("[System] 9600bps 속도로 모뎀 접속 시도 및 115200bps 강제 상향 개시...\n");
        uart_set_baudrate(UART_ID, 9600);
        this->modem_ClearRxBuffer();
        
        for (int i = 0; i < 3; i++)
        {
            if (check_at_alive())
            {
                alive = true;
                printf("[System] 모뎀이 9600bps로 접속되었습니다. 115200bps로 속도를 전환합니다...\n");
                
                // 모뎀 통신 속도 115200bps 변경 명령 송출
                this->modem_SendCmd("AT+IPR=115200");
                modem_sleep(500);
                
                // 피코 UART도 115200bps로 상향 조정
                uart_set_baudrate(UART_ID, 115200);
                modem_sleep(500);
                
                // 115200bps 속도 전환 정상 완료 여부 검증
                if (check_at_alive())
                {
                    printf("[System] 모뎀 115200bps 전환 검증 성공. 설정을 영구 저장(AT&W)합니다...\n");
                    this->modem_SendCmd("AT&W"); // 비휘발성 저장
                    modem_sleep(500);
                }
                else
                {
                    alive = false;
                }
                break;
            }
            modem_sleep(1000);
        }
    }

    at_status = alive ? 0 : 1;
    if (!alive)
    {
        printf("[Error] 모뎀이 응답하지 않습니다 (Baudrate 동기화 실패).\n");
        cpin_status = 1;
        return false;
    }

    // 1-1. ATE0 (Echo Off) 설정 및 2초 대기 (가장 먼저 전송!)
    printf("[System] ATE0 (Echo OFF) 전송 및 2초 대기...\n");
    modem_SendCmdWaitOK("ATE0", 3000, 2000);

    // Step 2: Enable detailed error reporting (+CMEE=1) 및 2초 대기
    modem_SendCmdWaitOK("AT+CMEE=1", 3000, 2000);

    // Step 2-1: 모뎀의 잔존 유령 HTTP 세션 일괄 강제 정리 (CME ERROR: 912 원천 차단)
    printf("[System] 모뎀 잔존 유령 HTTP 세션 강제 소거 개시 (1~10번)...\n");
    for (int sid = 1; sid <= 10; sid++)
    {
        char clean_cmd[64];
        snprintf(clean_cmd, sizeof(clean_cmd), "AT+KHTTPCLOSE=%d", sid);
        this->modem_SendCmdWaitResponse(clean_cmd, "OK", "910", 1000);
        modem_sleep(100);

        snprintf(clean_cmd, sizeof(clean_cmd), "AT+KHTTPDEL=%d", sid);
        this->modem_SendCmdWaitResponse(clean_cmd, "OK", "910", 1000);
        modem_sleep(100);
    }
    printf("[System] 모뎀 유령 HTTP 세션 소거 완료.\n");

    // Step 3: Turn on full modem capabilities and wait 30s as specified
    printf("[System] 모뎀 풀 기능 가동 중 (AT+CFUN=1) 후 30초 대기...\n");
    modem_SendCmdWaitOK("AT+CFUN=1", 5000, 30000);

    // Step 4: Validate SIM card status & wait 2s
    cpin_status = check_sim_status();
    modem_sleep(2000);

    // Step 5: Pull IMEI and wait 2s
    retrieve_imei(device_imei);
    modem_sleep(2000);

    // Step 6: Pull CIMI and wait 2s
    retrieve_cimi(device_cimi);
    modem_sleep(2000);

    // Supabase Root 2021 CA 인증서 주입 시퀀스
    printf("[System] SSL Root CA 인증서 주입 개시 (0번 슬롯)...\n");
    this->modem_SendCmdWaitResponse("AT+KCERTDELETE=0,0", "OK", "ERROR", 3000);
    modem_sleep(1000);

    char cert_cmd[64];
    int cert_len = sizeof(EMQX_ROOT_CA_CERT_DER);
    snprintf(cert_cmd, sizeof(cert_cmd), "AT+KCERTSTORE=0,%d,0", cert_len);

    if (this->modem_SendCmdWaitResponse(cert_cmd, "CONNECT", nullptr, 5000))
    {
        modem_sleep(200);
        this->modem_ClearRxBuffer();

        printf("[System] 인증서 데이터 스트림 안전 전송 시작 (%d 바이트)...\n", cert_len);
        for (int i = 0; i < cert_len; i++)
        {
            uart_putc(UART_ID, EMQX_ROOT_CA_CERT_DER[i]);
            sleep_us(200);
            if (i > 0 && i % 200 == 0)
            {
                printf("[%d/%d 바이트 송신 완료]\n", i, cert_len);
            }
        }
        printf("\n[System] 데이터 본문 전송 완료! 1.5초 대기 후 최종 OK 수집...\n");
        modem_sleep(1500);

        uint32_t elapsed = 0;
        const uint32_t step_ms = 100;
        bool store_ok = false;
        while (elapsed < 5000)
        {
            modem_sleep(step_ms);
            elapsed += step_ms;
            this->modem_ReadResponse(0);
            if (strstr(this->rx_buffer, "OK") != nullptr)
            {
                store_ok = true;
                break;
            }
            if (strstr(this->rx_buffer, "ERROR") != nullptr)
            {
                break;
            }
        }
        if (store_ok)
        {
            printf("\n[System] SSL Root CA 인증서 주입 및 저장 완료 (Index 0)!\n");
        }
        else
        {
            printf("\n[System] 에러: SSL Root CA 인증서 주입 실패.\n");
        }
    }
    else
    {
        printf("[System] 에러: SSL Root CA 인증서 주입 프롬프트(CONNECT) 획득 실패.\n");
    }
    modem_sleep(2000);

    return (cpin_status == 0);
}

bool nb_iot::check_at_alive()
{
    this->modem_SendCmd("AT");
    modem_sleep(2000);
    this->modem_ReadResponse();
    return (strstr(this->rx_buffer, "OK") != nullptr);
}

int nb_iot::check_sim_status()
{
    this->modem_SendCmd("AT+CPIN?");
    modem_sleep(2000);
    this->modem_ReadResponse();
    return (strstr(this->rx_buffer, "+CPIN: READY") != nullptr) ? 0 : 1;
}

int nb_iot::check_rssi_csq()
{
    this->modem_SendCmd("AT+CSQ");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char *p = strstr(this->rx_buffer, "+CSQ:");
    if (p != nullptr)
    {
        int rssi = 99, ber = 99;
        if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) == 2)
        {
            last_csq = rssi;
            return rssi;
        }
    }
    last_csq = 99;
    return 99;
}

int nb_iot::check_network_registration()
{
    this->modem_SendCmd("AT+CEREG?");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char *p = strstr(this->rx_buffer, "+CEREG:");
    if (p != nullptr)
    {
        int n = 0, stat = -1;
        if (sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2 || sscanf(p, "+CEREG: %d", &stat) == 1)
        {
            last_cereg = stat;
            return stat;
        }
    }
    last_cereg = -1;
    return -1;
}

bool nb_iot::check_operator_name(char *oper_out, int max_len)
{
    // 1. 통신사 정보를 숫자(Numeric PLMN) 포맷으로 출력하도록 설정
    this->modem_SendCmd("AT+COPS=3,2");
    modem_sleep(500);
    this->modem_ReadResponse(); // OK 응답 소거

    // 2. 현재 가입/접속된 통신사 정보 조회
    this->modem_SendCmd("AT+COPS?");
    modem_sleep(1500);
    this->modem_ReadResponse();

    strcpy(oper_out, "Unknown");
    char *p = strstr(this->rx_buffer, "+COPS:");
    if (p != nullptr)
    {
        char *quote_start = strchr(p, '\"');
        if (quote_start != nullptr)
        {
            char *quote_end = strchr(quote_start + 1, '\"');
            if (quote_end != nullptr)
            {
                int len = quote_end - (quote_start + 1);
                if (len > 0)
                {
                    char plmn[32] = {0};
                    if (len < (int)sizeof(plmn))
                    {
                        memcpy(plmn, quote_start + 1, len);
                        plmn[len] = '\0';

                        // PLMN 코드를 친숙한 통신사 브랜드 명칭으로 치환
                        if (strcmp(plmn, "45005") == 0)
                        {
                            strncpy(oper_out, "SKT", max_len - 1);
                        }
                        else if (strcmp(plmn, "45008") == 0)
                        {
                            strncpy(oper_out, "KT", max_len - 1);
                        }
                        else if (strcmp(plmn, "45006") == 0 || strcmp(plmn, "45003") == 0)
                        {
                            strncpy(oper_out, "LGU+", max_len - 1);
                        }
                        else
                        {
                            // 알려지지 않은 국적/통신사는 PLMN 코드 그대로 저장
                            strncpy(oper_out, plmn, max_len - 1);
                        }
                        oper_out[max_len - 1] = '\0';
                        
                        strncpy(carrier_name, oper_out, sizeof(carrier_name) - 1);
                        carrier_name[sizeof(carrier_name) - 1] = '\0';
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool nb_iot::retrieve_imei(char *imei_out)
{
    strcpy(imei_out, "000000000000000");
    this->modem_SendCmd("AT+CGSN");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char digits[20];
    int digit_count = 0;
    memset(digits, 0, sizeof(digits));

    for (int i = 0; this->rx_buffer[i] != '\0'; i++)
    {
        if (this->rx_buffer[i] >= '0' && this->rx_buffer[i] <= '9')
        {
            digits[digit_count++] = this->rx_buffer[i];
            if (digit_count == 15)
            {
                digits[15] = '\0';
                strcpy(imei_out, digits);
                strcpy(this->device_imei, digits);
                return true;
            }
        }
        else if (digit_count > 0)
        {
            digit_count = 0;
            memset(digits, 0, sizeof(digits));
        }
    }
    return false;
}

bool nb_iot::retrieve_cimi(char *cimi_out)
{
    strcpy(cimi_out, "000000000000000");
    this->modem_SendCmd("AT+CIMI");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char digits[20];
    int digit_count = 0;
    memset(digits, 0, sizeof(digits));

    for (int i = 0; this->rx_buffer[i] != '\0'; i++)
    {
        if (this->rx_buffer[i] >= '0' && this->rx_buffer[i] <= '9')
        {
            digits[digit_count++] = this->rx_buffer[i];
            if (digit_count == 15)
            {
                digits[15] = '\0';
                strcpy(cimi_out, digits);
                strcpy(this->device_cimi, digits);
                return true;
            }
        }
        else if (digit_count > 0)
        {
            digit_count = 0;
            memset(digits, 0, sizeof(digits));
        }
    }
    return false;
}

bool nb_iot::modem_SocketOpen(const char *ip, const char *port)
{
    printf("[Socket] TCP 소켓 연결 프로세스 개시 (%s:%s)...\n", ip, port);
    this->modem_SendCmdWaitResponse("AT+KTCPCLOSE=1", "OK", "910", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitResponse("AT+KTCPDEL=1", "OK", "910", 5000);
    modem_sleep(1000);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+KCNXCFG=1,\"GPRS\",\"%s\"", APN_NAME);
    if (!this->modem_SendCmdWaitOK(cmd, 5000))
        return false;
    modem_sleep(1000);

    snprintf(cmd, sizeof(cmd), "AT+KTCPCFG=1,0,\"%s\",%s", ip, port);
    if (!this->modem_SendCmdWaitOK(cmd, 5000))
        return false;
    modem_sleep(1000);

    if (!this->modem_SendCmdWaitOK("AT+KTCPCNX=1", 10000))
        return false;

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool connection_established = false;
    this->modem_ClearRxBuffer();

    while (elapsed < 40000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, "+KTCP_IND: 1,1") != nullptr)
        {
            connection_established = true;
            break;
        }
        if (strstr(this->rx_buffer, "+KTCP_IND: 1,3") != nullptr || strstr(this->rx_buffer, "+KTCP_IND: 1,0") != nullptr)
        {
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!connection_established)
        return false;
    modem_sleep(1000);
    is_socket_open = true;
    return true;
}

bool nb_iot::modem_SocketSend(const char *data)
{
    if (!is_socket_open)
        return false;
    modem_sleep(1000);

    int data_len = strlen(data);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+KTCPSND=1,%d", data_len);
    if (!this->modem_SendCmdWaitResponse(cmd, "CONNECT", nullptr, 10000))
        return false;

    modem_sleep(5000);
    this->modem_PacedWrite(data);
    modem_sleep(1000);
    this->modem_PacedWrite("--EOF--Pattern--");

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool send_ok = false;
    while (elapsed < 10000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, "OK") != nullptr)
        {
            send_ok = true;
            break;
        }
        if (strstr(this->rx_buffer, "ERROR") != nullptr)
            break;
    }
    modem_sleep(1000);
    return send_ok;
}

void nb_iot::modem_SocketClose()
{
    if (!is_socket_open)
        return;
    this->modem_SendCmdWaitResponse("AT+KTCPCLOSE=1", "OK", "910", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitResponse("AT+KTCPDEL=1", "OK", "910", 5000);
    modem_sleep(1000);
    is_socket_open = false;
}

bool nb_iot::modem_HttpOpen(const char *host, const char *port)
{
    printf("[HTTPS] Supabase 직접 HTTPS 연동 시퀀스 개시 (%s:%s)...\n", host, port);
    char cnx_cmd[256];
    snprintf(cnx_cmd, sizeof(cnx_cmd), "AT+KCNXCFG=1,\"GPRS\",\"%s\"", APN_NAME);
    
    // GPRS 커넥션이 이미 활성화되어 있으면 ERROR가 발생하므로 실패를 리턴하지 않고 경고만 출력합니다.
    if (!this->modem_SendCmdWaitOK(cnx_cmd, 5000))
    {
        printf("[HTTPS] 경고: GPRS APN 설정 실패 (이미 활성화되었을 가능성 있음)\n");
    }
    modem_sleep(1000);

    if (!this->modem_SendCmdWaitOK("AT+KCNXPROFILE=1", 5000))
    {
        printf("[HTTPS] 경고: GPRS 프로필 활성화 실패 (이미 활성화되었을 가능성 있음)\n");
    }
    modem_sleep(1000);

    this->modem_SendCmd("AT+KCNXUP=1");
    uint32_t cnx_elapsed = 0;
    bool bearer_ok = false;

    while (cnx_elapsed < 20000)
    {
        modem_sleep(100);
        cnx_elapsed += 100;
        this->modem_ReadResponse(0);
        
        // 망 연결 성공 URC(+KCNX_IND: 1,1)가 확실히 수신될 때까지 안전하게 대기합니다.
        if (strstr(this->rx_buffer, "+KCNX_IND: 1,1") != nullptr)
        {
            bearer_ok = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }
    modem_sleep(2000);

    if (this->http_session_id > 0)
    {
        char close_cmd[64];
        snprintf(close_cmd, sizeof(close_cmd), "AT+KHTTPCLOSE=%d", this->http_session_id);
        this->modem_SendCmdWaitResponse(close_cmd, "OK", "910", 5000);
        modem_sleep(1000);

        char del_cmd[64];
        snprintf(del_cmd, sizeof(del_cmd), "AT+KHTTPDEL=%d", this->http_session_id);
        this->modem_SendCmdWaitResponse(del_cmd, "OK", "910", 5000);
        modem_sleep(1000);
    }

    this->modem_SendCmdWaitOK("AT+KSSLCFG=0,3", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitOK("AT+KSSLCRYPTO=0,8,3,25392,12,4,1,0", 5000);
    modem_sleep(1000);

    char cmd[384];
    snprintf(cmd, sizeof(cmd), "AT+KHTTPCFG=1,\"%s\",443,2,\"\",\"\",1,0,0", host);
    if (!this->modem_SendCmdWaitOK(cmd, 5000))
        return false;

    char *p = strstr(this->rx_buffer, "+KHTTPCFG:");
    if (p != nullptr)
    {
        int parsed_id = 0;
        if (sscanf(p, "+KHTTPCFG: %d", &parsed_id) == 1)
            this->http_session_id = parsed_id;
    }
    modem_sleep(1000);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool connected = false;
    char expected_urc[32], failure_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KHTTP_IND: %d,1", this->http_session_id);
    snprintf(failure_urc, sizeof(failure_urc), "+KHTTP_IND: %d,0", this->http_session_id);

    if (strstr(this->rx_buffer, expected_urc) != nullptr)
    {
        connected = true;
    }
    else
    {
        while (elapsed < 30000)
        {
            modem_sleep(step_ms);
            elapsed += step_ms;
            this->modem_ReadResponse(0);
            if (strstr(this->rx_buffer, expected_urc) != nullptr)
            {
                connected = true;
                break;
            }
            if (strstr(this->rx_buffer, failure_urc) != nullptr)
                break;
            if (this->buffer_idx > 800)
                this->modem_ClearRxBuffer();
        }
    }

    if (!connected)
        return false;
    modem_sleep(1000);
    is_socket_open = true;
    return true;
}

bool nb_iot::modem_HttpPost(const char *request_uri, const char *payload, char *response_buf, size_t response_buf_len)
{
    if (!is_socket_open) return false;

    // 1. 보낼 JSON 본문의 길이를 바이트 단위로 정확히 계산
    int payload_len = strlen(payload); 

    printf("[HTTPS] Send Payload Body: %s (Length: %d)\n", payload, payload_len);
    printf("[HTTPS] HTTP 헤더 주입 개시 (AT+KHTTPHEADER=%d)...\n", this->http_session_id);

    char header_cmd[64];
    snprintf(header_cmd, sizeof(header_cmd), "AT+KHTTPHEADER=%d", this->http_session_id);
    if (!this->modem_SendCmdWaitResponse(header_cmd, "CONNECT", nullptr, 5000)) return false;
    modem_sleep(200);

    // 2. 헤더에 Content-Length 및 Prefer: return=representation 추가!!
    char header_buf[512];
    snprintf(header_buf, sizeof(header_buf),
             "apikey: %s\r\n"
             "Authorization: Bearer %s\r\n"
             "Content-Type: application/json\r\n"
             "Prefer: return=representation\r\n"
             "Content-Length: %d\r\n\r\n",
             SUPABASE_ANON_KEY, SUPABASE_ANON_KEY, payload_len);

    printf("[HTTPS] 헤더 주입 송출:\n%s", header_buf);
    this->modem_PacedWrite(header_buf);
    modem_sleep(200);

    // 3. 헤더 마감 패턴 송출
    this->modem_PacedWrite("--EOF--Pattern--");
    modem_sleep(500);


    // 종료 패턴 역시 모뎀이 처리할 수 있도록 대기 추가
    modem_sleep(1000);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool header_ok = false;
    while (elapsed < 5000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, "OK") != nullptr)
        {
            header_ok = true;
            break;
        }
    }

    if (!header_ok)
    {
        printf("[HTTPS] 에러: HTTP 헤더 완료(OK) 수신 실패.\n");
        return false;
    }
    modem_sleep(1000);

    // // 디버그: 주입된 헤더 현황 확인
    // printf("[HTTPS] 주입된 헤더 현황 조회 (AT+KHTTPHEADER?)... \n");
    // this->modem_SendCmd("AT+KHTTPHEADER?");
    // modem_sleep(1000);
    // this->modem_ReadResponse(0);
    // modem_sleep(1000);

    // Step 2: HTTP POST 요청 개시 (포맷을 0으로 설정하여 무의미한 헤더 덤프를 차단하고 상태 라인만 간결히 수신)
    printf("[HTTPS] HTTP POST 요청 개시 (Endpoint: %s, Session: %d)...\n", request_uri, this->http_session_id);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+KHTTPPOST=%d,,\"%s\",0", this->http_session_id, request_uri);

    if (!this->modem_SendCmdWaitResponse(cmd, "CONNECT", nullptr, 10000))
    {
        printf("[HTTPS] 에러: HTTP POST 요청 프롬프트(CONNECT) 획득 실패.\n");
        return false;
    }

    // modem_sleep(200);

    // printf("[HTTPS] Payload 본문 송출\n");
    // this->modem_PacedWrite(payload);
    // modem_sleep(100);

    // printf("[HTTPS] --EOF--Pattern-- 전송\n");
    // this->modem_PacedWrite("--EOF--Pattern--");

    // // ====================================================================================
    // // [수정 포인트 3] 페이로드 송출 직후 수신 버퍼 조기 소거(Clear) 코드 제거 및 수신 조건 개선
    // // 모뎀이 응답 URC(+KHTTP_IND)를 띄우기도 전에 버퍼를 지우던 치명적 버그 수정
    // // ====================================================================================
    // elapsed = 0;
    // bool post_ok = false;
    // bool response_status_2xx = false;

    // 데이터 모드 진입 안착을 위한 200ms 정밀 대기
    modem_sleep(200);
    
    // ====================================================================================
    // [변경 부분] Payload 본문 송출 시 시리얼 모니터에 데이터 실시간 출력 시각화
    // ====================================================================================
    printf("[HTTPS] >>> Payload 본문 송출 시작 >>>\n");
    
    printf("%s\n", payload); // 시리얼 모니터에 MCU가 보낼 JSON 본문을 그대로 출력 
    // 모뎀으로 실제 데이터 전송
    //this->modem_PacedWrite(payload);
    char total_packet[1024];
// JSON 본문 바로 뒤에 공백/개행 없이 EOF 패턴을 붙여버림
snprintf(total_packet, sizeof(total_packet), "%s--EOF--Pattern--", payload);

// 모뎀으로 통째로 한 번에 전송
this->modem_PacedWrite(total_packet);
    //modem_sleep(100);
    
    // 본문 전송 완료 (EOF 패턴 전송)
    
    //this->modem_PacedWrite("--EOF--Pattern--");
    printf("[HTTPS] [Tx Pattern] --EOF--Pattern--\n"); // 패턴 송출 현황 시각화
    
    // ====================================================================================
    
    // OK 및 HTTP 전송 완료 URC 대기 (이하 원본 로직 유지)
    elapsed = 0;
    bool post_ok = false;
    bool response_status_2xx = false;








    // ★ 기존 원본에 있던 이 위치의 this->modem_ClearRxBuffer() 제거 완료!
    // (서버가 보낸 패킷이 읽히기도 전에 삭제되는 현상 방지)

    char status_prefix[32];
    snprintf(status_prefix, sizeof(status_prefix), "+KHTTP_IND: %d,3,", this->http_session_id);

    uint32_t elapsed_ms = 0;
    while (elapsed_ms < 15000)
    {
        modem_sleep(2); // 2ms polling sleep to prevent overflow
        elapsed_ms += 2;
        this->modem_ReadResponse(0);

        // [Fast Pass] 서버의 2xx 응답 헤더(HTTP/1.1 200, 201, 204 등)가 감지되면 즉시 성공 처리 후 탈출합니다.
        if (strstr(this->rx_buffer, "HTTP/1.1 204") != nullptr)
        {
            response_status_2xx = true;
            post_ok = true; 
            printf("\n[HTTPS] HTTP 204 No Content 감지 성공 (Fast Pass)!\n");
            break;
        }
        else if (strstr(this->rx_buffer, "HTTP/1.1 2") != nullptr)
        {
            response_status_2xx = true;
            post_ok = true; 
            printf("\n[HTTPS] HTTP 2xx 응답 감지 성공 (Fast Pass)!\n");
            break;
        }

        if (strstr(this->rx_buffer, "OK") != nullptr)
        {
            post_ok = true;
        }

        char *urc_p = strstr(this->rx_buffer, status_prefix);
        if (urc_p != nullptr)
        {
            // status_prefix는 "+KHTTP_IND: <session_id>,3," 형태이므로, 
            // 그 뒤에는 "data_len,status_code,..."가 연결됩니다. (예: "191,204,\"No Content\"")
            char *data_len_start = urc_p + strlen(status_prefix);
            char *comma = strchr(data_len_start, ',');
            if (comma != nullptr)
            {
                // comma + 1 은 status_code의 시작점을 가리킵니다. (예: "204,\"No Content\"")
                int status_code = atoi(comma + 1);
                if (status_code >= 200 && status_code < 300)
                {
                    response_status_2xx = true;
                    post_ok = true; // 서버의 HTTP 성공 응답은 최종 데이터 송신 처리가 완료됨을 뜻하므로 post_ok 도 강제 승인합니다.
                    break;
                }
                else
                {
                    printf("\n[HTTPS] non-2xx URC (Status: %d). Waiting for dump...\n", status_code);
                    for (int f = 0; f < 1000; f++) {
                        modem_sleep(2);
                        this->modem_ReadResponse(0);
                    }
                    break;
                }
            }
        }

        if (this->buffer_idx > 950)
        {
            // 버퍼를 완전히 비우면 유입 중인 URC 문자열 중간이 찢겨나갈 수 있으므로,
            // 최근 150바이트만 남기고 앞으로 복사하여 URC 조각 유실을 방지합니다.
            memmove(this->rx_buffer, this->rx_buffer + 800, this->buffer_idx - 800);
            this->buffer_idx -= 800;
            this->rx_buffer[this->buffer_idx] = '\0';
        }
    }

    modem_sleep(1000);

    // 💡 [구제책] URC가 시리얼 노이즈로 인해 깨진 경우라도, 
    // Supabase의 정상 응답 데이터 형식(예: "sensorId" 또는 "cmd")이 버퍼에 이미 존재하고
    // 최종적으로 모뎀으로부터 "OK"를 수신했다면 성공으로 간주하여 구제합니다.
    if (!response_status_2xx && post_ok)
    {
        if (strstr(this->rx_buffer, "sensorId") != nullptr || strstr(this->rx_buffer, "cmd") != nullptr)
        {
            response_status_2xx = true;
            printf("\n[HTTPS] 경고: URC 찌그러짐/누락이 발생했으나, 수신 버퍼 내 Supabase 데이터 및 OK 매칭으로 성공 구제 완료!\n");
        }
    }

    if (post_ok && response_status_2xx)
    {
        printf("[HTTPS] Supabase 데이터 직접 적재 성공!\n");
        
        // Extract JSON response if buffer is provided
        if (response_buf && response_buf_len > 0)
        {
            response_buf[0] = '\0';
            const char *json_start = strchr(this->rx_buffer, '[');
            while (json_start != nullptr)
            {
                const char *json_end = strchr(json_start, ']');
                if (json_end != nullptr)
                {
                    size_t len = json_end - json_start + 1;
                    if (len < response_buf_len)
                    {
                        // Copy to temporary buffer to check
                        char temp[256];
                        strncpy(temp, json_start, len);
                        temp[len] = '\0';
                        
                        // JSON 형식이 수집되면 무조건 수집 (cmd 필터 완화)
                        strcpy(response_buf, temp);
                        printf("[HTTPS] Parsed response body: %s\n", response_buf);
                        break;
                    }
                    json_start = strchr(json_end + 1, '[');
                }
                else
                {
                    break;
                }
            }
        }

        this->modem_ClearRxBuffer(); // 프로세스 완료 후 버퍼 정리
        return true;
    }
    else
    {
        printf("[HTTPS] 에러: 데이터 송출 완료 혹은 HTTP 2xx 응답 확인 실패.\n");
        this->modem_ClearRxBuffer();
        return false;
    }
}

void nb_iot::modem_HttpClose()
{
    // 소켓이 비정상적으로 닫히거나 연결 도중 실패하여 is_socket_open이 false여도, 
    // http_session_id가 존재한다면 모뎀의 세션 누수 방지를 위해 정리 절차를 강제 수행합니다.
    if (!is_socket_open && this->http_session_id <= 0)
        return;

    printf("[HTTPS] HTTPS 세션 정리 개시...\n");
    char close_cmd[64];
    snprintf(close_cmd, sizeof(close_cmd), "AT+KHTTPCLOSE=%d", this->http_session_id);
    this->modem_SendCmdWaitResponse(close_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    char del_cmd[64];
    snprintf(del_cmd, sizeof(del_cmd), "AT+KHTTPDEL=%d", this->http_session_id);
    this->modem_SendCmdWaitResponse(del_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    is_socket_open = false;
    printf("[HTTPS] HTTPS 세션 정리 완료.\n");
}

uint32_t nb_iot::retrieve_network_time()
{
    this->modem_ClearRxBuffer();
    // CCLK 조회를 위해 AT+CCLK? 전송
    if (!this->modem_SendCmdWaitResponse("AT+CCLK?", "+CCLK:", nullptr, 2000)) {
        return 0;
    }
    
    // rx_buffer에서 "+CCLK:" 위치 찾기
    const char *pos = strstr(this->rx_buffer, "+CCLK:");
    if (!pos) return 0;
    
    pos = strchr(pos, '"');
    if (!pos) return 0;
    pos++; // 따옴표 내부 시각 문자열 시작
    
    int year, month, day, hour, minute, second;
    // yy/mm/dd,hh:mm:ss 파싱
    if (sscanf(pos, "%d/%d/%d,%d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
        struct tm t;
        t.tm_year = (year >= 70) ? (year) : (year + 100); // 26 -> 126 (2026년)
        t.tm_mon = month - 1; // 0-11
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = second;
        t.tm_isdst = -1;
        
        time_t epoch = mktime(&t);
        if (epoch != -1) {
            return (uint32_t)epoch;
        }
    }
    return 0;
}

bool nb_iot::modem_MqttOpen(const char *host, const char *port, const char *client_id, const char *username, const char *password)
{
    printf("[MQTTS] MQTTS 연동 시퀀스 개시 (%s:%s)...\n", host, port);
    this->is_unauthenticated = false;

    char cnx_cmd[256];
    snprintf(cnx_cmd, sizeof(cnx_cmd), "AT+KCNXCFG=1,\"GPRS\",\"%s\"", APN_NAME);
    
    if (!this->modem_SendCmdWaitOK(cnx_cmd, 5000))
    {
        printf("[MQTTS] 경고: GPRS APN 설정 실패 (이미 활성화되었을 가능성 있음)\n");
    }
    modem_sleep(1000);

    if (!this->modem_SendCmdWaitOK("AT+KCNXPROFILE=1", 5000))
    {
        printf("[MQTTS] 경고: GPRS 프로필 활성화 실패 (이미 활성화되었을 가능성 있음)\n");
    }
    modem_sleep(1000);

    this->modem_SendCmd("AT+KCNXUP=1");
    uint32_t cnx_elapsed = 0;
    bool bearer_ok = false;

    while (cnx_elapsed < 20000)
    {
        modem_sleep(100);
        cnx_elapsed += 100;
        this->modem_ReadResponse(0);
        
        if (strstr(this->rx_buffer, "+KCNX_IND: 1,1") != nullptr)
        {
            bearer_ok = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }
    modem_sleep(2000);

    // SSL 및 TLS 세션 재개(Session Resumption) 설정
    printf("[MQTTS] TLS 암호화 세션 재개 활성화 구성 중...\n");
    this->modem_SendCmdWaitOK("AT+KSSLCFG=0,3", 5000); // TLS 활성화
    modem_sleep(1000);
    this->modem_SendCmdWaitOK("AT+KSSLCFG=2,0", 5000); // Session Mode: 0 (Automatic session resumption)
    modem_sleep(1000);
    this->modem_SendCmdWaitOK("AT+KSSLCRYPTO=0,8,3,25392,12,4,1,0", 5000); // Cipher suites configuration
    modem_sleep(1000);

    // MQTT 세션 설정 (Clean session=1, ClientID, Username, Password, Cipher Index=0)
    char cfg_cmd[512];
    snprintf(cfg_cmd, sizeof(cfg_cmd), 
        "AT+KMQTTCFG=1,1,\"%s\",%s,4,\"%s\",120,1,0,\"\",\"\",0,0,\"%s\",\"%s\",0",
        host, port, client_id, username, password);

    if (!this->modem_SendCmdWaitOK(cfg_cmd, 5000))
    {
        printf("[MQTTS] 에러: MQTT 설정(KMQTTCFG) 명령 전송 실패\n");
        return false;
    }

    char *p = strstr(this->rx_buffer, "+KMQTTCFG:");
    if (p != nullptr)
    {
        int parsed_id = 0;
        if (sscanf(p, "+KMQTTCFG: %d", &parsed_id) == 1)
            this->mqtt_session_id = parsed_id;
    }
    else
    {
        // 폴백으로 세션 ID 1 지정
        this->mqtt_session_id = 1;
    }
    modem_sleep(1000);

    // MQTT 브로커 연결 시도
    char cnx_mqtt[64];
    snprintf(cnx_mqtt, sizeof(cnx_mqtt), "AT+KMQTTCNX=%d", this->mqtt_session_id);
    this->modem_SendCmd(cnx_mqtt);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool connected = false;
    char expected_urc[32], failure_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KMQTT_IND: %d,1", this->mqtt_session_id);
    snprintf(failure_urc, sizeof(failure_urc), "+KMQTT_IND: %d,0", this->mqtt_session_id);

    while (elapsed < 30000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        
        if (strstr(this->rx_buffer, expected_urc) != nullptr)
        {
            connected = true;
            break;
        }
        if (strstr(this->rx_buffer, failure_urc) != nullptr)
        {
            printf("[MQTTS] 🚨 연결 실패 URC 감지! 인증이 거부되었거나 연결 끊김.\n");
            this->is_unauthenticated = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!connected)
    {
        printf("[MQTTS] MQTTS 연결 실패 (타임아웃 또는 인증 거절)\n");
        return false;
    }

    printf("[MQTTS] MQTTS 브로커 연결 성공! (Session ID: %d)\n", this->mqtt_session_id);
    is_socket_open = true;
    return true;
}

bool nb_iot::modem_MqttPublish(const char *topic, const char *payload)
{
    if (!is_socket_open || this->mqtt_session_id == 0) return false;

    // 80바이트 페이로드 제한 방어
    int payload_len = strlen(payload);
    if (payload_len > 80)
    {
        printf("[MQTTS] ⚠️ 경고: 페이로드 크기(%d 바이트)가 모뎀의 80바이트 한도를 초과합니다!\n", payload_len);
        return false;
    }

    printf("[MQTTS] Publish 전송 토픽: %s | 페이로드: %s\n", topic, payload);

    char pub_cmd[512];
    // QoS 1로 전송 시도
    snprintf(pub_cmd, sizeof(pub_cmd), "AT+KMQTTPUB=%d,\"%s\",1,0,\"%s\"", this->mqtt_session_id, topic, payload);
    
    if (!this->modem_SendCmdWaitOK(pub_cmd, 5000))
    {
        printf("[MQTTS] 에러: Publish 명령어 실행 실패\n");
        return false;
    }

    // QoS 1 전송 성공 URC (+KMQTT_IND: <session_id>,3) 대기
    uint32_t elapsed = 0;
    bool pub_success = false;
    char expected_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KMQTT_IND: %d,3", this->mqtt_session_id);

    while (elapsed < 10000)
    {
        modem_sleep(100);
        elapsed += 100;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, expected_urc) != nullptr)
        {
            pub_success = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!pub_success)
    {
        printf("[MQTTS] 에러: Publish URC 수신 실패 (QoS 1 핸드셰이크 누락)\n");
        return false;
    }

    printf("[MQTTS] Publish 전송 성공 URC 확인 완료.\n");
    return true;
}

bool nb_iot::modem_MqttSubscribe(const char *topic)
{
    if (!is_socket_open || this->mqtt_session_id == 0) return false;

    printf("[MQTTS] Subscribe 신청 토픽: %s\n", topic);

    char sub_cmd[256];
    // QoS 1로 구독 신청
    snprintf(sub_cmd, sizeof(sub_cmd), "AT+KMQTTSUB=%d,\"%s\",1", this->mqtt_session_id, topic);

    if (!this->modem_SendCmdWaitOK(sub_cmd, 5000))
    {
        printf("[MQTTS] 에러: Subscribe 명령어 실행 실패\n");
        return false;
    }

    // 구독 성공 URC (+KMQTT_IND: <session_id>,2) 대기
    uint32_t elapsed = 0;
    bool sub_success = false;
    char expected_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KMQTT_IND: %d,2", this->mqtt_session_id);

    while (elapsed < 10000)
    {
        modem_sleep(100);
        elapsed += 100;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, expected_urc) != nullptr)
        {
            sub_success = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!sub_success)
    {
        printf("[MQTTS] 에러: Subscribe URC 수신 실패\n");
        return false;
    }

    printf("[MQTTS] Subscribe 성공 URC 확인 완료.\n");
    return true;
}

void nb_iot::modem_MqttClose()
{
    if (this->mqtt_session_id == 0) return;

    printf("[MQTTS] MQTTS 세션 종료 시작 (Session ID: %d)...\n", this->mqtt_session_id);
    
    char close_cmd[64];
    snprintf(close_cmd, sizeof(close_cmd), "AT+KMQTTCLOSE=%d", this->mqtt_session_id);
    this->modem_SendCmdWaitResponse(close_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    char del_cmd[64];
    snprintf(del_cmd, sizeof(del_cmd), "AT+KMQTTDEL=%d", this->mqtt_session_id);
    this->modem_SendCmdWaitResponse(del_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    this->mqtt_session_id = 0;
    is_socket_open = false;
    printf("[MQTTS] MQTTS 세션 정상 종료 완료.\n");
}