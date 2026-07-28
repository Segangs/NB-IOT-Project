#ifndef CONFIG_H
#define CONFIG_H

// ====================================================================================
// [1] UART & Modem Pin Configurations
// ====================================================================================
#define UART_ID           uart0
#define BAUD_RATE         115200
#define UART_TX_PIN       0
#define UART_RX_PIN       1
#define MODEM_WAKEUP_PIN  2
#define MODEM_RESET_PIN   3
#define PWR_ON_PIN        4

// ====================================================================================
// [2] I2C & LCD Configurations
// ====================================================================================
#define I2C_PORT          i2c0
#define LCD_ADDR          0x27
#define LCD_ADDR_ALT      0x3F
#define SDA_PIN           16      // LCD SDA through BSS138 level shifter
#define SCL_PIN           17      // LCD SCL through BSS138 level shifter
#define LCD_POWER_STABILIZE_DELAY_MS 5000

// ====================================================================================
// [3] Sensor Configurations
// ====================================================================================
#define MIC_I2S_BCLK_PIN  18      // Shared I2S BCLK, RJ45 ports 1 and 2
#define MIC_I2S_LRCLK_PIN 19      // Shared I2S LRCLK, RJ45 ports 1 and 2
#define MIC1_DOUT_PIN     20      // SPH0645LM4H DOUT, RJ45 Port 1
#define MIC2_DOUT_PIN     21      // SPH0645LM4H DOUT, RJ45 Port 2
#define TEMP1_SENSOR_PIN  22      // DS18B20 DATA, RJ45 Port 1
#define TEMP2_SENSOR_PIN  26      // DS18B20 DATA, RJ45 Port 2
#define ENABLE_DS18B20_READ 1
#define ENABLE_TEMP1_DS18B20 1
#define ENABLE_TEMP2_DS18B20 1
#define DS18B20_BOOT_DELAY_MS 30000
#define DS18B20_SAMPLE_INTERVAL_MS 30000
#define TEMP1_CAL_OFFSET_C 5.0f
#define TEMP2_CAL_OFFSET_C 0.0f
#define ADC_VSYS_CHANNEL  3       // VSYS measurement (usually ADC channel 3 on RP2040/RP2350)

// ====================================================================================
// [4] Networking & Server Settings
// ====================================================================================
#ifndef APN_NAME
#define APN_NAME          "YOUR_APN_NAME_PLACEHOLDER"
#endif

#ifndef SUPABASE_HOST
#define SUPABASE_HOST     "YOUR_SUPABASE_HOST_PLACEHOLDER"
#endif

#ifndef SUPABASE_PORT
#define SUPABASE_PORT     "443"
#endif

#ifndef SUPABASE_ANON_KEY
#define SUPABASE_ANON_KEY "YOUR_SUPABASE_ANON_KEY_PLACEHOLDER"
#endif

#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST  "YOUR_MQTT_BROKER_HOST_PLACEHOLDER"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT  "8883"
#endif

#ifndef MQTT_DEVICE_ID
#define MQTT_DEVICE_ID     ""
#endif

#ifndef MQTT_USERNAME
#define MQTT_USERNAME      ""
#endif

#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD      ""
#endif

// ====================================================================================
// [5] Timing & Frequencies (Units: minutes)
// ====================================================================================
#define MODEM_RSSI_CHECK_INTERVAL_MIN    5
#define SENSOR_TEMP_CHECK_INTERVAL_MIN   20

// ====================================================================================
// [6] External Power Detect Configurations
// ====================================================================================
#define POWER_ADAPTER_DETECT_PIN         7
#define POWER_ADAPTER_PRESENT_LEVEL      1
#define POWER_INT_PIN                    14
#define POWER_KILL_PIN                   15
#define POWER_KILL_INACTIVE_LEVEL        1
#define POWER_KILL_ACTIVE_LEVEL          0
#define POWER_INT_DEBOUNCE_MS 500
#define POWER_ADAPTER_DEBOUNCE_MS 1000
#define POWER_ADAPTER_SHUTDOWN_COMMIT_MS 210000
#define POWER_ADAPTER_ABSOLUTE_OFF_MS 300000
#define SHUTDOWN_HARD_DEADLINE_MS 90000
#define MODEM_POWEROFF_FINALIZE_RESERVE_MS 1000

// ====================================================================================
// [7] Self-Diagnostic Thresholds
// ====================================================================================
#define VSYS_VOLTAGE_MIN  3.0f    // Minimum stable Pico input voltage (V)
#define VSYS_VOLTAGE_MAX  5.5f    // Maximum stable Pico input voltage (V)
#define CHIP_TEMP_MIN    -10.0f   // Minimum safe internal chip temperature (C)
#define CHIP_TEMP_MAX     85.0f   // Maximum safe internal chip temperature (C)

// DS18B20 Sensor Diagnostic Thresholds
#define DS18B20_TEMP_MIN -55.0f
#define DS18B20_TEMP_MAX 125.0f

// ====================================================================================
// [8] LED Configurations
// ====================================================================================
#define STATUS_LED_RED_PIN              8
#define STATUS_LED_GREEN_PIN            9
#define RJ45_PORT1_TEMP_LED_PIN         10
#define RJ45_PORT1_MIC_LED_PIN          11
#define RJ45_PORT2_TEMP_LED_PIN         12
#define RJ45_PORT2_MIC_LED_PIN          13
#define MODEM_TXON_INPUT_PIN            5
#define TXON_LED_PIN                    28

// ====================================================================================
// [9] Buzzer Configurations
// ====================================================================================
#define BUZZER_PIN                      6
#define DEFAULT_TEMP_UPPER_LIMIT        -9.0f

#endif // CONFIG_H
