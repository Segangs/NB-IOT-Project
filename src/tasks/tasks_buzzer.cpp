#include "tasks_buzzer.hpp"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "../config.h"
#include "../lib/log.hpp"
#include "app_context.hpp"

// ====================================================================================
// Buzzer Control Helpers (8002A speaker amplifier input on GP6 using PWM)
// ====================================================================================
void buzzer_stop(uint pin)
{
    uint slice_num = pwm_gpio_to_slice_num(pin);
    pwm_set_enabled(slice_num, false);
    
    // Reconfigure pin as standard GPIO output and force low (0V) to suppress static noise
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
}

void buzzer_set_frequency(uint pin, uint32_t frequency)
{
    if (frequency == 0)
    {
        buzzer_stop(pin);
        return;
    }

    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin);
    uint chan = pwm_gpio_to_channel(pin);

    // Get system clock frequency safely
    uint32_t sys_clk = clock_get_hz(clk_sys);
    if (sys_clk == 0) {
        sys_clk = 150000000; // Fallback to RP2350 standard 150MHz
    }

    float div = 125.0f;
    uint32_t wrap = sys_clk / (div * frequency);
    if (wrap > 65535) wrap = 65535;

    pwm_set_clkdiv(slice_num, div);
    pwm_set_wrap(slice_num, wrap);
    pwm_set_chan_level(slice_num, chan, wrap / 10); // 10% duty cycle to significantly reduce current draw
    pwm_set_enabled(slice_num, true);
}

struct Note {
    uint32_t freq;
    uint32_t duration;
};

// ====================================================================================
// Core 0 Task: Buzzer Controller Task (Core 0)
// ====================================================================================
void vBuzzerTask(void *pvParameters)
{
    // Initialize speaker PWM pin as simple GPIO to start in off state
    gpio_init(BUZZER_PIN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);
    gpio_put(BUZZER_PIN, 0);

    // Ding-Dong (Mi-Do) Melody Configuration
    // Ding (E5): 659 Hz, Dong (C5): 523 Hz (Lower than Mi)
    const Note ding_dong[] = {
        {659, 200},  // Ding (E5)
        {0,   50},   // Brief gap between Ding and Dong
        {523, 400},  // Dong (C5) - Lower pitch
        {0,   600}   // Silent delay before next Ding-Dong
    };
    const int num_notes = sizeof(ding_dong) / sizeof(ding_dong[0]);

    LOG("BUZZER_READY\n");

    while (true)
    {
        // Wait until temperature exceeds the upper limit (g_buzzer_trigger == true)
        if (!g_buzzer_trigger)
        {
            vTaskDelay(pdMS_TO_TICKS(100)); // Poll every 100ms
            continue;
        }

        LOG("BUZZER_ON\n");
        g_buzzer_active = true;

        for (int repeat = 0; repeat < 5; repeat++)
        {
            for (int i = 0; i < num_notes; i++)
            {
                // Play note using hardware PWM
                buzzer_set_frequency(BUZZER_PIN, ding_dong[i].freq);
                vTaskDelay(pdMS_TO_TICKS(ding_dong[i].duration));

                // Very brief gap to distinguish rapid same notes
                buzzer_set_frequency(BUZZER_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }

        g_buzzer_active = false;
        LOG("BUZZER_COOLDOWN\n");
        buzzer_stop(BUZZER_PIN);

        // Sleep for 1 minute (60 seconds = 60,000 ms) before checking trigger status again
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
