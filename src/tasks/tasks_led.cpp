#include "tasks_led.hpp"

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/gpio.h"
#include "../config.h"
#include "app_context.hpp"

// ====================================================================================
// Core 0 Task: PCB Status LED Controller
// ====================================================================================
void vStatusLedTask(void *pvParameters)
{
    const uint rj45_led_pins[] = {
        RJ45_PORT1_TEMP_LED_PIN,
        RJ45_PORT1_MIC_LED_PIN,
        RJ45_PORT2_TEMP_LED_PIN,
        RJ45_PORT2_MIC_LED_PIN
    };

    gpio_init(STATUS_LED_RED_PIN);
    gpio_set_dir(STATUS_LED_RED_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_RED_PIN, 0);

    gpio_init(STATUS_LED_GREEN_PIN);
    gpio_set_dir(STATUS_LED_GREEN_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_GREEN_PIN, 0);

    gpio_init(TXON_LED_PIN);
    gpio_set_dir(TXON_LED_PIN, GPIO_OUT);
    gpio_put(TXON_LED_PIN, 1);

    for (uint pin : rj45_led_pins)
    {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }

    gpio_init(MODEM_TXON_INPUT_PIN);
    gpio_set_dir(MODEM_TXON_INPUT_PIN, GPIO_IN);
    gpio_pull_up(MODEM_TXON_INPUT_PIN);

    gpio_init(POWER_ADAPTER_DETECT_PIN);
    gpio_set_dir(POWER_ADAPTER_DETECT_PIN, GPIO_IN);
    gpio_pull_down(POWER_ADAPTER_DETECT_PIN);

    bool red_on = false;
    TickType_t last_red_toggle = xTaskGetTickCount();
    const TickType_t red_blink_interval = pdMS_TO_TICKS(500);
    const TickType_t sensor_blink_time = pdMS_TO_TICKS(120);
    uint32_t last_ch0_seq = g_temp_ch0_sample_seq;
    uint32_t last_ch1_seq = g_temp_ch1_sample_seq;
    TickType_t ch0_blink_until = 0;
    TickType_t ch1_blink_until = 0;
    const TickType_t txon_sw_blink_interval = pdMS_TO_TICKS(100);

    while (true)
    {
        TickType_t now = xTaskGetTickCount();
        bool adapter_present = gpio_get(POWER_ADAPTER_DETECT_PIN) == POWER_ADAPTER_PRESENT_LEVEL;
        (void)adapter_present;
        // GP7 external-power detect is disabled until the PCB divider circuit is connected.
        // Re-enable with: lcd_params.is_battery_mode = !adapter_present;
        lcd_params.is_battery_mode = false;

        if (lcd_params.is_booting)
        {
            if ((now - last_red_toggle) >= red_blink_interval)
            {
                red_on = !red_on;
                gpio_put(STATUS_LED_RED_PIN, red_on ? 1 : 0);
                last_red_toggle = now;
            }
            gpio_put(STATUS_LED_GREEN_PIN, 0);
        }
        else
        {
            red_on = false;
            gpio_put(STATUS_LED_RED_PIN, 0);
            gpio_put(STATUS_LED_GREEN_PIN, 1);
        }

        // Keep GP28 on normally and blink it during firmware-known TX windows.
        bool txon_sw_blink_on = !lcd_params.is_transmitting ||
                                ((now / txon_sw_blink_interval) % 2 != 0);
        gpio_put(TXON_LED_PIN, txon_sw_blink_on ? 1 : 0);

        bool port1_temp_ok = (g_temp_ch0_sample_seq > 0) && (lcd_params.status_ch0 == 0);
        bool port2_temp_ok = (g_temp_ch1_sample_seq > 0) && (g_sensor_count >= 2) && (lcd_params.status_ch1 == 0);

        if (g_temp_ch0_sample_seq != last_ch0_seq)
        {
            last_ch0_seq = g_temp_ch0_sample_seq;
            ch0_blink_until = now + sensor_blink_time;
        }
        if (g_temp_ch1_sample_seq != last_ch1_seq)
        {
            last_ch1_seq = g_temp_ch1_sample_seq;
            ch1_blink_until = now + sensor_blink_time;
        }

        gpio_put(RJ45_PORT1_TEMP_LED_PIN, (port1_temp_ok && now >= ch0_blink_until) ? 1 : 0);
        gpio_put(RJ45_PORT2_TEMP_LED_PIN, (port2_temp_ok && now >= ch1_blink_until) ? 1 : 0);
        gpio_put(RJ45_PORT1_MIC_LED_PIN, g_mic1_stream_active ? 1 : 0);
        gpio_put(RJ45_PORT2_MIC_LED_PIN, g_mic2_stream_active ? 1 : 0);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
