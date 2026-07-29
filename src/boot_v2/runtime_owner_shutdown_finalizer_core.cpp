#include "runtime_owner_shutdown_finalizer_core.hpp"

#include <array>

namespace boot_v2 {
namespace {

constexpr std::array<RuntimeOwnerShutdownCleanupStep, 7> kCleanupSteps{{
    RuntimeOwnerShutdownCleanupStep::StopOutputs,
    RuntimeOwnerShutdownCleanupStep::PublishDying,
    RuntimeOwnerShutdownCleanupStep::CloseDeleteSessions,
    RuntimeOwnerShutdownCleanupStep::ScanSessions,
    RuntimeOwnerShutdownCleanupStep::DisconnectPdp,
    RuntimeOwnerShutdownCleanupStep::SetCfun0,
    RuntimeOwnerShutdownCleanupStep::PowerOffModem,
}};

bool urgent_messages_equal(
    const RuntimeOwnerUrgentMessage left,
    const RuntimeOwnerUrgentMessage right) noexcept
{
    return left.source == right.source &&
           left.intent == right.intent &&
           left.producer_sequence == right.producer_sequence &&
           left.incident_correlation_id == right.incident_correlation_id &&
           left.reserved[0] == right.reserved[0] &&
           left.reserved[1] == right.reserved[1];
}

bool initial_usb_is_clean(const UsbPowerObservation observation) noexcept
{
    return usb_power_observation_is_canonical(observation) &&
           observation.cleanup_skipped_mask == 0 &&
           observation.cleanup_timed_out_mask == 0 &&
           observation.hard_deadline_reached == 0;
}

bool is_valid_step_result(
    const RuntimeOwnerShutdownStepResult result) noexcept
{
    return result == RuntimeOwnerShutdownStepResult::Succeeded ||
           result == RuntimeOwnerShutdownStepResult::Failed ||
           result == RuntimeOwnerShutdownStepResult::TimedOut ||
           result == RuntimeOwnerShutdownStepResult::Skipped;
}

} // namespace

RuntimeOwnerShutdownStartResult RuntimeOwnerShutdownFinalizerCore::start(
    const RuntimeOwnerUrgentMessage context,
    const UsbPowerObservation initial_usb,
    const std::uint32_t started_at_monotonic_ms,
    const std::uint32_t hard_deadline_duration_ms) noexcept
{
    if (!runtime_owner_is_canonical_urgent(context) ||
        !usb_power_observation_is_canonical(initial_usb) ||
        hard_deadline_duration_ms == 0) {
        return RuntimeOwnerShutdownStartResult::RejectedInvalid;
    }
    if (phase_ != Phase::Idle) {
        const bool duplicate =
            urgent_messages_equal(context_, context) &&
            usb_power_observations_equal(initial_usb_, initial_usb) &&
            started_at_monotonic_ms_ == started_at_monotonic_ms &&
            hard_deadline_duration_ms_ == hard_deadline_duration_ms;
        return duplicate
                   ? RuntimeOwnerShutdownStartResult::AcceptedDuplicate
                   : RuntimeOwnerShutdownStartResult::RejectedUnsafe;
    }
    if (!initial_usb_is_clean(initial_usb)) {
        return RuntimeOwnerShutdownStartResult::RejectedUnsafe;
    }

    context_ = context;
    initial_usb_ = initial_usb;
    started_at_monotonic_ms_ = started_at_monotonic_ms;
    hard_deadline_duration_ms_ = hard_deadline_duration_ms;
    phase_ = Phase::Running;
    return RuntimeOwnerShutdownStartResult::Started;
}

RuntimeOwnerShutdownDirective RuntimeOwnerShutdownFinalizerCore::next(
    const std::uint32_t now_monotonic_ms) noexcept
{
    if (phase_ == Phase::Idle) {
        return directive(
            RuntimeOwnerShutdownFinalizeAction::Idle,
            RuntimeOwnerShutdownCleanupStep::Invalid,
            0);
    }
    if (phase_ == Phase::WatchdogAllowed) {
        return directive(
            RuntimeOwnerShutdownFinalizeAction::CommitWatchdog,
            RuntimeOwnerShutdownCleanupStep::Invalid,
            0);
    }
    if (phase_ == Phase::Gp15Allowed) {
        return directive(
            RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill,
            RuntimeOwnerShutdownCleanupStep::Invalid,
            0);
    }
    if (phase_ == Phase::AwaitingUsb) {
        return directive(
            RuntimeOwnerShutdownFinalizeAction::RecheckUsb,
            RuntimeOwnerShutdownCleanupStep::Invalid,
            0);
    }

    if (deadline_reached(now_monotonic_ms)) {
        hard_deadline_reached_ = 1;
        phase_ = Phase::AwaitingUsb;
        return directive(
            RuntimeOwnerShutdownFinalizeAction::RecheckUsb,
            RuntimeOwnerShutdownCleanupStep::Invalid,
            0);
    }
    if (step_index_ >= kCleanupSteps.size()) {
        phase_ = Phase::AwaitingUsb;
        return directive(
            RuntimeOwnerShutdownFinalizeAction::RecheckUsb,
            RuntimeOwnerShutdownCleanupStep::Invalid,
            remaining(now_monotonic_ms));
    }
    return directive(
        RuntimeOwnerShutdownFinalizeAction::RunCleanupStep,
        kCleanupSteps[step_index_],
        remaining(now_monotonic_ms));
}

RuntimeOwnerShutdownCompletionResult
RuntimeOwnerShutdownFinalizerCore::complete(
    const RuntimeOwnerShutdownCleanupStep step,
    const RuntimeOwnerShutdownStepResult result,
    const std::uint32_t now_monotonic_ms) noexcept
{
    if (phase_ == Phase::Idle || !is_valid_step_result(result) ||
        runtime_owner_shutdown_step_mask(step) == 0) {
        return RuntimeOwnerShutdownCompletionResult::RejectedInvalid;
    }
    if (phase_ != Phase::Running ||
        deadline_reached(now_monotonic_ms)) {
        if (phase_ == Phase::Running) {
            hard_deadline_reached_ = 1;
            phase_ = Phase::AwaitingUsb;
        }
        return RuntimeOwnerShutdownCompletionResult::RejectedTerminal;
    }
    if (step_index_ >= kCleanupSteps.size() ||
        step != kCleanupSteps[step_index_]) {
        return RuntimeOwnerShutdownCompletionResult::RejectedWrongStep;
    }

    const std::uint8_t mask = runtime_owner_shutdown_step_mask(step);
    if (result == RuntimeOwnerShutdownStepResult::TimedOut) {
        cleanup_timed_out_mask_ =
            static_cast<std::uint8_t>(cleanup_timed_out_mask_ | mask);
    } else if (result == RuntimeOwnerShutdownStepResult::Skipped) {
        cleanup_skipped_mask_ =
            static_cast<std::uint8_t>(cleanup_skipped_mask_ | mask);
    } else if (result == RuntimeOwnerShutdownStepResult::Failed) {
        cleanup_failed_mask_ =
            static_cast<std::uint8_t>(cleanup_failed_mask_ | mask);
    }
    ++step_index_;
    if (step_index_ >= kCleanupSteps.size()) {
        phase_ = Phase::AwaitingUsb;
    }
    return RuntimeOwnerShutdownCompletionResult::Accepted;
}

RuntimeOwnerShutdownUsbResult
RuntimeOwnerShutdownFinalizerCore::submit_usb_recheck(
    const UsbPowerObservation observation) noexcept
{
    if (phase_ == Phase::WatchdogAllowed ||
        phase_ == Phase::Gp15Allowed) {
        return RuntimeOwnerShutdownUsbResult::RejectedTerminal;
    }
    if (phase_ != Phase::AwaitingUsb ||
        !usb_power_observation_is_canonical(observation) ||
        observation.sample_sequence <= initial_usb_.sample_sequence ||
        static_cast<std::int32_t>(
            observation.sampled_at_monotonic_ms -
            initial_usb_.sampled_at_monotonic_ms) < 0 ||
        observation.cleanup_skipped_mask != cleanup_skipped_mask_ ||
        observation.cleanup_timed_out_mask != cleanup_timed_out_mask_ ||
        observation.hard_deadline_reached != hard_deadline_reached_) {
        return RuntimeOwnerShutdownUsbResult::RejectedInvalid;
    }

    if (context_.intent == RuntimeOwnerShutdownIntent::Reboot) {
        phase_ = Phase::WatchdogAllowed;
        return RuntimeOwnerShutdownUsbResult::WatchdogAllowed;
    }
    if (observation.present == 0) {
        phase_ = Phase::Gp15Allowed;
        return RuntimeOwnerShutdownUsbResult::Gp15Allowed;
    }
    phase_ = Phase::WatchdogAllowed;
    return RuntimeOwnerShutdownUsbResult::WatchdogAllowed;
}

bool RuntimeOwnerShutdownFinalizerCore::shutdown_context(
    RuntimeOwnerUrgentMessage &output) const noexcept
{
    if (phase_ == Phase::Idle) {
        return false;
    }
    output = context_;
    return true;
}

RuntimeOwnerShutdownDirective
RuntimeOwnerShutdownFinalizerCore::directive(
    const RuntimeOwnerShutdownFinalizeAction action,
    const RuntimeOwnerShutdownCleanupStep step,
    const std::uint32_t remaining_ms) const noexcept
{
    RuntimeOwnerShutdownDirective result{};
    result.action = action;
    result.step = step;
    result.cleanup_skipped_mask = cleanup_skipped_mask_;
    result.cleanup_timed_out_mask = cleanup_timed_out_mask_;
    result.hard_deadline = hard_deadline_reached_;
    result.cleanup_failed_mask = cleanup_failed_mask_;
    result.initial_usb_present = initial_usb_.present;
    result.remaining_ms = remaining_ms;
    return result;
}

bool RuntimeOwnerShutdownFinalizerCore::deadline_reached(
    const std::uint32_t now_monotonic_ms) const noexcept
{
    return now_monotonic_ms - started_at_monotonic_ms_ >=
           hard_deadline_duration_ms_;
}

std::uint32_t RuntimeOwnerShutdownFinalizerCore::remaining(
    const std::uint32_t now_monotonic_ms) const noexcept
{
    const std::uint32_t elapsed =
        now_monotonic_ms - started_at_monotonic_ms_;
    return elapsed >= hard_deadline_duration_ms_
               ? 0
               : hard_deadline_duration_ms_ - elapsed;
}

} // namespace boot_v2
