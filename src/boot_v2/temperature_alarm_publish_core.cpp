#include "temperature_alarm_publish_core.hpp"

namespace boot_v2 {
namespace {

bool edge_is_valid(const TemperatureAlarmEdge edge) noexcept
{
    return edge == TemperatureAlarmEdge::Clear ||
           edge == TemperatureAlarmEdge::High;
}

bool result_is_terminal(
    const TemperatureAlarmTerminalResult result) noexcept
{
    return result == TemperatureAlarmTerminalResult::Succeeded ||
           result == TemperatureAlarmTerminalResult::Failed ||
           result == TemperatureAlarmTerminalResult::TimedOut ||
           result == TemperatureAlarmTerminalResult::Cancelled;
}

} // namespace

TemperatureAlarmPublishDecision TemperatureAlarmPublishCore::observe(
    const bool update_allowed,
    const bool alarm_high,
    const std::int16_t value_deci_celsius) noexcept
{
    if (!update_allowed) {
        return {};
    }

    latest_observed_high_ = alarm_high;
    if (!observation_initialized_) {
        observation_initialized_ = true;
        if (!alarm_high) {
            return {};
        }
        begin_offer(TemperatureAlarmEdge::High, value_deci_celsius);
    }

    if (offer_pending_) {
        return in_flight_ ? TemperatureAlarmPublishDecision{}
                          : current_offer();
    }

    if (latest_observed_high_ == delivered_high_) {
        return {};
    }

    begin_offer(
        latest_observed_high_ ? TemperatureAlarmEdge::High
                              : TemperatureAlarmEdge::Clear,
        value_deci_celsius);
    return current_offer();
}

bool TemperatureAlarmPublishCore::mark_enqueued(
    const std::uint32_t snapshot_revision,
    const TemperatureAlarmEdge edge) noexcept
{
    if (!offer_pending_ || in_flight_ ||
        snapshot_revision == 0 ||
        snapshot_revision != pending_revision_ ||
        !edge_is_valid(edge) ||
        edge != pending_edge_) {
        return false;
    }

    in_flight_ = true;
    return true;
}

void TemperatureAlarmPublishCore::confirm_submitted() noexcept
{
    if (!offer_pending_ || in_flight_) {
        return;
    }

    (void)mark_enqueued(pending_revision_, pending_edge_);
}

bool TemperatureAlarmPublishCore::apply_completion(
    const std::uint32_t snapshot_revision,
    const TemperatureAlarmEdge edge,
    const TemperatureAlarmTerminalResult result) noexcept
{
    if (!offer_pending_ || !in_flight_ ||
        snapshot_revision == 0 ||
        snapshot_revision != pending_revision_ ||
        !edge_is_valid(edge) ||
        edge != pending_edge_ ||
        !result_is_terminal(result)) {
        return false;
    }

    if (result == TemperatureAlarmTerminalResult::Succeeded) {
        delivered_high_ = edge == TemperatureAlarmEdge::High;
        clear_offer();
    } else {
        in_flight_ = false;
        renew_offer_revision();
    }
    return true;
}

std::uint32_t TemperatureAlarmPublishCore::allocate_revision() noexcept
{
    const std::uint32_t revision = next_revision_;
    ++next_revision_;
    if (next_revision_ == 0) {
        next_revision_ = 1;
    }
    return revision;
}

TemperatureAlarmPublishDecision
TemperatureAlarmPublishCore::current_offer() const noexcept
{
    return {
        pending_revision_,
        pending_edge_,
        1,
        pending_value_deci_celsius_,
    };
}

void TemperatureAlarmPublishCore::begin_offer(
    const TemperatureAlarmEdge edge,
    const std::int16_t value_deci_celsius) noexcept
{
    offer_pending_ = true;
    in_flight_ = false;
    pending_edge_ = edge;
    pending_value_deci_celsius_ = value_deci_celsius;
    pending_revision_ = allocate_revision();
}

void TemperatureAlarmPublishCore::clear_offer() noexcept
{
    offer_pending_ = false;
    in_flight_ = false;
    pending_edge_ = TemperatureAlarmEdge::Invalid;
    pending_value_deci_celsius_ = 0;
    pending_revision_ = 0;
}

void TemperatureAlarmPublishCore::renew_offer_revision() noexcept
{
    pending_revision_ = allocate_revision();
}

} // namespace boot_v2
