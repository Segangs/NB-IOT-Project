#ifndef TASKS_LCD_HPP
#define TASKS_LCD_HPP

#include "../../lib/LCD_I2C.hpp"
#include <array>
#include <stdint.h>

// RSSI signal icons (Custom Characters definitions)
extern std::array<uint8_t, 8> RSSI_ANT;
extern std::array<uint8_t, 8> RSSI01;
extern std::array<uint8_t, 8> RSSI12;
extern std::array<uint8_t, 8> RSSI03;
extern std::array<uint8_t, 8> RSSI34;

// Initialize LCD custom characters and configure backlight
void lcd_custom_init(LCD_I2C *lcd);

// Print formatted text on two rows (left aligned, auto-padded to 12 chars to preserve status animations space)
void lcd_display_print(LCD_I2C *lcd, const char *line1, const char *line2);

// Draw the signal antenna and bar columns based on the CSQ RSSI value
void lcd_draw_rssi(LCD_I2C *lcd, int csq);

// Step through the antenna loading animation (used when searching for a network carrier)
void lcd_antenna_animation_step(LCD_I2C *lcd);

// Step through the transmit data flow animation (">  ", ">> ", ">>>", " >>", "  >", "   ") on bottom-right (chars 13-15, row 1)
void lcd_tx_animation_step(LCD_I2C *lcd);

// Shared state struct for the LCD task parameter
struct LcdTaskParams {
    LCD_I2C *lcd;
    volatile int current_csq;
    volatile bool is_searching_network;
    volatile bool is_transmitting;
    volatile bool is_booting;
    volatile bool is_modem_busy; // Core 0 & Core 1 UART 경합 방지용 플래그
    volatile bool is_unauthenticated; // MQTT 인증 실패 상태 플래그
    volatile float current_temperature; // Ch0
    volatile float current_temperature_ch1; // Ch1
    volatile int status_ch0;
    volatile int status_ch1;
    volatile float current_vsys_voltage;
    volatile bool is_vsys_stable;
    char status_text[16];
};

// FreeRTOS Task function running at regular intervals to update the display
void vLcdTask(void *pvParameters);

#endif // TASKS_LCD_HPP
