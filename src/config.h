#ifndef CONFIG_H
#define CONFIG_H

// ====================================================================================
// [1] UART & Modem Pin Configurations
// ====================================================================================
#define UART_ID           uart0
#define BAUD_RATE         115200
#define UART_TX_PIN       0
#define UART_RX_PIN       1
#define PWR_ON_PIN        15

// ====================================================================================
// [2] I2C & LCD Configurations
// ====================================================================================
#define I2C_PORT          i2c0
#define LCD_ADDR          0x27
#define SDA_PIN           20
#define SCL_PIN           21

// ====================================================================================
// [3] Sensor (ADC) Configurations
// ====================================================================================
#define ADC_NTC_PIN       26      // NTC Thermistor ADC pin (GPIO 26 / ADC 0) 
#define ADC_NTC_CHANNEL   0
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
#define MQTT_BROKER_HOST  "p.zxcx.io"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT  "8883"
#endif

// ====================================================================================
// [5] Timing & Frequencies (Units: minutes)
// ====================================================================================
#define MODEM_RSSI_CHECK_INTERVAL_MIN    5
#define SENSOR_TEMP_CHECK_INTERVAL_MIN   20

// ====================================================================================
// [6] Self-Diagnostic Thresholds
// ====================================================================================
#define VSYS_VOLTAGE_MIN  3.0f    // Minimum stable Pico input voltage (V)
#define VSYS_VOLTAGE_MAX  5.5f    // Maximum stable Pico input voltage (V)
#define CHIP_TEMP_MIN    -10.0f   // Minimum safe internal chip temperature (C)
#define CHIP_TEMP_MAX     85.0f   // Maximum safe internal chip temperature (C)

// NTC Sensor Diagnostic Thresholds
#define NTC_TEMP_MIN     -50.0f
#define NTC_TEMP_MAX     120.0f
#define NTC_ADC_SHORT     150      // ADC value below this is considered a Short circuit (0V ~ 0.12V)
#define NTC_ADC_OPEN      3900    // ADC value above this is considered an Open circuit (3.14V ~ 3.3V)
#define NTC_TEMP_OFFSET   -3.8f     // NTC sensor software calibration offset (Celsius)

// ====================================================================================
// [7] Buzzer Configurations
// ====================================================================================
#define BUZZER_PIN                      16
#define DEFAULT_TEMP_UPPER_LIMIT        -9.0f

#endif // CONFIG_H
