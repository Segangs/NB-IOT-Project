#ifndef NB_IOT_BOOT_V2_LCD_STATUS_POLICY_HPP
#define NB_IOT_BOOT_V2_LCD_STATUS_POLICY_HPP

namespace boot_v2 {

[[nodiscard]] constexpr const char *lcd_normal_status_line(
    const bool battery_mode,
    const char *const status_text) noexcept
{
    return battery_mode
        ? "BATT MODE"
        : (status_text == nullptr ? "" : status_text);
}

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_LCD_STATUS_POLICY_HPP
