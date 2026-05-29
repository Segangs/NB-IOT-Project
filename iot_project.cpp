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
    bool is_socket_open = false; // 소켓상태 (열림, 닫힘)

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
    bool modem_CheckNetwork();

    void modem_SendCmd(const char *cmd);
    void modem_SendCmdUserInput();
    void modem_ReadResponse(int check = 0);
    void modem_ClearRxBuffer();

    void modem_SocketOpen(const char *ip, const char *port);
    void modem_SocketSend(const char *data);
    void modem_SocketClose();

    // LCD 관련
    void lcd_RSSICreateIcon();
    void lcd_print(const char *line1 = "", const char *line2 = "");
    void lcd_RSSIPrint(int csq);
    void lcd_RSSIAnimation();
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

    printf("PWR 1. 30s\n");

    sleep_ms(30000); // 모뎀 부팅 대기
    this->modem_SendCmd("AT+CMEE=1");
    // this->send_cmd("ATE1");
    this->modem_SendCmd("AT+CFUN=1");
    // sleep_ms(10000); // 모뎀 부팅 대기
}

// 모뎀 망등록 체크
bool nb_iot::modem_CheckNetwork()
{
    printf("Checking Network Registration (AT+CEREG?)\n");
    this->modem_SendCmd("AT+CEREG?");
    sleep_ms(1000); // 모뎀 응답 대기
    this->modem_ReadResponse();

    // 1. 요청하신 "+CEREG: 5,5" 문자열이 버퍼에 있는지 다이렉트로 확인
    if (strstr(this->rx_buffer, "+CEREG: 5,5") != NULL)
    {
        return true;
    }

    // 2. 혹시 모를 파싱 예외 처리 (sscanf 활용하여 stat이 1(홈망) 또는 5(로밍망)인지 체크)
    char *p = strstr(this->rx_buffer, "+CEREG:");
    if (p != NULL)
    {
        int n = 0, stat = 0;
        if (sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2)
        {
            if (stat == 1 || stat == 5)
            {
                return true;
            }
        }
    }
    return false;
}

// 모뎀 RSSI check
void nb_iot::modem_RSSIcheck()
{
    int csq = 99;

    if (this->is_socket_open)
    {
        return;
    }
    static absolute_time_t next_check;

    // 부팅시 실행은 30초 무시
    static bool first_run = true;
    if (!first_run && !time_reached(next_check))
    {
        return;
    }

    // 30초 체크
    first_run = false;

    // 다음 실행 시각 재설정
    next_check = make_timeout_time_ms(30000);

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

// 모뎀 TCP 소켓 열기
void nb_iot::modem_SocketOpen(const char *ip, const char *port)
{
    this->lcd_print("Sock init..");
    this->is_socket_open = true;
    this->modem_SendCmd("AT+KTCPCLOSE=1");
    sleep_ms(1000);

    this->modem_SendCmd("AT+KTCPDEL=1");
    sleep_ms(1000);

    this->lcd_print("Sock init..", "AT+KCNXCFG");
    printf("GPRS Connection Configuration : AT+KCNXCFG=1,GPRS,YOUR_APN_NAME_PLACEHOLDER\n");
    this->modem_SendCmd("AT+KCNXCFG=1,\"GPRS\",\"YOUR_APN_NAME_PLACEHOLDER\"");
    sleep_ms(2000);
    this->modem_ReadResponse();

    this->lcd_print("Sock init..", "AT+KTCPCFG");
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+KTCPCFG=1,0,\"%s\",%s", ip, port);
    printf("TCP Connection Configuration : (%s)\n", cmd);
    this->modem_SendCmd(cmd);
    sleep_ms(2000);
    this->modem_ReadResponse();

this->lcd_print("Sock init..", "AT+KTCPCNX");
    printf("Start TCP Connection : AT+KTCPCNX=1\n");
    this->modem_SendCmd("AT+KTCPCNX=1");
    sleep_ms(5000);
    this->modem_ReadResponse();

    // 다이렉트 모드
    this->lcd_print("Sock init..", "AT+KTCPSTART");
    printf("Start a TCP Connection in Direct Data Flow : KTCPSTART=1\n");
    this->modem_SendCmd("AT+KTCPSTART=1");
    sleep_ms(2000);
    this->modem_ReadResponse();
    sleep_ms(5000);
}

// 모뎀 TCP 데이터 전송
void nb_iot::modem_SocketSend(const char *data)
{
    // KCTPSTART로 보낼 때
    this->lcd_print("Data >>>");
    printf("\n데이터 송신: %s\n", data);
    uart_puts(UART_ID, data);
    uart_puts(UART_ID, "\n");
    sleep_ms(100);

    // KTCPSND로 보낼 때

    // int len = strlen(data);
    // char cmd[64];

    // snprintf(cmd, sizeof(cmd), "AT+KTCPSND=1,%d", len);
    // printf("데이터 송신 준비 요청: %s\n", cmd);
    // this->modem_SendCmd(cmd);

    // sleep_ms(200);
    // this->modem_ReadResponse();

    // printf("데이터 전송: %s\n", data);
    // uart_puts(UART_ID, data);
    // uart_puts(UART_ID, "--EOF--Pattern--");

    // sleep_ms(500);
    // this->modem_ReadResponse();
}

// 모뎀 TCP 소켓 닫기
void nb_iot::modem_SocketClose()
{
    printf("\n소켓닫기...\n");
    this->lcd_print("Sock Close..");

    // printf("명령모드로 복귀 (+++)...\n");
    // sleep_ms(1000);
    // uart_puts(UART_ID, "+++");
    // sleep_ms(1000);
    // this->modem_ReadResponse();

    this->modem_SendCmd("--EOF--Pattern--");
    sleep_ms(1000);
    this->modem_SendCmd("AT+KTCPCLOSE=1");
    sleep_ms(1000);
    this->modem_SendCmd("AT+KTCPDEL=1");
    this->lcd_print("Sock Closed");
    sleep_ms(1000);
    this->modem_ReadResponse();
    this->is_socket_open = false;
}

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

void nb_iot::lcd_RSSIAnimation()
{
    static absolute_time_t next_anim = make_timeout_time_ms(0);
    static int frame = 0;

    if (!time_reached(next_anim)) // 400ms가 지나지 않았다면 아무것도 안 하고 리턴
    {
        return;
    }
    next_anim = make_timeout_time_ms(400); // 다음 프레임 시간 설정(400ms 뒤)

    p_lcd->SetCursor(0, 13);
    p_lcd->PrintCustomChar(0);

    int anim_stage = (frame % 4); // 0, 1, 2, 3 반복
    if (anim_stage == 0)
    {
        p_lcd->SetCursor(0, 14);
        p_lcd->PrintString("\x01 "); // 1칸
    }

    else if (anim_stage == 1)
    {
        p_lcd->SetCursor(0, 14);
        p_lcd->PrintString("\x02 "); // 2칸
    }

    else if (anim_stage == 2)
    {
        p_lcd->SetCursor(0, 14);
        p_lcd->PrintString("\x02\x03"); // 3칸
    }

    else if (anim_stage == 3)
    {
        p_lcd->SetCursor(0, 14);
        p_lcd->PrintString("\x02\x04"); // 4칸 완충
    }

    frame++;
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
    iot.modem_RSSIcheck();
    iot.lcd_print("Ready");
    sleep_ms(3000);

    absolute_time_t next_net_check = make_timeout_time_ms(0); // 켜지자마자 첫 검사 수행
    int retry_cnt = 0;
    while (true)
    {
        // 1. 💡 400ms 마다 LCD 애니메이션 프레임을 업데이트합니다. (Non-blocking)
        iot.lcd_RSSIAnimation();
        // 2. 💡 5초(5000ms)마다 한 번씩 모뎀에게 망 등록이 되었는지 물어봅니다.
        if (time_reached(next_net_check))
        {
            retry_cnt++;
            printf("망 등록 상태 확인 중... (시도 횟수: %d)\n", retry_cnt);
            iot.lcd_print("Net Check..");

            if (iot.modem_CheckNetwork())
            {
                iot.lcd_print("Net Ready..");
                printf("\n🎉 망 등록 성공! 소켓 통신을 시작합니다. 🎉\n");
                break; // 망을 잡았으니 루프를 탈출하여 다음 리모컨 로직으로 진행!
            }

            // 아직 못 잡았다면 다음 검사 시간을 5초 뒤로 예약
            next_net_check = make_timeout_time_ms(5000);
        }

        // 미세한 대기로 CPU 과부하 방지
        sleep_us(100);
    }

    printf("--- IR Receiver NEC Test Start ---\n");

    // 2. PIO 및 핀 설정
    PIO pio = pio0;   // pio0 인스턴스 사용
    uint sm = 0;      // 상태 머신 0번 사용
    uint rx_pin = 17; // GP17 핀 사용

    nec_rx_init(pio, rx_pin);
    printf("Initialized NEC IR Receiver on GP17\n");

    // 변수 선언 (리모컨 주소값과 데이터값이 저장될 공간)
    uint8_t address = 0;
    uint8_t data = 0;

    while (true)
    {
        iot.modem_SendCmdUserInput();
        iot.modem_ReadResponse();
        iot.modem_RSSIcheck();
        sleep_us(10);

        // IR 리모컨 데이터가 수신되었는지 확인
        if (!pio_sm_is_rx_fifo_empty(pio, sm))
        {
            uint32_t rx_frame = pio_sm_get_blocking(pio, sm);

            uint8_t address = 0;
            uint8_t data = 0;

            // 읽어온 데이터를 디코딩
            if (nec_decode_frame(rx_frame, &address, &data))
            {
                printf("Received Code -> Address: 0x%02X, Data: 0x%02X\n", address, data);

                // 리모컨 키 코드에 따른 제어 블록
                switch (data)
                {
                case 0x45: // POWER 버튼
                    printf("[POWER] 소켓 오픈 요청\n");
                    iot.modem_SocketOpen("segang.duckdns.org", "1818");
                    iot.lcd_print("Connected");
                    break;

                case 0x47: // MUTE 버튼
                    printf("[MUTE] 소켓 닫기 요청\n");
                    iot.modem_SocketClose();
                    iot.lcd_print("Ready");
                    break;

                // 숫자 버튼 처리
                case 0x16:
                    printf("[0] 전송\n");
                    iot.modem_SocketSend("0");
                    iot.lcd_print("Send: 0");
                    break;
                case 0x0C:
                    printf("[1] 전송\n");
                    iot.modem_SocketSend("11");
                    iot.lcd_print("Send: 1");
                    break;
                case 0x18:
                    printf("[2] 전송\n");
                    iot.modem_SocketSend("21");
                    iot.lcd_print("Send: 2");
                    break;
                case 0x5E:
                    printf("[3] 전송\n");
                    iot.modem_SocketSend("3");
                    iot.lcd_print("Send: 3");
                    break;
                case 0x08:
                    printf("[4] 전송\n");
                    iot.modem_SocketSend("10");
                    iot.lcd_print("Send: 4");
                    break;
                case 0x1C:
                    printf("[5] 전송\n");
                    iot.modem_SocketSend("20");
                    iot.lcd_print("Send: 5");
                    break;
                case 0x5A:
                    printf("[6] 전송\n");
                    iot.modem_SocketSend("6");
                    iot.lcd_print("Send: 6");
                    break;
                case 0x42:
                    printf("[7] 전송\n");
                    iot.modem_SocketSend("7");
                    iot.lcd_print("Send: 7");
                    break;
                case 0x52:
                    printf("[8] 전송\n");
                    iot.modem_SocketSend("8");
                    iot.lcd_print("Send: 8");
                    break;
                case 0x4A:
                    printf("[9] 전송\n");
                    iot.modem_SocketSend("9");
                    iot.lcd_print("Send: 9");
                    break;

                default:
                    printf("미정의 버튼 입력: 0x%02X\n", data);
                    break;
                }
            }
        }
    }
    return 0;
}