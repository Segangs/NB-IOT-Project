#include "power_state_core.hpp"

#include <limits>

namespace boot_v2 {
namespace {

constexpr PowerTimingPolicy kDefaultPolicy{};

bool valid_policy(const PowerTimingPolicy policy) noexcept
{
    return policy.debounce_ms != 0 &&
           policy.commit_ms > policy.debounce_ms &&
           policy.absolute_off_ms > policy.commit_ms;
}

std::uint32_t elapsed(
    const std::uint32_t now,
    const std::uint32_t since) noexcept
{
    return now - since;
}

} // namespace

PowerStateCore::PowerStateCore(const PowerTimingPolicy policy) noexcept
    : policy_(valid_policy(policy) ? policy : kDefaultPolicy)
{
}

PowerStateDecision PowerStateCore::observe(
    const bool adapter_present,
    const std::uint32_t now_ms) noexcept
{
    switch (state_) {
    case PowerStateKind::ExternalPower:
        if (!adapter_present) {
            state_ = PowerStateKind::DebouncingLoss;
            transition_started_ms_ = now_ms;
        }
        break;

    case PowerStateKind::DebouncingLoss:
        if (adapter_present) {
            state_ = PowerStateKind::ExternalPower;
        } else if (
            elapsed(now_ms, transition_started_ms_) >= policy_.debounce_ms) {
            open_incident(transition_started_ms_ + policy_.debounce_ms);
        }
        break;

    case PowerStateKind::Grace:
        if (elapsed(now_ms, grace_started_ms_) >= policy_.commit_ms) {
            commit();
        } else if (adapter_present) {
            state_ = PowerStateKind::DebouncingRestore;
            transition_started_ms_ = now_ms;
        }
        break;

    case PowerStateKind::DebouncingRestore:
        if (elapsed(now_ms, grace_started_ms_) >= policy_.commit_ms) {
            commit();
        } else if (!adapter_present) {
            state_ = PowerStateKind::Grace;
        } else if (
            elapsed(now_ms, transition_started_ms_) >= policy_.debounce_ms) {
            state_ = PowerStateKind::ExternalPower;
            restored_pending_ = 1;
        }
        break;

    case PowerStateKind::Committed:
        break;
    }

    return decision(now_ms);
}

bool PowerStateCore::acknowledge(const PowerStateAction action) noexcept
{
    switch (action) {
    case PowerStateAction::PublishAdapterRemoved:
        if (removed_pending_ == 0) {
            return false;
        }
        removed_pending_ = 0;
        return true;

    case PowerStateAction::PublishAdapterRestored:
        if (restored_pending_ == 0) {
            return false;
        }
        restored_pending_ = 0;
        incident_id_ = 0;
        grace_started_ms_ = 0;
        return true;

    case PowerStateAction::CommitShutdown:
        if (commit_pending_ == 0) {
            return false;
        }
        commit_pending_ = 0;
        return true;

    case PowerStateAction::None:
    default:
        return false;
    }
}

PowerStateDecision PowerStateCore::decision(
    const std::uint32_t now_ms) const noexcept
{
    PowerStateDecision result{};
    result.state = state_;
    result.battery_grace_active =
        state_ == PowerStateKind::Grace ||
                state_ == PowerStateKind::DebouncingRestore ||
                state_ == PowerStateKind::Committed
            ? 1
            : 0;
    result.shutdown_committed =
        state_ == PowerStateKind::Committed ? 1 : 0;
    result.incident_id = incident_id_;

    if (commit_pending_ != 0) {
        result.action = PowerStateAction::CommitShutdown;
        result.sequence = 2;
    } else if (removed_pending_ != 0) {
        result.action = PowerStateAction::PublishAdapterRemoved;
        result.sequence = 1;
    } else if (restored_pending_ != 0) {
        result.action = PowerStateAction::PublishAdapterRestored;
        result.sequence = 2;
    }

    if (incident_id_ != 0) {
        const std::uint32_t elapsed_ms =
            elapsed(now_ms, grace_started_ms_);
        result.elapsed_seconds = elapsed_ms / 1000;
        if (elapsed_ms < policy_.absolute_off_ms) {
            result.remaining_seconds =
                (policy_.absolute_off_ms - elapsed_ms + 999) / 1000;
        }
    }
    return result;
}

void PowerStateCore::open_incident(
    const std::uint32_t grace_started_ms) noexcept
{
    state_ = PowerStateKind::Grace;
    grace_started_ms_ = grace_started_ms;
    std::uint32_t candidate =
        grace_started_ms_ == 0 ? 1 : grace_started_ms_;
    if (candidate == last_incident_id_) {
        candidate =
            candidate == std::numeric_limits<std::uint32_t>::max()
                ? 1
                : candidate + 1;
    }
    incident_id_ = candidate;
    last_incident_id_ = candidate;
    removed_pending_ = 1;
    restored_pending_ = 0;
    commit_pending_ = 0;
}

void PowerStateCore::commit() noexcept
{
    state_ = PowerStateKind::Committed;
    commit_pending_ = 1;
}

} // namespace boot_v2
