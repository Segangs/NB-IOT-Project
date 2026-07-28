#include "runtime_owner_adapter_core.hpp"

#include <cstddef>
#include <limits>

namespace boot_v2 {

namespace {

void increment_saturating(std::uint32_t &counter) noexcept
{
    if (counter != std::numeric_limits<std::uint32_t>::max()) {
        ++counter;
    }
}

void add_saturating(
    std::uint32_t &counter,
    std::uint32_t amount) noexcept
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    if (amount > maximum - counter) {
        counter = maximum;
    } else {
        counter += amount;
    }
}

constexpr bool is_safety_delivery_effect(
    const RuntimeOwnerEffectKind kind) noexcept
{
    return kind == RuntimeOwnerEffectKind::RecordFault ||
           kind == RuntimeOwnerEffectKind::EnterRecovery;
}

constexpr bool is_delivery_only_effect(
    const RuntimeOwnerEffectKind kind) noexcept
{
    return kind == RuntimeOwnerEffectKind::EndBootOrchestration ||
           is_safety_delivery_effect(kind);
}

constexpr bool is_liveness_start_effect(
    const RuntimeOwnerEffectKind effect) noexcept
{
    return effect == RuntimeOwnerEffectKind::StartAtProbe ||
           effect == RuntimeOwnerEffectKind::StartProbePublish ||
           effect == RuntimeOwnerEffectKind::VerifySubscription ||
           effect == RuntimeOwnerEffectKind::PullFollowupConfig;
}

constexpr bool receipt_requires_physical_inflight(
    const TrustedReceiptKind kind) noexcept
{
    return kind == TrustedReceiptKind::TransportEstablished ||
           kind == TrustedReceiptKind::TransportAttemptFailed ||
           kind == TrustedReceiptKind::OperationCompleted ||
           kind == TrustedReceiptKind::OperationFailed ||
           kind == TrustedReceiptKind::DeadlineExpired ||
           kind == TrustedReceiptKind::SnapshotSucceeded ||
           kind == TrustedReceiptKind::SnapshotFailed;
}

bool physical_inflight_matches_receipt(
    const AdapterDispatch inflight,
    const TrustedReceipt receipt) noexcept
{
    if (inflight.kind != AdapterDispatchKind::CoreEffect ||
        inflight.dispatch_sequence == 0 ||
        inflight.enqueue_sequence != 0) {
        return false;
    }

    const RuntimeOwnerEffect effect = inflight.effect;
    switch (receipt.kind) {
    case TrustedReceiptKind::TransportEstablished:
        return effect.kind ==
                   RuntimeOwnerEffectKind::StartTransportAttempt &&
               effect.correlation_id == 0 &&
               effect.attempt.mqtt_session_id == 0 &&
               effect.attempt.mqtt_generation ==
                   receipt.mqtt_generation &&
               effect.attempt.config_apply_epoch == 0;
    case TrustedReceiptKind::TransportAttemptFailed:
        return effect.kind ==
                   RuntimeOwnerEffectKind::StartTransportAttempt &&
               effect.correlation_id == 0 &&
               effect.attempt.mqtt_session_id == 0 &&
               effect.attempt.mqtt_generation ==
                   receipt.mqtt_generation &&
               effect.attempt.config_apply_epoch == 0;
    case TrustedReceiptKind::OperationCompleted:
    case TrustedReceiptKind::OperationFailed:
    case TrustedReceiptKind::DeadlineExpired:
        return is_liveness_start_effect(effect.kind) &&
               effect.kind == receipt.effect_kind &&
               effect.correlation_id == receipt.correlation_id &&
               effect.attempt.mqtt_session_id ==
                   receipt.mqtt_session_id &&
               effect.attempt.mqtt_generation ==
                   receipt.mqtt_generation &&
               effect.attempt.config_apply_epoch ==
                   receipt.config_apply_epoch;
    case TrustedReceiptKind::SnapshotSucceeded:
    case TrustedReceiptKind::SnapshotFailed:
        return effect.kind == RuntimeOwnerEffectKind::FreezeBootSnapshot &&
               effect.kind == receipt.effect_kind &&
               effect.correlation_id == receipt.correlation_id &&
               effect.attempt.mqtt_session_id ==
                   receipt.mqtt_session_id &&
               effect.attempt.mqtt_generation ==
                   receipt.mqtt_generation &&
               effect.attempt.config_apply_epoch ==
                   receipt.config_apply_epoch;
    default:
        return false;
    }
}

constexpr std::uint8_t liveness_ticket_index(
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

bool exact_config_issued_liveness_ticket(
    const RuntimeOwnerView core,
    const TrustedReceipt receipt,
    std::uint8_t &ticket_index) noexcept
{
    constexpr std::uint32_t kCorrelationBundleSize = 6;
    constexpr std::uint32_t kLivenessTicketCount = 4;
    ticket_index = liveness_ticket_index(receipt.effect_kind);
    if (ticket_index >= kLivenessTicketCount ||
        core.last_correlation_id < kCorrelationBundleSize) {
        return false;
    }

    const std::uint32_t first_ticket_correlation =
        core.last_correlation_id - (kCorrelationBundleSize - 1);
    return receipt.correlation_id ==
               first_ticket_correlation + ticket_index &&
           receipt.mqtt_session_id ==
               core.active_attempt.mqtt_session_id &&
           receipt.mqtt_generation ==
               core.active_attempt.mqtt_generation &&
           receipt.config_apply_epoch ==
               core.active_attempt.config_apply_epoch;
}

bool exact_config_issued_snapshot_token(
    const RuntimeOwnerView core,
    const TrustedReceipt receipt) noexcept
{
    return core.last_correlation_id > 1 &&
           receipt.effect_kind ==
               RuntimeOwnerEffectKind::FreezeBootSnapshot &&
           receipt.correlation_id == core.last_correlation_id - 1 &&
           receipt.mqtt_session_id ==
               core.active_attempt.mqtt_session_id &&
           receipt.mqtt_generation ==
               core.active_attempt.mqtt_generation &&
           receipt.config_apply_epoch ==
               core.active_attempt.config_apply_epoch;
}

constexpr bool runtime_owner_effect_is_zero(
    const RuntimeOwnerEffect effect) noexcept
{
    return effect.kind == RuntimeOwnerEffectKind::None &&
           effect.correlation_id == 0 &&
           effect.attempt.mqtt_session_id == 0 &&
           effect.attempt.mqtt_generation == 0 &&
           effect.attempt.config_apply_epoch == 0 &&
           effect.fault_code == RuntimeOwnerFaultCode::None;
}

constexpr bool runtime_owner_phase_is_known(
    const RuntimeOwnerPhase phase) noexcept
{
    switch (phase) {
    case RuntimeOwnerPhase::ColdStart:
    case RuntimeOwnerPhase::TransportConnecting:
    case RuntimeOwnerPhase::AwaitingConfigCommit:
    case RuntimeOwnerPhase::LivenessWaiting:
    case RuntimeOwnerPhase::SnapshotFreezePending:
    case RuntimeOwnerPhase::RuntimeReady:
    case RuntimeOwnerPhase::RecoveryPending:
    case RuntimeOwnerPhase::ShutdownCommitted:
        return true;
    default:
        return false;
    }
}

constexpr RuntimeOwnerPhase known_phase_or_before(
    const RuntimeOwnerPhase observed,
    const RuntimeOwnerPhase before) noexcept
{
    return runtime_owner_phase_is_known(observed) ? observed : before;
}

constexpr bool trusted_receipts_equal(
    const TrustedReceipt left,
    const TrustedReceipt right) noexcept
{
    return left.kind == right.kind &&
           left.effect_kind == right.effect_kind &&
           left.reserved == right.reserved &&
           left.correlation_id == right.correlation_id &&
           left.mqtt_session_id == right.mqtt_session_id &&
           left.mqtt_generation == right.mqtt_generation &&
           left.config_commit_sequence == right.config_commit_sequence &&
           left.config_apply_epoch == right.config_apply_epoch &&
           left.diagnostic_code == right.diagnostic_code;
}

constexpr bool normal_completions_equal(
    const NormalCompletion left,
    const NormalCompletion right) noexcept
{
    return left.kind == right.kind && left.reserved == right.reserved &&
           left.dispatch_sequence == right.dispatch_sequence &&
           left.enqueue_sequence == right.enqueue_sequence &&
           left.diagnostic_code == right.diagnostic_code;
}

bool canonical_begin_transition(
    const RuntimeOwnerView before,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if ((before.phase != RuntimeOwnerPhase::ColdStart &&
         before.phase != RuntimeOwnerPhase::RecoveryPending) ||
        before.boot_orchestration_ended ||
        before.mqtt_generation_counter ==
            std::numeric_limits<std::uint32_t>::max() ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after !=
            RuntimeOwnerPhase::TransportConnecting ||
        transition.effect_count != 1 ||
        after.phase != transition.phase_after ||
        after.boot_orchestration_ended ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter !=
            before.mqtt_generation_counter + 1 ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt.mqtt_session_id != 0 ||
        after.active_attempt.mqtt_generation != 0 ||
        after.active_attempt.config_apply_epoch != 0 ||
        after.last_fault != RuntimeOwnerFaultCode::None) {
        return false;
    }

    const RuntimeOwnerEffect start = transition.effects[0];
    if (start.kind != RuntimeOwnerEffectKind::StartTransportAttempt ||
        start.correlation_id != 0 ||
        start.attempt.mqtt_session_id != 0 ||
        start.attempt.mqtt_generation !=
            after.mqtt_generation_counter ||
        start.attempt.config_apply_epoch != 0 ||
        start.fault_code != RuntimeOwnerFaultCode::None) {
        return false;
    }
    for (std::size_t index = 1; index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

bool canonical_transport_established_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if (transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != RuntimeOwnerPhase::TransportConnecting ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::AwaitingConfigCommit ||
        transition.effect_count != 0 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != receipt.mqtt_session_id ||
        after.mqtt_generation != receipt.mqtt_generation ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != before.active_attempt ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != before.last_fault) {
        return false;
    }
    for (const RuntimeOwnerEffect effect : transition.effects) {
        if (!runtime_owner_effect_is_zero(effect)) {
            return false;
        }
    }
    return true;
}

bool canonical_transport_attempt_failed_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if (before.phase != RuntimeOwnerPhase::TransportConnecting ||
        before.boot_orchestration_ended ||
        receipt.mqtt_generation != before.mqtt_generation_counter ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::RecoveryPending ||
        transition.effect_count != 2 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != LivenessAttemptToken{} ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != RuntimeOwnerFaultCode::TransportFailure) {
        return false;
    }

    constexpr std::array<RuntimeOwnerEffectKind, 2> expected_kinds{{
        RuntimeOwnerEffectKind::RecordFault,
        RuntimeOwnerEffectKind::EnterRecovery,
    }};
    for (std::size_t index = 0; index < expected_kinds.size(); ++index) {
        const RuntimeOwnerEffect effect = transition.effects[index];
        if (effect.kind != expected_kinds[index] ||
            effect.correlation_id != 0 ||
            effect.attempt != LivenessAttemptToken{} ||
            effect.fault_code != RuntimeOwnerFaultCode::TransportFailure) {
            return false;
        }
    }
    for (std::size_t index = expected_kinds.size();
         index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

bool canonical_transport_disconnected_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    const bool phase_allowed =
        before.phase == RuntimeOwnerPhase::AwaitingConfigCommit ||
        before.phase == RuntimeOwnerPhase::LivenessWaiting ||
        before.phase == RuntimeOwnerPhase::SnapshotFreezePending ||
        before.phase == RuntimeOwnerPhase::RuntimeReady;
    if (!phase_allowed ||
        receipt.mqtt_session_id != before.mqtt_session_id ||
        receipt.mqtt_generation != before.mqtt_generation ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::RecoveryPending ||
        transition.effect_count != 2 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != LivenessAttemptToken{} ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != RuntimeOwnerFaultCode::TransportDisconnected) {
        return false;
    }

    constexpr std::array<RuntimeOwnerEffectKind, 2> expected_kinds{{
        RuntimeOwnerEffectKind::RecordFault,
        RuntimeOwnerEffectKind::EnterRecovery,
    }};
    for (std::size_t index = 0; index < expected_kinds.size(); ++index) {
        const RuntimeOwnerEffect effect = transition.effects[index];
        if (effect.kind != expected_kinds[index] ||
            effect.correlation_id != 0 ||
            effect.attempt != before.active_attempt ||
            effect.fault_code !=
                RuntimeOwnerFaultCode::TransportDisconnected) {
            return false;
        }
    }
    for (std::size_t index = expected_kinds.size();
         index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

bool canonical_operation_completed_transition(
    const RuntimeOwnerView before,
    const bool final_ticket,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    const RuntimeOwnerPhase expected_phase =
        final_ticket ? RuntimeOwnerPhase::SnapshotFreezePending
                     : RuntimeOwnerPhase::LivenessWaiting;
    const std::uint8_t expected_effect_count = final_ticket ? 1 : 0;
    if (before.phase != RuntimeOwnerPhase::LivenessWaiting ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != expected_phase ||
        transition.effect_count != expected_effect_count ||
        after.phase != expected_phase ||
        after.mqtt_session_id != before.mqtt_session_id ||
        after.mqtt_generation != before.mqtt_generation ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != before.active_attempt ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != before.last_fault) {
        return false;
    }

    std::size_t first_unused_effect = 0;
    if (final_ticket) {
        const RuntimeOwnerEffect freeze = transition.effects[0];
        if (before.last_correlation_id == 0 ||
            freeze.kind != RuntimeOwnerEffectKind::FreezeBootSnapshot ||
            freeze.correlation_id != before.last_correlation_id - 1 ||
            freeze.attempt != before.active_attempt ||
            freeze.fault_code != RuntimeOwnerFaultCode::None) {
            return false;
        }
        first_unused_effect = 1;
    }
    for (std::size_t index = first_unused_effect;
         index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

constexpr RuntimeOwnerFaultCode liveness_failure_fault(
    const TrustedReceiptKind kind) noexcept
{
    return kind == TrustedReceiptKind::OperationFailed
        ? RuntimeOwnerFaultCode::LivenessFailure
        : RuntimeOwnerFaultCode::DeadlineExpired;
}

bool canonical_liveness_failure_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    const RuntimeOwnerFaultCode expected_fault =
        liveness_failure_fault(receipt.kind);
    if (before.phase != RuntimeOwnerPhase::LivenessWaiting ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::RecoveryPending ||
        transition.effect_count != 2 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != LivenessAttemptToken{} ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != expected_fault) {
        return false;
    }

    constexpr std::array<RuntimeOwnerEffectKind, 2> expected_kinds{{
        RuntimeOwnerEffectKind::RecordFault,
        RuntimeOwnerEffectKind::EnterRecovery,
    }};
    for (std::size_t index = 0; index < expected_kinds.size(); ++index) {
        const RuntimeOwnerEffect effect = transition.effects[index];
        if (effect.kind != expected_kinds[index] ||
            effect.correlation_id != receipt.correlation_id ||
            effect.attempt != before.active_attempt ||
            effect.fault_code != expected_fault) {
            return false;
        }
    }
    for (std::size_t index = expected_kinds.size();
         index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

bool canonical_snapshot_succeeded_transition(
    const RuntimeOwnerView before,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if (before.phase != RuntimeOwnerPhase::SnapshotFreezePending ||
        before.boot_orchestration_ended ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::RuntimeReady ||
        transition.effect_count != 1 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != before.mqtt_session_id ||
        after.mqtt_generation != before.mqtt_generation ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != before.active_attempt ||
        !after.boot_orchestration_ended ||
        after.last_fault != before.last_fault) {
        return false;
    }

    const RuntimeOwnerEffect end_boot = transition.effects[0];
    if (end_boot.kind != RuntimeOwnerEffectKind::EndBootOrchestration ||
        end_boot.correlation_id != before.last_correlation_id ||
        end_boot.attempt != before.active_attempt ||
        end_boot.fault_code != RuntimeOwnerFaultCode::None) {
        return false;
    }
    for (std::size_t index = 1; index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

bool canonical_snapshot_failed_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if (before.phase != RuntimeOwnerPhase::SnapshotFreezePending ||
        before.boot_orchestration_ended ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::RecoveryPending ||
        transition.effect_count != 2 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != LivenessAttemptToken{} ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != RuntimeOwnerFaultCode::SnapshotFailure) {
        return false;
    }

    constexpr std::array<RuntimeOwnerEffectKind, 2> expected_kinds{{
        RuntimeOwnerEffectKind::RecordFault,
        RuntimeOwnerEffectKind::EnterRecovery,
    }};
    for (std::size_t index = 0; index < expected_kinds.size(); ++index) {
        const RuntimeOwnerEffect effect = transition.effects[index];
        if (effect.kind != expected_kinds[index] ||
            effect.correlation_id != receipt.correlation_id ||
            effect.attempt != before.active_attempt ||
            effect.fault_code != RuntimeOwnerFaultCode::SnapshotFailure) {
            return false;
        }
    }
    for (std::size_t index = expected_kinds.size();
         index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

bool canonical_config_committed_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
#if defined(NB_IOT_POST_CONFIG_HANDOFF_TRIAL)
    constexpr std::uint32_t kCoreCorrelationBundleSize = 6;
    if (before.config_apply_epoch_counter ==
            std::numeric_limits<std::uint32_t>::max() ||
        before.last_correlation_id >
            std::numeric_limits<std::uint32_t>::max() -
                kCoreCorrelationBundleSize ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != RuntimeOwnerPhase::AwaitingConfigCommit ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::SnapshotFreezePending ||
        transition.effect_count != 1 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != before.mqtt_session_id ||
        after.mqtt_generation != before.mqtt_generation ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter + 1 ||
        after.last_config_commit_sequence !=
            receipt.config_commit_sequence ||
        after.last_correlation_id !=
            before.last_correlation_id + kCoreCorrelationBundleSize ||
        after.boot_orchestration_ended != before.boot_orchestration_ended ||
        after.last_fault != RuntimeOwnerFaultCode::None) {
        return false;
    }

    const LivenessAttemptToken expected_attempt{
        before.mqtt_session_id,
        before.mqtt_generation,
        before.config_apply_epoch_counter + 1,
    };
    const RuntimeOwnerEffect freeze = transition.effects[0];
    if (after.active_attempt != expected_attempt ||
        freeze.kind != RuntimeOwnerEffectKind::FreezeBootSnapshot ||
        freeze.correlation_id != before.last_correlation_id + 5 ||
        freeze.attempt != expected_attempt ||
        freeze.fault_code != RuntimeOwnerFaultCode::None) {
        return false;
    }
    for (std::size_t index = 1; index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
#else
    constexpr std::uint32_t kCoreCorrelationBundleSize = 6;
    if (before.config_apply_epoch_counter ==
            std::numeric_limits<std::uint32_t>::max() ||
        before.last_correlation_id >
            std::numeric_limits<std::uint32_t>::max() -
                kCoreCorrelationBundleSize ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != RuntimeOwnerPhase::AwaitingConfigCommit ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::LivenessWaiting ||
        transition.effect_count != 4 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != before.mqtt_session_id ||
        after.mqtt_generation != before.mqtt_generation ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter + 1 ||
        after.last_config_commit_sequence !=
            receipt.config_commit_sequence ||
        after.last_correlation_id !=
            before.last_correlation_id + kCoreCorrelationBundleSize ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != RuntimeOwnerFaultCode::None) {
        return false;
    }

    const LivenessAttemptToken expected_attempt{
        before.mqtt_session_id,
        before.mqtt_generation,
        before.config_apply_epoch_counter + 1,
    };
    if (after.active_attempt != expected_attempt) {
        return false;
    }
    constexpr std::array<RuntimeOwnerEffectKind, 4> expected_kinds{{
        RuntimeOwnerEffectKind::StartAtProbe,
        RuntimeOwnerEffectKind::StartProbePublish,
        RuntimeOwnerEffectKind::VerifySubscription,
        RuntimeOwnerEffectKind::PullFollowupConfig,
    }};
    for (std::size_t index = 0; index < transition.effects.size(); ++index) {
        const RuntimeOwnerEffect effect = transition.effects[index];
        if (effect.kind != expected_kinds[index] ||
            effect.correlation_id !=
                before.last_correlation_id + 1 +
                    static_cast<std::uint32_t>(index) ||
            effect.attempt != expected_attempt ||
            effect.fault_code != RuntimeOwnerFaultCode::None) {
            return false;
        }
    }
    return true;
#endif
}

constexpr bool config_counter_saturation_expected(
    const RuntimeOwnerView before) noexcept
{
#if defined(NB_IOT_POST_CONFIG_HANDOFF_TRIAL)
    constexpr std::uint32_t kCoreCorrelationBundleSize = 2;
#else
    constexpr std::uint32_t kCoreCorrelationBundleSize = 6;
#endif
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    return before.config_apply_epoch_counter == maximum ||
           before.last_correlation_id >
               maximum - kCoreCorrelationBundleSize;
}

bool canonical_config_counter_saturation_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if (!config_counter_saturation_expected(before) ||
        before.phase != RuntimeOwnerPhase::AwaitingConfigCommit ||
        before.boot_orchestration_ended ||
        receipt.mqtt_session_id != before.mqtt_session_id ||
        receipt.mqtt_generation != before.mqtt_generation ||
        receipt.config_commit_sequence <=
            before.last_config_commit_sequence ||
        transition.disposition != RuntimeOwnerDisposition::FailClosed ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::RecoveryPending ||
        transition.effect_count != 2 ||
        after.phase != transition.phase_after ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != LivenessAttemptToken{} ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != RuntimeOwnerFaultCode::CounterSaturation) {
        return false;
    }

    constexpr std::array<RuntimeOwnerEffectKind, 2> expected_kinds{{
        RuntimeOwnerEffectKind::RecordFault,
        RuntimeOwnerEffectKind::EnterRecovery,
    }};
    for (std::size_t index = 0; index < expected_kinds.size(); ++index) {
        const RuntimeOwnerEffect effect = transition.effects[index];
        if (effect.kind != expected_kinds[index] ||
            effect.correlation_id != 0 ||
            effect.attempt != before.active_attempt ||
            effect.fault_code != RuntimeOwnerFaultCode::CounterSaturation) {
            return false;
        }
    }
    for (std::size_t index = expected_kinds.size();
         index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

bool canonical_shutdown_transition(
    const RuntimeOwnerView before,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if (transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::ShutdownCommitted ||
        transition.effect_count != 0 ||
        after.phase != RuntimeOwnerPhase::ShutdownCommitted ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != LivenessAttemptToken{} ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != before.last_fault) {
        return false;
    }
    for (const RuntimeOwnerEffect effect : transition.effects) {
        if (!runtime_owner_effect_is_zero(effect)) {
            return false;
        }
    }
    return true;
}

bool canonical_critical_transition(
    const RuntimeOwnerView before,
    const RuntimeOwnerTransition transition,
    const RuntimeOwnerView after) noexcept
{
    if (before.phase > RuntimeOwnerPhase::RuntimeReady ||
        transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.phase_before != before.phase ||
        transition.phase_after != RuntimeOwnerPhase::RecoveryPending ||
        transition.effect_count != 2 ||
        after.phase != RuntimeOwnerPhase::RecoveryPending ||
        after.mqtt_session_id != 0 || after.mqtt_generation != 0 ||
        after.mqtt_generation_counter != before.mqtt_generation_counter ||
        after.config_apply_epoch_counter !=
            before.config_apply_epoch_counter ||
        after.last_config_commit_sequence !=
            before.last_config_commit_sequence ||
        after.last_correlation_id != before.last_correlation_id ||
        after.active_attempt != LivenessAttemptToken{} ||
        after.boot_orchestration_ended !=
            before.boot_orchestration_ended ||
        after.last_fault != RuntimeOwnerFaultCode::CriticalIngress) {
        return false;
    }

    constexpr std::array<RuntimeOwnerEffectKind, 2> expected_kinds{{
        RuntimeOwnerEffectKind::RecordFault,
        RuntimeOwnerEffectKind::EnterRecovery,
    }};
    for (std::size_t index = 0; index < expected_kinds.size(); ++index) {
        const RuntimeOwnerEffect effect = transition.effects[index];
        if (effect.kind != expected_kinds[index] ||
            effect.correlation_id != 0 ||
            effect.attempt != before.active_attempt ||
            effect.fault_code != RuntimeOwnerFaultCode::CriticalIngress) {
            return false;
        }
    }
    for (std::size_t index = expected_kinds.size();
         index < transition.effects.size(); ++index) {
        if (!runtime_owner_effect_is_zero(transition.effects[index])) {
            return false;
        }
    }
    return true;
}

struct SafetyPairDispatchPlan {
    bool available{false};
    bool uses_terminal_reserve{false};
    std::uint32_t record_fault_sequence{0};
    std::uint32_t enter_recovery_sequence{0};
};

constexpr SafetyPairDispatchPlan plan_safety_pair_dispatch(
    const std::uint32_t last_dispatch_sequence) noexcept
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    if (last_dispatch_sequence <= maximum - 5) {
        return {
            true,
            false,
            last_dispatch_sequence + 1,
            last_dispatch_sequence + 2,
        };
    }
    if (last_dispatch_sequence <= maximum - 2) {
        return {true, true, maximum - 1, maximum};
    }
    return {};
}

constexpr AdapterStepResult canonical_idle_step(
    const RuntimeOwnerPhase phase) noexcept
{
    return {
        AdapterStepAction::Idle,
        RuntimeOwnerDisposition::Rejected,
        phase,
        phase,
        0,
        0,
        0,
    };
}

constexpr bool is_trusted_diagnostic_event(
    const TrustedReceiptKind kind) noexcept
{
    return kind == TrustedReceiptKind::TransportAttemptFailed ||
           kind == TrustedReceiptKind::OperationFailed ||
           kind == TrustedReceiptKind::DeadlineExpired ||
           kind == TrustedReceiptKind::SnapshotFailed ||
           kind == TrustedReceiptKind::TransportDisconnected;
}

} // namespace

NormalSubmitResult RuntimeOwnerNormalPort::submit(
    const NormalIntent input) noexcept
{
    if (owner_ == nullptr) {
        return NormalSubmitResult::RejectedNotReady;
    }
    return owner_->submit_normal(input);
}

UrgentRequestResult RuntimeOwnerShutdownPort::request() noexcept
{
    if (owner_ == nullptr) {
        return UrgentRequestResult::Invalid;
    }
    return owner_->request_shutdown();
}

TrustedIngressResult RuntimeOwnerTrustedReceiptPort::submit(
    const TrustedReceipt input) noexcept
{
    if (owner_ == nullptr) {
        return TrustedIngressResult::RejectedNotAllowed;
    }
    return owner_->enqueue_trusted_receipt(input);
}

TrustedIngressResult RuntimeOwnerNormalCompletionPort::submit(
    const NormalCompletion input) noexcept
{
    if (owner_ == nullptr) {
        return TrustedIngressResult::RejectedNotAllowed;
    }
    return owner_->enqueue_normal_completion(input);
}

RuntimeOwnerAdapterView RuntimeOwnerAdapterCore::view() const noexcept
{
    RuntimeOwnerAdapterView result{};
    result.core = core_.view();
    result.current_dispatch = current_dispatch_;
    result.physical_inflight = physical_inflight_;
    result.critical = critical_;
    result.last_normal_enqueue_sequence = last_normal_enqueue_sequence_;
    result.last_trusted_ingress_sequence =
        last_trusted_ingress_sequence_;
    result.last_dispatch_sequence = last_dispatch_sequence_;
    result.last_ack_dispatch_sequence = last_ack_dispatch_sequence_;
    result.last_trusted_diagnostic_ingress_sequence =
        last_trusted_diagnostic_ingress_sequence_;
    result.last_trusted_diagnostic_code = last_trusted_diagnostic_code_;
    result.normal_coalesced_count = normal_coalesced_count_;
    result.normal_rejected_full_count = normal_rejected_full_count_;
    result.normal_cancelled_count = normal_cancelled_count_;
    result.dispatch_rejected_ack_count = dispatch_rejected_ack_count_;
    result.trusted_rejected_full_count = trusted_rejected_full_count_;
    result.trusted_protocol_violation_count =
        trusted_protocol_violation_count_;
    result.trusted_stale_count = trusted_stale_count_;
    result.trusted_duplicate_count = trusted_duplicate_count_;
    result.trusted_cancelled_count = trusted_cancelled_count_;
    result.effect_cancelled_count = effect_cancelled_count_;
    result.normal_completion_stale_count =
        normal_completion_stale_count_;
    result.normal_depth = normal_count_;
    result.normal_high_water = normal_high_water_;
    result.trusted_depth = trusted_count_;
    result.trusted_high_water = trusted_high_water_;
    result.pending_effect_count = pending_effect_count_;
    result.transport_request_pending =
        static_cast<std::uint8_t>(transport_request_pending_);
    result.shutdown_pending = static_cast<std::uint8_t>(shutdown_pending_);
    result.shutdown_terminal_override_latched =
        static_cast<std::uint8_t>(shutdown_terminal_override_latched_);
    result.critical_pending = static_cast<std::uint8_t>(critical_pending_);
    result.boot_end_released = static_cast<std::uint8_t>(boot_end_released_);
    result.core_fail_closed_latched =
        static_cast<std::uint8_t>(core_fail_closed_latched_);
    result.core_adapter_fatal_latched =
        static_cast<std::uint8_t>(core_adapter_fatal_latched_);
    result.sequence_fatal_latched =
        static_cast<std::uint8_t>(sequence_fatal_latched_);
    result.dispatch_fatal_latched =
        static_cast<std::uint8_t>(dispatch_fatal_latched_);
    result.safety_delivery_blocked =
        static_cast<std::uint8_t>(safety_delivery_blocked_);
    result.physical_inflight_cancel_pending = static_cast<std::uint8_t>(
        physical_inflight_cancel_pending_);
    return result;
}

bool RuntimeOwnerAdapterCore::normal_intents_have_same_key(
    const NormalIntent left,
    const NormalIntent right) noexcept
{
    if (left.kind != right.kind) {
        return false;
    }
    return (left.kind != NormalIntentKind::PublishTelemetry &&
            left.kind != NormalIntentKind::PublishAdapterRemoved &&
            left.kind != NormalIntentKind::PublishAdapterRestored) ||
           left.subject_id == right.subject_id;
}

bool RuntimeOwnerAdapterCore::trusted_receipt_is_canonical(
    const TrustedReceipt input) noexcept
{
    if (input.reserved != 0) {
        return false;
    }

    switch (input.kind) {
    case TrustedReceiptKind::TransportEstablished:
        return input.effect_kind ==
                   RuntimeOwnerEffectKind::StartTransportAttempt &&
               input.correlation_id == 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch == 0 &&
               input.diagnostic_code == 0;
    case TrustedReceiptKind::TransportAttemptFailed:
        return input.effect_kind ==
                   RuntimeOwnerEffectKind::StartTransportAttempt &&
               input.correlation_id == 0 && input.mqtt_session_id == 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch == 0;
    case TrustedReceiptKind::ConfigCommitted:
        return input.effect_kind == RuntimeOwnerEffectKind::None &&
               input.correlation_id == 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence != 0 &&
               input.config_apply_epoch == 0 &&
               input.diagnostic_code == 0;
    case TrustedReceiptKind::OperationCompleted:
        return is_liveness_start_effect(input.effect_kind) &&
               input.correlation_id != 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch != 0 &&
               input.diagnostic_code == 0;
    case TrustedReceiptKind::OperationFailed:
    case TrustedReceiptKind::DeadlineExpired:
        return is_liveness_start_effect(input.effect_kind) &&
               input.correlation_id != 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch != 0;
    case TrustedReceiptKind::SnapshotSucceeded:
        return input.effect_kind ==
                   RuntimeOwnerEffectKind::FreezeBootSnapshot &&
               input.correlation_id != 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch != 0 &&
               input.diagnostic_code == 0;
    case TrustedReceiptKind::SnapshotFailed:
        return input.effect_kind ==
                   RuntimeOwnerEffectKind::FreezeBootSnapshot &&
               input.correlation_id != 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch != 0;
    case TrustedReceiptKind::TransportDisconnected:
        return input.effect_kind == RuntimeOwnerEffectKind::None &&
               input.correlation_id == 0 && input.mqtt_session_id != 0 &&
               input.mqtt_generation != 0 &&
               input.config_commit_sequence == 0 &&
               input.config_apply_epoch == 0;
    case TrustedReceiptKind::Invalid:
    default:
        return false;
    }
}

bool RuntimeOwnerAdapterCore::normal_completion_is_canonical(
    const NormalCompletion input) noexcept
{
    if (input.reserved != std::array<std::uint8_t, 3>{} ||
        input.dispatch_sequence == 0 || input.enqueue_sequence == 0) {
        return false;
    }
    switch (input.kind) {
    case NormalCompletionKind::Succeeded:
        return input.diagnostic_code == 0;
    case NormalCompletionKind::Failed:
    case NormalCompletionKind::TimedOut:
    case NormalCompletionKind::Cancelled:
        return true;
    case NormalCompletionKind::Invalid:
    default:
        return false;
    }
}

bool RuntimeOwnerAdapterCore::normal_admission_blocked() const noexcept
{
    const RuntimeOwnerView core = core_.view();
    return core.phase == RuntimeOwnerPhase::ShutdownCommitted ||
           shutdown_pending_ ||
           shutdown_terminal_override_latched_ ||
           core_fail_closed_latched_ ||
           core_adapter_fatal_latched_ ||
           sequence_fatal_latched_ ||
           dispatch_fatal_latched_ ||
           safety_delivery_blocked_;
}

bool RuntimeOwnerAdapterCore::trusted_admission_blocked() const noexcept
{
    const RuntimeOwnerView core = core_.view();
    return shutdown_pending_ ||
           core.phase == RuntimeOwnerPhase::ShutdownCommitted ||
           shutdown_terminal_override_latched_ ||
           core_fail_closed_latched_ ||
           core_adapter_fatal_latched_ ||
           sequence_fatal_latched_ ||
           dispatch_fatal_latched_ ||
           safety_delivery_blocked_;
}

void RuntimeOwnerAdapterCore::record_critical(
    const AdapterCriticalReason reason,
    const std::uint32_t ingress_sequence,
    const std::uint32_t diagnostic_code) noexcept
{
    const std::uint8_t reason_value = static_cast<std::uint8_t>(reason);
    if (reason_value == 0 ||
        reason_value >
            static_cast<std::uint8_t>(AdapterCriticalReason::CoreAdapterInvariant)) {
        return;
    }

    if (critical_.occurrence_count == 0) {
        critical_.first_reason = reason;
        critical_.first_ingress_sequence = ingress_sequence;
        critical_.first_diagnostic_code = diagnostic_code;
    }
    critical_.last_reason = reason;
    critical_.reason_mask |= (1u << (reason_value - 1u));
    critical_.last_ingress_sequence = ingress_sequence;
    critical_.last_diagnostic_code = diagnostic_code;
    increment_saturating(critical_.occurrence_count);
    critical_pending_ = true;
}

TrustedIngressResult RuntimeOwnerAdapterCore::enqueue_trusted_receipt(
    const TrustedReceipt input) noexcept
{
    if (trusted_admission_blocked()) {
        return TrustedIngressResult::RejectedNotAllowed;
    }
    if (!trusted_receipt_is_canonical(input)) {
        increment_saturating(trusted_protocol_violation_count_);
        record_critical(
            AdapterCriticalReason::TrustedProtocolViolation,
            0,
            input.diagnostic_code);
        return TrustedIngressResult::RejectedInvalid;
    }
    if (last_trusted_ingress_sequence_ ==
        std::numeric_limits<std::uint32_t>::max()) {
        sequence_fatal_latched_ = true;
        record_critical(
            AdapterCriticalReason::TrustedSequenceSaturation,
            0,
            input.diagnostic_code);
        return TrustedIngressResult::RejectedSequenceSaturated;
    }
    if (trusted_count_ == kTrustedQueueCapacity) {
        increment_saturating(trusted_rejected_full_count_);
        record_critical(
            AdapterCriticalReason::TrustedQueueOverflow,
            0,
            input.diagnostic_code);
        return TrustedIngressResult::RejectedFull;
    }

    const std::uint32_t next_sequence =
        last_trusted_ingress_sequence_ + 1;
    TrustedIngressEnvelope envelope{};
    envelope.kind = TrustedIngressPayloadKind::CoreReceipt;
    envelope.ingress_sequence = next_sequence;
    envelope.receipt = input;
    trusted_queue_[trusted_tail_] = envelope;
    trusted_tail_ = static_cast<std::uint8_t>(
        (trusted_tail_ + 1) % kTrustedQueueCapacity);
    ++trusted_count_;
    if (trusted_count_ > trusted_high_water_) {
        trusted_high_water_ = trusted_count_;
    }
    last_trusted_ingress_sequence_ = next_sequence;
    if (is_trusted_diagnostic_event(input.kind)) {
        last_trusted_diagnostic_ingress_sequence_ = next_sequence;
        last_trusted_diagnostic_code_ = input.diagnostic_code;
    }
    return TrustedIngressResult::Accepted;
}

TrustedIngressResult RuntimeOwnerAdapterCore::enqueue_normal_completion(
    const NormalCompletion input) noexcept
{
    if (trusted_admission_blocked()) {
        return TrustedIngressResult::RejectedNotAllowed;
    }
    if (!normal_completion_is_canonical(input)) {
        increment_saturating(trusted_protocol_violation_count_);
        record_critical(
            AdapterCriticalReason::TrustedProtocolViolation,
            0,
            input.diagnostic_code);
        return TrustedIngressResult::RejectedInvalid;
    }
    if (last_trusted_ingress_sequence_ ==
        std::numeric_limits<std::uint32_t>::max()) {
        sequence_fatal_latched_ = true;
        record_critical(
            AdapterCriticalReason::TrustedSequenceSaturation,
            0,
            input.diagnostic_code);
        return TrustedIngressResult::RejectedSequenceSaturated;
    }
    if (trusted_count_ == kTrustedQueueCapacity) {
        increment_saturating(trusted_rejected_full_count_);
        record_critical(
            AdapterCriticalReason::TrustedQueueOverflow,
            0,
            input.diagnostic_code);
        return TrustedIngressResult::RejectedFull;
    }

    const std::uint32_t next_sequence =
        last_trusted_ingress_sequence_ + 1;
    TrustedIngressEnvelope envelope{};
    envelope.kind = TrustedIngressPayloadKind::NormalCompletion;
    envelope.ingress_sequence = next_sequence;
    envelope.normal_completion = input;
    trusted_queue_[trusted_tail_] = envelope;
    trusted_tail_ = static_cast<std::uint8_t>(
        (trusted_tail_ + 1) % kTrustedQueueCapacity);
    ++trusted_count_;
    if (trusted_count_ > trusted_high_water_) {
        trusted_high_water_ = trusted_count_;
    }
    last_trusted_ingress_sequence_ = next_sequence;
    return TrustedIngressResult::Accepted;
}

NormalSubmitResult RuntimeOwnerAdapterCore::submit_normal(
    const NormalIntent input) noexcept
{
    if (normal_admission_blocked()) {
        return NormalSubmitResult::RejectedNotReady;
    }

    const RuntimeOwnerView core = core_.view();
    if (core.phase != RuntimeOwnerPhase::RuntimeReady ||
        !core.boot_orchestration_ended || !boot_end_released_) {
        return NormalSubmitResult::RejectedNotReady;
    }
    if (!runtime_owner_normal_intent_is_canonical(input)) {
        return NormalSubmitResult::RejectedInvalid;
    }
    if (last_normal_enqueue_sequence_ ==
        std::numeric_limits<std::uint32_t>::max()) {
        sequence_fatal_latched_ = true;
        record_critical(
            AdapterCriticalReason::NormalSequenceSaturation, 0, 0);
        return NormalSubmitResult::RejectedSequenceSaturated;
    }

    const std::uint32_t next_sequence =
        last_normal_enqueue_sequence_ + 1;
    for (std::uint8_t offset = 0; offset < normal_count_; ++offset) {
        const std::uint8_t index = static_cast<std::uint8_t>(
            (normal_head_ + offset) % kNormalQueueCapacity);
        NormalQueueEntry &queued = normal_queue_[index];
        if (normal_intents_have_same_key(queued.intent, input)) {
            queued.intent = input;
            queued.enqueue_sequence = next_sequence;
            last_normal_enqueue_sequence_ = next_sequence;
            increment_saturating(normal_coalesced_count_);
            return NormalSubmitResult::AcceptedCoalesced;
        }
    }

    if (normal_count_ == kNormalQueueCapacity) {
        increment_saturating(normal_rejected_full_count_);
        return NormalSubmitResult::RejectedFull;
    }

    normal_queue_[normal_tail_] = {input, next_sequence};
    normal_tail_ = static_cast<std::uint8_t>(
        (normal_tail_ + 1) % kNormalQueueCapacity);
    ++normal_count_;
    if (normal_count_ > normal_high_water_) {
        normal_high_water_ = normal_count_;
    }
    last_normal_enqueue_sequence_ = next_sequence;
    return NormalSubmitResult::Accepted;
}

OwnerRequestResult
RuntimeOwnerAdapterCore::request_transport_attempt() noexcept
{
    if (core_fail_closed_latched_ || core_adapter_fatal_latched_ ||
        sequence_fatal_latched_ || dispatch_fatal_latched_ ||
        safety_delivery_blocked_) {
        return OwnerRequestResult::RejectedFatal;
    }
    if (shutdown_pending_) {
        return OwnerRequestResult::RejectedNotAllowed;
    }
    if (transport_request_pending_) {
        return OwnerRequestResult::AcceptedDuplicate;
    }

    const RuntimeOwnerView core = core_.view();
    const bool phase_allows_request =
        core.phase == RuntimeOwnerPhase::ColdStart ||
        core.phase == RuntimeOwnerPhase::RecoveryPending;
    if (!phase_allows_request || core.boot_orchestration_ended ||
        boot_end_released_) {
        return OwnerRequestResult::RejectedNotAllowed;
    }

    transport_request_pending_ = true;
    return OwnerRequestResult::Accepted;
}

RuntimeOwnerTransition RuntimeOwnerAdapterCore::submit_core_input(
    const RuntimeOwnerInput input) noexcept
{
    const RuntimeOwnerTransition transition = core_.submit(input);
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
    increment_saturating(core_submit_count_);
    if (core_transition_override_pending_) {
        core_transition_override_pending_ = false;
        return core_transition_override_;
    }
#endif
    return transition;
}

RuntimeOwnerView RuntimeOwnerAdapterCore::view_after_core_submit() noexcept
{
    RuntimeOwnerView after = core_.view();
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
    if (core_post_submit_view_override_pending_) {
        core_post_submit_view_override_pending_ = false;
        after = core_post_submit_view_override_;
    }
#endif
    return after;
}

void RuntimeOwnerAdapterCore::quarantine_physical_inflight() noexcept
{
    if (physical_inflight_.kind == AdapterDispatchKind::None ||
        physical_inflight_cancel_pending_) {
        return;
    }
    physical_inflight_cancel_pending_ = true;
    if (physical_inflight_.kind == AdapterDispatchKind::NormalIntent) {
        increment_saturating(normal_cancelled_count_);
    } else {
        increment_saturating(effect_cancelled_count_);
    }
}

void RuntimeOwnerAdapterCore::cancel_non_safety_authorization() noexcept
{
    if (current_dispatch_.kind == AdapterDispatchKind::NormalIntent) {
        increment_saturating(normal_cancelled_count_);
        current_dispatch_ = {};
    } else if (current_dispatch_.kind == AdapterDispatchKind::CoreEffect &&
               !is_safety_delivery_effect(
                   current_dispatch_.effect.kind)) {
        increment_saturating(effect_cancelled_count_);
        current_dispatch_ = {};
    }

    add_saturating(normal_cancelled_count_, normal_count_);
    normal_queue_ = {};
    normal_head_ = 0;
    normal_tail_ = 0;
    normal_count_ = 0;

    std::array<PendingEffectSlot, kPendingEffectCapacity> preserved{};
    std::uint8_t preserved_count = 0;
    for (std::uint8_t offset = 0; offset < pending_effect_count_; ++offset) {
        const std::uint8_t index = static_cast<std::uint8_t>(
            (pending_effect_head_ + offset) % kPendingEffectCapacity);
        const PendingEffectSlot slot = pending_effects_[index];
        if (is_safety_delivery_effect(slot.effect.kind)) {
            preserved[preserved_count] = slot;
            ++preserved_count;
        } else {
            increment_saturating(effect_cancelled_count_);
        }
    }
    pending_effects_ = preserved;
    pending_effect_head_ = 0;
    pending_effect_tail_ = static_cast<std::uint8_t>(
        preserved_count % kPendingEffectCapacity);
    pending_effect_count_ = preserved_count;
    transport_request_pending_ = false;
    quarantine_physical_inflight();
}

void RuntimeOwnerAdapterCore::cancel_for_active_critical() noexcept
{
    cancel_non_safety_authorization();
    add_saturating(trusted_cancelled_count_, trusted_count_);
    trusted_queue_ = {};
    trusted_head_ = 0;
    trusted_tail_ = 0;
    trusted_count_ = 0;
    accepted_liveness_mask_ = 0;
}

void RuntimeOwnerAdapterCore::cancel_for_shutdown() noexcept
{
    cancel_non_safety_authorization();
    physical_inflight_ = {};
    physical_inflight_cancel_pending_ = false;

    bool record_fault_preserved =
        current_dispatch_.kind == AdapterDispatchKind::CoreEffect &&
        current_dispatch_.effect.kind ==
            RuntimeOwnerEffectKind::RecordFault;
    if (current_dispatch_.kind == AdapterDispatchKind::CoreEffect &&
        current_dispatch_.effect.kind ==
            RuntimeOwnerEffectKind::EnterRecovery) {
        increment_saturating(effect_cancelled_count_);
        current_dispatch_ = {};
    }

    std::array<PendingEffectSlot, kPendingEffectCapacity> preserved{};
    std::uint8_t preserved_count = 0;
    for (std::uint8_t offset = 0; offset < pending_effect_count_; ++offset) {
        const std::uint8_t index = static_cast<std::uint8_t>(
            (pending_effect_head_ + offset) % kPendingEffectCapacity);
        const PendingEffectSlot slot = pending_effects_[index];
        if (!record_fault_preserved &&
            slot.effect.kind == RuntimeOwnerEffectKind::RecordFault) {
            preserved[0] = slot;
            preserved_count = 1;
            record_fault_preserved = true;
        } else {
            increment_saturating(effect_cancelled_count_);
        }
    }
    pending_effects_ = preserved;
    pending_effect_head_ = 0;
    pending_effect_tail_ = preserved_count;
    pending_effect_count_ = preserved_count;

    add_saturating(trusted_cancelled_count_, trusted_count_);
    trusted_queue_ = {};
    trusted_head_ = 0;
    trusted_tail_ = 0;
    trusted_count_ = 0;
    accepted_liveness_mask_ = 0;
    critical_pending_ = false;
}

AdapterStepResult RuntimeOwnerAdapterCore::shutdown_terminal_step(
    const RuntimeOwnerPhase phase) noexcept
{
    if (current_dispatch_.kind == AdapterDispatchKind::None &&
        pending_effect_count_ != 0 &&
        pending_effects_[pending_effect_head_].effect.kind ==
            RuntimeOwnerEffectKind::RecordFault) {
        PendingEffectSlot &pending =
            pending_effects_[pending_effect_head_];
        current_dispatch_ = {
            AdapterDispatchKind::CoreEffect,
            {},
            pending.preassigned_dispatch_sequence,
            0,
            pending.effect,
            {},
        };
        const std::uint32_t dispatch_sequence =
            current_dispatch_.dispatch_sequence;
        pending = {};
        pending_effect_head_ = static_cast<std::uint8_t>(
            (pending_effect_head_ + 1) % kPendingEffectCapacity);
        --pending_effect_count_;
        return {
            AdapterStepAction::DispatchPrepared,
            RuntimeOwnerDisposition::Rejected,
            phase,
            phase,
            0,
            0,
            dispatch_sequence,
        };
    }

    return {
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        phase,
        phase,
        0,
        0,
        0,
    };
}

AdapterStepResult RuntimeOwnerAdapterCore::handle_malformed_core_transition(
    const MalformedTransitionOrigin origin,
    const RuntimeOwnerPhase phase_before,
    const RuntimeOwnerPhase observed_phase_after,
    const std::uint32_t trusted_ingress_sequence) noexcept
{
    const RuntimeOwnerPhase fallback_phase_after =
        known_phase_or_before(observed_phase_after, phase_before);
    const SafetyPairDispatchPlan safety_dispatch_plan =
        plan_safety_pair_dispatch(last_dispatch_sequence_);
    const std::uint32_t consumed_ingress_sequence =
        origin == MalformedTransitionOrigin::TrustedHead
            ? trusted_ingress_sequence
            : 0;

    if (origin == MalformedTransitionOrigin::Shutdown) {
        cancel_for_shutdown();
        record_critical(
            AdapterCriticalReason::CoreAdapterInvariant,
            0,
            0);
        core_adapter_fatal_latched_ = true;

        if (fallback_phase_after != RuntimeOwnerPhase::ShutdownCommitted) {
            if (safety_dispatch_plan.available) {
                const std::array<PendingEffectSlot, 2> synthetic{{
                    {
                        safety_dispatch_plan.record_fault_sequence,
                        {
                            RuntimeOwnerEffectKind::RecordFault,
                            0,
                            {},
                            RuntimeOwnerFaultCode::InternalInvariant,
                        },
                    },
                    {
                        safety_dispatch_plan.enter_recovery_sequence,
                        {
                            RuntimeOwnerEffectKind::EnterRecovery,
                            0,
                            {},
                            RuntimeOwnerFaultCode::InternalInvariant,
                        },
                    },
                }};
                for (const PendingEffectSlot slot : synthetic) {
                    pending_effects_[pending_effect_tail_] = slot;
                    pending_effect_tail_ = static_cast<std::uint8_t>(
                        (pending_effect_tail_ + 1) %
                        kPendingEffectCapacity);
                    ++pending_effect_count_;
                }
                last_dispatch_sequence_ =
                    safety_dispatch_plan.enter_recovery_sequence;
                if (safety_dispatch_plan.uses_terminal_reserve) {
                    dispatch_fatal_latched_ = true;
                }
                cancel_for_shutdown();
            } else {
                safety_delivery_blocked_ = true;
            }
        }

        shutdown_pending_ = false;
        shutdown_terminal_override_latched_ = true;
        critical_pending_ = false;
        return {
            AdapterStepAction::CoreAdapterFatalHandled,
            RuntimeOwnerDisposition::FailClosed,
            phase_before,
            fallback_phase_after,
            0,
            0,
            0,
        };
    }

    const std::uint8_t trusted_cancelled = static_cast<std::uint8_t>(
        trusted_count_ -
        ((origin == MalformedTransitionOrigin::TrustedHead &&
          trusted_count_ != 0)
             ? 1
             : 0));
    cancel_non_safety_authorization();
    add_saturating(trusted_cancelled_count_, trusted_cancelled);

    trusted_queue_ = {};
    trusted_head_ = 0;
    trusted_tail_ = 0;
    trusted_count_ = 0;
    pending_effects_ = {};
    pending_effect_head_ = 0;
    pending_effect_tail_ = 0;
    pending_effect_count_ = 0;
    accepted_liveness_mask_ = 0;
    transport_request_pending_ = false;

    record_critical(
        AdapterCriticalReason::CoreAdapterInvariant,
        consumed_ingress_sequence,
        0);
    core_adapter_fatal_latched_ = true;

    if (fallback_phase_after != RuntimeOwnerPhase::ShutdownCommitted) {
        if (safety_dispatch_plan.available) {
            const std::array<PendingEffectSlot, 2> pending_shadow{{
                {
                    safety_dispatch_plan.record_fault_sequence,
                    {
                        RuntimeOwnerEffectKind::RecordFault,
                        0,
                        {},
                        RuntimeOwnerFaultCode::InternalInvariant,
                    },
                },
                {
                    safety_dispatch_plan.enter_recovery_sequence,
                    {
                        RuntimeOwnerEffectKind::EnterRecovery,
                        0,
                        {},
                        RuntimeOwnerFaultCode::InternalInvariant,
                    },
                },
            }};
            for (std::size_t index = 0;
                 index < pending_shadow.size(); ++index) {
                pending_effects_[index] = pending_shadow[index];
            }
            pending_effect_tail_ = static_cast<std::uint8_t>(
                pending_shadow.size());
            pending_effect_count_ = static_cast<std::uint8_t>(
                pending_shadow.size());
            last_dispatch_sequence_ =
                safety_dispatch_plan.enter_recovery_sequence;
            if (safety_dispatch_plan.uses_terminal_reserve) {
                dispatch_fatal_latched_ = true;
            }
        } else {
            safety_delivery_blocked_ = true;
        }
    }

    critical_pending_ = false;

    return {
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        phase_before,
        fallback_phase_after,
        consumed_ingress_sequence,
        0,
        0,
    };
}

AdapterStepResult RuntimeOwnerAdapterCore::step() noexcept
{
    const RuntimeOwnerView before = core_.view();
    const bool fatal_before_shutdown =
        core_fail_closed_latched_ || core_adapter_fatal_latched_ ||
        sequence_fatal_latched_ || dispatch_fatal_latched_ ||
        safety_delivery_blocked_;
    if (shutdown_pending_ && fatal_before_shutdown) {
        cancel_for_shutdown();
        shutdown_pending_ = false;
        shutdown_terminal_override_latched_ = true;
        return shutdown_terminal_step(before.phase);
    }

    if (shutdown_pending_ && !core_fail_closed_latched_ &&
        !core_adapter_fatal_latched_ && !sequence_fatal_latched_ &&
        !dispatch_fatal_latched_ && !safety_delivery_blocked_) {
        RuntimeOwnerInput input{};
        input.kind = RuntimeOwnerInputKind::ShutdownCommitted;
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
        if (!canonical_shutdown_transition(before, transition, after)) {
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::Shutdown,
                before.phase,
                after.phase,
                0);
        }

        cancel_for_shutdown();
        shutdown_pending_ = false;
        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            0,
            0,
            0,
        };
    }

    if (shutdown_terminal_override_latched_ ||
        before.phase == RuntimeOwnerPhase::ShutdownCommitted) {
        return shutdown_terminal_step(before.phase);
    }

    if (critical_pending_ &&
        before.phase <= RuntimeOwnerPhase::RuntimeReady &&
        !core_fail_closed_latched_ && !core_adapter_fatal_latched_ &&
        !dispatch_fatal_latched_ && !safety_delivery_blocked_) {
        const SafetyPairDispatchPlan safety_dispatch_plan =
            plan_safety_pair_dispatch(last_dispatch_sequence_);
        if (!safety_dispatch_plan.available) {
            cancel_for_active_critical();
            critical_pending_ = false;
            safety_delivery_blocked_ = true;
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }

        RuntimeOwnerInput input{};
        input.kind = RuntimeOwnerInputKind::CriticalIngressFault;
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
        if (!canonical_critical_transition(before, transition, after)) {
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::Critical,
                before.phase,
                after.phase,
                0);
        }

        cancel_for_active_critical();
        const std::array<PendingEffectSlot, 2> pending_shadow{{
            {
                safety_dispatch_plan.record_fault_sequence,
                transition.effects[0],
            },
            {
                safety_dispatch_plan.enter_recovery_sequence,
                transition.effects[1],
            },
        }};
        for (std::size_t index = 0;
             index < pending_shadow.size(); ++index) {
            const std::uint8_t slot = static_cast<std::uint8_t>(
                (pending_effect_tail_ + index) % kPendingEffectCapacity);
            pending_effects_[slot] = pending_shadow[index];
        }
        pending_effect_tail_ = static_cast<std::uint8_t>(
            (pending_effect_tail_ + pending_shadow.size()) %
            kPendingEffectCapacity);
        pending_effect_count_ = static_cast<std::uint8_t>(
            pending_effect_count_ + pending_shadow.size());
        last_dispatch_sequence_ =
            safety_dispatch_plan.enter_recovery_sequence;
        critical_pending_ = false;
        if (safety_dispatch_plan.uses_terminal_reserve) {
            dispatch_fatal_latched_ = true;
        }
        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            0,
            0,
            0,
        };
    }

    if (current_dispatch_.kind == AdapterDispatchKind::CoreEffect &&
        is_safety_delivery_effect(current_dispatch_.effect.kind)) {
        return {
            AdapterStepAction::AwaitingDispatchAck,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            0,
        };
    }
    if (pending_effect_count_ != 0 &&
        is_safety_delivery_effect(
            pending_effects_[pending_effect_head_].effect.kind)) {
        PendingEffectSlot &pending =
            pending_effects_[pending_effect_head_];
        current_dispatch_ = {
            AdapterDispatchKind::CoreEffect,
            {},
            pending.preassigned_dispatch_sequence,
            0,
            pending.effect,
            {},
        };
        const std::uint32_t dispatch_sequence =
            current_dispatch_.dispatch_sequence;
        pending = {};
        pending_effect_head_ = static_cast<std::uint8_t>(
            (pending_effect_head_ + 1) % kPendingEffectCapacity);
        --pending_effect_count_;
        return {
            AdapterStepAction::DispatchPrepared,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            dispatch_sequence,
        };
    }

    const bool terminal_fatal =
        core_fail_closed_latched_ || core_adapter_fatal_latched_ ||
        sequence_fatal_latched_ || dispatch_fatal_latched_ ||
        safety_delivery_blocked_;
    const bool fatal_terminal_ready =
        terminal_fatal && !shutdown_pending_ &&
        (!critical_pending_ ||
         before.phase == RuntimeOwnerPhase::RecoveryPending);
    if (fatal_terminal_ready) {
        return {
            AdapterStepAction::Terminal,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            0,
        };
    }

    if (critical_pending_ &&
        before.phase == RuntimeOwnerPhase::RecoveryPending &&
        !terminal_fatal) {
        critical_pending_ = false;
        return {
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            0,
        };
    }

    if (shutdown_pending_ || critical_pending_ || terminal_fatal) {
        return canonical_idle_step(before.phase);
    }

    const bool config_head_candidate =
        trusted_count_ != 0 &&
        trusted_queue_[trusted_head_].kind ==
            TrustedIngressPayloadKind::CoreReceipt &&
        trusted_queue_[trusted_head_].receipt.kind ==
            TrustedReceiptKind::ConfigCommitted;
    if (config_head_candidate) {
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        const TrustedReceipt &config_candidate =
            trusted_queue_[trusted_head_].receipt;
        const bool config_override_authorized =
            core_transition_override_pending_ &&
            before.phase == RuntimeOwnerPhase::AwaitingConfigCommit &&
            !before.boot_orchestration_ended &&
            config_candidate.mqtt_session_id == before.mqtt_session_id &&
            config_candidate.mqtt_generation == before.mqtt_generation &&
                config_candidate.config_commit_sequence >
                before.last_config_commit_sequence;
        bool bypass_used = false;
#endif

        TrustedIngressEnvelope &envelope = trusted_queue_[trusted_head_];
        const std::uint32_t ingress_sequence = envelope.ingress_sequence;
        const TrustedReceipt receipt = envelope.receipt;
        if (last_trusted_receipt_signature_.ingress_sequence != 0 &&
            trusted_receipts_equal(
                receipt, last_trusted_receipt_signature_.receipt)) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }
        if (before.phase != RuntimeOwnerPhase::AwaitingConfigCommit ||
            before.boot_orchestration_ended ||
            receipt.mqtt_session_id != before.mqtt_session_id ||
            receipt.mqtt_generation != before.mqtt_generation ||
            receipt.config_commit_sequence <=
                before.last_config_commit_sequence) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        const bool expected_counter_fail_closed =
            config_counter_saturation_expected(before);
        SafetyPairDispatchPlan safety_dispatch_plan{};
        if (expected_counter_fail_closed) {
            safety_dispatch_plan =
                plan_safety_pair_dispatch(last_dispatch_sequence_);
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
            if (!safety_dispatch_plan.available &&
                !config_override_authorized) {
#else
            if (!safety_dispatch_plan.available) {
#endif
                record_critical(
                    AdapterCriticalReason::DispatchSequenceSaturation,
                    ingress_sequence,
                    0);
                return {
                    AdapterStepAction::CriticalLedgerHandled,
                    RuntimeOwnerDisposition::Rejected,
                    before.phase,
                    before.phase,
                    0,
                    0,
                    0,
                };
            }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
            if (!safety_dispatch_plan.available) {
                bypass_used = true;
            }
#endif
        }

        constexpr std::uint32_t kLastConfigBundleStart =
            std::numeric_limits<std::uint32_t>::max() - 6;
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (!expected_counter_fail_closed &&
            !config_override_authorized &&
            last_dispatch_sequence_ > kLastConfigBundleStart) {
#else
        if (!expected_counter_fail_closed &&
            last_dispatch_sequence_ > kLastConfigBundleStart) {
#endif
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation,
                ingress_sequence,
                0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (!expected_counter_fail_closed &&
            last_dispatch_sequence_ > kLastConfigBundleStart) {
            bypass_used = true;
        }
#endif

        const RuntimeOwnerInput input{
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            receipt.mqtt_session_id,
            receipt.mqtt_generation,
            receipt.config_commit_sequence,
            0,
        };
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        last_config_validation_bypass_used_ = bypass_used;
#endif
        if (expected_counter_fail_closed) {
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
            if (bypass_used ||
                !canonical_config_counter_saturation_transition(
                    before, receipt, transition, after)) {
#else
            if (!canonical_config_counter_saturation_transition(
                    before, receipt, transition, after)) {
#endif
                return handle_malformed_core_transition(
                    MalformedTransitionOrigin::TrustedHead,
                    before.phase,
                    after.phase,
                    ingress_sequence);
            }

            cancel_non_safety_authorization();
            const std::array<PendingEffectSlot, 2> pending_shadow{{
                {
                    safety_dispatch_plan.record_fault_sequence,
                    transition.effects[0],
                },
                {
                    safety_dispatch_plan.enter_recovery_sequence,
                    transition.effects[1],
                },
            }};
            for (std::size_t index = 0;
                 index < pending_shadow.size(); ++index) {
                const std::uint8_t slot = static_cast<std::uint8_t>(
                    (pending_effect_tail_ + index) %
                    kPendingEffectCapacity);
                pending_effects_[slot] = pending_shadow[index];
            }
            pending_effect_tail_ = static_cast<std::uint8_t>(
                (pending_effect_tail_ + pending_shadow.size()) %
                kPendingEffectCapacity);
            pending_effect_count_ = static_cast<std::uint8_t>(
                pending_effect_count_ + pending_shadow.size());
            last_dispatch_sequence_ =
                safety_dispatch_plan.enter_recovery_sequence;
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            last_trusted_receipt_signature_ =
                {ingress_sequence, receipt};
            accepted_liveness_mask_ = 0;
            core_fail_closed_latched_ = true;
            if (safety_dispatch_plan.uses_terminal_reserve) {
                dispatch_fatal_latched_ = true;
            }

            return {
                AdapterStepAction::CoreTransitionApplied,
                transition.disposition,
                transition.phase_before,
                transition.phase_after,
                ingress_sequence,
                0,
                0,
            };
        }

#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (bypass_used ||
            !canonical_config_committed_transition(
                before, receipt, transition, after)) {
#else
        if (!canonical_config_committed_transition(
                before, receipt, transition, after)) {
#endif
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::TrustedHead,
                before.phase,
                after.phase,
                ingress_sequence);
        }

        cancel_non_safety_authorization();
        std::array<PendingEffectSlot, 4> pending_shadow{};
        const std::size_t queued_effect_count = transition.effect_count;
        for (std::size_t index = 0; index < queued_effect_count; ++index) {
            pending_shadow[index] = {
                last_dispatch_sequence_ + 1 +
                    static_cast<std::uint32_t>(index),
                transition.effects[index],
            };
        }
        for (std::size_t index = 0; index < queued_effect_count; ++index) {
            const std::uint8_t slot = static_cast<std::uint8_t>(
                (pending_effect_tail_ + index) % kPendingEffectCapacity);
            pending_effects_[slot] = pending_shadow[index];
        }
        pending_effect_tail_ = static_cast<std::uint8_t>(
            (pending_effect_tail_ + queued_effect_count) %
            kPendingEffectCapacity);
        pending_effect_count_ = static_cast<std::uint8_t>(
            pending_effect_count_ + queued_effect_count);
        last_dispatch_sequence_ +=
            static_cast<std::uint32_t>(queued_effect_count);
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        last_trusted_receipt_signature_ = {ingress_sequence, receipt};
        accepted_liveness_mask_ = 0;

        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            ingress_sequence,
            0,
            0,
        };
    }

    const bool disconnect_head_candidate =
        trusted_count_ != 0 &&
        trusted_queue_[trusted_head_].kind ==
            TrustedIngressPayloadKind::CoreReceipt &&
        trusted_queue_[trusted_head_].receipt.kind ==
            TrustedReceiptKind::TransportDisconnected;
    if (current_dispatch_.kind != AdapterDispatchKind::None &&
        (!disconnect_head_candidate ||
         (current_dispatch_.kind == AdapterDispatchKind::CoreEffect &&
          is_safety_delivery_effect(
              current_dispatch_.effect.kind)))) {
        return {
            AdapterStepAction::AwaitingDispatchAck,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            0,
        };
    }

    if (pending_effect_count_ != 0 &&
        is_safety_delivery_effect(
            pending_effects_[pending_effect_head_].effect.kind)) {
        PendingEffectSlot &pending =
            pending_effects_[pending_effect_head_];
        current_dispatch_ = {
            AdapterDispatchKind::CoreEffect,
            {},
            pending.preassigned_dispatch_sequence,
            0,
            pending.effect,
            {},
        };
        const std::uint32_t dispatch_sequence =
            current_dispatch_.dispatch_sequence;
        pending = {};
        pending_effect_head_ = static_cast<std::uint8_t>(
            (pending_effect_head_ + 1) % kPendingEffectCapacity);
        --pending_effect_count_;
        return {
            AdapterStepAction::DispatchPrepared,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            dispatch_sequence,
        };
    }

    if (physical_inflight_.kind != AdapterDispatchKind::None &&
        trusted_count_ == 0) {
        return {
            AdapterStepAction::AwaitingTrustedReceipt,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            0,
        };
    }

    if (pending_effect_count_ != 0 &&
        physical_inflight_.kind == AdapterDispatchKind::None
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        && !core_transition_override_pending_
#endif
        && !disconnect_head_candidate
    ) {
        PendingEffectSlot &pending = pending_effects_[pending_effect_head_];
        current_dispatch_ = {
            AdapterDispatchKind::CoreEffect,
            {},
            pending.preassigned_dispatch_sequence,
            0,
            pending.effect,
            {},
        };
        const std::uint32_t dispatch_sequence =
            current_dispatch_.dispatch_sequence;
        pending = {};
        pending_effect_head_ = static_cast<std::uint8_t>(
            (pending_effect_head_ + 1) % kPendingEffectCapacity);
        --pending_effect_count_;
        return {
            AdapterStepAction::DispatchPrepared,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            0,
            dispatch_sequence,
        };
    }

    if (transport_request_pending_) {
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        bool bypass_used = false;
        if (pending_effect_count_ != 0 &&
            !core_transition_override_pending_) {
#else
        if (pending_effect_count_ != 0) {
#endif
            return canonical_idle_step(before.phase);
        }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (pending_effect_count_ != 0) {
            bypass_used = true;
        }
#endif

        constexpr std::uint32_t kLastNonSafetySingleEffectStart =
            std::numeric_limits<std::uint32_t>::max() - 3;
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (!core_transition_override_pending_ &&
            last_dispatch_sequence_ > kLastNonSafetySingleEffectStart) {
#else
        if (last_dispatch_sequence_ > kLastNonSafetySingleEffectStart) {
#endif
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation, 0, 0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (last_dispatch_sequence_ >
            kLastNonSafetySingleEffectStart) {
            bypass_used = true;
        }
#endif

        RuntimeOwnerInput input{};
        input.kind = RuntimeOwnerInputKind::BeginTransportAttempt;
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (bypass_used ||
            !canonical_begin_transition(before, transition, after)) {
#else
        if (!canonical_begin_transition(before, transition, after)) {
#endif
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::TransportRequest,
                before.phase,
                after.phase,
                0);
        }

        const std::uint32_t dispatch_sequence =
            last_dispatch_sequence_ + 1;
        pending_effects_[pending_effect_tail_] = {
            dispatch_sequence,
            transition.effects[0],
        };
        pending_effect_tail_ = static_cast<std::uint8_t>(
            (pending_effect_tail_ + 1) % kPendingEffectCapacity);
        ++pending_effect_count_;
        last_dispatch_sequence_ = dispatch_sequence;
        transport_request_pending_ = false;

        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            0,
            0,
            0,
        };
    }

    if (trusted_count_ == 0 &&
        before.phase == RuntimeOwnerPhase::RuntimeReady &&
        before.boot_orchestration_ended && boot_end_released_ &&
        normal_count_ != 0) {
        constexpr std::uint32_t kLastNonSafetySingleEffectStart =
            std::numeric_limits<std::uint32_t>::max() - 3;
        if (last_dispatch_sequence_ >
            kLastNonSafetySingleEffectStart) {
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation, 0, 0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }

        NormalQueueEntry &entry = normal_queue_[normal_head_];
        const std::uint32_t dispatch_sequence =
            last_dispatch_sequence_ + 1;
        const std::uint32_t enqueue_sequence = entry.enqueue_sequence;
        current_dispatch_ = {
            AdapterDispatchKind::NormalIntent,
            {},
            dispatch_sequence,
            enqueue_sequence,
            {},
            entry.intent,
        };
        entry = {};
        normal_head_ = static_cast<std::uint8_t>(
            (normal_head_ + 1) % kNormalQueueCapacity);
        --normal_count_;
        last_dispatch_sequence_ = dispatch_sequence;
        return {
            AdapterStepAction::DispatchPrepared,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            0,
            enqueue_sequence,
            dispatch_sequence,
        };
    }

#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
    bool bypass_used = false;
    const bool transport_established_override_armed =
        core_transition_override_pending_ &&
        trusted_count_ != 0 &&
        trusted_queue_[trusted_head_].kind ==
            TrustedIngressPayloadKind::CoreReceipt &&
        trusted_queue_[trusted_head_].receipt.kind ==
            TrustedReceiptKind::TransportEstablished &&
        before.phase == RuntimeOwnerPhase::TransportConnecting &&
        !before.boot_orchestration_ended &&
        trusted_queue_[trusted_head_].receipt.mqtt_generation ==
            before.mqtt_generation_counter;
    std::uint8_t operation_completed_override_ticket_index = 0xff;
    const bool operation_completed_override_head =
        trusted_count_ != 0 &&
        trusted_queue_[trusted_head_].kind ==
            TrustedIngressPayloadKind::CoreReceipt &&
        trusted_queue_[trusted_head_].receipt.kind ==
            TrustedReceiptKind::OperationCompleted;
    const bool operation_completed_override_exact_ticket =
        operation_completed_override_head &&
        exact_config_issued_liveness_ticket(
            before,
            trusted_queue_[trusted_head_].receipt,
            operation_completed_override_ticket_index);
    const std::uint8_t operation_completed_override_ticket_mask =
        operation_completed_override_exact_ticket
            ? static_cast<std::uint8_t>(
                  1u << operation_completed_override_ticket_index)
            : 0;
    const bool operation_completed_override_exact_signature =
        operation_completed_override_head &&
        last_trusted_receipt_signature_.ingress_sequence != 0 &&
        trusted_receipts_equal(
            trusted_queue_[trusted_head_].receipt,
            last_trusted_receipt_signature_.receipt);
    const bool operation_completed_override_authorized =
        core_transition_override_pending_ &&
        operation_completed_override_head &&
        before.phase == RuntimeOwnerPhase::LivenessWaiting &&
        !before.boot_orchestration_ended &&
        operation_completed_override_exact_ticket &&
        !operation_completed_override_exact_signature &&
        (accepted_liveness_mask_ &
         operation_completed_override_ticket_mask) == 0;
    std::uint8_t liveness_failure_override_ticket_index = 0xff;
    const bool liveness_failure_override_head =
        trusted_count_ != 0 &&
        trusted_queue_[trusted_head_].kind ==
            TrustedIngressPayloadKind::CoreReceipt &&
        (trusted_queue_[trusted_head_].receipt.kind ==
             TrustedReceiptKind::OperationFailed ||
         trusted_queue_[trusted_head_].receipt.kind ==
             TrustedReceiptKind::DeadlineExpired);
    const bool liveness_failure_override_exact_ticket =
        liveness_failure_override_head &&
        exact_config_issued_liveness_ticket(
            before,
            trusted_queue_[trusted_head_].receipt,
            liveness_failure_override_ticket_index);
    const std::uint8_t liveness_failure_override_ticket_mask =
        liveness_failure_override_exact_ticket
            ? static_cast<std::uint8_t>(
                  1u << liveness_failure_override_ticket_index)
            : 0;
    const bool liveness_failure_override_exact_signature =
        liveness_failure_override_head &&
        last_trusted_receipt_signature_.ingress_sequence != 0 &&
        trusted_receipts_equal(
            trusted_queue_[trusted_head_].receipt,
            last_trusted_receipt_signature_.receipt);
    const bool liveness_failure_override_authorized =
        core_transition_override_pending_ &&
        liveness_failure_override_head &&
        before.phase == RuntimeOwnerPhase::LivenessWaiting &&
        !before.boot_orchestration_ended &&
        liveness_failure_override_exact_ticket &&
        !liveness_failure_override_exact_signature &&
        (accepted_liveness_mask_ &
         liveness_failure_override_ticket_mask) == 0;
    const bool snapshot_override_head =
        trusted_count_ != 0 &&
        trusted_queue_[trusted_head_].kind ==
            TrustedIngressPayloadKind::CoreReceipt &&
        (trusted_queue_[trusted_head_].receipt.kind ==
             TrustedReceiptKind::SnapshotSucceeded ||
         trusted_queue_[trusted_head_].receipt.kind ==
             TrustedReceiptKind::SnapshotFailed);
    const bool snapshot_override_exact_token =
        snapshot_override_head &&
        exact_config_issued_snapshot_token(
            before, trusted_queue_[trusted_head_].receipt);
    const bool snapshot_override_exact_signature =
        snapshot_override_head &&
        last_trusted_receipt_signature_.ingress_sequence != 0 &&
        trusted_receipts_equal(
            trusted_queue_[trusted_head_].receipt,
            last_trusted_receipt_signature_.receipt);
    constexpr std::uint8_t kAllSnapshotLivenessTickets = 0x0f;
    const bool snapshot_override_authorized =
        core_transition_override_pending_ &&
        snapshot_override_head &&
        before.phase == RuntimeOwnerPhase::SnapshotFreezePending &&
        !before.boot_orchestration_ended &&
        accepted_liveness_mask_ == kAllSnapshotLivenessTickets &&
        snapshot_override_exact_token &&
        !snapshot_override_exact_signature;
    if (trusted_count_ == 0 ||
        (pending_effect_count_ != 0 &&
         physical_inflight_.kind == AdapterDispatchKind::None &&
         !transport_established_override_armed &&
         !operation_completed_override_authorized &&
         !liveness_failure_override_authorized &&
         !snapshot_override_authorized &&
         !disconnect_head_candidate)) {
#else
    if (trusted_count_ == 0 ||
        (pending_effect_count_ != 0 &&
         physical_inflight_.kind == AdapterDispatchKind::None &&
         !disconnect_head_candidate)) {
#endif
        return canonical_idle_step(before.phase);
    }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
    if (pending_effect_count_ != 0 &&
        physical_inflight_.kind == AdapterDispatchKind::None) {
        bypass_used = true;
    }
#endif
    TrustedIngressEnvelope &envelope = trusted_queue_[trusted_head_];
    const std::uint32_t ingress_sequence = envelope.ingress_sequence;
    if (envelope.kind == TrustedIngressPayloadKind::NormalCompletion) {
        const NormalCompletion completion = envelope.normal_completion;
        const bool exact_signature =
            last_normal_completion_signature_.ingress_sequence != 0 &&
            normal_completions_equal(
                completion,
                last_normal_completion_signature_.completion);
        if (exact_signature) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        const bool exact_inflight =
            physical_inflight_.kind == AdapterDispatchKind::NormalIntent &&
            completion.dispatch_sequence ==
                physical_inflight_.dispatch_sequence &&
            completion.enqueue_sequence ==
                physical_inflight_.enqueue_sequence;
        if (!exact_inflight) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(normal_completion_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        physical_inflight_ = {};
        if (physical_inflight_cancel_pending_) {
            physical_inflight_cancel_pending_ = false;
            increment_saturating(normal_completion_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        last_normal_completion_signature_ = {
            ingress_sequence,
            completion,
        };
        return {
            AdapterStepAction::TrustedReceiptDiscarded,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            ingress_sequence,
            0,
            0,
        };
    }
    if (envelope.kind != TrustedIngressPayloadKind::CoreReceipt) {
        return canonical_idle_step(before.phase);
    }

    const TrustedReceipt receipt = envelope.receipt;
    const bool exact_signature =
        last_trusted_receipt_signature_.ingress_sequence != 0 &&
        trusted_receipts_equal(
            receipt, last_trusted_receipt_signature_.receipt);
    if (exact_signature) {
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        increment_saturating(trusted_duplicate_count_);
        return {
            AdapterStepAction::TrustedReceiptDiscarded,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            ingress_sequence,
            0,
            0,
        };
    }
    if (receipt.kind == TrustedReceiptKind::OperationCompleted) {
        std::uint8_t completed_ticket_index = 0xff;
        const bool exact_completed_ticket =
            exact_config_issued_liveness_ticket(
                before, receipt, completed_ticket_index);
        const std::uint8_t completed_ticket_mask =
            exact_completed_ticket
                ? static_cast<std::uint8_t>(
                      1u << completed_ticket_index)
                : 0;
        if (exact_completed_ticket &&
            (accepted_liveness_mask_ & completed_ticket_mask) != 0) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }
    }
    if (physical_inflight_cancel_pending_ &&
        receipt_requires_physical_inflight(receipt.kind)) {
        const bool exact_cancelled_terminal =
            receipt_requires_physical_inflight(receipt.kind) &&
            physical_inflight_matches_receipt(
                physical_inflight_, receipt);
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        increment_saturating(trusted_stale_count_);
        if (exact_cancelled_terminal) {
            physical_inflight_ = {};
            physical_inflight_cancel_pending_ = false;
        }
        return {
            AdapterStepAction::TrustedReceiptDiscarded,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            ingress_sequence,
            0,
            0,
        };
    }
    if (receipt_requires_physical_inflight(receipt.kind) &&
        !physical_inflight_matches_receipt(
            physical_inflight_, receipt)) {
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        increment_saturating(trusted_stale_count_);
        return {
            AdapterStepAction::TrustedReceiptDiscarded,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            ingress_sequence,
            0,
            0,
        };
    }
    if (receipt.kind == TrustedReceiptKind::OperationCompleted) {
        std::uint8_t ticket_index = 0xff;
        const bool exact_ticket = exact_config_issued_liveness_ticket(
            before, receipt, ticket_index);
        const std::uint8_t ticket_mask =
            exact_ticket
                ? static_cast<std::uint8_t>(1u << ticket_index)
                : 0;
        const bool exact_signature =
            last_trusted_receipt_signature_.ingress_sequence != 0 &&
            trusted_receipts_equal(
                receipt, last_trusted_receipt_signature_.receipt);
        if (exact_signature ||
            (exact_ticket &&
             (accepted_liveness_mask_ & ticket_mask) != 0)) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        if (before.phase != RuntimeOwnerPhase::LivenessWaiting ||
            !exact_ticket) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        constexpr std::uint8_t kAllLivenessTickets = 0x0f;
        const std::uint8_t next_liveness_mask =
            static_cast<std::uint8_t>(
                accepted_liveness_mask_ | ticket_mask);
        const bool final_ticket =
            next_liveness_mask == kAllLivenessTickets;
        constexpr std::uint32_t kLastNonSafetySingleEffectStart =
            std::numeric_limits<std::uint32_t>::max() - 3;
        if (final_ticket &&
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
            !operation_completed_override_authorized &&
#endif
            last_dispatch_sequence_ >
                kLastNonSafetySingleEffectStart) {
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation,
                ingress_sequence,
                0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (final_ticket &&
            last_dispatch_sequence_ >
                kLastNonSafetySingleEffectStart) {
            bypass_used = true;
        }
#endif

        const RuntimeOwnerInput input{
            RuntimeOwnerInputKind::LivenessOperationCompleted,
            receipt.effect_kind,
            receipt.correlation_id,
            receipt.mqtt_session_id,
            receipt.mqtt_generation,
            0,
            receipt.config_apply_epoch,
        };
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        last_operation_completed_validation_bypass_used_ = bypass_used;
        if (bypass_used ||
            !canonical_operation_completed_transition(
                before, final_ticket, transition, after)) {
#else
        if (!canonical_operation_completed_transition(
                before, final_ticket, transition, after)) {
#endif
                physical_inflight_ = {};
                return handle_malformed_core_transition(
                MalformedTransitionOrigin::TrustedHead,
                before.phase,
                after.phase,
                ingress_sequence);
        }

        PendingEffectSlot freeze_shadow{};
        if (final_ticket) {
            freeze_shadow = {
                last_dispatch_sequence_ + 1,
                transition.effects[0],
            };
            pending_effects_[pending_effect_tail_] = freeze_shadow;
            pending_effect_tail_ = static_cast<std::uint8_t>(
                (pending_effect_tail_ + 1) % kPendingEffectCapacity);
            ++pending_effect_count_;
            last_dispatch_sequence_ =
                freeze_shadow.preassigned_dispatch_sequence;
        }
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        last_trusted_receipt_signature_ = {ingress_sequence, receipt};
        physical_inflight_ = {};
        accepted_liveness_mask_ = next_liveness_mask;

        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            ingress_sequence,
            0,
            0,
        };
    }

    if (receipt.kind == TrustedReceiptKind::OperationFailed ||
        receipt.kind == TrustedReceiptKind::DeadlineExpired) {
        if (last_trusted_receipt_signature_.ingress_sequence != 0 &&
            trusted_receipts_equal(
                receipt, last_trusted_receipt_signature_.receipt)) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        std::uint8_t ticket_index = 0xff;
        const bool exact_ticket = exact_config_issued_liveness_ticket(
            before, receipt, ticket_index);
        const std::uint8_t ticket_mask =
            exact_ticket
                ? static_cast<std::uint8_t>(1u << ticket_index)
                : 0;
        if (before.phase != RuntimeOwnerPhase::LivenessWaiting ||
            !exact_ticket ||
            (accepted_liveness_mask_ & ticket_mask) != 0) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        const SafetyPairDispatchPlan safety_dispatch_plan =
            plan_safety_pair_dispatch(last_dispatch_sequence_);
        if (!safety_dispatch_plan.available
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
            && !liveness_failure_override_authorized
#endif
        ) {
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation,
                ingress_sequence,
                0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (!safety_dispatch_plan.available) {
            bypass_used = true;
        }
#endif

        const RuntimeOwnerInput input{
            receipt.kind == TrustedReceiptKind::OperationFailed
                ? RuntimeOwnerInputKind::LivenessOperationFailed
                : RuntimeOwnerInputKind::DeadlineExpired,
            receipt.effect_kind,
            receipt.correlation_id,
            receipt.mqtt_session_id,
            receipt.mqtt_generation,
            0,
            receipt.config_apply_epoch,
        };
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        last_liveness_failure_validation_bypass_used_ = bypass_used;
        if (bypass_used ||
            !canonical_liveness_failure_transition(
                before, receipt, transition, after)) {
#else
        if (!canonical_liveness_failure_transition(
                before, receipt, transition, after)) {
#endif
            physical_inflight_ = {};
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::TrustedHead,
                before.phase,
                after.phase,
                ingress_sequence);
        }

        add_saturating(effect_cancelled_count_, pending_effect_count_);
        pending_effects_ = {};
        pending_effect_head_ = 0;
        pending_effect_tail_ = 0;
        pending_effect_count_ = 0;

        const std::array<PendingEffectSlot, 2> pending_shadow{{
            {
                safety_dispatch_plan.record_fault_sequence,
                transition.effects[0],
            },
            {
                safety_dispatch_plan.enter_recovery_sequence,
                transition.effects[1],
            },
        }};
        for (std::size_t index = 0;
             index < pending_shadow.size(); ++index) {
            const std::uint8_t slot = static_cast<std::uint8_t>(
                (pending_effect_tail_ + index) % kPendingEffectCapacity);
            pending_effects_[slot] = pending_shadow[index];
        }
        pending_effect_tail_ = static_cast<std::uint8_t>(
            (pending_effect_tail_ + pending_shadow.size()) %
            kPendingEffectCapacity);
        pending_effect_count_ = static_cast<std::uint8_t>(
            pending_effect_count_ + pending_shadow.size());
        last_dispatch_sequence_ =
            safety_dispatch_plan.enter_recovery_sequence;
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        last_trusted_receipt_signature_ = {ingress_sequence, receipt};
        physical_inflight_ = {};
        accepted_liveness_mask_ = 0;
        if (safety_dispatch_plan.uses_terminal_reserve) {
            dispatch_fatal_latched_ = true;
        }

        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            ingress_sequence,
            0,
            0,
        };
    }

    if (receipt.kind == TrustedReceiptKind::SnapshotSucceeded ||
        receipt.kind == TrustedReceiptKind::SnapshotFailed) {
        if (last_trusted_receipt_signature_.ingress_sequence != 0 &&
            trusted_receipts_equal(
                receipt, last_trusted_receipt_signature_.receipt)) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

#if defined(NB_IOT_POST_CONFIG_HANDOFF_TRIAL)
        const bool liveness_gate_satisfied = accepted_liveness_mask_ == 0;
#else
        constexpr std::uint8_t kAllLivenessTickets = 0x0f;
        const bool liveness_gate_satisfied =
            accepted_liveness_mask_ == kAllLivenessTickets;
#endif
        if (before.phase != RuntimeOwnerPhase::SnapshotFreezePending ||
            before.boot_orchestration_ended ||
            !liveness_gate_satisfied ||
            !exact_config_issued_snapshot_token(before, receipt)) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        const bool snapshot_failed =
            receipt.kind == TrustedReceiptKind::SnapshotFailed;
        constexpr std::uint32_t kLastEndBootDispatchStart =
            std::numeric_limits<std::uint32_t>::max() - 3;
        const bool end_boot_sequence_available =
            last_dispatch_sequence_ <= kLastEndBootDispatchStart;
        if (!snapshot_failed && !end_boot_sequence_available
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
            && !snapshot_override_authorized
#endif
        ) {
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation,
                ingress_sequence,
                0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        if (!snapshot_failed && !end_boot_sequence_available) {
            bypass_used = true;
        }
#endif
        SafetyPairDispatchPlan safety_dispatch_plan{};
        if (snapshot_failed) {
            safety_dispatch_plan =
                plan_safety_pair_dispatch(last_dispatch_sequence_);
            if (!safety_dispatch_plan.available
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
                && !snapshot_override_authorized
#endif
            ) {
                record_critical(
                    AdapterCriticalReason::DispatchSequenceSaturation,
                    ingress_sequence,
                    0);
                return {
                    AdapterStepAction::CriticalLedgerHandled,
                    RuntimeOwnerDisposition::Rejected,
                    before.phase,
                    before.phase,
                    0,
                    0,
                    0,
                };
            }
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
            if (!safety_dispatch_plan.available) {
                bypass_used = true;
            }
#endif
        }

        const RuntimeOwnerInput input{
            snapshot_failed
                ? RuntimeOwnerInputKind::SnapshotFreezeFailed
                : RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
            RuntimeOwnerEffectKind::FreezeBootSnapshot,
            receipt.correlation_id,
            receipt.mqtt_session_id,
            receipt.mqtt_generation,
            0,
            receipt.config_apply_epoch,
        };
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
        const bool canonical = snapshot_failed
            ? canonical_snapshot_failed_transition(
                  before, receipt, transition, after)
            : canonical_snapshot_succeeded_transition(
                  before, transition, after);
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
        last_snapshot_validation_bypass_used_ = bypass_used;
        if (bypass_used || !canonical) {
#else
        if (!canonical) {
#endif
            physical_inflight_ = {};
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::TrustedHead,
                before.phase,
                after.phase,
                ingress_sequence);
        }

        if (snapshot_failed) {
            const std::array<PendingEffectSlot, 2> pending_shadow{{
                {
                    safety_dispatch_plan.record_fault_sequence,
                    transition.effects[0],
                },
                {
                    safety_dispatch_plan.enter_recovery_sequence,
                    transition.effects[1],
                },
            }};
            for (std::size_t index = 0;
                 index < pending_shadow.size(); ++index) {
                const std::uint8_t slot = static_cast<std::uint8_t>(
                    (pending_effect_tail_ + index) %
                    kPendingEffectCapacity);
                pending_effects_[slot] = pending_shadow[index];
            }
            pending_effect_tail_ = static_cast<std::uint8_t>(
                (pending_effect_tail_ + pending_shadow.size()) %
                kPendingEffectCapacity);
            pending_effect_count_ = static_cast<std::uint8_t>(
                pending_effect_count_ + pending_shadow.size());
            last_dispatch_sequence_ =
                safety_dispatch_plan.enter_recovery_sequence;
        } else {
            const std::uint32_t end_boot_sequence =
                last_dispatch_sequence_ + 1;
            pending_effects_[pending_effect_tail_] = {
                end_boot_sequence,
                transition.effects[0],
            };
            pending_effect_tail_ = static_cast<std::uint8_t>(
                (pending_effect_tail_ + 1) % kPendingEffectCapacity);
            ++pending_effect_count_;
            last_dispatch_sequence_ = end_boot_sequence;
        }

        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        last_trusted_receipt_signature_ = {ingress_sequence, receipt};
        physical_inflight_ = {};
        accepted_liveness_mask_ = 0;
        if (snapshot_failed) {
            if (safety_dispatch_plan.uses_terminal_reserve) {
                dispatch_fatal_latched_ = true;
            }
        }

        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            ingress_sequence,
            0,
            0,
        };
    }

    if (receipt.kind == TrustedReceiptKind::TransportAttemptFailed) {
        if (last_trusted_receipt_signature_.ingress_sequence != 0 &&
            trusted_receipts_equal(
                receipt, last_trusted_receipt_signature_.receipt)) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        if (before.phase != RuntimeOwnerPhase::TransportConnecting ||
            before.boot_orchestration_ended ||
            receipt.mqtt_generation != before.mqtt_generation_counter) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        const SafetyPairDispatchPlan safety_dispatch_plan =
            plan_safety_pair_dispatch(last_dispatch_sequence_);
        if (!safety_dispatch_plan.available) {
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation,
                ingress_sequence,
                0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }

        const RuntimeOwnerInput input{
            RuntimeOwnerInputKind::TransportAttemptFailed,
            RuntimeOwnerEffectKind::StartTransportAttempt,
            0,
            0,
            receipt.mqtt_generation,
            0,
            0,
        };
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
        if (!canonical_transport_attempt_failed_transition(
                before, receipt, transition, after)) {
            physical_inflight_ = {};
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::TrustedHead,
                before.phase,
                after.phase,
                ingress_sequence);
        }

        const std::array<PendingEffectSlot, 2> pending_shadow{{
            {
                safety_dispatch_plan.record_fault_sequence,
                transition.effects[0],
            },
            {
                safety_dispatch_plan.enter_recovery_sequence,
                transition.effects[1],
            },
        }};
        for (std::size_t index = 0;
             index < pending_shadow.size(); ++index) {
            const std::uint8_t slot = static_cast<std::uint8_t>(
                (pending_effect_tail_ + index) % kPendingEffectCapacity);
            pending_effects_[slot] = pending_shadow[index];
        }
        pending_effect_tail_ = static_cast<std::uint8_t>(
            (pending_effect_tail_ + pending_shadow.size()) %
            kPendingEffectCapacity);
        pending_effect_count_ = static_cast<std::uint8_t>(
            pending_effect_count_ + pending_shadow.size());
        last_dispatch_sequence_ =
            safety_dispatch_plan.enter_recovery_sequence;
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        last_trusted_receipt_signature_ = {ingress_sequence, receipt};
        physical_inflight_ = {};
        accepted_liveness_mask_ = 0;
        if (safety_dispatch_plan.uses_terminal_reserve) {
            dispatch_fatal_latched_ = true;
        }

        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            ingress_sequence,
            0,
            0,
        };
    }

    if (receipt.kind == TrustedReceiptKind::TransportDisconnected) {
        if (last_trusted_receipt_signature_.ingress_sequence != 0 &&
            trusted_receipts_equal(
                receipt, last_trusted_receipt_signature_.receipt)) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_duplicate_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        const bool phase_allowed =
            before.phase == RuntimeOwnerPhase::AwaitingConfigCommit ||
            before.phase == RuntimeOwnerPhase::LivenessWaiting ||
            before.phase == RuntimeOwnerPhase::SnapshotFreezePending ||
            before.phase == RuntimeOwnerPhase::RuntimeReady;
        if (!phase_allowed ||
            receipt.mqtt_session_id != before.mqtt_session_id ||
            receipt.mqtt_generation != before.mqtt_generation) {
            envelope = {};
            trusted_head_ = static_cast<std::uint8_t>(
                (trusted_head_ + 1) % kTrustedQueueCapacity);
            --trusted_count_;
            increment_saturating(trusted_stale_count_);
            return {
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                ingress_sequence,
                0,
                0,
            };
        }

        const SafetyPairDispatchPlan safety_dispatch_plan =
            plan_safety_pair_dispatch(last_dispatch_sequence_);
        if (!safety_dispatch_plan.available) {
            record_critical(
                AdapterCriticalReason::DispatchSequenceSaturation,
                ingress_sequence,
                0);
            return {
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                before.phase,
                before.phase,
                0,
                0,
                0,
            };
        }

        const RuntimeOwnerInput input{
            RuntimeOwnerInputKind::TransportDisconnected,
            RuntimeOwnerEffectKind::None,
            0,
            receipt.mqtt_session_id,
            receipt.mqtt_generation,
            0,
            0,
        };
        const RuntimeOwnerTransition transition = submit_core_input(input);
        const RuntimeOwnerView after = view_after_core_submit();
        if (!canonical_transport_disconnected_transition(
                before, receipt, transition, after)) {
            return handle_malformed_core_transition(
                MalformedTransitionOrigin::TrustedHead,
                before.phase,
                after.phase,
                ingress_sequence);
        }

        cancel_non_safety_authorization();
        const std::array<PendingEffectSlot, 2> pending_shadow{{
            {
                safety_dispatch_plan.record_fault_sequence,
                transition.effects[0],
            },
            {
                safety_dispatch_plan.enter_recovery_sequence,
                transition.effects[1],
            },
        }};
        for (std::size_t index = 0;
             index < pending_shadow.size(); ++index) {
            const std::uint8_t slot = static_cast<std::uint8_t>(
                (pending_effect_tail_ + index) % kPendingEffectCapacity);
            pending_effects_[slot] = pending_shadow[index];
        }
        pending_effect_tail_ = static_cast<std::uint8_t>(
            (pending_effect_tail_ + pending_shadow.size()) %
            kPendingEffectCapacity);
        pending_effect_count_ = static_cast<std::uint8_t>(
            pending_effect_count_ + pending_shadow.size());
        last_dispatch_sequence_ =
            safety_dispatch_plan.enter_recovery_sequence;
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        last_trusted_receipt_signature_ = {ingress_sequence, receipt};
        accepted_liveness_mask_ = 0;
        if (safety_dispatch_plan.uses_terminal_reserve) {
            dispatch_fatal_latched_ = true;
        }

        return {
            AdapterStepAction::CoreTransitionApplied,
            transition.disposition,
            transition.phase_before,
            transition.phase_after,
            ingress_sequence,
            0,
            0,
        };
    }

    if (receipt.kind != TrustedReceiptKind::TransportEstablished) {
        return canonical_idle_step(before.phase);
    }

    if (before.phase != RuntimeOwnerPhase::TransportConnecting ||
        before.boot_orchestration_ended ||
        receipt.mqtt_generation != before.mqtt_generation_counter) {
        envelope = {};
        trusted_head_ = static_cast<std::uint8_t>(
            (trusted_head_ + 1) % kTrustedQueueCapacity);
        --trusted_count_;
        return {
            AdapterStepAction::TrustedReceiptDiscarded,
            RuntimeOwnerDisposition::Rejected,
            before.phase,
            before.phase,
            ingress_sequence,
            0,
            0,
        };
    }

    const RuntimeOwnerInput input{
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerEffectKind::None,
        0,
        receipt.mqtt_session_id,
        receipt.mqtt_generation,
        0,
        0,
    };
    const RuntimeOwnerTransition transition = submit_core_input(input);
    const RuntimeOwnerView after = view_after_core_submit();
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
    if (bypass_used ||
        !canonical_transport_established_transition(
            before, receipt, transition, after)) {
#else
    if (!canonical_transport_established_transition(
            before, receipt, transition, after)) {
#endif
        physical_inflight_ = {};
        return handle_malformed_core_transition(
            MalformedTransitionOrigin::TrustedHead,
            before.phase,
            after.phase,
            ingress_sequence);
    }

    envelope = {};
    trusted_head_ = static_cast<std::uint8_t>(
        (trusted_head_ + 1) % kTrustedQueueCapacity);
    --trusted_count_;
    last_trusted_receipt_signature_ = {ingress_sequence, receipt};
    physical_inflight_ = {};

    return {
        AdapterStepAction::CoreTransitionApplied,
        transition.disposition,
        transition.phase_before,
        transition.phase_after,
        ingress_sequence,
        0,
        0,
    };
}

AdapterDispatch RuntimeOwnerAdapterCore::peek_dispatch() const noexcept
{
    return current_dispatch_;
}

DispatchAckResult RuntimeOwnerAdapterCore::acknowledge_dispatch(
    const std::uint32_t dispatch_sequence) noexcept
{
    if (current_dispatch_.kind == AdapterDispatchKind::None) {
        if (dispatch_sequence != 0 &&
            dispatch_sequence == last_ack_dispatch_sequence_) {
            return DispatchAckResult::AcceptedDuplicate;
        }
        increment_saturating(dispatch_rejected_ack_count_);
        return DispatchAckResult::RejectedNoDispatch;
    }

    if (dispatch_sequence != current_dispatch_.dispatch_sequence) {
        if (dispatch_sequence != 0 &&
            dispatch_sequence == last_ack_dispatch_sequence_) {
            return DispatchAckResult::AcceptedDuplicate;
        }
        increment_saturating(dispatch_rejected_ack_count_);
        return DispatchAckResult::RejectedWrongSequence;
    }

    const AdapterDispatch acknowledged = current_dispatch_;
    current_dispatch_ = {};
    last_ack_dispatch_sequence_ = dispatch_sequence;
    const bool delivery_only =
        acknowledged.kind == AdapterDispatchKind::CoreEffect &&
        is_delivery_only_effect(acknowledged.effect.kind);
    if (delivery_only) {
        if (acknowledged.effect.kind ==
            RuntimeOwnerEffectKind::EndBootOrchestration) {
            boot_end_released_ = true;
        }
        return DispatchAckResult::AcceptedDelivery;
    }

    physical_inflight_ = acknowledged;
    return DispatchAckResult::AcceptedOperationInflight;
}

UrgentRequestResult RuntimeOwnerAdapterCore::request_shutdown() noexcept
{
    if (core_.view().phase == RuntimeOwnerPhase::ShutdownCommitted ||
        shutdown_terminal_override_latched_) {
        return UrgentRequestResult::AlreadyTerminal;
    }
    if (shutdown_pending_) {
        return UrgentRequestResult::AcceptedDuplicate;
    }
    shutdown_pending_ = true;
    return UrgentRequestResult::Accepted;
}

} // namespace boot_v2
