#ifndef NB_IOT_BOOT_V2_MQTT_POWER_EVENT_CODEC_HPP
#define NB_IOT_BOOT_V2_MQTT_POWER_EVENT_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace boot_v2 {

struct MqttPowerEvent {
    std::uint8_t event_type{0};
    std::uint32_t incident_id{0};
    std::uint32_t sequence{0};
    std::uint8_t state_code{0};
    std::int32_t value0{0};
    std::int32_t value1{0};
    std::uint32_t unix_seconds{0};
    std::uint8_t clock_valid{0};
};

[[nodiscard]] bool mqtt_power_event_is_canonical(
    MqttPowerEvent event) noexcept;
[[nodiscard]] bool mqtt_power_event_build(
    MqttPowerEvent event,
    char *output,
    std::size_t output_capacity) noexcept;

static_assert(std::is_standard_layout<MqttPowerEvent>::value);
static_assert(std::is_trivially_copyable<MqttPowerEvent>::value);

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_MQTT_POWER_EVENT_CODEC_HPP
