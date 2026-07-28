#include "mqtt_power_event_codec.hpp"

#include <cstdio>

namespace boot_v2 {
namespace {

constexpr std::size_t kModemPayloadLimit = 80;

bool canonical_clock(const MqttPowerEvent event) noexcept
{
    return event.clock_valid <= 1 &&
           (event.clock_valid != 0 || event.unix_seconds == 0);
}

} // namespace

bool mqtt_power_event_is_canonical(const MqttPowerEvent event) noexcept
{
    if (event.incident_id == 0 || event.sequence == 0 ||
        !canonical_clock(event)) {
        return false;
    }

    switch (event.event_type) {
    case 4:
        return event.state_code == 1 &&
               event.value0 == 0 &&
               event.value1 == 0;
    case 5:
        return event.state_code == 0 &&
               event.value0 == 1 &&
               event.value1 == 0;
    case 6:
        return event.state_code == 2 &&
               event.value0 >= 0 &&
               event.value0 <= 300 &&
               event.value1 >= 0 &&
               event.value1 <= 300;
    default:
        return false;
    }
}

bool mqtt_power_event_build(
    const MqttPowerEvent event,
    char *const output,
    const std::size_t output_capacity) noexcept
{
    if (output == nullptr || output_capacity == 0) {
        return false;
    }
    output[0] = '\0';
    if (!mqtt_power_event_is_canonical(event)) {
        return false;
    }

    const int written = std::snprintf(
        output,
        output_capacity,
        "[1,%u,%lu,%lu,%u,%ld,%ld,%lu,%u]",
        static_cast<unsigned>(event.event_type),
        static_cast<unsigned long>(event.incident_id),
        static_cast<unsigned long>(event.sequence),
        static_cast<unsigned>(event.state_code),
        static_cast<long>(event.value0),
        static_cast<long>(event.value1),
        static_cast<unsigned long>(event.unix_seconds),
        static_cast<unsigned>(event.clock_valid));
    output[output_capacity - 1] = '\0';
    return written >= 0 &&
           static_cast<std::size_t>(written) < output_capacity &&
           static_cast<std::size_t>(written) <= kModemPayloadLimit;
}

} // namespace boot_v2
