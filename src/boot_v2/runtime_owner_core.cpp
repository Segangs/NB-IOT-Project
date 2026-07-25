#include "runtime_owner_core.hpp"

#include <cstddef>
#include <limits>

namespace boot_v2 {

namespace {

constexpr std::uint32_t kCorrelationBundleSize = 6;
constexpr std::uint8_t kAllLivenessSignals = 0x0f;

constexpr bool all_zero_except_kind(const RuntimeOwnerInput input) noexcept
{
    return input.receipt_kind == RuntimeOwnerEffectKind::None &&
           input.correlation_id == 0 &&
           input.mqtt_session_id == 0 &&
           input.mqtt_generation == 0 &&
           input.config_commit_sequence == 0 &&
           input.config_apply_epoch == 0;
}

} // namespace

RuntimeOwnerTransition RuntimeOwnerCore::submit(
    const RuntimeOwnerInput input) noexcept
{
    const RuntimeOwnerPhase before = phase_;
    if (!input_has_canonical_fields(input)) {
        return rejected(before);
    }

    if (phase_ == RuntimeOwnerPhase::ShutdownCommitted) {
        RuntimeOwnerTransition transition = rejected(before);
        if (input.kind == RuntimeOwnerInputKind::ShutdownCommitted) {
            transition.disposition = RuntimeOwnerDisposition::AcceptedDuplicate;
        }
        return transition;
    }

    if (phase_ == RuntimeOwnerPhase::RecoveryPending &&
        has_last_failure_ && input_equals(input, last_failure_)) {
        RuntimeOwnerTransition transition = rejected(before);
        transition.disposition = RuntimeOwnerDisposition::AcceptedDuplicate;
        return transition;
    }

    if (input.kind == RuntimeOwnerInputKind::ShutdownCommitted) {
        invalidate_authorization(true);
        has_last_failure_ = false;
        last_failure_ = {};
        phase_ = RuntimeOwnerPhase::ShutdownCommitted;
        RuntimeOwnerTransition transition{};
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_before = before;
        transition.phase_after = phase_;
        return transition;
    }

    switch (input.kind) {
    case RuntimeOwnerInputKind::BeginTransportAttempt: {
        if (boot_orchestration_ended_ || fatal_latched_ ||
            (phase_ != RuntimeOwnerPhase::ColdStart &&
             phase_ != RuntimeOwnerPhase::RecoveryPending)) {
            return rejected(before);
        }
        if (mqtt_generation_counter_ ==
            std::numeric_limits<std::uint32_t>::max()) {
            return fail_closed(
                before, RuntimeOwnerFaultCode::CounterSaturation, 0, {});
        }

        const std::uint32_t next_generation = mqtt_generation_counter_ + 1;
        invalidate_authorization(true);
        mqtt_generation_counter_ = next_generation;
        has_last_failure_ = false;
        last_failure_ = {};
        last_fault_ = RuntimeOwnerFaultCode::None;
        phase_ = RuntimeOwnerPhase::TransportConnecting;

        RuntimeOwnerTransition transition{};
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_before = before;
        transition.phase_after = phase_;
        append_effect(
            transition,
            RuntimeOwnerEffectKind::StartTransportAttempt,
            0,
            {0, next_generation, 0},
            RuntimeOwnerFaultCode::None);
        return transition;
    }

    case RuntimeOwnerInputKind::TransportEstablished: {
        if (boot_orchestration_ended_ ||
            phase_ != RuntimeOwnerPhase::TransportConnecting ||
            input.mqtt_generation != mqtt_generation_counter_) {
            return rejected(before);
        }
        active_mqtt_session_id_ = input.mqtt_session_id;
        active_mqtt_generation_ = input.mqtt_generation;
        phase_ = RuntimeOwnerPhase::AwaitingConfigCommit;

        RuntimeOwnerTransition transition{};
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_before = before;
        transition.phase_after = phase_;
        return transition;
    }

    case RuntimeOwnerInputKind::TransportAttemptFailed:
        if (phase_ != RuntimeOwnerPhase::TransportConnecting ||
            input.mqtt_generation != mqtt_generation_counter_) {
            return rejected(before);
        }
        return accept_failure(
            before,
            input,
            RuntimeOwnerFaultCode::TransportFailure,
            0,
            {});

    case RuntimeOwnerInputKind::ConfigActivationCommitted: {
        if (boot_orchestration_ended_ ||
            (phase_ != RuntimeOwnerPhase::AwaitingConfigCommit &&
             phase_ != RuntimeOwnerPhase::LivenessWaiting &&
             phase_ != RuntimeOwnerPhase::SnapshotFreezePending) ||
            input.mqtt_session_id != active_mqtt_session_id_ ||
            input.mqtt_generation != active_mqtt_generation_) {
            return rejected(before);
        }
        if (input.config_commit_sequence == last_config_commit_sequence_) {
            RuntimeOwnerTransition transition = rejected(before);
            transition.disposition = RuntimeOwnerDisposition::AcceptedDuplicate;
            return transition;
        }
        if (input.config_commit_sequence < last_config_commit_sequence_) {
            return rejected(before);
        }
        const std::uint32_t maximum =
            std::numeric_limits<std::uint32_t>::max();
        if (config_apply_epoch_counter_ == maximum ||
            correlation_id_counter_ > maximum - kCorrelationBundleSize) {
            return fail_closed(
                before,
                RuntimeOwnerFaultCode::CounterSaturation,
                0,
                active_attempt_);
        }

        const std::uint32_t next_epoch = config_apply_epoch_counter_ + 1;
        const LivenessAttemptToken next_attempt{
            active_mqtt_session_id_, active_mqtt_generation_, next_epoch};
        const std::uint32_t first_id = correlation_id_counter_ + 1;
        config_apply_epoch_counter_ = next_epoch;
        last_config_commit_sequence_ = input.config_commit_sequence;
        correlation_id_counter_ += kCorrelationBundleSize;
        active_attempt_ = next_attempt;
#if defined(NB_IOT_POST_CONFIG_HANDOFF_TRIAL)
        // Keep the established correlation-id layout even though this trial
        // omits the four liveness operations.  The adapter can therefore
        // distinguish this handoff by its empty liveness mask without
        // weakening the snapshot/end-boot identity contract.
        pending_snapshot_effect_id_ = first_id + 4;
        pending_boot_end_effect_id_ = first_id + 5;
        phase_ = RuntimeOwnerPhase::SnapshotFreezePending;
        has_last_failure_ = false;
        last_failure_ = {};
        last_fault_ = RuntimeOwnerFaultCode::None;

        RuntimeOwnerTransition transition{};
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_before = before;
        transition.phase_after = phase_;
        append_effect(
            transition,
            RuntimeOwnerEffectKind::FreezeBootSnapshot,
            pending_snapshot_effect_id_,
            active_attempt_,
            RuntimeOwnerFaultCode::None);
        return transition;
#else
        const LivenessServiceCommandResult begin_result =
            liveness_boundary_.submit({
                LivenessServiceCommandKind::BeginAfterConfigApply,
                next_attempt,
            });
        if (begin_result != LivenessServiceCommandResult::AcceptedWaiting) {
            return fail_closed(
                before,
                RuntimeOwnerFaultCode::InternalInvariant,
                0,
                active_attempt_);
        }
        accepted_liveness_mask_ = 0;
        pending_snapshot_effect_id_ = first_id + 4;
        pending_boot_end_effect_id_ = first_id + 5;

        const std::array<RuntimeOwnerEffectKind, 4> kinds{
            RuntimeOwnerEffectKind::StartAtProbe,
            RuntimeOwnerEffectKind::StartProbePublish,
            RuntimeOwnerEffectKind::VerifySubscription,
            RuntimeOwnerEffectKind::PullFollowupConfig,
        };
        for (std::size_t index = 0; index < active_tickets_.size(); ++index) {
            active_tickets_[index] = {
                kinds[index], first_id + static_cast<std::uint32_t>(index),
                next_attempt, RuntimeOwnerFaultCode::None};
        }
        phase_ = RuntimeOwnerPhase::LivenessWaiting;
        has_last_failure_ = false;
        last_failure_ = {};
        last_fault_ = RuntimeOwnerFaultCode::None;

        RuntimeOwnerTransition transition{};
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_before = before;
        transition.phase_after = phase_;
        for (const RuntimeOwnerEffect ticket : active_tickets_) {
            append_effect(
                transition,
                ticket.kind,
                ticket.correlation_id,
                ticket.attempt,
                RuntimeOwnerFaultCode::None);
        }
        return transition;
#endif
    }

    case RuntimeOwnerInputKind::LivenessOperationCompleted: {
        if (phase_ != RuntimeOwnerPhase::LivenessWaiting &&
            phase_ != RuntimeOwnerPhase::SnapshotFreezePending) {
            return rejected(before);
        }
        const std::uint8_t index = liveness_index(input.receipt_kind);
        if (index >= active_tickets_.size()) {
            return rejected(before);
        }
        const RuntimeOwnerEffect ticket = active_tickets_[index];
        if (ticket.kind != input.receipt_kind ||
            ticket.correlation_id != input.correlation_id ||
            ticket.attempt.mqtt_session_id != input.mqtt_session_id ||
            ticket.attempt.mqtt_generation != input.mqtt_generation ||
            ticket.attempt.config_apply_epoch != input.config_apply_epoch) {
            return rejected(before);
        }

        const std::uint8_t signal_mask =
            static_cast<std::uint8_t>(1u << index);
        if ((accepted_liveness_mask_ & signal_mask) != 0) {
            RuntimeOwnerTransition transition = rejected(before);
            transition.disposition = RuntimeOwnerDisposition::AcceptedDuplicate;
            return transition;
        }
        if (phase_ != RuntimeOwnerPhase::LivenessWaiting) {
            return rejected(before);
        }
        if (pending_snapshot_effect_id_ == 0 ||
            pending_boot_end_effect_id_ == 0) {
            return fail_closed(
                before,
                RuntimeOwnerFaultCode::InternalInvariant,
                input.correlation_id,
                ticket.attempt);
        }

        const LivenessServiceCommandResult boundary_result =
            liveness_boundary_.submit({
                liveness_command_kind(input.receipt_kind), ticket.attempt});
        if (boundary_result == LivenessServiceCommandResult::Rejected) {
            return fail_closed(
                before,
                RuntimeOwnerFaultCode::InternalInvariant,
                input.correlation_id,
                ticket.attempt);
        }
        accepted_liveness_mask_ = static_cast<std::uint8_t>(
            accepted_liveness_mask_ | signal_mask);

        RuntimeOwnerTransition transition{};
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_before = before;
        transition.phase_after = phase_;
        if (accepted_liveness_mask_ == kAllLivenessSignals) {
            if (boundary_result != LivenessServiceCommandResult::AcceptedPassed ||
                !liveness_boundary_.passed_for(active_attempt_)) {
                return fail_closed(
                    before,
                    RuntimeOwnerFaultCode::InternalInvariant,
                    input.correlation_id,
                    ticket.attempt);
            }
            phase_ = RuntimeOwnerPhase::SnapshotFreezePending;
            transition.phase_after = phase_;
            append_effect(
                transition,
                RuntimeOwnerEffectKind::FreezeBootSnapshot,
                pending_snapshot_effect_id_,
                active_attempt_,
                RuntimeOwnerFaultCode::None);
        } else if (boundary_result !=
                   LivenessServiceCommandResult::AcceptedWaiting) {
            return fail_closed(
                before,
                RuntimeOwnerFaultCode::InternalInvariant,
                input.correlation_id,
                ticket.attempt);
        }
        return transition;
    }

    case RuntimeOwnerInputKind::LivenessOperationFailed:
    case RuntimeOwnerInputKind::DeadlineExpired: {
        if (phase_ != RuntimeOwnerPhase::LivenessWaiting) {
            return rejected(before);
        }
        const std::uint8_t index = liveness_index(input.receipt_kind);
        if (index >= active_tickets_.size()) {
            return rejected(before);
        }
        const RuntimeOwnerEffect ticket = active_tickets_[index];
        const std::uint8_t signal_mask =
            static_cast<std::uint8_t>(1u << index);
        if (ticket.kind != input.receipt_kind ||
            ticket.correlation_id != input.correlation_id ||
            ticket.attempt.mqtt_session_id != input.mqtt_session_id ||
            ticket.attempt.mqtt_generation != input.mqtt_generation ||
            ticket.attempt.config_apply_epoch != input.config_apply_epoch ||
            (accepted_liveness_mask_ & signal_mask) != 0) {
            return rejected(before);
        }
        return accept_failure(
            before,
            input,
            input.kind == RuntimeOwnerInputKind::DeadlineExpired
                ? RuntimeOwnerFaultCode::DeadlineExpired
                : RuntimeOwnerFaultCode::LivenessFailure,
            input.correlation_id,
            ticket.attempt);
    }

    case RuntimeOwnerInputKind::SnapshotFreezeSucceeded: {
        const bool exact =
            input.receipt_kind == RuntimeOwnerEffectKind::FreezeBootSnapshot &&
            input.correlation_id == pending_snapshot_effect_id_ &&
            input.mqtt_session_id == active_attempt_.mqtt_session_id &&
            input.mqtt_generation == active_attempt_.mqtt_generation &&
            input.config_apply_epoch == active_attempt_.config_apply_epoch;
        if (phase_ == RuntimeOwnerPhase::RuntimeReady && exact) {
            RuntimeOwnerTransition transition = rejected(before);
            transition.disposition = RuntimeOwnerDisposition::AcceptedDuplicate;
            return transition;
        }
        if (phase_ != RuntimeOwnerPhase::SnapshotFreezePending || !exact ||
            pending_boot_end_effect_id_ == 0) {
            return rejected(before);
        }
        phase_ = RuntimeOwnerPhase::RuntimeReady;
        boot_orchestration_ended_ = true;

        RuntimeOwnerTransition transition{};
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_before = before;
        transition.phase_after = phase_;
        append_effect(
            transition,
            RuntimeOwnerEffectKind::EndBootOrchestration,
            pending_boot_end_effect_id_,
            active_attempt_,
            RuntimeOwnerFaultCode::None);
        return transition;
    }

    case RuntimeOwnerInputKind::SnapshotFreezeFailed: {
        if (phase_ != RuntimeOwnerPhase::SnapshotFreezePending ||
            input.correlation_id != pending_snapshot_effect_id_ ||
            input.mqtt_session_id != active_attempt_.mqtt_session_id ||
            input.mqtt_generation != active_attempt_.mqtt_generation ||
            input.config_apply_epoch != active_attempt_.config_apply_epoch) {
            return rejected(before);
        }
        return accept_failure(
            before,
            input,
            RuntimeOwnerFaultCode::SnapshotFailure,
            input.correlation_id,
            active_attempt_);
    }

    case RuntimeOwnerInputKind::TransportDisconnected: {
        if ((phase_ != RuntimeOwnerPhase::AwaitingConfigCommit &&
             phase_ != RuntimeOwnerPhase::LivenessWaiting &&
             phase_ != RuntimeOwnerPhase::SnapshotFreezePending &&
             phase_ != RuntimeOwnerPhase::RuntimeReady) ||
            input.mqtt_session_id != active_mqtt_session_id_ ||
            input.mqtt_generation != active_mqtt_generation_) {
            return rejected(before);
        }
        return accept_failure(
            before,
            input,
            RuntimeOwnerFaultCode::TransportDisconnected,
            0,
            active_attempt_);
    }

    case RuntimeOwnerInputKind::CriticalIngressFault:
        if (phase_ > RuntimeOwnerPhase::RuntimeReady) {
            return rejected(before);
        }
        return accept_failure(
            before,
            input,
            RuntimeOwnerFaultCode::CriticalIngress,
            0,
            active_attempt_);

    case RuntimeOwnerInputKind::Invalid:
    case RuntimeOwnerInputKind::ShutdownCommitted:
    default:
        return rejected(before);
    }
}

RuntimeOwnerView RuntimeOwnerCore::view() const noexcept
{
    return {
        phase_,
        active_mqtt_session_id_,
        active_mqtt_generation_,
        mqtt_generation_counter_,
        config_apply_epoch_counter_,
        last_config_commit_sequence_,
        correlation_id_counter_,
        active_attempt_,
        boot_orchestration_ended_,
        last_fault_,
    };
}

bool RuntimeOwnerCore::input_has_canonical_fields(
    const RuntimeOwnerInput input) noexcept
{
    const std::uint8_t kind = static_cast<std::uint8_t>(input.kind);
    const std::uint8_t receipt =
        static_cast<std::uint8_t>(input.receipt_kind);
    if (kind > static_cast<std::uint8_t>(
                   RuntimeOwnerInputKind::ShutdownCommitted) ||
        receipt > static_cast<std::uint8_t>(
                      RuntimeOwnerEffectKind::EnterRecovery)) {
        return false;
    }

    switch (input.kind) {
    case RuntimeOwnerInputKind::Invalid:
    case RuntimeOwnerInputKind::BeginTransportAttempt:
    case RuntimeOwnerInputKind::CriticalIngressFault:
    case RuntimeOwnerInputKind::ShutdownCommitted:
        return all_zero_except_kind(input);

    case RuntimeOwnerInputKind::TransportEstablished:
        return input.receipt_kind == RuntimeOwnerEffectKind::None &&
               input.correlation_id == 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch == 0;

    case RuntimeOwnerInputKind::TransportAttemptFailed:
        return input.receipt_kind ==
                   RuntimeOwnerEffectKind::StartTransportAttempt &&
               input.correlation_id == 0 && input.mqtt_session_id == 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch == 0;

    case RuntimeOwnerInputKind::ConfigActivationCommitted:
        return input.receipt_kind == RuntimeOwnerEffectKind::None &&
               input.correlation_id == 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence != 0 &&
               input.config_apply_epoch == 0;

    case RuntimeOwnerInputKind::LivenessOperationCompleted:
    case RuntimeOwnerInputKind::LivenessOperationFailed:
    case RuntimeOwnerInputKind::DeadlineExpired:
        return is_liveness_effect(input.receipt_kind) &&
               input.correlation_id != 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch != 0;

    case RuntimeOwnerInputKind::SnapshotFreezeSucceeded:
    case RuntimeOwnerInputKind::SnapshotFreezeFailed:
        return input.receipt_kind ==
                   RuntimeOwnerEffectKind::FreezeBootSnapshot &&
               input.correlation_id != 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch != 0;

    case RuntimeOwnerInputKind::TransportDisconnected:
        return input.receipt_kind == RuntimeOwnerEffectKind::None &&
               input.correlation_id == 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch == 0;
    }
    return false;
}

bool RuntimeOwnerCore::input_equals(
    const RuntimeOwnerInput left,
    const RuntimeOwnerInput right) noexcept
{
    return left.kind == right.kind &&
           left.receipt_kind == right.receipt_kind &&
           left.correlation_id == right.correlation_id &&
           left.mqtt_session_id == right.mqtt_session_id &&
           left.mqtt_generation == right.mqtt_generation &&
           left.config_commit_sequence == right.config_commit_sequence &&
           left.config_apply_epoch == right.config_apply_epoch;
}

bool RuntimeOwnerCore::is_liveness_effect(
    const RuntimeOwnerEffectKind kind) noexcept
{
    return kind == RuntimeOwnerEffectKind::StartAtProbe ||
           kind == RuntimeOwnerEffectKind::StartProbePublish ||
           kind == RuntimeOwnerEffectKind::VerifySubscription ||
           kind == RuntimeOwnerEffectKind::PullFollowupConfig;
}

std::uint8_t RuntimeOwnerCore::liveness_index(
    const RuntimeOwnerEffectKind kind) noexcept
{
    switch (kind) {
    case RuntimeOwnerEffectKind::StartAtProbe:
        return 0;
    case RuntimeOwnerEffectKind::StartProbePublish:
        return 1;
    case RuntimeOwnerEffectKind::VerifySubscription:
        return 2;
    case RuntimeOwnerEffectKind::PullFollowupConfig:
        return 3;
    default:
        return 0xff;
    }
}

LivenessServiceCommandKind RuntimeOwnerCore::liveness_command_kind(
    const RuntimeOwnerEffectKind kind) noexcept
{
    switch (kind) {
    case RuntimeOwnerEffectKind::StartAtProbe:
        return LivenessServiceCommandKind::AtOk;
    case RuntimeOwnerEffectKind::StartProbePublish:
        return LivenessServiceCommandKind::SameSessionPubAck;
    case RuntimeOwnerEffectKind::VerifySubscription:
        return LivenessServiceCommandKind::SubscriptionAlive;
    case RuntimeOwnerEffectKind::PullFollowupConfig:
        return LivenessServiceCommandKind::FollowupConfigReceived;
    default:
        return LivenessServiceCommandKind::Invalid;
    }
}

RuntimeOwnerTransition RuntimeOwnerCore::rejected(
    const RuntimeOwnerPhase before) const noexcept
{
    RuntimeOwnerTransition transition{};
    transition.phase_before = before;
    transition.phase_after = phase_;
    return transition;
}

RuntimeOwnerTransition RuntimeOwnerCore::fail_closed(
    const RuntimeOwnerPhase before,
    const RuntimeOwnerFaultCode fault,
    const std::uint32_t source_correlation,
    const LivenessAttemptToken source_attempt) noexcept
{
    invalidate_authorization(true);
    phase_ = RuntimeOwnerPhase::RecoveryPending;
    last_fault_ = fault;
    fatal_latched_ = true;
    has_last_failure_ = false;
    last_failure_ = {};

    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::FailClosed;
    transition.phase_before = before;
    transition.phase_after = phase_;
    append_effect(
        transition,
        RuntimeOwnerEffectKind::RecordFault,
        source_correlation,
        source_attempt,
        fault);
    append_effect(
        transition,
        RuntimeOwnerEffectKind::EnterRecovery,
        source_correlation,
        source_attempt,
        fault);
    return transition;
}

RuntimeOwnerTransition RuntimeOwnerCore::accept_failure(
    const RuntimeOwnerPhase before,
    const RuntimeOwnerInput input,
    const RuntimeOwnerFaultCode fault,
    const std::uint32_t source_correlation,
    const LivenessAttemptToken source_attempt) noexcept
{
    invalidate_authorization(true);
    phase_ = RuntimeOwnerPhase::RecoveryPending;
    last_fault_ = fault;
    fatal_latched_ = false;
    has_last_failure_ = true;
    last_failure_ = input;

    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = before;
    transition.phase_after = phase_;
    append_effect(
        transition,
        RuntimeOwnerEffectKind::RecordFault,
        source_correlation,
        source_attempt,
        fault);
    append_effect(
        transition,
        RuntimeOwnerEffectKind::EnterRecovery,
        source_correlation,
        source_attempt,
        fault);
    return transition;
}

void RuntimeOwnerCore::invalidate_authorization(
    const bool clear_transport) noexcept
{
    if (clear_transport) {
        active_mqtt_session_id_ = 0;
        active_mqtt_generation_ = 0;
    }
    active_attempt_ = {};
    active_tickets_ = {};
    accepted_liveness_mask_ = 0;
    pending_snapshot_effect_id_ = 0;
    pending_boot_end_effect_id_ = 0;
}

void RuntimeOwnerCore::append_effect(
    RuntimeOwnerTransition &transition,
    const RuntimeOwnerEffectKind kind,
    const std::uint32_t correlation_id,
    const LivenessAttemptToken attempt,
    const RuntimeOwnerFaultCode fault) noexcept
{
    if (transition.effect_count >= transition.effects.size()) {
        return;
    }
    transition.effects[transition.effect_count] = {
        kind, correlation_id, attempt, fault};
    ++transition.effect_count;
}

} // namespace boot_v2
