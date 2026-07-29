#ifndef NB_IOT_BOOT_V2_TEMPERATURE_ALARM_PUBLISH_CORE_HPP
#define NB_IOT_BOOT_V2_TEMPERATURE_ALARM_PUBLISH_CORE_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace boot_v2 {

enum class TemperatureAlarmEdge : std::uint8_t {
    Invalid = 0,
    Clear = 1,
    High = 2,
};

enum class TemperatureAlarmTerminalResult : std::uint8_t {
    Invalid = 0,
    Succeeded = 1,
    Failed = 2,
    TimedOut = 3,
    Cancelled = 4,
};

struct TemperatureAlarmPublishDecision {
    std::uint32_t snapshot_revision{0};
    TemperatureAlarmEdge edge{TemperatureAlarmEdge::Invalid};
    std::uint8_t publish_required{0};
    std::int16_t value_deci_celsius{0};
};

class TemperatureAlarmPublishCore {
public:
    [[nodiscard]] TemperatureAlarmPublishDecision observe(
        bool update_allowed,
        bool alarm_high,
        std::int16_t value_deci_celsius) noexcept;

    [[nodiscard]] bool mark_enqueued(
        std::uint32_t snapshot_revision,
        TemperatureAlarmEdge edge) noexcept;

    // Compatibility entry point for the staged sensor-reader migration.
    // Queue acceptance marks the current offer in-flight; it never confirms
    // physical delivery.
    void confirm_submitted() noexcept;

    [[nodiscard]] bool apply_completion(
        std::uint32_t snapshot_revision,
        TemperatureAlarmEdge edge,
        TemperatureAlarmTerminalResult result) noexcept;

private:
    [[nodiscard]] std::uint32_t allocate_revision() noexcept;
    [[nodiscard]] TemperatureAlarmPublishDecision current_offer() const
        noexcept;
    void begin_offer(
        TemperatureAlarmEdge edge,
        std::int16_t value_deci_celsius) noexcept;
    void clear_offer() noexcept;
    void renew_offer_revision() noexcept;

    bool observation_initialized_{false};
    bool latest_observed_high_{false};
    bool delivered_high_{false};
    bool offer_pending_{false};
    bool in_flight_{false};
    TemperatureAlarmEdge pending_edge_{TemperatureAlarmEdge::Invalid};
    std::int16_t pending_value_deci_celsius_{0};
    std::uint32_t pending_revision_{0};
    std::uint32_t next_revision_{1};
};

static_assert(sizeof(TemperatureAlarmPublishDecision) == 8);
static_assert(alignof(TemperatureAlarmPublishDecision) == 4);
static_assert(
    std::is_standard_layout<TemperatureAlarmPublishDecision>::value);
static_assert(
    std::is_trivially_copyable<TemperatureAlarmPublishDecision>::value);
static_assert(
    offsetof(TemperatureAlarmPublishDecision, snapshot_revision) == 0);
static_assert(offsetof(TemperatureAlarmPublishDecision, edge) == 4);
static_assert(
    offsetof(TemperatureAlarmPublishDecision, publish_required) == 5);
static_assert(
    offsetof(TemperatureAlarmPublishDecision, value_deci_celsius) == 6);
static_assert(
    std::is_same<
        decltype(TemperatureAlarmPublishDecision::value_deci_celsius),
        std::int16_t>::value);

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_TEMPERATURE_ALARM_PUBLISH_CORE_HPP
