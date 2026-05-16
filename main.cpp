// sim7080 iot 260429

#include <iostream>
#include <stdio.h>
#include <cstring>
#include <array>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/uart.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "LCD_I2C.hpp"

#include <math.h> // log 함수 사용을 위해 필수

using namespace std;

#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define PWR_PIN 14

// LED
#define LED_RED 12
#define LED_GRN 11
#define LED_BLE 10

// I2C (LCD)
#define I2C_PORT i2c0
#define LCD_ADDR 0x27
#define SDA_PIN 20
#define SCL_PIN 21

// // 환경 설정 상수
const float VCC = 3.3f;
const float R_FIXED = 10000.0f; // 사용 중인 10k 고정 저항
const uint ADC_PIN = 26;

// NTC 센서 특성 (데이터시트가 없을 때 가장 표준적인 값)
const float B_COEFFICIENT = 3950.0f;
const float ROOM_TEMP_K = 298.15f;  // 25°C를 절대온도로 변환 (273.15 + 25)
const float R_ROOM_TEMP = 10000.0f; // 25°C일 때의 표준 저항 (10k)

// 100번 읽어 평균을 내는 안정화 함수
float get_averaged_voltage()
{
    uint32_t sum = 0;
    for (int i = 0; i < 100; i++)
    {
        sum += adc_read();
    }
    return ((float)sum / 100.0f) * (VCC / 4095.0f);
}

/// //////

class nb_iot
{
private:
    char rx_buffer[256];
    int buffer_idx;

    // LCD 객체에 접근하기 위한 포인터 추가
    LCD_I2C *p_lcd;

    char device_imei[20] = "0";
    char device_imsi[20] = "0";

public:
    nb_iot(LCD_I2C *lcd_ptr); // 생성자에서 LCD 객체의 주소를 받음
    ~nb_iot();

    static bool led_red_blink_callback(struct repeating_timer *t); //  R LED  BLINK
    struct repeating_timer blink_timer;                            // R LED BLINK STOP

    void device_init();
    void device_Check();

    void send_cmd(const char *cmd);
    void read_response(int check = 0);
    void check_status();
    void display_temp(float temp);

    // LCD 출력을 도와주는 헬퍼 함수 (선택 사항)
    void lcd_log(const char *line1, const char *line2 = "");
};

// 클래스
nb_iot::nb_iot(LCD_I2C *lcd_ptr) : p_lcd(lcd_ptr) // 포인터 저장
{
    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_init(LED_GRN);
    gpio_set_dir(LED_GRN, GPIO_OUT);
    gpio_init(LED_BLE);
    gpio_set_dir(LED_BLE, GPIO_OUT);

    add_repeating_timer_ms(-500, led_red_blink_callback, NULL, &blink_timer);

    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    sleep_ms(10000);
}

// 함수 구현

bool nb_iot::led_red_blink_callback(struct repeating_timer *t)
{
    gpio_put(LED_RED, !gpio_get(LED_RED)); // 반전

    return true;
}

void nb_iot::device_init() // Power on
{
    gpio_init(PWR_PIN);
    gpio_set_dir(PWR_PIN, GPIO_OUT);
    lcd_log("Powering ON..", ""); // LCD 출력
    sleep_ms(1500);

    printf("Power ON..\n");
    gpio_put(PWR_PIN, 1);
    sleep_ms(1500);

    lcd_log("System Boot..", "");
    printf("System Boot..\n");
    gpio_put(PWR_PIN, 0);

    sleep_ms(10000);
    this->send_cmd("ATE0"); // ATE0 으로 설정
    check_status();
}

void nb_iot::send_cmd(const char *cmd)
{
    uart_puts(UART_ID, cmd);
    uart_puts(UART_ID, "\r\n");
    sleep_ms(100);
}

void nb_iot::read_response(int check)
{
    sleep_ms(10);
    memset(this->rx_buffer, 0, sizeof(this->rx_buffer)); // 버퍼를 0으로 비우기.
    this->buffer_idx = 0;                                // 버퍼idx초기화

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
        }
    }
    this->rx_buffer[buffer_idx] = '\0'; // 버퍼 끝 NULL 추가
}

void nb_iot::check_status()
{
    this->read_response(1);
    if (strstr(rx_buffer, "NORMAL POWER DOWN")) // NORMAL POWER DOWN시 강제 재시작
    {
        printf("(E01) Retry Boot..\n");
        this->device_init();
    }

    if (strstr(rx_buffer, "RDY")) // RDY시 기기 CHECK
    {
        printf("Boot Successful..\n");
        this->device_Check();
    }
}

void nb_iot::device_Check()
{
    printf("Checking Device..\n");
    send_cmd("AT+CGSN"); // IMEI
    lcd_log("Checking Device", "AT+CGSN");
    read_response(1);

    if (buffer_idx > 15)
    {
        strncpy(this->device_imei, rx_buffer, 15);
        printf("IMEI OK : %s\n", this->device_imei);
        lcd_log("IMEI OK", this->device_imei); // IMEI 번호를 LCD에 표시
    }
    else
    {
        printf("IMEI CHECK FAILED\n");
    }

    sleep_ms(1000);
    send_cmd("AT+CIMI"); // IMSI
    lcd_log("Checking Device", "AT+CIMI");
    read_response(1);

    if (buffer_idx > 15)
    {
        strncpy(this->device_imsi, rx_buffer, 15);
        printf("IMSI OK : %s\n", this->device_imsi);
        lcd_log("IMSI OK", this->device_imsi); // IMEI 번호를 LCD에 표시
    }
    else
    {
        printf("IMSI CHECK FAILED\n");
    }
    sleep_ms(1000);
}

void nb_iot::lcd_log(const char *line1, const char *line2)
{
    p_lcd->Clear();
    p_lcd->SetCursor(0, 0);
    p_lcd->PrintString(line1);
    p_lcd->SetCursor(1, 0);
    p_lcd->PrintString(line2);
}

void nb_iot::display_temp(float temp)
{
    char buf[17];
    // 소수점 1자리 + 섭씨기호 + C + 남은공백(이전잔상제거)
    snprintf(buf, sizeof(buf), "%.1f\337C  ", temp);
    p_lcd->SetCursor(1, 0);
    p_lcd->PrintString(buf);
}

// 소멸자
nb_iot::~nb_iot()
{
}

std::array<uint8_t, 8> RSSI_ANT = {
    0b11111,    0b01010,    0b00100,    0b00100,    0b00100,    0b00100,    0b00100,    0b00100};

std::array<uint8_t, 8> RSSI01 = {
    0b00000,    0b00000,    0b00000,    0b00000,    0b00000,    0b00000,    0b11000,    0b11000};

std::array<uint8_t, 8> RSSI12 = {
    0b00000,    0b00000,    0b00000,    0b00000,    0b00011,    0b00011,    0b11011,    0b11011};

std::array<uint8_t, 8> RSSI03 = {
    0b00000,    0b00000,    0b11000,    0b11000,    0b11000,    0b11000,    0b11000,    0b11000};

std::array<uint8_t, 8> RSSI34 = {
    0b00011,    0b00011,    0b11011,    0b11011,    0b11011,    0b11011,    0b11011,    0b11011};

int main()
{
    stdio_init_all();

    // 1. I2C 하드웨어 자체를 먼저 초기화해야 합니다.
    LCD_I2C lcd(LCD_ADDR, 16, 2, I2C_PORT, SDA_PIN, SCL_PIN);

    lcd.BacklightOn();
    lcd.PrintString("System Start");

    nb_iot iot(&lcd);
    lcd.CreateCustomChar(0, RSSI_ANT);
    lcd.CreateCustomChar(1, RSSI01);
    lcd.CreateCustomChar(2, RSSI12);
    lcd.CreateCustomChar(3, RSSI03);
    lcd.CreateCustomChar(4, RSSI34);

    iot.device_init();

    lcd.Clear();
    lcd.SetCursor(0, 0);
    lcd.PrintString("Ready");
    // lcd.SetCursor(1, 0);
    // lcd.PrintString("-13.4");
    // lcd.PrintChar((char)223); // 섭씨 기호 출력 (°)
    // lcd.PrintString("C");

    // 등록한 0번 캐릭터 출력
    // lcd.SetCursor(0, 13);
    // lcd.PrintCustomChar(0);

    // lcd.SetCursor(0, 14);
    // lcd.PrintCustomChar(2);
    // lcd.SetCursor(0, 15);
    // lcd.PrintCustomChar(4);
    // lcd.PrintString("\x01\x02\x04");

    // lcd.SetCursor(1, 6);
    // lcd.PrintCustomChar(0);
    // lcd.SetCursor(1, 7);
    // lcd.PrintCustomChar(2);
    // lcd.SetCursor(1, 8);
    // lcd.PrintCustomChar(3);

    // lcd.SetCursor(1, 11);
    // lcd.PrintCustomChar(0);
    // lcd.SetCursor(1, 12);
    // lcd.PrintCustomChar(1);

    // sleep_ms(10000);

    cancel_repeating_timer(&iot.blink_timer);
    gpio_put(LED_RED, 0);
    gpio_put(LED_GRN, 1);

    // iot.lcd_log("Ready", ""); // LCD 출력
    sleep_ms(5000);

    // ADC 초기화
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(0); // ADC0 (GPIO 26) 선택

    lcd.SetCursor(0, 13);
    lcd.PrintCustomChar(0);
    while (true)
    {
        
       
            lcd.SetCursor(0, 14);
            lcd.PrintString("\x01 ");
            sleep_ms(500);
            lcd.SetCursor(0, 14);
            lcd.PrintString("\x02 ");
            sleep_ms(500);
            lcd.SetCursor(0, 14);
            lcd.PrintString("\x02\x03");
            sleep_ms(500);
            lcd.SetCursor(0, 14);
            lcd.PrintString("\x02\x04");
            sleep_ms(500);
        
        //sleep_ms(10000);
        //iot.send_cmd("AT");
        //iot.read_response();

        float voltage = get_averaged_voltage();

        if (voltage > 0.05f && voltage < 3.25f)
        {
            // 1. 현재 저항 계산
            float r_sensor = R_FIXED * (VCC / voltage - 1.0f);

            // 2. 섭씨 온도로 변환 (B-parameter 식)
            float temperature;
            temperature = r_sensor / R_ROOM_TEMP; // (R / Ro)
            temperature = log(temperature);       // ln(R / Ro)
            temperature /= B_COEFFICIENT;         // 1/B * ln(R / Ro)
            temperature += 1.0f / ROOM_TEMP_K;    // + (1 / To)
            temperature = 1.0f / temperature;     // 역수
            temperature -= 273.15f;               // 절대온도 -> 섭씨

            printf("Temp: %.2f °C | Resistance: %.1f Ohm\n", temperature, r_sensor);
            iot.display_temp(temperature);
        }
        else
        {
            printf("Sensor Error: Check wiring!\n");
        }

            lcd.SetCursor(1, 13);
            lcd.PrintString(">  ");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString(">> ");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString(" >>");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString("  >");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString("   ");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString(">  ");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString(">> ");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString(" >>");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString("  >");
            sleep_ms(200);
            lcd.SetCursor(1, 13);
            lcd.PrintString(" ok");



        sleep_ms(30000);
    }

    return 0;
}