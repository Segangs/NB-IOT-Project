#include "tasks_sensor.hpp"
#include "../config.h"
#include "tasks_lcd.hpp"
#include "../lib/log.hpp"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <stdio.h>

extern LcdTaskParams lcd_params;

// FreeRTOS standard headers
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

// Standard FreeRTOS Mutex to protect ADC hardware resources securely
static SemaphoreHandle_t adc_mutex = nullptr;
static SemaphoreHandle_t one_wire_mutex = nullptr;

void sensor_init()
{
    // Initialize ADC hardware
    adc_init();

#if ENABLE_DS18B20_READ && ENABLE_TEMP1_DS18B20
    // Configure DS18B20 1-Wire buses. External 4.7k/5.1k pull-ups are expected,
    // while internal pull-ups provide a weak backup during temporary wiring tests.
    gpio_init(TEMP1_SENSOR_PIN);
    gpio_set_dir(TEMP1_SENSOR_PIN, GPIO_IN);
    gpio_pull_up(TEMP1_SENSOR_PIN);
#endif
#if ENABLE_DS18B20_READ && ENABLE_TEMP2_DS18B20
    gpio_init(TEMP2_SENSOR_PIN);
    gpio_set_dir(TEMP2_SENSOR_PIN, GPIO_IN);
    gpio_pull_up(TEMP2_SENSOR_PIN);
#endif

    // Enable the internal RP2040/RP2350 temperature sensor on channel 4
    adc_set_temp_sensor_enabled(true);

    // CYW43 무선 칩의 CS 핀(GPIO 25)을 High로 설정하여 SPI 버스 점유를 해제(Deselect)합니다.
    // 이를 통해 GPIO 29 (SPI CLK) 핀에 유입되는 무선 칩의 간섭을 차단하고 VSYS를 정확하게 계측합니다.
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    // Create the ADC mutex
    adc_mutex = xSemaphoreCreateMutex();
    one_wire_mutex = xSemaphoreCreateMutex();
}

float read_vsys_voltage(bool &is_stable)
{
    float voltage = 0.0f;

    if (adc_mutex != nullptr && xSemaphoreTake(adc_mutex, portMAX_DELAY) == pdTRUE) {
        // Pico W / Pico 2 W의 공유 핀(GPIO 29 / ADC 3) 하이재킹 시퀀스
        // 매 리딩 시점에 와이파이 CS(GPIO 25)를 확실하게 High로 올려 통신을 해제하고 측정을 활성화합니다.
        gpio_init(25);
        gpio_set_dir(25, GPIO_OUT);
        gpio_put(25, 1);

        gpio_set_function(29, GPIO_FUNC_NULL);
        adc_gpio_init(29);
        gpio_disable_pulls(29); // 내부 풀업/풀다운을 비활성화하여 6.6V 과도 계측 오류 방지

        adc_select_input(ADC_VSYS_CHANNEL);
        sleep_us(500); // 채널 멀티플렉서 안정화 대기 시간 충분히 확대 (200us -> 500us)

        // Clear residual charge in the ADC sample capacitor before averaging.
        (void)adc_read();
        (void)adc_read();

        // 💡 노이즈 제거 및 안정적 계측을 위한 100회 평균 필터링 적용
        uint32_t sum = 0;
        for (int i = 0; i < 100; i++) {
            sum += adc_read();
        }

        xSemaphoreGive(adc_mutex);

        float raw_avg = (float)sum / 100.0f;
        // Convert to actual voltage (Pico 2 VSYS divider ratio is 3:1)
        voltage = (raw_avg * 3.3f / 4095.0f) * 3.0f;
    }

    // Validate if within stable thresholds
    is_stable = (voltage >= VSYS_VOLTAGE_MIN && voltage <= VSYS_VOLTAGE_MAX);
    return voltage;
}

float read_internal_temp(bool &is_normal)
{
    float temp = 0.0f;

    if (adc_mutex != nullptr && xSemaphoreTake(adc_mutex, portMAX_DELAY) == pdTRUE) {
        adc_set_temp_sensor_enabled(true); // Ensure internal temp sensor bias is enabled
        adc_select_input(4);
        sleep_us(500); // Settle time (expanded from 50us to 500us for accuracy)
        uint16_t raw = adc_read();
        xSemaphoreGive(adc_mutex);

        float voltage = raw * 3.3f / 4095.0f;
        temp = 27.0f - (voltage - 0.706f) / 0.001721f;
        temp -= 45.0f; // 💡 실기 실리콘 편차 보정을 위한 소프트웨어 오프셋 적용 (-45.0C)
    }

    // Validate if within safe operational range
    is_normal = (temp >= CHIP_TEMP_MIN && temp <= CHIP_TEMP_MAX);
    return temp;
}

bool check_flash_integrity(uint32_t &calculated_checksum)
{
    const uint8_t *flash_addr = (const uint8_t *)0x10000000;
    const uint32_t size_to_check = 64 * 1024;

    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < size_to_check; i++) {
        hash ^= flash_addr[i];
        hash *= 16777619u;
    }

    calculated_checksum = hash;
    return true;
}

bool test_ram_integrity()
{
    const uint32_t test_size = 1024;
    volatile uint8_t *ram_buffer = new uint8_t[test_size];
    if (ram_buffer == nullptr) {
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0; i < test_size; i++) {
        ram_buffer[i] = 0x55;
    }
    for (uint32_t i = 0; i < test_size; i++) {
        if (ram_buffer[i] != 0x55) {
            ok = false;
            break;
        }
    }

    if (ok) {
        for (uint32_t i = 0; i < test_size; i++) {
            ram_buffer[i] = 0xAA;
        }
        for (uint32_t i = 0; i < test_size; i++) {
            if (ram_buffer[i] != 0xAA) {
                ok = false;
                break;
            }
        }
    }

    delete[] ram_buffer;
    return ok;
}

static void ow_drive_low(uint pin)
{
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
}

static void ow_release(uint pin)
{
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

static bool ow_reset(uint pin, bool &idle_high, bool &presence_seen)
{
    ow_release(pin);
    sleep_us(10);
    idle_high = gpio_get(pin);
    if (!idle_high) {
        presence_seen = false;
        return false;
    }

    ow_drive_low(pin);
    sleep_us(500);
    ow_release(pin);
    sleep_us(70);
    presence_seen = !gpio_get(pin);
    sleep_us(430);
    return presence_seen;
}

static void ow_write_bit(uint pin, bool bit)
{
    ow_drive_low(pin);
    if (bit) {
        sleep_us(6);
        ow_release(pin);
        sleep_us(64);
    } else {
        sleep_us(60);
        ow_release(pin);
        sleep_us(10);
    }
}

static bool ow_read_bit(uint pin)
{
    ow_drive_low(pin);
    sleep_us(6);
    ow_release(pin);
    sleep_us(9);
    bool bit = gpio_get(pin);
    sleep_us(55);
    return bit;
}

static void ow_write_byte(uint pin, uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(pin, (value >> i) & 0x01);
    }
}

static uint8_t ow_read_byte(uint pin)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit(pin)) {
            value |= (uint8_t)(1u << i);
        }
    }
    return value;
}

static uint8_t ds18b20_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t inbyte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            inbyte >>= 1;
        }
    }
    return crc;
}

static bool ds18b20_read_temperature(uint pin, float &temperature, int &status)
{
    temperature = -999.0f;
#if !ENABLE_DS18B20_READ
    (void)pin;
    status = 1;
    return false;
#else
    status = 0;
    bool idle_high = false;
    bool presence_seen = false;

    if (!ow_reset(pin, idle_high, presence_seen)) {
        status = idle_high ? 1 : 5; // 1: no presence, 5: line stuck low
        return false;
    }

    ow_write_byte(pin, 0xCC); // Skip ROM: one DS18B20 per bus
    ow_write_byte(pin, 0x44); // Convert T
    vTaskDelay(pdMS_TO_TICKS(750));

    if (!ow_reset(pin, idle_high, presence_seen)) {
        status = idle_high ? 1 : 5;
        return false;
    }

    ow_write_byte(pin, 0xCC);
    ow_write_byte(pin, 0xBE); // Read Scratchpad

    uint8_t scratchpad[9] = {0};
    for (uint8_t &byte : scratchpad) {
        byte = ow_read_byte(pin);
    }

    if (ds18b20_crc8(scratchpad, 8) != scratchpad[8]) {
        status = 2; // CRC mismatch / noisy wiring
        return false;
    }

    int16_t raw = (int16_t)((scratchpad[1] << 8) | scratchpad[0]);
    temperature = (float)raw / 16.0f;
    if (temperature < DS18B20_TEMP_MIN || temperature > DS18B20_TEMP_MAX) {
        status = 3; // Out of DS18B20 range
        return false;
    }

    return true;
#endif
}

void check_temperature_status_dual(float &temp_ch0, int &status_ch0, float &temp_ch1, int &status_ch1)
{
    if (one_wire_mutex == nullptr || xSemaphoreTake(one_wire_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        temp_ch0 = -999.0f;
        status_ch0 = 4;
        temp_ch1 = -999.0f;
        status_ch1 = 4;
        return;
    }

    if (ENABLE_TEMP1_DS18B20) {
        ds18b20_read_temperature(TEMP1_SENSOR_PIN, temp_ch0, status_ch0);
        if (status_ch0 == 0) {
            temp_ch0 += TEMP1_CAL_OFFSET_C;
        }
    } else {
        temp_ch0 = -999.0f;
        status_ch0 = 1;
    }

    if (ENABLE_TEMP2_DS18B20) {
        ds18b20_read_temperature(TEMP2_SENSOR_PIN, temp_ch1, status_ch1);
        if (status_ch1 == 0) {
            temp_ch1 += TEMP2_CAL_OFFSET_C;
        }
    } else {
        temp_ch1 = -999.0f;
        status_ch1 = 1;
    }
    xSemaphoreGive(one_wire_mutex);

    // 💡 소프트웨어 저주파 통과 필터 (Low Pass Filter) 적용
    static float filtered_temp_ch0 = -999.0f;
    static float filtered_temp_ch1 = -999.0f;
    const float alpha = 0.15f;

    // Ch0 필터링
    if (status_ch0 == 0) {
        if (filtered_temp_ch0 <= -990.0f) {
            filtered_temp_ch0 = temp_ch0; // 초기화
        } else {
            filtered_temp_ch0 = (alpha * temp_ch0) + ((1.0f - alpha) * filtered_temp_ch0);
        }
        temp_ch0 = filtered_temp_ch0;
    } else {
        filtered_temp_ch0 = -999.0f;
    }

    // Ch1 필터링
    if (status_ch1 == 0) {
        if (filtered_temp_ch1 <= -990.0f) {
            filtered_temp_ch1 = temp_ch1; // 초기화
        } else {
            filtered_temp_ch1 = (alpha * temp_ch1) + ((1.0f - alpha) * filtered_temp_ch1);
        }
        temp_ch1 = filtered_temp_ch1;
    } else {
        filtered_temp_ch1 = -999.0f;
    }

    // 3분 주기 한 줄 출력 (180000ms)
    static uint32_t last_dbg_print_ms = 0;
    static bool first_run = true;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (first_run) {
        last_dbg_print_ms = now_ms;
        first_run = false;
    }

    if (now_ms - last_dbg_print_ms >= 180000) {
        // 부팅 중이거나, 모뎀이 데이터 전송 중이거나 제어 모드일 때는 UART 출력 버퍼 포화/경쟁 방지를 위해 출력을 한 턴 건너뜁니다.
        if (lcd_params.is_booting || lcd_params.is_transmitting || lcd_params.is_modem_busy) {
            return;
        }
        LOG("SENSOR_DBG DS18B20 GP%d %.2f,%d GP%d %.2f,%d\n",
            TEMP1_SENSOR_PIN, temp_ch0, status_ch0,
            TEMP2_SENSOR_PIN, temp_ch1, status_ch1);
        last_dbg_print_ms = now_ms;
    }
}
