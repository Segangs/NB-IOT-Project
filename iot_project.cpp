// 260526 프로젝트

#include <stdio.h>
#include <cstring>
#include <array>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/uart.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "lib/LCD_I2C.hpp"
extern "C"
{
#include "lib/nec_receive.h"
}

// 핀 정의
#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define PWR_ON_PIN 28

// I2C (LCD)
#define I2C_PORT i2c0
#define LCD_ADDR 0x27
#define SDA_PIN 20
#define SCL_PIN 21

// RSSI 신호세기 아이콘
std::array<uint8_t, 8> RSSI_ANT = {0b11111, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
std::array<uint8_t, 8> RSSI01 = {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11000, 0b11000};
std::array<uint8_t, 8> RSSI12 = {0b00000, 0b00000, 0b00000, 0b00000, 0b00011, 0b00011, 0b11011, 0b11011};
std::array<uint8_t, 8> RSSI03 = {0b00000, 0b00000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000};
std::array<uint8_t, 8> RSSI34 = {0b00011, 0b00011, 0b11011, 0b11011, 0b11011, 0b11011, 0b11011, 0b11011};

class nb_iot
{
private:
    // modem 관련
    char rx_buffer[256];
    int buffer_idx;
    char device_imei[20] = "0";
    char device_imsi[20] = "0";

    LCD_I2C *p_lcd; // LCD 객체에 접근하기 위한 포인터 추가

public:
    nb_iot(LCD_I2C *lcd_ptr); // 생성자에서 LCD 객체의 주소를 받음
    ~nb_iot();

    // void device_init();
    // void device_Check();
    // void check_status();

    // 모뎀 관련
    void modem_init();
    void modem_RSSIcheck();

    void modem_SendCmd(const char *cmd);
    void modem_SendCmdUserInput();
    void modem_ReadResponse(int check = 0);
    void modem_ClearRxBuffer();

    // LCD 관련
    void lcd_RSSICreateIcon();
    void lcd_print(const char *line1 = "", const char *line2 = "");
    void lcd_RSSIPrint(int csq);
};

nb_iot::nb_iot(LCD_I2C *lcd_ptr) : p_lcd(lcd_ptr)
{
    p_lcd->BacklightOn();
    p_lcd->PrintString("System Start");
    this->lcd_RSSICreateIcon();
}

// 소멸자
nb_iot::~nb_iot()
{
}

// 모뎀 init
void nb_iot::modem_init()
{
    sleep_ms(5000);
    // 1. UART 설정
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    sleep_ms(5000);

    // 2. PWR_ON 핀 설정
    gpio_init(PWR_ON_PIN);
    gpio_set_dir(PWR_ON_PIN, GPIO_OUT);
    gpio_put(PWR_ON_PIN, 1); // 평소엔 High 유지
    sleep_ms(500);

    // 3. 모뎀 전원 켜기 펄스 (Low 신호 인가)
    printf("Powering ON..\n");
    this->lcd_print("Power ON..");

    gpio_put(PWR_ON_PIN, 0);
    sleep_ms(1000); // 데이터시트 권장 시간 확인 필요
    gpio_put(PWR_ON_PIN, 1);
    printf("PWR 1\n");
    sleep_ms(2000); // 모뎀 부팅 대기
    gpio_put(PWR_ON_PIN, 0);

    printf("PWR 1. 10s\n");

    sleep_ms(10000); // 모뎀 부팅 대기
    // this->send_cmd("ATE1");
    this->modem_SendCmd("AT+CFUN=4");
}

// 모뎀 RSSI check
void nb_iot::modem_RSSIcheck()
{
    int csq = 99;
    // 30초 체크
    static absolute_time_t next_check = make_timeout_time_ms(30000);

    if (!time_reached(next_check))
    {
        return;
    }

    // 다음 실행 시각 재설정
    next_check = make_timeout_time_ms(10000);

    modem_SendCmd("AT+CSQ");
    sleep_ms(100);
    modem_ReadResponse();

    char *p = strstr(this->rx_buffer, "+CSQ:"); // CSQ부터 정신없이 찾음
    if (p != NULL)
    {
        int rssi = 99;
        int ber = 99;

        // +CSQ 앞숫자만 뽑아냄
        if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) == 2)
        {
            lcd_RSSIPrint(rssi);
        }
    }
}

// 모뎀 명령어 전송
void nb_iot::modem_SendCmd(const char *cmd)
{
    this->modem_ClearRxBuffer(); // 명령어 전송 전에 Rx버퍼를 정리한다.
    uart_puts(UART_ID, cmd);
    uart_puts(UART_ID, "\r\n");
}

// 모뎀 명령어 전송 (키보드 입력.. 모뎀 -> UART)
void nb_iot::modem_SendCmdUserInput()
{
    int pc_char = getchar_timeout_us(0); // PC 입력 확인 (Non-blocking)
    if (pc_char != PICO_ERROR_TIMEOUT)
    {
        uart_putc(UART_ID, (char)pc_char);
    }
}

// 모뎀 명령어 받기...
void nb_iot::modem_ReadResponse(int check)
{
    while (uart_is_readable(UART_ID))
    {
        if (this->buffer_idx < 255)
        {
            char response = uart_getc(UART_ID);
            if (this->buffer_idx == 0)
            {
                if (response == 0x00 || (unsigned char)response == 0xFF || response == '\r' || response == '\n') // 처음에 널문자 들어오면 버퍼에서 읽어올수 없으니 무시..
                {
                    continue;
                }
            }
            if (!check)
                putchar(response);
            this->rx_buffer[buffer_idx] = response; // 버퍼 저장
            this->buffer_idx++;
            this->rx_buffer[buffer_idx] = '\0'; // 버퍼 끝 NULL 추가
        }
        else
        {
            // 버퍼가 꽉 찬 경우 예외 처리 (넘치지 않도록 초기화 등)
            this->buffer_idx = 0;
        }
    }
}

// 모뎀 버퍼 비우기
void nb_iot::modem_ClearRxBuffer()
{
    memset(this->rx_buffer, 0, sizeof(this->rx_buffer)); // 버퍼 0으로 비우고
    this->buffer_idx = 0;                                // 버퍼 idx를 초기화한ㄷㅏ..
}

// 모뎀 소켓 열기

// LCD RSSI Icon CREATE
void nb_iot::lcd_RSSICreateIcon()
{
    p_lcd->CreateCustomChar(0, RSSI_ANT);
    p_lcd->CreateCustomChar(1, RSSI01);
    p_lcd->CreateCustomChar(2, RSSI12);
    p_lcd->CreateCustomChar(3, RSSI03);
    p_lcd->CreateCustomChar(4, RSSI34);
}

// LCD RSSI Print
void nb_iot::lcd_RSSIPrint(int csq)
{
    p_lcd->SetCursor(0, 13);
    p_lcd->PrintCustomChar(0);
    p_lcd->SetCursor(0, 14);
    if (csq == 99 || csq == 0)
    {
        p_lcd->PrintString("x ");
    }
    else if (csq >= 24)
    {
        p_lcd->PrintString("\x02\x04");
    }
    else if (csq >= 18)
    {
        p_lcd->PrintString("\x02\x03");
    }
    else if (csq >= 8)
    {
        p_lcd->PrintString("\x02 ");
    }
    else if (csq >= 0)
    {
        p_lcd->PrintString("\x01 ");
    }
    else
    {
        p_lcd->PrintString("e ");
    }
}

// LCD 두줄 출력
void nb_iot::lcd_print(const char *line1, const char *line2)
{
    char buf1[14]; // 12칸 + NULL
    char buf2[14];

    // 왼쪽 정렬 후 남는 12칸 공간을 공백으로 강제 배치 (%-12.12s)
    snprintf(buf1, sizeof(buf1), "%-12.12s", line1); // snprintf 사용 12자까지만
    p_lcd->SetCursor(0, 0);
    p_lcd->PrintString(buf1);

    snprintf(buf2, sizeof(buf2), "%-12.12s", line2);
    p_lcd->SetCursor(1, 0);
    p_lcd->PrintString(buf2);
}

int main()
{
    stdio_init_all(); // USB 시리얼 모니터링 초기화

    LCD_I2C lcd(LCD_ADDR, 16, 2, I2C_PORT, SDA_PIN, SCL_PIN); // LCD 초기화


    nb_iot iot(&lcd);
    iot.modem_init();

    sleep_ms(5000);
    iot.lcd_print("Ready");

    printf("--- IR Receiver NEC Test Start ---\n");

    // 2. PIO 및 핀 설정
    PIO pio = pio0;   // pio0 인스턴스 사용
    uint sm = 0;      // 상태 머신 0번 사용
    uint rx_pin = 17; // GP17 핀 사용

    nec_rx_init(pio, rx_pin);
    printf("Initialized NEC IR Receiver on GP17\n");
    printf("Press any button on your remote control...\n");

    // 변수 선언 (리모컨 주소값과 데이터값이 저장될 공간)
    uint8_t address = 0;
    uint8_t data = 0;

    while (true)
    {
        iot.modem_SendCmdUserInput();
        // iot.modem_ReadResponse();
        iot.modem_RSSIcheck();
        // sleep_us(10);

        // 2) 읽어온 32비트 데이터를 라이브러리 함수를 통해 주소와 데이터로 분해(디코딩)합니다.
        if (!pio_sm_is_rx_fifo_empty(pio, sm))
        {
            uint32_t rx_frame = pio_sm_get_blocking(pio, sm); // 데이터가 있는 게 확인됐으니 바로 읽힘

            uint8_t address = 0;
            uint8_t data = 0;

            // 읽어온 데이터를 디코딩
            if (nec_decode_frame(rx_frame, &address, &data))
            {
                printf("Received Code -> Address: 0x%02X (%d), Data: 0x%02X (%d)\n",
                       address, address, data, data);
            }
        }
    }
    return 0;
}