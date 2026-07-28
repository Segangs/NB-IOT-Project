#ifndef NB_IOT_BOOT_V2_STATUS_LED_POLICY_HPP
#define NB_IOT_BOOT_V2_STATUS_LED_POLICY_HPP

#include <cstdint>

namespace boot_v2 {

struct StatusLedPolicyInput {
    std::uint32_t now_ms{0};
    bool booting{false};
    bool power_button_shutdown{false};
    bool battery_grace{false};
    bool modem_tx_active{false};
};

struct StatusLedOutputs {
    std::uint8_t red{0};
    std::uint8_t green{0};
    std::uint8_t modem_tx{0};
};

[[nodiscard]] constexpr StatusLedOutputs status_led_outputs(
    const StatusLedPolicyInput input) noexcept
{
    const bool attention =
        input.booting ||
        input.power_button_shutdown ||
        input.battery_grace;
    const bool red_phase_on =
        ((input.now_ms / 500U) % 2U) != 0U;

    return {
        static_cast<std::uint8_t>(
            attention && red_phase_on ? 1 : 0),
        static_cast<std::uint8_t>(attention ? 0 : 1),
        static_cast<std::uint8_t>(
            input.modem_tx_active ? 0 : 1),
    };
}

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_STATUS_LED_POLICY_HPP
