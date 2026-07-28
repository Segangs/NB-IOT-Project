#ifndef NB_IOT_BOOT_V2_TEMPERATURE_ALARM_PUBLISH_CORE_HPP
#define NB_IOT_BOOT_V2_TEMPERATURE_ALARM_PUBLISH_CORE_HPP

#include <cstdint>
#include <type_traits>

namespace boot_v2 {

struct TemperatureAlarmPublishDecision {
    std::uint8_t publish_required{0};
    std::uint8_t alarm_high{0};
    std::uint8_t reserved[2]{};
};

class TemperatureAlarmPublishCore {
public:
    [[nodiscard]] TemperatureAlarmPublishDecision observe(
        bool update_allowed,
        bool alarm_high) noexcept;
    void confirm_submitted() noexcept;

private:
    bool initialized_{false};
    bool confirmed_high_{false};
    bool pending_{false};
    bool pending_high_{false};
};

static_assert(sizeof(TemperatureAlarmPublishDecision) == 4);
static_assert(alignof(TemperatureAlarmPublishDecision) == 1);
static_assert(
    std::is_standard_layout<TemperatureAlarmPublishDecision>::value);
static_assert(
    std::is_trivially_copyable<TemperatureAlarmPublishDecision>::value);

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_TEMPERATURE_ALARM_PUBLISH_CORE_HPP
