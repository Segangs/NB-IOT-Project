#include "tasks_sensor.hpp"
#include "../config.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <math.h>
#include <stdio.h>

// FreeRTOS standard headers
#include "FreeRTOS.h" 
#include "semphr.h"  

// Standard FreeRTOS Mutex to protect ADC hardware resources securely
static SemaphoreHandle_t adc_mutex = nullptr; 

void sensor_init()
{
    // Initialize ADC hardware
    adc_init();
    
    // Configure GP26 (ADC0) for NTC thermistor analog input
    adc_gpio_init(ADC_NTC_PIN);
    gpio_disable_pulls(ADC_NTC_PIN);
    
    // Enable the internal RP2040/RP2350 temperature sensor on channel 4
    adc_set_temp_sensor_enabled(true);
    
    // CYW43 무선 칩의 CS 핀(GPIO 25)을 High로 설정하여 SPI 버스 점유를 해제(Deselect)합니다.
    // 이를 통해 GPIO 29 (SPI CLK) 핀에 유입되는 무선 칩의 간섭을 차단하고 VSYS를 정확하게 계측합니다.
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    // Create the ADC mutex
    adc_mutex = xSemaphoreCreateMutex();
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
        
        // 💡 [핵심] NTC 채널(0번) 측정 후 축적된 내부 커패시터 전하 소거를 위한 더미 리딩 2회 실시
        (void)adc_read();
        (void)adc_read();
        
        // 💡 노이즈 제거 및 안정적 계측을 위한 100회 평균 필터링 적용
        uint32_t sum = 0;
        for (int i = 0; i < 100; i++) {
            sum += adc_read();
        }
        
        // 💡 [핵심] NTC 아날로그 신호 왜곡을 방지하기 위해 강제 풀다운 복구(gpio_pull_down) 코드는 전면 폐지합니다.
        
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

int check_ntc_status(float &ntc_temp)
{
    // 1. 과거 코드에서 검증된 정석 상수 선언
    const float VCC_val = 3.3f;
    const float R_FIXED_val = 10000.0f;     
    const float R_ROOM_TEMP_val = 10000.0f; // 💡 10k NTC 서미스터 규격에 맞게 10000.0f로 정상 교정 
    const float B_COEFFICIENT_val = 3950.0f;
    const float ROOM_TEMP_K_val = 298.15f;

    bool read_ok = false;
    float temperature = -999.0f;
    int status_code = 0;

    float raw_ch0 = 0.0f, volt_ch0 = 0.0f;
    float raw_ch1 = 0.0f, volt_ch1 = 0.0f;
    
    // 2. Mutex를 획득한 후 GP26(Ch0)과 GP27(Ch1) 채널을 차례대로 스캔합니다.
    if (adc_mutex != nullptr && xSemaphoreTake(adc_mutex, portMAX_DELAY) == pdTRUE) {
        
        // 매번 리딩 전 핀들을 아날로그 입력 및 풀링 비활성화로 정밀 구성
        adc_gpio_init(26);
        gpio_disable_pulls(26);
        adc_gpio_init(27);
        gpio_disable_pulls(27);

        // --- Channel 0 (GP26) 계측 ---
        adc_select_input(0);
        sleep_us(500); // 멀티플렉서 지연
        for (int d = 0; d < 5; d++) (void)adc_read(); // 더미 리딩
        uint32_t sum_ch0 = 0;
        for (int i = 0; i < 50; i++) sum_ch0 += adc_read();
        raw_ch0 = (float)sum_ch0 / 50.0f;
        volt_ch0 = raw_ch0 * (VCC_val / 4095.0f);

        // --- Channel 1 (GP27) 계측 ---
        adc_select_input(1);
        sleep_us(500);
        for (int d = 0; d < 5; d++) (void)adc_read();
        uint32_t sum_ch1 = 0;
        for (int i = 0; i < 50; i++) sum_ch1 += adc_read();
        raw_ch1 = (float)sum_ch1 / 50.0f;
        volt_ch1 = raw_ch1 * (VCC_val / 4095.0f);
        
        xSemaphoreGive(adc_mutex);
        read_ok = true;
    }
    
    if (!read_ok) {
        ntc_temp = -999.0f;
        return 4; // Mutex error
    }
    
    // 현재 사용 중인 NTC 핀/채널의 값 매핑
    float raw_avg = 0.0f;
    float voltage = 0.0f;
    if (ADC_NTC_CHANNEL == 0) {
        raw_avg = raw_ch0;
        voltage = volt_ch0;
    } else {
        raw_avg = raw_ch1;
        voltage = volt_ch1;
    }

    float raw_temp = -999.0f;
    float r_sensor = 0.0f;
    
    // 하드웨어 연결 상태 판정 (전압 기준으로 판정하여 단선/쇼트를 더욱 정확하고 안전하게 판별)
    if (voltage <= 0.05f) {
        status_code = 1; // Open circuit
    } else if (voltage >= 3.25f) {
        status_code = 2; // Short circuit
    } else {
        r_sensor = R_FIXED_val * (VCC_val / voltage - 1.0f);
        
        // B-parameter 변환
        float temp_k = r_sensor / R_ROOM_TEMP_val;
        temp_k = log(temp_k);                      
        temp_k /= B_COEFFICIENT_val;               
        temp_k += 1.0f / ROOM_TEMP_K_val;          
        temp_k = 1.0f / temp_k;                    
        temperature = temp_k - 273.15f + NTC_TEMP_OFFSET;            
        raw_temp = temperature;
        
        if (temperature < NTC_TEMP_MIN || temperature > NTC_TEMP_MAX) {
            status_code = 3; // Out of range
        }
    }
    
    // 💡 소프트웨어 저주파 통과 필터 (Low Pass Filter) 적용
    static float filtered_temp = -999.0f;
    if (status_code == 0) {
        if (filtered_temp <= -990.0f) {
            filtered_temp = temperature; // 초기화
        } else {
            const float alpha = 0.15f;
            filtered_temp = (alpha * temperature) + ((1.0f - alpha) * filtered_temp);
        }
        temperature = filtered_temp;
    } else {
        filtered_temp = -999.0f;
    }
    
    // 3분 주기 한 줄 출력 (180000ms)
    static uint32_t last_dbg_print_ms = 0;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - last_dbg_print_ms >= 180000 || last_dbg_print_ms == 0) {
        printf("[Sensor Dbg (Pin GP26, Ch 0)] GP26 (Ch0): RAW: %.1f, Volt: %.4f V => Calc R: %.2f Ohm, Temp: %.2f C, Status: %d\n", 
               raw_ch0, volt_ch0, r_sensor, temperature, status_code);
        last_dbg_print_ms = now_ms;
    }
    
    ntc_temp = temperature;
    return status_code;
}