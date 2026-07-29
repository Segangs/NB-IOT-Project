#ifndef NB_IOT_BOOT_V2_TEMPERATURE_ALARM_DELIVERY_MAILBOX_HPP
#define NB_IOT_BOOT_V2_TEMPERATURE_ALARM_DELIVERY_MAILBOX_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "temperature_alarm_publish_core.hpp"

namespace boot_v2 {

struct TemperatureAlarmDeliveryEvent {
    std::uint32_t sensor_id{0};
    std::uint32_t snapshot_revision{0};
    TemperatureAlarmTerminalResult result{
        TemperatureAlarmTerminalResult::Invalid};
    TemperatureAlarmEdge edge{TemperatureAlarmEdge::Invalid};
    std::uint16_t reserved{0};
};

[[nodiscard]] constexpr bool
temperature_alarm_delivery_event_is_canonical(
    const TemperatureAlarmDeliveryEvent event) noexcept
{
    const bool sensor_known =
        event.sensor_id == 1 || event.sensor_id == 2;
    const bool result_known =
        event.result == TemperatureAlarmTerminalResult::Succeeded ||
        event.result == TemperatureAlarmTerminalResult::Failed ||
        event.result == TemperatureAlarmTerminalResult::TimedOut ||
        event.result == TemperatureAlarmTerminalResult::Cancelled;
    const bool edge_known =
        event.edge == TemperatureAlarmEdge::Clear ||
        event.edge == TemperatureAlarmEdge::High;
    return sensor_known && event.snapshot_revision != 0 &&
           result_known && edge_known && event.reserved == 0;
}

enum class TemperatureAlarmDeliveryPushResult : std::uint8_t {
    RejectedInvalid = 0,
    Accepted = 1,
    AcceptedDuplicate = 2,
    RejectedFull = 3,
};

enum class TemperatureAlarmDeliveryPopResult : std::uint8_t {
    Empty = 0,
    Popped = 1,
};

struct TemperatureAlarmDeliveryMailboxView {
    std::uint32_t overflow_count{0};
    std::uint8_t depth{0};
    std::uint8_t overflow_latched{0};
    std::uint8_t overflow_log_pending{0};
    std::uint8_t reserved{0};
};

struct TemperatureAlarmDeliveryConsumerView {
    std::uint32_t rejected_completion_count{0};
};

namespace temperature_alarm_delivery_detail {

[[nodiscard]] constexpr std::uint32_t saturating_increment(
    const std::uint32_t value) noexcept
{
    return value == std::numeric_limits<std::uint32_t>::max()
               ? value
               : value + 1;
}

[[nodiscard]] constexpr bool logical_index_is_pending(
    const std::uint32_t logical_index,
    const std::uint32_t read_index,
    const std::uint32_t write_index) noexcept
{
    return logical_index - read_index < write_index - read_index;
}

} // namespace temperature_alarm_delivery_detail

class TemperatureAlarmDeliveryMailbox {
public:
    TemperatureAlarmDeliveryMailbox() noexcept = default;
    TemperatureAlarmDeliveryMailbox(
        const TemperatureAlarmDeliveryMailbox &) = delete;
    TemperatureAlarmDeliveryMailbox &operator=(
        const TemperatureAlarmDeliveryMailbox &) = delete;
    TemperatureAlarmDeliveryMailbox(
        TemperatureAlarmDeliveryMailbox &&) = delete;
    TemperatureAlarmDeliveryMailbox &operator=(
        TemperatureAlarmDeliveryMailbox &&) = delete;
    ~TemperatureAlarmDeliveryMailbox() noexcept = default;

    [[nodiscard]] static constexpr std::uint8_t capacity() noexcept
    {
        return kCapacity;
    }

    [[nodiscard]] TemperatureAlarmDeliveryPushResult try_push(
        TemperatureAlarmDeliveryEvent event) noexcept;
    [[nodiscard]] TemperatureAlarmDeliveryPopResult try_pop(
        TemperatureAlarmDeliveryEvent &event) noexcept;
    [[nodiscard]] TemperatureAlarmDeliveryMailboxView view() const noexcept;
    [[nodiscard]] bool take_overflow_log_pending() noexcept;

private:
    [[nodiscard]] static constexpr bool events_equal(
        const TemperatureAlarmDeliveryEvent left,
        const TemperatureAlarmDeliveryEvent right) noexcept
    {
        return left.sensor_id == right.sensor_id &&
               left.snapshot_revision == right.snapshot_revision &&
               left.result == right.result && left.edge == right.edge &&
               left.reserved == right.reserved;
    }

    void record_overflow() noexcept;

    static constexpr std::uint8_t kCapacity = 16;
    std::array<TemperatureAlarmDeliveryEvent, kCapacity> slots_{};
    alignas(4) std::atomic<std::uint32_t> read_index_{0};
    alignas(4) std::atomic<std::uint32_t> write_index_{0};
    alignas(4) std::atomic<std::uint32_t> overflow_count_{0};
    std::atomic<std::uint8_t> overflow_latched_{0};
    std::atomic<std::uint8_t> overflow_log_pending_{0};
};

class TemperatureAlarmDeliveryConsumerCore {
public:
    [[nodiscard]] bool apply(
        TemperatureAlarmDeliveryEvent event,
        TemperatureAlarmPublishCore &channel0,
        TemperatureAlarmPublishCore &channel1) noexcept;

    [[nodiscard]] TemperatureAlarmDeliveryConsumerView view() const noexcept
    {
        return {rejected_completion_count_};
    }

private:
    std::uint32_t rejected_completion_count_{0};
};

static_assert(sizeof(TemperatureAlarmDeliveryEvent) == 12);
static_assert(alignof(TemperatureAlarmDeliveryEvent) == 4);
static_assert(
    std::is_standard_layout<TemperatureAlarmDeliveryEvent>::value);
static_assert(
    std::is_trivially_copyable<TemperatureAlarmDeliveryEvent>::value);
static_assert(offsetof(TemperatureAlarmDeliveryEvent, sensor_id) == 0);
static_assert(
    offsetof(TemperatureAlarmDeliveryEvent, snapshot_revision) == 4);
static_assert(offsetof(TemperatureAlarmDeliveryEvent, result) == 8);
static_assert(offsetof(TemperatureAlarmDeliveryEvent, edge) == 9);
static_assert(offsetof(TemperatureAlarmDeliveryEvent, reserved) == 10);

static_assert(std::is_same<
              typename std::underlying_type<
                  TemperatureAlarmDeliveryPushResult>::type,
              std::uint8_t>::value);
static_assert(std::is_same<
              typename std::underlying_type<
                  TemperatureAlarmDeliveryPopResult>::type,
              std::uint8_t>::value);
static_assert(sizeof(TemperatureAlarmDeliveryMailboxView) == 8);
static_assert(alignof(TemperatureAlarmDeliveryMailboxView) == 4);
static_assert(
    std::is_standard_layout<
        TemperatureAlarmDeliveryMailboxView>::value);
static_assert(
    std::is_trivially_copyable<
        TemperatureAlarmDeliveryMailboxView>::value);
static_assert(sizeof(TemperatureAlarmDeliveryConsumerView) == 4);
static_assert(alignof(TemperatureAlarmDeliveryConsumerView) == 4);
static_assert(
    std::is_standard_layout<
        TemperatureAlarmDeliveryConsumerView>::value);
static_assert(
    std::is_trivially_copyable<
        TemperatureAlarmDeliveryConsumerView>::value);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_TEMPERATURE_ALARM_DELIVERY_MAILBOX_HPP
