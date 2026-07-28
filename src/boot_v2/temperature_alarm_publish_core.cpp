#include "temperature_alarm_publish_core.hpp"

namespace boot_v2 {

TemperatureAlarmPublishDecision TemperatureAlarmPublishCore::observe(
    const bool update_allowed,
    const bool alarm_high) noexcept
{
    if (!update_allowed) {
        return {};
    }
    if (pending_) {
        return {1, static_cast<std::uint8_t>(pending_high_), {}};
    }
    if (!initialized_) {
        if (!alarm_high) {
            initialized_ = true;
            confirmed_high_ = false;
            return {};
        }
        pending_ = true;
        pending_high_ = true;
        return {1, 1, {}};
    }
    if (alarm_high == confirmed_high_) {
        return {};
    }
    pending_ = true;
    pending_high_ = alarm_high;
    return {1, static_cast<std::uint8_t>(alarm_high), {}};
}

void TemperatureAlarmPublishCore::confirm_submitted() noexcept
{
    if (!pending_) {
        return;
    }
    initialized_ = true;
    confirmed_high_ = pending_high_;
    pending_ = false;
}

} // namespace boot_v2
