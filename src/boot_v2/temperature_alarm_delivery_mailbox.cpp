#include "temperature_alarm_delivery_mailbox.hpp"

#include <limits>

namespace boot_v2 {

TemperatureAlarmDeliveryPushResult
TemperatureAlarmDeliveryMailbox::try_push(
    const TemperatureAlarmDeliveryEvent event) noexcept
{
    if (!temperature_alarm_delivery_event_is_canonical(event)) {
        return TemperatureAlarmDeliveryPushResult::RejectedInvalid;
    }

    const std::uint32_t write =
        write_index_.load(std::memory_order_relaxed);
    std::uint32_t read =
        read_index_.load(std::memory_order_acquire);
    std::uint32_t depth = write - read;
    for (std::uint32_t offset = 0; offset < depth; ++offset) {
        const std::uint32_t logical_index = read + offset;
        const std::size_t index =
            static_cast<std::size_t>(logical_index % kCapacity);
        if (events_equal(slots_[index], event)) {
            const std::uint32_t latest_read =
                read_index_.load(std::memory_order_acquire);
            if (temperature_alarm_delivery_detail::
                    logical_index_is_pending(
                        logical_index, latest_read, write)) {
                return TemperatureAlarmDeliveryPushResult::
                    AcceptedDuplicate;
            }
        }
    }

    read = read_index_.load(std::memory_order_acquire);
    depth = write - read;
    if (depth == kCapacity) {
        record_overflow();
        return TemperatureAlarmDeliveryPushResult::RejectedFull;
    }

    slots_[static_cast<std::size_t>(write % kCapacity)] = event;
    write_index_.store(write + 1, std::memory_order_release);
    return TemperatureAlarmDeliveryPushResult::Accepted;
}

TemperatureAlarmDeliveryPopResult
TemperatureAlarmDeliveryMailbox::try_pop(
    TemperatureAlarmDeliveryEvent &event) noexcept
{
    const std::uint32_t read =
        read_index_.load(std::memory_order_relaxed);
    const std::uint32_t write =
        write_index_.load(std::memory_order_acquire);
    if (read == write) {
        return TemperatureAlarmDeliveryPopResult::Empty;
    }

    event = slots_[static_cast<std::size_t>(read % kCapacity)];
    read_index_.store(read + 1, std::memory_order_release);
    return TemperatureAlarmDeliveryPopResult::Popped;
}

TemperatureAlarmDeliveryMailboxView
TemperatureAlarmDeliveryMailbox::view() const noexcept
{
    const std::uint32_t write =
        write_index_.load(std::memory_order_acquire);
    const std::uint32_t read =
        read_index_.load(std::memory_order_acquire);
    return {
        overflow_count_.load(std::memory_order_relaxed),
        static_cast<std::uint8_t>(write - read),
        overflow_latched_.load(std::memory_order_relaxed),
        overflow_log_pending_.load(std::memory_order_relaxed),
        0,
    };
}

bool TemperatureAlarmDeliveryMailbox::take_overflow_log_pending() noexcept
{
    return overflow_log_pending_.exchange(
               0, std::memory_order_acq_rel) != 0;
}

void TemperatureAlarmDeliveryMailbox::record_overflow() noexcept
{
    const std::uint32_t count =
        overflow_count_.load(std::memory_order_relaxed);
    overflow_count_.store(
        temperature_alarm_delivery_detail::saturating_increment(count),
        std::memory_order_relaxed);
    if (overflow_latched_.exchange(1, std::memory_order_relaxed) == 0) {
        overflow_log_pending_.store(1, std::memory_order_release);
    }
}

bool TemperatureAlarmDeliveryConsumerCore::apply(
    const TemperatureAlarmDeliveryEvent event,
    TemperatureAlarmPublishCore &channel0,
    TemperatureAlarmPublishCore &channel1) noexcept
{
    bool applied = false;
    if (temperature_alarm_delivery_event_is_canonical(event)) {
        TemperatureAlarmPublishCore &channel =
            event.sensor_id == 1 ? channel0 : channel1;
        applied = channel.apply_completion(
            event.snapshot_revision, event.edge, event.result);
    }
    if (!applied) {
        rejected_completion_count_ =
            temperature_alarm_delivery_detail::saturating_increment(
                rejected_completion_count_);
    }
    return applied;
}

} // namespace boot_v2
