#include "liveness_service.hpp"

namespace boot_v2 {

LivenessServiceCommandResult PostConfigLivenessService::submit(
    const LivenessServiceCommand command) noexcept
{
    if (!command.attempt.valid()) {
        return LivenessServiceCommandResult::Rejected;
    }

    if (command.kind == LivenessServiceCommandKind::BeginAfterConfigApply) {
        if (gate_.status() != LivenessGateStatus::NotStarted &&
            command.attempt.config_apply_epoch <=
                gate_.active_attempt().config_apply_epoch) {
            return LivenessServiceCommandResult::Rejected;
        }
        if (!gate_.begin_after_config_apply(command.attempt)) {
            return LivenessServiceCommandResult::Rejected;
        }
        return LivenessServiceCommandResult::AcceptedWaiting;
    }

    LivenessSignal signal = LivenessSignal::AtOk;
    switch (command.kind) {
    case LivenessServiceCommandKind::AtOk:
        signal = LivenessSignal::AtOk;
        break;
    case LivenessServiceCommandKind::SameSessionPubAck:
        signal = LivenessSignal::SameSessionPubAck;
        break;
    case LivenessServiceCommandKind::SubscriptionAlive:
        signal = LivenessSignal::SubscriptionAlive;
        break;
    case LivenessServiceCommandKind::FollowupConfigReceived:
        signal = LivenessSignal::FollowupConfigReceived;
        break;
    case LivenessServiceCommandKind::Invalid:
    case LivenessServiceCommandKind::BeginAfterConfigApply:
    default:
        return LivenessServiceCommandResult::Rejected;
    }

    if (command.attempt != gate_.active_attempt()) {
        return LivenessServiceCommandResult::Rejected;
    }
    if (!gate_.observe(signal, command.attempt)) {
        return LivenessServiceCommandResult::Rejected;
    }
    return gate_.passed_for(command.attempt)
               ? LivenessServiceCommandResult::AcceptedPassed
               : LivenessServiceCommandResult::AcceptedWaiting;
}

LivenessGateStatus PostConfigLivenessService::status() const noexcept
{
    return gate_.status();
}

LivenessAttemptToken PostConfigLivenessService::active_attempt() const noexcept
{
    return gate_.active_attempt();
}

bool PostConfigLivenessService::passed_for(
    const LivenessAttemptToken attempt) const noexcept
{
    return gate_.passed_for(attempt);
}

LivenessServiceCommandResult PostConfigLivenessCommandBoundary::submit(
    const LivenessServiceCommand command) noexcept
{
    return service_.submit(command);
}

LivenessGateStatus PostConfigLivenessCommandBoundary::status() const noexcept
{
    return service_.status();
}

LivenessAttemptToken
PostConfigLivenessCommandBoundary::active_attempt() const noexcept
{
    return service_.active_attempt();
}

bool PostConfigLivenessCommandBoundary::passed_for(
    const LivenessAttemptToken attempt) const noexcept
{
    return service_.passed_for(attempt);
}

} // namespace boot_v2
