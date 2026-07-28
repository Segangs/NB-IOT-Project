#ifndef NB_IOT_BOOT_V2_POWER_STATE_RUNTIME_HPP
#define NB_IOT_BOOT_V2_POWER_STATE_RUNTIME_HPP

namespace boot_v2 {

void power_state_set_battery_grace(bool active) noexcept;
[[nodiscard]] bool power_state_battery_grace_active() noexcept;

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_POWER_STATE_RUNTIME_HPP
