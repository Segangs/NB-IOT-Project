#ifndef NB_IOT_BOOT_V2_POWER_STATE_CORE_HPP
#define NB_IOT_BOOT_V2_POWER_STATE_CORE_HPP

#include <cstdint>
#include <type_traits>

namespace boot_v2 {

struct PowerTimingPolicy {
    std::uint32_t debounce_ms{1000};
    std::uint32_t commit_ms{210000};
    std::uint32_t absolute_off_ms{300000};
};

enum class PowerStateKind : std::uint8_t {
    ExternalPower = 0,
    DebouncingLoss = 1,
    Grace = 2,
    DebouncingRestore = 3,
    Committed = 4,
};

enum class PowerStateAction : std::uint8_t {
    None = 0,
    PublishAdapterRemoved = 1,
    PublishAdapterRestored = 2,
    CommitShutdown = 3,
};

[[nodiscard]] constexpr bool power_state_action_submit_allowed(
    const PowerStateAction action,
    const bool runtime_ready) noexcept
{
    if (action == PowerStateAction::None) {
        return false;
    }
    if (action == PowerStateAction::CommitShutdown) {
        return true;
    }
    return runtime_ready;
}

struct PowerStateDecision {
    PowerStateKind state{PowerStateKind::ExternalPower};
    PowerStateAction action{PowerStateAction::None};
    std::uint8_t battery_grace_active{0};
    std::uint8_t shutdown_committed{0};
    std::uint32_t incident_id{0};
    std::uint32_t sequence{0};
    std::uint32_t elapsed_seconds{0};
    std::uint32_t remaining_seconds{0};
};

class PowerStateCore {
public:
    PowerStateCore() noexcept = default;
    explicit PowerStateCore(PowerTimingPolicy policy) noexcept;

    [[nodiscard]] PowerStateDecision observe(
        bool adapter_present,
        std::uint32_t now_ms) noexcept;
    [[nodiscard]] bool acknowledge(PowerStateAction action) noexcept;

private:
    [[nodiscard]] PowerStateDecision decision(
        std::uint32_t now_ms) const noexcept;
    void open_incident(std::uint32_t grace_started_ms) noexcept;
    void commit() noexcept;

    PowerTimingPolicy policy_{};
    PowerStateKind state_{PowerStateKind::ExternalPower};
    std::uint32_t transition_started_ms_{0};
    std::uint32_t grace_started_ms_{0};
    std::uint32_t incident_id_{0};
    std::uint32_t last_incident_id_{0};
    std::uint8_t removed_pending_{0};
    std::uint8_t restored_pending_{0};
    std::uint8_t commit_pending_{0};
};

static_assert(std::is_standard_layout<PowerTimingPolicy>::value);
static_assert(std::is_trivially_copyable<PowerTimingPolicy>::value);
static_assert(std::is_standard_layout<PowerStateDecision>::value);
static_assert(std::is_trivially_copyable<PowerStateDecision>::value);

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_POWER_STATE_CORE_HPP
