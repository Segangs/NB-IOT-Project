#ifndef NB_IOT_BOOT_V2_POWER_ADAPTER_PROBE_CORE_HPP
#define NB_IOT_BOOT_V2_POWER_ADAPTER_PROBE_CORE_HPP

#include <cstdint>

namespace boot_v2 {

enum class PowerAdapterProbeKind : std::uint8_t {
    Low = 0,
    Weak = 1,
    Strong = 2,
};

struct PowerAdapterDiagnosticSnapshot {
    std::uint32_t falling_edges{0};
    std::uint32_t rising_edges{0};
    std::uint32_t recovery_us{0};
    bool current_high{false};
    bool floating_high{false};
    bool loaded_high{false};
    bool recovered_high{false};
};

constexpr PowerAdapterProbeKind classify_power_adapter_probe(
    const bool floating_high,
    const bool loaded_high) noexcept
{
    if (!floating_high) {
        return PowerAdapterProbeKind::Low;
    }
    return loaded_high
        ? PowerAdapterProbeKind::Strong
        : PowerAdapterProbeKind::Weak;
}

constexpr bool update_power_adapter_non_strong_latch(
    const bool non_strong_seen,
    const PowerAdapterProbeKind sample) noexcept
{
    return non_strong_seen || sample != PowerAdapterProbeKind::Strong;
}

constexpr std::uint8_t encode_power_adapter_diagnostic_flags(
    const PowerAdapterDiagnosticSnapshot snapshot) noexcept
{
    return static_cast<std::uint8_t>(
        (snapshot.current_high ? 1U : 0U) |
        (snapshot.floating_high ? 2U : 0U) |
        (snapshot.loaded_high ? 4U : 0U) |
        (snapshot.recovered_high ? 8U : 0U));
}

constexpr std::int16_t clamp_power_adapter_recovery_us(
    const std::uint32_t recovery_us) noexcept
{
    return static_cast<std::int16_t>(
        recovery_us > 32767U ? 32767U : recovery_us);
}

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_POWER_ADAPTER_PROBE_CORE_HPP
