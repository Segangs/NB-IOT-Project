#include "tasks_lcd.hpp"
#include "../boot_v2/lcd_status_policy.hpp"
#include "../config.h"
#include "../lib/log.hpp"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

// RSSI signal icons definitions
std::array<uint8_t, 8> RSSI_ANT = {0b11111, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
std::array<uint8_t, 8> RSSI01  = {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11000, 0b11000};
std::array<uint8_t, 8> RSSI12  = {0b00000, 0b00000, 0b00000, 0b00000, 0b00011, 0b00011, 0b11011, 0b11011};
std::array<uint8_t, 8> RSSI03  = {0b00000, 0b00000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000};
std::array<uint8_t, 8> RSSI34  = {0b00011, 0b00011, 0b11011, 0b11011, 0b11011, 0b11011, 0b11011, 0b11011};

extern int g_sensor_count;

void lcd_custom_init(LCD_I2C *lcd)
{
    lcd->BacklightOn();
    lcd->CreateCustomChar(0, RSSI_ANT);
    lcd->CreateCustomChar(1, RSSI01);
    lcd->CreateCustomChar(2, RSSI12);
    lcd->CreateCustomChar(3, RSSI03);
    lcd->CreateCustomChar(4, RSSI34);
}

void lcd_display_print(LCD_I2C *lcd, const char *line1, const char *line2)
{
    char buf1[13]; // 12 characters + NULL
    char buf2[13];

    // Left align and pad up to 12 chars to avoid overwriting rightmost 4 indicator spaces
    snprintf(buf1, sizeof(buf1), "%-12.12s", line1);
    lcd->SetCursor(0, 0);
    lcd->PrintString(buf1);

    snprintf(buf2, sizeof(buf2), "%-12.12s", line2);
    lcd->SetCursor(1, 0);
    lcd->PrintString(buf2);
}

void lcd_draw_rssi(LCD_I2C *lcd, int csq)
{
    // Render standard antenna icon at Row 0, Col 13
    lcd->SetCursor(0, 13);
    lcd->PrintCustomChar(0);

    // Render bars at Row 0, Col 14-15
    lcd->SetCursor(0, 14);
    if (csq == 99 || csq == 0) {
        lcd->PrintString("x ");
    } else if (csq >= 24) {
        lcd->PrintString("\x02\x04"); // 4 bars
    } else if (csq >= 18) {
        lcd->PrintString("\x02\x03"); // 3 bars
    } else if (csq >= 8) {
        lcd->PrintString("\x02 ");    // 2 bars
    } else if (csq >= 0) {
        lcd->PrintString("\x01 ");    // 1 bar
    } else {
        lcd->PrintString("e ");        // error
    }
}

void lcd_antenna_animation_step(LCD_I2C *lcd)
{
    static int frame = 0;
    
    lcd->SetCursor(0, 13);
    lcd->PrintCustomChar(0);
    
    int anim_stage = (frame % 4);
    lcd->SetCursor(0, 14);
    
    if (anim_stage == 0) {
        lcd->PrintString("\x01 "); // 1 bar
    } else if (anim_stage == 1) {
        lcd->PrintString("\x02 "); // 2 bars
    } else if (anim_stage == 2) {
        lcd->PrintString("\x02\x03"); // 3 bars
    } else if (anim_stage == 3) {
        lcd->PrintString("\x02\x04"); // 4 bars
    }
    
    frame++;
}

void lcd_tx_animation_step(LCD_I2C *lcd)
{
    static int tx_frame = 0;
    lcd->SetCursor(1, 13);
    
    int stage = (tx_frame % 6);
    if (stage == 0) {
        lcd->PrintString(">  ");
    } else if (stage == 1) {
        lcd->PrintString(">> ");
    } else if (stage == 2) {
        lcd->PrintString(">>>");
    } else if (stage == 3) {
        lcd->PrintString(" >>");
    } else if (stage == 4) {
        lcd->PrintString("  >");
    } else if (stage == 5) {
        lcd->PrintString("   ");
    }
    
    tx_frame++;
}

void vLcdTask(void *pvParameters)
{
    LcdTaskParams *params = (LcdTaskParams *)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(LCD_POWER_STABILIZE_DELAY_MS));

    const uint8_t lcd_addr = LCD_ADDR;
    LOG("LCD_INIT_START 0x%02X\n", lcd_addr);
    static LCD_I2C lcd_device(
        lcd_addr, 16, 2, I2C_PORT, SDA_PIN, SCL_PIN);
    LOG("LCD_INIT_DONE 0x%02X\n", lcd_addr);
    LCD_I2C *const lcd = &lcd_device;
    params->lcd = lcd;
    
    // Perform custom loading of graphics
    lcd_custom_init(lcd);
    
    uint32_t loop_counter = 0;
    
    while (true)
    {
        // 1. Render Text Content based on booting phase status
        if (params->is_booting) {
            // Pin 'Boot..' to top row, place detailed diagnostic text on bottom row, hide temp
            lcd_display_print(lcd, "Boot..", params->status_text);
        } else {
            // Normal operational layout
            char temp_str[16];
            if (params->is_unauthenticated) {
                // MQTT 인증 실패 시 온도를 표시하는 하단 줄에 "Unauth" 고정 출력
                snprintf(temp_str, sizeof(temp_str), "Unauth");
            } else {
                bool t1_ok = params->status_ch0 == 0 && params->current_temperature > -990.0f;
                bool t2_ok = params->status_ch1 == 0 && params->current_temperature_ch1 > -990.0f;

                if (t1_ok && t2_ok) {
                    snprintf(temp_str, sizeof(temp_str), "%.1f %.1f\xDF""C",
                             params->current_temperature, params->current_temperature_ch1);
                } else if (t1_ok) {
                    snprintf(temp_str, sizeof(temp_str), "%.1f\xDF""C", params->current_temperature);
                } else if (t2_ok) {
                    snprintf(temp_str, sizeof(temp_str), "T2: %.1f\xDF""C", params->current_temperature_ch1);
                } else {
                    int err_code = params->status_ch0 != 0 ? params->status_ch0 : params->status_ch1;
                    const char* err_name = "Limit";
                    if (err_code == 1) err_name = "Cut";
                    else if (err_code == 2) err_name = "CRC";
                    else if (err_code == 5) err_name = "Low";
                    snprintf(temp_str, sizeof(temp_str), "T1/T2: %s", err_name);
                }
            }

            const char *const status_line =
                boot_v2::lcd_normal_status_line(
                    params->is_battery_mode,
                    params->status_text);
            lcd_display_print(lcd, status_line, temp_str);
        }
        
        // 3. Network RSSI status / animation (Row 0, Chars 13-15)
        // RSSI animation runs every 400ms (so run once every 4 iterations of 100ms delay)
        if (params->is_searching_network) {
            if (loop_counter % 4 == 0) {
                lcd_antenna_animation_step(lcd);
            }
        } else {
            lcd_draw_rssi(lcd, params->current_csq);
        }
        
        // 4. Transmission indicator animation (Row 1, Chars 13-15)
        // TX animation flows every 300ms (so run once every 3 iterations of 100ms delay)
        if (params->is_transmitting) {
            if (loop_counter % 3 == 0) {
                lcd_tx_animation_step(lcd);
            }
        } else {
            // Clear rightmost 3 chars of row 1 when inactive
            lcd->SetCursor(1, 13);
            lcd->PrintString("   ");
        }
        
        loop_counter++;
        vTaskDelay(pdMS_TO_TICKS(100)); // Sleep for 100ms
    }
}
