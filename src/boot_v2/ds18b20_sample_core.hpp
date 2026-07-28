#ifndef NB_IOT_BOOT_V2_DS18B20_SAMPLE_CORE_HPP
#define NB_IOT_BOOT_V2_DS18B20_SAMPLE_CORE_HPP

#include <cstdint>

namespace boot_v2 {

enum class Ds18b20RawSampleStatus : std::uint8_t {
    Valid = 0,
    PowerOnSentinel = 1,
    OutOfRange = 2,
};

struct Ds18b20RawSampleDecision {
    Ds18b20RawSampleStatus status{Ds18b20RawSampleStatus::OutOfRange};
    float temperature_celsius{0.0f};
};

[[nodiscard]] constexpr Ds18b20RawSampleDecision
decode_ds18b20_raw_sample(const std::int16_t raw) noexcept
{
    constexpr std::int16_t kPowerOnSentinelRaw = 0x0550;
    constexpr std::int16_t kMinimumRaw = -55 * 16;
    constexpr std::int16_t kMaximumRaw = 125 * 16;

    if (raw == kPowerOnSentinelRaw) {
        return {Ds18b20RawSampleStatus::PowerOnSentinel, 0.0f};
    }
    if (raw < kMinimumRaw || raw > kMaximumRaw) {
        return {Ds18b20RawSampleStatus::OutOfRange, 0.0f};
    }
    return {
        Ds18b20RawSampleStatus::Valid,
        static_cast<float>(raw) / 16.0f,
    };
}

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_DS18B20_SAMPLE_CORE_HPP
