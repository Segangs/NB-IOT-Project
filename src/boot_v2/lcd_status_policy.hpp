#ifndef NB_IOT_BOOT_V2_LCD_STATUS_POLICY_HPP
#define NB_IOT_BOOT_V2_LCD_STATUS_POLICY_HPP

#include "sensor_quality_core.hpp"

namespace boot_v2 {

struct LcdTemperatureDisplayState {
    int raw_status{0};
    bool value_valid{false};
    float value_celsius{0.0f};
};

struct LcdSensorDisplayState {
    LcdTemperatureDisplayState channel0{};
    LcdTemperatureDisplayState channel1{};
};

[[nodiscard]] constexpr LcdTemperatureDisplayState
make_lcd_temperature_display_state(
    const int raw_status,
    const SensorQualityDecision &quality) noexcept
{
    const bool value_valid = quality.display_value_valid != 0;
    return {
        raw_status,
        value_valid,
        value_valid
            ? static_cast<float>(quality.display_value_deci_celsius) /
                  10.0f
            : -990.0f - static_cast<float>(raw_status),
    };
}

[[nodiscard]] constexpr LcdSensorDisplayState
make_lcd_sensor_display_state(
    const int raw_status_ch0,
    const SensorQualityDecision &quality_ch0,
    const int raw_status_ch1,
    const SensorQualityDecision &quality_ch1) noexcept
{
    return {
        make_lcd_temperature_display_state(
            raw_status_ch0, quality_ch0),
        make_lcd_temperature_display_state(
            raw_status_ch1, quality_ch1),
    };
}

[[nodiscard]] constexpr const char *lcd_normal_status_line(
    const bool battery_mode,
    const char *const status_text) noexcept
{
    return battery_mode
        ? "BATT MODE"
        : (status_text == nullptr ? "" : status_text);
}

[[nodiscard]] constexpr bool lcd_temperature_value_visible(
    const int,
    const bool display_value_valid) noexcept
{
    return display_value_valid;
}

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_LCD_STATUS_POLICY_HPP
