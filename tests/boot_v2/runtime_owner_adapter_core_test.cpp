#include "runtime_owner_adapter_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
namespace boot_v2 {

class RuntimeOwnerCoreTestPeer {
public:
    static void fixture_prepare_awaiting_config(
        RuntimeOwnerCore &core,
        const std::uint32_t session_id,
        const std::uint32_t generation,
        const std::uint32_t last_config_commit_sequence) noexcept
    {
        core.phase_ = RuntimeOwnerPhase::AwaitingConfigCommit;
        core.last_fault_ = RuntimeOwnerFaultCode::None;
        core.mqtt_generation_counter_ = generation;
        core.active_mqtt_session_id_ = session_id;
        core.active_mqtt_generation_ = generation;
        core.last_config_commit_sequence_ = last_config_commit_sequence;
        core.boot_orchestration_ended_ = false;
        core.fatal_latched_ = false;
        core.has_last_failure_ = false;
        core.last_failure_ = {};
    }

    static void fixture_set_config_counters(
        RuntimeOwnerCore &core,
        const std::uint32_t config_apply_epoch_counter,
        const std::uint32_t correlation_id_counter) noexcept
    {
        core.config_apply_epoch_counter_ = config_apply_epoch_counter;
        core.correlation_id_counter_ = correlation_id_counter;
    }

    static void fixture_set_boot_orchestration_ended(
        RuntimeOwnerCore &core,
        const bool ended) noexcept
    {
        core.boot_orchestration_ended_ = ended;
    }
};

bool RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_runtime_ready(
    RuntimeOwnerAdapterCore &adapter) noexcept
{
    RuntimeOwnerTransition transition = adapter.core_.submit({
        RuntimeOwnerInputKind::BeginTransportAttempt,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        0,
        0,
        0,
    });
    if (transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.effect_count != 1 ||
        transition.effects[0].kind !=
            RuntimeOwnerEffectKind::StartTransportAttempt) {
        return false;
    }

    const std::uint32_t generation =
        transition.effects[0].attempt.mqtt_generation;
    constexpr std::uint32_t kFixtureSession = 1;
    transition = adapter.core_.submit({
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerEffectKind::None,
        0,
        kFixtureSession,
        generation,
        0,
        0,
    });
    if (transition.disposition != RuntimeOwnerDisposition::Accepted) {
        return false;
    }

    transition = adapter.core_.submit({
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        kFixtureSession,
        generation,
        1,
        0,
    });
    if (transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.effect_count != 4) {
        return false;
    }

    const std::array<RuntimeOwnerEffect, 4> tickets = transition.effects;
    RuntimeOwnerEffect freeze{};
    for (std::size_t index = 0; index < tickets.size(); ++index) {
        const RuntimeOwnerEffect ticket = tickets[index];
        transition = adapter.core_.submit({
            RuntimeOwnerInputKind::LivenessOperationCompleted,
            ticket.kind,
            ticket.correlation_id,
            ticket.attempt.mqtt_session_id,
            ticket.attempt.mqtt_generation,
            0,
            ticket.attempt.config_apply_epoch,
        });
        if (transition.disposition != RuntimeOwnerDisposition::Accepted) {
            return false;
        }
        if (transition.effect_count == 1 &&
            transition.effects[0].kind ==
                RuntimeOwnerEffectKind::FreezeBootSnapshot) {
            freeze = transition.effects[0];
        }
    }
    if (freeze.kind != RuntimeOwnerEffectKind::FreezeBootSnapshot) {
        return false;
    }

    transition = adapter.core_.submit({
        RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
        freeze.kind,
        freeze.correlation_id,
        freeze.attempt.mqtt_session_id,
        freeze.attempt.mqtt_generation,
        0,
        freeze.attempt.config_apply_epoch,
    });
    const RuntimeOwnerView ready = adapter.core_.view();
    return transition.disposition == RuntimeOwnerDisposition::Accepted &&
           ready.phase == RuntimeOwnerPhase::RuntimeReady &&
           ready.boot_orchestration_ended;
}

bool RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_recovery_pending(
    RuntimeOwnerAdapterCore &adapter) noexcept
{
    const RuntimeOwnerTransition transition = adapter.core_.submit({
        RuntimeOwnerInputKind::CriticalIngressFault,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        0,
        0,
        0,
    });
    return transition.disposition == RuntimeOwnerDisposition::Accepted &&
           transition.phase_after == RuntimeOwnerPhase::RecoveryPending &&
           adapter.core_.view().phase == RuntimeOwnerPhase::RecoveryPending;
}

bool RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
    RuntimeOwnerAdapterCore &adapter,
    const RuntimeOwnerPhase phase) noexcept
{
    if (phase == RuntimeOwnerPhase::ColdStart) {
        return adapter.core_.view().phase == RuntimeOwnerPhase::ColdStart;
    }
    if (phase == RuntimeOwnerPhase::RecoveryPending) {
        return fixture_drive_core_to_recovery_pending(adapter);
    }
    if (phase == RuntimeOwnerPhase::ShutdownCommitted) {
        fixture_commit_core_shutdown(adapter);
        return adapter.core_.view().phase ==
               RuntimeOwnerPhase::ShutdownCommitted;
    }
    if (phase == RuntimeOwnerPhase::RuntimeReady) {
        return fixture_drive_core_to_runtime_ready(adapter);
    }

    RuntimeOwnerTransition transition = adapter.core_.submit({
        RuntimeOwnerInputKind::BeginTransportAttempt,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        0,
        0,
        0,
    });
    if (transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.effect_count != 1) {
        return false;
    }
    if (phase == RuntimeOwnerPhase::TransportConnecting) {
        return adapter.core_.view().phase ==
               RuntimeOwnerPhase::TransportConnecting;
    }

    const std::uint32_t generation =
        transition.effects[0].attempt.mqtt_generation;
    constexpr std::uint32_t kFixtureSession = 1;
    transition = adapter.core_.submit({
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerEffectKind::None,
        0,
        kFixtureSession,
        generation,
        0,
        0,
    });
    if (transition.disposition != RuntimeOwnerDisposition::Accepted) {
        return false;
    }
    if (phase == RuntimeOwnerPhase::AwaitingConfigCommit) {
        return adapter.core_.view().phase ==
               RuntimeOwnerPhase::AwaitingConfigCommit;
    }

    transition = adapter.core_.submit({
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        kFixtureSession,
        generation,
        1,
        0,
    });
    if (transition.disposition != RuntimeOwnerDisposition::Accepted ||
        transition.effect_count != 4) {
        return false;
    }
    if (phase == RuntimeOwnerPhase::LivenessWaiting) {
        return adapter.core_.view().phase ==
               RuntimeOwnerPhase::LivenessWaiting;
    }
    if (phase != RuntimeOwnerPhase::SnapshotFreezePending) {
        return false;
    }

    const std::array<RuntimeOwnerEffect, 4> tickets = transition.effects;
    for (const RuntimeOwnerEffect ticket : tickets) {
        transition = adapter.core_.submit({
            RuntimeOwnerInputKind::LivenessOperationCompleted,
            ticket.kind,
            ticket.correlation_id,
            ticket.attempt.mqtt_session_id,
            ticket.attempt.mqtt_generation,
            0,
            ticket.attempt.config_apply_epoch,
        });
        if (transition.disposition != RuntimeOwnerDisposition::Accepted) {
            return false;
        }
    }
    return adapter.core_.view().phase ==
           RuntimeOwnerPhase::SnapshotFreezePending;
}

bool RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_post_boot_recovery(
    RuntimeOwnerAdapterCore &adapter) noexcept
{
    if (!fixture_drive_core_to_runtime_ready(adapter)) {
        return false;
    }
    const RuntimeOwnerTransition transition = adapter.core_.submit({
        RuntimeOwnerInputKind::CriticalIngressFault,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        0,
        0,
        0,
    });
    const RuntimeOwnerView view = adapter.core_.view();
    return transition.disposition == RuntimeOwnerDisposition::Accepted &&
           view.phase == RuntimeOwnerPhase::RecoveryPending &&
           view.boot_orchestration_ended;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t session_id,
    const std::uint32_t generation,
    const std::uint32_t last_config_commit_sequence) noexcept
{
    RuntimeOwnerCoreTestPeer::fixture_prepare_awaiting_config(
        adapter.core_,
        session_id,
        generation,
        last_config_commit_sequence);
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t config_apply_epoch_counter,
    const std::uint32_t correlation_id_counter) noexcept
{
    RuntimeOwnerCoreTestPeer::fixture_set_config_counters(
        adapter.core_,
        config_apply_epoch_counter,
        correlation_id_counter);
}

void RuntimeOwnerAdapterCoreTestPeer::
    fixture_set_core_boot_orchestration_ended(
        RuntimeOwnerAdapterCore &adapter,
        const bool ended) noexcept
{
    RuntimeOwnerCoreTestPeer::fixture_set_boot_orchestration_ended(
        adapter.core_, ended);
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_boot_end_released(
    RuntimeOwnerAdapterCore &adapter,
    const bool released) noexcept
{
    adapter.boot_end_released_ = released;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_commit_core_shutdown(
    RuntimeOwnerAdapterCore &adapter) noexcept
{
    (void)adapter.core_.submit({
        RuntimeOwnerInputKind::ShutdownCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        0,
        0,
        0,
    });
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
    RuntimeOwnerAdapterCore &adapter,
    const bool latched) noexcept
{
    adapter.core_adapter_fatal_latched_ = latched;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_fail_closed(
    RuntimeOwnerAdapterCore &adapter,
    const bool latched) noexcept
{
    adapter.core_fail_closed_latched_ = latched;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_sequence_fatal(
    RuntimeOwnerAdapterCore &adapter,
    const bool latched) noexcept
{
    adapter.sequence_fatal_latched_ = latched;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_dispatch_fatal(
    RuntimeOwnerAdapterCore &adapter,
    const bool latched) noexcept
{
    adapter.dispatch_fatal_latched_ = latched;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_safety_delivery_blocked(
    RuntimeOwnerAdapterCore &adapter,
    const bool blocked) noexcept
{
    adapter.safety_delivery_blocked_ = blocked;
}

void RuntimeOwnerAdapterCoreTestPeer::
    fixture_set_shutdown_terminal_override(
        RuntimeOwnerAdapterCore &adapter,
        const bool latched) noexcept
{
    adapter.shutdown_terminal_override_latched_ = latched;
}

void RuntimeOwnerAdapterCoreTestPeer::
    fixture_set_last_normal_enqueue_sequence(
        RuntimeOwnerAdapterCore &adapter,
        const std::uint32_t sequence) noexcept
{
    adapter.last_normal_enqueue_sequence_ = sequence;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_normal_diagnostic_counts(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t coalesced,
    const std::uint32_t rejected_full) noexcept
{
    adapter.normal_coalesced_count_ = coalesced;
    adapter.normal_rejected_full_count_ = rejected_full;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_trusted_ingress_sequence(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t sequence) noexcept
{
    adapter.last_trusted_ingress_sequence_ = sequence;
}

void RuntimeOwnerAdapterCoreTestPeer::
    fixture_set_last_trusted_receipt_signature(
        RuntimeOwnerAdapterCore &adapter,
        const std::uint32_t ingress_sequence,
        const TrustedReceipt receipt) noexcept
{
    adapter.last_trusted_receipt_signature_ = {
        ingress_sequence,
        receipt,
    };
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t sequence) noexcept
{
    adapter.last_dispatch_sequence_ = sequence;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
    RuntimeOwnerAdapterCore &adapter) noexcept
{
    adapter.pending_effects_ = {};
    adapter.pending_effect_head_ = 0;
    adapter.pending_effect_tail_ = 0;
    adapter.pending_effect_count_ = 0;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_accepted_liveness_mask(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint8_t mask) noexcept
{
    adapter.accepted_liveness_mask_ = mask;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_trusted_diagnostic_counts(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t rejected_full,
    const std::uint32_t protocol_violation) noexcept
{
    adapter.trusted_rejected_full_count_ = rejected_full;
    adapter.trusted_protocol_violation_count_ = protocol_violation;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_set_critical_occurrence_count(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t occurrence_count) noexcept
{
    adapter.critical_.occurrence_count = occurrence_count;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
    RuntimeOwnerAdapterCore &adapter,
    const RuntimeOwnerTransition transition) noexcept
{
    adapter.core_transition_override_ = transition;
    adapter.core_transition_override_pending_ = true;
}

void RuntimeOwnerAdapterCoreTestPeer::
    fixture_override_next_core_post_submit_view(
        RuntimeOwnerAdapterCore &adapter,
        const RuntimeOwnerView view) noexcept
{
    adapter.core_post_submit_view_override_ = view;
    adapter.core_post_submit_view_override_pending_ = true;
}

void RuntimeOwnerAdapterCoreTestPeer::fixture_seed_begin_fallback_cleanup_state(
    RuntimeOwnerAdapterCore &adapter) noexcept
{
    adapter.normal_queue_[0] = {
        {
            NormalIntentKind::PublishTelemetry,
            0,
            0,
            77,
            9,
        },
        41,
    };
    adapter.normal_head_ = 0;
    adapter.normal_tail_ = 1;
    adapter.normal_count_ = 1;
    adapter.normal_high_water_ = 1;
    adapter.last_normal_enqueue_sequence_ = 41;

    adapter.trusted_queue_[0] = {
        TrustedIngressPayloadKind::CoreReceipt,
        {},
        43,
        {
            TrustedReceiptKind::TransportEstablished,
            RuntimeOwnerEffectKind::StartTransportAttempt,
            0,
            0,
            77,
            1,
            0,
            0,
            0,
        },
        {},
    };
    adapter.trusted_head_ = 0;
    adapter.trusted_tail_ = 1;
    adapter.trusted_count_ = 1;
    adapter.trusted_high_water_ = 1;
    adapter.last_trusted_ingress_sequence_ = 43;

    adapter.pending_effects_[0] = {
        47,
        {
            RuntimeOwnerEffectKind::StartAtProbe,
            53,
            {77, 1, 2},
            RuntimeOwnerFaultCode::None,
        },
    };
    adapter.pending_effect_head_ = 0;
    adapter.pending_effect_tail_ = 1;
    adapter.pending_effect_count_ = 1;
    adapter.accepted_liveness_mask_ = 0x05;
}

void RuntimeOwnerAdapterCoreTestPeer::
    fixture_seed_trusted_fallback_nonqueue_state(
        RuntimeOwnerAdapterCore &adapter) noexcept
{
    adapter.normal_queue_[0] = {
        {
            NormalIntentKind::PublishTelemetry,
            0,
            0,
            88,
            12,
        },
        51,
    };
    adapter.normal_head_ = 0;
    adapter.normal_tail_ = 1;
    adapter.normal_count_ = 1;
    adapter.normal_high_water_ = 1;
    adapter.last_normal_enqueue_sequence_ = 51;

    adapter.pending_effects_[2] = {
        57,
        {
            RuntimeOwnerEffectKind::StartProbePublish,
            61,
            {77, 1, 2},
            RuntimeOwnerFaultCode::None,
        },
    };
    adapter.pending_effect_head_ = 2;
    adapter.pending_effect_tail_ = 3;
    adapter.pending_effect_count_ = 1;
    adapter.accepted_liveness_mask_ = 0x0a;

    adapter.last_trusted_receipt_signature_ = {
        67,
        {
            TrustedReceiptKind::TransportAttemptFailed,
            RuntimeOwnerEffectKind::StartTransportAttempt,
            0,
            0,
            0,
            1,
            0,
            0,
            71,
        },
    };
    adapter.trusted_stale_count_ = 73;
    adapter.trusted_duplicate_count_ = 79;
    adapter.trusted_protocol_violation_count_ = 83;
}

void RuntimeOwnerAdapterCoreTestPeer::
    fixture_seed_authorization_pending_effect(
        RuntimeOwnerAdapterCore &adapter) noexcept
{
    adapter.pending_effects_ = {};
    adapter.pending_effects_[0] = {
        57,
        {
            RuntimeOwnerEffectKind::StartProbePublish,
            61,
            {77, 1, 2},
            RuntimeOwnerFaultCode::None,
        },
    };
    adapter.pending_effect_head_ = 0;
    adapter.pending_effect_tail_ = 1;
    adapter.pending_effect_count_ = 1;
}

std::uint32_t RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
    const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.core_submit_count_;
}

bool RuntimeOwnerAdapterCoreTestPeer::
    fixture_core_transition_override_pending(
        const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.core_transition_override_pending_;
}

bool RuntimeOwnerAdapterCoreTestPeer::
    fixture_core_post_submit_view_override_pending(
        const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.core_post_submit_view_override_pending_;
}

bool RuntimeOwnerAdapterCoreTestPeer::
    fixture_last_snapshot_validation_bypass_used(
        const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.last_snapshot_validation_bypass_used_;
}

bool RuntimeOwnerAdapterCoreTestPeer::
    fixture_last_config_validation_bypass_used(
        const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.last_config_validation_bypass_used_;
}

bool RuntimeOwnerAdapterCoreTestPeer::
    fixture_last_operation_completed_validation_bypass_used(
        const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.last_operation_completed_validation_bypass_used_;
}

bool RuntimeOwnerAdapterCoreTestPeer::
    fixture_last_liveness_failure_validation_bypass_used(
        const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.last_liveness_failure_validation_bypass_used_;
}

RuntimeOwnerAdapterCoreTestPeer::TrustedEnqueueResult
RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
    RuntimeOwnerAdapterCore &adapter,
    const TrustedReceipt input) noexcept
{
    return adapter.trusted_receipt_port().submit(input);
}

RuntimeOwnerAdapterCoreTestPeer::TrustedEnqueueResult
RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
    RuntimeOwnerAdapterCore &adapter,
    const NormalCompletion input) noexcept
{
    return adapter.normal_completion_port().submit(input);
}

bool RuntimeOwnerAdapterCoreTestPeer::
    fixture_enqueue_trusted_receipt_unchecked(
        RuntimeOwnerAdapterCore &adapter,
        const TrustedReceipt input) noexcept
{
    if (adapter.trusted_count_ ==
            RuntimeOwnerAdapterCore::kTrustedQueueCapacity ||
        adapter.last_trusted_ingress_sequence_ ==
            std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const std::uint32_t ingress_sequence =
        adapter.last_trusted_ingress_sequence_ + 1;
    adapter.trusted_queue_[adapter.trusted_tail_] = {
        TrustedIngressPayloadKind::CoreReceipt,
        {},
        ingress_sequence,
        input,
        {},
    };
    adapter.trusted_tail_ = static_cast<std::uint8_t>(
        (adapter.trusted_tail_ + 1) %
        RuntimeOwnerAdapterCore::kTrustedQueueCapacity);
    ++adapter.trusted_count_;
    if (adapter.trusted_high_water_ < adapter.trusted_count_) {
        adapter.trusted_high_water_ = adapter.trusted_count_;
    }
    adapter.last_trusted_ingress_sequence_ = ingress_sequence;
    if (input.diagnostic_code != 0) {
        adapter.last_trusted_diagnostic_ingress_sequence_ =
            ingress_sequence;
        adapter.last_trusted_diagnostic_code_ = input.diagnostic_code;
    }
    return true;
}

bool RuntimeOwnerAdapterCoreTestPeer::fixture_consume_normal(
    RuntimeOwnerAdapterCore &adapter,
    NormalIntent &intent,
    std::uint32_t &enqueue_sequence) noexcept
{
    if (adapter.normal_count_ == 0) {
        return false;
    }
    RuntimeOwnerAdapterCore::NormalQueueEntry &entry =
        adapter.normal_queue_[adapter.normal_head_];
    intent = entry.intent;
    enqueue_sequence = entry.enqueue_sequence;
    entry = {};
    adapter.normal_head_ = static_cast<std::uint8_t>(
        (adapter.normal_head_ + 1) %
        RuntimeOwnerAdapterCore::kNormalQueueCapacity);
    --adapter.normal_count_;
    return true;
}

bool RuntimeOwnerAdapterCoreTestPeer::fixture_consume_trusted(
    RuntimeOwnerAdapterCore &adapter,
    TrustedIngressEnvelope &envelope) noexcept
{
    if (adapter.trusted_count_ == 0) {
        return false;
    }
    TrustedIngressEnvelope &entry =
        adapter.trusted_queue_[adapter.trusted_head_];
    envelope = entry;
    entry = {};
    adapter.trusted_head_ = static_cast<std::uint8_t>(
        (adapter.trusted_head_ + 1) %
        RuntimeOwnerAdapterCore::kTrustedQueueCapacity);
    --adapter.trusted_count_;
    return true;
}

RuntimeOwnerAdapterPrivateSnapshot RuntimeOwnerAdapterCoreTestPeer::snapshot(
    const RuntimeOwnerAdapterCore &adapter) noexcept
{
    RuntimeOwnerAdapterPrivateSnapshot result{};
    result.core = adapter.core_.view();
    result.current_dispatch = adapter.current_dispatch_;
    result.physical_inflight = adapter.physical_inflight_;
    for (std::size_t index = 0; index < result.normal_slots.size(); ++index) {
        result.normal_slots[index] = {
            adapter.normal_queue_[index].intent,
            adapter.normal_queue_[index].enqueue_sequence,
        };
    }
    for (std::size_t index = 0; index < result.trusted_slots.size(); ++index) {
        const RuntimeOwnerAdapterCore::TrustedIngressEnvelope &entry =
            adapter.trusted_queue_[index];
        result.trusted_slots[index] = {
            static_cast<std::uint8_t>(entry.kind),
            entry.reserved,
            entry.ingress_sequence,
            entry.receipt,
            entry.normal_completion,
        };
    }
    for (std::size_t index = 0;
         index < result.pending_effect_slots.size(); ++index) {
        result.pending_effect_slots[index] = {
            adapter.pending_effects_[index].preassigned_dispatch_sequence,
            adapter.pending_effects_[index].effect,
        };
    }
    result.last_trusted_receipt_signature = {
        adapter.last_trusted_receipt_signature_.ingress_sequence,
        adapter.last_trusted_receipt_signature_.receipt,
    };
    result.last_normal_completion_signature = {
        adapter.last_normal_completion_signature_.ingress_sequence,
        adapter.last_normal_completion_signature_.completion,
    };
    result.critical = adapter.critical_;
    result.last_normal_enqueue_sequence =
        adapter.last_normal_enqueue_sequence_;
    result.last_trusted_ingress_sequence =
        adapter.last_trusted_ingress_sequence_;
    result.last_dispatch_sequence = adapter.last_dispatch_sequence_;
    result.last_ack_dispatch_sequence =
        adapter.last_ack_dispatch_sequence_;
    result.last_trusted_diagnostic_ingress_sequence =
        adapter.last_trusted_diagnostic_ingress_sequence_;
    result.last_trusted_diagnostic_code =
        adapter.last_trusted_diagnostic_code_;
    result.normal_coalesced_count = adapter.normal_coalesced_count_;
    result.normal_rejected_full_count = adapter.normal_rejected_full_count_;
    result.normal_cancelled_count = adapter.normal_cancelled_count_;
    result.dispatch_rejected_ack_count =
        adapter.dispatch_rejected_ack_count_;
    result.trusted_rejected_full_count =
        adapter.trusted_rejected_full_count_;
    result.trusted_protocol_violation_count =
        adapter.trusted_protocol_violation_count_;
    result.trusted_stale_count = adapter.trusted_stale_count_;
    result.trusted_duplicate_count = adapter.trusted_duplicate_count_;
    result.trusted_cancelled_count = adapter.trusted_cancelled_count_;
    result.effect_cancelled_count = adapter.effect_cancelled_count_;
    result.normal_completion_stale_count =
        adapter.normal_completion_stale_count_;
    result.normal_head = adapter.normal_head_;
    result.normal_tail = adapter.normal_tail_;
    result.normal_count = adapter.normal_count_;
    result.normal_high_water = adapter.normal_high_water_;
    result.trusted_head = adapter.trusted_head_;
    result.trusted_tail = adapter.trusted_tail_;
    result.trusted_count = adapter.trusted_count_;
    result.trusted_high_water = adapter.trusted_high_water_;
    result.pending_effect_head = adapter.pending_effect_head_;
    result.pending_effect_tail = adapter.pending_effect_tail_;
    result.pending_effect_count = adapter.pending_effect_count_;
    result.accepted_liveness_mask = adapter.accepted_liveness_mask_;
    result.transport_request_pending =
        adapter.transport_request_pending_;
    result.shutdown_pending = adapter.shutdown_pending_;
    result.shutdown_terminal_override_latched =
        adapter.shutdown_terminal_override_latched_;
    result.boot_end_released = adapter.boot_end_released_;
    result.critical_pending = adapter.critical_pending_;
    result.core_fail_closed_latched = adapter.core_fail_closed_latched_;
    result.core_adapter_fatal_latched =
        adapter.core_adapter_fatal_latched_;
    result.sequence_fatal_latched = adapter.sequence_fatal_latched_;
    result.dispatch_fatal_latched = adapter.dispatch_fatal_latched_;
    result.safety_delivery_blocked = adapter.safety_delivery_blocked_;
    result.physical_inflight_cancel_pending =
        adapter.physical_inflight_cancel_pending_;
    return result;
}

} // namespace boot_v2
#endif

namespace {

std::size_t g_allocation_count = 0;
std::size_t g_deallocation_count = 0;
std::size_t g_check_count = 0;
std::size_t g_failure_count = 0;

void check_impl(
    const bool condition,
    const char *const expression,
    const char *const file,
    const int line)
{
    ++g_check_count;
    if (!condition) {
        ++g_failure_count;
        std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
    }
}

#define CHECK(...) check_impl((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

using boot_v2::AdapterCriticalLedger;
using boot_v2::AdapterCriticalReason;
using boot_v2::AdapterDispatch;
using boot_v2::AdapterDispatchKind;
using boot_v2::AdapterStepAction;
using boot_v2::AdapterStepResult;
using boot_v2::DispatchAckResult;
using boot_v2::LivenessAttemptToken;
using boot_v2::NormalCompletion;
using boot_v2::NormalCompletionKind;
using boot_v2::NormalIntent;
using boot_v2::NormalIntentKind;
using boot_v2::NormalSubmitResult;
using boot_v2::runtime_owner_normal_intent_is_canonical;
using boot_v2::OwnerRequestResult;
using boot_v2::RuntimeOwnerAdapterView;
using boot_v2::RuntimeOwnerAdapterCore;
using boot_v2::RuntimeOwnerNormalCompletionPort;
using boot_v2::RuntimeOwnerAdapterNormalSlotSnapshot;
using boot_v2::RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot;
using boot_v2::RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot;
using boot_v2::RuntimeOwnerAdapterPendingEffectSlotSnapshot;
using boot_v2::RuntimeOwnerAdapterPrivateSnapshot;
using boot_v2::RuntimeOwnerAdapterTrustedSlotSnapshot;
using boot_v2::RuntimeOwnerDisposition;
using boot_v2::RuntimeOwnerEffect;
using boot_v2::RuntimeOwnerEffectKind;
using boot_v2::RuntimeOwnerFaultCode;
using boot_v2::RuntimeOwnerInput;
using boot_v2::RuntimeOwnerNormalPort;
using boot_v2::RuntimeOwnerPhase;
using boot_v2::RuntimeOwnerShutdownPort;
using boot_v2::RuntimeOwnerTrustedReceiptPort;
using boot_v2::RuntimeOwnerTransition;
using boot_v2::RuntimeOwnerView;
using boot_v2::TrustedReceipt;
using boot_v2::TrustedReceiptKind;
using boot_v2::TrustedIngressResult;
using boot_v2::UrgentRequestResult;
using boot_v2::RuntimeOwnerAdapterCoreTestPeer;
using LastNormalCompletionSignature =
    RuntimeOwnerAdapterCoreTestPeer::LastNormalCompletionSignature;
using LastTrustedReceiptSignature =
    RuntimeOwnerAdapterCoreTestPeer::LastTrustedReceiptSignature;
using TrustedEnqueueResult =
    RuntimeOwnerAdapterCoreTestPeer::TrustedEnqueueResult;
using PendingEffectSlot =
    RuntimeOwnerAdapterCoreTestPeer::PendingEffectSlot;
using TrustedIngressEnvelope =
    RuntimeOwnerAdapterCoreTestPeer::TrustedIngressEnvelope;
using TrustedIngressPayloadKind =
    RuntimeOwnerAdapterCoreTestPeer::TrustedIngressPayloadKind;

constexpr NormalIntent make_telemetry_intent(
    const std::uint32_t subject_id,
    const std::uint32_t snapshot_revision) noexcept
{
    return {
        NormalIntentKind::PublishTelemetry,
        0,
        0,
        subject_id,
        snapshot_revision,
    };
}

constexpr NormalIntent make_kind_only_intent(
    const NormalIntentKind kind) noexcept
{
    return {kind, 0, 0, 0, 0};
}

constexpr TrustedReceipt make_canonical_trusted_receipt(
    const TrustedReceiptKind kind,
    const RuntimeOwnerEffectKind operation_effect =
        RuntimeOwnerEffectKind::StartAtProbe,
    const std::uint32_t diagnostic_code = 0) noexcept
{
    switch (kind) {
    case TrustedReceiptKind::TransportEstablished:
        return {
            kind,
            RuntimeOwnerEffectKind::StartTransportAttempt,
            0,
            0,
            11,
            12,
            0,
            0,
            0,
        };
    case TrustedReceiptKind::TransportAttemptFailed:
        return {
            kind,
            RuntimeOwnerEffectKind::StartTransportAttempt,
            0,
            0,
            0,
            12,
            0,
            0,
            diagnostic_code,
        };
    case TrustedReceiptKind::ConfigCommitted:
        return {
            kind,
            RuntimeOwnerEffectKind::None,
            0,
            0,
            11,
            12,
            13,
            0,
            0,
        };
    case TrustedReceiptKind::OperationCompleted:
        return {kind, operation_effect, 0, 14, 11, 12, 0, 15, 0};
    case TrustedReceiptKind::OperationFailed:
    case TrustedReceiptKind::DeadlineExpired:
        return {
            kind,
            operation_effect,
            0,
            14,
            11,
            12,
            0,
            15,
            diagnostic_code,
        };
    case TrustedReceiptKind::SnapshotSucceeded:
        return {
            kind,
            RuntimeOwnerEffectKind::FreezeBootSnapshot,
            0,
            14,
            11,
            12,
            0,
            15,
            0,
        };
    case TrustedReceiptKind::SnapshotFailed:
        return {
            kind,
            RuntimeOwnerEffectKind::FreezeBootSnapshot,
            0,
            14,
            11,
            12,
            0,
            15,
            diagnostic_code,
        };
    case TrustedReceiptKind::TransportDisconnected:
        return {
            kind,
            RuntimeOwnerEffectKind::None,
            0,
            0,
            11,
            12,
            0,
            0,
            diagnostic_code,
        };
    case TrustedReceiptKind::Invalid:
    default:
        return {};
    }
}

constexpr bool normal_intents_equal(
    const NormalIntent left,
    const NormalIntent right) noexcept
{
    return left.kind == right.kind &&
           left.flags == right.flags &&
           left.reserved == right.reserved &&
           left.subject_id == right.subject_id &&
           left.snapshot_revision == right.snapshot_revision;
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

constexpr bool critical_ledgers_equal(
    const AdapterCriticalLedger left,
    const AdapterCriticalLedger right) noexcept
{
    return left.first_reason == right.first_reason &&
           left.last_reason == right.last_reason &&
           left.reserved == right.reserved &&
           left.reason_mask == right.reason_mask &&
           left.first_ingress_sequence == right.first_ingress_sequence &&
           left.last_ingress_sequence == right.last_ingress_sequence &&
           left.first_diagnostic_code == right.first_diagnostic_code &&
           left.last_diagnostic_code == right.last_diagnostic_code &&
           left.occurrence_count == right.occurrence_count;
}

constexpr bool runtime_owner_views_equal(
    const RuntimeOwnerView left,
    const RuntimeOwnerView right) noexcept
{
    return left.phase == right.phase &&
           left.mqtt_session_id == right.mqtt_session_id &&
           left.mqtt_generation == right.mqtt_generation &&
           left.mqtt_generation_counter == right.mqtt_generation_counter &&
           left.config_apply_epoch_counter ==
               right.config_apply_epoch_counter &&
           left.last_config_commit_sequence ==
               right.last_config_commit_sequence &&
           left.last_correlation_id == right.last_correlation_id &&
           left.active_attempt == right.active_attempt &&
           left.boot_orchestration_ended ==
               right.boot_orchestration_ended &&
           left.last_fault == right.last_fault;
}

constexpr bool normal_slot_snapshots_equal(
    const RuntimeOwnerAdapterNormalSlotSnapshot left,
    const RuntimeOwnerAdapterNormalSlotSnapshot right) noexcept
{
    return normal_intents_equal(left.intent, right.intent) &&
           left.enqueue_sequence == right.enqueue_sequence;
}

constexpr bool trusted_slot_snapshots_equal(
    const RuntimeOwnerAdapterTrustedSlotSnapshot left,
    const RuntimeOwnerAdapterTrustedSlotSnapshot right) noexcept
{
    return left.payload_kind == right.payload_kind &&
           left.reserved == right.reserved &&
           left.ingress_sequence == right.ingress_sequence &&
           trusted_receipts_equal(left.receipt, right.receipt) &&
           normal_completions_equal(
               left.normal_completion, right.normal_completion);
}

constexpr bool runtime_owner_effects_equal(
    const RuntimeOwnerEffect left,
    const RuntimeOwnerEffect right) noexcept
{
    return left.kind == right.kind &&
           left.correlation_id == right.correlation_id &&
           left.attempt == right.attempt &&
           left.fault_code == right.fault_code;
}

constexpr bool adapter_dispatches_equal(
    const AdapterDispatch left,
    const AdapterDispatch right) noexcept
{
    return left.kind == right.kind && left.reserved == right.reserved &&
           left.dispatch_sequence == right.dispatch_sequence &&
           left.enqueue_sequence == right.enqueue_sequence &&
           runtime_owner_effects_equal(left.effect, right.effect) &&
           normal_intents_equal(left.normal_intent, right.normal_intent);
}

constexpr bool pending_effect_slot_snapshots_equal(
    const RuntimeOwnerAdapterPendingEffectSlotSnapshot left,
    const RuntimeOwnerAdapterPendingEffectSlotSnapshot right) noexcept
{
    return left.preassigned_dispatch_sequence ==
               right.preassigned_dispatch_sequence &&
           runtime_owner_effects_equal(left.effect, right.effect);
}

constexpr bool last_trusted_receipt_signatures_equal(
    const RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot left,
    const RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot right) noexcept
{
    return left.ingress_sequence == right.ingress_sequence &&
           trusted_receipts_equal(left.receipt, right.receipt);
}

constexpr bool last_normal_completion_signatures_equal(
    const RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot left,
    const RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot right) noexcept
{
    return left.ingress_sequence == right.ingress_sequence &&
           normal_completions_equal(left.completion, right.completion);
}

bool private_snapshots_equal(
    const RuntimeOwnerAdapterPrivateSnapshot &left,
    const RuntimeOwnerAdapterPrivateSnapshot &right) noexcept
{
    if (!runtime_owner_views_equal(left.core, right.core) ||
        !adapter_dispatches_equal(
            left.current_dispatch, right.current_dispatch) ||
        !adapter_dispatches_equal(
            left.physical_inflight, right.physical_inflight) ||
        !critical_ledgers_equal(left.critical, right.critical) ||
        !last_trusted_receipt_signatures_equal(
            left.last_trusted_receipt_signature,
            right.last_trusted_receipt_signature) ||
        !last_normal_completion_signatures_equal(
            left.last_normal_completion_signature,
            right.last_normal_completion_signature) ||
        left.last_normal_enqueue_sequence !=
            right.last_normal_enqueue_sequence ||
        left.last_trusted_ingress_sequence !=
            right.last_trusted_ingress_sequence ||
        left.last_dispatch_sequence != right.last_dispatch_sequence ||
        left.last_ack_dispatch_sequence !=
            right.last_ack_dispatch_sequence ||
        left.last_trusted_diagnostic_ingress_sequence !=
            right.last_trusted_diagnostic_ingress_sequence ||
        left.last_trusted_diagnostic_code !=
            right.last_trusted_diagnostic_code ||
        left.normal_coalesced_count != right.normal_coalesced_count ||
        left.normal_rejected_full_count != right.normal_rejected_full_count ||
        left.normal_cancelled_count != right.normal_cancelled_count ||
        left.dispatch_rejected_ack_count !=
            right.dispatch_rejected_ack_count ||
        left.trusted_rejected_full_count !=
            right.trusted_rejected_full_count ||
        left.trusted_protocol_violation_count !=
            right.trusted_protocol_violation_count ||
        left.trusted_stale_count != right.trusted_stale_count ||
        left.trusted_duplicate_count != right.trusted_duplicate_count ||
        left.trusted_cancelled_count != right.trusted_cancelled_count ||
        left.effect_cancelled_count != right.effect_cancelled_count ||
        left.normal_completion_stale_count !=
            right.normal_completion_stale_count ||
        left.normal_head != right.normal_head ||
        left.normal_tail != right.normal_tail ||
        left.normal_count != right.normal_count ||
        left.normal_high_water != right.normal_high_water ||
        left.trusted_head != right.trusted_head ||
        left.trusted_tail != right.trusted_tail ||
        left.trusted_count != right.trusted_count ||
        left.trusted_high_water != right.trusted_high_water ||
        left.pending_effect_head != right.pending_effect_head ||
        left.pending_effect_tail != right.pending_effect_tail ||
        left.pending_effect_count != right.pending_effect_count ||
        left.accepted_liveness_mask != right.accepted_liveness_mask ||
        left.transport_request_pending !=
            right.transport_request_pending ||
        left.shutdown_pending != right.shutdown_pending ||
        left.shutdown_terminal_override_latched !=
            right.shutdown_terminal_override_latched ||
        left.boot_end_released != right.boot_end_released ||
        left.critical_pending != right.critical_pending ||
        left.core_fail_closed_latched != right.core_fail_closed_latched ||
        left.core_adapter_fatal_latched !=
            right.core_adapter_fatal_latched ||
        left.sequence_fatal_latched != right.sequence_fatal_latched ||
        left.dispatch_fatal_latched != right.dispatch_fatal_latched ||
        left.safety_delivery_blocked != right.safety_delivery_blocked ||
        left.physical_inflight_cancel_pending !=
            right.physical_inflight_cancel_pending) {
        return false;
    }
    for (std::size_t index = 0; index < left.normal_slots.size(); ++index) {
        if (!normal_slot_snapshots_equal(
                left.normal_slots[index], right.normal_slots[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.trusted_slots.size(); ++index) {
        if (!trusted_slot_snapshots_equal(
                left.trusted_slots[index], right.trusted_slots[index])) {
            return false;
        }
    }
    for (std::size_t index = 0;
         index < left.pending_effect_slots.size(); ++index) {
        if (!pending_effect_slot_snapshots_equal(
                left.pending_effect_slots[index],
                right.pending_effect_slots[index])) {
            return false;
        }
    }
    return true;
}

template <typename Type>
void implicit_default_sink(Type);

template <typename Type, typename = void>
struct IsImplicitlyDefaultListConstructible : std::false_type {
};

template <typename Type>
struct IsImplicitlyDefaultListConstructible<
    Type,
    std::void_t<decltype(implicit_default_sink<Type>({}))>> : std::true_type {
};

template <typename Type, typename = void>
struct HasCoreInputRejectedEnumMember : std::false_type {
};

template <typename Type>
struct HasCoreInputRejectedEnumMember<
    Type,
    std::void_t<decltype(Type::CoreInputRejected)>> : std::true_type {
};

template <typename Type, typename = void>
struct HasLvalueNormalPort : std::false_type {
};

template <typename Type>
struct HasLvalueNormalPort<
    Type,
    std::void_t<decltype(std::declval<Type &>().normal_port())>>
    : std::integral_constant<
          bool,
          std::is_same<
              decltype(std::declval<Type &>().normal_port()),
              RuntimeOwnerNormalPort>::value &&
              noexcept(std::declval<Type &>().normal_port())> {
};

template <typename Type, typename = void>
struct HasRvalueNormalPort : std::false_type {
};

template <typename Type>
struct HasRvalueNormalPort<
    Type,
    std::void_t<decltype(std::declval<Type &&>().normal_port())>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasConstLvalueNormalPort : std::false_type {
};

template <typename Type>
struct HasConstLvalueNormalPort<
    Type,
    std::void_t<decltype(std::declval<const Type &>().normal_port())>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasLvalueShutdownPort : std::false_type {
};

template <typename Type>
struct HasLvalueShutdownPort<
    Type,
    std::void_t<decltype(std::declval<Type &>().shutdown_port())>>
    : std::integral_constant<
          bool,
          std::is_same<
              decltype(std::declval<Type &>().shutdown_port()),
              RuntimeOwnerShutdownPort>::value &&
              noexcept(std::declval<Type &>().shutdown_port())> {
};

template <typename Type, typename = void>
struct HasRvalueShutdownPort : std::false_type {
};

template <typename Type>
struct HasRvalueShutdownPort<
    Type,
    std::void_t<decltype(std::declval<Type &&>().shutdown_port())>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasConstLvalueShutdownPort : std::false_type {
};

template <typename Type>
struct HasConstLvalueShutdownPort<
    Type,
    std::void_t<decltype(std::declval<const Type &>().shutdown_port())>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasLvalueTrustedReceiptPort : std::false_type {
};

template <typename Type>
struct HasLvalueTrustedReceiptPort<
    Type,
    std::void_t<decltype(std::declval<Type &>().trusted_receipt_port())>>
    : std::integral_constant<
          bool,
          std::is_same<
              decltype(std::declval<Type &>().trusted_receipt_port()),
              RuntimeOwnerTrustedReceiptPort>::value &&
              noexcept(
                  std::declval<Type &>().trusted_receipt_port())> {
};

template <typename Type, typename = void>
struct HasRvalueTrustedReceiptPort : std::false_type {
};

template <typename Type>
struct HasRvalueTrustedReceiptPort<
    Type,
    std::void_t<decltype(
        std::declval<Type &&>().trusted_receipt_port())>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasConstLvalueTrustedReceiptPort : std::false_type {
};

template <typename Type>
struct HasConstLvalueTrustedReceiptPort<
    Type,
    std::void_t<decltype(
        std::declval<const Type &>().trusted_receipt_port())>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasLvalueNormalCompletionPort : std::false_type {
};

template <typename Type>
struct HasLvalueNormalCompletionPort<
    Type,
    std::void_t<decltype(
        std::declval<Type &>().normal_completion_port())>>
    : std::integral_constant<
          bool,
          std::is_same<
              decltype(std::declval<Type &>().normal_completion_port()),
              RuntimeOwnerNormalCompletionPort>::value &&
              noexcept(
                  std::declval<Type &>().normal_completion_port())> {
};

template <typename Type, typename = void>
struct HasRvalueNormalCompletionPort : std::false_type {
};

template <typename Type>
struct HasRvalueNormalCompletionPort<
    Type,
    std::void_t<decltype(
        std::declval<Type &&>().normal_completion_port())>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasConstLvalueNormalCompletionPort : std::false_type {
};

template <typename Type>
struct HasConstLvalueNormalCompletionPort<
    Type,
    std::void_t<decltype(
        std::declval<const Type &>().normal_completion_port())>>
    : std::true_type {
};

#define DEFINE_HAS_PUBLIC_NOARG_METHOD(TraitName, MethodName)                \
    template <typename Type, typename = void>                               \
    struct TraitName : std::false_type {                                    \
    };                                                                      \
                                                                            \
    template <typename Type>                                                \
    struct TraitName<                                                       \
        Type,                                                               \
        std::void_t<decltype(std::declval<Type &>().MethodName())>>          \
        : std::true_type {                                                  \
    }

DEFINE_HAS_PUBLIC_NOARG_METHOD(HasRequestTransportAttempt, request_transport_attempt);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasStep, step);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasPeekDispatch, peek_dispatch);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasView, view);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasRequest, request);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasTrustedPort, trusted_port);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasCoreGetter, core);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasCorePtrGetter, core_ptr);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasStateGetter, state);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasStatePtrGetter, state_ptr);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasBoundaryGetter, boundary);
DEFINE_HAS_PUBLIC_NOARG_METHOD(HasBoundaryPtrGetter, boundary_ptr);

#undef DEFINE_HAS_PUBLIC_NOARG_METHOD

template <typename Type, typename = void>
struct HasAcknowledgeDispatch : std::false_type {
};

template <typename Type>
struct HasAcknowledgeDispatch<
    Type,
    std::void_t<decltype(std::declval<Type &>().acknowledge_dispatch(
        std::declval<std::uint32_t>()))>> : std::true_type {
};

template <typename Type, typename = void>
struct HasNormalIntentSubmit : std::false_type {
};

template <typename Type>
struct HasNormalIntentSubmit<
    Type,
    std::void_t<decltype(std::declval<Type &>().submit(
        std::declval<NormalIntent>()))>> : std::true_type {
};

template <typename Type, typename = void>
struct HasTrustedReceiptSubmit : std::false_type {
};

template <typename Type>
struct HasTrustedReceiptSubmit<
    Type,
    std::void_t<decltype(std::declval<Type &>().submit(
        std::declval<TrustedReceipt>()))>> : std::true_type {
};

template <typename Type, typename = void>
struct HasNormalCompletionSubmit : std::false_type {
};

template <typename Type>
struct HasNormalCompletionSubmit<
    Type,
    std::void_t<decltype(std::declval<Type &>().submit(
        std::declval<NormalCompletion>()))>> : std::true_type {
};

template <typename Type, typename = void>
struct HasRawCoreSubmit : std::false_type {
};

template <typename Type>
struct HasRawCoreSubmit<
    Type,
    std::void_t<decltype(std::declval<Type &>().submit(
        std::declval<RuntimeOwnerInput>()))>> : std::true_type {
};

template <typename Type, typename = void>
struct HasTrustedEnqueue : std::false_type {
};

template <typename Type>
struct HasTrustedEnqueue<
    Type,
    std::void_t<decltype(std::declval<Type &>().enqueue_trusted(
        std::declval<TrustedReceipt>()))>> : std::true_type {
};

template <typename Type, typename = void>
struct HasTrustedReceiptForTestingEnqueue : std::false_type {
};

template <typename Type>
struct HasTrustedReceiptForTestingEnqueue<
    Type,
    std::void_t<decltype(
        std::declval<Type &>().enqueue_trusted_receipt_for_testing(
            std::declval<TrustedReceipt>()))>> : std::true_type {
};

using NormalFactorySignature =
    RuntimeOwnerNormalPort (RuntimeOwnerAdapterCore::*)() & noexcept;
using ShutdownFactorySignature =
    RuntimeOwnerShutdownPort (RuntimeOwnerAdapterCore::*)() & noexcept;
using TrustedReceiptFactorySignature =
    RuntimeOwnerTrustedReceiptPort (RuntimeOwnerAdapterCore::*)() & noexcept;
using NormalCompletionFactorySignature =
    RuntimeOwnerNormalCompletionPort (RuntimeOwnerAdapterCore::*)() & noexcept;
using OwnerRequestSignature =
    OwnerRequestResult (RuntimeOwnerAdapterCore::*)() noexcept;
using StepSignature = AdapterStepResult (RuntimeOwnerAdapterCore::*)() noexcept;
using PeekDispatchSignature =
    AdapterDispatch (RuntimeOwnerAdapterCore::*)() const noexcept;
using AcknowledgeDispatchSignature =
    DispatchAckResult (RuntimeOwnerAdapterCore::*)(std::uint32_t) noexcept;
using ViewSignature =
    RuntimeOwnerAdapterView (RuntimeOwnerAdapterCore::*)() const noexcept;
using NormalSubmitSignature =
    NormalSubmitResult (RuntimeOwnerNormalPort::*)(NormalIntent) noexcept;
using ShutdownRequestSignature =
    UrgentRequestResult (RuntimeOwnerShutdownPort::*)() noexcept;
using TrustedReceiptSubmitSignature =
    TrustedIngressResult (RuntimeOwnerTrustedReceiptPort::*)(
        TrustedReceipt) noexcept;
using NormalCompletionSubmitSignature =
    TrustedIngressResult (RuntimeOwnerNormalCompletionPort::*)(
        NormalCompletion) noexcept;

static_assert(std::is_default_constructible<RuntimeOwnerAdapterCore>::value);
static_assert(std::is_nothrow_default_constructible<RuntimeOwnerAdapterCore>::value);
static_assert(std::is_nothrow_destructible<RuntimeOwnerAdapterCore>::value);
static_assert(!IsImplicitlyDefaultListConstructible<RuntimeOwnerAdapterCore>::value);
static_assert(!std::is_copy_constructible<RuntimeOwnerAdapterCore>::value);
static_assert(!std::is_copy_assignable<RuntimeOwnerAdapterCore>::value);
static_assert(!std::is_move_constructible<RuntimeOwnerAdapterCore>::value);
static_assert(!std::is_move_assignable<RuntimeOwnerAdapterCore>::value);

static_assert(!std::is_default_constructible<RuntimeOwnerNormalPort>::value);
static_assert(!std::is_copy_constructible<RuntimeOwnerNormalPort>::value);
static_assert(!std::is_copy_assignable<RuntimeOwnerNormalPort>::value);
static_assert(!std::is_move_constructible<RuntimeOwnerNormalPort>::value);
static_assert(!std::is_move_assignable<RuntimeOwnerNormalPort>::value);
static_assert(std::is_nothrow_destructible<RuntimeOwnerNormalPort>::value);
static_assert(!std::is_constructible<
              RuntimeOwnerNormalPort,
              RuntimeOwnerAdapterCore *>::value);
static_assert(sizeof(RuntimeOwnerNormalPort) == sizeof(void *));

static_assert(!std::is_default_constructible<RuntimeOwnerShutdownPort>::value);
static_assert(!std::is_copy_constructible<RuntimeOwnerShutdownPort>::value);
static_assert(!std::is_copy_assignable<RuntimeOwnerShutdownPort>::value);
static_assert(!std::is_move_constructible<RuntimeOwnerShutdownPort>::value);
static_assert(!std::is_move_assignable<RuntimeOwnerShutdownPort>::value);
static_assert(std::is_nothrow_destructible<RuntimeOwnerShutdownPort>::value);
static_assert(!std::is_constructible<
              RuntimeOwnerShutdownPort,
              RuntimeOwnerAdapterCore *>::value);
static_assert(sizeof(RuntimeOwnerShutdownPort) == sizeof(void *));

static_assert(!std::is_default_constructible<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!std::is_copy_constructible<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!std::is_copy_assignable<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!std::is_move_constructible<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!std::is_move_assignable<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(std::is_nothrow_destructible<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!std::is_constructible<
              RuntimeOwnerTrustedReceiptPort,
              RuntimeOwnerAdapterCore *>::value);
static_assert(sizeof(RuntimeOwnerTrustedReceiptPort) == sizeof(void *));

static_assert(!std::is_default_constructible<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!std::is_copy_constructible<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!std::is_copy_assignable<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!std::is_move_constructible<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!std::is_move_assignable<RuntimeOwnerNormalCompletionPort>::value);
static_assert(std::is_nothrow_destructible<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!std::is_constructible<
              RuntimeOwnerNormalCompletionPort,
              RuntimeOwnerAdapterCore *>::value);
static_assert(sizeof(RuntimeOwnerNormalCompletionPort) == sizeof(void *));

static_assert(HasLvalueNormalPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasRvalueNormalPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasConstLvalueNormalPort<RuntimeOwnerAdapterCore>::value);
static_assert(HasLvalueShutdownPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasRvalueShutdownPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasConstLvalueShutdownPort<RuntimeOwnerAdapterCore>::value);
static_assert(HasLvalueTrustedReceiptPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasRvalueTrustedReceiptPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasConstLvalueTrustedReceiptPort<RuntimeOwnerAdapterCore>::value);
static_assert(HasLvalueNormalCompletionPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasRvalueNormalCompletionPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasConstLvalueNormalCompletionPort<RuntimeOwnerAdapterCore>::value);

static_assert(std::is_same<
              decltype(static_cast<NormalFactorySignature>(
                  &RuntimeOwnerAdapterCore::normal_port)),
              NormalFactorySignature>::value);
static_assert(std::is_same<
              decltype(static_cast<ShutdownFactorySignature>(
                  &RuntimeOwnerAdapterCore::shutdown_port)),
              ShutdownFactorySignature>::value);
static_assert(std::is_same<
              decltype(static_cast<TrustedReceiptFactorySignature>(
                  &RuntimeOwnerAdapterCore::trusted_receipt_port)),
              TrustedReceiptFactorySignature>::value);
static_assert(std::is_same<
              decltype(static_cast<NormalCompletionFactorySignature>(
                  &RuntimeOwnerAdapterCore::normal_completion_port)),
              NormalCompletionFactorySignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerAdapterCore::request_transport_attempt),
              OwnerRequestSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerAdapterCore::step),
              StepSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerAdapterCore::peek_dispatch),
              PeekDispatchSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerAdapterCore::acknowledge_dispatch),
              AcknowledgeDispatchSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerAdapterCore::view),
              ViewSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerNormalPort::submit),
              NormalSubmitSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerShutdownPort::request),
              ShutdownRequestSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerTrustedReceiptPort::submit),
              TrustedReceiptSubmitSignature>::value);
static_assert(std::is_same<
              decltype(&RuntimeOwnerNormalCompletionPort::submit),
              NormalCompletionSubmitSignature>::value);

static_assert(!HasRawCoreSubmit<RuntimeOwnerAdapterCore>::value);
static_assert(!HasNormalIntentSubmit<RuntimeOwnerAdapterCore>::value);
static_assert(!HasRequest<RuntimeOwnerAdapterCore>::value);
static_assert(!HasTrustedEnqueue<RuntimeOwnerAdapterCore>::value);
static_assert(!HasTrustedPort<RuntimeOwnerAdapterCore>::value);
static_assert(!HasCoreGetter<RuntimeOwnerAdapterCore>::value);
static_assert(!HasCorePtrGetter<RuntimeOwnerAdapterCore>::value);
static_assert(!HasStateGetter<RuntimeOwnerAdapterCore>::value);
static_assert(!HasStatePtrGetter<RuntimeOwnerAdapterCore>::value);
static_assert(!HasBoundaryGetter<RuntimeOwnerAdapterCore>::value);
static_assert(!HasBoundaryPtrGetter<RuntimeOwnerAdapterCore>::value);

static_assert(HasNormalIntentSubmit<RuntimeOwnerNormalPort>::value);
static_assert(!HasRequest<RuntimeOwnerNormalPort>::value);
static_assert(!HasRequestTransportAttempt<RuntimeOwnerNormalPort>::value);
static_assert(!HasStep<RuntimeOwnerNormalPort>::value);
static_assert(!HasPeekDispatch<RuntimeOwnerNormalPort>::value);
static_assert(!HasAcknowledgeDispatch<RuntimeOwnerNormalPort>::value);
static_assert(!HasView<RuntimeOwnerNormalPort>::value);
static_assert(!HasTrustedEnqueue<RuntimeOwnerNormalPort>::value);
static_assert(!HasTrustedPort<RuntimeOwnerNormalPort>::value);
static_assert(!HasRawCoreSubmit<RuntimeOwnerNormalPort>::value);

static_assert(HasRequest<RuntimeOwnerShutdownPort>::value);
static_assert(!HasNormalIntentSubmit<RuntimeOwnerShutdownPort>::value);
static_assert(!HasRawCoreSubmit<RuntimeOwnerShutdownPort>::value);
static_assert(!HasRequestTransportAttempt<RuntimeOwnerShutdownPort>::value);
static_assert(!HasStep<RuntimeOwnerShutdownPort>::value);
static_assert(!HasPeekDispatch<RuntimeOwnerShutdownPort>::value);
static_assert(!HasAcknowledgeDispatch<RuntimeOwnerShutdownPort>::value);
static_assert(!HasView<RuntimeOwnerShutdownPort>::value);
static_assert(!HasTrustedEnqueue<RuntimeOwnerShutdownPort>::value);
static_assert(!HasTrustedPort<RuntimeOwnerShutdownPort>::value);

static_assert(HasTrustedReceiptSubmit<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasNormalCompletionSubmit<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasNormalIntentSubmit<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasRawCoreSubmit<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasRequest<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasRequestTransportAttempt<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasStep<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasPeekDispatch<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasAcknowledgeDispatch<RuntimeOwnerTrustedReceiptPort>::value);
static_assert(!HasView<RuntimeOwnerTrustedReceiptPort>::value);

static_assert(HasNormalCompletionSubmit<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasTrustedReceiptSubmit<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasNormalIntentSubmit<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasRawCoreSubmit<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasRequest<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasRequestTransportAttempt<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasStep<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasPeekDispatch<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasAcknowledgeDispatch<RuntimeOwnerNormalCompletionPort>::value);
static_assert(!HasView<RuntimeOwnerNormalCompletionPort>::value);

template <typename Enum>
void check_enum_type_and_unknown()
{
    CHECK((std::is_same<typename std::underlying_type<Enum>::type,
                        std::uint8_t>::value));
    const auto unknown = static_cast<Enum>(255);
    CHECK(static_cast<std::uint8_t>(unknown) == 255);
}

template <typename Enum>
void check_enum_value(const Enum value, const std::uint8_t expected)
{
    CHECK(static_cast<std::uint8_t>(value) == expected);
}

template <typename Type, std::size_t ExpectedSize, std::size_t ExpectedAlign>
constexpr bool has_fixed_dto_contract =
    sizeof(Type) == ExpectedSize &&
    alignof(Type) == ExpectedAlign &&
    std::is_standard_layout<Type>::value &&
    std::is_trivially_copyable<Type>::value;

template <typename... Fields>
constexpr bool has_only_nonowning_value_fields =
    ((!std::is_pointer<Fields>::value &&
      !std::is_reference<Fields>::value &&
      std::is_trivially_copyable<Fields>::value) && ...);

constexpr bool kAdapterStepResultValueFields =
    has_only_nonowning_value_fields<
        decltype(AdapterStepResult::action),
        decltype(AdapterStepResult::core_disposition),
        decltype(AdapterStepResult::phase_before),
        decltype(AdapterStepResult::phase_after),
        decltype(AdapterStepResult::consumed_ingress_sequence),
        decltype(AdapterStepResult::consumed_enqueue_sequence),
        decltype(AdapterStepResult::prepared_dispatch_sequence)>;

constexpr bool kNormalIntentValueFields = has_only_nonowning_value_fields<
    decltype(NormalIntent::kind),
    decltype(NormalIntent::flags),
    decltype(NormalIntent::reserved),
    decltype(NormalIntent::subject_id),
    decltype(NormalIntent::snapshot_revision)>;

constexpr bool kTrustedReceiptValueFields = has_only_nonowning_value_fields<
    decltype(TrustedReceipt::kind),
    decltype(TrustedReceipt::effect_kind),
    decltype(TrustedReceipt::reserved),
    decltype(TrustedReceipt::correlation_id),
    decltype(TrustedReceipt::mqtt_session_id),
    decltype(TrustedReceipt::mqtt_generation),
    decltype(TrustedReceipt::config_commit_sequence),
    decltype(TrustedReceipt::config_apply_epoch),
    decltype(TrustedReceipt::diagnostic_code)>;

constexpr bool kNormalCompletionValueFields = has_only_nonowning_value_fields<
    decltype(NormalCompletion::kind),
    decltype(NormalCompletion::reserved),
    decltype(NormalCompletion::dispatch_sequence),
    decltype(NormalCompletion::enqueue_sequence),
    decltype(NormalCompletion::diagnostic_code)>;

constexpr bool kTrustedIngressEnvelopeValueFields =
    has_only_nonowning_value_fields<
        decltype(TrustedIngressEnvelope::kind),
        decltype(TrustedIngressEnvelope::reserved),
        decltype(TrustedIngressEnvelope::ingress_sequence),
        decltype(TrustedIngressEnvelope::receipt),
        decltype(TrustedIngressEnvelope::normal_completion)>;

constexpr bool kAdapterDispatchValueFields = has_only_nonowning_value_fields<
    decltype(AdapterDispatch::kind),
    decltype(AdapterDispatch::reserved),
    decltype(AdapterDispatch::dispatch_sequence),
    decltype(AdapterDispatch::enqueue_sequence),
    decltype(AdapterDispatch::effect),
    decltype(AdapterDispatch::normal_intent)>;

constexpr bool kAdapterCriticalLedgerValueFields =
    has_only_nonowning_value_fields<
        decltype(AdapterCriticalLedger::first_reason),
        decltype(AdapterCriticalLedger::last_reason),
        decltype(AdapterCriticalLedger::reserved),
        decltype(AdapterCriticalLedger::reason_mask),
        decltype(AdapterCriticalLedger::first_ingress_sequence),
        decltype(AdapterCriticalLedger::last_ingress_sequence),
        decltype(AdapterCriticalLedger::first_diagnostic_code),
        decltype(AdapterCriticalLedger::last_diagnostic_code),
        decltype(AdapterCriticalLedger::occurrence_count)>;

constexpr bool kRuntimeOwnerAdapterViewValueFields =
    has_only_nonowning_value_fields<
        decltype(RuntimeOwnerAdapterView::core),
        decltype(RuntimeOwnerAdapterView::current_dispatch),
        decltype(RuntimeOwnerAdapterView::physical_inflight),
        decltype(RuntimeOwnerAdapterView::critical),
        decltype(RuntimeOwnerAdapterView::last_normal_enqueue_sequence),
        decltype(RuntimeOwnerAdapterView::last_trusted_ingress_sequence),
        decltype(RuntimeOwnerAdapterView::last_dispatch_sequence),
        decltype(RuntimeOwnerAdapterView::last_ack_dispatch_sequence),
        decltype(RuntimeOwnerAdapterView::last_trusted_diagnostic_ingress_sequence),
        decltype(RuntimeOwnerAdapterView::last_trusted_diagnostic_code),
        decltype(RuntimeOwnerAdapterView::normal_coalesced_count),
        decltype(RuntimeOwnerAdapterView::normal_rejected_full_count),
        decltype(RuntimeOwnerAdapterView::normal_cancelled_count),
        decltype(RuntimeOwnerAdapterView::trusted_rejected_full_count),
        decltype(RuntimeOwnerAdapterView::trusted_protocol_violation_count),
        decltype(RuntimeOwnerAdapterView::trusted_stale_count),
        decltype(RuntimeOwnerAdapterView::trusted_duplicate_count),
        decltype(RuntimeOwnerAdapterView::trusted_cancelled_count),
        decltype(RuntimeOwnerAdapterView::effect_cancelled_count),
        decltype(RuntimeOwnerAdapterView::dispatch_rejected_ack_count),
        decltype(RuntimeOwnerAdapterView::normal_completion_stale_count),
        decltype(RuntimeOwnerAdapterView::normal_depth),
        decltype(RuntimeOwnerAdapterView::normal_high_water),
        decltype(RuntimeOwnerAdapterView::trusted_depth),
        decltype(RuntimeOwnerAdapterView::trusted_high_water),
        decltype(RuntimeOwnerAdapterView::pending_effect_count),
        decltype(RuntimeOwnerAdapterView::transport_request_pending),
        decltype(RuntimeOwnerAdapterView::shutdown_pending),
        decltype(RuntimeOwnerAdapterView::shutdown_terminal_override_latched),
        decltype(RuntimeOwnerAdapterView::critical_pending),
        decltype(RuntimeOwnerAdapterView::boot_end_released),
        decltype(RuntimeOwnerAdapterView::core_fail_closed_latched),
        decltype(RuntimeOwnerAdapterView::core_adapter_fatal_latched),
        decltype(RuntimeOwnerAdapterView::sequence_fatal_latched),
        decltype(RuntimeOwnerAdapterView::dispatch_fatal_latched),
        decltype(RuntimeOwnerAdapterView::safety_delivery_blocked),
        decltype(RuntimeOwnerAdapterView::physical_inflight_cancel_pending)>;

constexpr bool kLastTrustedReceiptSignatureValueFields =
    has_only_nonowning_value_fields<
        decltype(LastTrustedReceiptSignature::ingress_sequence),
        decltype(LastTrustedReceiptSignature::receipt)>;

constexpr bool kLastNormalCompletionSignatureValueFields =
    has_only_nonowning_value_fields<
        decltype(LastNormalCompletionSignature::ingress_sequence),
        decltype(LastNormalCompletionSignature::completion)>;

static_assert(has_fixed_dto_contract<AdapterStepResult, 16, 4>);
static_assert(has_fixed_dto_contract<PendingEffectSlot, 28, 4>);
static_assert(has_fixed_dto_contract<
              RuntimeOwnerAdapterPendingEffectSlotSnapshot,
              28,
              4>);
static_assert(has_fixed_dto_contract<
              RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot,
              32,
              4>);
static_assert(has_fixed_dto_contract<NormalIntent, 12, 4>);
static_assert(has_fixed_dto_contract<TrustedReceipt, 28, 4>);
static_assert(has_fixed_dto_contract<NormalCompletion, 16, 4>);
static_assert(has_fixed_dto_contract<TrustedIngressEnvelope, 52, 4>);
static_assert(has_fixed_dto_contract<AdapterDispatch, 48, 4>);
static_assert(has_fixed_dto_contract<AdapterCriticalLedger, 28, 4>);
static_assert(has_fixed_dto_contract<RuntimeOwnerAdapterView, 252, 4>);
static_assert(has_fixed_dto_contract<LastTrustedReceiptSignature, 32, 4>);
static_assert(has_fixed_dto_contract<LastNormalCompletionSignature, 20, 4>);

static_assert(kAdapterStepResultValueFields);
static_assert(kNormalIntentValueFields);
static_assert(kTrustedReceiptValueFields);
static_assert(kNormalCompletionValueFields);
static_assert(kTrustedIngressEnvelopeValueFields);
static_assert(kAdapterDispatchValueFields);
static_assert(kAdapterCriticalLedgerValueFields);
static_assert(kRuntimeOwnerAdapterViewValueFields);
static_assert(kLastTrustedReceiptSignatureValueFields);
static_assert(kLastNormalCompletionSignatureValueFields);

constexpr bool has_safe_default(const boot_v2::LivenessAttemptToken value)
{
    return value.mqtt_session_id == 0 &&
           value.mqtt_generation == 0 &&
           value.config_apply_epoch == 0;
}

constexpr bool has_safe_default(const RuntimeOwnerEffect value)
{
    return value.kind == RuntimeOwnerEffectKind::None &&
           value.correlation_id == 0 &&
           has_safe_default(value.attempt) &&
           value.fault_code == RuntimeOwnerFaultCode::None;
}

constexpr bool has_safe_default(const RuntimeOwnerView value)
{
    return value.phase == RuntimeOwnerPhase::ColdStart &&
           value.mqtt_session_id == 0 &&
           value.mqtt_generation == 0 &&
           value.mqtt_generation_counter == 0 &&
           value.config_apply_epoch_counter == 0 &&
           value.last_config_commit_sequence == 0 &&
           value.last_correlation_id == 0 &&
           has_safe_default(value.active_attempt) &&
           !value.boot_orchestration_ended &&
           value.last_fault == RuntimeOwnerFaultCode::None;
}

constexpr bool has_safe_default(const AdapterStepResult value)
{
    return value.action == AdapterStepAction::Invalid &&
           value.core_disposition == RuntimeOwnerDisposition::Rejected &&
           value.phase_before == RuntimeOwnerPhase::ColdStart &&
           value.phase_after == RuntimeOwnerPhase::ColdStart &&
           value.consumed_ingress_sequence == 0 &&
           value.consumed_enqueue_sequence == 0 &&
           value.prepared_dispatch_sequence == 0;
}

constexpr bool has_safe_default(const NormalIntent value)
{
    return value.kind == NormalIntentKind::Invalid &&
           value.flags == 0 &&
           value.reserved == 0 &&
           value.subject_id == 0 &&
           value.snapshot_revision == 0;
}

constexpr bool has_safe_default(const TrustedReceipt value)
{
    return value.kind == TrustedReceiptKind::Invalid &&
           value.effect_kind == RuntimeOwnerEffectKind::None &&
           value.reserved == 0 &&
           value.correlation_id == 0 &&
           value.mqtt_session_id == 0 &&
           value.mqtt_generation == 0 &&
           value.config_commit_sequence == 0 &&
           value.config_apply_epoch == 0 &&
           value.diagnostic_code == 0;
}

constexpr bool has_safe_default(const NormalCompletion value)
{
    return value.kind == NormalCompletionKind::Invalid &&
           value.reserved[0] == 0 &&
           value.reserved[1] == 0 &&
           value.reserved[2] == 0 &&
           value.dispatch_sequence == 0 &&
           value.enqueue_sequence == 0 &&
           value.diagnostic_code == 0;
}

constexpr bool has_safe_default(const TrustedIngressEnvelope value)
{
    return value.kind == TrustedIngressPayloadKind::None &&
           value.reserved[0] == 0 &&
           value.reserved[1] == 0 &&
           value.reserved[2] == 0 &&
           value.ingress_sequence == 0 &&
           has_safe_default(value.receipt) &&
           has_safe_default(value.normal_completion);
}

constexpr bool has_safe_default(const AdapterDispatch value)
{
    return value.kind == AdapterDispatchKind::None &&
           value.reserved[0] == 0 &&
           value.reserved[1] == 0 &&
           value.reserved[2] == 0 &&
           value.dispatch_sequence == 0 &&
           value.enqueue_sequence == 0 &&
           has_safe_default(value.effect) &&
           has_safe_default(value.normal_intent);
}

constexpr bool has_safe_default(const AdapterCriticalLedger value)
{
    return value.first_reason == AdapterCriticalReason::None &&
           value.last_reason == AdapterCriticalReason::None &&
           value.reserved == 0 &&
           value.reason_mask == 0 &&
           value.first_ingress_sequence == 0 &&
           value.last_ingress_sequence == 0 &&
           value.first_diagnostic_code == 0 &&
           value.last_diagnostic_code == 0 &&
           value.occurrence_count == 0;
}

constexpr bool has_safe_default(const RuntimeOwnerAdapterView value)
{
    return has_safe_default(value.core) &&
           has_safe_default(value.current_dispatch) &&
           has_safe_default(value.physical_inflight) &&
           has_safe_default(value.critical) &&
           value.last_normal_enqueue_sequence == 0 &&
           value.last_trusted_ingress_sequence == 0 &&
           value.last_dispatch_sequence == 0 &&
           value.last_ack_dispatch_sequence == 0 &&
           value.last_trusted_diagnostic_ingress_sequence == 0 &&
           value.last_trusted_diagnostic_code == 0 &&
           value.normal_coalesced_count == 0 &&
           value.normal_rejected_full_count == 0 &&
           value.normal_cancelled_count == 0 &&
           value.trusted_rejected_full_count == 0 &&
           value.trusted_protocol_violation_count == 0 &&
           value.trusted_stale_count == 0 &&
           value.trusted_duplicate_count == 0 &&
           value.trusted_cancelled_count == 0 &&
           value.effect_cancelled_count == 0 &&
           value.dispatch_rejected_ack_count == 0 &&
           value.normal_completion_stale_count == 0 &&
           value.normal_depth == 0 &&
           value.normal_high_water == 0 &&
           value.trusted_depth == 0 &&
           value.trusted_high_water == 0 &&
           value.pending_effect_count == 0 &&
           value.transport_request_pending == 0 &&
           value.shutdown_pending == 0 &&
           value.shutdown_terminal_override_latched == 0 &&
           value.critical_pending == 0 &&
           value.boot_end_released == 0 &&
           value.core_fail_closed_latched == 0 &&
           value.core_adapter_fatal_latched == 0 &&
           value.sequence_fatal_latched == 0 &&
           value.dispatch_fatal_latched == 0 &&
           value.safety_delivery_blocked == 0 &&
           value.physical_inflight_cancel_pending == 0;
}

constexpr bool has_safe_default(const LastTrustedReceiptSignature value)
{
    return value.ingress_sequence == 0 && has_safe_default(value.receipt);
}

constexpr bool has_safe_default(const LastNormalCompletionSignature value)
{
    return value.ingress_sequence == 0 && has_safe_default(value.completion);
}

static_assert(has_safe_default(AdapterStepResult{}));
static_assert(has_safe_default(NormalIntent{}));
static_assert(has_safe_default(TrustedReceipt{}));
static_assert(has_safe_default(NormalCompletion{}));
static_assert(has_safe_default(TrustedIngressEnvelope{}));
static_assert(has_safe_default(AdapterDispatch{}));
static_assert(has_safe_default(AdapterCriticalLedger{}));
static_assert(has_safe_default(RuntimeOwnerAdapterView{}));
static_assert(has_safe_default(LastTrustedReceiptSignature{}));
static_assert(has_safe_default(LastNormalCompletionSignature{}));

void test_enum_numeric_and_unknown_contract()
{
    check_enum_type_and_unknown<NormalSubmitResult>();
    check_enum_value(NormalSubmitResult::RejectedInvalid, 0);
    check_enum_value(NormalSubmitResult::RejectedNotReady, 1);
    check_enum_value(NormalSubmitResult::RejectedFull, 2);
    check_enum_value(NormalSubmitResult::RejectedSequenceSaturated, 3);
    check_enum_value(NormalSubmitResult::Accepted, 4);
    check_enum_value(NormalSubmitResult::AcceptedCoalesced, 5);

    check_enum_type_and_unknown<UrgentRequestResult>();
    check_enum_value(UrgentRequestResult::Invalid, 0);
    check_enum_value(UrgentRequestResult::Accepted, 1);
    check_enum_value(UrgentRequestResult::AcceptedDuplicate, 2);
    check_enum_value(UrgentRequestResult::AlreadyTerminal, 3);

    check_enum_type_and_unknown<OwnerRequestResult>();
    check_enum_value(OwnerRequestResult::RejectedNotAllowed, 0);
    check_enum_value(OwnerRequestResult::RejectedFatal, 1);
    check_enum_value(OwnerRequestResult::Accepted, 2);
    check_enum_value(OwnerRequestResult::AcceptedDuplicate, 3);

    check_enum_type_and_unknown<DispatchAckResult>();
    check_enum_value(DispatchAckResult::RejectedNoDispatch, 0);
    check_enum_value(DispatchAckResult::RejectedWrongSequence, 1);
    check_enum_value(DispatchAckResult::AcceptedDelivery, 2);
    check_enum_value(DispatchAckResult::AcceptedOperationInflight, 3);
    check_enum_value(DispatchAckResult::AcceptedDuplicate, 4);

    check_enum_type_and_unknown<AdapterStepAction>();
    check_enum_value(AdapterStepAction::Invalid, 0);
    check_enum_value(AdapterStepAction::Idle, 1);
    check_enum_value(AdapterStepAction::AwaitingDispatchAck, 2);
    check_enum_value(AdapterStepAction::AwaitingTrustedReceipt, 3);
    check_enum_value(AdapterStepAction::CoreTransitionApplied, 4);
    CHECK(!HasCoreInputRejectedEnumMember<AdapterStepAction>::value);
    check_enum_value(AdapterStepAction::DispatchPrepared, 6);
    check_enum_value(AdapterStepAction::TrustedReceiptDiscarded, 7);
    check_enum_value(AdapterStepAction::CriticalLedgerHandled, 8);
    check_enum_value(AdapterStepAction::CoreAdapterFatalHandled, 9);
    check_enum_value(AdapterStepAction::Terminal, 10);

    CHECK((std::is_same<TrustedEnqueueResult, TrustedIngressResult>::value));
    check_enum_type_and_unknown<TrustedIngressResult>();
    check_enum_value(TrustedIngressResult::RejectedInvalid, 0);
    check_enum_value(TrustedIngressResult::RejectedNotAllowed, 1);
    check_enum_value(TrustedIngressResult::RejectedFull, 2);
    check_enum_value(TrustedIngressResult::RejectedSequenceSaturated, 3);
    check_enum_value(TrustedIngressResult::Accepted, 4);

    check_enum_type_and_unknown<NormalIntentKind>();
    check_enum_value(NormalIntentKind::Invalid, 0);
    check_enum_value(NormalIntentKind::PublishTelemetry, 1);
    check_enum_value(NormalIntentKind::RefreshRssi, 2);
    check_enum_value(NormalIntentKind::PullConfig, 3);
    check_enum_value(NormalIntentKind::PullCommand, 4);

    check_enum_type_and_unknown<TrustedReceiptKind>();
    check_enum_value(TrustedReceiptKind::Invalid, 0);
    check_enum_value(TrustedReceiptKind::TransportEstablished, 1);
    check_enum_value(TrustedReceiptKind::TransportAttemptFailed, 2);
    check_enum_value(TrustedReceiptKind::ConfigCommitted, 3);
    check_enum_value(TrustedReceiptKind::OperationCompleted, 4);
    check_enum_value(TrustedReceiptKind::OperationFailed, 5);
    check_enum_value(TrustedReceiptKind::DeadlineExpired, 6);
    check_enum_value(TrustedReceiptKind::SnapshotSucceeded, 7);
    check_enum_value(TrustedReceiptKind::SnapshotFailed, 8);
    check_enum_value(TrustedReceiptKind::TransportDisconnected, 9);

    check_enum_type_and_unknown<NormalCompletionKind>();
    check_enum_value(NormalCompletionKind::Invalid, 0);
    check_enum_value(NormalCompletionKind::Succeeded, 1);
    check_enum_value(NormalCompletionKind::Failed, 2);
    check_enum_value(NormalCompletionKind::TimedOut, 3);
    check_enum_value(NormalCompletionKind::Cancelled, 4);

    check_enum_type_and_unknown<TrustedIngressPayloadKind>();
    check_enum_value(TrustedIngressPayloadKind::None, 0);
    check_enum_value(TrustedIngressPayloadKind::CoreReceipt, 1);
    check_enum_value(TrustedIngressPayloadKind::NormalCompletion, 2);

    check_enum_type_and_unknown<AdapterDispatchKind>();
    check_enum_value(AdapterDispatchKind::None, 0);
    check_enum_value(AdapterDispatchKind::CoreEffect, 1);
    check_enum_value(AdapterDispatchKind::NormalIntent, 2);

    check_enum_type_and_unknown<AdapterCriticalReason>();
    check_enum_value(AdapterCriticalReason::None, 0);
    check_enum_value(AdapterCriticalReason::TrustedQueueOverflow, 1);
    check_enum_value(AdapterCriticalReason::TrustedProtocolViolation, 2);
    check_enum_value(AdapterCriticalReason::NormalSequenceSaturation, 3);
    check_enum_value(AdapterCriticalReason::TrustedSequenceSaturation, 4);
    check_enum_value(AdapterCriticalReason::DispatchSequenceSaturation, 5);
    check_enum_value(AdapterCriticalReason::PendingEffectInvariant, 6);
    check_enum_value(AdapterCriticalReason::CoreAdapterInvariant, 7);
}

void test_exact_dto_layout_and_type_traits()
{
    CHECK((has_fixed_dto_contract<AdapterStepResult, 16, 4>));
    CHECK((has_fixed_dto_contract<NormalIntent, 12, 4>));
    CHECK((has_fixed_dto_contract<TrustedReceipt, 28, 4>));
    CHECK((has_fixed_dto_contract<NormalCompletion, 16, 4>));
    CHECK((has_fixed_dto_contract<TrustedIngressEnvelope, 52, 4>));
    CHECK((has_fixed_dto_contract<AdapterDispatch, 48, 4>));
    CHECK((has_fixed_dto_contract<AdapterCriticalLedger, 28, 4>));
    CHECK((has_fixed_dto_contract<RuntimeOwnerAdapterView, 252, 4>));
    CHECK((has_fixed_dto_contract<LastTrustedReceiptSignature, 32, 4>));
    CHECK((has_fixed_dto_contract<LastNormalCompletionSignature, 20, 4>));
}

void test_dto_pointer_reference_and_owning_container_free_contract()
{
    CHECK(kAdapterStepResultValueFields);
    CHECK(kNormalIntentValueFields);
    CHECK(kTrustedReceiptValueFields);
    CHECK(kNormalCompletionValueFields);
    CHECK(kTrustedIngressEnvelopeValueFields);
    CHECK(kAdapterDispatchValueFields);
    CHECK(kAdapterCriticalLedgerValueFields);
    CHECK(kRuntimeOwnerAdapterViewValueFields);
    CHECK(kLastTrustedReceiptSignatureValueFields);
    CHECK(kLastNormalCompletionSignatureValueFields);
}

void test_dto_safe_zero_defaults()
{
    CHECK(has_safe_default(AdapterStepResult{}));
    CHECK(has_safe_default(NormalIntent{}));
    CHECK(has_safe_default(TrustedReceipt{}));
    CHECK(has_safe_default(NormalCompletion{}));
    CHECK(has_safe_default(TrustedIngressEnvelope{}));
    CHECK(has_safe_default(AdapterDispatch{}));
    CHECK(has_safe_default(AdapterCriticalLedger{}));
    CHECK(has_safe_default(RuntimeOwnerAdapterView{}));
    CHECK(has_safe_default(LastTrustedReceiptSignature{}));
    CHECK(has_safe_default(LastNormalCompletionSignature{}));
}

void test_adapter_explicit_noexcept_lifetime_and_noncopyable_contract()
{
    RuntimeOwnerAdapterCore adapter{};
    (void)adapter;

    CHECK(std::is_default_constructible<RuntimeOwnerAdapterCore>::value);
    CHECK(std::is_nothrow_default_constructible<RuntimeOwnerAdapterCore>::value);
    CHECK(std::is_nothrow_destructible<RuntimeOwnerAdapterCore>::value);
    CHECK(!IsImplicitlyDefaultListConstructible<RuntimeOwnerAdapterCore>::value);
    CHECK(!std::is_copy_constructible<RuntimeOwnerAdapterCore>::value);
    CHECK(!std::is_copy_assignable<RuntimeOwnerAdapterCore>::value);
    CHECK(!std::is_move_constructible<RuntimeOwnerAdapterCore>::value);
    CHECK(!std::is_move_assignable<RuntimeOwnerAdapterCore>::value);
}

void test_port_capability_construction_and_copy_contract()
{
    CHECK(!std::is_default_constructible<RuntimeOwnerNormalPort>::value);
    CHECK(!std::is_copy_constructible<RuntimeOwnerNormalPort>::value);
    CHECK(!std::is_copy_assignable<RuntimeOwnerNormalPort>::value);
    CHECK(!std::is_move_constructible<RuntimeOwnerNormalPort>::value);
    CHECK(!std::is_move_assignable<RuntimeOwnerNormalPort>::value);
    CHECK(std::is_nothrow_destructible<RuntimeOwnerNormalPort>::value);
    CHECK((!std::is_constructible<
           RuntimeOwnerNormalPort,
           RuntimeOwnerAdapterCore *>::value));

    CHECK(!std::is_default_constructible<RuntimeOwnerShutdownPort>::value);
    CHECK(!std::is_copy_constructible<RuntimeOwnerShutdownPort>::value);
    CHECK(!std::is_copy_assignable<RuntimeOwnerShutdownPort>::value);
    CHECK(!std::is_move_constructible<RuntimeOwnerShutdownPort>::value);
    CHECK(!std::is_move_assignable<RuntimeOwnerShutdownPort>::value);
    CHECK(std::is_nothrow_destructible<RuntimeOwnerShutdownPort>::value);
    CHECK((!std::is_constructible<
           RuntimeOwnerShutdownPort,
           RuntimeOwnerAdapterCore *>::value));

    CHECK(!std::is_default_constructible<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!std::is_copy_constructible<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!std::is_copy_assignable<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!std::is_move_constructible<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!std::is_move_assignable<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(std::is_nothrow_destructible<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK((!std::is_constructible<
           RuntimeOwnerTrustedReceiptPort,
           RuntimeOwnerAdapterCore *>::value));

    CHECK(!std::is_default_constructible<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!std::is_copy_constructible<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!std::is_copy_assignable<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!std::is_move_constructible<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!std::is_move_assignable<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(std::is_nothrow_destructible<RuntimeOwnerNormalCompletionPort>::value);
    CHECK((!std::is_constructible<
           RuntimeOwnerNormalCompletionPort,
           RuntimeOwnerAdapterCore *>::value));
}

void test_lvalue_only_port_factories()
{
    RuntimeOwnerAdapterCore adapter{};
    auto normal = adapter.normal_port();
    auto shutdown = adapter.shutdown_port();
    auto trusted_receipt = adapter.trusted_receipt_port();
    auto normal_completion = adapter.normal_completion_port();

    CHECK(sizeof(normal) == sizeof(void *));
    CHECK(sizeof(shutdown) == sizeof(void *));
    CHECK(sizeof(trusted_receipt) == sizeof(void *));
    CHECK(sizeof(normal_completion) == sizeof(void *));
    CHECK(HasLvalueNormalPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasRvalueNormalPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasConstLvalueNormalPort<RuntimeOwnerAdapterCore>::value);
    CHECK(HasLvalueShutdownPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasRvalueShutdownPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasConstLvalueShutdownPort<RuntimeOwnerAdapterCore>::value);
    CHECK(HasLvalueTrustedReceiptPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasRvalueTrustedReceiptPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasConstLvalueTrustedReceiptPort<RuntimeOwnerAdapterCore>::value);
    CHECK(HasLvalueNormalCompletionPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasRvalueNormalCompletionPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasConstLvalueNormalCompletionPort<RuntimeOwnerAdapterCore>::value);
}

void test_owner_only_api_signature_contract()
{
    CHECK((std::is_same<
           decltype(static_cast<NormalFactorySignature>(
               &RuntimeOwnerAdapterCore::normal_port)),
           NormalFactorySignature>::value));
    CHECK((std::is_same<
           decltype(static_cast<ShutdownFactorySignature>(
               &RuntimeOwnerAdapterCore::shutdown_port)),
           ShutdownFactorySignature>::value));
    CHECK((std::is_same<
           decltype(static_cast<TrustedReceiptFactorySignature>(
               &RuntimeOwnerAdapterCore::trusted_receipt_port)),
           TrustedReceiptFactorySignature>::value));
    CHECK((std::is_same<
           decltype(static_cast<NormalCompletionFactorySignature>(
               &RuntimeOwnerAdapterCore::normal_completion_port)),
           NormalCompletionFactorySignature>::value));
    CHECK((std::is_same<
           decltype(&RuntimeOwnerAdapterCore::request_transport_attempt),
           OwnerRequestSignature>::value));
    CHECK((std::is_same<
           decltype(&RuntimeOwnerAdapterCore::step),
           StepSignature>::value));
    CHECK((std::is_same<
           decltype(&RuntimeOwnerAdapterCore::peek_dispatch),
           PeekDispatchSignature>::value));
    CHECK((std::is_same<
           decltype(&RuntimeOwnerAdapterCore::acknowledge_dispatch),
           AcknowledgeDispatchSignature>::value));
    CHECK((std::is_same<
           decltype(&RuntimeOwnerAdapterCore::view),
           ViewSignature>::value));
}

void test_port_minimal_surface_contract()
{
    CHECK((std::is_same<
           decltype(&RuntimeOwnerNormalPort::submit),
           NormalSubmitSignature>::value));
    CHECK(!HasRequest<RuntimeOwnerNormalPort>::value);
    CHECK(!HasRequestTransportAttempt<RuntimeOwnerNormalPort>::value);
    CHECK(!HasStep<RuntimeOwnerNormalPort>::value);
    CHECK(!HasPeekDispatch<RuntimeOwnerNormalPort>::value);
    CHECK(!HasAcknowledgeDispatch<RuntimeOwnerNormalPort>::value);
    CHECK(!HasView<RuntimeOwnerNormalPort>::value);
    CHECK(!HasTrustedEnqueue<RuntimeOwnerNormalPort>::value);
    CHECK(!HasTrustedPort<RuntimeOwnerNormalPort>::value);
    CHECK(!HasRawCoreSubmit<RuntimeOwnerNormalPort>::value);

    CHECK((std::is_same<
           decltype(&RuntimeOwnerShutdownPort::request),
           ShutdownRequestSignature>::value));
    CHECK(!HasNormalIntentSubmit<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasRawCoreSubmit<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasRequestTransportAttempt<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasStep<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasPeekDispatch<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasAcknowledgeDispatch<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasView<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasTrustedEnqueue<RuntimeOwnerShutdownPort>::value);
    CHECK(!HasTrustedPort<RuntimeOwnerShutdownPort>::value);

    CHECK((std::is_same<
           decltype(&RuntimeOwnerTrustedReceiptPort::submit),
           TrustedReceiptSubmitSignature>::value));
    CHECK(HasTrustedReceiptSubmit<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasNormalCompletionSubmit<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasNormalIntentSubmit<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasRawCoreSubmit<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasRequest<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasRequestTransportAttempt<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasStep<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasPeekDispatch<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasAcknowledgeDispatch<RuntimeOwnerTrustedReceiptPort>::value);
    CHECK(!HasView<RuntimeOwnerTrustedReceiptPort>::value);

    CHECK((std::is_same<
           decltype(&RuntimeOwnerNormalCompletionPort::submit),
           NormalCompletionSubmitSignature>::value));
    CHECK(HasNormalCompletionSubmit<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasTrustedReceiptSubmit<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasNormalIntentSubmit<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasRawCoreSubmit<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasRequest<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasRequestTransportAttempt<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasStep<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasPeekDispatch<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasAcknowledgeDispatch<RuntimeOwnerNormalCompletionPort>::value);
    CHECK(!HasView<RuntimeOwnerNormalCompletionPort>::value);
}

void test_forbidden_adapter_public_surface_contract()
{
    CHECK(!HasRawCoreSubmit<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasNormalIntentSubmit<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasRequest<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasTrustedEnqueue<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasTrustedReceiptForTestingEnqueue<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasTrustedPort<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasCoreGetter<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasCorePtrGetter<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasStateGetter<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasStatePtrGetter<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasBoundaryGetter<RuntimeOwnerAdapterCore>::value);
    CHECK(!HasBoundaryPtrGetter<RuntimeOwnerAdapterCore>::value);
}

void test_typed_ingress_ports_use_production_validation_and_shared_ring()
{
    RuntimeOwnerAdapterCore adapter{};
    auto trusted_receipt = adapter.trusted_receipt_port();
    auto normal_completion = adapter.normal_completion_port();

    const TrustedReceipt canonical_receipt{
        TrustedReceiptKind::TransportAttemptFailed,
        RuntimeOwnerEffectKind::StartTransportAttempt,
        0,
        0,
        0,
        1,
        0,
        0,
        77,
    };
    const NormalCompletion canonical_completion{
        NormalCompletionKind::Succeeded,
        {},
        1,
        1,
        0,
    };
    CHECK(trusted_receipt.submit(canonical_receipt) ==
          TrustedIngressResult::Accepted);
    CHECK(normal_completion.submit(canonical_completion) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.view().trusted_depth == 2);
    CHECK(adapter.view().last_trusted_ingress_sequence == 2);

    TrustedReceipt invalid_receipt = canonical_receipt;
    invalid_receipt.reserved = 1;
    CHECK(trusted_receipt.submit(invalid_receipt) ==
          TrustedIngressResult::RejectedInvalid);
    NormalCompletion invalid_completion = canonical_completion;
    invalid_completion.dispatch_sequence = 0;
    CHECK(normal_completion.submit(invalid_completion) ==
          TrustedIngressResult::RejectedInvalid);
    CHECK(adapter.view().trusted_depth == 2);
    CHECK(adapter.view().last_trusted_ingress_sequence == 2);
    CHECK(adapter.view().trusted_protocol_violation_count == 2);
}

void fixture_prepare_runtime_ready(
    RuntimeOwnerAdapterCore &adapter,
    const bool boot_end_released)
{
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_runtime_ready(adapter));
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_boot_end_released(
        adapter, boot_end_released);
    const RuntimeOwnerAdapterPrivateSnapshot state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.core.phase == RuntimeOwnerPhase::RuntimeReady);
    CHECK(state.core.boot_orchestration_ended);
    CHECK(state.boot_end_released == boot_end_released);
}

void test_pending_effect_storage_exact_layout_and_zero_initial_state()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerAdapterPrivateSnapshot state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.pending_effect_slots.size() == 4);
    CHECK(state.pending_effect_head == 0);
    CHECK(state.pending_effect_tail == 0);
    CHECK(state.pending_effect_count == 0);
    CHECK(state.last_dispatch_sequence == 0);
    for (const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot :
         state.pending_effect_slots) {
        CHECK(slot.preassigned_dispatch_sequence == 0);
        CHECK(runtime_owner_effects_equal(slot.effect, RuntimeOwnerEffect{}));
    }
    CHECK(adapter.view().pending_effect_count == 0);
    CHECK(adapter.view().last_dispatch_sequence == 0);

    constexpr std::uint32_t kSeedDispatchSequence = 37;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, kSeedDispatchSequence);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
              .last_dispatch_sequence == kSeedDispatchSequence);
    CHECK(adapter.view().last_dispatch_sequence == kSeedDispatchSequence);
}

void check_transport_request_accepts_without_core_submit(
    RuntimeOwnerAdapterCore &adapter)
{
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(before.core, after.core));
    CHECK(after.transport_request_pending);
    CHECK(adapter.view().transport_request_pending == 1);
    CHECK(adapter.view().pending_effect_count == 0);
    CHECK(adapter.view().last_dispatch_sequence == 0);
    after.transport_request_pending = before.transport_request_pending;
    CHECK(private_snapshots_equal(before, after));
}

enum class TransportFatalFixture : std::uint8_t {
    CoreFailClosed,
    CoreAdapterFatal,
    SequenceFatal,
    DispatchFatal,
    SafetyDeliveryBlocked,
};

void apply_transport_fatal_fixture(
    RuntimeOwnerAdapterCore &adapter,
    const TransportFatalFixture fixture)
{
    switch (fixture) {
    case TransportFatalFixture::CoreFailClosed:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_fail_closed(
            adapter, true);
        break;
    case TransportFatalFixture::CoreAdapterFatal:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
            adapter, true);
        break;
    case TransportFatalFixture::SequenceFatal:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_sequence_fatal(
            adapter, true);
        break;
    case TransportFatalFixture::DispatchFatal:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_dispatch_fatal(
            adapter, true);
        break;
    case TransportFatalFixture::SafetyDeliveryBlocked:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_safety_delivery_blocked(
            adapter, true);
        break;
    }
}

void test_transport_request_accepts_only_coldstart_and_preboot_recovery()
{
    RuntimeOwnerAdapterCore cold_start{};
    check_transport_request_accepts_without_core_submit(cold_start);

    RuntimeOwnerAdapterCore recovery{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_recovery_pending(recovery));
    CHECK(!recovery.view().core.boot_orchestration_ended);
    CHECK(recovery.view().boot_end_released == 0);
    check_transport_request_accepts_without_core_submit(recovery);
}

void test_transport_request_duplicate_precedes_phase_and_is_mutation_free()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
        adapter, RuntimeOwnerPhase::TransportConnecting));
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.transport_request_pending);
    CHECK(before.core.phase == RuntimeOwnerPhase::TransportConnecting);
    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::AcceptedDuplicate);
    CHECK(private_snapshots_equal(
        before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_transport_request_rejects_all_other_phases_and_boot_end()
{
    constexpr std::array<RuntimeOwnerPhase, 6> disallowed_phases{{
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RuntimeReady,
        RuntimeOwnerPhase::ShutdownCommitted,
    }};

    for (const RuntimeOwnerPhase phase : disallowed_phases) {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, phase));
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::RejectedNotAllowed);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(adapter.view().transport_request_pending == 0);
    }

    RuntimeOwnerAdapterCore core_boot_ended{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_post_boot_recovery(core_boot_ended));
    const RuntimeOwnerAdapterPrivateSnapshot core_boot_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(core_boot_ended);
    CHECK(core_boot_ended.request_transport_attempt() ==
          OwnerRequestResult::RejectedNotAllowed);
    CHECK(private_snapshots_equal(
        core_boot_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(core_boot_ended)));

    RuntimeOwnerAdapterCore adapter_boot_ended{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_recovery_pending(adapter_boot_ended));
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_boot_end_released(
        adapter_boot_ended, true);
    const RuntimeOwnerAdapterPrivateSnapshot adapter_boot_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter_boot_ended);
    CHECK(adapter_boot_ended.request_transport_attempt() ==
          OwnerRequestResult::RejectedNotAllowed);
    CHECK(private_snapshots_equal(
        adapter_boot_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter_boot_ended)));
}

void test_transport_request_fatal_precedes_disallowed_and_pending()
{
    constexpr std::array<TransportFatalFixture, 5> fatal_fixtures{{
        TransportFatalFixture::CoreFailClosed,
        TransportFatalFixture::CoreAdapterFatal,
        TransportFatalFixture::SequenceFatal,
        TransportFatalFixture::DispatchFatal,
        TransportFatalFixture::SafetyDeliveryBlocked,
    }};

    for (const TransportFatalFixture fixture : fatal_fixtures) {
        RuntimeOwnerAdapterCore adapter{};
        apply_transport_fatal_fixture(adapter, fixture);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::RejectedFatal);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(adapter.view().transport_request_pending == 0);
    }

    RuntimeOwnerAdapterCore disallowed{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
        disallowed, RuntimeOwnerPhase::RuntimeReady));
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_dispatch_fatal(
        disallowed, true);
    const RuntimeOwnerAdapterPrivateSnapshot disallowed_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(disallowed);
    CHECK(disallowed.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    CHECK(private_snapshots_equal(
        disallowed_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(disallowed)));

    RuntimeOwnerAdapterCore pending{};
    CHECK(pending.request_transport_attempt() == OwnerRequestResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
        pending, true);
    const RuntimeOwnerAdapterPrivateSnapshot pending_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(pending);
    CHECK(pending.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    CHECK(private_snapshots_equal(
        pending_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(pending)));
}

void test_transport_request_shutdown_pending_precedes_allowed_phase_and_duplicate()
{
    constexpr std::array<RuntimeOwnerPhase, 2> allowed_phases{{
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::RecoveryPending,
    }};

    for (const RuntimeOwnerPhase phase : allowed_phases) {
        RuntimeOwnerAdapterCore no_trigger{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            no_trigger, phase));
        CHECK(no_trigger.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot no_trigger_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(no_trigger);
        CHECK(no_trigger.request_transport_attempt() ==
              OwnerRequestResult::RejectedNotAllowed);
        CHECK(private_snapshots_equal(
            no_trigger_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(no_trigger)));

        RuntimeOwnerAdapterCore trigger_pending{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            trigger_pending, phase));
        CHECK(trigger_pending.request_transport_attempt() ==
              OwnerRequestResult::Accepted);
        CHECK(trigger_pending.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot trigger_pending_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(trigger_pending);
        CHECK(trigger_pending.request_transport_attempt() ==
              OwnerRequestResult::RejectedNotAllowed);
        CHECK(private_snapshots_equal(
            trigger_pending_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(trigger_pending)));

        RuntimeOwnerAdapterCore fatal{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            fatal, phase));
        CHECK(fatal.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
            fatal, true);
        const RuntimeOwnerAdapterPrivateSnapshot fatal_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(fatal);
        CHECK(fatal.request_transport_attempt() ==
              OwnerRequestResult::RejectedFatal);
        CHECK(private_snapshots_equal(
            fatal_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(fatal)));
    }
}

void check_exact_step_result(
    const AdapterStepResult result,
    const AdapterStepAction action,
    const RuntimeOwnerDisposition disposition,
    const RuntimeOwnerPhase phase_before,
    const RuntimeOwnerPhase phase_after)
{
    CHECK(result.action == action);
    CHECK(result.core_disposition == disposition);
    CHECK(result.phase_before == phase_before);
    CHECK(result.phase_after == phase_after);
    CHECK(result.consumed_ingress_sequence == 0);
    CHECK(result.consumed_enqueue_sequence == 0);
    CHECK(result.prepared_dispatch_sequence == 0);
}

void check_canonical_begin_pending_effect(
    const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot,
    const std::uint32_t expected_dispatch_sequence,
    const std::uint32_t expected_generation)
{
    CHECK(slot.preassigned_dispatch_sequence ==
          expected_dispatch_sequence);
    CHECK(slot.effect.kind ==
          RuntimeOwnerEffectKind::StartTransportAttempt);
    CHECK(slot.effect.correlation_id == 0);
    CHECK(slot.effect.attempt.mqtt_session_id == 0);
    CHECK(slot.effect.attempt.mqtt_generation == expected_generation);
    CHECK(slot.effect.attempt.config_apply_epoch == 0);
    CHECK(slot.effect.fault_code == RuntimeOwnerFaultCode::None);
}

void check_unused_pending_effect_slots_are_zero(
    const RuntimeOwnerAdapterPrivateSnapshot &state,
    const std::size_t first_unused)
{
    for (std::size_t offset = first_unused;
         offset < state.pending_effect_slots.size(); ++offset) {
        const std::size_t index =
            (state.pending_effect_head + offset) %
            state.pending_effect_slots.size();
        CHECK(state.pending_effect_slots[index]
                  .preassigned_dispatch_sequence == 0);
        CHECK(runtime_owner_effects_equal(
            state.pending_effect_slots[index].effect,
            RuntimeOwnerEffect{}));
    }
}

void check_pending_owner_trigger_hits_terminal_without_mutation(
    RuntimeOwnerAdapterCore &adapter)
{
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.transport_request_pending);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        before.core.phase,
        before.core.phase);
    CHECK(private_snapshots_equal(
        before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_step_without_owner_trigger_is_canonical_idle_and_mutation_free()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Idle,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::ColdStart);
    CHECK(private_snapshots_equal(
        before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_owner_trigger_step_prioritizes_shutdown_critical_and_fatal_terminal()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::Accepted);
        auto shutdown = adapter.shutdown_port();
        CHECK(shutdown.request() == UrgentRequestResult::Accepted);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::ColdStart,
            RuntimeOwnerPhase::ShutdownCommitted);
        CHECK(adapter.view().transport_request_pending == 0);
        CHECK(adapter.step().action == AdapterStepAction::Terminal);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::Accepted);
        TrustedReceipt invalid = make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
        invalid.reserved = 1;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) ==
              TrustedEnqueueResult::RejectedInvalid);
        CHECK(adapter.view().critical_pending == 1);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::ColdStart,
            RuntimeOwnerPhase::RecoveryPending);
        CHECK(adapter.view().transport_request_pending == 0);
        CHECK(adapter.view().critical_pending == 0);
        CHECK(adapter.view().pending_effect_count == 2);
    }

    constexpr std::array<TransportFatalFixture, 5> fatal_fixtures{{
        TransportFatalFixture::CoreFailClosed,
        TransportFatalFixture::CoreAdapterFatal,
        TransportFatalFixture::SequenceFatal,
        TransportFatalFixture::DispatchFatal,
        TransportFatalFixture::SafetyDeliveryBlocked,
    }};
    for (const TransportFatalFixture fixture : fatal_fixtures) {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::Accepted);
        apply_transport_fatal_fixture(adapter, fixture);
        check_pending_owner_trigger_hits_terminal_without_mutation(adapter);
    }
}

void test_owner_trigger_step_submits_begin_once_and_atomically_commits_pending_effect()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.transport_request_pending);
    CHECK(before.pending_effect_count == 0);
    CHECK(before.last_dispatch_sequence == 0);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::TransportConnecting);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.phase == RuntimeOwnerPhase::TransportConnecting);
    CHECK(after.core.mqtt_generation_counter == 1);
    CHECK(after.core.last_fault == RuntimeOwnerFaultCode::None);
    CHECK(!after.transport_request_pending);
    CHECK(after.last_dispatch_sequence == 1);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 1);
    CHECK(after.pending_effect_count == 1);
    check_canonical_begin_pending_effect(
        after.pending_effect_slots[0], 1, 1);
    check_unused_pending_effect_slots_are_zero(after, 1);

    const RuntimeOwnerAdapterView view = adapter.view();
    CHECK(view.core.phase == RuntimeOwnerPhase::TransportConnecting);
    CHECK(view.core.mqtt_generation_counter == 1);
    CHECK(view.transport_request_pending == 0);
    CHECK(view.pending_effect_count == 1);
    CHECK(view.last_dispatch_sequence == 1);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(prepared.phase_before == RuntimeOwnerPhase::TransportConnecting);
    CHECK(prepared.phase_after == RuntimeOwnerPhase::TransportConnecting);
    CHECK(prepared.consumed_ingress_sequence == 0);
    CHECK(prepared.consumed_enqueue_sequence == 0);
    CHECK(prepared.prepared_dispatch_sequence == 1);
    CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::CoreEffect);
    CHECK(adapter.peek_dispatch().dispatch_sequence == 1);
    CHECK(adapter.view().pending_effect_count == 0);
}

void test_owner_trigger_step_recovery_begin_uses_next_dispatch_sequence()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_recovery_pending(adapter));
    constexpr std::uint32_t kPreviousDispatchSequence = 41;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, kPreviousDispatchSequence);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::TransportConnecting);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.phase == RuntimeOwnerPhase::TransportConnecting);
    CHECK(after.core.mqtt_generation_counter == 1);
    CHECK(after.core.last_fault == RuntimeOwnerFaultCode::None);
    CHECK(!after.transport_request_pending);
    CHECK(after.last_dispatch_sequence == 42);
    CHECK(after.pending_effect_count == 1);
    check_canonical_begin_pending_effect(
        after.pending_effect_slots[0], 42, 1);
    check_unused_pending_effect_slots_are_zero(after, 1);
}

void test_owner_trigger_last_non_safety_dispatch_id_preserves_terminal_reserve()
{
    RuntimeOwnerAdapterCore adapter{};
    constexpr std::uint32_t kLastSuccessfulStart =
        std::numeric_limits<std::uint32_t>::max() - 3;
    constexpr std::uint32_t kReservedBeginSequence =
        std::numeric_limits<std::uint32_t>::max() - 2;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, kLastSuccessfulStart);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::TransportConnecting);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.last_dispatch_sequence == kReservedBeginSequence);
    CHECK(after.pending_effect_count == 1);
    check_canonical_begin_pending_effect(
        after.pending_effect_slots[0], kReservedBeginSequence, 1);
}

void test_owner_trigger_dispatch_shortage_records_critical_before_core_submit()
{
    RuntimeOwnerAdapterCore adapter{};
    constexpr std::uint32_t kInsufficientStart =
        std::numeric_limits<std::uint32_t>::max() - 2;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, kInsufficientStart);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CriticalLedgerHandled,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::ColdStart);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(before.core, after.core));
    CHECK(after.transport_request_pending);
    CHECK(after.last_dispatch_sequence == kInsufficientStart);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 0);
    CHECK(after.pending_effect_count == 0);
    check_unused_pending_effect_slots_are_zero(after, 0);
    CHECK(after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::DispatchSequenceSaturation);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::DispatchSequenceSaturation);
    CHECK(after.critical.reason_mask == (1u << 4u));
    CHECK(after.critical.first_ingress_sequence == 0);
    CHECK(after.critical.last_ingress_sequence == 0);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(!after.sequence_fatal_latched);
    CHECK(!after.dispatch_fatal_latched);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::RecoveryPending);
    CHECK(adapter.view().critical_pending == 0);
    CHECK(adapter.view().dispatch_fatal_latched == 1);
    CHECK(adapter.view().pending_effect_count == 2);
}

void test_task4a_request_and_begin_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

constexpr TrustedReceipt make_transport_attempt_failed_receipt(
    std::uint32_t generation,
    std::uint32_t diagnostic_code) noexcept;

void fixture_prepare_connecting_without_pending(
    RuntimeOwnerAdapterCore &adapter);

void test_task5_pending_core_effect_prepares_one_sticky_current_dispatch()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.pending_effect_count == 1);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(prepared.phase_before == RuntimeOwnerPhase::TransportConnecting);
    CHECK(prepared.phase_after == RuntimeOwnerPhase::TransportConnecting);
    CHECK(prepared.consumed_ingress_sequence == 0);
    CHECK(prepared.consumed_enqueue_sequence == 0);
    CHECK(prepared.prepared_dispatch_sequence == 1);

    const AdapterDispatch expected{
        AdapterDispatchKind::CoreEffect,
        {},
        1,
        0,
        before.pending_effect_slots[before.pending_effect_head].effect,
        {},
    };
    CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), expected));
    CHECK(adapter_dispatches_equal(
        adapter.view().current_dispatch, expected));
    CHECK(adapter.view().pending_effect_count == 0);

    const AdapterDispatch first_peek = adapter.peek_dispatch();
    const RuntimeOwnerAdapterView before_wait = adapter.view();
    const AdapterStepResult waiting = adapter.step();
    CHECK(waiting.action == AdapterStepAction::AwaitingDispatchAck);
    CHECK(waiting.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(waiting.phase_before == RuntimeOwnerPhase::TransportConnecting);
    CHECK(waiting.phase_after == RuntimeOwnerPhase::TransportConnecting);
    CHECK(waiting.consumed_ingress_sequence == 0);
    CHECK(waiting.consumed_enqueue_sequence == 0);
    CHECK(waiting.prepared_dispatch_sequence == 0);
    CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), first_peek));
    CHECK(adapter_dispatches_equal(
        adapter.view().current_dispatch, before_wait.current_dispatch));
}

void test_task5_ready_normal_head_prepares_one_sticky_current_dispatch()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_runtime_ready(adapter));
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_boot_end_released(
        adapter, true);
    auto normal = adapter.normal_port();
    constexpr NormalIntent intent = make_telemetry_intent(77, 9);
    CHECK(normal.submit(intent) == NormalSubmitResult::Accepted);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(prepared.phase_before == RuntimeOwnerPhase::RuntimeReady);
    CHECK(prepared.phase_after == RuntimeOwnerPhase::RuntimeReady);
    CHECK(prepared.consumed_ingress_sequence == 0);
    CHECK(prepared.consumed_enqueue_sequence == 1);
    CHECK(prepared.prepared_dispatch_sequence == 1);

    constexpr AdapterDispatch expected{
        AdapterDispatchKind::NormalIntent,
        {},
        1,
        1,
        {},
        intent,
    };
    CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), expected));
    CHECK(adapter_dispatches_equal(
        adapter.view().current_dispatch, expected));
    CHECK(adapter.view().normal_depth == 0);

    const AdapterDispatch first_peek = adapter.peek_dispatch();
    const AdapterStepResult waiting = adapter.step();
    CHECK(waiting.action == AdapterStepAction::AwaitingDispatchAck);
    CHECK(waiting.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(waiting.phase_before == RuntimeOwnerPhase::RuntimeReady);
    CHECK(waiting.phase_after == RuntimeOwnerPhase::RuntimeReady);
    CHECK(waiting.consumed_ingress_sequence == 0);
    CHECK(waiting.consumed_enqueue_sequence == 0);
    CHECK(waiting.prepared_dispatch_sequence == 0);
    CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), first_peek));
}

void test_task5_ack_without_current_rejects_and_only_counts_diagnostic()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter.acknowledge_dispatch(1) ==
          DispatchAckResult::RejectedNoDispatch);
    CHECK(adapter.view().dispatch_rejected_ack_count == 1);
    CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
    CHECK(adapter.view().physical_inflight.kind ==
          AdapterDispatchKind::None);
    CHECK(adapter.view().last_ack_dispatch_sequence == 0);
    RuntimeOwnerAdapterPrivateSnapshot expected = before;
    expected.dispatch_rejected_ack_count = 1;
    CHECK(private_snapshots_equal(
        expected, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_task5_wrong_ack_preserves_exact_current_then_core_ack_moves_inflight()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
    CHECK(offered.effect.kind ==
          RuntimeOwnerEffectKind::StartTransportAttempt);

    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence + 1) ==
          DispatchAckResult::RejectedWrongSequence);
    CHECK(adapter.view().dispatch_rejected_ack_count == 1);
    CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), offered));
    CHECK(adapter.view().physical_inflight.kind ==
          AdapterDispatchKind::None);
    CHECK(adapter.view().last_ack_dispatch_sequence == 0);

    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, offered));
    CHECK(adapter.view().last_ack_dispatch_sequence ==
          offered.dispatch_sequence);
    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedDuplicate);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, offered));
    CHECK(adapter.view().dispatch_rejected_ack_count == 1);
    CHECK(adapter.step().action == AdapterStepAction::AwaitingTrustedReceipt);
}

void test_task5_last_ack_duplicate_preserves_new_current_dispatch()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_attempt_failed_receipt(1, 77)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);

    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch first = adapter.peek_dispatch();
    CHECK(first.effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(adapter.acknowledge_dispatch(first.dispatch_sequence) ==
          DispatchAckResult::AcceptedDelivery);

    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch second = adapter.peek_dispatch();
    CHECK(second.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
    const RuntimeOwnerAdapterPrivateSnapshot before_duplicate =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter.acknowledge_dispatch(first.dispatch_sequence) ==
          DispatchAckResult::AcceptedDuplicate);
    CHECK(private_snapshots_equal(
        before_duplicate,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(adapter.acknowledge_dispatch(second.dispatch_sequence) ==
          DispatchAckResult::AcceptedDelivery);
}

void test_task5_delivery_only_safety_acks_do_not_create_physical_inflight()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_attempt_failed_receipt(1, 77)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().pending_effect_count == 2);

    constexpr std::array<RuntimeOwnerEffectKind, 2> expected{{
        RuntimeOwnerEffectKind::RecordFault,
        RuntimeOwnerEffectKind::EnterRecovery,
    }};
    for (const RuntimeOwnerEffectKind kind : expected) {
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(offered.effect.kind == kind);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
        CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
        CHECK(adapter.view().physical_inflight.kind ==
              AdapterDispatchKind::None);
        CHECK(adapter.view().last_ack_dispatch_sequence ==
              offered.dispatch_sequence);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDuplicate);
    }
    CHECK(adapter.view().pending_effect_count == 0);
}

void test_task5_exact_normal_ack_moves_full_dispatch_to_physical_inflight()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_runtime_ready(adapter));
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_boot_end_released(
        adapter, true);
    auto normal = adapter.normal_port();
    CHECK(normal.submit(make_telemetry_intent(77, 9)) ==
          NormalSubmitResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(offered.kind == AdapterDispatchKind::NormalIntent);
    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, offered));
    CHECK(adapter.step().action == AdapterStepAction::AwaitingTrustedReceipt);
}

constexpr NormalCompletion make_normal_completion(
    const NormalCompletionKind kind,
    const AdapterDispatch dispatch,
    const std::uint32_t diagnostic_code = 0) noexcept
{
    return {
        kind,
        {},
        dispatch.dispatch_sequence,
        dispatch.enqueue_sequence,
        diagnostic_code,
    };
}

constexpr TrustedReceipt make_transport_disconnected_receipt(
    std::uint32_t session_id,
    std::uint32_t generation,
    std::uint32_t diagnostic_code) noexcept;
constexpr TrustedReceipt make_config_committed_receipt(
    std::uint32_t session_id,
    std::uint32_t generation,
    std::uint32_t config_commit_sequence) noexcept;
constexpr TrustedReceipt make_operation_completed_receipt(
    RuntimeOwnerEffect ticket) noexcept;
std::array<RuntimeOwnerEffect, 4>
fixture_prepare_liveness_waiting_via_config(
    RuntimeOwnerAdapterCore &adapter,
    bool keep_pending_effects);

AdapterDispatch fixture_prepare_normal_physical_inflight(
    RuntimeOwnerAdapterCore &adapter)
{
    fixture_prepare_runtime_ready(adapter, true);
    auto normal = adapter.normal_port();
    CHECK(normal.submit(make_telemetry_intent(71, 19)) ==
          NormalSubmitResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch dispatch = adapter.peek_dispatch();
    CHECK(dispatch.kind == AdapterDispatchKind::NormalIntent);
    CHECK(dispatch.dispatch_sequence != 0);
    CHECK(dispatch.enqueue_sequence != 0);
    CHECK(adapter.acknowledge_dispatch(dispatch.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, dispatch));
    return dispatch;
}

void test_task5_normal_completion_canonical_shape_and_shared_ring_capacity()
{
    RuntimeOwnerAdapterCore adapter{};
    const AdapterDispatch dispatch =
        fixture_prepare_normal_physical_inflight(adapter);
    const NormalCompletion canonical = make_normal_completion(
        NormalCompletionKind::Succeeded, dispatch);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
              adapter, canonical) == TrustedEnqueueResult::Accepted);

    NormalCompletion malformed = canonical;
    malformed.reserved[1] = 1;
    {
        RuntimeOwnerAdapterCore invalid{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
                  invalid, malformed) ==
              TrustedEnqueueResult::RejectedInvalid);
    }
    malformed = canonical;
    malformed.dispatch_sequence = 0;
    {
        RuntimeOwnerAdapterCore invalid{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
                  invalid, malformed) ==
              TrustedEnqueueResult::RejectedInvalid);
    }
    malformed = canonical;
    malformed.enqueue_sequence = 0;
    {
        RuntimeOwnerAdapterCore invalid{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
                  invalid, malformed) ==
              TrustedEnqueueResult::RejectedInvalid);
    }
    malformed = canonical;
    malformed.diagnostic_code = 1;
    {
        RuntimeOwnerAdapterCore invalid{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
                  invalid, malformed) ==
              TrustedEnqueueResult::RejectedInvalid);
    }

    RuntimeOwnerAdapterCore full{};
    for (std::uint32_t index = 1; index <= 8; ++index) {
        TrustedReceipt receipt = make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
        receipt.mqtt_session_id = 100 + index;
        receipt.mqtt_generation = 200 + index;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  full, receipt) == TrustedEnqueueResult::Accepted);
    }
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
              full,
              {NormalCompletionKind::Failed, {}, 1, 1, 91}) ==
          TrustedEnqueueResult::RejectedFull);
    CHECK(full.view().trusted_depth == 8);
    CHECK(full.view().trusted_rejected_full_count == 1);
}

void test_task5_exact_normal_completion_closes_inflight_for_all_outcomes()
{
    for (const NormalCompletionKind kind : {
             NormalCompletionKind::Succeeded,
             NormalCompletionKind::Failed,
             NormalCompletionKind::TimedOut,
             NormalCompletionKind::Cancelled,
         }) {
        RuntimeOwnerAdapterCore adapter{};
        const AdapterDispatch dispatch =
            fixture_prepare_normal_physical_inflight(adapter);
        const NormalCompletion completion = make_normal_completion(
            kind,
            dispatch,
            kind == NormalCompletionKind::Succeeded ? 0 : 91);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
                  adapter, completion) == TrustedEnqueueResult::Accepted);
        const std::uint32_t ingress_sequence =
            adapter.view().last_trusted_ingress_sequence;
        const AdapterStepResult result = adapter.step();
        CHECK(result.action == AdapterStepAction::TrustedReceiptDiscarded);
        CHECK(result.core_disposition == RuntimeOwnerDisposition::Rejected);
        CHECK(result.phase_before == RuntimeOwnerPhase::RuntimeReady);
        CHECK(result.phase_after == RuntimeOwnerPhase::RuntimeReady);
        CHECK(result.consumed_ingress_sequence == ingress_sequence);
        CHECK(result.consumed_enqueue_sequence == 0);
        CHECK(result.prepared_dispatch_sequence == 0);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(has_safe_default(after.physical_inflight));
        CHECK(after.last_normal_completion_signature.ingress_sequence ==
              ingress_sequence);
        CHECK(normal_completions_equal(
            after.last_normal_completion_signature.completion,
            completion));
        CHECK(after.normal_completion_stale_count == 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
    }
}

void test_task5_wrong_and_duplicate_normal_completion_are_bounded()
{
    RuntimeOwnerAdapterCore adapter{};
    const AdapterDispatch dispatch =
        fixture_prepare_normal_physical_inflight(adapter);
    NormalCompletion wrong = make_normal_completion(
        NormalCompletionKind::Failed, dispatch, 91);
    ++wrong.enqueue_sequence;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
              adapter, wrong) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot wrong_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    const RuntimeOwnerAdapterPrivateSnapshot wrong_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter_dispatches_equal(
        wrong_after.physical_inflight, wrong_before.physical_inflight));
    CHECK(wrong_after.normal_completion_stale_count ==
          wrong_before.normal_completion_stale_count + 1);
    CHECK(wrong_after.last_normal_completion_signature.ingress_sequence == 0);

    const NormalCompletion accepted = make_normal_completion(
        NormalCompletionKind::Succeeded, dispatch);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
              adapter, accepted) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().core_disposition == RuntimeOwnerDisposition::Rejected);
    const RuntimeOwnerAdapterPrivateSnapshot accepted_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(has_safe_default(accepted_after.physical_inflight));

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
              adapter, accepted) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot duplicate_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    const RuntimeOwnerAdapterPrivateSnapshot duplicate_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(duplicate_after.trusted_duplicate_count ==
          duplicate_before.trusted_duplicate_count + 1);
    CHECK(duplicate_after.normal_completion_stale_count ==
          duplicate_before.normal_completion_stale_count);
    CHECK(last_normal_completion_signatures_equal(
        duplicate_after.last_normal_completion_signature,
        duplicate_before.last_normal_completion_signature));
}

void fixture_ack_all_pending_safety_dispatches(
    RuntimeOwnerAdapterCore &adapter)
{
    while (adapter.view().pending_effect_count != 0) {
        const AdapterStepResult prepared = adapter.step();
        CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(offered.effect.kind == RuntimeOwnerEffectKind::RecordFault ||
              offered.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
    }
    CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
}

void test_task5_accepted_config_cancels_current_and_quarantines_inflight()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        CHECK(adapter.peek_dispatch().kind ==
              AdapterDispatchKind::CoreEffect);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        const RuntimeOwnerAdapterView after = adapter.view();
        CHECK(after.core.phase == RuntimeOwnerPhase::LivenessWaiting);
        CHECK(has_safe_default(after.current_dispatch));
        CHECK(has_safe_default(after.physical_inflight));
        CHECK(after.pending_effect_count == 4);
        CHECK(after.effect_cancelled_count == 1);
        CHECK(after.physical_inflight_cancel_pending == 0);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch started = adapter.peek_dispatch();
        CHECK(adapter.acknowledge_dispatch(started.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        const RuntimeOwnerAdapterView after = adapter.view();
        CHECK(adapter_dispatches_equal(after.physical_inflight, started));
        CHECK(after.physical_inflight_cancel_pending == 1);
        CHECK(after.effect_cancelled_count == 1);
        CHECK(after.pending_effect_count == 4);
        CHECK(adapter.step().action ==
              AdapterStepAction::AwaitingTrustedReceipt);
    }
}

void test_task5_exact_config_retransmission_is_duplicate()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    const TrustedReceipt config =
        make_config_committed_receipt(77, 1, 9);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, config) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);

    const RuntimeOwnerAdapterPrivateSnapshot accepted =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, config) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    const RuntimeOwnerAdapterPrivateSnapshot duplicate =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(duplicate.trusted_duplicate_count ==
          accepted.trusted_duplicate_count + 1);
    CHECK(duplicate.trusted_stale_count == accepted.trusted_stale_count);
    CHECK(duplicate.core.phase == accepted.core.phase);
    CHECK(duplicate.pending_effect_count == accepted.pending_effect_count);
    CHECK(last_trusted_receipt_signatures_equal(
        duplicate.last_trusted_receipt_signature,
        accepted.last_trusted_receipt_signature));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
}

void test_task5_quarantined_inflight_does_not_block_valid_disconnect()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_authorization_pending_effect(adapter);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch started = adapter.peek_dispatch();
    CHECK(adapter.acknowledge_dispatch(started.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().core.phase == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, started));

    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    const std::uint32_t stale_count_before =
        adapter.view().trusted_stale_count;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(77, 1, 91)) ==
          TrustedEnqueueResult::Accepted);
    const AdapterStepResult disconnected = adapter.step();
    CHECK(disconnected.action == AdapterStepAction::CoreTransitionApplied);
    CHECK(disconnected.phase_before == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(disconnected.phase_after == RuntimeOwnerPhase::RecoveryPending);
    CHECK(adapter.view().core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(adapter.view().trusted_stale_count == stale_count_before);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, started));
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);
    CHECK(adapter.view().pending_effect_count == 2);
}

void test_task5_ready_disconnect_cancels_normal_and_quarantines_completion()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_runtime_ready(adapter, true);
    auto normal = adapter.normal_port();
    CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
          NormalSubmitResult::Accepted);
    CHECK(normal.submit(make_telemetry_intent(2, 2)) ==
          NormalSubmitResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch started = adapter.peek_dispatch();
    CHECK(adapter.acknowledge_dispatch(started.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(1, 1, 91)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    const RuntimeOwnerAdapterView disconnected = adapter.view();
    CHECK(disconnected.core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(disconnected.normal_depth == 0);
    CHECK(disconnected.normal_cancelled_count == 2);
    CHECK(adapter_dispatches_equal(
        disconnected.physical_inflight, started));
    CHECK(disconnected.physical_inflight_cancel_pending == 1);
    CHECK(disconnected.pending_effect_count == 2);

    fixture_ack_all_pending_safety_dispatches(adapter);
    NormalCompletion wrong = make_normal_completion(
        NormalCompletionKind::Cancelled, started, 77);
    ++wrong.dispatch_sequence;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
              adapter, wrong) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);
    CHECK(adapter.view().normal_completion_stale_count == 1);

    const NormalCompletion exact = make_normal_completion(
        NormalCompletionKind::Cancelled, started, 78);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
              adapter, exact) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(has_safe_default(adapter.view().physical_inflight));
    CHECK(adapter.view().physical_inflight_cancel_pending == 0);
    CHECK(adapter.view().normal_completion_stale_count == 2);
    CHECK(adapter.view().last_ack_dispatch_sequence ==
          disconnected.last_ack_dispatch_sequence + 2);
}

void test_task5_disconnect_quarantine_requires_exact_core_terminal_receipt()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter, false);
    const AdapterDispatch started = adapter.view().physical_inflight;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(77, 1, 91)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);
    CHECK(adapter.view().effect_cancelled_count == 4);
    fixture_ack_all_pending_safety_dispatches(adapter);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_operation_completed_receipt(tickets[1])) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, started));
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_operation_completed_receipt(tickets[0])) ==
          TrustedEnqueueResult::Accepted);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(has_safe_default(adapter.view().physical_inflight));
    CHECK(adapter.view().physical_inflight_cancel_pending == 0);
    CHECK(adapter.view().trusted_stale_count == 2);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
}

void test_task5_rejected_control_receipts_preserve_old_authorization()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 9);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch started = adapter.peek_dispatch();
        CHECK(adapter.acknowledge_dispatch(started.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(adapter_dispatches_equal(
            after.physical_inflight, before.physical_inflight));
        CHECK(!after.physical_inflight_cancel_pending);
        CHECK(after.effect_cancelled_count ==
              before.effect_cancelled_count);
        CHECK(after.normal_cancelled_count ==
              before.normal_cancelled_count);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        auto normal = adapter.normal_port();
        CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
              NormalSubmitResult::Accepted);
        CHECK(normal.submit(make_telemetry_intent(2, 2)) ==
              NormalSubmitResult::Accepted);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch started = adapter.peek_dispatch();
        CHECK(adapter.acknowledge_dispatch(started.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_disconnected_receipt(2, 1, 91)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(adapter_dispatches_equal(
            after.physical_inflight, before.physical_inflight));
        CHECK(after.normal_count == before.normal_count);
        CHECK(after.normal_cancelled_count ==
              before.normal_cancelled_count);
        CHECK(!after.physical_inflight_cancel_pending);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        const TrustedReceipt config =
            make_config_committed_receipt(77, 1, 9);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, config) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(before.pending_effect_count == 4);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, config) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.pending_effect_count == before.pending_effect_count);
        CHECK(after.effect_cancelled_count ==
              before.effect_cancelled_count);
        CHECK(after.normal_cancelled_count ==
              before.normal_cancelled_count);
    }
}

void test_task5_full_lifecycle_is_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        const AdapterDispatch dispatch =
            fixture_prepare_normal_physical_inflight(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
                  adapter,
                  make_normal_completion(
                      NormalCompletionKind::Succeeded, dispatch)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().core_disposition ==
              RuntimeOwnerDisposition::Rejected);
    }
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        auto normal = adapter.normal_port();
        CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
              NormalSubmitResult::Accepted);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch started = adapter.peek_dispatch();
        CHECK(adapter.acknowledge_dispatch(started.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_disconnected_receipt(1, 1, 91)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        fixture_ack_all_pending_safety_dispatches(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_normal_completion(
                  adapter,
                  make_normal_completion(
                      NormalCompletionKind::Cancelled, started, 91)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

constexpr TrustedReceipt make_transport_established_receipt(
    const std::uint32_t session_id,
    const std::uint32_t generation) noexcept
{
    return {
        TrustedReceiptKind::TransportEstablished,
        RuntimeOwnerEffectKind::StartTransportAttempt,
        0,
        0,
        session_id,
        generation,
        0,
        0,
        0,
    };
}

constexpr TrustedReceipt make_transport_attempt_failed_receipt(
    const std::uint32_t generation,
    const std::uint32_t diagnostic_code = 0) noexcept
{
    return {
        TrustedReceiptKind::TransportAttemptFailed,
        RuntimeOwnerEffectKind::StartTransportAttempt,
        0,
        0,
        0,
        generation,
        0,
        0,
        diagnostic_code,
    };
}

constexpr TrustedReceipt make_transport_disconnected_receipt(
    const std::uint32_t session_id,
    const std::uint32_t generation,
    const std::uint32_t diagnostic_code = 0) noexcept
{
    return {
        TrustedReceiptKind::TransportDisconnected,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        session_id,
        generation,
        0,
        0,
        diagnostic_code,
    };
}

void check_exact_ingress_step_result(
    const AdapterStepResult result,
    const AdapterStepAction action,
    const RuntimeOwnerDisposition disposition,
    const RuntimeOwnerPhase phase_before,
    const RuntimeOwnerPhase phase_after,
    const std::uint32_t consumed_ingress_sequence)
{
    CHECK(result.action == action);
    CHECK(result.core_disposition == disposition);
    CHECK(result.phase_before == phase_before);
    CHECK(result.phase_after == phase_after);
    CHECK(result.consumed_ingress_sequence == consumed_ingress_sequence);
    CHECK(result.consumed_enqueue_sequence == 0);
    CHECK(result.prepared_dispatch_sequence == 0);
}

void fixture_prepare_connecting_without_pending(
    RuntimeOwnerAdapterCore &adapter)
{
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().core.phase ==
          RuntimeOwnerPhase::TransportConnecting);
    CHECK(adapter.view().pending_effect_count == 1);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch attempt = adapter.peek_dispatch();
    CHECK(attempt.kind == AdapterDispatchKind::CoreEffect);
    CHECK(attempt.effect.kind ==
          RuntimeOwnerEffectKind::StartTransportAttempt);
    CHECK(adapter.acknowledge_dispatch(attempt.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    CHECK(adapter.view().pending_effect_count == 0);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, attempt));
}

void test_owner_trigger_precedes_ordinary_transport_head_and_pending_blocks_it()
{
    RuntimeOwnerAdapterCore adapter{};
    constexpr TrustedReceipt receipt =
        make_transport_established_receipt(77, 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::TransportConnecting);
    const RuntimeOwnerAdapterPrivateSnapshot after_begin =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after_begin.pending_effect_count == 1);
    CHECK(after_begin.trusted_count == 1);
    CHECK(after_begin.trusted_head == 0);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.prepared_dispatch_sequence == 1);
    CHECK(adapter.view().trusted_depth == 1);
    CHECK(adapter.acknowledge_dispatch(1) ==
          DispatchAckResult::AcceptedOperationInflight);
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        1);
}

void test_transport_established_head_applies_once_updates_signature_and_dequeues()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    constexpr TrustedReceipt receipt =
        make_transport_established_receipt(77, 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.trusted_count == 1);
    CHECK(before.last_trusted_receipt_signature.ingress_sequence == 0);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        1);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.phase == RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(after.core.mqtt_session_id == 77);
    CHECK(after.core.mqtt_generation == 1);
    CHECK(after.core.mqtt_generation_counter == 1);
    CHECK(after.core.config_apply_epoch_counter == 0);
    CHECK(after.core.last_config_commit_sequence == 0);
    CHECK(after.core.last_correlation_id == 0);
    CHECK(!after.core.active_attempt.valid());
    CHECK(!after.core.boot_orchestration_ended);
    CHECK(after.core.last_fault == RuntimeOwnerFaultCode::None);
    CHECK(after.trusted_head == 1);
    CHECK(after.trusted_tail == 1);
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_high_water == 1);
    CHECK(trusted_slot_snapshots_equal(
        after.trusted_slots[0], RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    CHECK(after.last_trusted_receipt_signature.ingress_sequence == 1);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.pending_effect_count == 0);
    CHECK(after.last_dispatch_sequence == 1);

    const RuntimeOwnerAdapterPrivateSnapshot repeated_before = after;
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Idle,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(private_snapshots_equal(
        repeated_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void check_discarded_trusted_head_is_only_dequeue_mutation(
    RuntimeOwnerAdapterCore &adapter,
    const RuntimeOwnerPhase expected_phase)
{
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t stale_before =
        adapter.view().trusted_stale_count;
    CHECK(before.trusted_count == 1);
    const std::uint8_t consumed_head = before.trusted_head;
    const std::uint32_t consumed_ingress =
        before.trusted_slots[consumed_head].ingress_sequence;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::TrustedReceiptDiscarded,
        RuntimeOwnerDisposition::Rejected,
        expected_phase,
        expected_phase,
        consumed_ingress);

    RuntimeOwnerAdapterPrivateSnapshot expected = before;
    expected.trusted_slots[consumed_head] = {};
    expected.trusted_head = static_cast<std::uint8_t>(
        (consumed_head + 1) % expected.trusted_slots.size());
    --expected.trusted_count;
    expected.trusted_stale_count = stale_before + 1;
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(private_snapshots_equal(expected, after));
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(adapter.view().trusted_stale_count == stale_before + 1);
    CHECK(adapter.view().trusted_duplicate_count == 0);
}

void test_transport_established_wrong_generation_or_phase_discards_without_core_submit()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_connecting_without_pending(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_established_receipt(77, 2)) ==
              TrustedEnqueueResult::Accepted);
        check_discarded_trusted_head_is_only_dequeue_mutation(
            adapter, RuntimeOwnerPhase::TransportConnecting);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_established_receipt(77, 1)) ==
              TrustedEnqueueResult::Accepted);
        check_discarded_trusted_head_is_only_dequeue_mutation(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit);
    }
}

void test_task5_transport_receipt_requires_exact_acked_physical_attempt()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch attempt = adapter.peek_dispatch();
    CHECK(attempt.kind == AdapterDispatchKind::CoreEffect);
    CHECK(attempt.effect.kind ==
          RuntimeOwnerEffectKind::StartTransportAttempt);
    CHECK(adapter.acknowledge_dispatch(attempt.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_established_receipt(77, 2)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.view().trusted_stale_count == 1);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, attempt));
    CHECK(adapter.view().core.phase ==
          RuntimeOwnerPhase::TransportConnecting);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_established_receipt(77, 1)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().core.phase ==
          RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(adapter.view().physical_inflight.kind ==
          AdapterDispatchKind::None);
}

void test_transport_head_defers_to_shutdown_without_dequeue_or_signature_update()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, make_transport_established_receipt(77, 1)) ==
          TrustedEnqueueResult::Accepted);
    auto shutdown = adapter.shutdown_port();
    CHECK(shutdown.request() == UrgentRequestResult::Accepted);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(adapter.view().trusted_depth == 0);
    CHECK(adapter.view().trusted_cancelled_count == 1);
    CHECK(adapter.step().action == AdapterStepAction::Terminal);
}

constexpr TrustedReceipt make_config_committed_receipt(
    const std::uint32_t session_id,
    const std::uint32_t generation,
    const std::uint32_t config_commit_sequence) noexcept
{
    return {
        TrustedReceiptKind::ConfigCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        session_id,
        generation,
        config_commit_sequence,
        0,
        0,
    };
}

constexpr TrustedReceipt make_operation_completed_receipt(
    const RuntimeOwnerEffect ticket) noexcept
{
    return {
        TrustedReceiptKind::OperationCompleted,
        ticket.kind,
        0,
        ticket.correlation_id,
        ticket.attempt.mqtt_session_id,
        ticket.attempt.mqtt_generation,
        0,
        ticket.attempt.config_apply_epoch,
        0,
    };
}

constexpr TrustedReceipt make_liveness_failure_receipt(
    const TrustedReceiptKind kind,
    const RuntimeOwnerEffect ticket,
    const std::uint32_t diagnostic_code = 0) noexcept
{
    return {
        kind,
        ticket.kind,
        0,
        ticket.correlation_id,
        ticket.attempt.mqtt_session_id,
        ticket.attempt.mqtt_generation,
        0,
        ticket.attempt.config_apply_epoch,
        diagnostic_code,
    };
}

constexpr TrustedReceipt make_snapshot_receipt(
    const TrustedReceiptKind kind,
    const RuntimeOwnerEffect freeze,
    const std::uint32_t diagnostic_code = 0) noexcept
{
    return {
        kind,
        RuntimeOwnerEffectKind::FreezeBootSnapshot,
        0,
        freeze.correlation_id,
        freeze.attempt.mqtt_session_id,
        freeze.attempt.mqtt_generation,
        0,
        freeze.attempt.config_apply_epoch,
        diagnostic_code,
    };
}

void fixture_prepare_awaiting_config_via_trusted(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t session_id)
{
    fixture_prepare_connecting_without_pending(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_established_receipt(session_id, 1)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().core.phase ==
          RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(adapter.view().trusted_depth == 0);
    CHECK(adapter.view().pending_effect_count == 0);
}

std::array<RuntimeOwnerEffect, 4>
fixture_prepare_liveness_waiting_via_config(
    RuntimeOwnerAdapterCore &adapter,
    const bool keep_pending_effects = false)
{
    fixture_prepare_awaiting_config_via_trusted(adapter, 77);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    const RuntimeOwnerAdapterPrivateSnapshot state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.core.phase == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(state.pending_effect_count == 4);

    std::array<RuntimeOwnerEffect, 4> tickets{};
    for (std::size_t index = 0; index < tickets.size(); ++index) {
        const std::size_t slot =
            (state.pending_effect_head + index) % tickets.size();
        tickets[index] = state.pending_effect_slots[slot].effect;
    }
    if (!keep_pending_effects) {
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(runtime_owner_effects_equal(offered.effect, tickets[0]));
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
    }
    return tickets;
}

void check_operation_completed_accepts(
    RuntimeOwnerAdapterCore &adapter,
    RuntimeOwnerEffect ticket,
    bool final_ticket);

void fixture_advance_to_acked_liveness_ticket(
    RuntimeOwnerAdapterCore &adapter,
    const std::array<RuntimeOwnerEffect, 4> &tickets,
    const std::size_t target_index)
{
    CHECK(target_index < tickets.size());
    for (std::size_t index = 0; index < target_index; ++index) {
        const std::uint8_t ticket_mask =
            static_cast<std::uint8_t>(1u << index);
        if ((RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
                 .accepted_liveness_mask & ticket_mask) == 0) {
            check_operation_completed_accepts(
                adapter, tickets[index], false);
        }
    }
    if (adapter.view().physical_inflight.kind ==
        AdapterDispatchKind::None) {
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(runtime_owner_effects_equal(
            offered.effect, tickets[target_index]));
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
    }
    CHECK(runtime_owner_effects_equal(
        adapter.view().physical_inflight.effect,
        tickets[target_index]));
}

void check_canonical_config_pending_bundle(
    const RuntimeOwnerAdapterPrivateSnapshot &state,
    const std::uint32_t first_dispatch_sequence,
    const std::uint32_t first_correlation_id,
    const LivenessAttemptToken expected_attempt)
{
    constexpr std::array<RuntimeOwnerEffectKind, 4> expected_kinds{{
        RuntimeOwnerEffectKind::StartAtProbe,
        RuntimeOwnerEffectKind::StartProbePublish,
        RuntimeOwnerEffectKind::VerifySubscription,
        RuntimeOwnerEffectKind::PullFollowupConfig,
    }};
    CHECK(state.pending_effect_count == 4);
    for (std::size_t index = 0;
        index < state.pending_effect_slots.size(); ++index) {
        const std::size_t slot_index =
            (state.pending_effect_head + index) %
            state.pending_effect_slots.size();
        const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot =
            state.pending_effect_slots[slot_index];
        CHECK(slot.preassigned_dispatch_sequence ==
              first_dispatch_sequence + index);
        CHECK(slot.effect.kind == expected_kinds[index]);
        CHECK(slot.effect.correlation_id == first_correlation_id + index);
        CHECK(slot.effect.attempt == expected_attempt);
        CHECK(slot.effect.fault_code == RuntimeOwnerFaultCode::None);
    }
}

void check_canonical_counter_saturation_pending_pair(
    const RuntimeOwnerAdapterPrivateSnapshot &state,
    const std::uint32_t record_fault_dispatch_sequence,
    const std::uint32_t enter_recovery_dispatch_sequence)
{
    CHECK(state.pending_effect_count == 2);
    CHECK(state.pending_effect_tail ==
          (state.pending_effect_head + 2) %
              state.pending_effect_slots.size());

    const RuntimeOwnerAdapterPendingEffectSlotSnapshot record_fault =
        state.pending_effect_slots[state.pending_effect_head];
    CHECK(record_fault.preassigned_dispatch_sequence ==
          record_fault_dispatch_sequence);
    CHECK(record_fault.effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(record_fault.effect.correlation_id == 0);
    CHECK(record_fault.effect.attempt == LivenessAttemptToken{});
    CHECK(record_fault.effect.fault_code ==
          RuntimeOwnerFaultCode::CounterSaturation);

    const RuntimeOwnerAdapterPendingEffectSlotSnapshot enter_recovery =
        state.pending_effect_slots[
            (state.pending_effect_head + 1) %
            state.pending_effect_slots.size()];
    CHECK(enter_recovery.preassigned_dispatch_sequence ==
          enter_recovery_dispatch_sequence);
    CHECK(enter_recovery.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
    CHECK(enter_recovery.effect.correlation_id == 0);
    CHECK(enter_recovery.effect.attempt == LivenessAttemptToken{});
    CHECK(enter_recovery.effect.fault_code ==
          RuntimeOwnerFaultCode::CounterSaturation);

    for (std::size_t offset = 2;
         offset < state.pending_effect_slots.size(); ++offset) {
        const std::size_t slot =
            (state.pending_effect_head + offset) %
            state.pending_effect_slots.size();
        CHECK(pending_effect_slot_snapshots_equal(
            state.pending_effect_slots[slot],
            RuntimeOwnerAdapterPendingEffectSlotSnapshot{}));
    }
}

void check_canonical_recovery_pending_pair(
    const RuntimeOwnerAdapterPrivateSnapshot &state,
    const std::uint32_t record_fault_dispatch_sequence,
    const std::uint32_t enter_recovery_dispatch_sequence,
    const RuntimeOwnerFaultCode fault,
    const std::uint32_t source_correlation,
    const LivenessAttemptToken source_attempt)
{
    CHECK(state.pending_effect_count == 2);
    CHECK(state.pending_effect_tail ==
          (state.pending_effect_head + 2) %
              state.pending_effect_slots.size());

    const RuntimeOwnerAdapterPendingEffectSlotSnapshot record_fault =
        state.pending_effect_slots[state.pending_effect_head];
    CHECK(record_fault.preassigned_dispatch_sequence ==
          record_fault_dispatch_sequence);
    CHECK(record_fault.effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(record_fault.effect.correlation_id == source_correlation);
    CHECK(record_fault.effect.attempt == source_attempt);
    CHECK(record_fault.effect.fault_code == fault);

    const RuntimeOwnerAdapterPendingEffectSlotSnapshot enter_recovery =
        state.pending_effect_slots[
            (state.pending_effect_head + 1) %
            state.pending_effect_slots.size()];
    CHECK(enter_recovery.preassigned_dispatch_sequence ==
          enter_recovery_dispatch_sequence);
    CHECK(enter_recovery.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
    CHECK(enter_recovery.effect.correlation_id == source_correlation);
    CHECK(enter_recovery.effect.attempt == source_attempt);
    CHECK(enter_recovery.effect.fault_code == fault);

    for (std::size_t offset = 2;
         offset < state.pending_effect_slots.size(); ++offset) {
        const std::size_t slot =
            (state.pending_effect_head + offset) %
            state.pending_effect_slots.size();
        CHECK(pending_effect_slot_snapshots_equal(
            state.pending_effect_slots[slot],
            RuntimeOwnerAdapterPendingEffectSlotSnapshot{}));
    }
}

void check_malformed_fatal_safety_then_terminal(
    RuntimeOwnerAdapterCore &adapter,
    const RuntimeOwnerPhase phase,
    const bool safety_pair_expected,
    const std::uint32_t expected_core_submit_count)
{
    if (safety_pair_expected) {
        for (const RuntimeOwnerEffectKind expected_kind : {
                 RuntimeOwnerEffectKind::RecordFault,
                 RuntimeOwnerEffectKind::EnterRecovery,
             }) {
            const AdapterStepResult prepared = adapter.step();
            CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
            CHECK(prepared.core_disposition ==
                  RuntimeOwnerDisposition::Rejected);
            CHECK(prepared.phase_before == phase);
            CHECK(prepared.phase_after == phase);
            CHECK(prepared.consumed_ingress_sequence == 0);
            CHECK(prepared.consumed_enqueue_sequence == 0);
            const AdapterDispatch offered = adapter.peek_dispatch();
            CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
            CHECK(offered.effect.kind == expected_kind);
            CHECK(prepared.prepared_dispatch_sequence ==
                  offered.dispatch_sequence);
            CHECK(adapter.acknowledge_dispatch(
                      offered.dispatch_sequence) ==
                  DispatchAckResult::AcceptedDelivery);
        }
    } else {
        CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
        CHECK(adapter.view().pending_effect_count == 0);
    }

    const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        phase,
        phase);
    CHECK(private_snapshots_equal(
        terminal_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == expected_core_submit_count);

    const RuntimeOwnerAdapterPrivateSnapshot repeated_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        phase,
        phase);
    CHECK(private_snapshots_equal(
        repeated_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == expected_core_submit_count);
}

RuntimeOwnerTransition make_canonical_begin_transition_override()
{
    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = RuntimeOwnerPhase::ColdStart;
    transition.phase_after = RuntimeOwnerPhase::TransportConnecting;
    transition.effect_count = 1;
    transition.effects[0] = {
        RuntimeOwnerEffectKind::StartTransportAttempt,
        0,
        {0, 1, 0},
        RuntimeOwnerFaultCode::None,
    };
    return transition;
}

RuntimeOwnerView make_canonical_begin_post_submit_view_override()
{
    RuntimeOwnerView view{};
    view.phase = RuntimeOwnerPhase::TransportConnecting;
    view.mqtt_generation_counter = 1;
    return view;
}

void check_c1a_begin_malformed_fallback(
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 0,
    const bool terminal_reserve = false,
    const bool safety_blocked = false,
    const bool override_post_submit_view = false,
    const RuntimeOwnerView post_submit_view = {},
    const RuntimeOwnerPhase expected_result_phase =
        RuntimeOwnerPhase::TransportConnecting,
    const bool suppress_synthetic_pair = false)
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, initial_dispatch_sequence);
    RuntimeOwnerAdapterCoreTestPeer::fixture_seed_begin_fallback_cleanup_state(
        adapter);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);
    if (override_post_submit_view) {
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter, post_submit_view);
    }
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::ColdStart);
    CHECK(before.transport_request_pending);
    CHECK(before.trusted_count == 1);
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count == 1);
    CHECK(before.accepted_liveness_mask == 0x05);
    CHECK(before.normal_slots[0].intent.kind ==
          NormalIntentKind::PublishTelemetry);
    CHECK(before.trusted_slots[0].payload_kind == 1);
    CHECK(before.pending_effect_slots[0].effect.kind ==
          RuntimeOwnerEffectKind::StartAtProbe);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == 0);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter) ==
          override_post_submit_view);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::ColdStart,
        expected_result_phase);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerView expected_core = before.core;
    expected_core.phase = RuntimeOwnerPhase::TransportConnecting;
    expected_core.mqtt_generation_counter = 1;
    CHECK(runtime_owner_views_equal(after.core, expected_core));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == 1);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
    CHECK(!after.transport_request_pending);
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.safety_delivery_blocked == safety_blocked);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.reason_mask == (1u << 6u));
    CHECK(after.critical.first_ingress_sequence == 0);
    CHECK(after.critical.last_ingress_sequence == 0);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    for (const RuntimeOwnerAdapterNormalSlotSnapshot slot :
         after.normal_slots) {
        CHECK(normal_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterNormalSlotSnapshot{}));
    }

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    if (suppress_synthetic_pair) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(!after.safety_delivery_blocked);
    } else if (safety_blocked) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
    } else {
        const std::uint32_t record_sequence = terminal_reserve
            ? maximum - 1
            : initial_dispatch_sequence + 1;
        const std::uint32_t recovery_sequence = terminal_reserve
            ? maximum
            : initial_dispatch_sequence + 2;
        CHECK(after.last_dispatch_sequence == recovery_sequence);
        check_canonical_recovery_pending_pair(
            after,
            record_sequence,
            recovery_sequence,
            RuntimeOwnerFaultCode::InternalInvariant,
            0,
            {});
        CHECK(after.dispatch_fatal_latched == terminal_reserve);
    }

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::TransportConnecting,
        !suppress_synthetic_pair && !safety_blocked,
        1);
}

void test_c1a_begin_unknown_and_unexpected_dispositions_fail_closed()
{
    constexpr std::array<RuntimeOwnerDisposition, 4> corruptions{{
        static_cast<RuntimeOwnerDisposition>(255),
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_canonical_begin_transition_override();
        malformed.disposition = disposition;
        check_c1a_begin_malformed_fallback(malformed);
    }
}

void test_c1a_begin_phase_before_corruptions_fail_closed()
{
    constexpr std::array<RuntimeOwnerPhase, 2> corruptions{{
        RuntimeOwnerPhase::RecoveryPending,
        static_cast<RuntimeOwnerPhase>(255),
    }};
    for (const RuntimeOwnerPhase phase : corruptions) {
        RuntimeOwnerTransition malformed =
            make_canonical_begin_transition_override();
        malformed.phase_before = phase;
        check_c1a_begin_malformed_fallback(malformed);
    }
}

void test_c1a_begin_phase_after_corruptions_fail_closed()
{
    constexpr std::array<RuntimeOwnerPhase, 3> corruptions{{
        RuntimeOwnerPhase::ColdStart,
        static_cast<RuntimeOwnerPhase>(255),
        RuntimeOwnerPhase::AwaitingConfigCommit,
    }};
    for (const RuntimeOwnerPhase phase : corruptions) {
        RuntimeOwnerTransition malformed =
            make_canonical_begin_transition_override();
        malformed.phase_after = phase;
        check_c1a_begin_malformed_fallback(malformed);
    }
}

void test_c1a_begin_known_wrong_post_view_phase_fails_closed()
{
    RuntimeOwnerView post_submit_view =
        make_canonical_begin_post_submit_view_override();
    post_submit_view.phase = RuntimeOwnerPhase::AwaitingConfigCommit;
    check_c1a_begin_malformed_fallback(
        make_canonical_begin_transition_override(),
        0,
        false,
        false,
        true,
        post_submit_view,
        RuntimeOwnerPhase::AwaitingConfigCommit);
}

void test_c1a_begin_unknown_post_view_phase_is_normalized_to_before()
{
    RuntimeOwnerView post_submit_view =
        make_canonical_begin_post_submit_view_override();
    post_submit_view.phase = static_cast<RuntimeOwnerPhase>(255);
    check_c1a_begin_malformed_fallback(
        make_canonical_begin_transition_override(),
        0,
        false,
        false,
        true,
        post_submit_view,
        RuntimeOwnerPhase::ColdStart);
}

void test_c1a_begin_shutdown_post_view_suppresses_synthetic_pair()
{
    RuntimeOwnerView post_submit_view =
        make_canonical_begin_post_submit_view_override();
    post_submit_view.phase = RuntimeOwnerPhase::ShutdownCommitted;
    check_c1a_begin_malformed_fallback(
        make_canonical_begin_transition_override(),
        0,
        false,
        false,
        true,
        post_submit_view,
        RuntimeOwnerPhase::ShutdownCommitted,
        true);
}

void test_c1a_begin_post_view_fields_fail_closed_independently()
{
    std::array<RuntimeOwnerView, 11> corruptions{};
    for (RuntimeOwnerView &view : corruptions) {
        view = make_canonical_begin_post_submit_view_override();
    }
    corruptions[0].mqtt_session_id = 1;
    corruptions[1].mqtt_generation = 1;
    corruptions[2].mqtt_generation_counter = 2;
    corruptions[3].config_apply_epoch_counter = 1;
    corruptions[4].last_config_commit_sequence = 1;
    corruptions[5].last_correlation_id = 1;
    corruptions[6].active_attempt.mqtt_session_id = 1;
    corruptions[7].active_attempt.mqtt_generation = 1;
    corruptions[8].active_attempt.config_apply_epoch = 1;
    corruptions[9].boot_orchestration_ended = true;
    corruptions[10].last_fault =
        RuntimeOwnerFaultCode::InternalInvariant;

    for (const RuntimeOwnerView post_submit_view : corruptions) {
        check_c1a_begin_malformed_fallback(
            make_canonical_begin_transition_override(),
            0,
            false,
            false,
            true,
            post_submit_view);
    }
}

void test_c1a_begin_effect_count_corruptions_fail_closed()
{
    constexpr std::array<std::uint8_t, 3> corruptions{{5, 0, 2}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_canonical_begin_transition_override();
        malformed.effect_count = effect_count;
        check_c1a_begin_malformed_fallback(malformed);
    }
}

void test_c1a_begin_used_effect_field_corruptions_fail_closed()
{
    std::array<RuntimeOwnerTransition, 7> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_canonical_begin_transition_override();
    }
    corruptions[0].effects[0].kind = RuntimeOwnerEffectKind::RecordFault;
    corruptions[1].effects[0].correlation_id = 1;
    corruptions[2].effects[0].attempt.mqtt_session_id = 1;
    corruptions[3].effects[0].attempt.mqtt_generation = 2;
    corruptions[4].effects[0].attempt.config_apply_epoch = 1;
    corruptions[5].effects[0].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    corruptions[6].effects[0] = {};
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1a_begin_malformed_fallback(malformed);
    }
}

void test_c1a_begin_last_unused_effect_nonzero_fails_closed()
{
    std::array<RuntimeOwnerTransition, 6> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_canonical_begin_transition_override();
    }
    corruptions[0].effects[3].kind =
        RuntimeOwnerEffectKind::EnterRecovery;
    corruptions[1].effects[3].correlation_id = 99;
    corruptions[2].effects[3].attempt.mqtt_session_id = 1;
    corruptions[3].effects[3].attempt.mqtt_generation = 2;
    corruptions[4].effects[3].attempt.config_apply_epoch = 3;
    corruptions[5].effects[3].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1a_begin_malformed_fallback(malformed);
    }
}

void test_c1a_begin_malformed_cleanup_purges_seeded_state()
{
    RuntimeOwnerTransition malformed =
        make_canonical_begin_transition_override();
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1a_begin_malformed_fallback(malformed);
}

void test_c1a_begin_malformed_sequence_regular_and_terminal_reserve()
{
    RuntimeOwnerTransition malformed =
        make_canonical_begin_transition_override();
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1a_begin_malformed_fallback(malformed, 41, false, false);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        maximum - 4,
        maximum - 3,
        maximum - 2,
    }};
    for (const std::uint32_t start : terminal_starts) {
        check_c1a_begin_malformed_fallback(
            malformed, start, true, false);
    }
}

void test_c1a_begin_malformed_damaged_reserve_blocks_without_retry()
{
    RuntimeOwnerTransition malformed =
        make_canonical_begin_transition_override();
    malformed.effects[3].kind = RuntimeOwnerEffectKind::RecordFault;
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 2> damaged_starts{{
        maximum - 1,
        maximum,
    }};
    for (const std::uint32_t start : damaged_starts) {
        check_c1a_begin_malformed_fallback(
            malformed, start, false, true);
    }
}

void test_c1a_review_begin_canonical_override_pending_bypass_fails_closed()
{
    check_c1a_begin_malformed_fallback(
        make_canonical_begin_transition_override());
}

void test_c1a_review_begin_canonical_override_max_sequence_bypass_fails_closed()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, maximum);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, make_canonical_begin_transition_override());

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::TransportConnecting);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(
        after.core, make_canonical_begin_post_submit_view_override()));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!after.transport_request_pending);
    CHECK(after.trusted_count == 0);
    CHECK(after.normal_count == 0);
    CHECK(after.pending_effect_count == 0);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 0);
    check_unused_pending_effect_slots_are_zero(after, 0);
    CHECK(after.last_dispatch_sequence == maximum);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.safety_delivery_blocked);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.first_ingress_sequence == 0);
    CHECK(after.critical.last_ingress_sequence == 0);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::TransportConnecting,
        false,
        submit_count_before + 1);
}

void test_c1a_begin_valid_override_remains_unaffected()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, make_canonical_begin_transition_override());
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter, make_canonical_begin_post_submit_view_override());
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::TransportConnecting);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.pending_effect_count == 1);
    CHECK(after.last_dispatch_sequence == 1);
    check_canonical_begin_pending_effect(
        after.pending_effect_slots[0], 1, 1);
    CHECK(!after.core_adapter_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == 1);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
}

void test_c1a_unarmed_pending_effect_preserves_existing_deferral()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_seed_begin_fallback_cleanup_state(
        adapter);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(prepared.phase_before == RuntimeOwnerPhase::ColdStart);
    CHECK(prepared.phase_after == RuntimeOwnerPhase::ColdStart);
    CHECK(prepared.prepared_dispatch_sequence ==
          before.pending_effect_slots[before.pending_effect_head]
              .preassigned_dispatch_sequence);
    CHECK(adapter.view().transport_request_pending == 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == 0);
}

void test_c1a_unarmed_begin_shortage_preserves_existing_preflight()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::uint32_t start =
        std::numeric_limits<std::uint32_t>::max() - 2;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, start);
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CriticalLedgerHandled,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::ColdStart);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == 0);
    CHECK(adapter.view().transport_request_pending == 1);
    CHECK(adapter.view().last_dispatch_sequence == start);
    CHECK(adapter.view().critical.first_reason ==
          AdapterCriticalReason::DispatchSequenceSaturation);
}

RuntimeOwnerTransition
make_canonical_transport_established_transition_override()
{
    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = RuntimeOwnerPhase::TransportConnecting;
    transition.phase_after = RuntimeOwnerPhase::AwaitingConfigCommit;
    return transition;
}

RuntimeOwnerView
make_canonical_transport_established_post_submit_view_override()
{
    RuntimeOwnerView view{};
    view.phase = RuntimeOwnerPhase::AwaitingConfigCommit;
    view.mqtt_session_id = 77;
    view.mqtt_generation = 1;
    view.mqtt_generation_counter = 1;
    return view;
}

void check_c1b1_transport_established_malformed_fallback(
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41,
    const bool terminal_reserve = false,
    const bool safety_blocked = false,
    const bool override_post_submit_view = false,
    const RuntimeOwnerView post_submit_view = {},
    const RuntimeOwnerPhase expected_result_phase =
        RuntimeOwnerPhase::AwaitingConfigCommit,
    const bool suppress_synthetic_pair = false)
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, initial_dispatch_sequence);
    constexpr TrustedReceipt head =
        make_transport_established_receipt(77, 1);
    constexpr TrustedReceipt trailing =
        make_transport_attempt_failed_receipt(1, 91);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, head) == TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, trailing) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);
    if (override_post_submit_view) {
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter, post_submit_view);
    }
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_before = adapter.view();
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(before.trusted_count == 2);
    CHECK(before.trusted_slots[before.trusted_head].ingress_sequence == 1);
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count == 1);
    CHECK(before.accepted_liveness_mask == 0x0a);
    CHECK(before.last_trusted_receipt_signature.ingress_sequence == 67);
    CHECK(before.last_trusted_diagnostic_ingress_sequence == 2);
    CHECK(before.last_trusted_diagnostic_code == 91);
    CHECK(public_before.trusted_stale_count == 73);
    CHECK(public_before.trusted_duplicate_count == 79);
    CHECK(public_before.trusted_protocol_violation_count == 83);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter) ==
          override_post_submit_view);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::TransportConnecting,
        expected_result_phase,
        1);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_after = adapter.view();
    RuntimeOwnerView expected_core = before.core;
    expected_core.phase = RuntimeOwnerPhase::AwaitingConfigCommit;
    expected_core.mqtt_session_id = 77;
    expected_core.mqtt_generation = 1;
    CHECK(runtime_owner_views_equal(after.core, expected_core));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.last_trusted_diagnostic_ingress_sequence ==
          before.last_trusted_diagnostic_ingress_sequence);
    CHECK(after.last_trusted_diagnostic_code ==
          before.last_trusted_diagnostic_code);
    CHECK(public_after.trusted_stale_count ==
          public_before.trusted_stale_count);
    CHECK(public_after.trusted_duplicate_count ==
          public_before.trusted_duplicate_count);
    CHECK(public_after.trusted_protocol_violation_count ==
          public_before.trusted_protocol_violation_count);
    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    for (const RuntimeOwnerAdapterNormalSlotSnapshot slot :
         after.normal_slots) {
        CHECK(normal_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterNormalSlotSnapshot{}));
    }
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.transport_request_pending);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.first_ingress_sequence == 1);
    CHECK(after.critical.last_ingress_sequence == 1);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    if (suppress_synthetic_pair) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(!after.safety_delivery_blocked);
    } else if (safety_blocked) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(after.safety_delivery_blocked);
    } else {
        const std::uint32_t record_sequence = terminal_reserve
            ? maximum - 1
            : initial_dispatch_sequence + 1;
        const std::uint32_t recovery_sequence = terminal_reserve
            ? maximum
            : initial_dispatch_sequence + 2;
        CHECK(after.last_dispatch_sequence == recovery_sequence);
        check_canonical_recovery_pending_pair(
            after,
            record_sequence,
            recovery_sequence,
            RuntimeOwnerFaultCode::InternalInvariant,
            0,
            {});
        CHECK(after.dispatch_fatal_latched == terminal_reserve);
        CHECK(!after.safety_delivery_blocked);
    }

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        !suppress_synthetic_pair && !safety_blocked,
        submit_count_before + 1);
}

void test_c1b1_transport_established_unknown_disposition_captures_fifo_provenance()
{
    RuntimeOwnerTransition malformed =
        make_canonical_transport_established_transition_override();
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b1_transport_established_malformed_fallback(malformed);
}

void test_c1b1_transport_established_unexpected_dispositions_fail_closed()
{
    constexpr std::array<RuntimeOwnerDisposition, 3> corruptions{{
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_canonical_transport_established_transition_override();
        malformed.disposition = disposition;
        check_c1b1_transport_established_malformed_fallback(malformed);
    }
}

void test_c1b1_transport_established_transition_phases_fail_closed()
{
    std::array<RuntimeOwnerTransition, 4> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition =
            make_canonical_transport_established_transition_override();
    }
    corruptions[0].phase_before = RuntimeOwnerPhase::ColdStart;
    corruptions[1].phase_before = static_cast<RuntimeOwnerPhase>(255);
    corruptions[2].phase_after = RuntimeOwnerPhase::RuntimeReady;
    corruptions[3].phase_after = static_cast<RuntimeOwnerPhase>(255);
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b1_transport_established_malformed_fallback(malformed);
    }
}

void test_c1b1_transport_established_effect_count_fail_closed()
{
    constexpr std::array<std::uint8_t, 2> corruptions{{1, 5}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_canonical_transport_established_transition_override();
        malformed.effect_count = effect_count;
        check_c1b1_transport_established_malformed_fallback(malformed);
    }
}

void test_c1b1_transport_established_used_effect_fields_fail_closed()
{
    std::array<RuntimeOwnerTransition, 6> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition =
            make_canonical_transport_established_transition_override();
        transition.effect_count = 1;
    }
    corruptions[0].effects[0].kind = RuntimeOwnerEffectKind::RecordFault;
    corruptions[1].effects[0].correlation_id = 1;
    corruptions[2].effects[0].attempt.mqtt_session_id = 1;
    corruptions[3].effects[0].attempt.mqtt_generation = 1;
    corruptions[4].effects[0].attempt.config_apply_epoch = 1;
    corruptions[5].effects[0].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b1_transport_established_malformed_fallback(malformed);
    }
}

void test_c1b1_transport_established_zero_effect_count_slot0_fields_fail_closed()
{
    std::array<RuntimeOwnerTransition, 6> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition =
            make_canonical_transport_established_transition_override();
        CHECK(transition.effect_count == 0);
    }
    corruptions[0].effects[0].kind = RuntimeOwnerEffectKind::RecordFault;
    corruptions[1].effects[0].correlation_id = 1;
    corruptions[2].effects[0].attempt.mqtt_session_id = 1;
    corruptions[3].effects[0].attempt.mqtt_generation = 1;
    corruptions[4].effects[0].attempt.config_apply_epoch = 1;
    corruptions[5].effects[0].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        CHECK(malformed.effect_count == 0);
        check_c1b1_transport_established_malformed_fallback(malformed);
    }
}

void test_c1b1_transport_established_unused_effect_fields_fail_closed()
{
    std::array<RuntimeOwnerTransition, 6> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition =
            make_canonical_transport_established_transition_override();
    }
    corruptions[0].effects[3].kind =
        RuntimeOwnerEffectKind::EnterRecovery;
    corruptions[1].effects[3].correlation_id = 1;
    corruptions[2].effects[3].attempt.mqtt_session_id = 1;
    corruptions[3].effects[3].attempt.mqtt_generation = 1;
    corruptions[4].effects[3].attempt.config_apply_epoch = 1;
    corruptions[5].effects[3].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b1_transport_established_malformed_fallback(malformed);
    }
}

void test_c1b1_transport_established_post_view_phases_fail_closed()
{
    RuntimeOwnerView known =
        make_canonical_transport_established_post_submit_view_override();
    known.phase = RuntimeOwnerPhase::RecoveryPending;
    check_c1b1_transport_established_malformed_fallback(
        make_canonical_transport_established_transition_override(),
        41,
        false,
        false,
        true,
        known,
        RuntimeOwnerPhase::RecoveryPending);

    RuntimeOwnerView unknown =
        make_canonical_transport_established_post_submit_view_override();
    unknown.phase = static_cast<RuntimeOwnerPhase>(255);
    check_c1b1_transport_established_malformed_fallback(
        make_canonical_transport_established_transition_override(),
        41,
        false,
        false,
        true,
        unknown,
        RuntimeOwnerPhase::TransportConnecting);

    RuntimeOwnerView shutdown =
        make_canonical_transport_established_post_submit_view_override();
    shutdown.phase = RuntimeOwnerPhase::ShutdownCommitted;
    check_c1b1_transport_established_malformed_fallback(
        make_canonical_transport_established_transition_override(),
        41,
        false,
        false,
        true,
        shutdown,
        RuntimeOwnerPhase::ShutdownCommitted,
        true);
}

void test_c1b1_transport_established_post_view_fields_fail_closed()
{
    std::array<RuntimeOwnerView, 11> corruptions{};
    for (RuntimeOwnerView &view : corruptions) {
        view =
            make_canonical_transport_established_post_submit_view_override();
    }
    corruptions[0].mqtt_session_id = 78;
    corruptions[1].mqtt_generation = 2;
    corruptions[2].mqtt_generation_counter = 2;
    corruptions[3].config_apply_epoch_counter = 1;
    corruptions[4].last_config_commit_sequence = 1;
    corruptions[5].last_correlation_id = 1;
    corruptions[6].active_attempt.mqtt_session_id = 1;
    corruptions[7].active_attempt.mqtt_generation = 1;
    corruptions[8].active_attempt.config_apply_epoch = 1;
    corruptions[9].boot_orchestration_ended = true;
    corruptions[10].last_fault =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerView post_submit_view : corruptions) {
        check_c1b1_transport_established_malformed_fallback(
            make_canonical_transport_established_transition_override(),
            41,
            false,
            false,
            true,
            post_submit_view);
    }
}

void test_c1b1_transport_established_sequence_reserves_and_damage()
{
    RuntimeOwnerTransition malformed =
        make_canonical_transport_established_transition_override();
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b1_transport_established_malformed_fallback(
        malformed, 41, false, false);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        maximum - 4,
        maximum - 3,
        maximum - 2,
    }};
    for (const std::uint32_t start : terminal_starts) {
        check_c1b1_transport_established_malformed_fallback(
            malformed, start, true, false);
    }
    constexpr std::array<std::uint32_t, 2> damaged_starts{{
        maximum - 1,
        maximum,
    }};
    for (const std::uint32_t start : damaged_starts) {
        check_c1b1_transport_established_malformed_fallback(
            malformed, start, false, true);
    }
}

void test_c1b1_review_transport_established_canonical_override_pending_bypass_fails_closed()
{
    RuntimeOwnerTransition malformed =
        make_canonical_transport_established_transition_override();
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b1_transport_established_malformed_fallback(
        malformed);
}

void test_c1b1_transport_established_valid_overrides_remain_unaffected()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    constexpr TrustedReceipt receipt =
        make_transport_established_receipt(77, 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_canonical_transport_established_transition_override());
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter,
            make_canonical_transport_established_post_submit_view_override());
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        1);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.trusted_count == 0);
    CHECK(after.last_trusted_receipt_signature.ingress_sequence == 1);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(!after.core_adapter_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(after.pending_effect_count == 0);
    CHECK(after.last_dispatch_sequence == 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
}

void test_c1b1_transport_established_unarmed_pending_defers()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    constexpr TrustedReceipt head =
        make_transport_established_receipt(77, 1);
    constexpr TrustedReceipt trailing =
        make_transport_attempt_failed_receipt(1, 91);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, head) == TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, trailing) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        1);
    CHECK(adapter.view().trusted_depth == 1);
    CHECK(adapter.view().pending_effect_count ==
          before.pending_effect_count);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
}

void test_c1b1_override_does_not_bypass_unrelated_trusted_pending_gate()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    constexpr TrustedReceipt receipt =
        make_transport_attempt_failed_receipt(2, 91);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_canonical_transport_established_transition_override());
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::TrustedReceiptDiscarded,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::TransportConnecting,
        before.last_trusted_ingress_sequence);
    CHECK(adapter.view().trusted_depth == 0);
    CHECK(adapter.view().trusted_stale_count ==
          before.trusted_stale_count + 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
}

void test_c1b1_stale_transport_established_override_does_not_bypass_pending_gate()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
        adapter, RuntimeOwnerPhase::TransportConnecting));
    constexpr TrustedReceipt stale =
        make_transport_established_receipt(77, 2);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, stale) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_canonical_transport_established_transition_override());
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(submit_count_before == 0);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Idle,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::TransportConnecting);

    CHECK(private_snapshots_equal(
        before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == 0);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
}

enum class C1b2ConfigTransitionSource : std::uint8_t {
    AcceptedBundle = 0,
    CounterSaturation = 1,
};

enum class C1b2ConfigMalformedExercise : std::uint8_t {
    Validator = 0,
    IntentionalPendingBypass = 1,
};

constexpr RuntimeOwnerPhase c1b2_config_source_phase_after(
    const C1b2ConfigTransitionSource source) noexcept
{
    return source == C1b2ConfigTransitionSource::AcceptedBundle
        ? RuntimeOwnerPhase::LivenessWaiting
        : RuntimeOwnerPhase::RecoveryPending;
}

RuntimeOwnerTransition make_c1b2_canonical_config_transition_override(
    const C1b2ConfigTransitionSource source)
{
    RuntimeOwnerTransition transition{};
    transition.phase_before = RuntimeOwnerPhase::AwaitingConfigCommit;
    if (source == C1b2ConfigTransitionSource::AcceptedBundle) {
        transition.disposition = RuntimeOwnerDisposition::Accepted;
        transition.phase_after = RuntimeOwnerPhase::LivenessWaiting;
        transition.effect_count = 4;
        constexpr std::array<RuntimeOwnerEffectKind, 4> kinds{{
            RuntimeOwnerEffectKind::StartAtProbe,
            RuntimeOwnerEffectKind::StartProbePublish,
            RuntimeOwnerEffectKind::VerifySubscription,
            RuntimeOwnerEffectKind::PullFollowupConfig,
        }};
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            transition.effects[index] = {
                kinds[index],
                static_cast<std::uint32_t>(index + 1),
                {77, 1, 1},
                RuntimeOwnerFaultCode::None,
            };
        }
        return transition;
    }

    transition.disposition = RuntimeOwnerDisposition::FailClosed;
    transition.phase_after = RuntimeOwnerPhase::RecoveryPending;
    transition.effect_count = 2;
    transition.effects[0] = {
        RuntimeOwnerEffectKind::RecordFault,
        0,
        {},
        RuntimeOwnerFaultCode::CounterSaturation,
    };
    transition.effects[1] = {
        RuntimeOwnerEffectKind::EnterRecovery,
        0,
        {},
        RuntimeOwnerFaultCode::CounterSaturation,
    };
    return transition;
}

RuntimeOwnerView make_c1b2_canonical_config_post_submit_view_override(
    const C1b2ConfigTransitionSource source)
{
    RuntimeOwnerView view{};
    view.mqtt_generation_counter = 1;
    if (source == C1b2ConfigTransitionSource::AcceptedBundle) {
        view.phase = RuntimeOwnerPhase::LivenessWaiting;
        view.mqtt_session_id = 77;
        view.mqtt_generation = 1;
        view.config_apply_epoch_counter = 1;
        view.last_config_commit_sequence = 9;
        view.last_correlation_id = 6;
        view.active_attempt = {77, 1, 1};
        return view;
    }

    view.phase = RuntimeOwnerPhase::RecoveryPending;
    view.config_apply_epoch_counter =
        std::numeric_limits<std::uint32_t>::max();
    view.last_fault = RuntimeOwnerFaultCode::CounterSaturation;
    return view;
}

void fixture_prepare_c1b2_config_source(
    RuntimeOwnerAdapterCore &adapter,
    const C1b2ConfigTransitionSource source)
{
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
        adapter,
        source == C1b2ConfigTransitionSource::CounterSaturation
            ? std::numeric_limits<std::uint32_t>::max()
            : 0,
        0);
}

void check_c1b2_config_malformed_fallback(
    const C1b2ConfigTransitionSource source,
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41,
    const bool terminal_reserve = false,
    const bool safety_blocked = false,
    const bool override_post_submit_view = false,
    const RuntimeOwnerView post_submit_view = {},
    const RuntimeOwnerPhase expected_result_phase =
        static_cast<RuntimeOwnerPhase>(255),
    const bool suppress_synthetic_pair = false,
    const C1b2ConfigMalformedExercise exercise =
        C1b2ConfigMalformedExercise::Validator)
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_c1b2_config_source(adapter, source);
    constexpr TrustedReceipt head =
        make_config_committed_receipt(77, 1, 9);
    constexpr TrustedReceipt trailing =
        make_transport_attempt_failed_receipt(1, 91);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, head) == TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, trailing) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    if (exercise == C1b2ConfigMalformedExercise::Validator) {
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
    }
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, initial_dispatch_sequence);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);
    if (override_post_submit_view) {
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter, post_submit_view);
    }

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_before = adapter.view();
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(before.trusted_count == 2);
    CHECK(before.trusted_slots[before.trusted_head].ingress_sequence == 1);
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count ==
          (exercise == C1b2ConfigMalformedExercise::Validator ? 0 : 1));
    if (exercise == C1b2ConfigMalformedExercise::Validator) {
        CHECK(initial_dispatch_sequence == 41);
    }
    CHECK(before.accepted_liveness_mask == 0x0a);
    CHECK(before.last_trusted_receipt_signature.ingress_sequence == 67);
    CHECK(before.last_trusted_diagnostic_ingress_sequence == 2);
    CHECK(before.last_trusted_diagnostic_code == 91);
    CHECK(public_before.trusted_stale_count == 73);
    CHECK(public_before.trusted_duplicate_count == 79);
    CHECK(public_before.trusted_protocol_violation_count == 83);
    CHECK(!before.core_fail_closed_latched);
    CHECK(submit_count_before == 0);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter) ==
          override_post_submit_view);

    const RuntimeOwnerPhase result_phase =
        expected_result_phase == static_cast<RuntimeOwnerPhase>(255)
            ? c1b2_config_source_phase_after(source)
            : expected_result_phase;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        result_phase,
        1);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_after = adapter.view();
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b2_canonical_config_post_submit_view_override(source)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    const bool expected_sequence_bypass =
        source == C1b2ConfigTransitionSource::AcceptedBundle
            ? initial_dispatch_sequence > maximum - 6
            : initial_dispatch_sequence >= maximum - 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_config_validation_bypass_used(adapter) ==
          expected_sequence_bypass);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));

    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.last_trusted_diagnostic_ingress_sequence ==
          before.last_trusted_diagnostic_ingress_sequence);
    CHECK(after.last_trusted_diagnostic_code ==
          before.last_trusted_diagnostic_code);
    CHECK(public_after.trusted_stale_count ==
          public_before.trusted_stale_count);
    CHECK(public_after.trusted_duplicate_count ==
          public_before.trusted_duplicate_count);
    CHECK(public_after.trusted_protocol_violation_count ==
          public_before.trusted_protocol_violation_count);
    CHECK(after.trusted_rejected_full_count ==
          before.trusted_rejected_full_count);
    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    for (const RuntimeOwnerAdapterNormalSlotSnapshot slot :
         after.normal_slots) {
        CHECK(normal_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterNormalSlotSnapshot{}));
    }
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.transport_request_pending);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.reason_mask == (1u << 6u));
    CHECK(after.critical.first_ingress_sequence == 1);
    CHECK(after.critical.last_ingress_sequence == 1);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);

    if (suppress_synthetic_pair) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(!after.safety_delivery_blocked);
    } else if (safety_blocked) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(after.safety_delivery_blocked);
    } else {
        const std::uint32_t record_sequence = terminal_reserve
            ? maximum - 1
            : initial_dispatch_sequence + 1;
        const std::uint32_t recovery_sequence = terminal_reserve
            ? maximum
            : initial_dispatch_sequence + 2;
        CHECK(after.last_dispatch_sequence == recovery_sequence);
        check_canonical_recovery_pending_pair(
            after,
            record_sequence,
            recovery_sequence,
            RuntimeOwnerFaultCode::InternalInvariant,
            0,
            {});
        CHECK(after.dispatch_fatal_latched == terminal_reserve);
        CHECK(!after.safety_delivery_blocked);
    }

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        c1b2_config_source_phase_after(source),
        !suppress_synthetic_pair && !safety_blocked,
        submit_count_before + 1);
}

void check_c1b2_config_intentional_pending_bypass_fallback(
    const C1b2ConfigTransitionSource source,
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41,
    const bool terminal_reserve = false,
    const bool safety_blocked = false)
{
    RuntimeOwnerTransition exercised = malformed;
    exercised.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b2_config_malformed_fallback(
        source,
        exercised,
        initial_dispatch_sequence,
        terminal_reserve,
        safety_blocked,
        false,
        {},
        static_cast<RuntimeOwnerPhase>(255),
        false,
        C1b2ConfigMalformedExercise::IntentionalPendingBypass);
}

void test_c1b2_config_normal_dispositions_fail_closed()
{
    constexpr std::array<RuntimeOwnerDisposition, 4> corruptions{{
        static_cast<RuntimeOwnerDisposition>(255),
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b2_canonical_config_transition_override(
                C1b2ConfigTransitionSource::AcceptedBundle);
        malformed.disposition = disposition;
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::AcceptedBundle, malformed);
    }
}

void test_c1b2_config_counter_saturation_dispositions_fail_closed()
{
    constexpr std::array<RuntimeOwnerDisposition, 4> corruptions{{
        static_cast<RuntimeOwnerDisposition>(255),
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b2_canonical_config_transition_override(
                C1b2ConfigTransitionSource::CounterSaturation);
        malformed.disposition = disposition;
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::CounterSaturation, malformed);
    }
}

void test_c1b2_config_normal_transition_phases_fail_closed()
{
    std::array<RuntimeOwnerTransition, 4> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_c1b2_canonical_config_transition_override(
            C1b2ConfigTransitionSource::AcceptedBundle);
    }
    corruptions[0].phase_before = RuntimeOwnerPhase::ColdStart;
    corruptions[1].phase_before = static_cast<RuntimeOwnerPhase>(255);
    corruptions[2].phase_after = RuntimeOwnerPhase::RuntimeReady;
    corruptions[3].phase_after = static_cast<RuntimeOwnerPhase>(255);
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::AcceptedBundle, malformed);
    }
}

void test_c1b2_config_counter_saturation_transition_phases_fail_closed()
{
    std::array<RuntimeOwnerTransition, 4> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_c1b2_canonical_config_transition_override(
            C1b2ConfigTransitionSource::CounterSaturation);
    }
    corruptions[0].phase_before = RuntimeOwnerPhase::ColdStart;
    corruptions[1].phase_before = static_cast<RuntimeOwnerPhase>(255);
    corruptions[2].phase_after = RuntimeOwnerPhase::RuntimeReady;
    corruptions[3].phase_after = static_cast<RuntimeOwnerPhase>(255);
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::CounterSaturation, malformed);
    }
}

void test_c1b2_config_normal_effect_counts_fail_closed()
{
    constexpr std::array<std::uint8_t, 3> corruptions{{0, 2, 5}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b2_canonical_config_transition_override(
                C1b2ConfigTransitionSource::AcceptedBundle);
        malformed.effect_count = effect_count;
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::AcceptedBundle, malformed);
    }
}

void test_c1b2_config_counter_saturation_effect_counts_fail_closed()
{
    constexpr std::array<std::uint8_t, 4> corruptions{{0, 1, 4, 5}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b2_canonical_config_transition_override(
                C1b2ConfigTransitionSource::CounterSaturation);
        malformed.effect_count = effect_count;
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::CounterSaturation, malformed);
    }
}

void test_c1b2_config_normal_last_used_effect_fields_fail_closed()
{
    std::array<RuntimeOwnerTransition, 6> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_c1b2_canonical_config_transition_override(
            C1b2ConfigTransitionSource::AcceptedBundle);
    }
    corruptions[0].effects[3].kind = RuntimeOwnerEffectKind::RecordFault;
    corruptions[1].effects[3].correlation_id = 99;
    corruptions[2].effects[3].attempt.mqtt_session_id = 78;
    corruptions[3].effects[3].attempt.mqtt_generation = 2;
    corruptions[4].effects[3].attempt.config_apply_epoch = 2;
    corruptions[5].effects[3].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::AcceptedBundle, malformed);
    }
}

void test_c1b2_config_counter_saturation_used_effect_fields_fail_closed()
{
    std::array<RuntimeOwnerTransition, 6> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_c1b2_canonical_config_transition_override(
            C1b2ConfigTransitionSource::CounterSaturation);
    }
    corruptions[0].effects[0].kind = RuntimeOwnerEffectKind::StartAtProbe;
    corruptions[1].effects[0].correlation_id = 1;
    corruptions[2].effects[0].attempt.mqtt_session_id = 1;
    corruptions[3].effects[0].attempt.mqtt_generation = 1;
    corruptions[4].effects[0].attempt.config_apply_epoch = 1;
    corruptions[5].effects[0].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::CounterSaturation, malformed);
    }
}

void test_c1b2_config_counter_saturation_unused_last_effect_fields_fail_closed()
{
    std::array<RuntimeOwnerTransition, 6> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_c1b2_canonical_config_transition_override(
            C1b2ConfigTransitionSource::CounterSaturation);
    }
    corruptions[0].effects[3].kind = RuntimeOwnerEffectKind::StartAtProbe;
    corruptions[1].effects[3].correlation_id = 1;
    corruptions[2].effects[3].attempt.mqtt_session_id = 1;
    corruptions[3].effects[3].attempt.mqtt_generation = 1;
    corruptions[4].effects[3].attempt.config_apply_epoch = 1;
    corruptions[5].effects[3].fault_code =
        RuntimeOwnerFaultCode::InternalInvariant;
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b2_config_malformed_fallback(
            C1b2ConfigTransitionSource::CounterSaturation, malformed);
    }
}

void check_c1b2_config_post_view_phases_fail_closed(
    const C1b2ConfigTransitionSource source)
{
    RuntimeOwnerView known =
        make_c1b2_canonical_config_post_submit_view_override(source);
    known.phase = RuntimeOwnerPhase::RuntimeReady;
    check_c1b2_config_malformed_fallback(
        source,
        make_c1b2_canonical_config_transition_override(source),
        41,
        false,
        false,
        true,
        known,
        RuntimeOwnerPhase::RuntimeReady);

    RuntimeOwnerView unknown =
        make_c1b2_canonical_config_post_submit_view_override(source);
    unknown.phase = static_cast<RuntimeOwnerPhase>(255);
    check_c1b2_config_malformed_fallback(
        source,
        make_c1b2_canonical_config_transition_override(source),
        41,
        false,
        false,
        true,
        unknown,
        RuntimeOwnerPhase::AwaitingConfigCommit);

    RuntimeOwnerView shutdown =
        make_c1b2_canonical_config_post_submit_view_override(source);
    shutdown.phase = RuntimeOwnerPhase::ShutdownCommitted;
    check_c1b2_config_malformed_fallback(
        source,
        make_c1b2_canonical_config_transition_override(source),
        41,
        false,
        false,
        true,
        shutdown,
        RuntimeOwnerPhase::ShutdownCommitted,
        true);
}

void test_c1b2_config_normal_post_view_phases_fail_closed()
{
    check_c1b2_config_post_view_phases_fail_closed(
        C1b2ConfigTransitionSource::AcceptedBundle);
}

void test_c1b2_config_counter_saturation_post_view_phases_fail_closed()
{
    check_c1b2_config_post_view_phases_fail_closed(
        C1b2ConfigTransitionSource::CounterSaturation);
}

void check_c1b2_config_post_view_fields_fail_closed(
    const C1b2ConfigTransitionSource source)
{
    std::array<RuntimeOwnerView, 11> corruptions{};
    for (RuntimeOwnerView &view : corruptions) {
        view = make_c1b2_canonical_config_post_submit_view_override(source);
    }
    if (source == C1b2ConfigTransitionSource::AcceptedBundle) {
        corruptions[0].mqtt_session_id = 78;
        corruptions[1].mqtt_generation = 2;
        corruptions[2].mqtt_generation_counter = 2;
        corruptions[3].config_apply_epoch_counter = 2;
        corruptions[4].last_config_commit_sequence = 10;
        corruptions[5].last_correlation_id = 7;
        corruptions[6].active_attempt.mqtt_session_id = 78;
        corruptions[7].active_attempt.mqtt_generation = 2;
        corruptions[8].active_attempt.config_apply_epoch = 2;
        corruptions[9].boot_orchestration_ended = true;
        corruptions[10].last_fault =
            RuntimeOwnerFaultCode::InternalInvariant;
    } else {
        corruptions[0].mqtt_session_id = 1;
        corruptions[1].mqtt_generation = 1;
        corruptions[2].mqtt_generation_counter = 2;
        corruptions[3].config_apply_epoch_counter =
            std::numeric_limits<std::uint32_t>::max() - 1;
        corruptions[4].last_config_commit_sequence = 1;
        corruptions[5].last_correlation_id = 1;
        corruptions[6].active_attempt.mqtt_session_id = 1;
        corruptions[7].active_attempt.mqtt_generation = 1;
        corruptions[8].active_attempt.config_apply_epoch = 1;
        corruptions[9].boot_orchestration_ended = true;
        corruptions[10].last_fault =
            RuntimeOwnerFaultCode::InternalInvariant;
    }
    for (const RuntimeOwnerView post_submit_view : corruptions) {
        check_c1b2_config_malformed_fallback(
            source,
            make_c1b2_canonical_config_transition_override(source),
            41,
            false,
            false,
            true,
            post_submit_view);
    }
}

void test_c1b2_config_normal_post_view_fields_fail_closed()
{
    check_c1b2_config_post_view_fields_fail_closed(
        C1b2ConfigTransitionSource::AcceptedBundle);
}

void test_c1b2_config_counter_saturation_post_view_fields_fail_closed()
{
    check_c1b2_config_post_view_fields_fail_closed(
        C1b2ConfigTransitionSource::CounterSaturation);
}

void check_c1b2_config_sequence_reserves_and_damage(
    const C1b2ConfigTransitionSource source)
{
    RuntimeOwnerTransition malformed =
        make_c1b2_canonical_config_transition_override(source);
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b2_config_intentional_pending_bypass_fallback(
        source, malformed, 41);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        maximum - 4,
        maximum - 3,
        maximum - 2,
    }};
    for (const std::uint32_t start : terminal_starts) {
        check_c1b2_config_intentional_pending_bypass_fallback(
            source, malformed, start, true, false);
    }
    constexpr std::array<std::uint32_t, 2> damaged_starts{{
        maximum - 1,
        maximum,
    }};
    for (const std::uint32_t start : damaged_starts) {
        check_c1b2_config_intentional_pending_bypass_fallback(
            source, malformed, start, false, true);
    }
}

void test_c1b2_config_normal_sequence_reserves_and_damage()
{
    check_c1b2_config_sequence_reserves_and_damage(
        C1b2ConfigTransitionSource::AcceptedBundle);
}

void test_c1b2_config_counter_saturation_sequence_reserves_and_damage()
{
    check_c1b2_config_sequence_reserves_and_damage(
        C1b2ConfigTransitionSource::CounterSaturation);
}

void test_c1b2_review_config_normal_canonical_override_pending_bypass_fails_closed()
{
    check_c1b2_config_intentional_pending_bypass_fallback(
        C1b2ConfigTransitionSource::AcceptedBundle,
        make_c1b2_canonical_config_transition_override(
            C1b2ConfigTransitionSource::AcceptedBundle));
}

void test_c1b2_review_config_counter_saturation_canonical_override_pending_bypass_fails_closed()
{
    check_c1b2_config_intentional_pending_bypass_fallback(
        C1b2ConfigTransitionSource::CounterSaturation,
        make_c1b2_canonical_config_transition_override(
            C1b2ConfigTransitionSource::CounterSaturation));
}

void check_c1b2_review_config_canonical_override_max_sequence_bypass_fails_closed(
    const C1b2ConfigTransitionSource source)
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_c1b2_config_source(adapter, source);
    constexpr TrustedReceipt receipt =
        make_config_committed_receipt(77, 1, 9);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, maximum);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_c1b2_canonical_config_transition_override(source));

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        c1b2_config_source_phase_after(source),
        before.last_trusted_ingress_sequence);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b2_canonical_config_post_submit_view_override(source)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_config_validation_bypass_used(adapter));
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    CHECK(after.pending_effect_count == 0);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 0);
    check_unused_pending_effect_slots_are_zero(after, 0);
    CHECK(after.last_dispatch_sequence == maximum);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.safety_delivery_blocked);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.first_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.critical.last_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        c1b2_config_source_phase_after(source),
        false,
        submit_count_before + 1);
}

void test_c1b2_review_config_normal_canonical_override_max_sequence_bypass_fails_closed()
{
    check_c1b2_review_config_canonical_override_max_sequence_bypass_fails_closed(
        C1b2ConfigTransitionSource::AcceptedBundle);
}

void test_c1b2_review_config_counter_saturation_canonical_override_unavailable_safety_plan_fails_closed()
{
    check_c1b2_review_config_canonical_override_max_sequence_bypass_fails_closed(
        C1b2ConfigTransitionSource::CounterSaturation);
}

void test_c1b2_config_valid_overrides_preserve_existing_paths()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_c1b2_config_source(
            adapter, C1b2ConfigTransitionSource::AcceptedBundle);
        constexpr TrustedReceipt receipt =
            make_config_committed_receipt(77, 1, 9);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, 41);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(
                adapter,
                make_c1b2_canonical_config_transition_override(
                    C1b2ConfigTransitionSource::AcceptedBundle));
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter,
                make_c1b2_canonical_config_post_submit_view_override(
                    C1b2ConfigTransitionSource::AcceptedBundle));

        check_exact_ingress_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::LivenessWaiting,
            1);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_dispatch_sequence == 45);
        CHECK(after.pending_effect_count == 4);
        check_canonical_config_pending_bundle(
            after, 42, 1, {77, 1, 1});
        CHECK(after.last_trusted_receipt_signature.ingress_sequence == 1);
        CHECK(trusted_receipts_equal(
            after.last_trusted_receipt_signature.receipt, receipt));
        CHECK(after.accepted_liveness_mask == 0);
        CHECK(!after.core_fail_closed_latched);
        CHECK(!after.core_adapter_fatal_latched);
        CHECK(!after.critical_pending);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == 1);
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_last_config_validation_bypass_used(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_core_transition_override_pending(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_core_post_submit_view_override_pending(adapter));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_c1b2_config_source(
            adapter, C1b2ConfigTransitionSource::CounterSaturation);
        constexpr TrustedReceipt receipt =
            make_config_committed_receipt(77, 1, 9);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, 41);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(
                adapter,
                make_c1b2_canonical_config_transition_override(
                    C1b2ConfigTransitionSource::CounterSaturation));
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter,
                make_c1b2_canonical_config_post_submit_view_override(
                    C1b2ConfigTransitionSource::CounterSaturation));

        check_exact_ingress_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::FailClosed,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::RecoveryPending,
            1);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_dispatch_sequence == 43);
        CHECK(after.pending_effect_count == 2);
        check_canonical_counter_saturation_pending_pair(after, 42, 43);
        CHECK(after.last_trusted_receipt_signature.ingress_sequence == 1);
        CHECK(trusted_receipts_equal(
            after.last_trusted_receipt_signature.receipt, receipt));
        CHECK(after.accepted_liveness_mask == 0);
        CHECK(after.core_fail_closed_latched);
        CHECK(!after.core_adapter_fatal_latched);
        CHECK(!after.critical_pending);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == 1);
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_last_config_validation_bypass_used(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_core_transition_override_pending(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_core_post_submit_view_override_pending(adapter));
    }
}

void test_c1b2_config_override_requires_full_authorization_to_bypass_pending_gate()
{
    enum class Mismatch : std::uint8_t {
        ReceiptKind = 0,
        Phase,
        BootEnded,
        Session,
        Generation,
        EqualCommit,
        OlderCommit,
    };
    constexpr std::array<Mismatch, 7> mismatches{{
        Mismatch::ReceiptKind,
        Mismatch::Phase,
        Mismatch::BootEnded,
        Mismatch::Session,
        Mismatch::Generation,
        Mismatch::EqualCommit,
        Mismatch::OlderCommit,
    }};
    for (const Mismatch mismatch : mismatches) {
        RuntimeOwnerAdapterCore adapter{};
        if (mismatch == Mismatch::Phase) {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
                adapter, RuntimeOwnerPhase::LivenessWaiting));
        } else {
            const std::uint32_t last_commit =
                mismatch == Mismatch::EqualCommit ||
                        mismatch == Mismatch::OlderCommit
                    ? 9
                    : 0;
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_prepare_core_awaiting_config(
                    adapter, 77, 1, last_commit);
        }

        if (mismatch == Mismatch::BootEnded) {
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_set_core_boot_orchestration_ended(adapter, true);
        }
        const RuntimeOwnerView authorization_view =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core;
        TrustedReceipt receipt = make_config_committed_receipt(
            authorization_view.mqtt_session_id,
            authorization_view.mqtt_generation,
            authorization_view.last_config_commit_sequence + 1);
        if (mismatch == Mismatch::ReceiptKind) {
            receipt.kind = static_cast<TrustedReceiptKind>(255);
        } else if (mismatch == Mismatch::Session) {
            receipt.mqtt_session_id = 78;
        } else if (mismatch == Mismatch::Generation) {
            receipt.mqtt_generation = 2;
        } else if (mismatch == Mismatch::EqualCommit) {
            receipt.config_commit_sequence =
                authorization_view.last_config_commit_sequence;
        } else if (mismatch == Mismatch::OlderCommit) {
            receipt.config_commit_sequence =
                authorization_view.last_config_commit_sequence - 1;
        }
        if (mismatch == Mismatch::ReceiptKind) {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::
                      fixture_enqueue_trusted_receipt_unchecked(
                          adapter, receipt));
        } else {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, receipt) == TrustedEnqueueResult::Accepted);
        }
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(
                adapter,
                make_c1b2_canonical_config_transition_override(
                    C1b2ConfigTransitionSource::AcceptedBundle));
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter,
                make_c1b2_canonical_config_post_submit_view_override(
                    C1b2ConfigTransitionSource::AcceptedBundle));
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);
        CHECK(before.trusted_count == 1);
        CHECK(before.pending_effect_count == 1);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_transition_override_pending(adapter));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_post_submit_view_override_pending(adapter));
        CHECK(submit_count_before == 0);

        const RuntimeOwnerAdapterTrustedSlotSnapshot head =
            before.trusted_slots[before.trusted_head];
        const bool config_head_candidate =
            head.receipt.kind == TrustedReceiptKind::ConfigCommitted;
        const bool exact_phase =
            before.core.phase == RuntimeOwnerPhase::AwaitingConfigCommit;
        const bool boot_active =
            !before.core.boot_orchestration_ended;
        const bool exact_session =
            head.receipt.mqtt_session_id == before.core.mqtt_session_id;
        const bool exact_generation =
            head.receipt.mqtt_generation == before.core.mqtt_generation;
        const bool newer_commit =
            head.receipt.config_commit_sequence >
            before.core.last_config_commit_sequence;
        const std::size_t false_authorization_terms =
            static_cast<std::size_t>(!config_head_candidate) +
            static_cast<std::size_t>(!exact_phase) +
            static_cast<std::size_t>(!boot_active) +
            static_cast<std::size_t>(!exact_session) +
            static_cast<std::size_t>(!exact_generation) +
            static_cast<std::size_t>(!newer_commit);
        CHECK(false_authorization_terms == 1);

        if (mismatch == Mismatch::ReceiptKind) {
            check_exact_step_result(
                adapter.step(),
                AdapterStepAction::Idle,
                RuntimeOwnerDisposition::Rejected,
                before.core.phase,
                before.core.phase);
            CHECK(private_snapshots_equal(
                before,
                RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        } else {
            check_exact_ingress_step_result(
                adapter.step(),
                AdapterStepAction::TrustedReceiptDiscarded,
                RuntimeOwnerDisposition::Rejected,
                before.core.phase,
                before.core.phase,
                head.ingress_sequence);
            CHECK(adapter.view().trusted_depth == 0);
            CHECK(adapter.view().trusted_stale_count ==
                  before.trusted_stale_count + 1);
        }
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_transition_override_pending(adapter));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_post_submit_view_override_pending(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_last_config_validation_bypass_used(adapter));
    }
}

void test_task4c_c1b2_config_malformed_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    for (const C1b2ConfigTransitionSource source : {
             C1b2ConfigTransitionSource::AcceptedBundle,
             C1b2ConfigTransitionSource::CounterSaturation,
         }) {
        RuntimeOwnerTransition malformed =
            make_c1b2_canonical_config_transition_override(source);
        malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
        check_c1b2_config_malformed_fallback(source, malformed);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_task4c_c1b1_transport_established_malformed_path_is_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    RuntimeOwnerTransition malformed =
        make_canonical_transport_established_transition_override();
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b1_transport_established_malformed_fallback(malformed);
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_task4c_c1a_begin_malformed_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    RuntimeOwnerTransition malformed =
        make_canonical_begin_transition_override();
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1a_begin_malformed_fallback(malformed);
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void check_config_counter_saturation_fail_closed(
    const std::uint32_t config_apply_epoch_counter,
    const std::uint32_t correlation_id_counter)
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
        adapter,
        config_apply_epoch_counter,
        correlation_id_counter);
    constexpr TrustedReceipt receipt =
        make_config_committed_receipt(77, 1, 9);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::RecoveryPending,
        1);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(after.core.mqtt_session_id == 0);
    CHECK(after.core.mqtt_generation == 0);
    CHECK(after.core.mqtt_generation_counter == 1);
    CHECK(after.core.config_apply_epoch_counter ==
          config_apply_epoch_counter);
    CHECK(after.core.last_config_commit_sequence == 0);
    CHECK(after.core.last_correlation_id == correlation_id_counter);
    CHECK(after.core.active_attempt == LivenessAttemptToken{});
    CHECK(!after.core.boot_orchestration_ended);
    CHECK(after.core.last_fault == RuntimeOwnerFaultCode::CounterSaturation);
    CHECK(after.trusted_head == 1);
    CHECK(after.trusted_tail == 1);
    CHECK(after.trusted_count == 0);
    CHECK(trusted_slot_snapshots_equal(
        after.trusted_slots[0], RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    CHECK(after.last_trusted_receipt_signature.ingress_sequence == 1);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.last_dispatch_sequence == 2);
    check_canonical_counter_saturation_pending_pair(after, 1, 2);
    CHECK(after.core_fail_closed_latched);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.prepared_dispatch_sequence == 1);
    CHECK(adapter.peek_dispatch().effect.kind ==
          RuntimeOwnerEffectKind::RecordFault);
}

void test_config_exact_epoch_counter_boundary_fail_closed_commits_safety_pair()
{
    check_config_counter_saturation_fail_closed(
        std::numeric_limits<std::uint32_t>::max(), 0);
}

void test_config_exact_correlation_counter_boundary_fail_closed_commits_safety_pair()
{
    check_config_counter_saturation_fail_closed(
        0, std::numeric_limits<std::uint32_t>::max() - 5);
}

void test_config_epoch_counter_one_before_boundary_remains_accepted()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
        adapter,
        std::numeric_limits<std::uint32_t>::max() - 1,
        0);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        1);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.config_apply_epoch_counter ==
          std::numeric_limits<std::uint32_t>::max());
    CHECK(after.core.last_correlation_id == 6);
    check_canonical_config_pending_bundle(
        after,
        1,
        1,
        {77, 1, std::numeric_limits<std::uint32_t>::max()});
}

void test_config_correlation_exact_last_bundle_start_remains_accepted()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    constexpr std::uint32_t kLastCoreBundleStart =
        std::numeric_limits<std::uint32_t>::max() - 6;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
        adapter, 0, kLastCoreBundleStart);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        1);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.config_apply_epoch_counter == 1);
    CHECK(after.core.last_correlation_id ==
          std::numeric_limits<std::uint32_t>::max());
    check_canonical_config_pending_bundle(
        after,
        1,
        std::numeric_limits<std::uint32_t>::max() - 5,
        {77, 1, 1});
}

void test_config_fail_closed_safety_pair_uses_regular_reserve_boundary()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
        adapter,
        std::numeric_limits<std::uint32_t>::max(),
        0);
    constexpr std::uint32_t kLastRegularSafetyStart =
        std::numeric_limits<std::uint32_t>::max() - 5;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, kLastRegularSafetyStart);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::RecoveryPending,
        1);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.last_dispatch_sequence ==
          std::numeric_limits<std::uint32_t>::max() - 3);
    check_canonical_counter_saturation_pending_pair(
        after,
        std::numeric_limits<std::uint32_t>::max() - 4,
        std::numeric_limits<std::uint32_t>::max() - 3);
    CHECK(!after.dispatch_fatal_latched);
}

void test_config_fail_closed_safety_pair_uses_exact_terminal_reserve()
{
    constexpr std::array<std::uint32_t, 3> terminal_pair_starts{{
        std::numeric_limits<std::uint32_t>::max() - 4,
        std::numeric_limits<std::uint32_t>::max() - 3,
        std::numeric_limits<std::uint32_t>::max() - 2,
    }};
    for (const std::uint32_t start : terminal_pair_starts) {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
            adapter,
            std::numeric_limits<std::uint32_t>::max(),
            0);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);

        check_exact_ingress_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::FailClosed,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::RecoveryPending,
            1);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_dispatch_sequence ==
              std::numeric_limits<std::uint32_t>::max());
        check_canonical_counter_saturation_pending_pair(
            after,
            std::numeric_limits<std::uint32_t>::max() - 1,
            std::numeric_limits<std::uint32_t>::max());
        CHECK(after.dispatch_fatal_latched);
    }
}

void test_config_fail_closed_dispatch_shortage_latches_once_before_core_submit()
{
    constexpr std::array<std::uint32_t, 2> insufficient_starts{{
        std::numeric_limits<std::uint32_t>::max() - 1,
        std::numeric_limits<std::uint32_t>::max(),
    }};
    for (const std::uint32_t start : insufficient_starts) {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
            adapter,
            std::numeric_limits<std::uint32_t>::max(),
            0);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::AwaitingConfigCommit);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(runtime_owner_views_equal(before.core, after.core));
        CHECK(after.trusted_head == before.trusted_head);
        CHECK(after.trusted_count == before.trusted_count);
        CHECK(after.pending_effect_count == 0);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(after.last_dispatch_sequence == start);
        CHECK(last_trusted_receipt_signatures_equal(
            after.last_trusted_receipt_signature,
            before.last_trusted_receipt_signature));
        CHECK(after.critical_pending);
        CHECK(after.critical.first_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(after.critical.first_ingress_sequence == 1);
        CHECK(after.critical.occurrence_count == 1);
        CHECK(!after.core_fail_closed_latched);
        CHECK(!after.dispatch_fatal_latched);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::AwaitingConfigCommit);
        CHECK(adapter.view().critical_pending == 0);
        CHECK(adapter.view().safety_delivery_blocked == 1);
    }
}

void test_config_committed_head_atomically_applies_four_effect_bundle_and_signature()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_awaiting_config_via_trusted(adapter, 77);
    constexpr TrustedReceipt receipt =
        make_config_committed_receipt(77, 1, 9);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(before.last_dispatch_sequence == 1);
    CHECK(before.pending_effect_count == 0);
    CHECK(before.last_trusted_receipt_signature.ingress_sequence == 1);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        2);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.phase == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(after.core.mqtt_session_id == 77);
    CHECK(after.core.mqtt_generation == 1);
    CHECK(after.core.mqtt_generation_counter == 1);
    CHECK(after.core.config_apply_epoch_counter == 1);
    CHECK(after.core.last_config_commit_sequence == 9);
    CHECK(after.core.last_correlation_id == 6);
    CHECK((after.core.active_attempt == LivenessAttemptToken{77, 1, 1}));
    CHECK(!after.core.boot_orchestration_ended);
    CHECK(after.core.last_fault == RuntimeOwnerFaultCode::None);
    CHECK(after.trusted_head == 2);
    CHECK(after.trusted_tail == 2);
    CHECK(after.trusted_count == 0);
    CHECK(trusted_slot_snapshots_equal(
        after.trusted_slots[1], RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    CHECK(after.last_trusted_receipt_signature.ingress_sequence == 2);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.last_dispatch_sequence == 5);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 0);
    check_canonical_config_pending_bundle(
        after, 2, 1, {77, 1, 1});
    CHECK(adapter.view().current_dispatch.kind == AdapterDispatchKind::None);
    CHECK(adapter.view().current_dispatch.dispatch_sequence == 0);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(prepared.phase_before == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(prepared.phase_after == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(prepared.consumed_ingress_sequence == 0);
    CHECK(prepared.consumed_enqueue_sequence == 0);
    CHECK(prepared.prepared_dispatch_sequence == 2);
    CHECK(adapter.view().pending_effect_count == 3);
}

void test_config_head_precedes_pending_owner_trigger_without_cancelling_trigger()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    constexpr TrustedReceipt receipt =
        make_config_committed_receipt(77, 1, 9);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        1);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(!after.transport_request_pending);
    CHECK(after.last_dispatch_sequence == 4);
    check_canonical_config_pending_bundle(
        after, 1, 1, {77, 1, 1});

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(prepared.phase_before == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(prepared.phase_after == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(prepared.consumed_ingress_sequence == 0);
    CHECK(prepared.consumed_enqueue_sequence == 0);
    CHECK(prepared.prepared_dispatch_sequence == 1);
    CHECK(adapter.view().pending_effect_count == 3);
    CHECK(adapter.view().transport_request_pending == 0);
}

void test_config_head_with_pending_effects_defers_without_preemption_or_dequeue()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().pending_effect_count == 1);
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        before.last_trusted_ingress_sequence);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.pending_effect_count == 4);
    CHECK(after.last_dispatch_sequence == 5);
    CHECK(after.effect_cancelled_count ==
          before.effect_cancelled_count + 1);
    CHECK(after.trusted_count == 0);
}

void test_config_wrong_phase_session_generation_equal_or_older_discards_without_submit()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_connecting_without_pending(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        check_discarded_trusted_head_is_only_dequeue_mutation(
            adapter, RuntimeOwnerPhase::TransportConnecting);
    }

    struct ConfigMismatchCase {
        std::uint32_t active_session;
        std::uint32_t active_generation;
        std::uint32_t last_commit;
        TrustedReceipt receipt;
    };
    constexpr std::array<ConfigMismatchCase, 4> cases{{
        {77, 1, 0, make_config_committed_receipt(78, 1, 9)},
        {77, 1, 0, make_config_committed_receipt(77, 2, 9)},
        {77, 1, 9, make_config_committed_receipt(77, 1, 9)},
        {77, 1, 9, make_config_committed_receipt(77, 1, 8)},
    }};
    for (const ConfigMismatchCase mismatch : cases) {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter,
            mismatch.active_session,
            mismatch.active_generation,
            mismatch.last_commit);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, mismatch.receipt) ==
              TrustedEnqueueResult::Accepted);
        check_discarded_trusted_head_is_only_dequeue_mutation(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit);
    }
}

void test_config_last_bundle_start_preserves_terminal_reserve()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    constexpr std::uint32_t kLastSuccessfulStart =
        std::numeric_limits<std::uint32_t>::max() - 6;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, kLastSuccessfulStart);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        1);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.last_dispatch_sequence ==
          std::numeric_limits<std::uint32_t>::max() - 2);
    check_canonical_config_pending_bundle(
        after,
        std::numeric_limits<std::uint32_t>::max() - 5,
        1,
        {77, 1, 1});
}

void test_config_dispatch_shortage_latches_once_without_submit_dequeue_or_partial_pending()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 77, 1, 0);
    constexpr std::uint32_t kInsufficientStart =
        std::numeric_limits<std::uint32_t>::max() - 5;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, kInsufficientStart);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CriticalLedgerHandled,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::AwaitingConfigCommit);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(before.core, after.core));
    CHECK(after.trusted_head == before.trusted_head);
    CHECK(after.trusted_count == before.trusted_count);
    CHECK(after.pending_effect_count == 0);
    check_unused_pending_effect_slots_are_zero(after, 0);
    CHECK(after.last_dispatch_sequence == kInsufficientStart);
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::DispatchSequenceSaturation);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::DispatchSequenceSaturation);
    CHECK(after.critical.first_ingress_sequence == 1);
    CHECK(after.critical.last_ingress_sequence == 1);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(!after.dispatch_fatal_latched);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::RecoveryPending);
    CHECK(adapter.view().critical_pending == 0);
    CHECK(adapter.view().last_dispatch_sequence ==
          std::numeric_limits<std::uint32_t>::max() - 3);
    CHECK(adapter.view().pending_effect_count == 2);
}

void test_config_head_defers_to_shutdown_critical_and_fatal_without_dequeue()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        auto shutdown = adapter.shutdown_port();
        CHECK(shutdown.request() == UrgentRequestResult::Accepted);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::ShutdownCommitted);
        CHECK(adapter.view().trusted_depth == 0);
        CHECK(adapter.view().trusted_cancelled_count == 1);
        CHECK(adapter.step().action == AdapterStepAction::Terminal);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        TrustedReceipt invalid =
            make_transport_established_receipt(77, 1);
        invalid.reserved = 1;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) ==
              TrustedEnqueueResult::RejectedInvalid);
        CHECK(adapter.view().critical_pending == 1);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::RecoveryPending);
        CHECK(adapter.view().critical_pending == 0);
        CHECK(adapter.view().pending_effect_count == 2);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_dispatch_fatal(
            adapter, true);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Terminal,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::AwaitingConfigCommit);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }
}

void check_classified_trusted_discard(
    RuntimeOwnerAdapterCore &adapter,
    const RuntimeOwnerPhase phase,
    const std::uint32_t expected_stale_count,
    const std::uint32_t expected_duplicate_count)
{
    if (adapter.view().pending_effect_count != 0 &&
        adapter.view().physical_inflight.kind ==
            AdapterDispatchKind::None) {
        CHECK(adapter.step().action ==
              AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        const bool delivery_only =
            offered.effect.kind ==
                RuntimeOwnerEffectKind::EndBootOrchestration ||
            offered.effect.kind == RuntimeOwnerEffectKind::RecordFault ||
            offered.effect.kind == RuntimeOwnerEffectKind::EnterRecovery;
        CHECK(adapter.acknowledge_dispatch(
                  offered.dispatch_sequence) ==
              (delivery_only
                   ? DispatchAckResult::AcceptedDelivery
                   : DispatchAckResult::AcceptedOperationInflight));
    }
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.trusted_count == 1);
    const std::uint8_t consumed_head = before.trusted_head;
    const std::uint32_t consumed_ingress =
        before.trusted_slots[consumed_head].ingress_sequence;

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::TrustedReceiptDiscarded,
        RuntimeOwnerDisposition::Rejected,
        phase,
        phase,
        consumed_ingress);

    RuntimeOwnerAdapterPrivateSnapshot expected = before;
    expected.trusted_slots[consumed_head] = {};
    expected.trusted_head = static_cast<std::uint8_t>(
        (consumed_head + 1) % expected.trusted_slots.size());
    --expected.trusted_count;
    expected.trusted_stale_count = expected_stale_count;
    expected.trusted_duplicate_count = expected_duplicate_count;
    CHECK(private_snapshots_equal(
        expected, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(adapter.view().trusted_stale_count == expected_stale_count);
    CHECK(adapter.view().trusted_duplicate_count == expected_duplicate_count);
}

void test_transport_attempt_failed_happy_exact_commits_canonical_safety_pair()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    constexpr TrustedReceipt receipt =
        make_transport_attempt_failed_receipt(1, 77);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::RecoveryPending,
        1);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(after.core.mqtt_session_id == 0);
    CHECK(after.core.mqtt_generation == 0);
    CHECK(after.core.mqtt_generation_counter == 1);
    CHECK(after.core.config_apply_epoch_counter == 0);
    CHECK(after.core.last_config_commit_sequence == 0);
    CHECK(after.core.last_correlation_id == 0);
    CHECK(after.core.active_attempt == LivenessAttemptToken{});
    CHECK(!after.core.boot_orchestration_ended);
    CHECK(after.core.last_fault == RuntimeOwnerFaultCode::TransportFailure);
    CHECK(after.trusted_head == 1);
    CHECK(after.trusted_tail == 1);
    CHECK(after.trusted_count == 0);
    CHECK(trusted_slot_snapshots_equal(
        after.trusted_slots[0], RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    CHECK(after.last_trusted_receipt_signature.ingress_sequence == 1);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.last_dispatch_sequence == 3);
    check_canonical_recovery_pending_pair(
        after,
        2,
        3,
        RuntimeOwnerFaultCode::TransportFailure,
        0,
        {});
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(adapter.view().trusted_stale_count == 0);
    CHECK(adapter.view().trusted_duplicate_count == 0);
}

void test_transport_attempt_failed_wrong_generation_or_phase_is_stale_without_submit()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_connecting_without_pending(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_attempt_failed_receipt(2, 1)) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::TransportConnecting, 1, 0);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_attempt_failed_receipt(1, 2)) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::ColdStart, 1, 0);
    }
}

void test_transport_attempt_failed_exact_signature_is_duplicate_but_otherwise_stale()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_connecting_without_pending(adapter);
    constexpr TrustedReceipt accepted =
        make_transport_attempt_failed_receipt(1, 77);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, accepted) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(adapter);
    const RuntimeOwnerAdapterPrivateSnapshot accepted_state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, accepted) == TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RecoveryPending, 0, 1);
    CHECK(runtime_owner_views_equal(
        accepted_state.core,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
              .last_trusted_receipt_signature.ingress_sequence == 1);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_attempt_failed_receipt(1, 78)) ==
          TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RecoveryPending, 1, 1);
    CHECK(runtime_owner_views_equal(
        accepted_state.core,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
              .last_trusted_receipt_signature.ingress_sequence == 1);
}

void test_transport_attempt_failed_diagnostic_code_does_not_change_core_translation()
{
    RuntimeOwnerAdapterCore zero_diagnostic{};
    RuntimeOwnerAdapterCore max_diagnostic{};
    fixture_prepare_connecting_without_pending(zero_diagnostic);
    fixture_prepare_connecting_without_pending(max_diagnostic);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              zero_diagnostic,
              make_transport_attempt_failed_receipt(1, 0)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              max_diagnostic,
              make_transport_attempt_failed_receipt(
                  1, std::numeric_limits<std::uint32_t>::max())) ==
          TrustedEnqueueResult::Accepted);
    CHECK(zero_diagnostic.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    CHECK(max_diagnostic.step().action ==
          AdapterStepAction::CoreTransitionApplied);

    const RuntimeOwnerAdapterPrivateSnapshot zero_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(zero_diagnostic);
    const RuntimeOwnerAdapterPrivateSnapshot max_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(max_diagnostic);
    CHECK(runtime_owner_views_equal(zero_after.core, max_after.core));
    CHECK(zero_after.last_dispatch_sequence ==
          max_after.last_dispatch_sequence);
    for (std::size_t index = 0;
         index < zero_after.pending_effect_slots.size(); ++index) {
        CHECK(pending_effect_slot_snapshots_equal(
            zero_after.pending_effect_slots[index],
            max_after.pending_effect_slots[index]));
    }
}

void test_transport_attempt_failed_pending_effect_blocks_without_dequeue()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().pending_effect_count == 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_attempt_failed_receipt(1, 9)) ==
          TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.prepared_dispatch_sequence == 1);
    CHECK(adapter.view().trusted_depth == before.trusted_count);
    CHECK(adapter.acknowledge_dispatch(1) ==
          DispatchAckResult::AcceptedOperationInflight);
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::RecoveryPending,
        before.last_trusted_ingress_sequence);
}

void test_transport_attempt_failed_sequence_preflight_regular_and_terminal_reserve()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_connecting_without_pending(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, std::numeric_limits<std::uint32_t>::max() - 5);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_attempt_failed_receipt(1)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_dispatch_sequence ==
              std::numeric_limits<std::uint32_t>::max() - 3);
        check_canonical_recovery_pending_pair(
            after,
            std::numeric_limits<std::uint32_t>::max() - 4,
            std::numeric_limits<std::uint32_t>::max() - 3,
            RuntimeOwnerFaultCode::TransportFailure,
            0,
            {});
        CHECK(!after.dispatch_fatal_latched);
    }

    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        std::numeric_limits<std::uint32_t>::max() - 4,
        std::numeric_limits<std::uint32_t>::max() - 3,
        std::numeric_limits<std::uint32_t>::max() - 2,
    }};
    for (const std::uint32_t start : terminal_starts) {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_connecting_without_pending(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_attempt_failed_receipt(1)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_dispatch_sequence ==
              std::numeric_limits<std::uint32_t>::max());
        check_canonical_recovery_pending_pair(
            after,
            std::numeric_limits<std::uint32_t>::max() - 1,
            std::numeric_limits<std::uint32_t>::max(),
            RuntimeOwnerFaultCode::TransportFailure,
            0,
            {});
        CHECK(after.dispatch_fatal_latched);
    }
}

void test_transport_attempt_failed_sequence_shortage_is_bounded_before_submit()
{
    constexpr std::array<std::uint32_t, 2> insufficient_starts{{
        std::numeric_limits<std::uint32_t>::max() - 1,
        std::numeric_limits<std::uint32_t>::max(),
    }};
    for (const std::uint32_t start : insufficient_starts) {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_connecting_without_pending(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_attempt_failed_receipt(1, 91)) ==
              TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::TransportConnecting,
            RuntimeOwnerPhase::TransportConnecting);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
        CHECK(runtime_owner_views_equal(before.core, after.core));
        CHECK(after.trusted_head == before.trusted_head);
        CHECK(after.trusted_count == before.trusted_count);
        CHECK(after.pending_effect_count == 0);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(after.last_dispatch_sequence == start);
        CHECK(last_trusted_receipt_signatures_equal(
            after.last_trusted_receipt_signature,
            before.last_trusted_receipt_signature));
        CHECK(after.critical_pending);
        CHECK(after.critical.first_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(after.critical.first_ingress_sequence == 1);
        CHECK(after.critical.first_diagnostic_code == 0);
        CHECK(after.critical.occurrence_count == 1);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::TransportConnecting,
            RuntimeOwnerPhase::TransportConnecting);
        CHECK(adapter.view().critical_pending == 0);
        CHECK(adapter.view().safety_delivery_blocked == 1);
    }
}

void test_transport_disconnected_happy_exact_accepts_all_active_phases()
{
    constexpr std::array<RuntimeOwnerPhase, 4> allowed_phases{{
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RuntimeReady,
    }};
    for (const RuntimeOwnerPhase phase : allowed_phases) {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, phase));
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(before.core.mqtt_session_id == 1);
        CHECK(before.core.mqtt_generation == 1);
        CHECK(before.core.boot_orchestration_ended ==
              (phase == RuntimeOwnerPhase::RuntimeReady));
        const TrustedReceipt receipt = make_transport_disconnected_receipt(
            1, 1, 70 + static_cast<std::uint32_t>(phase));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);

        check_exact_ingress_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            phase,
            RuntimeOwnerPhase::RecoveryPending,
            1);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.core.phase == RuntimeOwnerPhase::RecoveryPending);
        CHECK(after.core.mqtt_session_id == 0);
        CHECK(after.core.mqtt_generation == 0);
        CHECK(after.core.mqtt_generation_counter ==
              before.core.mqtt_generation_counter);
        CHECK(after.core.config_apply_epoch_counter ==
              before.core.config_apply_epoch_counter);
        CHECK(after.core.last_config_commit_sequence ==
              before.core.last_config_commit_sequence);
        CHECK(after.core.last_correlation_id ==
              before.core.last_correlation_id);
        CHECK(after.core.active_attempt == LivenessAttemptToken{});
        CHECK(after.core.boot_orchestration_ended ==
              before.core.boot_orchestration_ended);
        CHECK(after.core.last_fault ==
              RuntimeOwnerFaultCode::TransportDisconnected);
        CHECK(after.trusted_count == 0);
        CHECK(after.last_trusted_receipt_signature.ingress_sequence == 1);
        CHECK(trusted_receipts_equal(
            after.last_trusted_receipt_signature.receipt, receipt));
        CHECK(after.last_dispatch_sequence == 2);
        check_canonical_recovery_pending_pair(
            after,
            1,
            2,
            RuntimeOwnerFaultCode::TransportDisconnected,
            0,
            before.core.active_attempt);
        CHECK(!after.core_fail_closed_latched);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(!after.critical_pending);
    }
}

void test_transport_disconnected_wrong_phase_session_or_generation_is_stale()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_disconnected_receipt(1, 1, 1)) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::ColdStart, 1, 0);
    }

    constexpr std::array<TrustedReceipt, 2> mismatches{{
        make_transport_disconnected_receipt(2, 1, 2),
        make_transport_disconnected_receipt(1, 2, 3),
    }};
    for (const TrustedReceipt mismatch : mismatches) {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, mismatch) == TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit, 1, 0);
    }
}

void test_transport_disconnected_exact_signature_is_duplicate_before_view_check()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
        adapter, RuntimeOwnerPhase::AwaitingConfigCommit));
    constexpr TrustedReceipt accepted =
        make_transport_disconnected_receipt(1, 1, 77);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, accepted) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(adapter);
    const RuntimeOwnerAdapterPrivateSnapshot accepted_state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, accepted) == TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RecoveryPending, 0, 1);
    CHECK(runtime_owner_views_equal(
        accepted_state.core,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
              .last_trusted_receipt_signature.ingress_sequence == 1);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(1, 1, 78)) ==
          TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RecoveryPending, 1, 1);
    CHECK(runtime_owner_views_equal(
        accepted_state.core,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core));
}

void test_transport_disconnected_diagnostic_code_does_not_change_core_translation()
{
    RuntimeOwnerAdapterCore zero_diagnostic{};
    RuntimeOwnerAdapterCore max_diagnostic{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
        zero_diagnostic, RuntimeOwnerPhase::SnapshotFreezePending));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
        max_diagnostic, RuntimeOwnerPhase::SnapshotFreezePending));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              zero_diagnostic,
              make_transport_disconnected_receipt(1, 1, 0)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              max_diagnostic,
              make_transport_disconnected_receipt(
                  1, 1, std::numeric_limits<std::uint32_t>::max())) ==
          TrustedEnqueueResult::Accepted);
    CHECK(zero_diagnostic.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    CHECK(max_diagnostic.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    const RuntimeOwnerAdapterPrivateSnapshot zero_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(zero_diagnostic);
    const RuntimeOwnerAdapterPrivateSnapshot max_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(max_diagnostic);
    CHECK(runtime_owner_views_equal(zero_after.core, max_after.core));
    for (std::size_t index = 0;
         index < zero_after.pending_effect_slots.size(); ++index) {
        CHECK(pending_effect_slot_snapshots_equal(
            zero_after.pending_effect_slots[index],
            max_after.pending_effect_slots[index]));
    }
}

void test_transport_disconnected_pending_effect_defers_without_dequeue()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
        adapter, 1, 1, 0);
    CHECK(adapter.view().pending_effect_count == 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(1, 1, 9)) ==
          TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::RecoveryPending,
        before.last_trusted_ingress_sequence);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.pending_effect_count == 2);
    CHECK(after.effect_cancelled_count ==
          before.effect_cancelled_count + 1);
    CHECK(after.trusted_count == 0);
}

void test_transport_disconnected_sequence_preflight_regular_and_terminal_reserve()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit));
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, std::numeric_limits<std::uint32_t>::max() - 5);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_disconnected_receipt(1, 1)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_dispatch_sequence ==
              std::numeric_limits<std::uint32_t>::max() - 3);
        check_canonical_recovery_pending_pair(
            after,
            std::numeric_limits<std::uint32_t>::max() - 4,
            std::numeric_limits<std::uint32_t>::max() - 3,
            RuntimeOwnerFaultCode::TransportDisconnected,
            0,
            {});
        CHECK(!after.dispatch_fatal_latched);
    }

    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        std::numeric_limits<std::uint32_t>::max() - 4,
        std::numeric_limits<std::uint32_t>::max() - 3,
        std::numeric_limits<std::uint32_t>::max() - 2,
    }};
    for (const std::uint32_t start : terminal_starts) {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit));
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_disconnected_receipt(1, 1)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_dispatch_sequence ==
              std::numeric_limits<std::uint32_t>::max());
        check_canonical_recovery_pending_pair(
            after,
            std::numeric_limits<std::uint32_t>::max() - 1,
            std::numeric_limits<std::uint32_t>::max(),
            RuntimeOwnerFaultCode::TransportDisconnected,
            0,
            {});
        CHECK(after.dispatch_fatal_latched);
    }
}

void test_transport_disconnected_damaged_sequence_shortage_is_bounded()
{
    constexpr std::array<std::uint32_t, 2> insufficient_starts{{
        std::numeric_limits<std::uint32_t>::max() - 1,
        std::numeric_limits<std::uint32_t>::max(),
    }};
    for (const std::uint32_t start : insufficient_starts) {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, RuntimeOwnerPhase::AwaitingConfigCommit));
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_disconnected_receipt(1, 1, 91)) ==
              TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::AwaitingConfigCommit);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
        CHECK(runtime_owner_views_equal(before.core, after.core));
        CHECK(after.trusted_head == before.trusted_head);
        CHECK(after.trusted_count == before.trusted_count);
        CHECK(after.pending_effect_count == 0);
        CHECK(after.last_dispatch_sequence == start);
        CHECK(last_trusted_receipt_signatures_equal(
            after.last_trusted_receipt_signature,
            before.last_trusted_receipt_signature));
        CHECK(after.critical_pending);
        CHECK(after.critical.first_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(after.critical.first_ingress_sequence == 1);
        CHECK(after.critical.occurrence_count == 1);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::AwaitingConfigCommit,
            RuntimeOwnerPhase::AwaitingConfigCommit);
        CHECK(adapter.view().critical_pending == 0);
        CHECK(adapter.view().safety_delivery_blocked == 1);
    }
}

enum class C1b4TransportFaultSource : std::uint8_t {
    AttemptFailed = 0,
    Disconnected = 1,
};

enum class C1b4TransitionMutation : std::uint8_t {
    None = 0,
    DispositionUnknown,
    DispositionRejected,
    DispositionAcceptedDuplicate,
    DispositionFailClosed,
    PhaseBeforeKnownWrong,
    PhaseBeforeUnknown,
    PhaseAfterKnownWrong,
    PhaseAfterUnknown,
    EffectCountZero,
    EffectCountOne,
    EffectCountThree,
    EffectCountFive,
    Used0Kind,
    Used0Correlation,
    Used0AttemptSession,
    Used0AttemptGeneration,
    Used0AttemptEpoch,
    Used0Fault,
    Used1Kind,
    Used1Correlation,
    Used1AttemptSession,
    Used1AttemptGeneration,
    Used1AttemptEpoch,
    Used1Fault,
    Unused3Kind,
    Unused3Correlation,
    Unused3AttemptSession,
    Unused3AttemptGeneration,
    Unused3AttemptEpoch,
    Unused3Fault,
};

enum class C1b4PostViewMutation : std::uint8_t {
    None = 0,
    KnownWrongPhase,
    UnknownPhase,
    ShutdownPhase,
    MqttSession,
    MqttGeneration,
    MqttGenerationCounter,
    ConfigApplyEpochCounter,
    LastConfigCommitSequence,
    LastCorrelationId,
    ActiveAttemptSession,
    ActiveAttemptGeneration,
    ActiveAttemptEpoch,
    BootOrchestrationEnded,
    LastFault,
};

constexpr bool c1b4_is_disconnected(
    const C1b4TransportFaultSource source) noexcept
{
    return source == C1b4TransportFaultSource::Disconnected;
}

RuntimeOwnerView make_c1b4_transport_fault_post_view(
    const RuntimeOwnerView before,
    const C1b4TransportFaultSource source)
{
    RuntimeOwnerView after = before;
    after.phase = RuntimeOwnerPhase::RecoveryPending;
    after.mqtt_session_id = 0;
    after.mqtt_generation = 0;
    after.active_attempt = {};
    after.last_fault = c1b4_is_disconnected(source)
        ? RuntimeOwnerFaultCode::TransportDisconnected
        : RuntimeOwnerFaultCode::TransportFailure;
    return after;
}

RuntimeOwnerTransition make_c1b4_transport_fault_transition(
    const RuntimeOwnerView before,
    const C1b4TransportFaultSource source)
{
    const RuntimeOwnerFaultCode fault = c1b4_is_disconnected(source)
        ? RuntimeOwnerFaultCode::TransportDisconnected
        : RuntimeOwnerFaultCode::TransportFailure;
    const LivenessAttemptToken attempt = c1b4_is_disconnected(source)
        ? before.active_attempt
        : LivenessAttemptToken{};
    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = before.phase;
    transition.phase_after = RuntimeOwnerPhase::RecoveryPending;
    transition.effect_count = 2;
    transition.effects[0] = {
        RuntimeOwnerEffectKind::RecordFault,
        0,
        attempt,
        fault,
    };
    transition.effects[1] = {
        RuntimeOwnerEffectKind::EnterRecovery,
        0,
        attempt,
        fault,
    };
    return transition;
}

void mutate_c1b4_transport_fault_transition(
    RuntimeOwnerTransition &transition,
    const C1b4TransitionMutation mutation)
{
    switch (mutation) {
    case C1b4TransitionMutation::None:
        break;
    case C1b4TransitionMutation::DispositionUnknown:
        transition.disposition =
            static_cast<RuntimeOwnerDisposition>(255);
        break;
    case C1b4TransitionMutation::DispositionRejected:
        transition.disposition = RuntimeOwnerDisposition::Rejected;
        break;
    case C1b4TransitionMutation::DispositionAcceptedDuplicate:
        transition.disposition =
            RuntimeOwnerDisposition::AcceptedDuplicate;
        break;
    case C1b4TransitionMutation::DispositionFailClosed:
        transition.disposition = RuntimeOwnerDisposition::FailClosed;
        break;
    case C1b4TransitionMutation::PhaseBeforeKnownWrong:
        transition.phase_before = RuntimeOwnerPhase::ColdStart;
        break;
    case C1b4TransitionMutation::PhaseBeforeUnknown:
        transition.phase_before = static_cast<RuntimeOwnerPhase>(255);
        break;
    case C1b4TransitionMutation::PhaseAfterKnownWrong:
        transition.phase_after = RuntimeOwnerPhase::RuntimeReady;
        break;
    case C1b4TransitionMutation::PhaseAfterUnknown:
        transition.phase_after = static_cast<RuntimeOwnerPhase>(255);
        break;
    case C1b4TransitionMutation::EffectCountZero:
        transition.effect_count = 0;
        break;
    case C1b4TransitionMutation::EffectCountOne:
        transition.effect_count = 1;
        break;
    case C1b4TransitionMutation::EffectCountThree:
        transition.effect_count = 3;
        break;
    case C1b4TransitionMutation::EffectCountFive:
        transition.effect_count = 5;
        break;
    case C1b4TransitionMutation::Used0Kind:
        transition.effects[0].kind = RuntimeOwnerEffectKind::None;
        break;
    case C1b4TransitionMutation::Used0Correlation:
        ++transition.effects[0].correlation_id;
        break;
    case C1b4TransitionMutation::Used0AttemptSession:
        ++transition.effects[0].attempt.mqtt_session_id;
        break;
    case C1b4TransitionMutation::Used0AttemptGeneration:
        ++transition.effects[0].attempt.mqtt_generation;
        break;
    case C1b4TransitionMutation::Used0AttemptEpoch:
        ++transition.effects[0].attempt.config_apply_epoch;
        break;
    case C1b4TransitionMutation::Used0Fault:
        transition.effects[0].fault_code =
            RuntimeOwnerFaultCode::InternalInvariant;
        break;
    case C1b4TransitionMutation::Used1Kind:
        transition.effects[1].kind = RuntimeOwnerEffectKind::None;
        break;
    case C1b4TransitionMutation::Used1Correlation:
        ++transition.effects[1].correlation_id;
        break;
    case C1b4TransitionMutation::Used1AttemptSession:
        ++transition.effects[1].attempt.mqtt_session_id;
        break;
    case C1b4TransitionMutation::Used1AttemptGeneration:
        ++transition.effects[1].attempt.mqtt_generation;
        break;
    case C1b4TransitionMutation::Used1AttemptEpoch:
        ++transition.effects[1].attempt.config_apply_epoch;
        break;
    case C1b4TransitionMutation::Used1Fault:
        transition.effects[1].fault_code =
            RuntimeOwnerFaultCode::InternalInvariant;
        break;
    case C1b4TransitionMutation::Unused3Kind:
        transition.effects[3].kind = RuntimeOwnerEffectKind::RecordFault;
        break;
    case C1b4TransitionMutation::Unused3Correlation:
        ++transition.effects[3].correlation_id;
        break;
    case C1b4TransitionMutation::Unused3AttemptSession:
        ++transition.effects[3].attempt.mqtt_session_id;
        break;
    case C1b4TransitionMutation::Unused3AttemptGeneration:
        ++transition.effects[3].attempt.mqtt_generation;
        break;
    case C1b4TransitionMutation::Unused3AttemptEpoch:
        ++transition.effects[3].attempt.config_apply_epoch;
        break;
    case C1b4TransitionMutation::Unused3Fault:
        transition.effects[3].fault_code =
            RuntimeOwnerFaultCode::InternalInvariant;
        break;
    }
}

void mutate_c1b4_transport_fault_post_view(
    RuntimeOwnerView &view,
    const C1b4PostViewMutation mutation)
{
    switch (mutation) {
    case C1b4PostViewMutation::None:
        break;
    case C1b4PostViewMutation::KnownWrongPhase:
        view.phase = RuntimeOwnerPhase::RuntimeReady;
        break;
    case C1b4PostViewMutation::UnknownPhase:
        view.phase = static_cast<RuntimeOwnerPhase>(255);
        break;
    case C1b4PostViewMutation::ShutdownPhase:
        view.phase = RuntimeOwnerPhase::ShutdownCommitted;
        break;
    case C1b4PostViewMutation::MqttSession:
        ++view.mqtt_session_id;
        break;
    case C1b4PostViewMutation::MqttGeneration:
        ++view.mqtt_generation;
        break;
    case C1b4PostViewMutation::MqttGenerationCounter:
        ++view.mqtt_generation_counter;
        break;
    case C1b4PostViewMutation::ConfigApplyEpochCounter:
        ++view.config_apply_epoch_counter;
        break;
    case C1b4PostViewMutation::LastConfigCommitSequence:
        ++view.last_config_commit_sequence;
        break;
    case C1b4PostViewMutation::LastCorrelationId:
        ++view.last_correlation_id;
        break;
    case C1b4PostViewMutation::ActiveAttemptSession:
        ++view.active_attempt.mqtt_session_id;
        break;
    case C1b4PostViewMutation::ActiveAttemptGeneration:
        ++view.active_attempt.mqtt_generation;
        break;
    case C1b4PostViewMutation::ActiveAttemptEpoch:
        ++view.active_attempt.config_apply_epoch;
        break;
    case C1b4PostViewMutation::BootOrchestrationEnded:
        view.boot_orchestration_ended =
            !view.boot_orchestration_ended;
        break;
    case C1b4PostViewMutation::LastFault:
        view.last_fault = RuntimeOwnerFaultCode::InternalInvariant;
        break;
    }
}

void check_c1b4_transport_fault_malformed_fallback(
    const C1b4TransportFaultSource source,
    const C1b4TransitionMutation transition_mutation,
    const C1b4PostViewMutation post_view_mutation =
        C1b4PostViewMutation::None,
    const std::uint32_t initial_dispatch_sequence = 41)
{
    CHECK(transition_mutation != C1b4TransitionMutation::None ||
          post_view_mutation != C1b4PostViewMutation::None);
    RuntimeOwnerAdapterCore adapter{};
    if (c1b4_is_disconnected(source)) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, RuntimeOwnerPhase::LivenessWaiting));
    } else {
        fixture_prepare_connecting_without_pending(adapter);
    }

    const RuntimeOwnerView prepared_core =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core;
    const TrustedReceipt head = c1b4_is_disconnected(source)
        ? make_transport_disconnected_receipt(
              prepared_core.mqtt_session_id,
              prepared_core.mqtt_generation,
              91)
        : make_transport_attempt_failed_receipt(
              prepared_core.mqtt_generation_counter,
              91);
    const TrustedReceipt trailing = c1b4_is_disconnected(source)
        ? make_transport_attempt_failed_receipt(1, 93)
        : make_transport_disconnected_receipt(1, 1, 93);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, head) == TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, trailing) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_accepted_liveness_mask(
        adapter, 0x0f);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, initial_dispatch_sequence);

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_before = adapter.view();
    RuntimeOwnerTransition observed_transition =
        make_c1b4_transport_fault_transition(before.core, source);
    RuntimeOwnerView observed_post_view =
        make_c1b4_transport_fault_post_view(before.core, source);
    const LivenessAttemptToken expected_effect_attempt =
        c1b4_is_disconnected(source)
            ? before.core.active_attempt
            : LivenessAttemptToken{};
    CHECK(observed_transition.effects[0].attempt ==
          expected_effect_attempt);
    CHECK(observed_transition.effects[1].attempt ==
          expected_effect_attempt);
    if (c1b4_is_disconnected(source)) {
        CHECK(before.core.active_attempt != LivenessAttemptToken{});
    }
    mutate_c1b4_transport_fault_transition(
        observed_transition, transition_mutation);
    mutate_c1b4_transport_fault_post_view(
        observed_post_view, post_view_mutation);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, observed_transition);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter, observed_post_view);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    const std::uint32_t expected_ingress =
        before.trusted_slots[before.trusted_head].ingress_sequence;
    CHECK(before.trusted_count == 2);
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count == 0);
    CHECK(before.accepted_liveness_mask == 0x0f);
    CHECK(!before.transport_request_pending);
    CHECK(!before.core_adapter_fatal_latched);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter));

    const RuntimeOwnerPhase expected_result_phase =
        post_view_mutation == C1b4PostViewMutation::UnknownPhase
            ? before.core.phase
            : observed_post_view.phase;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        before.core.phase,
        expected_result_phase,
        expected_ingress);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_after = adapter.view();
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b4_transport_fault_post_view(before.core, source)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));

    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.trusted_high_water == before.trusted_high_water);
    CHECK(after.last_trusted_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.last_trusted_diagnostic_ingress_sequence ==
          before.last_trusted_diagnostic_ingress_sequence);
    CHECK(after.last_trusted_diagnostic_code ==
          before.last_trusted_diagnostic_code);
    CHECK(after.trusted_rejected_full_count ==
          before.trusted_rejected_full_count);
    CHECK(public_after.trusted_stale_count ==
          public_before.trusted_stale_count);
    CHECK(public_after.trusted_duplicate_count ==
          public_before.trusted_duplicate_count);
    CHECK(public_after.trusted_protocol_violation_count ==
          public_before.trusted_protocol_violation_count);
    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    CHECK(after.normal_high_water == before.normal_high_water);
    CHECK(after.last_normal_enqueue_sequence ==
          before.last_normal_enqueue_sequence);
    CHECK(after.normal_coalesced_count ==
          before.normal_coalesced_count);
    CHECK(after.normal_rejected_full_count ==
          before.normal_rejected_full_count);
    for (const RuntimeOwnerAdapterNormalSlotSnapshot slot :
         after.normal_slots) {
        CHECK(normal_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterNormalSlotSnapshot{}));
    }
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.transport_request_pending);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.reason_mask == (1u << 6u));
    CHECK(after.critical.first_ingress_sequence == expected_ingress);
    CHECK(after.critical.last_ingress_sequence == expected_ingress);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    const bool suppress_synthetic_pair =
        post_view_mutation == C1b4PostViewMutation::ShutdownPhase;
    const bool terminal_reserve =
        !suppress_synthetic_pair &&
        initial_dispatch_sequence >= maximum - 4;
    if (suppress_synthetic_pair) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
    } else {
        const std::uint32_t record_sequence = terminal_reserve
            ? maximum - 1
            : initial_dispatch_sequence + 1;
        const std::uint32_t recovery_sequence = terminal_reserve
            ? maximum
            : initial_dispatch_sequence + 2;
        CHECK(after.last_dispatch_sequence == recovery_sequence);
        check_canonical_recovery_pending_pair(
            after,
            record_sequence,
            recovery_sequence,
            RuntimeOwnerFaultCode::InternalInvariant,
            0,
            {});
        CHECK(after.dispatch_fatal_latched == terminal_reserve);
    }
    CHECK(!after.safety_delivery_blocked);
    const RuntimeOwnerFaultCode discarded_fault =
        c1b4_is_disconnected(source)
            ? RuntimeOwnerFaultCode::TransportDisconnected
            : RuntimeOwnerFaultCode::TransportFailure;
    for (const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot :
         after.pending_effect_slots) {
        CHECK(slot.effect.fault_code != discarded_fault);
    }
    CHECK(has_safe_default(public_after.current_dispatch));
    CHECK(has_safe_default(public_after.physical_inflight));
    CHECK(public_after.last_ack_dispatch_sequence ==
          public_before.last_ack_dispatch_sequence);
    CHECK(public_after.physical_inflight_cancel_pending == 0);

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::RecoveryPending,
        !suppress_synthetic_pair,
        submit_count_before + 1);
}

void check_c1b4_transport_fault_dispositions_fail_closed(
    const C1b4TransportFaultSource source)
{
    constexpr std::array<C1b4TransitionMutation, 4> mutations{{
        C1b4TransitionMutation::DispositionUnknown,
        C1b4TransitionMutation::DispositionRejected,
        C1b4TransitionMutation::DispositionAcceptedDuplicate,
        C1b4TransitionMutation::DispositionFailClosed,
    }};
    for (const C1b4TransitionMutation mutation : mutations) {
        check_c1b4_transport_fault_malformed_fallback(source, mutation);
    }
}

void check_c1b4_transport_fault_transition_phases_fail_closed(
    const C1b4TransportFaultSource source)
{
    constexpr std::array<C1b4TransitionMutation, 4> mutations{{
        C1b4TransitionMutation::PhaseBeforeKnownWrong,
        C1b4TransitionMutation::PhaseBeforeUnknown,
        C1b4TransitionMutation::PhaseAfterKnownWrong,
        C1b4TransitionMutation::PhaseAfterUnknown,
    }};
    for (const C1b4TransitionMutation mutation : mutations) {
        check_c1b4_transport_fault_malformed_fallback(source, mutation);
    }
}

void check_c1b4_transport_fault_effect_counts_fail_closed(
    const C1b4TransportFaultSource source)
{
    constexpr std::array<C1b4TransitionMutation, 4> mutations{{
        C1b4TransitionMutation::EffectCountZero,
        C1b4TransitionMutation::EffectCountOne,
        C1b4TransitionMutation::EffectCountThree,
        C1b4TransitionMutation::EffectCountFive,
    }};
    for (const C1b4TransitionMutation mutation : mutations) {
        check_c1b4_transport_fault_malformed_fallback(source, mutation);
    }
}

void check_c1b4_transport_fault_used_slots_fail_closed(
    const C1b4TransportFaultSource source)
{
    constexpr std::array<C1b4TransitionMutation, 12> mutations{{
        C1b4TransitionMutation::Used0Kind,
        C1b4TransitionMutation::Used0Correlation,
        C1b4TransitionMutation::Used0AttemptSession,
        C1b4TransitionMutation::Used0AttemptGeneration,
        C1b4TransitionMutation::Used0AttemptEpoch,
        C1b4TransitionMutation::Used0Fault,
        C1b4TransitionMutation::Used1Kind,
        C1b4TransitionMutation::Used1Correlation,
        C1b4TransitionMutation::Used1AttemptSession,
        C1b4TransitionMutation::Used1AttemptGeneration,
        C1b4TransitionMutation::Used1AttemptEpoch,
        C1b4TransitionMutation::Used1Fault,
    }};
    for (const C1b4TransitionMutation mutation : mutations) {
        check_c1b4_transport_fault_malformed_fallback(source, mutation);
    }
}

void check_c1b4_transport_fault_unused_slot3_fails_closed(
    const C1b4TransportFaultSource source)
{
    constexpr std::array<C1b4TransitionMutation, 6> mutations{{
        C1b4TransitionMutation::Unused3Kind,
        C1b4TransitionMutation::Unused3Correlation,
        C1b4TransitionMutation::Unused3AttemptSession,
        C1b4TransitionMutation::Unused3AttemptGeneration,
        C1b4TransitionMutation::Unused3AttemptEpoch,
        C1b4TransitionMutation::Unused3Fault,
    }};
    for (const C1b4TransitionMutation mutation : mutations) {
        check_c1b4_transport_fault_malformed_fallback(source, mutation);
    }
}

void check_c1b4_transport_fault_post_view_phases_fail_closed(
    const C1b4TransportFaultSource source)
{
    constexpr std::array<C1b4PostViewMutation, 3> mutations{{
        C1b4PostViewMutation::KnownWrongPhase,
        C1b4PostViewMutation::UnknownPhase,
        C1b4PostViewMutation::ShutdownPhase,
    }};
    for (const C1b4PostViewMutation mutation : mutations) {
        check_c1b4_transport_fault_malformed_fallback(
            source, C1b4TransitionMutation::None, mutation);
    }
}

void check_c1b4_transport_fault_post_view_fields_fail_closed(
    const C1b4TransportFaultSource source)
{
    constexpr std::array<C1b4PostViewMutation, 11> mutations{{
        C1b4PostViewMutation::MqttSession,
        C1b4PostViewMutation::MqttGeneration,
        C1b4PostViewMutation::MqttGenerationCounter,
        C1b4PostViewMutation::ConfigApplyEpochCounter,
        C1b4PostViewMutation::LastConfigCommitSequence,
        C1b4PostViewMutation::LastCorrelationId,
        C1b4PostViewMutation::ActiveAttemptSession,
        C1b4PostViewMutation::ActiveAttemptGeneration,
        C1b4PostViewMutation::ActiveAttemptEpoch,
        C1b4PostViewMutation::BootOrchestrationEnded,
        C1b4PostViewMutation::LastFault,
    }};
    for (const C1b4PostViewMutation mutation : mutations) {
        check_c1b4_transport_fault_malformed_fallback(
            source, C1b4TransitionMutation::None, mutation);
    }
}

void test_c1b4_transport_attempt_failed_full_malformed_matrix()
{
    constexpr C1b4TransportFaultSource source =
        C1b4TransportFaultSource::AttemptFailed;
    check_c1b4_transport_fault_dispositions_fail_closed(source);
    check_c1b4_transport_fault_transition_phases_fail_closed(source);
    check_c1b4_transport_fault_effect_counts_fail_closed(source);
    check_c1b4_transport_fault_used_slots_fail_closed(source);
    check_c1b4_transport_fault_unused_slot3_fails_closed(source);
    check_c1b4_transport_fault_post_view_phases_fail_closed(source);
}

void test_c1b4_transport_disconnected_full_malformed_matrix()
{
    constexpr C1b4TransportFaultSource source =
        C1b4TransportFaultSource::Disconnected;
    check_c1b4_transport_fault_dispositions_fail_closed(source);
    check_c1b4_transport_fault_transition_phases_fail_closed(source);
    check_c1b4_transport_fault_effect_counts_fail_closed(source);
    check_c1b4_transport_fault_used_slots_fail_closed(source);
    check_c1b4_transport_fault_unused_slot3_fails_closed(source);
    check_c1b4_transport_fault_post_view_phases_fail_closed(source);
}

void test_c1b4_transport_attempt_failed_post_view_fields_fail_closed()
{
    check_c1b4_transport_fault_post_view_fields_fail_closed(
        C1b4TransportFaultSource::AttemptFailed);
}

void test_c1b4_transport_disconnected_post_view_fields_fail_closed()
{
    check_c1b4_transport_fault_post_view_fields_fail_closed(
        C1b4TransportFaultSource::Disconnected);
}

void check_c1b4_transport_fault_terminal_reserves(
    const C1b4TransportFaultSource source)
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    for (const std::uint32_t start : {
             maximum - 4,
             maximum - 3,
             maximum - 2,
         }) {
        check_c1b4_transport_fault_malformed_fallback(
            source,
            C1b4TransitionMutation::DispositionUnknown,
            C1b4PostViewMutation::None,
            start);
    }
}

void test_c1b4_transport_fault_malformed_terminal_reserves()
{
    check_c1b4_transport_fault_terminal_reserves(
        C1b4TransportFaultSource::AttemptFailed);
    check_c1b4_transport_fault_terminal_reserves(
        C1b4TransportFaultSource::Disconnected);
}

void test_task4c_c1b4_transport_fault_malformed_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    for (const C1b4TransportFaultSource source : {
             C1b4TransportFaultSource::AttemptFailed,
             C1b4TransportFaultSource::Disconnected,
         }) {
        check_c1b4_transport_fault_malformed_fallback(
            source, C1b4TransitionMutation::Used0Fault);
        check_c1b4_transport_fault_malformed_fallback(
            source,
            C1b4TransitionMutation::None,
            C1b4PostViewMutation::LastFault);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void check_operation_completed_accepts(
    RuntimeOwnerAdapterCore &adapter,
    const RuntimeOwnerEffect ticket,
    const bool final_ticket)
{
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::LivenessWaiting);
    if (before.physical_inflight.kind == AdapterDispatchKind::None) {
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(runtime_owner_effects_equal(offered.effect, ticket));
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
    }
    CHECK(runtime_owner_effects_equal(
        adapter.view().physical_inflight.effect, ticket));
    const RuntimeOwnerAdapterPrivateSnapshot authorized_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const TrustedReceipt receipt =
        make_operation_completed_receipt(ticket);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t ingress_sequence =
        adapter.view().last_trusted_ingress_sequence;

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::LivenessWaiting,
        final_ticket ? RuntimeOwnerPhase::SnapshotFreezePending
                     : RuntimeOwnerPhase::LivenessWaiting,
        ingress_sequence);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    std::uint8_t completed_ticket_mask = 0;
    switch (ticket.kind) {
    case RuntimeOwnerEffectKind::StartAtProbe:
        completed_ticket_mask = 1u << 0u;
        break;
    case RuntimeOwnerEffectKind::StartProbePublish:
        completed_ticket_mask = 1u << 1u;
        break;
    case RuntimeOwnerEffectKind::VerifySubscription:
        completed_ticket_mask = 1u << 2u;
        break;
    case RuntimeOwnerEffectKind::PullFollowupConfig:
        completed_ticket_mask = 1u << 3u;
        break;
    default:
        break;
    }
    CHECK(completed_ticket_mask != 0);
    CHECK(after.accepted_liveness_mask ==
          static_cast<std::uint8_t>(
              authorized_before.accepted_liveness_mask |
              completed_ticket_mask));
    RuntimeOwnerView expected_core = authorized_before.core;
    expected_core.phase =
        final_ticket ? RuntimeOwnerPhase::SnapshotFreezePending
                     : RuntimeOwnerPhase::LivenessWaiting;
    CHECK(runtime_owner_views_equal(after.core, expected_core));
    CHECK(after.trusted_count == before.trusted_count);
    CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
          ingress_sequence);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);

    if (!final_ticket) {
        CHECK(after.last_dispatch_sequence ==
              authorized_before.last_dispatch_sequence);
        CHECK(after.pending_effect_count ==
              authorized_before.pending_effect_count);
        return;
    }

    CHECK(after.last_dispatch_sequence ==
          authorized_before.last_dispatch_sequence + 1);
    CHECK(after.pending_effect_count == 1);
    CHECK(after.pending_effect_tail ==
          (after.pending_effect_head + 1) %
              after.pending_effect_slots.size());
    const RuntimeOwnerAdapterPendingEffectSlotSnapshot freeze =
        after.pending_effect_slots[after.pending_effect_head];
    CHECK(freeze.preassigned_dispatch_sequence ==
          authorized_before.last_dispatch_sequence + 1);
    CHECK(freeze.effect.kind ==
          RuntimeOwnerEffectKind::FreezeBootSnapshot);
    CHECK(freeze.effect.correlation_id ==
          authorized_before.core.last_correlation_id - 1);
    CHECK(freeze.effect.attempt == authorized_before.core.active_attempt);
    CHECK(freeze.effect.fault_code == RuntimeOwnerFaultCode::None);
    for (std::size_t offset = 1;
         offset < after.pending_effect_slots.size(); ++offset) {
        const std::size_t slot =
            (after.pending_effect_head + offset) %
            after.pending_effect_slots.size();
        CHECK(pending_effect_slot_snapshots_equal(
            after.pending_effect_slots[slot],
            RuntimeOwnerAdapterPendingEffectSlotSnapshot{}));
    }
}

RuntimeOwnerEffect fixture_prepare_snapshot_freeze_pending_via_config(
    RuntimeOwnerAdapterCore &adapter,
    const bool keep_pending_effect = false)
{
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    for (std::size_t index = 0; index < tickets.size(); ++index) {
        check_operation_completed_accepts(
            adapter, tickets[index], index + 1 == tickets.size());
    }

    const RuntimeOwnerAdapterPrivateSnapshot state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.core.phase == RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(!state.core.boot_orchestration_ended);
    CHECK(state.accepted_liveness_mask == 0x0f);
    CHECK(state.pending_effect_count == 1);
    const RuntimeOwnerEffect freeze =
        state.pending_effect_slots[state.pending_effect_head].effect;
    CHECK(freeze.kind == RuntimeOwnerEffectKind::FreezeBootSnapshot);
    CHECK(freeze.correlation_id == state.core.last_correlation_id - 1);
    CHECK(freeze.attempt == state.core.active_attempt);
    CHECK(freeze.fault_code == RuntimeOwnerFaultCode::None);

    if (!keep_pending_effect) {
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(runtime_owner_effects_equal(offered.effect, freeze));
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
    }
    return freeze;
}

void check_snapshot_succeeded_accepts(
    RuntimeOwnerAdapterCore &adapter,
    const TrustedReceipt receipt)
{
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(!before.core.boot_orchestration_ended);
    CHECK(!before.boot_end_released);
    CHECK(before.accepted_liveness_mask == 0x0f);
    CHECK(before.pending_effect_count == 0);
    CHECK(receipt.kind == TrustedReceiptKind::SnapshotSucceeded);
    CHECK(receipt.diagnostic_code == 0);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t ingress_sequence =
        adapter.view().last_trusted_ingress_sequence;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RuntimeReady,
        ingress_sequence);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerView expected_core = before.core;
    expected_core.phase = RuntimeOwnerPhase::RuntimeReady;
    expected_core.boot_orchestration_ended = true;
    CHECK(runtime_owner_views_equal(after.core, expected_core));
    CHECK(after.trusted_count == before.trusted_count);
    CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
          ingress_sequence);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.pending_effect_count == 1);
    CHECK(after.pending_effect_tail ==
          (after.pending_effect_head + 1) %
              after.pending_effect_slots.size());
    const RuntimeOwnerAdapterPendingEffectSlotSnapshot end_boot =
        after.pending_effect_slots[after.pending_effect_head];
    CHECK(end_boot.preassigned_dispatch_sequence ==
          before.last_dispatch_sequence + 1);
    CHECK(end_boot.effect.kind ==
          RuntimeOwnerEffectKind::EndBootOrchestration);
    CHECK(end_boot.effect.correlation_id == before.core.last_correlation_id);
    CHECK(end_boot.effect.attempt == before.core.active_attempt);
    CHECK(end_boot.effect.fault_code == RuntimeOwnerFaultCode::None);
    check_unused_pending_effect_slots_are_zero(after, 1);
    CHECK(after.last_dispatch_sequence == before.last_dispatch_sequence + 1);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.boot_end_released);
    CHECK(has_safe_default(adapter.view().current_dispatch));
    CHECK(has_safe_default(adapter.view().physical_inflight));
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);
}

void check_snapshot_failed_accepts(
    RuntimeOwnerAdapterCore &adapter,
    const TrustedReceipt receipt,
    const std::uint32_t record_fault_sequence,
    const std::uint32_t enter_recovery_sequence,
    const bool terminal_reserve)
{
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(!before.core.boot_orchestration_ended);
    CHECK(!before.boot_end_released);
    CHECK(before.accepted_liveness_mask == 0x0f);
    CHECK(before.pending_effect_count == 0);
    CHECK(receipt.kind == TrustedReceiptKind::SnapshotFailed);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t ingress_sequence =
        adapter.view().last_trusted_ingress_sequence;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RecoveryPending,
        ingress_sequence);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerView expected_core = before.core;
    expected_core.phase = RuntimeOwnerPhase::RecoveryPending;
    expected_core.mqtt_session_id = 0;
    expected_core.mqtt_generation = 0;
    expected_core.active_attempt = {};
    expected_core.last_fault = RuntimeOwnerFaultCode::SnapshotFailure;
    CHECK(runtime_owner_views_equal(after.core, expected_core));
    CHECK(after.trusted_count == before.trusted_count);
    CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
          ingress_sequence);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.last_dispatch_sequence == enter_recovery_sequence);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.boot_end_released);
    check_canonical_recovery_pending_pair(
        after,
        record_fault_sequence,
        enter_recovery_sequence,
        RuntimeOwnerFaultCode::SnapshotFailure,
        receipt.correlation_id,
        before.core.active_attempt);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.dispatch_fatal_latched == terminal_reserve);
    CHECK(!after.critical_pending);
}

void test_operation_completed_dispatches_config_tickets_serially()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    const AdapterDispatch first = adapter.view().physical_inflight;
    CHECK(runtime_owner_effects_equal(first.effect, tickets[0]));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_operation_completed_receipt(tickets[1])) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.view().trusted_stale_count == 1);
    CHECK(adapter_dispatches_equal(
        adapter.view().physical_inflight, first));

    for (std::size_t index = 0; index < tickets.size(); ++index) {
        check_operation_completed_accepts(
            adapter, tickets[index], index + 1 == tickets.size());
    }
}

void test_operation_completed_immediate_and_non_immediate_duplicates()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        for (std::size_t index = 0; index < tickets.size(); ++index) {
            check_operation_completed_accepts(
                adapter, tickets[index], index + 1 == tickets.size());
        }
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
        const RuntimeOwnerAdapterPrivateSnapshot accepted =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_operation_completed_receipt(tickets[3])) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::SnapshotFreezePending, 0, 1);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(runtime_owner_views_equal(accepted.core, after.core));
        CHECK(after.last_trusted_receipt_signature.ingress_sequence == 6);
        CHECK(trusted_receipts_equal(
            after.last_trusted_receipt_signature.receipt,
            make_operation_completed_receipt(tickets[3])));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        check_operation_completed_accepts(adapter, tickets[0], false);
        check_operation_completed_accepts(adapter, tickets[1], false);
        const RuntimeOwnerAdapterPrivateSnapshot accepted =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_operation_completed_receipt(tickets[0])) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::LivenessWaiting, 0, 1);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(runtime_owner_views_equal(accepted.core, after.core));
        CHECK(after.last_trusted_receipt_signature.ingress_sequence == 4);
        CHECK(trusted_receipts_equal(
            after.last_trusted_receipt_signature.receipt,
            make_operation_completed_receipt(tickets[1])));
    }
}

void test_operation_completed_ticket_field_mismatches_and_phase_are_stale()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    const TrustedReceipt exact =
        make_operation_completed_receipt(tickets[0]);
    std::array<TrustedReceipt, 5> mismatches{{
        exact,
        exact,
        exact,
        exact,
        exact,
    }};
    mismatches[0].effect_kind = tickets[1].kind;
    ++mismatches[1].correlation_id;
    ++mismatches[2].mqtt_session_id;
    ++mismatches[3].mqtt_generation;
    ++mismatches[4].config_apply_epoch;
    for (std::size_t index = 0; index < mismatches.size(); ++index) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, mismatches[index]) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter,
            RuntimeOwnerPhase::LivenessWaiting,
            static_cast<std::uint32_t>(index + 1),
            0);
    }

    RuntimeOwnerAdapterCore cold{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              cold, exact) == TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        cold, RuntimeOwnerPhase::ColdStart, 1, 0);
}

void test_operation_completed_pending_effects_defer_without_dequeue()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter, true);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_operation_completed_receipt(tickets[0])) ==
          TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.prepared_dispatch_sequence ==
          before.pending_effect_slots[before.pending_effect_head]
              .preassigned_dispatch_sequence);
    const AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(runtime_owner_effects_equal(offered.effect, tickets[0]));
    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    CHECK(adapter.step().action ==
          AdapterStepAction::CoreTransitionApplied);
}

void test_operation_completed_sequence_preflight_only_for_final_ticket()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, maximum);
        check_operation_completed_accepts(adapter, tickets[0], false);
        CHECK(adapter.view().last_dispatch_sequence == maximum);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        for (std::size_t index = 0; index < 3; ++index) {
            check_operation_completed_accepts(adapter, tickets[index], false);
        }
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, maximum - 3);
        check_operation_completed_accepts(adapter, tickets[3], true);
        CHECK(adapter.view().last_dispatch_sequence == maximum - 2);
    }
}

void test_operation_completed_final_sequence_shortage_is_bounded()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 3> last_sequences{{
        maximum - 2,
        maximum - 1,
        maximum,
    }};
    for (const std::uint32_t last_sequence : last_sequences) {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        for (std::size_t index = 0; index < 3; ++index) {
            check_operation_completed_accepts(adapter, tickets[index], false);
        }
        CHECK(adapter.step().action ==
              AdapterStepAction::DispatchPrepared);
        const AdapterDispatch final_ticket = adapter.peek_dispatch();
        CHECK(runtime_owner_effects_equal(
            final_ticket.effect, tickets[3]));
        CHECK(adapter.acknowledge_dispatch(
                  final_ticket.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, last_sequence);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_operation_completed_receipt(tickets[3])) ==
              TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::LivenessWaiting,
            RuntimeOwnerPhase::LivenessWaiting);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(runtime_owner_views_equal(before.core, after.core));
        CHECK(after.trusted_count == 1);
        CHECK(after.pending_effect_count == 0);
        CHECK(after.last_dispatch_sequence == last_sequence);
        CHECK(after.last_trusted_receipt_signature.ingress_sequence == 5);
        CHECK(after.critical_pending);
        CHECK(after.critical.first_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(after.critical.last_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(after.critical.first_ingress_sequence == 6);
        CHECK(after.critical.last_ingress_sequence == 6);
        CHECK(after.critical.occurrence_count == 1);

        if (last_sequence == maximum - 2) {
            check_exact_step_result(
                adapter.step(),
                AdapterStepAction::CoreTransitionApplied,
                RuntimeOwnerDisposition::Accepted,
                RuntimeOwnerPhase::LivenessWaiting,
                RuntimeOwnerPhase::RecoveryPending);
            CHECK(adapter.view().critical_pending == 0);
            CHECK(adapter.view().dispatch_fatal_latched == 1);
            CHECK(adapter.view().pending_effect_count == 2);
        } else {
            check_exact_step_result(
                adapter.step(),
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                RuntimeOwnerPhase::LivenessWaiting,
                RuntimeOwnerPhase::LivenessWaiting);
            CHECK(adapter.view().critical_pending == 0);
            CHECK(adapter.view().safety_delivery_blocked == 1);
        }
    }
}

RuntimeOwnerTransition make_c1b3a_canonical_operation_completed_transition(
    const RuntimeOwnerView before,
    const bool final_ticket)
{
    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = RuntimeOwnerPhase::LivenessWaiting;
    transition.phase_after =
        final_ticket ? RuntimeOwnerPhase::SnapshotFreezePending
                     : RuntimeOwnerPhase::LivenessWaiting;
    transition.effect_count = final_ticket ? 1 : 0;
    if (final_ticket) {
        transition.effects[0] = {
            RuntimeOwnerEffectKind::FreezeBootSnapshot,
            before.last_correlation_id - 1,
            before.active_attempt,
            RuntimeOwnerFaultCode::None,
        };
    }
    return transition;
}

enum class C1b3aOperationCompletedSource : std::uint8_t {
    NonFinal = 0,
    Final = 1,
};

enum class C1b3aMalformedExercise : std::uint8_t {
    Validator = 0,
    IntentionalPendingBypass = 1,
};

enum class C1b3aPostViewMutation : std::uint8_t {
    None = 0,
    KnownWrongPhase,
    UnknownPhase,
    ShutdownPhase,
    MqttSession,
    MqttGeneration,
    MqttGenerationCounter,
    ConfigApplyEpochCounter,
    LastConfigCommitSequence,
    LastCorrelationId,
    ActiveAttemptSession,
    ActiveAttemptGeneration,
    ActiveAttemptEpoch,
    BootOrchestrationEnded,
    LastFault,
};

constexpr bool c1b3a_is_final(
    const C1b3aOperationCompletedSource source) noexcept
{
    return source == C1b3aOperationCompletedSource::Final;
}

constexpr RuntimeOwnerPhase c1b3a_actual_phase_after(
    const C1b3aOperationCompletedSource source) noexcept
{
    return c1b3a_is_final(source)
        ? RuntimeOwnerPhase::SnapshotFreezePending
        : RuntimeOwnerPhase::LivenessWaiting;
}

RuntimeOwnerView make_c1b3a_canonical_operation_completed_post_view(
    const RuntimeOwnerView before,
    const C1b3aOperationCompletedSource source)
{
    RuntimeOwnerView after = before;
    after.phase = c1b3a_actual_phase_after(source);
    return after;
}

void mutate_c1b3a_post_view(
    RuntimeOwnerView &view,
    const C1b3aPostViewMutation mutation)
{
    switch (mutation) {
    case C1b3aPostViewMutation::None:
        break;
    case C1b3aPostViewMutation::KnownWrongPhase:
        view.phase = RuntimeOwnerPhase::RuntimeReady;
        break;
    case C1b3aPostViewMutation::UnknownPhase:
        view.phase = static_cast<RuntimeOwnerPhase>(255);
        break;
    case C1b3aPostViewMutation::ShutdownPhase:
        view.phase = RuntimeOwnerPhase::ShutdownCommitted;
        break;
    case C1b3aPostViewMutation::MqttSession:
        ++view.mqtt_session_id;
        break;
    case C1b3aPostViewMutation::MqttGeneration:
        ++view.mqtt_generation;
        break;
    case C1b3aPostViewMutation::MqttGenerationCounter:
        ++view.mqtt_generation_counter;
        break;
    case C1b3aPostViewMutation::ConfigApplyEpochCounter:
        ++view.config_apply_epoch_counter;
        break;
    case C1b3aPostViewMutation::LastConfigCommitSequence:
        ++view.last_config_commit_sequence;
        break;
    case C1b3aPostViewMutation::LastCorrelationId:
        ++view.last_correlation_id;
        break;
    case C1b3aPostViewMutation::ActiveAttemptSession:
        ++view.active_attempt.mqtt_session_id;
        break;
    case C1b3aPostViewMutation::ActiveAttemptGeneration:
        ++view.active_attempt.mqtt_generation;
        break;
    case C1b3aPostViewMutation::ActiveAttemptEpoch:
        ++view.active_attempt.config_apply_epoch;
        break;
    case C1b3aPostViewMutation::BootOrchestrationEnded:
        view.boot_orchestration_ended =
            !view.boot_orchestration_ended;
        break;
    case C1b3aPostViewMutation::LastFault:
        view.last_fault = RuntimeOwnerFaultCode::InternalInvariant;
        break;
    }
}

void fixture_prepare_c1b3a_operation_completed_source(
    RuntimeOwnerAdapterCore &adapter,
    const C1b3aOperationCompletedSource source,
    RuntimeOwnerEffect &target_ticket)
{
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    if (c1b3a_is_final(source)) {
        for (std::size_t index = 0; index < 3; ++index) {
            check_operation_completed_accepts(
                adapter, tickets[index], false);
        }
        target_ticket = tickets[3];
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(runtime_owner_effects_equal(offered.effect, target_ticket));
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        return;
    }
    target_ticket = tickets[0];
}

RuntimeOwnerTransition make_c1b3a_canonical_transition_for_source(
    const C1b3aOperationCompletedSource source)
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerEffect target_ticket{};
    fixture_prepare_c1b3a_operation_completed_source(
        adapter, source, target_ticket);
    return make_c1b3a_canonical_operation_completed_transition(
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core,
        c1b3a_is_final(source));
}

void check_c1b3a_operation_completed_malformed_fallback(
    const C1b3aOperationCompletedSource source,
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41,
    const C1b3aPostViewMutation post_view_mutation =
        C1b3aPostViewMutation::None,
    const C1b3aMalformedExercise exercise =
        C1b3aMalformedExercise::Validator)
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerEffect target_ticket{};
    fixture_prepare_c1b3a_operation_completed_source(
        adapter, source, target_ticket);
    const TrustedReceipt head =
        make_operation_completed_receipt(target_ticket);
    constexpr TrustedReceipt trailing =
        make_transport_attempt_failed_receipt(1, 91);
    const std::uint32_t expected_head_ingress =
        adapter.view().last_trusted_ingress_sequence + 1;
    const std::uint32_t expected_trailing_ingress =
        expected_head_ingress + 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, head) == TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, trailing) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    if (exercise == C1b3aMalformedExercise::Validator) {
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
    }
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_accepted_liveness_mask(
        adapter, c1b3a_is_final(source) ? 0x07 : 0x0a);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, initial_dispatch_sequence);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_before = adapter.view();
    RuntimeOwnerView observed_post_view =
        make_c1b3a_canonical_operation_completed_post_view(
            before.core, source);
    if (post_view_mutation != C1b3aPostViewMutation::None) {
        mutate_c1b3a_post_view(
            observed_post_view, post_view_mutation);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter, observed_post_view);
    }
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(before.trusted_count == 2);
    CHECK(before.trusted_slots[before.trusted_head].ingress_sequence ==
          expected_head_ingress);
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count ==
          (exercise == C1b3aMalformedExercise::Validator ? 0 : 1));
    if (exercise == C1b3aMalformedExercise::Validator) {
        CHECK(initial_dispatch_sequence == 41);
    }
    CHECK(before.accepted_liveness_mask ==
          (c1b3a_is_final(source) ? 0x07 : 0x0a));
    CHECK(before.last_trusted_receipt_signature.ingress_sequence == 67);
    CHECK(before.last_trusted_diagnostic_ingress_sequence ==
          expected_trailing_ingress);
    CHECK(before.last_trusted_diagnostic_code == 91);
    CHECK(public_before.trusted_stale_count == 73);
    CHECK(public_before.trusted_duplicate_count == 79);
    CHECK(public_before.trusted_protocol_violation_count == 83);
    CHECK(!before.core_fail_closed_latched);
    CHECK(!before.core_adapter_fatal_latched);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter) ==
          (post_view_mutation != C1b3aPostViewMutation::None));

    const RuntimeOwnerPhase expected_result_phase =
        post_view_mutation == C1b3aPostViewMutation::UnknownPhase
            ? RuntimeOwnerPhase::LivenessWaiting
            : observed_post_view.phase;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::LivenessWaiting,
        expected_result_phase,
        expected_head_ingress);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_after = adapter.view();
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b3a_canonical_operation_completed_post_view(
            before.core, source)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    const bool expected_sequence_bypass =
        c1b3a_is_final(source) &&
        initial_dispatch_sequence >
            std::numeric_limits<std::uint32_t>::max() - 3;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_operation_completed_validation_bypass_used(
                  adapter) ==
          expected_sequence_bypass);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));

    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.trusted_high_water == before.trusted_high_water);
    CHECK(after.last_trusted_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.last_trusted_diagnostic_ingress_sequence ==
          before.last_trusted_diagnostic_ingress_sequence);
    CHECK(after.last_trusted_diagnostic_code ==
          before.last_trusted_diagnostic_code);
    CHECK(public_after.trusted_stale_count ==
          public_before.trusted_stale_count);
    CHECK(public_after.trusted_duplicate_count ==
          public_before.trusted_duplicate_count);
    CHECK(public_after.trusted_protocol_violation_count ==
          public_before.trusted_protocol_violation_count);

    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    CHECK(after.normal_high_water == before.normal_high_water);
    CHECK(after.last_normal_enqueue_sequence ==
          before.last_normal_enqueue_sequence);
    CHECK(after.normal_coalesced_count ==
          before.normal_coalesced_count);
    CHECK(after.normal_rejected_full_count ==
          before.normal_rejected_full_count);
    for (const RuntimeOwnerAdapterNormalSlotSnapshot slot :
         after.normal_slots) {
        CHECK(normal_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterNormalSlotSnapshot{}));
    }
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.transport_request_pending);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.reason_mask == (1u << 6u));
    CHECK(after.critical.first_ingress_sequence ==
          expected_head_ingress);
    CHECK(after.critical.last_ingress_sequence ==
          expected_head_ingress);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    const bool suppress_synthetic_pair =
        post_view_mutation == C1b3aPostViewMutation::ShutdownPhase;
    const bool safety_blocked =
        !suppress_synthetic_pair &&
        initial_dispatch_sequence >= maximum - 1;
    const bool terminal_reserve =
        !suppress_synthetic_pair && !safety_blocked &&
        initial_dispatch_sequence >= maximum - 4;
    if (suppress_synthetic_pair || safety_blocked) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(after.safety_delivery_blocked == safety_blocked);
    } else {
        const std::uint32_t record_sequence = terminal_reserve
            ? maximum - 1
            : initial_dispatch_sequence + 1;
        const std::uint32_t recovery_sequence = terminal_reserve
            ? maximum
            : initial_dispatch_sequence + 2;
        CHECK(after.last_dispatch_sequence == recovery_sequence);
        check_canonical_recovery_pending_pair(
            after,
            record_sequence,
            recovery_sequence,
            RuntimeOwnerFaultCode::InternalInvariant,
            0,
            {});
        CHECK(after.dispatch_fatal_latched == terminal_reserve);
        CHECK(!after.safety_delivery_blocked);
    }
    for (const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot :
         after.pending_effect_slots) {
        CHECK(slot.effect.kind !=
              RuntimeOwnerEffectKind::FreezeBootSnapshot);
    }
    CHECK(has_safe_default(public_after.current_dispatch));
    CHECK(has_safe_default(public_after.physical_inflight));
    CHECK(public_after.last_ack_dispatch_sequence ==
          public_before.last_ack_dispatch_sequence);
    CHECK(public_after.physical_inflight_cancel_pending == 0);

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        c1b3a_actual_phase_after(source),
        !suppress_synthetic_pair && !safety_blocked,
        submit_count_before + 1);
}

void check_c1b3a_operation_completed_intentional_pending_bypass_fallback(
    const C1b3aOperationCompletedSource source,
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41)
{
    RuntimeOwnerTransition exercised = malformed;
    exercised.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b3a_operation_completed_malformed_fallback(
        source,
        exercised,
        initial_dispatch_sequence,
        C1b3aPostViewMutation::None,
        C1b3aMalformedExercise::IntentionalPendingBypass);
}

void test_c1b3a_operation_completed_nonfinal_unknown_disposition_fails_closed()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerTransition malformed =
        make_c1b3a_canonical_operation_completed_transition(
            before.core, false);
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_operation_completed_receipt(tickets[0])) ==
          TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::LivenessWaiting,
        3);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_last_operation_completed_validation_bypass_used(
                   adapter));
}

void test_c1b3a_operation_completed_final_wrong_count_fails_closed()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    for (std::size_t index = 0; index < 3; ++index) {
        check_operation_completed_accepts(adapter, tickets[index], false);
    }
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch final_ticket = adapter.peek_dispatch();
    CHECK(runtime_owner_effects_equal(final_ticket.effect, tickets[3]));
    CHECK(adapter.acknowledge_dispatch(final_ticket.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerTransition malformed =
        make_c1b3a_canonical_operation_completed_transition(
            before.core, true);
    malformed.effect_count = 0;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_operation_completed_receipt(tickets[3])) ==
          TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::SnapshotFreezePending,
        6);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_last_operation_completed_validation_bypass_used(
                   adapter));
}

void test_c1b3a_operation_completed_nonfinal_dispositions_fail_closed()
{
    constexpr std::array<RuntimeOwnerDisposition, 4> corruptions{{
        static_cast<RuntimeOwnerDisposition>(255),
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3a_canonical_transition_for_source(
                C1b3aOperationCompletedSource::NonFinal);
        malformed.disposition = disposition;
        check_c1b3a_operation_completed_malformed_fallback(
            C1b3aOperationCompletedSource::NonFinal, malformed);
    }
}

void test_c1b3a_operation_completed_final_dispositions_fail_closed()
{
    constexpr std::array<RuntimeOwnerDisposition, 4> corruptions{{
        static_cast<RuntimeOwnerDisposition>(255),
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3a_canonical_transition_for_source(
                C1b3aOperationCompletedSource::Final);
        malformed.disposition = disposition;
        check_c1b3a_operation_completed_malformed_fallback(
            C1b3aOperationCompletedSource::Final, malformed);
    }
}

void check_c1b3a_operation_completed_transition_phases_fail_closed(
    const C1b3aOperationCompletedSource source)
{
    std::array<RuntimeOwnerTransition, 4> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_c1b3a_canonical_transition_for_source(source);
    }
    corruptions[0].phase_before = RuntimeOwnerPhase::ColdStart;
    corruptions[1].phase_before = static_cast<RuntimeOwnerPhase>(255);
    corruptions[2].phase_after = RuntimeOwnerPhase::RuntimeReady;
    corruptions[3].phase_after = static_cast<RuntimeOwnerPhase>(255);
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b3a_operation_completed_malformed_fallback(
            source, malformed);
    }
}

void test_c1b3a_operation_completed_nonfinal_transition_phases_fail_closed()
{
    check_c1b3a_operation_completed_transition_phases_fail_closed(
        C1b3aOperationCompletedSource::NonFinal);
}

void test_c1b3a_operation_completed_final_transition_phases_fail_closed()
{
    check_c1b3a_operation_completed_transition_phases_fail_closed(
        C1b3aOperationCompletedSource::Final);
}

void test_c1b3a_operation_completed_nonfinal_effect_counts_fail_closed()
{
    constexpr std::array<std::uint8_t, 4> corruptions{{1, 2, 4, 5}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3a_canonical_transition_for_source(
                C1b3aOperationCompletedSource::NonFinal);
        malformed.effect_count = effect_count;
        check_c1b3a_operation_completed_malformed_fallback(
            C1b3aOperationCompletedSource::NonFinal, malformed);
    }
}

void test_c1b3a_operation_completed_final_effect_counts_fail_closed()
{
    constexpr std::array<std::uint8_t, 4> corruptions{{0, 2, 4, 5}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3a_canonical_transition_for_source(
                C1b3aOperationCompletedSource::Final);
        malformed.effect_count = effect_count;
        check_c1b3a_operation_completed_malformed_fallback(
            C1b3aOperationCompletedSource::Final, malformed);
    }
}

void corrupt_c1b3a_effect_field(
    RuntimeOwnerEffect &effect,
    const std::size_t field_index,
    const bool used)
{
    switch (field_index) {
    case 0:
        effect.kind = used ? RuntimeOwnerEffectKind::StartAtProbe
                           : RuntimeOwnerEffectKind::FreezeBootSnapshot;
        break;
    case 1:
        ++effect.correlation_id;
        break;
    case 2:
        ++effect.attempt.mqtt_session_id;
        break;
    case 3:
        ++effect.attempt.mqtt_generation;
        break;
    case 4:
        ++effect.attempt.config_apply_epoch;
        break;
    case 5:
        effect.fault_code = RuntimeOwnerFaultCode::InternalInvariant;
        break;
    default:
        break;
    }
}

void check_c1b3a_operation_completed_effect_slot_fields_fail_closed(
    const C1b3aOperationCompletedSource source,
    const std::size_t slot_index,
    const bool used)
{
    for (std::size_t field_index = 0; field_index < 6; ++field_index) {
        RuntimeOwnerTransition malformed =
            make_c1b3a_canonical_transition_for_source(source);
        corrupt_c1b3a_effect_field(
            malformed.effects[slot_index], field_index, used);
        check_c1b3a_operation_completed_malformed_fallback(
            source, malformed);
    }
}

void test_c1b3a_operation_completed_nonfinal_unused_slot0_fields_fail_closed()
{
    check_c1b3a_operation_completed_effect_slot_fields_fail_closed(
        C1b3aOperationCompletedSource::NonFinal, 0, false);
}

void test_c1b3a_operation_completed_nonfinal_unused_slot3_fields_fail_closed()
{
    check_c1b3a_operation_completed_effect_slot_fields_fail_closed(
        C1b3aOperationCompletedSource::NonFinal, 3, false);
}

void test_c1b3a_operation_completed_final_used_slot0_fields_fail_closed()
{
    check_c1b3a_operation_completed_effect_slot_fields_fail_closed(
        C1b3aOperationCompletedSource::Final, 0, true);
}

void test_c1b3a_operation_completed_final_unused_slot3_fields_fail_closed()
{
    check_c1b3a_operation_completed_effect_slot_fields_fail_closed(
        C1b3aOperationCompletedSource::Final, 3, false);
}

void check_c1b3a_operation_completed_post_view_phases_fail_closed(
    const C1b3aOperationCompletedSource source)
{
    for (const C1b3aPostViewMutation mutation : {
             C1b3aPostViewMutation::KnownWrongPhase,
             C1b3aPostViewMutation::UnknownPhase,
             C1b3aPostViewMutation::ShutdownPhase,
         }) {
        check_c1b3a_operation_completed_malformed_fallback(
            source,
            make_c1b3a_canonical_transition_for_source(source),
            41,
            mutation);
    }
}

void test_c1b3a_operation_completed_nonfinal_post_view_phases_fail_closed()
{
    check_c1b3a_operation_completed_post_view_phases_fail_closed(
        C1b3aOperationCompletedSource::NonFinal);
}

void test_c1b3a_operation_completed_final_post_view_phases_fail_closed()
{
    check_c1b3a_operation_completed_post_view_phases_fail_closed(
        C1b3aOperationCompletedSource::Final);
}

void check_c1b3a_operation_completed_post_view_fields_fail_closed(
    const C1b3aOperationCompletedSource source)
{
    constexpr std::array<C1b3aPostViewMutation, 11> mutations{{
        C1b3aPostViewMutation::MqttSession,
        C1b3aPostViewMutation::MqttGeneration,
        C1b3aPostViewMutation::MqttGenerationCounter,
        C1b3aPostViewMutation::ConfigApplyEpochCounter,
        C1b3aPostViewMutation::LastConfigCommitSequence,
        C1b3aPostViewMutation::LastCorrelationId,
        C1b3aPostViewMutation::ActiveAttemptSession,
        C1b3aPostViewMutation::ActiveAttemptGeneration,
        C1b3aPostViewMutation::ActiveAttemptEpoch,
        C1b3aPostViewMutation::BootOrchestrationEnded,
        C1b3aPostViewMutation::LastFault,
    }};
    for (const C1b3aPostViewMutation mutation : mutations) {
        check_c1b3a_operation_completed_malformed_fallback(
            source,
            make_c1b3a_canonical_transition_for_source(source),
            41,
            mutation);
    }
}

void test_c1b3a_operation_completed_nonfinal_post_view_fields_fail_closed()
{
    check_c1b3a_operation_completed_post_view_fields_fail_closed(
        C1b3aOperationCompletedSource::NonFinal);
}

void test_c1b3a_operation_completed_final_post_view_fields_fail_closed()
{
    check_c1b3a_operation_completed_post_view_fields_fail_closed(
        C1b3aOperationCompletedSource::Final);
}

void check_c1b3a_operation_completed_sequence_reserves_and_damage(
    const C1b3aOperationCompletedSource source)
{
    RuntimeOwnerTransition malformed =
        make_c1b3a_canonical_transition_for_source(source);
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b3a_operation_completed_intentional_pending_bypass_fallback(
        source, malformed, 41);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    for (const std::uint32_t terminal_start : {
             maximum - 4,
             maximum - 3,
             maximum - 2,
         }) {
        check_c1b3a_operation_completed_intentional_pending_bypass_fallback(
            source, malformed, terminal_start);
    }
    for (const std::uint32_t damaged_start : {
             maximum - 1,
             maximum,
         }) {
        check_c1b3a_operation_completed_intentional_pending_bypass_fallback(
            source, malformed, damaged_start);
    }
}

void test_c1b3a_operation_completed_nonfinal_sequence_reserves_and_damage()
{
    check_c1b3a_operation_completed_sequence_reserves_and_damage(
        C1b3aOperationCompletedSource::NonFinal);
}

void test_c1b3a_operation_completed_final_sequence_reserves_and_damage()
{
    check_c1b3a_operation_completed_sequence_reserves_and_damage(
        C1b3aOperationCompletedSource::Final);
}

void test_c1b3a_review_final_canonical_override_pending_bypass_fails_closed()
{
    check_c1b3a_operation_completed_intentional_pending_bypass_fallback(
        C1b3aOperationCompletedSource::Final,
        make_c1b3a_canonical_transition_for_source(
            C1b3aOperationCompletedSource::Final));
}

void test_c1b3a_review_nonfinal_canonical_override_pending_bypass_fails_closed()
{
    check_c1b3a_operation_completed_intentional_pending_bypass_fallback(
        C1b3aOperationCompletedSource::NonFinal,
        make_c1b3a_canonical_transition_for_source(
            C1b3aOperationCompletedSource::NonFinal));
}

void test_c1b3a_review_final_canonical_override_max_sequence_bypass_fails_closed()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerEffect target_ticket{};
    fixture_prepare_c1b3a_operation_completed_source(
        adapter,
        C1b3aOperationCompletedSource::Final,
        target_ticket);
    const TrustedReceipt receipt =
        make_operation_completed_receipt(target_ticket);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, maximum);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_c1b3a_canonical_operation_completed_transition(
            before.core, true));
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter,
            make_c1b3a_canonical_operation_completed_post_view(
                before.core,
                C1b3aOperationCompletedSource::Final));

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::SnapshotFreezePending,
        before.last_trusted_ingress_sequence);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_operation_completed_validation_bypass_used(
                  adapter));
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b3a_canonical_operation_completed_post_view(
            before.core,
            C1b3aOperationCompletedSource::Final)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.pending_effect_count == 0);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 0);
    check_unused_pending_effect_slots_are_zero(after, 0);
    CHECK(after.last_dispatch_sequence == maximum);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.safety_delivery_blocked);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.first_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::SnapshotFreezePending,
        false,
        submit_count_before + 1);
}

void check_c1b3a_operation_completed_valid_override(
    const C1b3aOperationCompletedSource source)
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerEffect target_ticket{};
    fixture_prepare_c1b3a_operation_completed_source(
        adapter, source, target_ticket);
    const TrustedReceipt receipt =
        make_operation_completed_receipt(target_ticket);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_c1b3a_canonical_operation_completed_transition(
            before.core, c1b3a_is_final(source)));
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter,
            make_c1b3a_canonical_operation_completed_post_view(
                before.core, source));

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::LivenessWaiting,
        c1b3a_actual_phase_after(source),
        before.last_trusted_ingress_sequence);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_last_operation_completed_validation_bypass_used(
                   adapter));
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b3a_canonical_operation_completed_post_view(
            before.core, source)));
    CHECK(after.trusted_count == 0);
    CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.accepted_liveness_mask ==
          (c1b3a_is_final(source) ? 0x0f : 0x01));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
    CHECK(!after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.critical_pending);
    if (c1b3a_is_final(source)) {
        CHECK(after.last_dispatch_sequence ==
              before.last_dispatch_sequence + 1);
        CHECK(after.pending_effect_count == 1);
        CHECK(after.pending_effect_slots[0].effect.kind ==
              RuntimeOwnerEffectKind::FreezeBootSnapshot);
    } else {
        CHECK(after.last_dispatch_sequence ==
              before.last_dispatch_sequence);
        CHECK(after.pending_effect_count ==
              before.pending_effect_count);
    }
}

void test_c1b3a_operation_completed_valid_overrides_preserve_paths()
{
    check_c1b3a_operation_completed_valid_override(
        C1b3aOperationCompletedSource::NonFinal);
    check_c1b3a_operation_completed_valid_override(
        C1b3aOperationCompletedSource::Final);
}

void test_c1b3a_operation_completed_override_requires_full_authorization()
{
    enum class Mismatch : std::uint8_t {
        ReceiptKind = 0,
        Phase,
        BootEnded,
        EffectKind,
        Correlation,
        Session,
        Generation,
        Epoch,
        ExactSignature,
        CompletedMask,
    };
    constexpr std::array<Mismatch, 10> mismatches{{
        Mismatch::ReceiptKind,
        Mismatch::Phase,
        Mismatch::BootEnded,
        Mismatch::EffectKind,
        Mismatch::Correlation,
        Mismatch::Session,
        Mismatch::Generation,
        Mismatch::Epoch,
        Mismatch::ExactSignature,
        Mismatch::CompletedMask,
    }};
    for (const Mismatch mismatch : mismatches) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerPhase prepared_phase =
            mismatch == Mismatch::Phase
                ? RuntimeOwnerPhase::SnapshotFreezePending
                : RuntimeOwnerPhase::LivenessWaiting;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, prepared_phase));
        const RuntimeOwnerView issued_view =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core;
        CHECK(issued_view.last_correlation_id >= 6);
        const RuntimeOwnerEffect target_ticket{
            RuntimeOwnerEffectKind::StartAtProbe,
            issued_view.last_correlation_id - 5,
            issued_view.active_attempt,
            RuntimeOwnerFaultCode::None,
        };
        if (mismatch == Mismatch::BootEnded) {
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_set_core_boot_orchestration_ended(adapter, true);
        }
        TrustedReceipt receipt =
            make_operation_completed_receipt(target_ticket);
        switch (mismatch) {
        case Mismatch::ReceiptKind:
            receipt.kind = static_cast<TrustedReceiptKind>(255);
            break;
        case Mismatch::Phase:
            break;
        case Mismatch::BootEnded:
            break;
        case Mismatch::EffectKind:
            receipt.effect_kind =
                RuntimeOwnerEffectKind::StartProbePublish;
            break;
        case Mismatch::Correlation:
            ++receipt.correlation_id;
            break;
        case Mismatch::Session:
            ++receipt.mqtt_session_id;
            break;
        case Mismatch::Generation:
            ++receipt.mqtt_generation;
            break;
        case Mismatch::Epoch:
            ++receipt.config_apply_epoch;
            break;
        case Mismatch::ExactSignature:
            break;
        case Mismatch::CompletedMask:
            break;
        }
        if (mismatch == Mismatch::ReceiptKind) {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::
                      fixture_enqueue_trusted_receipt_unchecked(
                          adapter, receipt));
        } else {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, receipt) == TrustedEnqueueResult::Accepted);
        }
        const std::uint32_t head_ingress_sequence =
            adapter.view().last_trusted_ingress_sequence;
        if (mismatch == Mismatch::ExactSignature) {
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_set_last_trusted_receipt_signature(
                    adapter, head_ingress_sequence, receipt);
        }
        if (mismatch == Mismatch::CompletedMask) {
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_set_accepted_liveness_mask(adapter, 0x01);
        }
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        const RuntimeOwnerAdapterPrivateSnapshot seeded =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(
                adapter,
                make_c1b3a_canonical_operation_completed_transition(
                    seeded.core, false));
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter,
                make_c1b3a_canonical_operation_completed_post_view(
                    seeded.core,
                    C1b3aOperationCompletedSource::NonFinal));
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);
        CHECK(before.trusted_count == 1);
        CHECK(before.pending_effect_count == 1);
        CHECK(before.core.phase == prepared_phase);
        CHECK(before.core.boot_orchestration_ended ==
              (mismatch == Mismatch::BootEnded));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_transition_override_pending(adapter));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_post_submit_view_override_pending(adapter));
        CHECK(submit_count_before == 0);

        const RuntimeOwnerAdapterTrustedSlotSnapshot head =
            before.trusted_slots[before.trusted_head];
        CHECK(head.payload_kind == 1);
        CHECK(head.ingress_sequence == head_ingress_sequence);
        CHECK(trusted_receipts_equal(head.receipt, receipt));
        const bool exact_receipt_kind =
            head.receipt.kind == TrustedReceiptKind::OperationCompleted;
        const bool exact_phase =
            before.core.phase == RuntimeOwnerPhase::LivenessWaiting;
        const bool boot_active =
            !before.core.boot_orchestration_ended;
        const bool exact_effect_kind =
            head.receipt.effect_kind == target_ticket.kind;
        const bool exact_correlation =
            head.receipt.correlation_id == target_ticket.correlation_id;
        const bool exact_session =
            head.receipt.mqtt_session_id ==
            target_ticket.attempt.mqtt_session_id;
        const bool exact_generation =
            head.receipt.mqtt_generation ==
            target_ticket.attempt.mqtt_generation;
        const bool exact_epoch =
            head.receipt.config_apply_epoch ==
            target_ticket.attempt.config_apply_epoch;
        const bool signature_clear =
            before.last_trusted_receipt_signature.ingress_sequence == 0 ||
            !trusted_receipts_equal(
                head.receipt,
                before.last_trusted_receipt_signature.receipt);
        const bool completed_mask_clear =
            (before.accepted_liveness_mask & 0x01) == 0;
        const std::size_t false_authorization_terms =
            static_cast<std::size_t>(!exact_receipt_kind) +
            static_cast<std::size_t>(!exact_phase) +
            static_cast<std::size_t>(!boot_active) +
            static_cast<std::size_t>(!exact_effect_kind) +
            static_cast<std::size_t>(!exact_correlation) +
            static_cast<std::size_t>(!exact_session) +
            static_cast<std::size_t>(!exact_generation) +
            static_cast<std::size_t>(!exact_epoch) +
            static_cast<std::size_t>(!signature_clear) +
            static_cast<std::size_t>(!completed_mask_clear);
        CHECK(false_authorization_terms == 1);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Idle,
            RuntimeOwnerDisposition::Rejected,
            before.core.phase,
            before.core.phase);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_transition_override_pending(adapter));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_post_submit_view_override_pending(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_last_operation_completed_validation_bypass_used(
                       adapter));
    }
}

void test_task4c_c1b3a_operation_completed_malformed_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    for (const C1b3aOperationCompletedSource source : {
             C1b3aOperationCompletedSource::NonFinal,
             C1b3aOperationCompletedSource::Final,
         }) {
        RuntimeOwnerTransition malformed =
            make_c1b3a_canonical_transition_for_source(source);
        malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
        check_c1b3a_operation_completed_malformed_fallback(
            source, malformed);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_operation_completed_mask_resets_on_accepted_recovery()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    check_operation_completed_accepts(adapter, tickets[0], false);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(77, 1)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
              .accepted_liveness_mask == 0);
    RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_operation_completed_receipt(tickets[0])) ==
          TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RecoveryPending, 1, 0);
}

constexpr RuntimeOwnerFaultCode liveness_failure_fault(
    const TrustedReceiptKind kind) noexcept
{
    return kind == TrustedReceiptKind::OperationFailed
        ? RuntimeOwnerFaultCode::LivenessFailure
        : RuntimeOwnerFaultCode::DeadlineExpired;
}

enum class C1b3bLivenessFailureSource : std::uint8_t {
    OperationFailed = 0,
    DeadlineExpired = 1,
};

enum class C1b3bMalformedExercise : std::uint8_t {
    Validator = 0,
    IntentionalPendingBypass = 1,
};

enum class C1b3bPostViewMutation : std::uint8_t {
    None = 0,
    KnownWrongPhase,
    UnknownPhase,
    ShutdownPhase,
    MqttSession,
    MqttGeneration,
    MqttGenerationCounter,
    ConfigApplyEpochCounter,
    LastConfigCommitSequence,
    LastCorrelationId,
    ActiveAttemptSession,
    ActiveAttemptGeneration,
    ActiveAttemptEpoch,
    BootOrchestrationEnded,
    LastFault,
};

constexpr TrustedReceiptKind c1b3b_outcome_kind(
    const C1b3bLivenessFailureSource source) noexcept
{
    return source == C1b3bLivenessFailureSource::OperationFailed
        ? TrustedReceiptKind::OperationFailed
        : TrustedReceiptKind::DeadlineExpired;
}

RuntimeOwnerTransition make_c1b3b_canonical_liveness_failure_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt)
{
    const RuntimeOwnerFaultCode fault =
        liveness_failure_fault(receipt.kind);
    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = RuntimeOwnerPhase::LivenessWaiting;
    transition.phase_after = RuntimeOwnerPhase::RecoveryPending;
    transition.effect_count = 2;
    transition.effects[0] = {
        RuntimeOwnerEffectKind::RecordFault,
        receipt.correlation_id,
        before.active_attempt,
        fault,
    };
    transition.effects[1] = {
        RuntimeOwnerEffectKind::EnterRecovery,
        receipt.correlation_id,
        before.active_attempt,
        fault,
    };
    return transition;
}

RuntimeOwnerView make_c1b3b_canonical_liveness_failure_post_view(
    const RuntimeOwnerView before,
    const C1b3bLivenessFailureSource source)
{
    RuntimeOwnerView after = before;
    after.phase = RuntimeOwnerPhase::RecoveryPending;
    after.mqtt_session_id = 0;
    after.mqtt_generation = 0;
    after.active_attempt = {};
    after.last_fault = liveness_failure_fault(
        c1b3b_outcome_kind(source));
    return after;
}

void mutate_c1b3b_post_view(
    RuntimeOwnerView &view,
    const C1b3bPostViewMutation mutation)
{
    switch (mutation) {
    case C1b3bPostViewMutation::None:
        break;
    case C1b3bPostViewMutation::KnownWrongPhase:
        view.phase = RuntimeOwnerPhase::RuntimeReady;
        break;
    case C1b3bPostViewMutation::UnknownPhase:
        view.phase = static_cast<RuntimeOwnerPhase>(255);
        break;
    case C1b3bPostViewMutation::ShutdownPhase:
        view.phase = RuntimeOwnerPhase::ShutdownCommitted;
        break;
    case C1b3bPostViewMutation::MqttSession:
        ++view.mqtt_session_id;
        break;
    case C1b3bPostViewMutation::MqttGeneration:
        ++view.mqtt_generation;
        break;
    case C1b3bPostViewMutation::MqttGenerationCounter:
        ++view.mqtt_generation_counter;
        break;
    case C1b3bPostViewMutation::ConfigApplyEpochCounter:
        ++view.config_apply_epoch_counter;
        break;
    case C1b3bPostViewMutation::LastConfigCommitSequence:
        ++view.last_config_commit_sequence;
        break;
    case C1b3bPostViewMutation::LastCorrelationId:
        ++view.last_correlation_id;
        break;
    case C1b3bPostViewMutation::ActiveAttemptSession:
        ++view.active_attempt.mqtt_session_id;
        break;
    case C1b3bPostViewMutation::ActiveAttemptGeneration:
        ++view.active_attempt.mqtt_generation;
        break;
    case C1b3bPostViewMutation::ActiveAttemptEpoch:
        ++view.active_attempt.config_apply_epoch;
        break;
    case C1b3bPostViewMutation::BootOrchestrationEnded:
        view.boot_orchestration_ended =
            !view.boot_orchestration_ended;
        break;
    case C1b3bPostViewMutation::LastFault:
        view.last_fault = RuntimeOwnerFaultCode::InternalInvariant;
        break;
    }
}

RuntimeOwnerEffect fixture_prepare_c1b3b_liveness_failure_source(
    RuntimeOwnerAdapterCore &adapter)
{
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    return tickets[0];
}

TrustedReceipt make_c1b3b_liveness_failure_receipt(
    const C1b3bLivenessFailureSource source,
    const RuntimeOwnerEffect ticket,
    const std::uint32_t diagnostic_code = 97)
{
    return make_liveness_failure_receipt(
        c1b3b_outcome_kind(source), ticket, diagnostic_code);
}

RuntimeOwnerTransition make_c1b3b_canonical_transition_for_source(
    const C1b3bLivenessFailureSource source)
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect ticket =
        fixture_prepare_c1b3b_liveness_failure_source(adapter);
    const TrustedReceipt receipt =
        make_c1b3b_liveness_failure_receipt(source, ticket);
    return make_c1b3b_canonical_liveness_failure_transition(
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core,
        receipt);
}

void test_c1b3b_operation_failed_unknown_disposition_fails_closed()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    const TrustedReceipt receipt = make_liveness_failure_receipt(
        TrustedReceiptKind::OperationFailed, tickets[0], 91);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerTransition malformed =
        make_c1b3b_canonical_liveness_failure_transition(
            before.core, receipt);
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::RecoveryPending,
        before.last_trusted_ingress_sequence);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_last_liveness_failure_validation_bypass_used(
                   adapter));
}

void check_c1b3b_liveness_failure_malformed_fallback(
    const C1b3bLivenessFailureSource source,
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41,
    const C1b3bPostViewMutation post_view_mutation =
        C1b3bPostViewMutation::None,
    const C1b3bMalformedExercise exercise =
        C1b3bMalformedExercise::Validator)
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect target_ticket =
        fixture_prepare_c1b3b_liveness_failure_source(adapter);
    const TrustedReceipt head =
        make_c1b3b_liveness_failure_receipt(source, target_ticket);
    constexpr TrustedReceipt trailing =
        make_transport_attempt_failed_receipt(1, 91);
    const std::uint32_t expected_head_ingress =
        adapter.view().last_trusted_ingress_sequence + 1;
    const std::uint32_t expected_trailing_ingress =
        expected_head_ingress + 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, head) == TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, trailing) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    if (exercise == C1b3bMalformedExercise::Validator) {
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
    }
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_accepted_liveness_mask(
        adapter, 0x0a);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, initial_dispatch_sequence);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_before = adapter.view();
    RuntimeOwnerView observed_post_view =
        make_c1b3b_canonical_liveness_failure_post_view(
            before.core, source);
    if (post_view_mutation != C1b3bPostViewMutation::None) {
        mutate_c1b3b_post_view(
            observed_post_view, post_view_mutation);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter, observed_post_view);
    }
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(!before.core.boot_orchestration_ended);
    CHECK(before.trusted_count == 2);
    CHECK(before.trusted_slots[before.trusted_head].ingress_sequence ==
          expected_head_ingress);
    CHECK(trusted_receipts_equal(
        before.trusted_slots[before.trusted_head].receipt, head));
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count ==
          (exercise == C1b3bMalformedExercise::Validator ? 0 : 1));
    if (exercise == C1b3bMalformedExercise::Validator) {
        CHECK(initial_dispatch_sequence == 41);
    }
    CHECK(before.accepted_liveness_mask == 0x0a);
    CHECK(before.last_trusted_receipt_signature.ingress_sequence == 67);
    CHECK(before.last_trusted_diagnostic_ingress_sequence ==
          expected_trailing_ingress);
    CHECK(before.last_trusted_diagnostic_code == 91);
    CHECK(public_before.trusted_stale_count == 73);
    CHECK(public_before.trusted_duplicate_count == 79);
    CHECK(public_before.trusted_protocol_violation_count == 83);
    CHECK(!before.core_fail_closed_latched);
    CHECK(!before.core_adapter_fatal_latched);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter) ==
          (post_view_mutation != C1b3bPostViewMutation::None));

    const RuntimeOwnerPhase expected_result_phase =
        post_view_mutation == C1b3bPostViewMutation::UnknownPhase
            ? RuntimeOwnerPhase::LivenessWaiting
            : observed_post_view.phase;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::LivenessWaiting,
        expected_result_phase,
        expected_head_ingress);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_after = adapter.view();
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b3b_canonical_liveness_failure_post_view(
            before.core, source)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    const bool expected_sequence_bypass =
        initial_dispatch_sequence >=
            std::numeric_limits<std::uint32_t>::max() - 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_liveness_failure_validation_bypass_used(
                  adapter) ==
          expected_sequence_bypass);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));

    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.trusted_high_water == before.trusted_high_water);
    CHECK(after.last_trusted_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.last_trusted_diagnostic_ingress_sequence ==
          before.last_trusted_diagnostic_ingress_sequence);
    CHECK(after.last_trusted_diagnostic_code ==
          before.last_trusted_diagnostic_code);
    CHECK(public_after.trusted_stale_count ==
          public_before.trusted_stale_count);
    CHECK(public_after.trusted_duplicate_count ==
          public_before.trusted_duplicate_count);
    CHECK(public_after.trusted_protocol_violation_count ==
          public_before.trusted_protocol_violation_count);
    CHECK(after.trusted_rejected_full_count ==
          before.trusted_rejected_full_count);

    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    CHECK(after.normal_high_water == before.normal_high_water);
    CHECK(after.last_normal_enqueue_sequence ==
          before.last_normal_enqueue_sequence);
    CHECK(after.normal_coalesced_count ==
          before.normal_coalesced_count);
    CHECK(after.normal_rejected_full_count ==
          before.normal_rejected_full_count);
    for (const RuntimeOwnerAdapterNormalSlotSnapshot slot :
         after.normal_slots) {
        CHECK(normal_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterNormalSlotSnapshot{}));
    }
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.transport_request_pending);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.reason_mask == (1u << 6u));
    CHECK(after.critical.first_ingress_sequence ==
          expected_head_ingress);
    CHECK(after.critical.last_ingress_sequence ==
          expected_head_ingress);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    const bool suppress_synthetic_pair =
        post_view_mutation == C1b3bPostViewMutation::ShutdownPhase;
    const bool safety_blocked =
        !suppress_synthetic_pair &&
        initial_dispatch_sequence >= maximum - 1;
    const bool terminal_reserve =
        !suppress_synthetic_pair && !safety_blocked &&
        initial_dispatch_sequence >= maximum - 4;
    if (suppress_synthetic_pair || safety_blocked) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(after.safety_delivery_blocked == safety_blocked);
    } else {
        const std::uint32_t record_sequence = terminal_reserve
            ? maximum - 1
            : initial_dispatch_sequence + 1;
        const std::uint32_t recovery_sequence = terminal_reserve
            ? maximum
            : initial_dispatch_sequence + 2;
        CHECK(after.last_dispatch_sequence == recovery_sequence);
        check_canonical_recovery_pending_pair(
            after,
            record_sequence,
            recovery_sequence,
            RuntimeOwnerFaultCode::InternalInvariant,
            0,
            {});
        CHECK(after.dispatch_fatal_latched == terminal_reserve);
        CHECK(!after.safety_delivery_blocked);
    }
    const RuntimeOwnerFaultCode discarded_fault =
        liveness_failure_fault(c1b3b_outcome_kind(source));
    for (const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot :
         after.pending_effect_slots) {
        CHECK(slot.effect.fault_code != discarded_fault);
    }
    CHECK(has_safe_default(public_after.current_dispatch));
    CHECK(has_safe_default(public_after.physical_inflight));
    CHECK(public_after.last_ack_dispatch_sequence ==
          public_before.last_ack_dispatch_sequence);
    CHECK(public_after.physical_inflight_cancel_pending == 0);

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::RecoveryPending,
        !suppress_synthetic_pair && !safety_blocked,
        submit_count_before + 1);
}

void check_c1b3b_liveness_failure_intentional_pending_bypass_fallback(
    const C1b3bLivenessFailureSource source,
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41)
{
    RuntimeOwnerTransition exercised = malformed;
    exercised.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b3b_liveness_failure_malformed_fallback(
        source,
        exercised,
        initial_dispatch_sequence,
        C1b3bPostViewMutation::None,
        C1b3bMalformedExercise::IntentionalPendingBypass);
}

void check_c1b3b_liveness_failure_dispositions_fail_closed(
    const C1b3bLivenessFailureSource source)
{
    constexpr std::array<RuntimeOwnerDisposition, 4> corruptions{{
        static_cast<RuntimeOwnerDisposition>(255),
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3b_canonical_transition_for_source(source);
        malformed.disposition = disposition;
        check_c1b3b_liveness_failure_malformed_fallback(
            source, malformed);
    }
}

void test_c1b3b_operation_failed_dispositions_fail_closed()
{
    check_c1b3b_liveness_failure_dispositions_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed);
}

void test_c1b3b_deadline_expired_dispositions_fail_closed()
{
    check_c1b3b_liveness_failure_dispositions_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired);
}

void check_c1b3b_liveness_failure_transition_phases_fail_closed(
    const C1b3bLivenessFailureSource source)
{
    std::array<RuntimeOwnerTransition, 4> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition =
            make_c1b3b_canonical_transition_for_source(source);
    }
    corruptions[0].phase_before = RuntimeOwnerPhase::ColdStart;
    corruptions[1].phase_before =
        static_cast<RuntimeOwnerPhase>(255);
    corruptions[2].phase_after = RuntimeOwnerPhase::RuntimeReady;
    corruptions[3].phase_after =
        static_cast<RuntimeOwnerPhase>(255);
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b3b_liveness_failure_malformed_fallback(
            source, malformed);
    }
}

void test_c1b3b_operation_failed_transition_phases_fail_closed()
{
    check_c1b3b_liveness_failure_transition_phases_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed);
}

void test_c1b3b_deadline_expired_transition_phases_fail_closed()
{
    check_c1b3b_liveness_failure_transition_phases_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired);
}

void check_c1b3b_liveness_failure_effect_counts_fail_closed(
    const C1b3bLivenessFailureSource source)
{
    constexpr std::array<std::uint8_t, 4> corruptions{{0, 1, 3, 5}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3b_canonical_transition_for_source(source);
        malformed.effect_count = effect_count;
        check_c1b3b_liveness_failure_malformed_fallback(
            source, malformed);
    }
}

void test_c1b3b_operation_failed_effect_counts_fail_closed()
{
    check_c1b3b_liveness_failure_effect_counts_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed);
}

void test_c1b3b_deadline_expired_effect_counts_fail_closed()
{
    check_c1b3b_liveness_failure_effect_counts_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired);
}

void corrupt_c1b3b_effect_field(
    RuntimeOwnerEffect &effect,
    const std::size_t field_index,
    const bool used)
{
    switch (field_index) {
    case 0:
        effect.kind = used
            ? RuntimeOwnerEffectKind::StartAtProbe
            : RuntimeOwnerEffectKind::RecordFault;
        break;
    case 1:
        ++effect.correlation_id;
        break;
    case 2:
        ++effect.attempt.mqtt_session_id;
        break;
    case 3:
        ++effect.attempt.mqtt_generation;
        break;
    case 4:
        ++effect.attempt.config_apply_epoch;
        break;
    case 5:
        effect.fault_code = RuntimeOwnerFaultCode::InternalInvariant;
        break;
    default:
        break;
    }
}

void check_c1b3b_liveness_failure_effect_slot_fields_fail_closed(
    const C1b3bLivenessFailureSource source,
    const std::size_t slot_index,
    const bool used)
{
    for (std::size_t field_index = 0; field_index < 6; ++field_index) {
        RuntimeOwnerTransition malformed =
            make_c1b3b_canonical_transition_for_source(source);
        corrupt_c1b3b_effect_field(
            malformed.effects[slot_index], field_index, used);
        check_c1b3b_liveness_failure_malformed_fallback(
            source, malformed);
    }
}

void test_c1b3b_operation_failed_used_slot0_fields_fail_closed()
{
    check_c1b3b_liveness_failure_effect_slot_fields_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed, 0, true);
}

void test_c1b3b_operation_failed_used_slot1_fields_fail_closed()
{
    check_c1b3b_liveness_failure_effect_slot_fields_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed, 1, true);
}

void test_c1b3b_deadline_expired_used_slot0_fields_fail_closed()
{
    check_c1b3b_liveness_failure_effect_slot_fields_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired, 0, true);
}

void test_c1b3b_deadline_expired_used_slot1_fields_fail_closed()
{
    check_c1b3b_liveness_failure_effect_slot_fields_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired, 1, true);
}

void test_c1b3b_operation_failed_unused_slot3_fields_fail_closed()
{
    check_c1b3b_liveness_failure_effect_slot_fields_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed, 3, false);
}

void test_c1b3b_deadline_expired_unused_slot3_fields_fail_closed()
{
    check_c1b3b_liveness_failure_effect_slot_fields_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired, 3, false);
}

void check_c1b3b_liveness_failure_post_view_phases_fail_closed(
    const C1b3bLivenessFailureSource source)
{
    for (const C1b3bPostViewMutation mutation : {
             C1b3bPostViewMutation::KnownWrongPhase,
             C1b3bPostViewMutation::UnknownPhase,
             C1b3bPostViewMutation::ShutdownPhase,
         }) {
        check_c1b3b_liveness_failure_malformed_fallback(
            source,
            make_c1b3b_canonical_transition_for_source(source),
            41,
            mutation);
    }
}

void test_c1b3b_operation_failed_post_view_phases_fail_closed()
{
    check_c1b3b_liveness_failure_post_view_phases_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed);
}

void test_c1b3b_deadline_expired_post_view_phases_fail_closed()
{
    check_c1b3b_liveness_failure_post_view_phases_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired);
}

void check_c1b3b_liveness_failure_post_view_fields_fail_closed(
    const C1b3bLivenessFailureSource source)
{
    constexpr std::array<C1b3bPostViewMutation, 11> mutations{{
        C1b3bPostViewMutation::MqttSession,
        C1b3bPostViewMutation::MqttGeneration,
        C1b3bPostViewMutation::MqttGenerationCounter,
        C1b3bPostViewMutation::ConfigApplyEpochCounter,
        C1b3bPostViewMutation::LastConfigCommitSequence,
        C1b3bPostViewMutation::LastCorrelationId,
        C1b3bPostViewMutation::ActiveAttemptSession,
        C1b3bPostViewMutation::ActiveAttemptGeneration,
        C1b3bPostViewMutation::ActiveAttemptEpoch,
        C1b3bPostViewMutation::BootOrchestrationEnded,
        C1b3bPostViewMutation::LastFault,
    }};
    for (const C1b3bPostViewMutation mutation : mutations) {
        check_c1b3b_liveness_failure_malformed_fallback(
            source,
            make_c1b3b_canonical_transition_for_source(source),
            41,
            mutation);
    }
}

void test_c1b3b_operation_failed_post_view_fields_fail_closed()
{
    check_c1b3b_liveness_failure_post_view_fields_fail_closed(
        C1b3bLivenessFailureSource::OperationFailed);
}

void test_c1b3b_deadline_expired_post_view_fields_fail_closed()
{
    check_c1b3b_liveness_failure_post_view_fields_fail_closed(
        C1b3bLivenessFailureSource::DeadlineExpired);
}

void check_c1b3b_liveness_failure_sequence_reserves_and_damage(
    const C1b3bLivenessFailureSource source)
{
    RuntimeOwnerTransition malformed =
        make_c1b3b_canonical_transition_for_source(source);
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b3b_liveness_failure_intentional_pending_bypass_fallback(
        source, malformed, 41);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    for (const std::uint32_t terminal_start : {
             maximum - 4,
             maximum - 3,
             maximum - 2,
         }) {
        check_c1b3b_liveness_failure_intentional_pending_bypass_fallback(
            source, malformed, terminal_start);
    }
    for (const std::uint32_t damaged_start : {
             maximum - 1,
             maximum,
         }) {
        check_c1b3b_liveness_failure_intentional_pending_bypass_fallback(
            source, malformed, damaged_start);
    }
}

void test_c1b3b_operation_failed_sequence_reserves_and_damage()
{
    check_c1b3b_liveness_failure_sequence_reserves_and_damage(
        C1b3bLivenessFailureSource::OperationFailed);
}

void test_c1b3b_deadline_expired_sequence_reserves_and_damage()
{
    check_c1b3b_liveness_failure_sequence_reserves_and_damage(
        C1b3bLivenessFailureSource::DeadlineExpired);
}

void test_c1b3b_operation_failed_canonical_override_pending_bypass_fails_closed()
{
    check_c1b3b_liveness_failure_intentional_pending_bypass_fallback(
        C1b3bLivenessFailureSource::OperationFailed,
        make_c1b3b_canonical_transition_for_source(
            C1b3bLivenessFailureSource::OperationFailed));
}

void test_c1b3b_deadline_expired_canonical_override_pending_bypass_fails_closed()
{
    check_c1b3b_liveness_failure_intentional_pending_bypass_fallback(
        C1b3bLivenessFailureSource::DeadlineExpired,
        make_c1b3b_canonical_transition_for_source(
            C1b3bLivenessFailureSource::DeadlineExpired));
}

void check_liveness_failure_accepts(
    RuntimeOwnerAdapterCore &adapter,
    const TrustedReceipt receipt,
    const std::uint32_t record_fault_sequence,
    const std::uint32_t enter_recovery_sequence,
    const bool terminal_reserve)
{
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(before.physical_inflight.kind == AdapterDispatchKind::CoreEffect);
    CHECK(before.physical_inflight.effect.kind == receipt.effect_kind);
    CHECK(before.physical_inflight.effect.correlation_id ==
          receipt.correlation_id);
    CHECK(receipt.kind == TrustedReceiptKind::OperationFailed ||
          receipt.kind == TrustedReceiptKind::DeadlineExpired);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t ingress_sequence =
        adapter.view().last_trusted_ingress_sequence;

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::RecoveryPending,
        ingress_sequence);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerView expected_core = before.core;
    expected_core.phase = RuntimeOwnerPhase::RecoveryPending;
    expected_core.mqtt_session_id = 0;
    expected_core.mqtt_generation = 0;
    expected_core.active_attempt = {};
    expected_core.last_fault = liveness_failure_fault(receipt.kind);
    CHECK(runtime_owner_views_equal(after.core, expected_core));
    CHECK(after.trusted_count == before.trusted_count);
    CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
          ingress_sequence);
    CHECK(trusted_receipts_equal(
        after.last_trusted_receipt_signature.receipt, receipt));
    CHECK(after.last_dispatch_sequence == enter_recovery_sequence);
    CHECK(after.accepted_liveness_mask == 0);
    check_canonical_recovery_pending_pair(
        after,
        record_fault_sequence,
        enter_recovery_sequence,
        liveness_failure_fault(receipt.kind),
        receipt.correlation_id,
        before.core.active_attempt);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.dispatch_fatal_latched == terminal_reserve);
    CHECK(!after.critical_pending);
}

void check_c1b3b_canonical_override_unavailable_safety_plan_fails_closed(
    const C1b3bLivenessFailureSource source)
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect target_ticket =
        fixture_prepare_c1b3b_liveness_failure_source(adapter);
    const TrustedReceipt receipt =
        make_c1b3b_liveness_failure_receipt(source, target_ticket);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, maximum);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_c1b3b_canonical_liveness_failure_transition(
            before.core, receipt));
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter,
            make_c1b3b_canonical_liveness_failure_post_view(
                before.core, source));

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::RecoveryPending,
        before.last_trusted_ingress_sequence);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_liveness_failure_validation_bypass_used(
                  adapter));

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b3b_canonical_liveness_failure_post_view(
            before.core, source)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(after.normal_count == 0);
    CHECK(after.pending_effect_count == 0);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 0);
    check_unused_pending_effect_slots_are_zero(after, 0);
    CHECK(after.last_dispatch_sequence == maximum);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.safety_delivery_blocked);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.first_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.critical.last_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::RecoveryPending,
        false,
        submit_count_before + 1);
}

void test_c1b3b_operation_failed_canonical_override_unavailable_safety_plan_fails_closed()
{
    check_c1b3b_canonical_override_unavailable_safety_plan_fails_closed(
        C1b3bLivenessFailureSource::OperationFailed);
}

void test_c1b3b_deadline_expired_canonical_override_unavailable_safety_plan_fails_closed()
{
    check_c1b3b_canonical_override_unavailable_safety_plan_fails_closed(
        C1b3bLivenessFailureSource::DeadlineExpired);
}

enum class C1b3bAuthorizationMismatch : std::uint8_t {
    ReceiptKind = 0,
    Phase,
    BootEnded,
    EffectKind,
    Correlation,
    Session,
    Generation,
    Epoch,
    ExactSignature,
    CompletedMask,
};

void check_c1b3b_unauthorized_pending_override_remains_unconsumed(
    const C1b3bLivenessFailureSource source,
    const C1b3bAuthorizationMismatch mismatch)
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerPhase prepared_phase =
        mismatch == C1b3bAuthorizationMismatch::Phase
            ? RuntimeOwnerPhase::SnapshotFreezePending
            : RuntimeOwnerPhase::LivenessWaiting;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
        adapter, prepared_phase));
    const RuntimeOwnerView issued_view =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core;
    CHECK(issued_view.last_correlation_id >= 6);
    const RuntimeOwnerEffect target_ticket{
        RuntimeOwnerEffectKind::StartAtProbe,
        issued_view.last_correlation_id - 5,
        issued_view.active_attempt,
        RuntimeOwnerFaultCode::None,
    };
    if (mismatch == C1b3bAuthorizationMismatch::BootEnded) {
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_core_boot_orchestration_ended(adapter, true);
    }
    TrustedReceipt receipt =
        make_c1b3b_liveness_failure_receipt(source, target_ticket);
    switch (mismatch) {
    case C1b3bAuthorizationMismatch::ReceiptKind:
        receipt.kind = static_cast<TrustedReceiptKind>(255);
        break;
    case C1b3bAuthorizationMismatch::Phase:
        break;
    case C1b3bAuthorizationMismatch::BootEnded:
        break;
    case C1b3bAuthorizationMismatch::EffectKind:
        receipt.effect_kind = RuntimeOwnerEffectKind::StartProbePublish;
        break;
    case C1b3bAuthorizationMismatch::Correlation:
        ++receipt.correlation_id;
        break;
    case C1b3bAuthorizationMismatch::Session:
        ++receipt.mqtt_session_id;
        break;
    case C1b3bAuthorizationMismatch::Generation:
        ++receipt.mqtt_generation;
        break;
    case C1b3bAuthorizationMismatch::Epoch:
        ++receipt.config_apply_epoch;
        break;
    case C1b3bAuthorizationMismatch::ExactSignature:
        break;
    case C1b3bAuthorizationMismatch::CompletedMask:
        break;
    }
    if (mismatch == C1b3bAuthorizationMismatch::ReceiptKind) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_enqueue_trusted_receipt_unchecked(
                      adapter, receipt));
    } else {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
    }
    const std::uint32_t head_ingress_sequence =
        adapter.view().last_trusted_ingress_sequence;
    if (mismatch == C1b3bAuthorizationMismatch::ExactSignature) {
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_trusted_receipt_signature(
                adapter, head_ingress_sequence, receipt);
    }
    if (mismatch == C1b3bAuthorizationMismatch::CompletedMask) {
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_accepted_liveness_mask(
            adapter, 0x01);
    }
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_authorization_pending_effect(adapter);
    const RuntimeOwnerAdapterPrivateSnapshot seeded =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_c1b3b_canonical_liveness_failure_transition(
            seeded.core, receipt));
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter,
            make_c1b3b_canonical_liveness_failure_post_view(
                seeded.core, source));
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(before.trusted_count == 1);
    CHECK(before.pending_effect_count == 1);
    CHECK(before.core.phase == prepared_phase);
    CHECK(before.core.boot_orchestration_ended ==
          (mismatch == C1b3bAuthorizationMismatch::BootEnded));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter));
    CHECK(submit_count_before == 0);

    const RuntimeOwnerAdapterTrustedSlotSnapshot head =
        before.trusted_slots[before.trusted_head];
    CHECK(head.payload_kind == 1);
    CHECK(head.ingress_sequence == head_ingress_sequence);
    CHECK(trusted_receipts_equal(head.receipt, receipt));
    const bool accepted_receipt_kind =
        head.receipt.kind == TrustedReceiptKind::OperationFailed ||
        head.receipt.kind == TrustedReceiptKind::DeadlineExpired;
    const bool exact_phase =
        before.core.phase == RuntimeOwnerPhase::LivenessWaiting;
    const bool boot_active = !before.core.boot_orchestration_ended;
    const bool exact_effect_kind =
        head.receipt.effect_kind == target_ticket.kind;
    const bool exact_correlation =
        head.receipt.correlation_id == target_ticket.correlation_id;
    const bool exact_session =
        head.receipt.mqtt_session_id ==
        target_ticket.attempt.mqtt_session_id;
    const bool exact_generation =
        head.receipt.mqtt_generation ==
        target_ticket.attempt.mqtt_generation;
    const bool exact_epoch =
        head.receipt.config_apply_epoch ==
        target_ticket.attempt.config_apply_epoch;
    const bool signature_clear =
        before.last_trusted_receipt_signature.ingress_sequence == 0 ||
        !trusted_receipts_equal(
            head.receipt,
            before.last_trusted_receipt_signature.receipt);
    const bool completed_mask_clear =
        (before.accepted_liveness_mask & 0x01) == 0;
    const std::size_t false_authorization_terms =
        static_cast<std::size_t>(!accepted_receipt_kind) +
        static_cast<std::size_t>(!exact_phase) +
        static_cast<std::size_t>(!boot_active) +
        static_cast<std::size_t>(!exact_effect_kind) +
        static_cast<std::size_t>(!exact_correlation) +
        static_cast<std::size_t>(!exact_session) +
        static_cast<std::size_t>(!exact_generation) +
        static_cast<std::size_t>(!exact_epoch) +
        static_cast<std::size_t>(!signature_clear) +
        static_cast<std::size_t>(!completed_mask_clear);
    CHECK(false_authorization_terms == 1);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Idle,
        RuntimeOwnerDisposition::Rejected,
        before.core.phase,
        before.core.phase);
    CHECK(private_snapshots_equal(
        before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_last_liveness_failure_validation_bypass_used(
                   adapter));
}

void test_c1b3b_pending_override_requires_full_authorization()
{
    constexpr std::array<C1b3bAuthorizationMismatch, 10> mismatches{{
        C1b3bAuthorizationMismatch::ReceiptKind,
        C1b3bAuthorizationMismatch::Phase,
        C1b3bAuthorizationMismatch::BootEnded,
        C1b3bAuthorizationMismatch::EffectKind,
        C1b3bAuthorizationMismatch::Correlation,
        C1b3bAuthorizationMismatch::Session,
        C1b3bAuthorizationMismatch::Generation,
        C1b3bAuthorizationMismatch::Epoch,
        C1b3bAuthorizationMismatch::ExactSignature,
        C1b3bAuthorizationMismatch::CompletedMask,
    }};
    for (const C1b3bLivenessFailureSource source : {
             C1b3bLivenessFailureSource::OperationFailed,
             C1b3bLivenessFailureSource::DeadlineExpired,
         }) {
        for (const C1b3bAuthorizationMismatch mismatch : mismatches) {
            check_c1b3b_unauthorized_pending_override_remains_unconsumed(
                source, mismatch);
        }
    }
}

void test_c1b3b_valid_overrides_preserve_all_eight_paths()
{
    for (const C1b3bLivenessFailureSource source : {
             C1b3bLivenessFailureSource::OperationFailed,
             C1b3bLivenessFailureSource::DeadlineExpired,
         }) {
        for (std::size_t ticket_index = 0; ticket_index < 4;
             ++ticket_index) {
            RuntimeOwnerAdapterCore adapter{};
            const std::array<RuntimeOwnerEffect, 4> tickets =
                fixture_prepare_liveness_waiting_via_config(adapter);
            fixture_advance_to_acked_liveness_ticket(
                adapter, tickets, ticket_index);
            const TrustedReceipt receipt =
                make_c1b3b_liveness_failure_receipt(
                    source,
                    tickets[ticket_index],
                    100 + static_cast<std::uint32_t>(ticket_index));
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, receipt) == TrustedEnqueueResult::Accepted);
            const RuntimeOwnerAdapterPrivateSnapshot before =
                RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
            const std::uint32_t submit_count_before =
                RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                    adapter);
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_override_next_core_transition(
                    adapter,
                    make_c1b3b_canonical_liveness_failure_transition(
                        before.core, receipt));
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_override_next_core_post_submit_view(
                    adapter,
                    make_c1b3b_canonical_liveness_failure_post_view(
                        before.core, source));

            check_exact_ingress_step_result(
                adapter.step(),
                AdapterStepAction::CoreTransitionApplied,
                RuntimeOwnerDisposition::Accepted,
                RuntimeOwnerPhase::LivenessWaiting,
                RuntimeOwnerPhase::RecoveryPending,
                before.last_trusted_ingress_sequence);
            CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                       fixture_last_liveness_failure_validation_bypass_used(
                           adapter));
            const RuntimeOwnerAdapterPrivateSnapshot after =
                RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
            CHECK(runtime_owner_views_equal(
                after.core,
                make_c1b3b_canonical_liveness_failure_post_view(
                    before.core, source)));
            CHECK(after.trusted_count == 0);
            CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
                  before.last_trusted_ingress_sequence);
            CHECK(trusted_receipts_equal(
                after.last_trusted_receipt_signature.receipt, receipt));
            CHECK(after.last_dispatch_sequence ==
                  before.last_dispatch_sequence + 2);
            CHECK(after.accepted_liveness_mask == 0);
            check_canonical_recovery_pending_pair(
                after,
                before.last_dispatch_sequence + 1,
                before.last_dispatch_sequence + 2,
                liveness_failure_fault(receipt.kind),
                receipt.correlation_id,
                before.core.active_attempt);
            CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                      adapter) == submit_count_before + 1);
            CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                       fixture_core_transition_override_pending(adapter));
            CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                       fixture_core_post_submit_view_override_pending(
                           adapter));
            CHECK(!after.core_adapter_fatal_latched);
            CHECK(!after.core_fail_closed_latched);
            CHECK(!after.critical_pending);
        }
    }
}

void test_task4c_c1b3b_liveness_failure_malformed_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    for (const C1b3bLivenessFailureSource source : {
             C1b3bLivenessFailureSource::OperationFailed,
             C1b3bLivenessFailureSource::DeadlineExpired,
         }) {
        RuntimeOwnerTransition malformed =
            make_c1b3b_canonical_transition_for_source(source);
        malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
        check_c1b3b_liveness_failure_malformed_fallback(
            source, malformed);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_liveness_failure_eight_happy_ticket_cases()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        for (std::size_t ticket_index = 0; ticket_index < 4;
             ++ticket_index) {
            RuntimeOwnerAdapterCore adapter{};
            const std::array<RuntimeOwnerEffect, 4> tickets =
                fixture_prepare_liveness_waiting_via_config(adapter);
            fixture_advance_to_acked_liveness_ticket(
                adapter, tickets, ticket_index);
            const RuntimeOwnerAdapterPrivateSnapshot before =
                RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
            check_liveness_failure_accepts(
                adapter,
                make_liveness_failure_receipt(
                    outcome,
                    tickets[ticket_index],
                    40 + static_cast<std::uint32_t>(ticket_index)),
                before.last_dispatch_sequence + 1,
                before.last_dispatch_sequence + 2,
                false);
        }
    }
}

void test_liveness_failure_exact_duplicate_but_changed_diagnostic_is_stale()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        fixture_advance_to_acked_liveness_ticket(adapter, tickets, 2);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t accepted_ingress_sequence =
            before.last_trusted_ingress_sequence + 1;
        const TrustedReceipt accepted =
            make_liveness_failure_receipt(outcome, tickets[2], 77);
        check_liveness_failure_accepts(
            adapter,
            accepted,
            before.last_dispatch_sequence + 1,
            before.last_dispatch_sequence + 2,
            false);
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);

        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, accepted) == TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::RecoveryPending, 0, 1);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_liveness_failure_receipt(
                      outcome, tickets[2], 78)) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::RecoveryPending, 1, 1);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
              accepted_ingress_sequence);
        CHECK(trusted_receipts_equal(
            after.last_trusted_receipt_signature.receipt, accepted));
    }
}

void test_liveness_failure_completed_ticket_is_stale_not_duplicate()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        check_operation_completed_accepts(adapter, tickets[0], false);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_liveness_failure_receipt(
                      outcome, tickets[0], 91)) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            adapter, RuntimeOwnerPhase::LivenessWaiting, 1, 0);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.accepted_liveness_mask == 1);
        CHECK(after.last_trusted_receipt_signature.receipt.kind ==
              TrustedReceiptKind::OperationCompleted);
    }
}

void test_liveness_failure_uncompleted_ticket_accepts_with_other_mask_bit()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        check_operation_completed_accepts(adapter, tickets[0], false);
        fixture_advance_to_acked_liveness_ticket(adapter, tickets, 1);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(before.accepted_liveness_mask == 1);
        check_liveness_failure_accepts(
            adapter,
            make_liveness_failure_receipt(outcome, tickets[1], 92),
            before.last_dispatch_sequence + 1,
            before.last_dispatch_sequence + 2,
            false);
    }
}

void test_liveness_failure_ticket_field_and_phase_mismatches_are_stale()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        const TrustedReceipt exact =
            make_liveness_failure_receipt(outcome, tickets[0], 93);
        std::array<TrustedReceipt, 5> mismatches{{
            exact,
            exact,
            exact,
            exact,
            exact,
        }};
        mismatches[0].effect_kind = tickets[1].kind;
        ++mismatches[1].correlation_id;
        ++mismatches[2].mqtt_session_id;
        ++mismatches[3].mqtt_generation;
        ++mismatches[4].config_apply_epoch;
        for (std::size_t index = 0; index < mismatches.size(); ++index) {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, mismatches[index]) ==
                  TrustedEnqueueResult::Accepted);
            check_classified_trusted_discard(
                adapter,
                RuntimeOwnerPhase::LivenessWaiting,
                static_cast<std::uint32_t>(index + 1),
                0);
        }

        RuntimeOwnerAdapterCore cold{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  cold, exact) == TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            cold, RuntimeOwnerPhase::ColdStart, 1, 0);
    }
}

void test_liveness_failure_pending_effects_defer_without_dequeue()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter, true);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_liveness_failure_receipt(
                      outcome, tickets[0], 94)) ==
              TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const AdapterStepResult prepared = adapter.step();
        CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
        CHECK(prepared.prepared_dispatch_sequence ==
              before.pending_effect_slots[before.pending_effect_head]
                  .preassigned_dispatch_sequence);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(runtime_owner_effects_equal(offered.effect, tickets[0]));
        CHECK(adapter.acknowledge_dispatch(
                  offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
    }
}

void test_liveness_failure_sequence_regular_and_terminal_reserve()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        maximum - 4,
        maximum - 3,
        maximum - 2,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        {
            RuntimeOwnerAdapterCore adapter{};
            const std::array<RuntimeOwnerEffect, 4> tickets =
                fixture_prepare_liveness_waiting_via_config(adapter);
            RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
                adapter, 41);
            check_liveness_failure_accepts(
                adapter,
                make_liveness_failure_receipt(outcome, tickets[0]),
                42,
                43,
                false);
        }
        for (const std::uint32_t start : terminal_starts) {
            RuntimeOwnerAdapterCore adapter{};
            const std::array<RuntimeOwnerEffect, 4> tickets =
                fixture_prepare_liveness_waiting_via_config(adapter);
            fixture_advance_to_acked_liveness_ticket(adapter, tickets, 1);
            RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
                adapter, start);
            check_liveness_failure_accepts(
                adapter,
                make_liveness_failure_receipt(outcome, tickets[1]),
                maximum - 1,
                maximum,
                true);
        }
    }
}

void test_liveness_failure_damaged_sequence_shortage_is_bounded()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 2> damaged_starts{{
        maximum - 1,
        maximum,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        for (const std::uint32_t start : damaged_starts) {
            RuntimeOwnerAdapterCore adapter{};
            const std::array<RuntimeOwnerEffect, 4> tickets =
                fixture_prepare_liveness_waiting_via_config(adapter);
            fixture_advance_to_acked_liveness_ticket(adapter, tickets, 2);
            RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
                adapter, start);
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter,
                      make_liveness_failure_receipt(
                          outcome, tickets[2], 95)) ==
                  TrustedEnqueueResult::Accepted);
            const RuntimeOwnerAdapterPrivateSnapshot before =
                RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
            check_exact_step_result(
                adapter.step(),
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                RuntimeOwnerPhase::LivenessWaiting,
                RuntimeOwnerPhase::LivenessWaiting);
            const RuntimeOwnerAdapterPrivateSnapshot after =
                RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
            CHECK(runtime_owner_views_equal(before.core, after.core));
            CHECK(after.trusted_count == 1);
            CHECK(after.pending_effect_count ==
                  before.pending_effect_count);
            CHECK(after.last_dispatch_sequence == start);
            CHECK(after.accepted_liveness_mask ==
                  before.accepted_liveness_mask);
            CHECK(last_trusted_receipt_signatures_equal(
                after.last_trusted_receipt_signature,
                before.last_trusted_receipt_signature));
            CHECK(after.critical_pending);
            CHECK(after.critical.first_reason ==
                  AdapterCriticalReason::DispatchSequenceSaturation);
            CHECK(after.critical.last_reason ==
                  AdapterCriticalReason::DispatchSequenceSaturation);
            CHECK(after.critical.first_ingress_sequence ==
                  before.last_trusted_ingress_sequence);
            CHECK(after.critical.last_ingress_sequence ==
                  before.last_trusted_ingress_sequence);
            CHECK(after.critical.first_diagnostic_code == 0);
            CHECK(after.critical.last_diagnostic_code == 0);
            CHECK(after.critical.occurrence_count == 1);

            check_exact_step_result(
                adapter.step(),
                AdapterStepAction::CriticalLedgerHandled,
                RuntimeOwnerDisposition::Rejected,
                RuntimeOwnerPhase::LivenessWaiting,
                RuntimeOwnerPhase::LivenessWaiting);
            CHECK(adapter.view().critical_pending == 0);
            CHECK(adapter.view().safety_delivery_blocked == 1);
        }
    }
}

void test_liveness_failure_diagnostic_is_not_forwarded_to_core_or_effects()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore zero_diagnostic{};
        RuntimeOwnerAdapterCore max_diagnostic{};
        const std::array<RuntimeOwnerEffect, 4> zero_tickets =
            fixture_prepare_liveness_waiting_via_config(zero_diagnostic);
        const std::array<RuntimeOwnerEffect, 4> max_tickets =
            fixture_prepare_liveness_waiting_via_config(max_diagnostic);
        fixture_advance_to_acked_liveness_ticket(
            zero_diagnostic, zero_tickets, 3);
        fixture_advance_to_acked_liveness_ticket(
            max_diagnostic, max_tickets, 3);
        const RuntimeOwnerAdapterPrivateSnapshot zero_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(zero_diagnostic);
        const RuntimeOwnerAdapterPrivateSnapshot max_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(max_diagnostic);
        check_liveness_failure_accepts(
            zero_diagnostic,
            make_liveness_failure_receipt(outcome, zero_tickets[3], 0),
            zero_before.last_dispatch_sequence + 1,
            zero_before.last_dispatch_sequence + 2,
            false);
        check_liveness_failure_accepts(
            max_diagnostic,
            make_liveness_failure_receipt(
                outcome,
                max_tickets[3],
                std::numeric_limits<std::uint32_t>::max()),
            max_before.last_dispatch_sequence + 1,
            max_before.last_dispatch_sequence + 2,
            false);
        const RuntimeOwnerAdapterPrivateSnapshot zero_after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(zero_diagnostic);
        const RuntimeOwnerAdapterPrivateSnapshot max_after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(max_diagnostic);
        CHECK(runtime_owner_views_equal(zero_after.core, max_after.core));
        CHECK(zero_after.last_dispatch_sequence ==
              max_after.last_dispatch_sequence);
        CHECK(zero_after.pending_effect_count ==
              max_after.pending_effect_count);
        for (std::size_t index = 0;
             index < zero_after.pending_effect_slots.size(); ++index) {
            CHECK(pending_effect_slot_snapshots_equal(
                zero_after.pending_effect_slots[index],
                max_after.pending_effect_slots[index]));
        }
    }
}

RuntimeOwnerTransition make_c1b3c_canonical_snapshot_failed_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt)
{
    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = RuntimeOwnerPhase::SnapshotFreezePending;
    transition.phase_after = RuntimeOwnerPhase::RecoveryPending;
    transition.effect_count = 2;
    transition.effects[0] = {
        RuntimeOwnerEffectKind::RecordFault,
        receipt.correlation_id,
        before.active_attempt,
        RuntimeOwnerFaultCode::SnapshotFailure,
    };
    transition.effects[1] = {
        RuntimeOwnerEffectKind::EnterRecovery,
        receipt.correlation_id,
        before.active_attempt,
        RuntimeOwnerFaultCode::SnapshotFailure,
    };
    return transition;
}

void test_c1b3c_snapshot_failed_unknown_disposition_fails_closed()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    const TrustedReceipt receipt = make_snapshot_receipt(
        TrustedReceiptKind::SnapshotFailed, freeze, 91);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerTransition malformed =
        make_c1b3c_canonical_snapshot_failed_transition(
            before.core, receipt);
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RecoveryPending,
        before.last_trusted_ingress_sequence);
}

enum class C1b3cSnapshotSource : std::uint8_t {
    Succeeded = 0,
    Failed = 1,
};

enum class C1b3cPostViewMutation : std::uint8_t {
    None = 0,
    KnownWrongPhase,
    UnknownPhase,
    ShutdownPhase,
    MqttSession,
    MqttGeneration,
    MqttGenerationCounter,
    ConfigApplyEpochCounter,
    LastConfigCommitSequence,
    LastCorrelationId,
    ActiveAttemptSession,
    ActiveAttemptGeneration,
    ActiveAttemptEpoch,
    BootOrchestrationEnded,
    LastFault,
};

constexpr bool c1b3c_snapshot_failed(
    const C1b3cSnapshotSource source) noexcept
{
    return source == C1b3cSnapshotSource::Failed;
}

constexpr TrustedReceiptKind c1b3c_snapshot_receipt_kind(
    const C1b3cSnapshotSource source) noexcept
{
    return c1b3c_snapshot_failed(source)
        ? TrustedReceiptKind::SnapshotFailed
        : TrustedReceiptKind::SnapshotSucceeded;
}

constexpr std::uint32_t c1b3c_snapshot_diagnostic_code(
    const C1b3cSnapshotSource source) noexcept
{
    return c1b3c_snapshot_failed(source) ? 97 : 0;
}

RuntimeOwnerTransition make_c1b3c_canonical_snapshot_succeeded_transition(
    const RuntimeOwnerView before)
{
    RuntimeOwnerTransition transition{};
    transition.disposition = RuntimeOwnerDisposition::Accepted;
    transition.phase_before = RuntimeOwnerPhase::SnapshotFreezePending;
    transition.phase_after = RuntimeOwnerPhase::RuntimeReady;
    transition.effect_count = 1;
    transition.effects[0] = {
        RuntimeOwnerEffectKind::EndBootOrchestration,
        before.last_correlation_id,
        before.active_attempt,
        RuntimeOwnerFaultCode::None,
    };
    return transition;
}

RuntimeOwnerTransition make_c1b3c_canonical_snapshot_transition(
    const RuntimeOwnerView before,
    const TrustedReceipt receipt,
    const C1b3cSnapshotSource source)
{
    return c1b3c_snapshot_failed(source)
        ? make_c1b3c_canonical_snapshot_failed_transition(before, receipt)
        : make_c1b3c_canonical_snapshot_succeeded_transition(before);
}

RuntimeOwnerView make_c1b3c_canonical_snapshot_post_view(
    const RuntimeOwnerView before,
    const C1b3cSnapshotSource source)
{
    RuntimeOwnerView after = before;
    if (c1b3c_snapshot_failed(source)) {
        after.phase = RuntimeOwnerPhase::RecoveryPending;
        after.mqtt_session_id = 0;
        after.mqtt_generation = 0;
        after.active_attempt = {};
        after.last_fault = RuntimeOwnerFaultCode::SnapshotFailure;
    } else {
        after.phase = RuntimeOwnerPhase::RuntimeReady;
        after.boot_orchestration_ended = true;
    }
    return after;
}

void mutate_c1b3c_snapshot_post_view(
    RuntimeOwnerView &view,
    const C1b3cPostViewMutation mutation)
{
    switch (mutation) {
    case C1b3cPostViewMutation::None:
        break;
    case C1b3cPostViewMutation::KnownWrongPhase:
        view.phase = RuntimeOwnerPhase::ColdStart;
        break;
    case C1b3cPostViewMutation::UnknownPhase:
        view.phase = static_cast<RuntimeOwnerPhase>(255);
        break;
    case C1b3cPostViewMutation::ShutdownPhase:
        view.phase = RuntimeOwnerPhase::ShutdownCommitted;
        break;
    case C1b3cPostViewMutation::MqttSession:
        ++view.mqtt_session_id;
        break;
    case C1b3cPostViewMutation::MqttGeneration:
        ++view.mqtt_generation;
        break;
    case C1b3cPostViewMutation::MqttGenerationCounter:
        ++view.mqtt_generation_counter;
        break;
    case C1b3cPostViewMutation::ConfigApplyEpochCounter:
        ++view.config_apply_epoch_counter;
        break;
    case C1b3cPostViewMutation::LastConfigCommitSequence:
        ++view.last_config_commit_sequence;
        break;
    case C1b3cPostViewMutation::LastCorrelationId:
        ++view.last_correlation_id;
        break;
    case C1b3cPostViewMutation::ActiveAttemptSession:
        ++view.active_attempt.mqtt_session_id;
        break;
    case C1b3cPostViewMutation::ActiveAttemptGeneration:
        ++view.active_attempt.mqtt_generation;
        break;
    case C1b3cPostViewMutation::ActiveAttemptEpoch:
        ++view.active_attempt.config_apply_epoch;
        break;
    case C1b3cPostViewMutation::BootOrchestrationEnded:
        view.boot_orchestration_ended =
            !view.boot_orchestration_ended;
        break;
    case C1b3cPostViewMutation::LastFault:
        view.last_fault = RuntimeOwnerFaultCode::InternalInvariant;
        break;
    }
}

RuntimeOwnerTransition make_c1b3c_canonical_transition_for_source(
    const C1b3cSnapshotSource source)
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    const TrustedReceipt receipt = make_snapshot_receipt(
        c1b3c_snapshot_receipt_kind(source),
        freeze,
        c1b3c_snapshot_diagnostic_code(source));
    return make_c1b3c_canonical_snapshot_transition(
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core,
        receipt,
        source);
}

enum class C1b3cMalformedExercise : std::uint8_t {
    Validator = 0,
    IntentionalPendingBypass = 1,
};

void check_c1b3c_snapshot_malformed_fallback(
    const C1b3cSnapshotSource source,
    const RuntimeOwnerTransition malformed,
    const std::uint32_t initial_dispatch_sequence = 41,
    const C1b3cPostViewMutation post_view_mutation =
        C1b3cPostViewMutation::None,
    const C1b3cMalformedExercise exercise =
        C1b3cMalformedExercise::Validator)
{
    RuntimeOwnerTransition exercised_transition = malformed;
    if (exercise == C1b3cMalformedExercise::IntentionalPendingBypass) {
        exercised_transition.disposition =
            static_cast<RuntimeOwnerDisposition>(255);
    }
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    const TrustedReceipt head = make_snapshot_receipt(
        c1b3c_snapshot_receipt_kind(source),
        freeze,
        c1b3c_snapshot_diagnostic_code(source));
    constexpr TrustedReceipt trailing =
        make_transport_attempt_failed_receipt(1, 91);
    const std::uint32_t expected_head_ingress =
        adapter.view().last_trusted_ingress_sequence + 1;
    const std::uint32_t expected_trailing_ingress =
        expected_head_ingress + 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, head) == TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, trailing) == TrustedEnqueueResult::Accepted);
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    if (exercise == C1b3cMalformedExercise::Validator) {
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
    }
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_accepted_liveness_mask(
        adapter, 0x0f);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, initial_dispatch_sequence);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, exercised_transition);

    RuntimeOwnerView observed_post_view =
        make_c1b3c_canonical_snapshot_post_view(
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).core,
            source);
    if (post_view_mutation != C1b3cPostViewMutation::None) {
        mutate_c1b3c_snapshot_post_view(
            observed_post_view, post_view_mutation);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter, observed_post_view);
    }

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_before = adapter.view();
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(!before.core.boot_orchestration_ended);
    CHECK(before.trusted_count == 2);
    CHECK(before.trusted_slots[before.trusted_head].ingress_sequence ==
          expected_head_ingress);
    CHECK(trusted_receipts_equal(
        before.trusted_slots[before.trusted_head].receipt, head));
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count ==
          (exercise == C1b3cMalformedExercise::Validator ? 0 : 1));
    if (exercise == C1b3cMalformedExercise::Validator) {
        CHECK(initial_dispatch_sequence == 41);
    }
    CHECK(before.accepted_liveness_mask == 0x0f);
    CHECK(!before.boot_end_released);
    CHECK(before.last_trusted_receipt_signature.ingress_sequence == 67);
    CHECK(before.last_trusted_diagnostic_ingress_sequence ==
          expected_trailing_ingress);
    CHECK(before.last_trusted_diagnostic_code == 91);
    CHECK(public_before.trusted_stale_count == 73);
    CHECK(public_before.trusted_duplicate_count == 79);
    CHECK(public_before.trusted_protocol_violation_count == 83);
    CHECK(!before.core_fail_closed_latched);
    CHECK(!before.core_adapter_fatal_latched);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter) ==
          (post_view_mutation != C1b3cPostViewMutation::None));

    const RuntimeOwnerPhase expected_result_phase =
        post_view_mutation == C1b3cPostViewMutation::UnknownPhase
            ? RuntimeOwnerPhase::SnapshotFreezePending
            : observed_post_view.phase;
    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::SnapshotFreezePending,
        expected_result_phase,
        expected_head_ingress);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const RuntimeOwnerAdapterView public_after = adapter.view();
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b3c_canonical_snapshot_post_view(before.core, source)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    const std::uint32_t snapshot_maximum =
        std::numeric_limits<std::uint32_t>::max();
    const bool expected_sequence_bypass =
        (source == C1b3cSnapshotSource::Failed &&
         initial_dispatch_sequence >= snapshot_maximum - 1) ||
        (source == C1b3cSnapshotSource::Succeeded &&
         initial_dispatch_sequence > snapshot_maximum - 3);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_snapshot_validation_bypass_used(adapter) ==
          expected_sequence_bypass);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));

    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.trusted_high_water == before.trusted_high_water);
    CHECK(after.last_trusted_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.last_trusted_diagnostic_ingress_sequence ==
          before.last_trusted_diagnostic_ingress_sequence);
    CHECK(after.last_trusted_diagnostic_code ==
          before.last_trusted_diagnostic_code);
    CHECK(public_after.trusted_stale_count ==
          public_before.trusted_stale_count);
    CHECK(public_after.trusted_duplicate_count ==
          public_before.trusted_duplicate_count);
    CHECK(public_after.trusted_protocol_violation_count ==
          public_before.trusted_protocol_violation_count);
    CHECK(after.trusted_rejected_full_count ==
          before.trusted_rejected_full_count);

    CHECK(after.normal_count == 0);
    CHECK(after.normal_head == 0);
    CHECK(after.normal_tail == 0);
    CHECK(after.normal_high_water == before.normal_high_water);
    CHECK(after.last_normal_enqueue_sequence ==
          before.last_normal_enqueue_sequence);
    CHECK(after.normal_coalesced_count ==
          before.normal_coalesced_count);
    CHECK(after.normal_rejected_full_count ==
          before.normal_rejected_full_count);
    for (const RuntimeOwnerAdapterNormalSlotSnapshot slot :
         after.normal_slots) {
        CHECK(normal_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterNormalSlotSnapshot{}));
    }
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.transport_request_pending);
    CHECK(!after.boot_end_released);
    CHECK(public_after.boot_end_released == 0);
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.reason_mask == (1u << 6u));
    CHECK(after.critical.first_ingress_sequence ==
          expected_head_ingress);
    CHECK(after.critical.last_ingress_sequence ==
          expected_head_ingress);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    const bool suppress_synthetic_pair =
        post_view_mutation == C1b3cPostViewMutation::ShutdownPhase;
    const bool safety_blocked =
        !suppress_synthetic_pair &&
        initial_dispatch_sequence >= maximum - 1;
    const bool terminal_reserve =
        !suppress_synthetic_pair && !safety_blocked &&
        initial_dispatch_sequence >= maximum - 4;
    if (suppress_synthetic_pair || safety_blocked) {
        CHECK(after.pending_effect_count == 0);
        CHECK(after.pending_effect_head == 0);
        CHECK(after.pending_effect_tail == 0);
        CHECK(after.last_dispatch_sequence == initial_dispatch_sequence);
        check_unused_pending_effect_slots_are_zero(after, 0);
        CHECK(!after.dispatch_fatal_latched);
        CHECK(after.safety_delivery_blocked == safety_blocked);
    } else {
        const std::uint32_t record_sequence = terminal_reserve
            ? maximum - 1
            : initial_dispatch_sequence + 1;
        const std::uint32_t recovery_sequence = terminal_reserve
            ? maximum
            : initial_dispatch_sequence + 2;
        CHECK(after.last_dispatch_sequence == recovery_sequence);
        check_canonical_recovery_pending_pair(
            after,
            record_sequence,
            recovery_sequence,
            RuntimeOwnerFaultCode::InternalInvariant,
            0,
            {});
        CHECK(after.dispatch_fatal_latched == terminal_reserve);
        CHECK(!after.safety_delivery_blocked);
    }
    for (const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot :
         after.pending_effect_slots) {
        CHECK(slot.effect.kind !=
              RuntimeOwnerEffectKind::EndBootOrchestration);
        CHECK(slot.effect.fault_code !=
              RuntimeOwnerFaultCode::SnapshotFailure);
    }
    CHECK(has_safe_default(public_after.current_dispatch));
    CHECK(has_safe_default(public_after.physical_inflight));
    CHECK(public_after.last_ack_dispatch_sequence ==
          public_before.last_ack_dispatch_sequence);
    CHECK(public_after.physical_inflight_cancel_pending == 0);

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        after.core.phase,
        !suppress_synthetic_pair && !safety_blocked,
        submit_count_before + 1);
}

void check_c1b3c_snapshot_dispositions_fail_closed(
    const C1b3cSnapshotSource source)
{
    constexpr std::array<RuntimeOwnerDisposition, 4> corruptions{{
        static_cast<RuntimeOwnerDisposition>(255),
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    }};
    for (const RuntimeOwnerDisposition disposition : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3c_canonical_transition_for_source(source);
        malformed.disposition = disposition;
        check_c1b3c_snapshot_malformed_fallback(source, malformed);
    }
}

void test_c1b3c_snapshot_succeeded_dispositions_fail_closed()
{
    check_c1b3c_snapshot_dispositions_fail_closed(
        C1b3cSnapshotSource::Succeeded);
}

void test_c1b3c_snapshot_failed_dispositions_fail_closed()
{
    check_c1b3c_snapshot_dispositions_fail_closed(
        C1b3cSnapshotSource::Failed);
}

void check_c1b3c_snapshot_transition_phases_fail_closed(
    const C1b3cSnapshotSource source)
{
    std::array<RuntimeOwnerTransition, 4> corruptions{};
    for (RuntimeOwnerTransition &transition : corruptions) {
        transition = make_c1b3c_canonical_transition_for_source(source);
    }
    corruptions[0].phase_before = RuntimeOwnerPhase::ColdStart;
    corruptions[1].phase_before = static_cast<RuntimeOwnerPhase>(255);
    corruptions[2].phase_after = RuntimeOwnerPhase::LivenessWaiting;
    corruptions[3].phase_after = static_cast<RuntimeOwnerPhase>(255);
    for (const RuntimeOwnerTransition &malformed : corruptions) {
        check_c1b3c_snapshot_malformed_fallback(source, malformed);
    }
}

void test_c1b3c_snapshot_succeeded_transition_phases_fail_closed()
{
    check_c1b3c_snapshot_transition_phases_fail_closed(
        C1b3cSnapshotSource::Succeeded);
}

void test_c1b3c_snapshot_failed_transition_phases_fail_closed()
{
    check_c1b3c_snapshot_transition_phases_fail_closed(
        C1b3cSnapshotSource::Failed);
}

void check_c1b3c_snapshot_effect_counts_fail_closed(
    const C1b3cSnapshotSource source)
{
    const std::array<std::uint8_t, 4> corruptions =
        c1b3c_snapshot_failed(source)
            ? std::array<std::uint8_t, 4>{{0, 1, 3, 5}}
            : std::array<std::uint8_t, 4>{{0, 2, 4, 5}};
    for (const std::uint8_t effect_count : corruptions) {
        RuntimeOwnerTransition malformed =
            make_c1b3c_canonical_transition_for_source(source);
        malformed.effect_count = effect_count;
        check_c1b3c_snapshot_malformed_fallback(source, malformed);
    }
}

void test_c1b3c_snapshot_succeeded_effect_counts_fail_closed()
{
    check_c1b3c_snapshot_effect_counts_fail_closed(
        C1b3cSnapshotSource::Succeeded);
}

void test_c1b3c_snapshot_failed_effect_counts_fail_closed()
{
    check_c1b3c_snapshot_effect_counts_fail_closed(
        C1b3cSnapshotSource::Failed);
}

void corrupt_c1b3c_snapshot_effect_field(
    RuntimeOwnerEffect &effect,
    const std::size_t field_index,
    const bool used)
{
    switch (field_index) {
    case 0:
        effect.kind = used
            ? RuntimeOwnerEffectKind::StartAtProbe
            : RuntimeOwnerEffectKind::RecordFault;
        break;
    case 1:
        ++effect.correlation_id;
        break;
    case 2:
        ++effect.attempt.mqtt_session_id;
        break;
    case 3:
        ++effect.attempt.mqtt_generation;
        break;
    case 4:
        ++effect.attempt.config_apply_epoch;
        break;
    case 5:
        effect.fault_code = RuntimeOwnerFaultCode::InternalInvariant;
        break;
    default:
        break;
    }
}

void check_c1b3c_snapshot_effect_slot_fields_fail_closed(
    const C1b3cSnapshotSource source,
    const std::size_t slot_index,
    const bool used)
{
    for (std::size_t field_index = 0; field_index < 6; ++field_index) {
        RuntimeOwnerTransition malformed =
            make_c1b3c_canonical_transition_for_source(source);
        corrupt_c1b3c_snapshot_effect_field(
            malformed.effects[slot_index], field_index, used);
        check_c1b3c_snapshot_malformed_fallback(source, malformed);
    }
}

void test_c1b3c_snapshot_succeeded_used_slot0_fields_fail_closed()
{
    check_c1b3c_snapshot_effect_slot_fields_fail_closed(
        C1b3cSnapshotSource::Succeeded, 0, true);
}

void test_c1b3c_snapshot_failed_used_slot0_fields_fail_closed()
{
    check_c1b3c_snapshot_effect_slot_fields_fail_closed(
        C1b3cSnapshotSource::Failed, 0, true);
}

void test_c1b3c_snapshot_failed_used_slot1_fields_fail_closed()
{
    check_c1b3c_snapshot_effect_slot_fields_fail_closed(
        C1b3cSnapshotSource::Failed, 1, true);
}

void test_c1b3c_snapshot_succeeded_unused_slot3_fields_fail_closed()
{
    check_c1b3c_snapshot_effect_slot_fields_fail_closed(
        C1b3cSnapshotSource::Succeeded, 3, false);
}

void test_c1b3c_snapshot_failed_unused_slot3_fields_fail_closed()
{
    check_c1b3c_snapshot_effect_slot_fields_fail_closed(
        C1b3cSnapshotSource::Failed, 3, false);
}

void check_c1b3c_snapshot_post_view_phases_fail_closed(
    const C1b3cSnapshotSource source)
{
    for (const C1b3cPostViewMutation mutation : {
             C1b3cPostViewMutation::KnownWrongPhase,
             C1b3cPostViewMutation::UnknownPhase,
             C1b3cPostViewMutation::ShutdownPhase,
         }) {
        check_c1b3c_snapshot_malformed_fallback(
            source,
            make_c1b3c_canonical_transition_for_source(source),
            41,
            mutation);
    }
}

void test_c1b3c_snapshot_succeeded_post_view_phases_fail_closed()
{
    check_c1b3c_snapshot_post_view_phases_fail_closed(
        C1b3cSnapshotSource::Succeeded);
}

void test_c1b3c_snapshot_failed_post_view_phases_fail_closed()
{
    check_c1b3c_snapshot_post_view_phases_fail_closed(
        C1b3cSnapshotSource::Failed);
}

void check_c1b3c_snapshot_post_view_fields_fail_closed(
    const C1b3cSnapshotSource source)
{
    constexpr std::array<C1b3cPostViewMutation, 11> mutations{{
        C1b3cPostViewMutation::MqttSession,
        C1b3cPostViewMutation::MqttGeneration,
        C1b3cPostViewMutation::MqttGenerationCounter,
        C1b3cPostViewMutation::ConfigApplyEpochCounter,
        C1b3cPostViewMutation::LastConfigCommitSequence,
        C1b3cPostViewMutation::LastCorrelationId,
        C1b3cPostViewMutation::ActiveAttemptSession,
        C1b3cPostViewMutation::ActiveAttemptGeneration,
        C1b3cPostViewMutation::ActiveAttemptEpoch,
        C1b3cPostViewMutation::BootOrchestrationEnded,
        C1b3cPostViewMutation::LastFault,
    }};
    for (const C1b3cPostViewMutation mutation : mutations) {
        check_c1b3c_snapshot_malformed_fallback(
            source,
            make_c1b3c_canonical_transition_for_source(source),
            41,
            mutation);
    }
}

void test_c1b3c_snapshot_succeeded_post_view_fields_fail_closed()
{
    check_c1b3c_snapshot_post_view_fields_fail_closed(
        C1b3cSnapshotSource::Succeeded);
}

void test_c1b3c_snapshot_failed_post_view_fields_fail_closed()
{
    check_c1b3c_snapshot_post_view_fields_fail_closed(
        C1b3cSnapshotSource::Failed);
}

void check_c1b3c_snapshot_sequence_reserves_and_damage(
    const C1b3cSnapshotSource source)
{
    RuntimeOwnerTransition malformed =
        make_c1b3c_canonical_transition_for_source(source);
    malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
    check_c1b3c_snapshot_malformed_fallback(
        source,
        malformed,
        41,
        C1b3cPostViewMutation::None,
        C1b3cMalformedExercise::IntentionalPendingBypass);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    for (const std::uint32_t terminal_start : {
             maximum - 4,
             maximum - 3,
             maximum - 2,
        }) {
        check_c1b3c_snapshot_malformed_fallback(
            source,
            malformed,
            terminal_start,
            C1b3cPostViewMutation::None,
            C1b3cMalformedExercise::IntentionalPendingBypass);
    }
    for (const std::uint32_t damaged_start : {
             maximum - 1,
             maximum,
        }) {
        check_c1b3c_snapshot_malformed_fallback(
            source,
            malformed,
            damaged_start,
            C1b3cPostViewMutation::None,
            C1b3cMalformedExercise::IntentionalPendingBypass);
    }
}

void test_c1b3c_snapshot_succeeded_sequence_reserves_and_damage()
{
    check_c1b3c_snapshot_sequence_reserves_and_damage(
        C1b3cSnapshotSource::Succeeded);
}

void test_c1b3c_snapshot_failed_sequence_reserves_and_damage()
{
    check_c1b3c_snapshot_sequence_reserves_and_damage(
        C1b3cSnapshotSource::Failed);
}

void test_c1b3c_snapshot_succeeded_canonical_override_pending_bypass_fails_closed()
{
    check_c1b3c_snapshot_malformed_fallback(
        C1b3cSnapshotSource::Succeeded,
        make_c1b3c_canonical_transition_for_source(
            C1b3cSnapshotSource::Succeeded),
        41,
        C1b3cPostViewMutation::None,
        C1b3cMalformedExercise::IntentionalPendingBypass);
}

void test_c1b3c_snapshot_failed_canonical_override_pending_bypass_fails_closed()
{
    check_c1b3c_snapshot_malformed_fallback(
        C1b3cSnapshotSource::Failed,
        make_c1b3c_canonical_transition_for_source(
            C1b3cSnapshotSource::Failed),
        41,
        C1b3cPostViewMutation::None,
        C1b3cMalformedExercise::IntentionalPendingBypass);
}

void test_c1b3c_snapshot_succeeded_canonical_override_max_sequence_bypass_fails_closed()
{
    check_c1b3c_snapshot_malformed_fallback(
        C1b3cSnapshotSource::Succeeded,
        make_c1b3c_canonical_transition_for_source(
            C1b3cSnapshotSource::Succeeded),
        std::numeric_limits<std::uint32_t>::max(),
        C1b3cPostViewMutation::None,
        C1b3cMalformedExercise::IntentionalPendingBypass);
}

void test_c1b3c_snapshot_failed_canonical_override_unavailable_safety_plan_fails_closed()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    const TrustedReceipt receipt = make_snapshot_receipt(
        TrustedReceiptKind::SnapshotFailed, freeze, 97);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, maximum);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_c1b3c_canonical_snapshot_failed_transition(
            before.core, receipt));
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter,
            make_c1b3c_canonical_snapshot_post_view(
                before.core, C1b3cSnapshotSource::Failed));

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RecoveryPending,
        before.last_trusted_ingress_sequence);

    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(
        after.core,
        make_c1b3c_canonical_snapshot_post_view(
            before.core, C1b3cSnapshotSource::Failed)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_last_snapshot_validation_bypass_used(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_transition_override_pending(adapter));
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
    CHECK(after.normal_count == 0);
    CHECK(after.pending_effect_count == 0);
    CHECK(after.pending_effect_head == 0);
    CHECK(after.pending_effect_tail == 0);
    check_unused_pending_effect_slots_are_zero(after, 0);
    CHECK(after.last_dispatch_sequence == maximum);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(!after.boot_end_released);
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.core_adapter_fatal_latched);
    CHECK(!after.core_fail_closed_latched);
    CHECK(after.safety_delivery_blocked);
    CHECK(!after.dispatch_fatal_latched);
    CHECK(!after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(after.critical.reason_mask == (1u << 6u));
    CHECK(after.critical.first_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.critical.last_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::RejectedFatal);
    check_malformed_fatal_safety_then_terminal(
        adapter,
        RuntimeOwnerPhase::RecoveryPending,
        false,
        submit_count_before + 1);
}

enum class C1b3cAuthorizationMismatch : std::uint8_t {
    Phase = 0,
    BootEnded,
    Mask,
    EffectKind,
    Correlation,
    Session,
    Generation,
    Epoch,
};

void check_c1b3c_unauthorized_pending_override_remains_unconsumed(
    const C1b3cSnapshotSource source,
    const C1b3cAuthorizationMismatch mismatch)
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerEffect freeze{
        RuntimeOwnerEffectKind::FreezeBootSnapshot,
        1,
        {77, 1, 1},
        RuntimeOwnerFaultCode::None,
    };
    if (mismatch != C1b3cAuthorizationMismatch::Phase) {
        freeze = fixture_prepare_snapshot_freeze_pending_via_config(
            adapter);
    }
    TrustedReceipt receipt = make_snapshot_receipt(
        c1b3c_snapshot_receipt_kind(source),
        freeze,
        c1b3c_snapshot_diagnostic_code(source));
    switch (mismatch) {
    case C1b3cAuthorizationMismatch::Phase:
        break;
    case C1b3cAuthorizationMismatch::BootEnded:
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_core_boot_orchestration_ended(adapter, true);
        break;
    case C1b3cAuthorizationMismatch::Mask:
        break;
    case C1b3cAuthorizationMismatch::EffectKind:
        receipt.effect_kind = RuntimeOwnerEffectKind::StartAtProbe;
        break;
    case C1b3cAuthorizationMismatch::Correlation:
        ++receipt.correlation_id;
        break;
    case C1b3cAuthorizationMismatch::Session:
        ++receipt.mqtt_session_id;
        break;
    case C1b3cAuthorizationMismatch::Generation:
        ++receipt.mqtt_generation;
        break;
    case C1b3cAuthorizationMismatch::Epoch:
        ++receipt.config_apply_epoch;
        break;
    }
    if (mismatch == C1b3cAuthorizationMismatch::EffectKind) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_enqueue_trusted_receipt_unchecked(
                      adapter, receipt));
    } else {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
    }
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_seed_trusted_fallback_nonqueue_state(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_accepted_liveness_mask(
        adapter,
        mismatch == C1b3cAuthorizationMismatch::Mask ? 0x0e : 0x0f);
    const RuntimeOwnerAdapterPrivateSnapshot seeded =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter,
        make_c1b3c_canonical_snapshot_transition(
            seeded.core, receipt, source));
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter,
            make_c1b3c_canonical_snapshot_post_view(
                seeded.core, source));
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    if (mismatch == C1b3cAuthorizationMismatch::Phase) {
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Idle,
            RuntimeOwnerDisposition::Rejected,
            before.core.phase,
            before.core.phase);
        CHECK(private_snapshots_equal(
            before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    } else {
        const std::uint32_t ingress_sequence =
            before.trusted_slots[before.trusted_head].ingress_sequence;
        check_exact_ingress_step_result(
            adapter.step(),
            AdapterStepAction::TrustedReceiptDiscarded,
            RuntimeOwnerDisposition::Rejected,
            before.core.phase,
            before.core.phase,
            ingress_sequence);
        CHECK(adapter.view().trusted_depth == 0);
        CHECK(adapter.view().trusted_stale_count ==
              before.trusted_stale_count + 1);
    }
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_transition_override_pending(adapter));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter));
}

void test_c1b3c_snapshot_pending_override_requires_full_authorization()
{
    constexpr std::array<C1b3cAuthorizationMismatch, 8> mismatches{{
        C1b3cAuthorizationMismatch::Phase,
        C1b3cAuthorizationMismatch::BootEnded,
        C1b3cAuthorizationMismatch::Mask,
        C1b3cAuthorizationMismatch::EffectKind,
        C1b3cAuthorizationMismatch::Correlation,
        C1b3cAuthorizationMismatch::Session,
        C1b3cAuthorizationMismatch::Generation,
        C1b3cAuthorizationMismatch::Epoch,
    }};
    for (const C1b3cSnapshotSource source : {
             C1b3cSnapshotSource::Succeeded,
             C1b3cSnapshotSource::Failed,
         }) {
        for (const C1b3cAuthorizationMismatch mismatch : mismatches) {
            check_c1b3c_unauthorized_pending_override_remains_unconsumed(
                source, mismatch);
        }
    }
}

void test_c1b3c_snapshot_exact_signature_only_pending_override_remains_unconsumed()
{
    for (const C1b3cSnapshotSource source : {
             C1b3cSnapshotSource::Succeeded,
             C1b3cSnapshotSource::Failed,
         }) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        const TrustedReceipt receipt = make_snapshot_receipt(
            c1b3c_snapshot_receipt_kind(source),
            freeze,
            c1b3c_snapshot_diagnostic_code(source));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        const std::uint32_t head_ingress_sequence =
            adapter.view().last_trusted_ingress_sequence;
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_trusted_fallback_nonqueue_state(adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_accepted_liveness_mask(adapter, 0x0f);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_trusted_receipt_signature(
                adapter, head_ingress_sequence, receipt);
        const RuntimeOwnerAdapterPrivateSnapshot seeded =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(
                adapter,
                make_c1b3c_canonical_snapshot_transition(
                    seeded.core, receipt, source));
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter,
                make_c1b3c_canonical_snapshot_post_view(
                    seeded.core, source));
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        CHECK(before.core.phase ==
              RuntimeOwnerPhase::SnapshotFreezePending);
        CHECK(!before.core.boot_orchestration_ended);
        CHECK(before.accepted_liveness_mask == 0x0f);
        CHECK(before.pending_effect_count == 1);
        CHECK(before.trusted_count == 1);
        const RuntimeOwnerAdapterTrustedSlotSnapshot head =
            before.trusted_slots[before.trusted_head];
        CHECK(head.ingress_sequence == head_ingress_sequence);
        CHECK(trusted_receipts_equal(head.receipt, receipt));
        CHECK(receipt.effect_kind ==
              RuntimeOwnerEffectKind::FreezeBootSnapshot);
        CHECK(receipt.correlation_id ==
              before.core.last_correlation_id - 1);
        CHECK(receipt.mqtt_session_id ==
              before.core.active_attempt.mqtt_session_id);
        CHECK(receipt.mqtt_generation ==
              before.core.active_attempt.mqtt_generation);
        CHECK(receipt.config_apply_epoch ==
              before.core.active_attempt.config_apply_epoch);
        CHECK(before.last_trusted_receipt_signature.ingress_sequence ==
              head_ingress_sequence);
        CHECK(trusted_receipts_equal(
            before.last_trusted_receipt_signature.receipt, receipt));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_transition_override_pending(adapter));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_post_submit_view_override_pending(adapter));

        check_exact_ingress_step_result(
            adapter.step(),
            AdapterStepAction::TrustedReceiptDiscarded,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::SnapshotFreezePending,
            RuntimeOwnerPhase::SnapshotFreezePending,
            head_ingress_sequence);
        CHECK(adapter.view().trusted_depth == 0);
        CHECK(adapter.view().trusted_duplicate_count ==
              before.trusted_duplicate_count + 1);
        CHECK(adapter.view().pending_effect_count ==
              before.pending_effect_count);
        CHECK(adapter_dispatches_equal(
            adapter.view().physical_inflight,
            before.physical_inflight));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_transition_override_pending(adapter));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_post_submit_view_override_pending(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_last_snapshot_validation_bypass_used(adapter));
    }
}

void test_c1b3c_snapshot_duplicate_pending_overrides_remain_unconsumed()
{
    for (const C1b3cSnapshotSource source : {
             C1b3cSnapshotSource::Succeeded,
             C1b3cSnapshotSource::Failed,
         }) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        const TrustedReceipt receipt = make_snapshot_receipt(
            c1b3c_snapshot_receipt_kind(source),
            freeze,
            c1b3c_snapshot_diagnostic_code(source));
        const RuntimeOwnerAdapterPrivateSnapshot accepted_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        if (c1b3c_snapshot_failed(source)) {
            check_snapshot_failed_accepts(
                adapter,
                receipt,
                accepted_before.last_dispatch_sequence + 1,
                accepted_before.last_dispatch_sequence + 2,
                false);
        } else {
            check_snapshot_succeeded_accepts(adapter, receipt);
        }
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot seeded =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(
                adapter,
                make_c1b3c_canonical_snapshot_transition(
                    seeded.core, receipt, source));
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter,
                make_c1b3c_canonical_snapshot_post_view(
                    seeded.core, source));
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Idle,
            RuntimeOwnerDisposition::Rejected,
            before.core.phase,
            before.core.phase);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_transition_override_pending(adapter));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::
                  fixture_core_post_submit_view_override_pending(adapter));
    }
}

void test_c1b3c_snapshot_valid_overrides_preserve_success_and_failure_paths()
{
    for (const C1b3cSnapshotSource source : {
             C1b3cSnapshotSource::Succeeded,
             C1b3cSnapshotSource::Failed,
         }) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        const TrustedReceipt receipt = make_snapshot_receipt(
            c1b3c_snapshot_receipt_kind(source),
            freeze,
            c1b3c_snapshot_diagnostic_code(source));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(
                adapter,
                make_c1b3c_canonical_snapshot_transition(
                    before.core, receipt, source));
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter,
                make_c1b3c_canonical_snapshot_post_view(
                    before.core, source));

        check_exact_ingress_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::SnapshotFreezePending,
            c1b3c_snapshot_failed(source)
                ? RuntimeOwnerPhase::RecoveryPending
                : RuntimeOwnerPhase::RuntimeReady,
            before.last_trusted_ingress_sequence);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(runtime_owner_views_equal(
            after.core,
            make_c1b3c_canonical_snapshot_post_view(
                before.core, source)));
        CHECK(after.trusted_count == 0);
        CHECK(after.last_trusted_receipt_signature.ingress_sequence ==
              before.last_trusted_ingress_sequence);
        CHECK(trusted_receipts_equal(
            after.last_trusted_receipt_signature.receipt, receipt));
        CHECK(after.accepted_liveness_mask == 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before + 1);
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_last_snapshot_validation_bypass_used(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_core_transition_override_pending(adapter));
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_core_post_submit_view_override_pending(adapter));
        CHECK(!after.core_adapter_fatal_latched);
        CHECK(!after.core_fail_closed_latched);
        CHECK(!after.critical_pending);
        if (c1b3c_snapshot_failed(source)) {
            CHECK(!after.boot_end_released);
            CHECK(after.last_dispatch_sequence ==
                  before.last_dispatch_sequence + 2);
            check_canonical_recovery_pending_pair(
                after,
                before.last_dispatch_sequence + 1,
                before.last_dispatch_sequence + 2,
                RuntimeOwnerFaultCode::SnapshotFailure,
                receipt.correlation_id,
                before.core.active_attempt);
        } else {
            CHECK(!after.boot_end_released);
            CHECK(after.last_dispatch_sequence ==
                  before.last_dispatch_sequence + 1);
            CHECK(after.pending_effect_count == 1);
            const RuntimeOwnerAdapterPendingEffectSlotSnapshot end_boot =
                after.pending_effect_slots[after.pending_effect_head];
            CHECK(end_boot.preassigned_dispatch_sequence ==
                  before.last_dispatch_sequence + 1);
            CHECK(end_boot.effect.kind ==
                  RuntimeOwnerEffectKind::EndBootOrchestration);
            CHECK(end_boot.effect.correlation_id ==
                  before.core.last_correlation_id);
            CHECK(end_boot.effect.attempt == before.core.active_attempt);
            CHECK(end_boot.effect.fault_code == RuntimeOwnerFaultCode::None);
            check_unused_pending_effect_slots_are_zero(after, 1);
        }
    }
}

void test_task4c_c1b3c_snapshot_malformed_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    for (const C1b3cSnapshotSource source : {
             C1b3cSnapshotSource::Succeeded,
             C1b3cSnapshotSource::Failed,
         }) {
        RuntimeOwnerTransition malformed =
            make_c1b3c_canonical_transition_for_source(source);
        malformed.disposition = static_cast<RuntimeOwnerDisposition>(255);
        check_c1b3c_snapshot_malformed_fallback(source, malformed);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_snapshot_succeeded_requires_exact_end_boot_delivery_ack()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, 41);
    check_snapshot_succeeded_accepts(
        adapter,
        make_snapshot_receipt(
            TrustedReceiptKind::SnapshotSucceeded, freeze));

    CHECK(adapter.normal_port().submit(make_telemetry_intent(7, 9)) ==
          NormalSubmitResult::RejectedNotReady);
    CHECK(adapter.view().boot_end_released == 0);
    CHECK(adapter.view().pending_effect_count == 1);
    CHECK(has_safe_default(adapter.view().physical_inflight));

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
    CHECK(offered.dispatch_sequence == 42);
    CHECK(offered.enqueue_sequence == 0);
    CHECK(offered.effect.kind ==
          RuntimeOwnerEffectKind::EndBootOrchestration);
    CHECK(offered.effect.correlation_id == freeze.correlation_id + 1);
    CHECK(offered.effect.attempt == freeze.attempt);
    CHECK(offered.effect.fault_code == RuntimeOwnerFaultCode::None);
    CHECK(adapter.view().pending_effect_count == 0);

    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence + 1) ==
          DispatchAckResult::RejectedWrongSequence);
    CHECK(adapter.view().boot_end_released == 0);
    CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), offered));
    CHECK(has_safe_default(adapter.view().physical_inflight));

    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedDelivery);
    CHECK(adapter.view().boot_end_released == 1);
    CHECK(has_safe_default(adapter.peek_dispatch()));
    CHECK(has_safe_default(adapter.view().physical_inflight));
    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedDuplicate);
    CHECK(adapter.normal_port().submit(make_telemetry_intent(7, 9)) ==
          NormalSubmitResult::Accepted);
}

void test_snapshot_succeeded_dispatch_sequence_shortage_is_bounded()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, maximum - 3);
        check_snapshot_succeeded_accepts(
            adapter,
            make_snapshot_receipt(
                TrustedReceiptKind::SnapshotSucceeded, freeze));
        CHECK(adapter.view().last_dispatch_sequence == maximum - 2);
    }

    for (const std::uint32_t saturated_start : {
             maximum - 2,
             maximum - 1,
             maximum,
         }) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, saturated_start);
        const TrustedReceipt receipt = make_snapshot_receipt(
            TrustedReceiptKind::SnapshotSucceeded, freeze);
        CHECK(adapter.trusted_receipt_port().submit(receipt) ==
              TrustedIngressResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::SnapshotFreezePending,
            RuntimeOwnerPhase::SnapshotFreezePending);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(runtime_owner_views_equal(after.core, before.core));
        CHECK(after.trusted_count == before.trusted_count);
        CHECK(after.pending_effect_count == 0);
        CHECK(after.last_dispatch_sequence == saturated_start);
        CHECK(!after.boot_end_released);
        CHECK(after.critical_pending);
        CHECK(after.critical.last_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_last_snapshot_validation_bypass_used(adapter));
    }
}

void test_snapshot_failed_happy_commits_canonical_safety_pair()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, 41);
    check_snapshot_failed_accepts(
        adapter,
        make_snapshot_receipt(
            TrustedReceiptKind::SnapshotFailed, freeze, 77),
        42,
        43,
        false);
}

void test_snapshot_succeeded_duplicate_after_runtime_ready_is_adapter_duplicate()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    const TrustedReceipt receipt = make_snapshot_receipt(
        TrustedReceiptKind::SnapshotSucceeded, freeze);
    check_snapshot_succeeded_accepts(adapter, receipt);

    const RuntimeOwnerView accepted_core = adapter.view().core;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RuntimeReady, 0, 1);
    CHECK(runtime_owner_views_equal(accepted_core, adapter.view().core));
    CHECK(adapter.view().boot_end_released == 1);
    CHECK(adapter.view().pending_effect_count == 0);
}

void test_snapshot_failed_exact_signature_duplicate_precedes_authorization()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const TrustedReceipt receipt = make_snapshot_receipt(
        TrustedReceiptKind::SnapshotFailed, freeze, 78);
    check_snapshot_failed_accepts(
        adapter,
        receipt,
        before.last_dispatch_sequence + 1,
        before.last_dispatch_sequence + 2,
        false);
    RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(adapter);

    const RuntimeOwnerView accepted_core = adapter.view().core;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, receipt) == TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RecoveryPending, 0, 1);
    CHECK(runtime_owner_views_equal(accepted_core, adapter.view().core));
    CHECK(adapter.view().boot_end_released == 0);

    TrustedReceipt changed_diagnostic = receipt;
    ++changed_diagnostic.diagnostic_code;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, changed_diagnostic) ==
          TrustedEnqueueResult::Accepted);
    check_classified_trusted_discard(
        adapter, RuntimeOwnerPhase::RecoveryPending, 1, 1);
    CHECK(runtime_owner_views_equal(accepted_core, adapter.view().core));
}

void test_snapshot_wrong_phase_token_fields_and_mask_are_stale()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::SnapshotSucceeded,
        TrustedReceiptKind::SnapshotFailed,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        const TrustedReceipt exact =
            make_snapshot_receipt(outcome, freeze, 0);
        std::array<TrustedReceipt, 4> mismatches{{
            exact,
            exact,
            exact,
            exact,
        }};
        ++mismatches[0].correlation_id;
        ++mismatches[1].mqtt_session_id;
        ++mismatches[2].mqtt_generation;
        ++mismatches[3].config_apply_epoch;
        for (std::size_t index = 0; index < mismatches.size(); ++index) {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, mismatches[index]) ==
                  TrustedEnqueueResult::Accepted);
            check_classified_trusted_discard(
                adapter,
                RuntimeOwnerPhase::SnapshotFreezePending,
                static_cast<std::uint32_t>(index + 1),
                0);
            CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
                      .accepted_liveness_mask == 0x0f);
            CHECK(adapter.view().boot_end_released == 0);
        }

        RuntimeOwnerAdapterCore wrong_phase{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  wrong_phase, exact) == TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            wrong_phase, RuntimeOwnerPhase::ColdStart, 1, 0);

        RuntimeOwnerAdapterCore incomplete_mask{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            incomplete_mask,
            RuntimeOwnerPhase::SnapshotFreezePending));
        const RuntimeOwnerView incomplete_core = incomplete_mask.view().core;
        const RuntimeOwnerEffect exact_core_freeze{
            RuntimeOwnerEffectKind::FreezeBootSnapshot,
            incomplete_core.last_correlation_id - 1,
            incomplete_core.active_attempt,
            RuntimeOwnerFaultCode::None,
        };
        CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(incomplete_mask)
                  .accepted_liveness_mask == 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  incomplete_mask,
                  make_snapshot_receipt(outcome, exact_core_freeze)) ==
              TrustedEnqueueResult::Accepted);
        check_classified_trusted_discard(
            incomplete_mask,
            RuntimeOwnerPhase::SnapshotFreezePending,
            1,
            0);
    }
}

void test_snapshot_pending_effect_defers_without_dequeue()
{
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::SnapshotSucceeded,
        TrustedReceiptKind::SnapshotFailed,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(
                adapter, true);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, make_snapshot_receipt(outcome, freeze)) ==
              TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const AdapterStepResult prepared = adapter.step();
        CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
        CHECK(prepared.prepared_dispatch_sequence ==
              before.pending_effect_slots[before.pending_effect_head]
                  .preassigned_dispatch_sequence);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(runtime_owner_effects_equal(offered.effect, freeze));
        CHECK(adapter.acknowledge_dispatch(
                  offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedOperationInflight);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
    }
}

void test_snapshot_failed_diagnostic_is_not_forwarded_to_core_or_effects()
{
    RuntimeOwnerAdapterCore zero_diagnostic{};
    RuntimeOwnerAdapterCore max_diagnostic{};
    const RuntimeOwnerEffect zero_freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(
            zero_diagnostic);
    const RuntimeOwnerEffect max_freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(
            max_diagnostic);
    const RuntimeOwnerAdapterPrivateSnapshot zero_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(zero_diagnostic);
    const RuntimeOwnerAdapterPrivateSnapshot max_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(max_diagnostic);
    check_snapshot_failed_accepts(
        zero_diagnostic,
        make_snapshot_receipt(
            TrustedReceiptKind::SnapshotFailed, zero_freeze, 0),
        zero_before.last_dispatch_sequence + 1,
        zero_before.last_dispatch_sequence + 2,
        false);
    check_snapshot_failed_accepts(
        max_diagnostic,
        make_snapshot_receipt(
            TrustedReceiptKind::SnapshotFailed,
            max_freeze,
            std::numeric_limits<std::uint32_t>::max()),
        max_before.last_dispatch_sequence + 1,
        max_before.last_dispatch_sequence + 2,
        false);

    const RuntimeOwnerAdapterPrivateSnapshot zero_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(zero_diagnostic);
    const RuntimeOwnerAdapterPrivateSnapshot max_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(max_diagnostic);
    CHECK(runtime_owner_views_equal(zero_after.core, max_after.core));
    CHECK(zero_after.last_dispatch_sequence ==
          max_after.last_dispatch_sequence);
    CHECK(zero_after.pending_effect_count ==
          max_after.pending_effect_count);
    for (std::size_t index = 0;
         index < zero_after.pending_effect_slots.size(); ++index) {
        CHECK(pending_effect_slot_snapshots_equal(
            zero_after.pending_effect_slots[index],
            max_after.pending_effect_slots[index]));
    }
}

void test_snapshot_failed_sequence_regular_and_terminal_reserve()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, 41);
        check_snapshot_failed_accepts(
            adapter,
            make_snapshot_receipt(
                TrustedReceiptKind::SnapshotFailed, freeze),
            42,
            43,
            false);
    }

    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        maximum - 4,
        maximum - 3,
        maximum - 2,
    }};
    for (const std::uint32_t start : terminal_starts) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        check_snapshot_failed_accepts(
            adapter,
            make_snapshot_receipt(
                TrustedReceiptKind::SnapshotFailed, freeze),
            maximum - 1,
            maximum,
            true);
    }
}

void test_snapshot_failed_damaged_sequence_shortage_is_bounded()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::uint32_t, 2> damaged_starts{{
        maximum - 1,
        maximum,
    }};
    for (const std::uint32_t start : damaged_starts) {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_snapshot_receipt(
                      TrustedReceiptKind::SnapshotFailed,
                      freeze,
                      95)) == TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::SnapshotFreezePending,
            RuntimeOwnerPhase::SnapshotFreezePending);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(runtime_owner_views_equal(before.core, after.core));
        CHECK(after.trusted_count == 1);
        CHECK(after.pending_effect_count == 0);
        CHECK(after.last_dispatch_sequence == start);
        CHECK(after.accepted_liveness_mask == 0x0f);
        CHECK(!after.boot_end_released);
        CHECK(last_trusted_receipt_signatures_equal(
            after.last_trusted_receipt_signature,
            before.last_trusted_receipt_signature));
        CHECK(after.critical_pending);
        CHECK(after.critical.first_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(after.critical.last_reason ==
              AdapterCriticalReason::DispatchSequenceSaturation);
        CHECK(after.critical.first_ingress_sequence == 7);
        CHECK(after.critical.last_ingress_sequence == 7);
        CHECK(after.critical.first_diagnostic_code == 0);
        CHECK(after.critical.last_diagnostic_code == 0);
        CHECK(after.critical.occurrence_count == 1);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::SnapshotFreezePending,
            RuntimeOwnerPhase::SnapshotFreezePending);
        CHECK(adapter.view().critical_pending == 0);
        CHECK(adapter.view().safety_delivery_blocked == 1);
    }
}

void test_task4b1_trusted_transition_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_awaiting_config_via_trusted(adapter, 77);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);

    const std::size_t fail_closed_allocations_before = g_allocation_count;
    const std::size_t fail_closed_deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_prepare_core_awaiting_config(
            adapter, 77, 1, 0);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_config_counters(
            adapter,
            std::numeric_limits<std::uint32_t>::max(),
            0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_config_committed_receipt(77, 1, 9)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
    }
    CHECK(g_allocation_count == fail_closed_allocations_before);
    CHECK(g_deallocation_count == fail_closed_deallocations_before);
}

void test_task4b2_a1_transport_attempt_failed_path_is_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_connecting_without_pending(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_attempt_failed_receipt(1, 77)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_transport_attempt_failed_receipt(1, 77)) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_task4b2_a2_transport_disconnected_path_is_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_phase(
            adapter, RuntimeOwnerPhase::RuntimeReady));
        constexpr TrustedReceipt disconnected =
            make_transport_disconnected_receipt(1, 1, 77);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, disconnected) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, disconnected) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_task4b2_b1_operation_completed_path_is_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        for (std::size_t index = 0; index < tickets.size(); ++index) {
            check_operation_completed_accepts(
                adapter, tickets[index], index + 1 == tickets.size());
        }
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_operation_completed_receipt(tickets[3])) ==
              TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_task4b2_b2_liveness_failure_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    constexpr std::array<TrustedReceiptKind, 2> outcomes{{
        TrustedReceiptKind::OperationFailed,
        TrustedReceiptKind::DeadlineExpired,
    }};
    for (const TrustedReceiptKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        const std::array<RuntimeOwnerEffect, 4> tickets =
            fixture_prepare_liveness_waiting_via_config(adapter);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const TrustedReceipt receipt =
            make_liveness_failure_receipt(outcome, tickets[0], 77);
        check_liveness_failure_accepts(
            adapter,
            receipt,
            before.last_dispatch_sequence + 1,
            before.last_dispatch_sequence + 2,
            false);
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_task4b2_b3_snapshot_receipt_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        const TrustedReceipt receipt = make_snapshot_receipt(
            TrustedReceiptKind::SnapshotSucceeded, freeze);
        check_snapshot_succeeded_accepts(adapter, receipt);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::DispatchPrepared);
        const AdapterDispatch end_boot = adapter.peek_dispatch();
        CHECK(end_boot.effect.kind ==
              RuntimeOwnerEffectKind::EndBootOrchestration);
        CHECK(adapter.acknowledge_dispatch(
                  end_boot.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
    }
    {
        RuntimeOwnerAdapterCore adapter{};
        const RuntimeOwnerEffect freeze =
            fixture_prepare_snapshot_freeze_pending_via_config(adapter);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const TrustedReceipt receipt = make_snapshot_receipt(
            TrustedReceiptKind::SnapshotFailed, freeze, 77);
        check_snapshot_failed_accepts(
            adapter,
            receipt,
            before.last_dispatch_sequence + 1,
            before.last_dispatch_sequence + 2,
            false);
        RuntimeOwnerAdapterCoreTestPeer::fixture_clear_pending_effects(
            adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

constexpr std::array<TrustedReceiptKind, 9> kConcreteTrustedReceiptKinds{{
    TrustedReceiptKind::TransportEstablished,
    TrustedReceiptKind::TransportAttemptFailed,
    TrustedReceiptKind::ConfigCommitted,
    TrustedReceiptKind::OperationCompleted,
    TrustedReceiptKind::OperationFailed,
    TrustedReceiptKind::DeadlineExpired,
    TrustedReceiptKind::SnapshotSucceeded,
    TrustedReceiptKind::SnapshotFailed,
    TrustedReceiptKind::TransportDisconnected,
}};

constexpr std::array<RuntimeOwnerEffectKind, 11> kAllTrustedEffectCandidates{{
    RuntimeOwnerEffectKind::None,
    RuntimeOwnerEffectKind::StartTransportAttempt,
    RuntimeOwnerEffectKind::StartAtProbe,
    RuntimeOwnerEffectKind::StartProbePublish,
    RuntimeOwnerEffectKind::VerifySubscription,
    RuntimeOwnerEffectKind::PullFollowupConfig,
    RuntimeOwnerEffectKind::FreezeBootSnapshot,
    RuntimeOwnerEffectKind::EndBootOrchestration,
    RuntimeOwnerEffectKind::RecordFault,
    RuntimeOwnerEffectKind::EnterRecovery,
    static_cast<RuntimeOwnerEffectKind>(255),
}};

constexpr bool is_liveness_start_effect(
    const RuntimeOwnerEffectKind effect) noexcept
{
    return effect == RuntimeOwnerEffectKind::StartAtProbe ||
           effect == RuntimeOwnerEffectKind::StartProbePublish ||
           effect == RuntimeOwnerEffectKind::VerifySubscription ||
           effect == RuntimeOwnerEffectKind::PullFollowupConfig;
}

constexpr bool trusted_effect_allowed(
    const TrustedReceiptKind kind,
    const RuntimeOwnerEffectKind effect) noexcept
{
    switch (kind) {
    case TrustedReceiptKind::TransportEstablished:
    case TrustedReceiptKind::TransportAttemptFailed:
        return effect == RuntimeOwnerEffectKind::StartTransportAttempt;
    case TrustedReceiptKind::ConfigCommitted:
    case TrustedReceiptKind::TransportDisconnected:
        return effect == RuntimeOwnerEffectKind::None;
    case TrustedReceiptKind::OperationCompleted:
    case TrustedReceiptKind::OperationFailed:
    case TrustedReceiptKind::DeadlineExpired:
        return is_liveness_start_effect(effect);
    case TrustedReceiptKind::SnapshotSucceeded:
    case TrustedReceiptKind::SnapshotFailed:
        return effect == RuntimeOwnerEffectKind::FreezeBootSnapshot;
    case TrustedReceiptKind::Invalid:
    default:
        return false;
    }
}

constexpr bool trusted_kind_has_optional_diagnostic(
    const TrustedReceiptKind kind) noexcept
{
    return kind == TrustedReceiptKind::TransportAttemptFailed ||
           kind == TrustedReceiptKind::OperationFailed ||
           kind == TrustedReceiptKind::DeadlineExpired ||
           kind == TrustedReceiptKind::SnapshotFailed ||
           kind == TrustedReceiptKind::TransportDisconnected;
}

void check_single_trusted_acceptance(const TrustedReceipt input)
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, input) == TrustedEnqueueResult::Accepted);

    const RuntimeOwnerAdapterPrivateSnapshot accepted =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(runtime_owner_views_equal(before.core, accepted.core));
    CHECK(accepted.last_trusted_ingress_sequence == 1);
    CHECK(accepted.trusted_head == 0);
    CHECK(accepted.trusted_tail == 1);
    CHECK(accepted.trusted_count == 1);
    CHECK(accepted.trusted_high_water == 1);
    CHECK(accepted.trusted_rejected_full_count == 0);
    CHECK(accepted.trusted_protocol_violation_count == 0);
    CHECK(!accepted.critical_pending);
    CHECK(critical_ledgers_equal(accepted.critical, {}));
    if (trusted_kind_has_optional_diagnostic(input.kind)) {
        CHECK(accepted.last_trusted_diagnostic_ingress_sequence == 1);
        CHECK(accepted.last_trusted_diagnostic_code == input.diagnostic_code);
    } else {
        CHECK(accepted.last_trusted_diagnostic_ingress_sequence == 0);
        CHECK(accepted.last_trusted_diagnostic_code == 0);
    }

    const RuntimeOwnerAdapterView view = adapter.view();
    CHECK(view.last_trusted_ingress_sequence == 1);
    CHECK(view.last_trusted_diagnostic_ingress_sequence ==
          accepted.last_trusted_diagnostic_ingress_sequence);
    CHECK(view.last_trusted_diagnostic_code ==
          accepted.last_trusted_diagnostic_code);
    CHECK(view.trusted_depth == 1);
    CHECK(view.trusted_high_water == 1);
    CHECK(view.trusted_rejected_full_count == 0);
    CHECK(view.trusted_protocol_violation_count == 0);
    CHECK(critical_ledgers_equal(view.critical, {}));

    TrustedIngressEnvelope envelope{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_consume_trusted(
        adapter, envelope));
    CHECK(envelope.kind == TrustedIngressPayloadKind::CoreReceipt);
    CHECK(envelope.reserved == std::array<std::uint8_t, 3>{});
    CHECK(envelope.ingress_sequence == 1);
    CHECK(trusted_receipts_equal(envelope.receipt, input));
    CHECK(normal_completions_equal(envelope.normal_completion, {}));
    CHECK(adapter.view().trusted_depth == 0);
    CHECK(adapter.view().trusted_high_water == 1);
}

void check_single_trusted_invalid_rejection(const TrustedReceipt input)
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, input) == TrustedEnqueueResult::RejectedInvalid);

    RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.last_trusted_ingress_sequence == 0);
    CHECK(after.last_trusted_diagnostic_ingress_sequence == 0);
    CHECK(after.last_trusted_diagnostic_code == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_high_water == 0);
    CHECK(after.trusted_rejected_full_count == 0);
    CHECK(after.trusted_protocol_violation_count == 1);
    CHECK(after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(after.critical.reason_mask == (1u << 1u));
    CHECK(after.critical.first_ingress_sequence == 0);
    CHECK(after.critical.last_ingress_sequence == 0);
    CHECK(after.critical.first_diagnostic_code == input.diagnostic_code);
    CHECK(after.critical.last_diagnostic_code == input.diagnostic_code);
    CHECK(after.critical.occurrence_count == 1);

    after.trusted_protocol_violation_count =
        before.trusted_protocol_violation_count;
    after.critical_pending = before.critical_pending;
    after.critical = before.critical;
    CHECK(private_snapshots_equal(before, after));
}

enum class TrustedAdmissionGateFixture : std::uint8_t {
    ShutdownPending,
    CoreShutdownCommitted,
    ShutdownTerminalOverride,
    CoreFailClosed,
    CoreAdapterFatal,
    SequenceFatal,
    DispatchFatal,
    SafetyDeliveryBlocked,
};

void apply_trusted_admission_gate_fixture(
    RuntimeOwnerAdapterCore &adapter,
    const TrustedAdmissionGateFixture fixture)
{
    switch (fixture) {
    case TrustedAdmissionGateFixture::ShutdownPending: {
        auto shutdown = adapter.shutdown_port();
        CHECK(shutdown.request() == UrgentRequestResult::Accepted);
        break;
    }
    case TrustedAdmissionGateFixture::CoreShutdownCommitted:
        RuntimeOwnerAdapterCoreTestPeer::fixture_commit_core_shutdown(adapter);
        break;
    case TrustedAdmissionGateFixture::ShutdownTerminalOverride:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_shutdown_terminal_override(
            adapter, true);
        break;
    case TrustedAdmissionGateFixture::CoreFailClosed:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_fail_closed(
            adapter, true);
        break;
    case TrustedAdmissionGateFixture::CoreAdapterFatal:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
            adapter, true);
        break;
    case TrustedAdmissionGateFixture::SequenceFatal:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_sequence_fatal(
            adapter, true);
        break;
    case TrustedAdmissionGateFixture::DispatchFatal:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_dispatch_fatal(
            adapter, true);
        break;
    case TrustedAdmissionGateFixture::SafetyDeliveryBlocked:
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_safety_delivery_blocked(
            adapter, true);
        break;
    }
}

void test_trusted_admission_gate_precedes_shape_and_is_mutation_free()
{
    constexpr std::array<TrustedAdmissionGateFixture, 8> fixtures{{
        TrustedAdmissionGateFixture::ShutdownPending,
        TrustedAdmissionGateFixture::CoreShutdownCommitted,
        TrustedAdmissionGateFixture::ShutdownTerminalOverride,
        TrustedAdmissionGateFixture::CoreFailClosed,
        TrustedAdmissionGateFixture::CoreAdapterFatal,
        TrustedAdmissionGateFixture::SequenceFatal,
        TrustedAdmissionGateFixture::DispatchFatal,
        TrustedAdmissionGateFixture::SafetyDeliveryBlocked,
    }};

    for (const TrustedAdmissionGateFixture fixture : fixtures) {
        RuntimeOwnerAdapterCore adapter{};
        apply_trusted_admission_gate_fixture(adapter, fixture);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, TrustedReceipt{}) ==
              TrustedEnqueueResult::RejectedNotAllowed);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    RuntimeOwnerAdapterCore recovery{};
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_drive_core_to_recovery_pending(recovery));
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(recovery);
    const TrustedReceipt late = make_canonical_trusted_receipt(
        TrustedReceiptKind::TransportAttemptFailed,
        RuntimeOwnerEffectKind::StartAtProbe,
        0);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              recovery, late) == TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(recovery);
    CHECK(runtime_owner_views_equal(before.core, after.core));
    CHECK(after.core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(after.trusted_count == 1);
    CHECK(after.last_trusted_ingress_sequence == 1);
}

void test_trusted_receipt_full_canonical_matrix()
{
    for (const TrustedReceiptKind kind : kConcreteTrustedReceiptKinds) {
        for (const RuntimeOwnerEffectKind effect :
             kAllTrustedEffectCandidates) {
            TrustedReceipt input = make_canonical_trusted_receipt(kind);
            input.effect_kind = effect;
            if (trusted_effect_allowed(kind, effect)) {
                check_single_trusted_acceptance(input);
            } else {
                check_single_trusted_invalid_rejection(input);
            }
        }

        const TrustedReceipt canonical =
            make_canonical_trusted_receipt(kind);
        TrustedReceipt mutated = canonical;
        mutated.reserved = 1;
        check_single_trusted_invalid_rejection(mutated);

        mutated = canonical;
        mutated.correlation_id = canonical.correlation_id == 0 ? 1 : 0;
        check_single_trusted_invalid_rejection(mutated);

        mutated = canonical;
        mutated.mqtt_session_id = canonical.mqtt_session_id == 0 ? 1 : 0;
        check_single_trusted_invalid_rejection(mutated);

        mutated = canonical;
        mutated.mqtt_generation = 0;
        check_single_trusted_invalid_rejection(mutated);

        mutated = canonical;
        mutated.config_commit_sequence =
            canonical.config_commit_sequence == 0 ? 1 : 0;
        check_single_trusted_invalid_rejection(mutated);

        mutated = canonical;
        mutated.config_apply_epoch =
            canonical.config_apply_epoch == 0 ? 1 : 0;
        check_single_trusted_invalid_rejection(mutated);

        mutated = canonical;
        mutated.diagnostic_code = 91;
        if (trusted_kind_has_optional_diagnostic(kind)) {
            check_single_trusted_acceptance(mutated);
        } else {
            check_single_trusted_invalid_rejection(mutated);
        }
    }

    check_single_trusted_invalid_rejection(TrustedReceipt{});
    TrustedReceipt unknown = make_canonical_trusted_receipt(
        TrustedReceiptKind::TransportAttemptFailed,
        RuntimeOwnerEffectKind::StartAtProbe,
        99);
    unknown.kind = static_cast<TrustedReceiptKind>(255);
    unknown.effect_kind = static_cast<RuntimeOwnerEffectKind>(255);
    check_single_trusted_invalid_rejection(unknown);
}

void test_trusted_ring_fifo_two_wraps_high_water_and_no_coalescing()
{
    RuntimeOwnerAdapterCore adapter{};
    std::uint32_t next_sequence = 1;
    for (std::uint32_t cycle = 0; cycle < 2; ++cycle) {
        for (std::uint32_t offset = 0; offset < 8; ++offset) {
            TrustedReceipt input = make_canonical_trusted_receipt(
                TrustedReceiptKind::TransportEstablished);
            input.mqtt_session_id = 100 + next_sequence;
            input.mqtt_generation = 200 + next_sequence;
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, input) == TrustedEnqueueResult::Accepted);
            CHECK(adapter.view().last_trusted_ingress_sequence ==
                  next_sequence);
            CHECK(adapter.view().trusted_depth == offset + 1);
            CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).
                      trusted_tail ==
                  static_cast<std::uint8_t>((offset + 1) % 8));
            ++next_sequence;
        }

        const RuntimeOwnerAdapterPrivateSnapshot full =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(full.trusted_head == 0);
        CHECK(full.trusted_tail == 0);
        CHECK(full.trusted_count == 8);
        CHECK(full.trusted_high_water == 8);

        for (std::uint32_t offset = 0; offset < 8; ++offset) {
            const std::uint32_t expected_sequence = cycle * 8 + offset + 1;
            TrustedIngressEnvelope envelope{};
            CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_consume_trusted(
                adapter, envelope));
            CHECK(envelope.kind == TrustedIngressPayloadKind::CoreReceipt);
            CHECK(envelope.ingress_sequence == expected_sequence);
            CHECK(envelope.receipt.mqtt_session_id ==
                  100 + expected_sequence);
            CHECK(envelope.receipt.mqtt_generation ==
                  200 + expected_sequence);
            CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).
                      trusted_head ==
                  static_cast<std::uint8_t>((offset + 1) % 8));
        }

        const RuntimeOwnerAdapterPrivateSnapshot empty =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(empty.trusted_head == 0);
        CHECK(empty.trusted_tail == 0);
        CHECK(empty.trusted_count == 0);
        CHECK(empty.trusted_high_water == 8);
    }

    const RuntimeOwnerAdapterPrivateSnapshot before_empty_consume =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    TrustedIngressEnvelope untouched{};
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::fixture_consume_trusted(
        adapter, untouched));
    CHECK(private_snapshots_equal(
        before_empty_consume,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(adapter.view().last_trusted_ingress_sequence == 16);
    CHECK(adapter.view().trusted_depth == 0);
    CHECK(adapter.view().trusted_high_water == 8);
}

void test_trusted_admission_order_validation_then_sequence_then_capacity()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCore adapter{};
    const TrustedReceipt accepted = make_canonical_trusted_receipt(
        TrustedReceiptKind::TransportEstablished);
    for (std::uint32_t index = 0; index < 8; ++index) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, accepted) == TrustedEnqueueResult::Accepted);
    }
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_trusted_ingress_sequence(
        adapter, maximum);

    TrustedReceipt malformed = make_canonical_trusted_receipt(
        TrustedReceiptKind::OperationFailed,
        RuntimeOwnerEffectKind::StartAtProbe,
        41);
    malformed.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, malformed) == TrustedEnqueueResult::RejectedInvalid);
    RuntimeOwnerAdapterView view = adapter.view();
    CHECK(view.last_trusted_ingress_sequence == maximum);
    CHECK(view.trusted_depth == 8);
    CHECK(view.trusted_protocol_violation_count == 1);
    CHECK(view.trusted_rejected_full_count == 0);
    CHECK(view.sequence_fatal_latched == 0);
    CHECK(view.critical.last_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(view.critical.last_diagnostic_code == 41);

    const TrustedReceipt canonical = make_canonical_trusted_receipt(
        TrustedReceiptKind::OperationFailed,
        RuntimeOwnerEffectKind::StartAtProbe,
        42);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, canonical) ==
          TrustedEnqueueResult::RejectedSequenceSaturated);
    view = adapter.view();
    CHECK(view.last_trusted_ingress_sequence == maximum);
    CHECK(view.trusted_depth == 8);
    CHECK(view.trusted_protocol_violation_count == 1);
    CHECK(view.trusted_rejected_full_count == 0);
    CHECK(view.sequence_fatal_latched == 1);
    CHECK(view.critical.first_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(view.critical.first_diagnostic_code == 41);
    CHECK(view.critical.last_reason ==
          AdapterCriticalReason::TrustedSequenceSaturation);
    CHECK(view.critical.last_diagnostic_code == 42);
    CHECK(view.critical.reason_mask == ((1u << 1u) | (1u << 3u)));
    CHECK(view.critical.occurrence_count == 2);
}

void test_trusted_full_rejects_new_without_drop_evict_or_sequence_consumption()
{
    RuntimeOwnerAdapterCore adapter{};
    const TrustedReceipt repeated = make_canonical_trusted_receipt(
        TrustedReceiptKind::TransportEstablished);
    for (std::uint32_t index = 0; index < 8; ++index) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, repeated) == TrustedEnqueueResult::Accepted);
    }

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const TrustedReceipt rejected = make_canonical_trusted_receipt(
        TrustedReceiptKind::OperationFailed,
        RuntimeOwnerEffectKind::StartProbePublish,
        77);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, rejected) == TrustedEnqueueResult::RejectedFull);
    const RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);

    CHECK(after.last_trusted_ingress_sequence == 8);
    CHECK(after.trusted_head == before.trusted_head);
    CHECK(after.trusted_tail == before.trusted_tail);
    CHECK(after.trusted_count == 8);
    CHECK(after.trusted_high_water == 8);
    for (std::size_t index = 0; index < after.trusted_slots.size(); ++index) {
        CHECK(trusted_slot_snapshots_equal(
            after.trusted_slots[index], before.trusted_slots[index]));
    }
    CHECK(after.trusted_rejected_full_count == 1);
    CHECK(after.trusted_protocol_violation_count == 0);
    CHECK(after.last_trusted_diagnostic_ingress_sequence == 0);
    CHECK(after.last_trusted_diagnostic_code == 0);
    CHECK(after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::TrustedQueueOverflow);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::TrustedQueueOverflow);
    CHECK(after.critical.reason_mask == 1u);
    CHECK(after.critical.first_ingress_sequence == 0);
    CHECK(after.critical.last_ingress_sequence == 0);
    CHECK(after.critical.first_diagnostic_code == 77);
    CHECK(after.critical.last_diagnostic_code == 77);
    CHECK(after.critical.occurrence_count == 1);

    const RuntimeOwnerAdapterView view = adapter.view();
    CHECK(view.last_trusted_ingress_sequence == 8);
    CHECK(view.trusted_depth == 8);
    CHECK(view.trusted_high_water == 8);
    CHECK(view.trusted_rejected_full_count == 1);
    CHECK(view.critical_pending == 1);
    CHECK(critical_ledgers_equal(view.critical, after.critical));

    auto shutdown = adapter.shutdown_port();
    CHECK(shutdown.request() == UrgentRequestResult::Accepted);
    CHECK(adapter.view().trusted_depth == 8);
}

void test_trusted_sequence_last_success_saturation_before_full_and_sticky_gate()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_trusted_ingress_sequence(adapter, maximum - 1);
        const TrustedReceipt last_success = make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportAttemptFailed,
            RuntimeOwnerEffectKind::StartAtProbe,
            0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, last_success) == TrustedEnqueueResult::Accepted);
        CHECK(adapter.view().last_trusted_ingress_sequence == maximum);
        CHECK(adapter.view().last_trusted_diagnostic_ingress_sequence ==
              maximum);
        CHECK(adapter.view().last_trusted_diagnostic_code == 0);

        const RuntimeOwnerAdapterPrivateSnapshot before_saturation =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const TrustedReceipt causing = make_canonical_trusted_receipt(
            TrustedReceiptKind::SnapshotFailed,
            RuntimeOwnerEffectKind::StartAtProbe,
            88);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, causing) ==
              TrustedEnqueueResult::RejectedSequenceSaturated);
        const RuntimeOwnerAdapterPrivateSnapshot saturated =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(saturated.last_trusted_ingress_sequence == maximum);
        CHECK(saturated.trusted_count == before_saturation.trusted_count);
        CHECK(saturated.trusted_head == before_saturation.trusted_head);
        CHECK(saturated.trusted_tail == before_saturation.trusted_tail);
        CHECK(saturated.trusted_rejected_full_count == 0);
        CHECK(saturated.trusted_protocol_violation_count == 0);
        CHECK(saturated.last_trusted_diagnostic_ingress_sequence == maximum);
        CHECK(saturated.last_trusted_diagnostic_code == 0);
        CHECK(saturated.sequence_fatal_latched);
        CHECK(saturated.critical_pending);
        CHECK(saturated.critical.first_reason ==
              AdapterCriticalReason::TrustedSequenceSaturation);
        CHECK(saturated.critical.last_reason ==
              AdapterCriticalReason::TrustedSequenceSaturation);
        CHECK(saturated.critical.reason_mask == (1u << 3u));
        CHECK(saturated.critical.first_ingress_sequence == 0);
        CHECK(saturated.critical.last_ingress_sequence == 0);
        CHECK(saturated.critical.first_diagnostic_code == 88);
        CHECK(saturated.critical.last_diagnostic_code == 88);
        CHECK(saturated.critical.occurrence_count == 1);

        const RuntimeOwnerAdapterPrivateSnapshot fatal_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, TrustedReceipt{}) ==
              TrustedEnqueueResult::RejectedNotAllowed);
        CHECK(private_snapshots_equal(
            fatal_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        const TrustedReceipt accepted = make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
        for (std::uint32_t index = 0; index < 8; ++index) {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, accepted) == TrustedEnqueueResult::Accepted);
        }
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_trusted_ingress_sequence(adapter, maximum);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const TrustedReceipt causing = make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportDisconnected,
            RuntimeOwnerEffectKind::StartAtProbe,
            89);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, causing) ==
              TrustedEnqueueResult::RejectedSequenceSaturated);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.trusted_rejected_full_count ==
              before.trusted_rejected_full_count);
        CHECK(after.trusted_count == before.trusted_count);
        for (std::size_t index = 0; index < after.trusted_slots.size(); ++index) {
            CHECK(trusted_slot_snapshots_equal(
                after.trusted_slots[index], before.trusted_slots[index]));
        }
        CHECK(after.critical.last_reason ==
              AdapterCriticalReason::TrustedSequenceSaturation);
        CHECK(after.critical.last_diagnostic_code == 89);
    }
}

void test_trusted_diagnostic_projection_includes_zero_code_events_only()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<TrustedReceipt, 8> inputs{{
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportAttemptFailed,
            RuntimeOwnerEffectKind::StartAtProbe,
            0),
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished),
        make_canonical_trusted_receipt(
            TrustedReceiptKind::OperationFailed,
            RuntimeOwnerEffectKind::VerifySubscription,
            17),
        make_canonical_trusted_receipt(
            TrustedReceiptKind::OperationCompleted,
            RuntimeOwnerEffectKind::VerifySubscription),
        make_canonical_trusted_receipt(
            TrustedReceiptKind::DeadlineExpired,
            RuntimeOwnerEffectKind::PullFollowupConfig,
            0),
        make_canonical_trusted_receipt(
            TrustedReceiptKind::SnapshotFailed,
            RuntimeOwnerEffectKind::StartAtProbe,
            18),
        make_canonical_trusted_receipt(
            TrustedReceiptKind::SnapshotSucceeded),
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportDisconnected,
            RuntimeOwnerEffectKind::StartAtProbe,
            0),
    }};
    const std::array<std::uint32_t, 8> expected_sequences{{
        1, 1, 3, 3, 5, 6, 6, 8,
    }};
    const std::array<std::uint32_t, 8> expected_codes{{
        0, 0, 17, 17, 0, 18, 18, 0,
    }};

    for (std::size_t index = 0; index < inputs.size(); ++index) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, inputs[index]) == TrustedEnqueueResult::Accepted);
        const RuntimeOwnerAdapterView view = adapter.view();
        CHECK(view.last_trusted_diagnostic_ingress_sequence ==
              expected_sequences[index]);
        CHECK(view.last_trusted_diagnostic_code == expected_codes[index]);
        TrustedIngressEnvelope consumed{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_consume_trusted(
            adapter, consumed));
    }
}

void test_trusted_counters_and_critical_ledger_saturate_and_preserve_first()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_trusted_diagnostic_counts(
        adapter, maximum, maximum);

    TrustedReceipt invalid = make_canonical_trusted_receipt(
        TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    invalid.diagnostic_code = 11;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.view().trusted_protocol_violation_count == maximum);
    CHECK(adapter.view().critical.first_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(adapter.view().critical.first_diagnostic_code == 11);

    const TrustedReceipt queued = make_canonical_trusted_receipt(
        TrustedReceiptKind::TransportEstablished);
    for (std::uint32_t index = 0; index < 8; ++index) {
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, queued) == TrustedEnqueueResult::Accepted);
    }
    const TrustedReceipt overflow = make_canonical_trusted_receipt(
        TrustedReceiptKind::OperationFailed,
        RuntimeOwnerEffectKind::StartAtProbe,
        22);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, overflow) == TrustedEnqueueResult::RejectedFull);
    RuntimeOwnerAdapterView view = adapter.view();
    CHECK(view.trusted_rejected_full_count == maximum);
    CHECK(view.critical.first_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(view.critical.first_diagnostic_code == 11);
    CHECK(view.critical.last_reason ==
          AdapterCriticalReason::TrustedQueueOverflow);
    CHECK(view.critical.last_diagnostic_code == 22);
    CHECK(view.critical.reason_mask == 3u);
    CHECK(view.critical.occurrence_count == 2);

    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_drive_core_to_runtime_ready(
        adapter));
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_boot_end_released(
        adapter, true);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_normal_enqueue_sequence(
        adapter, maximum);
    auto normal = adapter.normal_port();
    CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
          NormalSubmitResult::RejectedSequenceSaturated);
    view = adapter.view();
    CHECK(view.critical.first_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(view.critical.first_diagnostic_code == 11);
    CHECK(view.critical.last_reason ==
          AdapterCriticalReason::NormalSequenceSaturation);
    CHECK(view.critical.last_diagnostic_code == 0);
    CHECK(view.critical.reason_mask == 7u);
    CHECK(view.critical.occurrence_count == 3);

    RuntimeOwnerAdapterCore saturated_ledger{};
    TrustedReceipt first = make_canonical_trusted_receipt(
        TrustedReceiptKind::TransportEstablished);
    first.reserved = 1;
    first.diagnostic_code = 31;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              saturated_ledger, first) ==
          TrustedEnqueueResult::RejectedInvalid);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_critical_occurrence_count(
        saturated_ledger, maximum);
    TrustedReceipt latest = first;
    latest.diagnostic_code = 32;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              saturated_ledger, latest) ==
          TrustedEnqueueResult::RejectedInvalid);
    view = saturated_ledger.view();
    CHECK(view.critical.occurrence_count == maximum);
    CHECK(view.critical.first_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(view.critical.first_diagnostic_code == 31);
    CHECK(view.critical.last_reason ==
          AdapterCriticalReason::TrustedProtocolViolation);
    CHECK(view.critical.last_diagnostic_code == 32);
}

void test_shared_normal_intent_canonical_contract()
{
    struct Case {
        NormalIntent intent;
        bool expected;
    };
    constexpr std::array<Case, 13> cases{{
        {{NormalIntentKind::PublishTelemetry, 0, 0, 1, 1}, true},
        {{NormalIntentKind::RefreshRssi, 0, 0, 0, 0}, true},
        {{NormalIntentKind::PullConfig, 0, 0, 0, 0}, true},
        {{NormalIntentKind::PullCommand, 0, 0, 0, 0}, true},
        {{NormalIntentKind::Invalid, 0, 0, 0, 0}, false},
        {{static_cast<NormalIntentKind>(0xff), 0, 0, 0, 0}, false},
        {{NormalIntentKind::PublishTelemetry, 0, 0, 0, 1}, false},
        {{NormalIntentKind::PublishTelemetry, 0, 0, 1, 0}, false},
        {{NormalIntentKind::RefreshRssi, 0, 0, 1, 0}, false},
        {{NormalIntentKind::PullConfig, 0, 0, 0, 1}, false},
        {{NormalIntentKind::PullCommand, 0, 0, 1, 0}, false},
        {{NormalIntentKind::PublishTelemetry, 1, 0, 1, 1}, false},
        {{NormalIntentKind::PublishTelemetry, 0, 1, 1, 1}, false},
    }};

    static_assert(runtime_owner_normal_intent_is_canonical(cases[0].intent));
    static_assert(!runtime_owner_normal_intent_is_canonical(cases[4].intent));
    for (const Case &entry : cases) {
        CHECK(runtime_owner_normal_intent_is_canonical(entry.intent) ==
              entry.expected);
    }
}

void test_normal_admission_precedence_and_canonical_validation()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        auto normal = adapter.normal_port();
        CHECK(normal.submit(NormalIntent{}) ==
              NormalSubmitResult::RejectedNotReady);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        auto normal = adapter.normal_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
              NormalSubmitResult::RejectedNotReady);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, false);
        auto normal = adapter.normal_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
              NormalSubmitResult::RejectedNotReady);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    const std::array<NormalIntent, 8> invalid_inputs{{
        {},
        {static_cast<NormalIntentKind>(255), 0, 0, 0, 0},
        {NormalIntentKind::PublishTelemetry, 0, 0, 0, 1},
        {NormalIntentKind::PublishTelemetry, 0, 0, 1, 0},
        {NormalIntentKind::PublishTelemetry, 1, 0, 1, 1},
        {NormalIntentKind::PublishTelemetry, 0, 1, 1, 1},
        {NormalIntentKind::RefreshRssi, 0, 0, 1, 0},
        {NormalIntentKind::PullConfig, 0, 0, 0, 1},
    }};
    for (const NormalIntent input : invalid_inputs) {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        auto normal = adapter.normal_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(normal.submit(input) == NormalSubmitResult::RejectedInvalid);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_commit_core_shutdown(adapter);
        auto normal = adapter.normal_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(before.core.phase == RuntimeOwnerPhase::ShutdownCommitted);
        CHECK(normal.submit(NormalIntent{}) ==
              NormalSubmitResult::RejectedNotReady);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
            adapter, true);
        auto normal = adapter.normal_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(normal.submit(NormalIntent{}) ==
              NormalSubmitResult::RejectedNotReady);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_shutdown_terminal_override(adapter, true);
        auto normal = adapter.normal_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(normal.submit(NormalIntent{}) ==
              NormalSubmitResult::RejectedNotReady);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }
}

void test_shutdown_pending_blocks_normal_before_shape_validation()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_runtime_ready(adapter, true);
    auto shutdown = adapter.shutdown_port();
    auto normal = adapter.normal_port();
    CHECK(shutdown.request() == UrgentRequestResult::Accepted);

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(normal.submit(NormalIntent{}) ==
          NormalSubmitResult::RejectedNotReady);
    CHECK(private_snapshots_equal(
        before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_safety_delivery_block_blocks_normal_before_shape_validation()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_runtime_ready(adapter, true);
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_safety_delivery_blocked(
        adapter, true);
    auto normal = adapter.normal_port();

    CHECK(adapter.view().safety_delivery_blocked == 1);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(normal.submit(NormalIntent{}) ==
          NormalSubmitResult::RejectedNotReady);
    CHECK(private_snapshots_equal(
        before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_normal_ring_fifo_high_water_and_index_wrap()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_runtime_ready(adapter, true);
    auto normal = adapter.normal_port();

    for (std::uint32_t subject = 1; subject <= 8; ++subject) {
        CHECK(normal.submit(make_telemetry_intent(subject, 100 + subject)) ==
              NormalSubmitResult::Accepted);
        const RuntimeOwnerAdapterView view = adapter.view();
        CHECK(view.normal_depth == subject);
        CHECK(view.normal_high_water == subject);
        CHECK(view.last_normal_enqueue_sequence == subject);
    }

    const RuntimeOwnerAdapterPrivateSnapshot full_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(normal.submit(make_telemetry_intent(9, 109)) ==
          NormalSubmitResult::RejectedFull);
    RuntimeOwnerAdapterPrivateSnapshot full_after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(full_after.normal_rejected_full_count ==
          full_before.normal_rejected_full_count + 1);
    full_after.normal_rejected_full_count =
        full_before.normal_rejected_full_count;
    CHECK(private_snapshots_equal(full_before, full_after));

    for (std::uint32_t expected = 1; expected <= 3; ++expected) {
        NormalIntent intent{};
        std::uint32_t sequence = 0;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_consume_normal(
            adapter, intent, sequence));
        CHECK(normal_intents_equal(
            intent, make_telemetry_intent(expected, 100 + expected)));
        CHECK(sequence == expected);
    }

    RuntimeOwnerAdapterPrivateSnapshot state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.normal_head == 3);
    CHECK(state.normal_tail == 0);
    CHECK(state.normal_count == 5);
    CHECK(state.normal_high_water == 8);

    for (std::uint32_t subject = 9; subject <= 11; ++subject) {
        CHECK(normal.submit(make_telemetry_intent(subject, 100 + subject)) ==
              NormalSubmitResult::Accepted);
    }
    state = RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.normal_head == 3);
    CHECK(state.normal_tail == 3);
    CHECK(state.normal_count == 8);
    CHECK(state.normal_high_water == 8);

    for (std::uint32_t expected = 4; expected <= 11; ++expected) {
        NormalIntent intent{};
        std::uint32_t sequence = 0;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_consume_normal(
            adapter, intent, sequence));
        CHECK(normal_intents_equal(
            intent, make_telemetry_intent(expected, 100 + expected)));
        CHECK(sequence == expected);
    }
    state = RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.normal_head == 3);
    CHECK(state.normal_tail == 3);
    CHECK(state.normal_count == 0);
    CHECK(state.normal_high_water == 8);
    CHECK(adapter.view().normal_depth == 0);
    CHECK(adapter.view().normal_high_water == 8);
}

void test_normal_queued_same_key_coalescing_and_stale_slot_exclusion()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_runtime_ready(adapter, true);
    auto normal = adapter.normal_port();

    CHECK(normal.submit(make_telemetry_intent(11, 1)) ==
          NormalSubmitResult::Accepted);
    CHECK(normal.submit(make_telemetry_intent(22, 2)) ==
          NormalSubmitResult::Accepted);
    CHECK(normal.submit(make_telemetry_intent(11, 3)) ==
          NormalSubmitResult::AcceptedCoalesced);

    RuntimeOwnerAdapterPrivateSnapshot state =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.normal_count == 2);
    CHECK(state.normal_high_water == 2);
    CHECK(state.last_normal_enqueue_sequence == 3);
    CHECK(state.normal_coalesced_count == 1);
    const std::size_t first_index = state.normal_head;
    const std::size_t second_index =
        static_cast<std::size_t>((state.normal_head + 1u) % 8u);
    CHECK(normal_intents_equal(
        state.normal_slots[first_index].intent,
        make_telemetry_intent(11, 3)));
    CHECK(state.normal_slots[first_index].enqueue_sequence == 3);
    CHECK(normal_intents_equal(
        state.normal_slots[second_index].intent,
        make_telemetry_intent(22, 2)));
    CHECK(state.normal_slots[second_index].enqueue_sequence == 2);

    CHECK(normal.submit(make_kind_only_intent(NormalIntentKind::RefreshRssi)) ==
          NormalSubmitResult::Accepted);
    CHECK(normal.submit(make_kind_only_intent(NormalIntentKind::RefreshRssi)) ==
          NormalSubmitResult::AcceptedCoalesced);
    state = RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(state.normal_count == 3);
    CHECK(state.last_normal_enqueue_sequence == 5);
    CHECK(state.normal_coalesced_count == 2);

    NormalIntent consumed{};
    std::uint32_t consumed_sequence = 0;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_consume_normal(
        adapter, consumed, consumed_sequence));
    CHECK(normal_intents_equal(consumed, make_telemetry_intent(11, 3)));
    CHECK(consumed_sequence == 3);
    CHECK(normal.submit(make_telemetry_intent(11, 4)) ==
          NormalSubmitResult::Accepted);
    CHECK(adapter.view().normal_depth == 3);
    CHECK(adapter.view().last_normal_enqueue_sequence == 6);

    for (std::uint32_t subject = 30; subject <= 34; ++subject) {
        CHECK(normal.submit(make_telemetry_intent(subject, subject)) ==
              NormalSubmitResult::Accepted);
    }
    CHECK(adapter.view().normal_depth == 8);
    CHECK(normal.submit(make_kind_only_intent(NormalIntentKind::RefreshRssi)) ==
          NormalSubmitResult::AcceptedCoalesced);
    CHECK(adapter.view().normal_depth == 8);
    CHECK(adapter.view().last_normal_enqueue_sequence == 12);
    CHECK(adapter.view().normal_coalesced_count == 3);

    const RuntimeOwnerAdapterPrivateSnapshot before_reject =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(normal.submit(make_telemetry_intent(99, 99)) ==
          NormalSubmitResult::RejectedFull);
    RuntimeOwnerAdapterPrivateSnapshot after_reject =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after_reject.normal_rejected_full_count ==
          before_reject.normal_rejected_full_count + 1);
    after_reject.normal_rejected_full_count =
        before_reject.normal_rejected_full_count;
    CHECK(private_snapshots_equal(before_reject, after_reject));
}

void test_normal_sequence_saturation_precedes_full_and_latches_fatal_critical()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_runtime_ready(adapter, true);
    auto normal = adapter.normal_port();
    for (std::uint32_t subject = 1; subject <= 8; ++subject) {
        CHECK(normal.submit(make_telemetry_intent(subject, subject)) ==
              NormalSubmitResult::Accepted);
    }
    RuntimeOwnerAdapterCoreTestPeer::
        fixture_set_last_normal_enqueue_sequence(
            adapter, std::numeric_limits<std::uint32_t>::max());

    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.normal_count == 8);
    CHECK(!before.sequence_fatal_latched);
    CHECK(!before.critical_pending);
    CHECK(normal.submit(make_telemetry_intent(99, 99)) ==
          NormalSubmitResult::RejectedSequenceSaturated);

    RuntimeOwnerAdapterPrivateSnapshot after =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(after.normal_count == 8);
    CHECK(after.last_normal_enqueue_sequence ==
          std::numeric_limits<std::uint32_t>::max());
    CHECK(after.normal_rejected_full_count ==
          before.normal_rejected_full_count);
    CHECK(after.sequence_fatal_latched);
    CHECK(after.critical_pending);
    CHECK(after.critical.first_reason ==
          AdapterCriticalReason::NormalSequenceSaturation);
    CHECK(after.critical.last_reason ==
          AdapterCriticalReason::NormalSequenceSaturation);
    CHECK(after.critical.reason_mask == (1u << 2u));
    CHECK(after.critical.first_ingress_sequence == 0);
    CHECK(after.critical.last_ingress_sequence == 0);
    CHECK(after.critical.first_diagnostic_code == 0);
    CHECK(after.critical.last_diagnostic_code == 0);
    CHECK(after.critical.occurrence_count == 1);
    CHECK(adapter.view().sequence_fatal_latched == 1);
    CHECK(adapter.view().critical_pending == 1);
    CHECK(critical_ledgers_equal(adapter.view().critical, after.critical));
    after.sequence_fatal_latched = false;
    after.critical_pending = false;
    after.critical = before.critical;
    CHECK(private_snapshots_equal(before, after));

    const RuntimeOwnerAdapterPrivateSnapshot fatal_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(normal.submit(NormalIntent{}) ==
          NormalSubmitResult::RejectedNotReady);
    CHECK(private_snapshots_equal(
        fatal_before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_normal_diagnostic_counters_saturate_without_wrapping()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        auto normal = adapter.normal_port();
        CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
              NormalSubmitResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_normal_diagnostic_counts(
            adapter, maximum, 0);
        CHECK(normal.submit(make_telemetry_intent(1, 2)) ==
              NormalSubmitResult::AcceptedCoalesced);
        CHECK(adapter.view().normal_coalesced_count == maximum);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter).
                  normal_coalesced_count == maximum);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        auto normal = adapter.normal_port();
        for (std::uint32_t subject = 1; subject <= 8; ++subject) {
            CHECK(normal.submit(make_telemetry_intent(subject, subject)) ==
                  NormalSubmitResult::Accepted);
        }
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_normal_diagnostic_counts(
            adapter, 0, maximum);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(normal.submit(make_telemetry_intent(99, 99)) ==
              NormalSubmitResult::RejectedFull);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.normal_rejected_full_count == maximum);
        CHECK(after.normal_count == before.normal_count);
        CHECK(after.normal_head == before.normal_head);
        CHECK(after.normal_tail == before.normal_tail);
        for (std::size_t index = 0; index < after.normal_slots.size(); ++index) {
            CHECK(normal_slot_snapshots_equal(
                after.normal_slots[index], before.normal_slots[index]));
        }
    }
}

void test_shutdown_sticky_request_and_terminal_precedence()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        auto shutdown = adapter.shutdown_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(shutdown.request() == UrgentRequestResult::Accepted);
        RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.shutdown_pending);
        after.shutdown_pending = false;
        CHECK(private_snapshots_equal(before, after));

        const RuntimeOwnerAdapterPrivateSnapshot duplicate_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(shutdown.request() == UrgentRequestResult::AcceptedDuplicate);
        CHECK(private_snapshots_equal(
            duplicate_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        auto normal = adapter.normal_port();
        for (std::uint32_t subject = 1; subject <= 8; ++subject) {
            CHECK(normal.submit(make_telemetry_intent(subject, subject)) ==
                  NormalSubmitResult::Accepted);
        }
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
            adapter, true);
        auto shutdown = adapter.shutdown_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(shutdown.request() == UrgentRequestResult::Accepted);
        RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(after.normal_count == 8);
        CHECK(after.shutdown_pending);
        after.shutdown_pending = false;
        CHECK(private_snapshots_equal(before, after));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_commit_core_shutdown(adapter);
        auto shutdown = adapter.shutdown_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(shutdown.request() == UrgentRequestResult::AlreadyTerminal);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_shutdown_terminal_override(adapter, true);
        auto shutdown = adapter.shutdown_port();
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(shutdown.request() == UrgentRequestResult::AlreadyTerminal);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        auto shutdown = adapter.shutdown_port();
        CHECK(shutdown.request() == UrgentRequestResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::fixture_commit_core_shutdown(adapter);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(shutdown.request() == UrgentRequestResult::AlreadyTerminal);
        CHECK(private_snapshots_equal(
            before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }
}

void test_shutdown_cancels_end_boot_without_safety_preservation()
{
    RuntimeOwnerAdapterCore adapter{};
    const RuntimeOwnerEffect freeze =
        fixture_prepare_snapshot_freeze_pending_via_config(adapter);
    check_snapshot_succeeded_accepts(
        adapter,
        make_snapshot_receipt(
            TrustedReceiptKind::SnapshotSucceeded, freeze));
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.pending_effect_count == 1);
    const std::uint32_t end_boot_sequence =
        before.pending_effect_slots[before.pending_effect_head]
            .preassigned_dispatch_sequence;
    auto trusted_receipt = adapter.trusted_receipt_port();

    CHECK(adapter.shutdown_port().request() ==
          UrgentRequestResult::Accepted);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::RuntimeReady,
        RuntimeOwnerPhase::ShutdownCommitted);
    const RuntimeOwnerAdapterPrivateSnapshot committed =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(committed.pending_effect_count == 0);
    CHECK(has_safe_default(committed.current_dispatch));
    CHECK(has_safe_default(committed.physical_inflight));
    CHECK(!committed.physical_inflight_cancel_pending);
    CHECK(committed.effect_cancelled_count ==
          before.effect_cancelled_count + 1);
    CHECK(!committed.boot_end_released);
    CHECK(adapter.acknowledge_dispatch(end_boot_sequence) ==
          DispatchAckResult::RejectedNoDispatch);
    CHECK(!adapter.view().boot_end_released);
    CHECK(trusted_receipt.submit(make_snapshot_receipt(
              TrustedReceiptKind::SnapshotSucceeded, freeze)) ==
          TrustedIngressResult::RejectedNotAllowed);
}

void test_task6_healthy_shutdown_commits_once_and_cancels_non_safety()
{
    RuntimeOwnerAdapterCore adapter{};
    const AdapterDispatch inflight =
        fixture_prepare_normal_physical_inflight(adapter);
    auto trusted_receipt = adapter.trusted_receipt_port();
    auto normal_completion = adapter.normal_completion_port();
    auto normal = adapter.normal_port();
    CHECK(normal.submit(make_telemetry_intent(72, 20)) ==
          NormalSubmitResult::Accepted);
    CHECK(normal.submit(make_telemetry_intent(73, 21)) ==
          NormalSubmitResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(1, 1, 91)) ==
          TrustedEnqueueResult::Accepted);

    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.view().critical_pending == 1);

    auto shutdown = adapter.shutdown_port();
    CHECK(shutdown.request() == UrgentRequestResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::RuntimeReady,
        RuntimeOwnerPhase::ShutdownCommitted);
    const RuntimeOwnerAdapterPrivateSnapshot committed =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!committed.shutdown_pending);
    CHECK(!committed.shutdown_terminal_override_latched);
    CHECK(!committed.critical_pending);
    CHECK(committed.normal_count == 0);
    CHECK(committed.trusted_count == 0);
    CHECK(committed.pending_effect_count == 0);
    CHECK(has_safe_default(committed.current_dispatch));
    CHECK(inflight.kind == AdapterDispatchKind::NormalIntent);
    CHECK(has_safe_default(committed.physical_inflight));
    CHECK(!committed.physical_inflight_cancel_pending);
    CHECK(committed.normal_cancelled_count ==
          before.normal_cancelled_count + 3);
    CHECK(committed.trusted_cancelled_count ==
          before.trusted_cancelled_count + 1);
    CHECK(committed.effect_cancelled_count ==
          before.effect_cancelled_count);
    CHECK(critical_ledgers_equal(committed.critical, before.critical));

    const RuntimeOwnerAdapterPrivateSnapshot late_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(normal_completion.submit(make_normal_completion(
              NormalCompletionKind::Succeeded, inflight)) ==
          TrustedIngressResult::RejectedNotAllowed);
    CHECK(trusted_receipt.submit(
              make_transport_disconnected_receipt(1, 1, 91)) ==
          TrustedIngressResult::RejectedNotAllowed);
    CHECK(private_snapshots_equal(
        late_before, RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));

    const RuntimeOwnerAdapterPrivateSnapshot terminal_before = committed;
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ShutdownCommitted,
        RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(private_snapshots_equal(
        terminal_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);

    const RuntimeOwnerAdapterPrivateSnapshot repeated_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ShutdownCommitted,
        RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(private_snapshots_equal(
        repeated_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(shutdown.request() == UrgentRequestResult::AlreadyTerminal);
}

void test_task6_healthy_shutdown_preserves_one_recoverable_fault_without_ack()
{
    RuntimeOwnerAdapterCore adapter{};
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    const RuntimeOwnerAdapterPrivateSnapshot recovery =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(recovery.core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(recovery.pending_effect_count == 2);
    CHECK(!recovery.core_adapter_fatal_latched);
    CHECK(!recovery.core_fail_closed_latched);

    CHECK(adapter.shutdown_port().request() ==
          UrgentRequestResult::Accepted);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::ShutdownCommitted);
    const RuntimeOwnerAdapterPrivateSnapshot committed =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!committed.shutdown_pending);
    CHECK(!committed.shutdown_terminal_override_latched);
    CHECK(!committed.critical_pending);
    CHECK(committed.pending_effect_count == 1);
    CHECK(committed.pending_effect_slots[committed.pending_effect_head]
              .effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(committed.effect_cancelled_count ==
          recovery.effect_cancelled_count + 1);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(offered.effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(adapter.view().pending_effect_count == 0);
    const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ShutdownCommitted,
        RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(private_snapshots_equal(
        terminal_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedDelivery);
    CHECK(adapter.step().action == AdapterStepAction::Terminal);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
}

void test_task6_active_and_recovery_critical_preserve_safety_priority()
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.view().critical_pending == 1);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::RecoveryPending);
    RuntimeOwnerAdapterPrivateSnapshot recovery =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!recovery.transport_request_pending);
    CHECK(!recovery.critical_pending);
    CHECK(recovery.pending_effect_count == 2);
    CHECK(recovery.last_dispatch_sequence == 2);
    check_canonical_recovery_pending_pair(
        recovery,
        1,
        2,
        RuntimeOwnerFaultCode::CriticalIngress,
        0,
        {});

    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(77, 1, 91)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.view().critical_pending == 1);

    AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.prepared_dispatch_sequence == 1);
    AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(offered.effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(adapter.view().trusted_depth == 2);
    CHECK(adapter.view().critical_pending == 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedDelivery);

    prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.prepared_dispatch_sequence == 2);
    offered = adapter.peek_dispatch();
    CHECK(offered.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
    CHECK(adapter.view().trusted_depth == 2);
    CHECK(adapter.view().critical_pending == 1);
    CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
          DispatchAckResult::AcceptedDelivery);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CriticalLedgerHandled,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::RecoveryPending);
    const RuntimeOwnerAdapterPrivateSnapshot handled =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(!handled.critical_pending);
    CHECK(handled.critical.occurrence_count == 3);
    CHECK(handled.trusted_count == 2);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);

    const AdapterStepResult stale = adapter.step();
    CHECK(stale.action == AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(stale.consumed_ingress_sequence == 1);
}

void test_task6_sequence_saturation_delivers_safety_then_terminal()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        const AdapterDispatch inflight =
            fixture_prepare_normal_physical_inflight(adapter);
        auto normal = adapter.normal_port();
        CHECK(normal.submit(make_telemetry_intent(72, 20)) ==
              NormalSubmitResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_normal_enqueue_sequence(
                adapter,
                std::numeric_limits<std::uint32_t>::max());
        CHECK(normal.submit(make_telemetry_intent(73, 21)) ==
              NormalSubmitResult::RejectedSequenceSaturated);
        CHECK(adapter.view().last_normal_enqueue_sequence ==
              std::numeric_limits<std::uint32_t>::max());
        CHECK(adapter.view().sequence_fatal_latched == 1);
        CHECK(adapter.view().critical.occurrence_count == 1);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::RuntimeReady,
            RuntimeOwnerPhase::RecoveryPending);
        CHECK(adapter.view().normal_depth == 0);
        CHECK(adapter_dispatches_equal(
            adapter.view().physical_inflight, inflight));
        CHECK(adapter.view().physical_inflight_cancel_pending == 1);
        CHECK(adapter.view().pending_effect_count == 2);

        AdapterStepResult prepared = adapter.step();
        CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
        CHECK(prepared.prepared_dispatch_sequence == 2);
        AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.effect.kind == RuntimeOwnerEffectKind::RecordFault);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);

        prepared = adapter.step();
        CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
        CHECK(prepared.prepared_dispatch_sequence == 3);
        offered = adapter.peek_dispatch();
        CHECK(offered.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);

        const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Terminal,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::RecoveryPending,
            RuntimeOwnerPhase::RecoveryPending);
        CHECK(private_snapshots_equal(
            terminal_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(adapter_dispatches_equal(
            adapter.view().physical_inflight, inflight));
        CHECK(adapter.view().physical_inflight_cancel_pending == 1);

        const RuntimeOwnerAdapterPrivateSnapshot repeated_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(adapter.step().action == AdapterStepAction::Terminal);
        CHECK(private_snapshots_equal(
            repeated_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_trusted_ingress_sequence(
                adapter,
                std::numeric_limits<std::uint32_t>::max());
        const TrustedReceipt receipt =
            make_canonical_trusted_receipt(
                TrustedReceiptKind::TransportEstablished);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, receipt) ==
              TrustedEnqueueResult::RejectedSequenceSaturated);
        CHECK(adapter.view().last_trusted_ingress_sequence ==
              std::numeric_limits<std::uint32_t>::max());
        CHECK(adapter.view().critical.occurrence_count == 1);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        for (const RuntimeOwnerEffectKind expected_kind : {
                 RuntimeOwnerEffectKind::RecordFault,
                 RuntimeOwnerEffectKind::EnterRecovery,
             }) {
            const AdapterStepResult prepared = adapter.step();
            CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
            const AdapterDispatch offered = adapter.peek_dispatch();
            CHECK(offered.effect.kind == expected_kind);
            CHECK(adapter.acknowledge_dispatch(
                      offered.dispatch_sequence) ==
                  DispatchAckResult::AcceptedDelivery);
        }
        CHECK(adapter.step().action == AdapterStepAction::Terminal);

        const RuntimeOwnerAdapterPrivateSnapshot fatal_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(adapter.normal_port().submit(NormalIntent{}) ==
              NormalSubmitResult::RejectedNotReady);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, TrustedReceipt{}) ==
              TrustedEnqueueResult::RejectedNotAllowed);
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::RejectedFatal);
        CHECK(private_snapshots_equal(
            fatal_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(adapter.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
    }
}

void test_task6_dispatch_reserve_exact_boundaries_and_damage()
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;

    {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, maximum - 5);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        check_canonical_recovery_pending_pair(
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter),
            maximum - 4,
            maximum - 3,
            RuntimeOwnerFaultCode::CriticalIngress,
            0,
            {});
        fixture_ack_all_pending_safety_dispatches(adapter);
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.view().last_dispatch_sequence == maximum - 2);
        CHECK(adapter.view().pending_effect_count == 1);
        CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
    }

    constexpr std::array<std::uint32_t, 3> terminal_starts{{
        std::numeric_limits<std::uint32_t>::max() - 4,
        std::numeric_limits<std::uint32_t>::max() - 3,
        std::numeric_limits<std::uint32_t>::max() - 2,
    }};
    for (const std::uint32_t start : terminal_starts) {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.view().last_dispatch_sequence == maximum);
        CHECK(adapter.view().dispatch_fatal_latched == 1);
        check_canonical_recovery_pending_pair(
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter),
            maximum - 1,
            maximum,
            RuntimeOwnerFaultCode::CriticalIngress,
            0,
            {});
        fixture_ack_all_pending_safety_dispatches(adapter);
        const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(adapter.step().action == AdapterStepAction::Terminal);
        CHECK(private_snapshots_equal(
            terminal_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(adapter.request_transport_attempt() ==
              OwnerRequestResult::RejectedFatal);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter,
                  make_canonical_trusted_receipt(
                      TrustedReceiptKind::TransportEstablished)) ==
              TrustedEnqueueResult::RejectedNotAllowed);
    }

    constexpr std::array<std::uint32_t, 2> damaged_starts{{
        std::numeric_limits<std::uint32_t>::max() - 1,
        std::numeric_limits<std::uint32_t>::max(),
    }};
    for (const std::uint32_t start : damaged_starts) {
        RuntimeOwnerAdapterCore adapter{};
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
            adapter, start);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CriticalLedgerHandled,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::ColdStart,
            RuntimeOwnerPhase::ColdStart);
        const RuntimeOwnerAdapterPrivateSnapshot blocked =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(!blocked.critical_pending);
        CHECK(blocked.safety_delivery_blocked);
        CHECK(blocked.last_dispatch_sequence == start);
        CHECK(blocked.pending_effect_count == 0);
        CHECK(blocked.critical.occurrence_count == 1);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Terminal,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::ColdStart,
            RuntimeOwnerPhase::ColdStart);
        CHECK(private_snapshots_equal(
            blocked,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
    }
}

void test_task6_fatal_shutdown_local_override_does_not_wait_for_ack()
{
    {
        RuntimeOwnerAdapterCore adapter{};
        const AdapterDispatch inflight =
            fixture_prepare_normal_physical_inflight(adapter);
        auto normal = adapter.normal_port();
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_normal_enqueue_sequence(
                adapter,
                std::numeric_limits<std::uint32_t>::max());
        CHECK(normal.submit(make_telemetry_intent(72, 20)) ==
              NormalSubmitResult::RejectedSequenceSaturated);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.view().pending_effect_count == 2);
        CHECK(adapter.view().sequence_fatal_latched == 1);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        CHECK(adapter.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        const AdapterStepResult offered_result = adapter.step();
        CHECK(offered_result.action == AdapterStepAction::DispatchPrepared);
        CHECK(offered_result.core_disposition ==
              RuntimeOwnerDisposition::Rejected);
        CHECK(offered_result.phase_before ==
              RuntimeOwnerPhase::RecoveryPending);
        CHECK(offered_result.phase_after ==
              RuntimeOwnerPhase::RecoveryPending);
        CHECK(offered_result.prepared_dispatch_sequence == 2);
        const RuntimeOwnerAdapterPrivateSnapshot overridden =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(!overridden.shutdown_pending);
        CHECK(overridden.shutdown_terminal_override_latched);
        CHECK(!overridden.critical_pending);
        CHECK(overridden.pending_effect_count == 0);
        CHECK(overridden.current_dispatch.effect.kind ==
              RuntimeOwnerEffectKind::RecordFault);
        CHECK(overridden.effect_cancelled_count == 1);
        CHECK(inflight.kind == AdapterDispatchKind::NormalIntent);
        CHECK(has_safe_default(overridden.physical_inflight));
        CHECK(!overridden.physical_inflight_cancel_pending);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);

        const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Terminal,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::RecoveryPending,
            RuntimeOwnerPhase::RecoveryPending);
        CHECK(private_snapshots_equal(
            terminal_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(adapter.acknowledge_dispatch(
                  overridden.current_dispatch.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
        CHECK(adapter.step().action == AdapterStepAction::Terminal);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        TrustedReceipt invalid =
            make_canonical_trusted_receipt(
                TrustedReceiptKind::TransportEstablished);
        invalid.reserved = 1;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.effect.kind == RuntimeOwnerEffectKind::RecordFault);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        offered = adapter.peek_dispatch();
        CHECK(offered.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
        RuntimeOwnerAdapterCoreTestPeer::fixture_set_core_adapter_fatal(
            adapter, true);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);
        CHECK(adapter.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Terminal,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::RecoveryPending,
            RuntimeOwnerPhase::RecoveryPending);
        CHECK(adapter.peek_dispatch().kind == AdapterDispatchKind::None);
        CHECK(adapter.view().pending_effect_count == 0);
        CHECK(adapter.view().shutdown_terminal_override_latched == 1);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before);
    }
}

void test_task6_malformed_shutdown_consumes_origin_and_preserves_one_fault()
{
    RuntimeOwnerAdapterCore adapter{};
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().pending_effect_count == 2);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.view().critical_pending == 1);

    RuntimeOwnerTransition malformed{};
    malformed.disposition = RuntimeOwnerDisposition::Rejected;
    malformed.phase_before = RuntimeOwnerPhase::RecoveryPending;
    malformed.phase_after = RuntimeOwnerPhase::ShutdownCommitted;
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);
    CHECK(adapter.shutdown_port().request() == UrgentRequestResult::Accepted);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::ShutdownCommitted);
    const RuntimeOwnerAdapterPrivateSnapshot fallback =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(fallback.core.phase == RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(!fallback.shutdown_pending);
    CHECK(fallback.shutdown_terminal_override_latched);
    CHECK(!fallback.critical_pending);
    CHECK(fallback.core_adapter_fatal_latched);
    CHECK(fallback.pending_effect_count == 1);
    CHECK(fallback.pending_effect_slots[fallback.pending_effect_head]
              .effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(fallback.effect_cancelled_count == 1);
    CHECK(fallback.critical.occurrence_count == 3);
    CHECK(fallback.critical.last_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(offered.effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(adapter.view().pending_effect_count == 0);
    const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter.step().action == AdapterStepAction::Terminal);
    CHECK(private_snapshots_equal(
        terminal_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(adapter.shutdown_port().request() ==
          UrgentRequestResult::AlreadyTerminal);
}

void test_task6_review_noncommitting_malformed_shutdown_without_existing_fault_is_bounded()
{
    RuntimeOwnerAdapterCore adapter{};
    const AdapterDispatch inflight =
        fixture_prepare_normal_physical_inflight(adapter);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::RuntimeReady);
    CHECK(before.current_dispatch.kind == AdapterDispatchKind::None);
    CHECK(before.pending_effect_count == 0);

    RuntimeOwnerAdapterCoreTestPeer::
        fixture_override_next_core_post_submit_view(
            adapter, before.core);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::
              fixture_core_post_submit_view_override_pending(adapter));
    CHECK(adapter.shutdown_port().request() ==
          UrgentRequestResult::Accepted);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::RuntimeReady,
        RuntimeOwnerPhase::RuntimeReady);
    const RuntimeOwnerAdapterPrivateSnapshot fallback =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!RuntimeOwnerAdapterCoreTestPeer::
               fixture_core_post_submit_view_override_pending(adapter));
    CHECK(fallback.core.phase == RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(!fallback.shutdown_pending);
    CHECK(fallback.shutdown_terminal_override_latched);
    CHECK(fallback.core_adapter_fatal_latched);
    CHECK(!fallback.critical_pending);
    CHECK(fallback.pending_effect_count == 1);
    CHECK(fallback.pending_effect_count <=
          fallback.pending_effect_slots.size());
    CHECK(fallback.current_dispatch.kind == AdapterDispatchKind::None);
    CHECK(fallback.last_dispatch_sequence ==
          before.last_dispatch_sequence + 2);
    CHECK(fallback.effect_cancelled_count ==
          before.effect_cancelled_count + 1);
    CHECK(inflight.kind == AdapterDispatchKind::NormalIntent);
    CHECK(has_safe_default(fallback.physical_inflight));
    CHECK(!fallback.physical_inflight_cancel_pending);

    std::size_t record_fault_count = 0;
    std::size_t enter_recovery_count = 0;
    for (std::size_t offset = 0;
         offset < fallback.pending_effect_count; ++offset) {
        const std::size_t index =
            (fallback.pending_effect_head + offset) %
            fallback.pending_effect_slots.size();
        const RuntimeOwnerAdapterPendingEffectSlotSnapshot slot =
            fallback.pending_effect_slots[index];
        record_fault_count +=
            slot.effect.kind == RuntimeOwnerEffectKind::RecordFault ? 1 : 0;
        enter_recovery_count +=
            slot.effect.kind == RuntimeOwnerEffectKind::EnterRecovery ? 1 : 0;
        CHECK(slot.preassigned_dispatch_sequence ==
              before.last_dispatch_sequence + 1);
        CHECK(slot.effect.correlation_id == 0);
        CHECK(slot.effect.attempt == LivenessAttemptToken{});
        CHECK(slot.effect.fault_code ==
              RuntimeOwnerFaultCode::InternalInvariant);
    }
    CHECK(record_fault_count == 1);
    CHECK(enter_recovery_count == 0);

    const AdapterStepResult prepared = adapter.step();
    CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
    CHECK(prepared.core_disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(prepared.phase_before == RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(prepared.phase_after == RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(prepared.consumed_ingress_sequence == 0);
    CHECK(prepared.consumed_enqueue_sequence == 0);
    CHECK(prepared.prepared_dispatch_sequence ==
          before.last_dispatch_sequence + 1);
    const AdapterDispatch offered = adapter.peek_dispatch();
    CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
    CHECK(offered.effect.kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(offered.dispatch_sequence == prepared.prepared_dispatch_sequence);
    CHECK(adapter.view().pending_effect_count == 0);

    const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ShutdownCommitted,
        RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(private_snapshots_equal(
        terminal_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), offered));
    CHECK(has_safe_default(adapter.view().physical_inflight));
    CHECK(adapter.view().physical_inflight_cancel_pending == 0);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
}

void test_task6_review_noncommitting_malformed_shutdown_preserves_existing_fault_identity()
{
    for (const bool make_record_fault_current :
         std::array<bool, 2>{{false, true}}) {
        RuntimeOwnerAdapterCore adapter{};
        const AdapterDispatch inflight =
            fixture_prepare_normal_physical_inflight(adapter);
        TrustedReceipt invalid =
            make_canonical_trusted_receipt(
                TrustedReceiptKind::TransportEstablished);
        invalid.reserved = 1;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.view().core.phase ==
              RuntimeOwnerPhase::RecoveryPending);
        CHECK(adapter.view().pending_effect_count == 2);
        CHECK(adapter.view().physical_inflight_cancel_pending == 1);

        if (make_record_fault_current) {
            CHECK(adapter.step().action ==
                  AdapterStepAction::DispatchPrepared);
            CHECK(adapter.peek_dispatch().effect.kind ==
                  RuntimeOwnerEffectKind::RecordFault);
        }

        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        const RuntimeOwnerAdapterPendingEffectSlotSnapshot pending_record =
            before.pending_effect_slots[before.pending_effect_head];
        const std::uint32_t original_dispatch_sequence =
            make_record_fault_current
                ? before.current_dispatch.dispatch_sequence
                : pending_record.preassigned_dispatch_sequence;
        const RuntimeOwnerEffect original_effect =
            make_record_fault_current
                ? before.current_dispatch.effect
                : pending_record.effect;
        CHECK(original_dispatch_sequence != 0);
        CHECK(original_effect.kind == RuntimeOwnerEffectKind::RecordFault);
        CHECK(original_effect.fault_code ==
              RuntimeOwnerFaultCode::CriticalIngress);
        CHECK(before.pending_effect_count ==
              (make_record_fault_current ? 1 : 2));

        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_post_submit_view(
                adapter, before.core);
        CHECK(adapter.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreAdapterFatalHandled,
            RuntimeOwnerDisposition::FailClosed,
            RuntimeOwnerPhase::RecoveryPending,
            RuntimeOwnerPhase::RecoveryPending);
        const RuntimeOwnerAdapterPrivateSnapshot fallback =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before + 1);
        CHECK(!RuntimeOwnerAdapterCoreTestPeer::
                   fixture_core_post_submit_view_override_pending(adapter));
        CHECK(fallback.core.phase == RuntimeOwnerPhase::ShutdownCommitted);
        CHECK(!fallback.shutdown_pending);
        CHECK(fallback.shutdown_terminal_override_latched);
        CHECK(fallback.core_adapter_fatal_latched);
        CHECK(!fallback.critical_pending);
        CHECK(fallback.pending_effect_count <=
              fallback.pending_effect_slots.size());
        CHECK(fallback.last_dispatch_sequence ==
              before.last_dispatch_sequence + 2);
        CHECK(fallback.effect_cancelled_count ==
              before.effect_cancelled_count + 3);
        CHECK(inflight.kind == AdapterDispatchKind::NormalIntent);
        CHECK(has_safe_default(fallback.physical_inflight));
        CHECK(!fallback.physical_inflight_cancel_pending);

        std::size_t record_fault_count =
            fallback.current_dispatch.kind ==
                        AdapterDispatchKind::CoreEffect &&
                    fallback.current_dispatch.effect.kind ==
                        RuntimeOwnerEffectKind::RecordFault
                ? 1
                : 0;
        std::size_t enter_recovery_count =
            fallback.current_dispatch.kind ==
                        AdapterDispatchKind::CoreEffect &&
                    fallback.current_dispatch.effect.kind ==
                        RuntimeOwnerEffectKind::EnterRecovery
                ? 1
                : 0;
        for (std::size_t offset = 0;
             offset < fallback.pending_effect_count; ++offset) {
            const std::size_t index =
                (fallback.pending_effect_head + offset) %
                fallback.pending_effect_slots.size();
            const RuntimeOwnerEffectKind kind =
                fallback.pending_effect_slots[index].effect.kind;
            record_fault_count +=
                kind == RuntimeOwnerEffectKind::RecordFault ? 1 : 0;
            enter_recovery_count +=
                kind == RuntimeOwnerEffectKind::EnterRecovery ? 1 : 0;
        }
        CHECK(record_fault_count == 1);
        CHECK(enter_recovery_count == 0);
        CHECK(fallback.pending_effect_count +
                  (fallback.current_dispatch.kind ==
                           AdapterDispatchKind::None
                       ? 0
                       : 1) ==
              1);

        if (make_record_fault_current) {
            CHECK(fallback.current_dispatch.dispatch_sequence ==
                  original_dispatch_sequence);
            CHECK(runtime_owner_effects_equal(
                fallback.current_dispatch.effect, original_effect));
            CHECK(fallback.pending_effect_count == 0);
        } else {
            CHECK(fallback.current_dispatch.kind ==
                  AdapterDispatchKind::None);
            CHECK(fallback.pending_effect_count == 1);
            const RuntimeOwnerAdapterPendingEffectSlotSnapshot preserved =
                fallback.pending_effect_slots[
                    fallback.pending_effect_head];
            CHECK(preserved.preassigned_dispatch_sequence ==
                  original_dispatch_sequence);
            CHECK(runtime_owner_effects_equal(
                preserved.effect, original_effect));

            const AdapterStepResult prepared = adapter.step();
            CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
            CHECK(prepared.prepared_dispatch_sequence ==
                  original_dispatch_sequence);
        }

        const AdapterDispatch preserved = adapter.peek_dispatch();
        CHECK(preserved.kind == AdapterDispatchKind::CoreEffect);
        CHECK(preserved.dispatch_sequence == original_dispatch_sequence);
        CHECK(runtime_owner_effects_equal(
            preserved.effect, original_effect));
        const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::Terminal,
            RuntimeOwnerDisposition::Rejected,
            RuntimeOwnerPhase::ShutdownCommitted,
            RuntimeOwnerPhase::ShutdownCommitted);
        CHECK(private_snapshots_equal(
            terminal_before,
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
        CHECK(adapter_dispatches_equal(adapter.peek_dispatch(), preserved));
        CHECK(has_safe_default(adapter.view().physical_inflight));
        CHECK(adapter.view().physical_inflight_cancel_pending == 0);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before + 1);
    }
}

void check_task6_active_critical_trusted_authorization_is_cleared(
    const RuntimeOwnerAdapterPrivateSnapshot &after,
    const RuntimeOwnerAdapterPrivateSnapshot &before)
{
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    const std::uint32_t cancelled_increment = before.trusted_count;
    const std::uint32_t expected_cancelled =
        before.trusted_cancelled_count > maximum - cancelled_increment
            ? maximum
            : before.trusted_cancelled_count + cancelled_increment;

    CHECK(after.trusted_count == 0);
    CHECK(after.trusted_head == 0);
    CHECK(after.trusted_tail == 0);
    CHECK(after.trusted_cancelled_count == expected_cancelled);
    CHECK(after.trusted_high_water == before.trusted_high_water);
    CHECK(after.last_trusted_ingress_sequence ==
          before.last_trusted_ingress_sequence);
    CHECK(after.accepted_liveness_mask == 0);
    CHECK(last_trusted_receipt_signatures_equal(
        after.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(after.last_trusted_diagnostic_ingress_sequence ==
          before.last_trusted_diagnostic_ingress_sequence);
    CHECK(after.last_trusted_diagnostic_code ==
          before.last_trusted_diagnostic_code);
    CHECK(after.trusted_rejected_full_count ==
          before.trusted_rejected_full_count);
    CHECK(after.trusted_protocol_violation_count ==
          before.trusted_protocol_violation_count);
    CHECK(after.trusted_stale_count == before.trusted_stale_count);
    CHECK(after.trusted_duplicate_count == before.trusted_duplicate_count);
    for (const RuntimeOwnerAdapterTrustedSlotSnapshot slot :
         after.trusted_slots) {
        CHECK(trusted_slot_snapshots_equal(
            slot, RuntimeOwnerAdapterTrustedSlotSnapshot{}));
    }
}

void test_task6_review_active_critical_cancels_full_trusted_ring_and_non_safety_authorization()
{
    const auto fill_then_overflow_trusted_ring =
        [](RuntimeOwnerAdapterCore &adapter) {
            for (std::uint32_t index = 0; index < 8; ++index) {
                TrustedReceipt receipt =
                    make_canonical_trusted_receipt(
                        TrustedReceiptKind::TransportEstablished);
                receipt.mqtt_session_id = 100 + index;
                receipt.mqtt_generation = 200 + index;
                CHECK(RuntimeOwnerAdapterCoreTestPeer::
                          enqueue_trusted_receipt(adapter, receipt) ==
                      TrustedEnqueueResult::Accepted);
            }
            TrustedReceipt overflow =
                make_canonical_trusted_receipt(
                    TrustedReceiptKind::TransportEstablished);
            overflow.mqtt_session_id = 999;
            overflow.mqtt_generation = 999;
            CHECK(RuntimeOwnerAdapterCoreTestPeer::
                      enqueue_trusted_receipt(adapter, overflow) ==
                  TrustedEnqueueResult::RejectedFull);
        };

    {
        RuntimeOwnerAdapterCore adapter{};
        const AdapterDispatch inflight =
            fixture_prepare_normal_physical_inflight(adapter);
        CHECK(adapter.normal_port().submit(
                  make_telemetry_intent(72, 20)) ==
              NormalSubmitResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_accepted_liveness_mask(adapter, 0x05);
        fill_then_overflow_trusted_ring(adapter);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(before.core.phase == RuntimeOwnerPhase::RuntimeReady);
        CHECK(before.trusted_count == before.trusted_slots.size());
        CHECK(before.trusted_high_water == before.trusted_slots.size());
        CHECK(before.accepted_liveness_mask == 0x05);
        CHECK(before.normal_count == 1);
        CHECK(before.pending_effect_count == 1);
        CHECK(before.current_dispatch.kind == AdapterDispatchKind::None);
        CHECK(before.critical_pending);
        CHECK(before.critical.last_reason ==
              AdapterCriticalReason::TrustedQueueOverflow);
        const std::uint32_t submit_count_before =
            RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                adapter);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::RuntimeReady,
            RuntimeOwnerPhase::RecoveryPending);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
                  adapter) == submit_count_before + 1);
        check_task6_active_critical_trusted_authorization_is_cleared(
            after, before);
        CHECK(critical_ledgers_equal(after.critical, before.critical));
        CHECK(!after.critical_pending);
        CHECK(after.normal_count == 0);
        CHECK(after.normal_head == 0);
        CHECK(after.normal_tail == 0);
        CHECK(after.current_dispatch.kind == AdapterDispatchKind::None);
        CHECK(after.normal_cancelled_count ==
              before.normal_cancelled_count + 2);
        CHECK(after.effect_cancelled_count ==
              before.effect_cancelled_count + 1);
        CHECK(adapter_dispatches_equal(after.physical_inflight, inflight));
        CHECK(after.physical_inflight_cancel_pending);
        check_canonical_recovery_pending_pair(
            after,
            before.last_dispatch_sequence + 1,
            before.last_dispatch_sequence + 2,
            RuntimeOwnerFaultCode::CriticalIngress,
            0,
            before.core.active_attempt);
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        fixture_prepare_runtime_ready(adapter, true);
        auto normal = adapter.normal_port();
        CHECK(normal.submit(make_telemetry_intent(81, 21)) ==
              NormalSubmitResult::Accepted);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        CHECK(adapter.peek_dispatch().kind ==
              AdapterDispatchKind::NormalIntent);
        CHECK(normal.submit(make_telemetry_intent(82, 22)) ==
              NormalSubmitResult::Accepted);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_seed_authorization_pending_effect(adapter);
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_accepted_liveness_mask(adapter, 0x0a);
        fill_then_overflow_trusted_ring(adapter);
        const RuntimeOwnerAdapterPrivateSnapshot before =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        CHECK(before.current_dispatch.kind ==
              AdapterDispatchKind::NormalIntent);
        CHECK(before.normal_count == 1);
        CHECK(before.pending_effect_count == 1);
        CHECK(before.trusted_count == before.trusted_slots.size());
        CHECK(before.accepted_liveness_mask == 0x0a);

        check_exact_step_result(
            adapter.step(),
            AdapterStepAction::CoreTransitionApplied,
            RuntimeOwnerDisposition::Accepted,
            RuntimeOwnerPhase::RuntimeReady,
            RuntimeOwnerPhase::RecoveryPending);
        const RuntimeOwnerAdapterPrivateSnapshot after =
            RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
        check_task6_active_critical_trusted_authorization_is_cleared(
            after, before);
        CHECK(critical_ledgers_equal(after.critical, before.critical));
        CHECK(after.current_dispatch.kind == AdapterDispatchKind::None);
        CHECK(after.normal_count == 0);
        CHECK(after.normal_cancelled_count ==
              before.normal_cancelled_count + 2);
        CHECK(after.effect_cancelled_count ==
              before.effect_cancelled_count + 1);
        CHECK(after.physical_inflight.kind == AdapterDispatchKind::None);
        check_canonical_recovery_pending_pair(
            after,
            before.last_dispatch_sequence + 1,
            before.last_dispatch_sequence + 2,
            RuntimeOwnerFaultCode::CriticalIngress,
            0,
            before.core.active_attempt);
    }
}

void test_task6_review_active_critical_resets_liveness_authorization_before_recoverable_attempt()
{
    RuntimeOwnerAdapterCore adapter{};
    const std::array<RuntimeOwnerEffect, 4> tickets =
        fixture_prepare_liveness_waiting_via_config(adapter);
    const TrustedReceipt first_completion =
        make_operation_completed_receipt(tickets[0]);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, first_completion) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
              .accepted_liveness_mask == 0x01);

    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch second_ticket = adapter.peek_dispatch();
    CHECK(runtime_owner_effects_equal(second_ticket.effect, tickets[1]));
    CHECK(adapter.acknowledge_dispatch(second_ticket.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, first_completion) == TrustedEnqueueResult::Accepted);
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.core.phase == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(before.accepted_liveness_mask == 0x01);
    CHECK(before.trusted_count == 1);
    CHECK(adapter_dispatches_equal(before.physical_inflight, second_ticket));
    CHECK(before.pending_effect_count == 2);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::RecoveryPending);
    const RuntimeOwnerAdapterPrivateSnapshot recovery =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    check_task6_active_critical_trusted_authorization_is_cleared(
        recovery, before);
    CHECK(critical_ledgers_equal(recovery.critical, before.critical));
    CHECK(adapter_dispatches_equal(
        recovery.physical_inflight, second_ticket));
    CHECK(recovery.physical_inflight_cancel_pending);
    CHECK(recovery.pending_effect_count == 2);
    CHECK(recovery.effect_cancelled_count ==
          before.effect_cancelled_count + 3);

    fixture_ack_all_pending_safety_dispatches(adapter);
    const std::uint32_t duplicate_count_before =
        adapter.view().trusted_duplicate_count;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_liveness_failure_receipt(
                  TrustedReceiptKind::OperationFailed,
                  tickets[1],
                  91)) == TrustedEnqueueResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.view().physical_inflight.kind ==
          AdapterDispatchKind::None);
    CHECK(adapter.view().physical_inflight_cancel_pending == 0);
    CHECK(adapter.view().trusted_duplicate_count ==
          duplicate_count_before);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)
              .accepted_liveness_mask == 0);

    CHECK(adapter.request_transport_attempt() ==
          OwnerRequestResult::Accepted);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::TransportConnecting);
    const RuntimeOwnerAdapterPrivateSnapshot next_attempt =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(next_attempt.accepted_liveness_mask == 0);
    CHECK(next_attempt.trusted_count == 0);
    CHECK(next_attempt.trusted_duplicate_count ==
          duplicate_count_before);
    CHECK(last_trusted_receipt_signatures_equal(
        next_attempt.last_trusted_receipt_signature,
        before.last_trusted_receipt_signature));
    CHECK(next_attempt.pending_effect_count == 1);
    CHECK(next_attempt.pending_effect_slots[next_attempt.pending_effect_head]
              .effect.kind ==
          RuntimeOwnerEffectKind::StartTransportAttempt);
}

void test_task6_review_damaged_safety_reserve_cancels_trusted_and_mask_without_core_submit()
{
    RuntimeOwnerAdapterCore adapter{};
    RuntimeOwnerAdapterCoreTestPeer::fixture_seed_begin_fallback_cleanup_state(
        adapter);
    const std::uint32_t start =
        std::numeric_limits<std::uint32_t>::max() - 1;
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, start);
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished,
            RuntimeOwnerEffectKind::StartAtProbe,
            97);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    const RuntimeOwnerAdapterPrivateSnapshot before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before.trusted_count == 1);
    CHECK(before.normal_count == 1);
    CHECK(before.pending_effect_count == 1);
    CHECK(before.accepted_liveness_mask == 0x05);
    CHECK(before.critical_pending);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CriticalLedgerHandled,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::ColdStart);
    const RuntimeOwnerAdapterPrivateSnapshot blocked =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
    check_task6_active_critical_trusted_authorization_is_cleared(
        blocked, before);
    CHECK(critical_ledgers_equal(blocked.critical, before.critical));
    CHECK(!blocked.critical_pending);
    CHECK(blocked.safety_delivery_blocked);
    CHECK(blocked.last_dispatch_sequence == start);
    CHECK(blocked.normal_count == 0);
    CHECK(blocked.normal_cancelled_count ==
          before.normal_cancelled_count + 1);
    CHECK(blocked.pending_effect_count == 0);
    CHECK(blocked.effect_cancelled_count ==
          before.effect_cancelled_count + 1);
    CHECK(blocked.current_dispatch.kind == AdapterDispatchKind::None);
    CHECK(blocked.physical_inflight.kind == AdapterDispatchKind::None);

    const RuntimeOwnerAdapterPrivateSnapshot terminal_before = blocked;
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::ColdStart);
    CHECK(private_snapshots_equal(
        terminal_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
}

void test_task6_review_preflight_critical_cancels_origin_ring_without_rewriting_identity_or_ledger()
{
    RuntimeOwnerAdapterCore adapter{};
    fixture_prepare_awaiting_config_via_trusted(adapter, 77);
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerAdapterCoreTestPeer::fixture_set_last_dispatch_sequence(
        adapter, maximum - 5);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_config_committed_receipt(77, 1, 9)) ==
          TrustedEnqueueResult::Accepted);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(77, 1, 91)) ==
          TrustedEnqueueResult::Accepted);
    const RuntimeOwnerAdapterPrivateSnapshot before_preflight =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(before_preflight.trusted_count == 2);
    const std::uint32_t origin_ingress_sequence =
        before_preflight
            .trusted_slots[before_preflight.trusted_head]
            .ingress_sequence;
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CriticalLedgerHandled,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::AwaitingConfigCommit);
    const RuntimeOwnerAdapterPrivateSnapshot preflight =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before);
    CHECK(preflight.critical_pending);
    CHECK(preflight.trusted_count == before_preflight.trusted_count);
    CHECK(preflight.trusted_head == before_preflight.trusted_head);
    CHECK(preflight.trusted_tail == before_preflight.trusted_tail);
    CHECK(preflight.trusted_cancelled_count ==
          before_preflight.trusted_cancelled_count);
    CHECK(preflight.critical.first_reason ==
          AdapterCriticalReason::DispatchSequenceSaturation);
    CHECK(preflight.critical.last_reason ==
          AdapterCriticalReason::DispatchSequenceSaturation);
    CHECK(preflight.critical.first_ingress_sequence ==
          origin_ingress_sequence);
    CHECK(preflight.critical.last_ingress_sequence ==
          origin_ingress_sequence);
    CHECK(preflight.critical.occurrence_count == 1);
    CHECK(last_trusted_receipt_signatures_equal(
        preflight.last_trusted_receipt_signature,
        before_preflight.last_trusted_receipt_signature));
    CHECK(preflight.last_trusted_diagnostic_ingress_sequence ==
          before_preflight.last_trusted_diagnostic_ingress_sequence);
    CHECK(preflight.last_trusted_diagnostic_code ==
          before_preflight.last_trusted_diagnostic_code);
    for (std::size_t index = 0;
         index < preflight.trusted_slots.size(); ++index) {
        CHECK(trusted_slot_snapshots_equal(
            preflight.trusted_slots[index],
            before_preflight.trusted_slots[index]));
    }

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreTransitionApplied,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::RecoveryPending);
    const RuntimeOwnerAdapterPrivateSnapshot recovery =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    check_task6_active_critical_trusted_authorization_is_cleared(
        recovery, preflight);
    CHECK(critical_ledgers_equal(recovery.critical, preflight.critical));
    CHECK(!recovery.critical_pending);
    CHECK(recovery.last_dispatch_sequence == maximum - 3);
    CHECK(!recovery.dispatch_fatal_latched);
    check_canonical_recovery_pending_pair(
        recovery,
        maximum - 4,
        maximum - 3,
        RuntimeOwnerFaultCode::CriticalIngress,
        0,
        {});
}

void test_task6_nonshutdown_malformed_fatal_delivers_safety_before_terminal()
{
    RuntimeOwnerAdapterCore adapter{};
    const AdapterDispatch inflight =
        fixture_prepare_normal_physical_inflight(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_transport_disconnected_receipt(1, 1, 91)) ==
          TrustedEnqueueResult::Accepted);
    const std::uint32_t ingress_sequence =
        adapter.view().last_trusted_ingress_sequence;

    RuntimeOwnerTransition malformed{};
    malformed.disposition = RuntimeOwnerDisposition::Rejected;
    malformed.phase_before = RuntimeOwnerPhase::RuntimeReady;
    malformed.phase_after = RuntimeOwnerPhase::RecoveryPending;
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_ingress_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::RuntimeReady,
        RuntimeOwnerPhase::RecoveryPending,
        ingress_sequence);
    const RuntimeOwnerAdapterPrivateSnapshot fallback =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(fallback.core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(fallback.core_adapter_fatal_latched);
    CHECK(!fallback.critical_pending);
    CHECK(fallback.pending_effect_count == 2);
    CHECK(fallback.last_dispatch_sequence == 3);
    CHECK(fallback.critical.first_reason ==
          AdapterCriticalReason::CoreAdapterInvariant);
    CHECK(fallback.critical.occurrence_count == 1);
    CHECK(adapter_dispatches_equal(fallback.physical_inflight, inflight));
    CHECK(fallback.physical_inflight_cancel_pending);
    check_canonical_recovery_pending_pair(
        fallback,
        2,
        3,
        RuntimeOwnerFaultCode::InternalInvariant,
        0,
        {});

    for (const RuntimeOwnerEffectKind expected_kind : {
             RuntimeOwnerEffectKind::RecordFault,
             RuntimeOwnerEffectKind::EnterRecovery,
         }) {
        const AdapterStepResult prepared = adapter.step();
        CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(offered.effect.kind == expected_kind);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
    }
    const RuntimeOwnerAdapterPrivateSnapshot terminal_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::RecoveryPending);
    CHECK(private_snapshots_equal(
        terminal_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(adapter_dispatches_equal(adapter.view().physical_inflight, inflight));
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);

    const RuntimeOwnerAdapterPrivateSnapshot repeated_before =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(adapter.step().action == AdapterStepAction::Terminal);
    CHECK(private_snapshots_equal(
        repeated_before,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
}

void test_task6_malformed_critical_origin_is_consumed_once()
{
    RuntimeOwnerAdapterCore adapter{};
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);

    RuntimeOwnerTransition malformed{};
    malformed.disposition = RuntimeOwnerDisposition::Rejected;
    malformed.phase_before = RuntimeOwnerPhase::ColdStart;
    malformed.phase_after = RuntimeOwnerPhase::RecoveryPending;
    RuntimeOwnerAdapterCoreTestPeer::fixture_override_next_core_transition(
        adapter, malformed);
    const std::uint32_t submit_count_before =
        RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(adapter);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::CoreAdapterFatalHandled,
        RuntimeOwnerDisposition::FailClosed,
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::RecoveryPending);
    const RuntimeOwnerAdapterPrivateSnapshot fallback =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
    CHECK(!fallback.critical_pending);
    CHECK(fallback.core_adapter_fatal_latched);
    CHECK(fallback.pending_effect_count == 2);
    CHECK(fallback.last_dispatch_sequence == 2);
    check_canonical_recovery_pending_pair(
        fallback,
        1,
        2,
        RuntimeOwnerFaultCode::InternalInvariant,
        0,
        {});

    for (const RuntimeOwnerEffectKind expected_kind : {
             RuntimeOwnerEffectKind::RecordFault,
             RuntimeOwnerEffectKind::EnterRecovery,
         }) {
        const AdapterStepResult prepared = adapter.step();
        CHECK(prepared.action == AdapterStepAction::DispatchPrepared);
        const AdapterDispatch offered = adapter.peek_dispatch();
        CHECK(offered.kind == AdapterDispatchKind::CoreEffect);
        CHECK(offered.effect.kind == expected_kind);
        CHECK(adapter.acknowledge_dispatch(offered.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
    }
    CHECK(adapter.step().action == AdapterStepAction::Terminal);
    CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_core_submit_count(
              adapter) == submit_count_before + 1);
}

void test_task6_recovery_sequence_fatal_does_not_livelock_on_additional_critical()
{
    RuntimeOwnerAdapterCore adapter{};
    TrustedReceipt invalid =
        make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
    invalid.reserved = 1;
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);
    CHECK(adapter.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    fixture_ack_all_pending_safety_dispatches(adapter);
    CHECK(adapter.view().core.phase == RuntimeOwnerPhase::RecoveryPending);
    CHECK(adapter.view().pending_effect_count == 0);

    RuntimeOwnerAdapterCoreTestPeer::
        fixture_set_last_trusted_ingress_sequence(
            adapter,
            std::numeric_limits<std::uint32_t>::max());
    CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
              adapter,
              make_canonical_trusted_receipt(
                  TrustedReceiptKind::TransportEstablished)) ==
          TrustedEnqueueResult::RejectedSequenceSaturated);
    const RuntimeOwnerAdapterPrivateSnapshot fatal =
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter);
    CHECK(fatal.sequence_fatal_latched);
    CHECK(fatal.critical_pending);
    CHECK(fatal.critical.occurrence_count == 2);

    check_exact_step_result(
        adapter.step(),
        AdapterStepAction::Terminal,
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::RecoveryPending);
    CHECK(private_snapshots_equal(
        fatal,
        RuntimeOwnerAdapterCoreTestPeer::snapshot(adapter)));
    CHECK(adapter.step().action == AdapterStepAction::Terminal);
}

void test_task6_urgent_and_fatal_lifecycles_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        (void)fixture_prepare_normal_physical_inflight(adapter);
        auto normal = adapter.normal_port();
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_normal_enqueue_sequence(
                adapter,
                std::numeric_limits<std::uint32_t>::max());
        CHECK(normal.submit(make_telemetry_intent(72, 20)) ==
              NormalSubmitResult::RejectedSequenceSaturated);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
        CHECK(adapter.peek_dispatch().effect.kind ==
              RuntimeOwnerEffectKind::RecordFault);
        CHECK(adapter.step().action == AdapterStepAction::Terminal);
    }
    {
        RuntimeOwnerAdapterCore adapter{};
        TrustedReceipt invalid =
            make_canonical_trusted_receipt(
                TrustedReceiptKind::TransportEstablished);
        invalid.reserved = 1;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) ==
              TrustedEnqueueResult::RejectedInvalid);
        RuntimeOwnerTransition malformed{};
        malformed.disposition = RuntimeOwnerDisposition::Rejected;
        malformed.phase_before = RuntimeOwnerPhase::ColdStart;
        malformed.phase_after = RuntimeOwnerPhase::RecoveryPending;
        RuntimeOwnerAdapterCoreTestPeer::
            fixture_override_next_core_transition(adapter, malformed);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreAdapterFatalHandled);
        fixture_ack_all_pending_safety_dispatches(adapter);
        CHECK(adapter.step().action == AdapterStepAction::Terminal);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

void test_task3a_constructor_and_exercised_paths_are_allocation_free()
{
    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerAdapterCore adapter{};
            CHECK(adapter.view().core.phase == RuntimeOwnerPhase::ColdStart);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }

    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerAdapterCore adapter{};
            auto normal = adapter.normal_port();
            auto shutdown = adapter.shutdown_port();
            CHECK(normal.submit(NormalIntent{}) ==
                  NormalSubmitResult::RejectedNotReady);
            fixture_prepare_runtime_ready(adapter, true);
            CHECK(normal.submit(make_telemetry_intent(1, 1)) ==
                  NormalSubmitResult::Accepted);
            CHECK(normal.submit(make_telemetry_intent(1, 2)) ==
                  NormalSubmitResult::AcceptedCoalesced);
            RuntimeOwnerAdapterCoreTestPeer::
                fixture_set_last_normal_enqueue_sequence(
                    adapter, std::numeric_limits<std::uint32_t>::max());
            CHECK(normal.submit(make_telemetry_intent(2, 2)) ==
                  NormalSubmitResult::RejectedSequenceSaturated);
            CHECK(adapter.view().critical_pending == 1);
            CHECK(shutdown.request() == UrgentRequestResult::Accepted);
            CHECK(shutdown.request() ==
                  UrgentRequestResult::AcceptedDuplicate);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }
}

void test_task3b_trusted_admission_and_ring_paths_are_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        const TrustedReceipt valid = make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, valid) == TrustedEnqueueResult::Accepted);
        TrustedIngressEnvelope consumed{};
        CHECK(RuntimeOwnerAdapterCoreTestPeer::fixture_consume_trusted(
            adapter, consumed));

        TrustedReceipt invalid = valid;
        invalid.reserved = 1;
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, invalid) == TrustedEnqueueResult::RejectedInvalid);

        RuntimeOwnerAdapterCoreTestPeer::
            fixture_set_last_trusted_ingress_sequence(
                adapter, std::numeric_limits<std::uint32_t>::max());
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, valid) ==
              TrustedEnqueueResult::RejectedSequenceSaturated);
        CHECK(adapter.view().sequence_fatal_latched == 1);
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);

    const std::size_t full_allocations_before = g_allocation_count;
    const std::size_t full_deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerAdapterCore adapter{};
        const TrustedReceipt valid = make_canonical_trusted_receipt(
            TrustedReceiptKind::TransportEstablished);
        for (std::uint32_t index = 0; index < 8; ++index) {
            CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                      adapter, valid) == TrustedEnqueueResult::Accepted);
        }
        CHECK(RuntimeOwnerAdapterCoreTestPeer::enqueue_trusted_receipt(
                  adapter, valid) == TrustedEnqueueResult::RejectedFull);
        CHECK(adapter.view().trusted_depth == 8);
    }
    CHECK(g_allocation_count == full_allocations_before);
    CHECK(g_deallocation_count == full_deallocations_before);
}

} // namespace

void *operator new(const std::size_t size)
{
    ++g_allocation_count;
    if (void *const memory = std::malloc(size)) {
        return memory;
    }
    std::abort();
}

void *operator new[](const std::size_t size)
{
    ++g_allocation_count;
    if (void *const memory = std::malloc(size)) {
        return memory;
    }
    std::abort();
}

void operator delete(void *const memory) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete[](void *const memory) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete(void *const memory, const std::size_t) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete[](void *const memory, const std::size_t) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

int main()
{
    test_enum_numeric_and_unknown_contract();
    test_exact_dto_layout_and_type_traits();
    test_dto_pointer_reference_and_owning_container_free_contract();
    test_dto_safe_zero_defaults();
    test_adapter_explicit_noexcept_lifetime_and_noncopyable_contract();
    test_port_capability_construction_and_copy_contract();
    test_lvalue_only_port_factories();
    test_owner_only_api_signature_contract();
    test_port_minimal_surface_contract();
    test_forbidden_adapter_public_surface_contract();
    test_typed_ingress_ports_use_production_validation_and_shared_ring();
    test_pending_effect_storage_exact_layout_and_zero_initial_state();
    test_transport_request_accepts_only_coldstart_and_preboot_recovery();
    test_transport_request_duplicate_precedes_phase_and_is_mutation_free();
    test_transport_request_rejects_all_other_phases_and_boot_end();
    test_transport_request_fatal_precedes_disallowed_and_pending();
    test_transport_request_shutdown_pending_precedes_allowed_phase_and_duplicate();
    test_step_without_owner_trigger_is_canonical_idle_and_mutation_free();
    test_owner_trigger_step_prioritizes_shutdown_critical_and_fatal_terminal();
    test_owner_trigger_step_submits_begin_once_and_atomically_commits_pending_effect();
    test_owner_trigger_step_recovery_begin_uses_next_dispatch_sequence();
    test_owner_trigger_last_non_safety_dispatch_id_preserves_terminal_reserve();
    test_owner_trigger_dispatch_shortage_records_critical_before_core_submit();
    test_owner_trigger_precedes_ordinary_transport_head_and_pending_blocks_it();
    test_c1a_begin_unknown_and_unexpected_dispositions_fail_closed();
    test_c1a_begin_phase_before_corruptions_fail_closed();
    test_c1a_begin_phase_after_corruptions_fail_closed();
    test_c1a_begin_known_wrong_post_view_phase_fails_closed();
    test_c1a_begin_unknown_post_view_phase_is_normalized_to_before();
    test_c1a_begin_shutdown_post_view_suppresses_synthetic_pair();
    test_c1a_begin_post_view_fields_fail_closed_independently();
    test_c1a_begin_effect_count_corruptions_fail_closed();
    test_c1a_begin_used_effect_field_corruptions_fail_closed();
    test_c1a_begin_last_unused_effect_nonzero_fails_closed();
    test_c1a_begin_malformed_cleanup_purges_seeded_state();
    test_c1a_begin_malformed_sequence_regular_and_terminal_reserve();
    test_c1a_begin_malformed_damaged_reserve_blocks_without_retry();
    test_c1a_review_begin_canonical_override_pending_bypass_fails_closed();
    test_c1a_review_begin_canonical_override_max_sequence_bypass_fails_closed();
    test_c1a_begin_valid_override_remains_unaffected();
    test_c1a_unarmed_pending_effect_preserves_existing_deferral();
    test_c1a_unarmed_begin_shortage_preserves_existing_preflight();
    test_c1b1_transport_established_unknown_disposition_captures_fifo_provenance();
    test_c1b1_transport_established_unexpected_dispositions_fail_closed();
    test_c1b1_transport_established_transition_phases_fail_closed();
    test_c1b1_transport_established_effect_count_fail_closed();
    test_c1b1_transport_established_used_effect_fields_fail_closed();
    test_c1b1_transport_established_zero_effect_count_slot0_fields_fail_closed();
    test_c1b1_transport_established_unused_effect_fields_fail_closed();
    test_c1b1_transport_established_post_view_phases_fail_closed();
    test_c1b1_transport_established_post_view_fields_fail_closed();
    test_c1b1_transport_established_sequence_reserves_and_damage();
    test_c1b1_review_transport_established_canonical_override_pending_bypass_fails_closed();
    test_c1b1_transport_established_valid_overrides_remain_unaffected();
    test_c1b1_transport_established_unarmed_pending_defers();
    test_c1b1_override_does_not_bypass_unrelated_trusted_pending_gate();
    test_c1b1_stale_transport_established_override_does_not_bypass_pending_gate();
    test_c1b2_config_normal_dispositions_fail_closed();
    test_c1b2_config_counter_saturation_dispositions_fail_closed();
    test_c1b2_config_normal_transition_phases_fail_closed();
    test_c1b2_config_counter_saturation_transition_phases_fail_closed();
    test_c1b2_config_normal_effect_counts_fail_closed();
    test_c1b2_config_counter_saturation_effect_counts_fail_closed();
    test_c1b2_config_normal_last_used_effect_fields_fail_closed();
    test_c1b2_config_counter_saturation_used_effect_fields_fail_closed();
    test_c1b2_config_counter_saturation_unused_last_effect_fields_fail_closed();
    test_c1b2_config_normal_post_view_phases_fail_closed();
    test_c1b2_config_counter_saturation_post_view_phases_fail_closed();
    test_c1b2_config_normal_post_view_fields_fail_closed();
    test_c1b2_config_counter_saturation_post_view_fields_fail_closed();
    test_c1b2_config_normal_sequence_reserves_and_damage();
    test_c1b2_config_counter_saturation_sequence_reserves_and_damage();
    test_c1b2_review_config_normal_canonical_override_pending_bypass_fails_closed();
    test_c1b2_review_config_counter_saturation_canonical_override_pending_bypass_fails_closed();
    test_c1b2_review_config_normal_canonical_override_max_sequence_bypass_fails_closed();
    test_c1b2_review_config_counter_saturation_canonical_override_unavailable_safety_plan_fails_closed();
    test_c1b2_config_valid_overrides_preserve_existing_paths();
    test_c1b2_config_override_requires_full_authorization_to_bypass_pending_gate();
    test_transport_established_head_applies_once_updates_signature_and_dequeues();
    test_transport_established_wrong_generation_or_phase_discards_without_core_submit();
    test_task5_transport_receipt_requires_exact_acked_physical_attempt();
    test_transport_head_defers_to_shutdown_without_dequeue_or_signature_update();
    test_config_committed_head_atomically_applies_four_effect_bundle_and_signature();
    test_config_head_precedes_pending_owner_trigger_without_cancelling_trigger();
    test_config_head_with_pending_effects_defers_without_preemption_or_dequeue();
    test_config_wrong_phase_session_generation_equal_or_older_discards_without_submit();
    test_config_exact_epoch_counter_boundary_fail_closed_commits_safety_pair();
    test_config_exact_correlation_counter_boundary_fail_closed_commits_safety_pair();
    test_config_epoch_counter_one_before_boundary_remains_accepted();
    test_config_correlation_exact_last_bundle_start_remains_accepted();
    test_config_fail_closed_safety_pair_uses_regular_reserve_boundary();
    test_config_fail_closed_safety_pair_uses_exact_terminal_reserve();
    test_config_fail_closed_dispatch_shortage_latches_once_before_core_submit();
    test_config_last_bundle_start_preserves_terminal_reserve();
    test_config_dispatch_shortage_latches_once_without_submit_dequeue_or_partial_pending();
    test_config_head_defers_to_shutdown_critical_and_fatal_without_dequeue();
    test_transport_attempt_failed_happy_exact_commits_canonical_safety_pair();
    test_transport_attempt_failed_wrong_generation_or_phase_is_stale_without_submit();
    test_transport_attempt_failed_exact_signature_is_duplicate_but_otherwise_stale();
    test_transport_attempt_failed_diagnostic_code_does_not_change_core_translation();
    test_transport_attempt_failed_pending_effect_blocks_without_dequeue();
    test_transport_attempt_failed_sequence_preflight_regular_and_terminal_reserve();
    test_transport_attempt_failed_sequence_shortage_is_bounded_before_submit();
    test_transport_disconnected_happy_exact_accepts_all_active_phases();
    test_transport_disconnected_wrong_phase_session_or_generation_is_stale();
    test_transport_disconnected_exact_signature_is_duplicate_before_view_check();
    test_transport_disconnected_diagnostic_code_does_not_change_core_translation();
    test_transport_disconnected_pending_effect_defers_without_dequeue();
    test_transport_disconnected_sequence_preflight_regular_and_terminal_reserve();
    test_transport_disconnected_damaged_sequence_shortage_is_bounded();
    test_c1b4_transport_attempt_failed_full_malformed_matrix();
    test_c1b4_transport_disconnected_full_malformed_matrix();
    test_c1b4_transport_attempt_failed_post_view_fields_fail_closed();
    test_c1b4_transport_disconnected_post_view_fields_fail_closed();
    test_c1b4_transport_fault_malformed_terminal_reserves();
    test_operation_completed_dispatches_config_tickets_serially();
    test_operation_completed_immediate_and_non_immediate_duplicates();
    test_operation_completed_ticket_field_mismatches_and_phase_are_stale();
    test_operation_completed_pending_effects_defer_without_dequeue();
    test_operation_completed_sequence_preflight_only_for_final_ticket();
    test_operation_completed_final_sequence_shortage_is_bounded();
    test_c1b3a_operation_completed_nonfinal_unknown_disposition_fails_closed();
    test_c1b3a_operation_completed_final_wrong_count_fails_closed();
    test_c1b3a_operation_completed_nonfinal_dispositions_fail_closed();
    test_c1b3a_operation_completed_final_dispositions_fail_closed();
    test_c1b3a_operation_completed_nonfinal_transition_phases_fail_closed();
    test_c1b3a_operation_completed_final_transition_phases_fail_closed();
    test_c1b3a_operation_completed_nonfinal_effect_counts_fail_closed();
    test_c1b3a_operation_completed_final_effect_counts_fail_closed();
    test_c1b3a_operation_completed_nonfinal_unused_slot0_fields_fail_closed();
    test_c1b3a_operation_completed_nonfinal_unused_slot3_fields_fail_closed();
    test_c1b3a_operation_completed_final_used_slot0_fields_fail_closed();
    test_c1b3a_operation_completed_final_unused_slot3_fields_fail_closed();
    test_c1b3a_operation_completed_nonfinal_post_view_phases_fail_closed();
    test_c1b3a_operation_completed_final_post_view_phases_fail_closed();
    test_c1b3a_operation_completed_nonfinal_post_view_fields_fail_closed();
    test_c1b3a_operation_completed_final_post_view_fields_fail_closed();
    test_c1b3a_operation_completed_nonfinal_sequence_reserves_and_damage();
    test_c1b3a_operation_completed_final_sequence_reserves_and_damage();
    test_c1b3a_review_nonfinal_canonical_override_pending_bypass_fails_closed();
    test_c1b3a_review_final_canonical_override_pending_bypass_fails_closed();
    test_c1b3a_review_final_canonical_override_max_sequence_bypass_fails_closed();
    test_c1b3a_operation_completed_valid_overrides_preserve_paths();
    test_c1b3a_operation_completed_override_requires_full_authorization();
    test_operation_completed_mask_resets_on_accepted_recovery();
    test_c1b3b_operation_failed_unknown_disposition_fails_closed();
    test_c1b3b_operation_failed_dispositions_fail_closed();
    test_c1b3b_deadline_expired_dispositions_fail_closed();
    test_c1b3b_operation_failed_transition_phases_fail_closed();
    test_c1b3b_deadline_expired_transition_phases_fail_closed();
    test_c1b3b_operation_failed_effect_counts_fail_closed();
    test_c1b3b_deadline_expired_effect_counts_fail_closed();
    test_c1b3b_operation_failed_used_slot0_fields_fail_closed();
    test_c1b3b_operation_failed_used_slot1_fields_fail_closed();
    test_c1b3b_deadline_expired_used_slot0_fields_fail_closed();
    test_c1b3b_deadline_expired_used_slot1_fields_fail_closed();
    test_c1b3b_operation_failed_unused_slot3_fields_fail_closed();
    test_c1b3b_deadline_expired_unused_slot3_fields_fail_closed();
    test_c1b3b_operation_failed_post_view_phases_fail_closed();
    test_c1b3b_deadline_expired_post_view_phases_fail_closed();
    test_c1b3b_operation_failed_post_view_fields_fail_closed();
    test_c1b3b_deadline_expired_post_view_fields_fail_closed();
    test_c1b3b_operation_failed_sequence_reserves_and_damage();
    test_c1b3b_deadline_expired_sequence_reserves_and_damage();
    test_c1b3b_operation_failed_canonical_override_pending_bypass_fails_closed();
    test_c1b3b_deadline_expired_canonical_override_pending_bypass_fails_closed();
    test_c1b3b_operation_failed_canonical_override_unavailable_safety_plan_fails_closed();
    test_c1b3b_deadline_expired_canonical_override_unavailable_safety_plan_fails_closed();
    test_c1b3b_pending_override_requires_full_authorization();
    test_c1b3b_valid_overrides_preserve_all_eight_paths();
    test_liveness_failure_eight_happy_ticket_cases();
    test_liveness_failure_exact_duplicate_but_changed_diagnostic_is_stale();
    test_liveness_failure_completed_ticket_is_stale_not_duplicate();
    test_liveness_failure_uncompleted_ticket_accepts_with_other_mask_bit();
    test_liveness_failure_ticket_field_and_phase_mismatches_are_stale();
    test_liveness_failure_pending_effects_defer_without_dequeue();
    test_liveness_failure_sequence_regular_and_terminal_reserve();
    test_liveness_failure_damaged_sequence_shortage_is_bounded();
    test_liveness_failure_diagnostic_is_not_forwarded_to_core_or_effects();
    test_c1b3c_snapshot_failed_unknown_disposition_fails_closed();
    test_c1b3c_snapshot_succeeded_dispositions_fail_closed();
    test_c1b3c_snapshot_failed_dispositions_fail_closed();
    test_c1b3c_snapshot_succeeded_transition_phases_fail_closed();
    test_c1b3c_snapshot_failed_transition_phases_fail_closed();
    test_c1b3c_snapshot_succeeded_effect_counts_fail_closed();
    test_c1b3c_snapshot_failed_effect_counts_fail_closed();
    test_c1b3c_snapshot_succeeded_used_slot0_fields_fail_closed();
    test_c1b3c_snapshot_failed_used_slot0_fields_fail_closed();
    test_c1b3c_snapshot_failed_used_slot1_fields_fail_closed();
    test_c1b3c_snapshot_succeeded_unused_slot3_fields_fail_closed();
    test_c1b3c_snapshot_failed_unused_slot3_fields_fail_closed();
    test_c1b3c_snapshot_succeeded_post_view_phases_fail_closed();
    test_c1b3c_snapshot_failed_post_view_phases_fail_closed();
    test_c1b3c_snapshot_succeeded_post_view_fields_fail_closed();
    test_c1b3c_snapshot_failed_post_view_fields_fail_closed();
    test_c1b3c_snapshot_succeeded_sequence_reserves_and_damage();
    test_c1b3c_snapshot_failed_sequence_reserves_and_damage();
    test_c1b3c_snapshot_succeeded_canonical_override_pending_bypass_fails_closed();
    test_c1b3c_snapshot_failed_canonical_override_pending_bypass_fails_closed();
    test_c1b3c_snapshot_succeeded_canonical_override_max_sequence_bypass_fails_closed();
    test_c1b3c_snapshot_failed_canonical_override_unavailable_safety_plan_fails_closed();
    test_c1b3c_snapshot_pending_override_requires_full_authorization();
    test_c1b3c_snapshot_exact_signature_only_pending_override_remains_unconsumed();
    test_c1b3c_snapshot_duplicate_pending_overrides_remain_unconsumed();
    test_c1b3c_snapshot_valid_overrides_preserve_success_and_failure_paths();
    test_snapshot_succeeded_requires_exact_end_boot_delivery_ack();
    test_snapshot_succeeded_dispatch_sequence_shortage_is_bounded();
    test_snapshot_failed_happy_commits_canonical_safety_pair();
    test_snapshot_succeeded_duplicate_after_runtime_ready_is_adapter_duplicate();
    test_snapshot_failed_exact_signature_duplicate_precedes_authorization();
    test_snapshot_wrong_phase_token_fields_and_mask_are_stale();
    test_snapshot_pending_effect_defers_without_dequeue();
    test_snapshot_failed_diagnostic_is_not_forwarded_to_core_or_effects();
    test_snapshot_failed_sequence_regular_and_terminal_reserve();
    test_snapshot_failed_damaged_sequence_shortage_is_bounded();
    test_trusted_admission_gate_precedes_shape_and_is_mutation_free();
    test_trusted_receipt_full_canonical_matrix();
    test_trusted_ring_fifo_two_wraps_high_water_and_no_coalescing();
    test_trusted_admission_order_validation_then_sequence_then_capacity();
    test_trusted_full_rejects_new_without_drop_evict_or_sequence_consumption();
    test_trusted_sequence_last_success_saturation_before_full_and_sticky_gate();
    test_trusted_diagnostic_projection_includes_zero_code_events_only();
    test_trusted_counters_and_critical_ledger_saturate_and_preserve_first();
    test_shared_normal_intent_canonical_contract();
    test_normal_admission_precedence_and_canonical_validation();
    test_shutdown_pending_blocks_normal_before_shape_validation();
    test_safety_delivery_block_blocks_normal_before_shape_validation();
    test_normal_ring_fifo_high_water_and_index_wrap();
    test_normal_queued_same_key_coalescing_and_stale_slot_exclusion();
    test_normal_sequence_saturation_precedes_full_and_latches_fatal_critical();
    test_normal_diagnostic_counters_saturate_without_wrapping();
    test_shutdown_sticky_request_and_terminal_precedence();
    test_shutdown_cancels_end_boot_without_safety_preservation();
    test_task6_healthy_shutdown_commits_once_and_cancels_non_safety();
    test_task6_healthy_shutdown_preserves_one_recoverable_fault_without_ack();
    test_task6_active_and_recovery_critical_preserve_safety_priority();
    test_task6_sequence_saturation_delivers_safety_then_terminal();
    test_task6_dispatch_reserve_exact_boundaries_and_damage();
    test_task6_fatal_shutdown_local_override_does_not_wait_for_ack();
    test_task6_malformed_shutdown_consumes_origin_and_preserves_one_fault();
    test_task6_review_noncommitting_malformed_shutdown_without_existing_fault_is_bounded();
    test_task6_review_noncommitting_malformed_shutdown_preserves_existing_fault_identity();
    test_task6_review_active_critical_cancels_full_trusted_ring_and_non_safety_authorization();
    test_task6_review_active_critical_resets_liveness_authorization_before_recoverable_attempt();
    test_task6_review_damaged_safety_reserve_cancels_trusted_and_mask_without_core_submit();
    test_task6_review_preflight_critical_cancels_origin_ring_without_rewriting_identity_or_ledger();
    test_task6_nonshutdown_malformed_fatal_delivers_safety_before_terminal();
    test_task6_malformed_critical_origin_is_consumed_once();
    test_task6_recovery_sequence_fatal_does_not_livelock_on_additional_critical();
    test_task6_urgent_and_fatal_lifecycles_are_allocation_free();
    test_task3a_constructor_and_exercised_paths_are_allocation_free();
    test_task3b_trusted_admission_and_ring_paths_are_allocation_free();
    test_task4a_request_and_begin_paths_are_allocation_free();
    test_task5_pending_core_effect_prepares_one_sticky_current_dispatch();
    test_task5_ready_normal_head_prepares_one_sticky_current_dispatch();
    test_task5_ack_without_current_rejects_and_only_counts_diagnostic();
    test_task5_wrong_ack_preserves_exact_current_then_core_ack_moves_inflight();
    test_task5_last_ack_duplicate_preserves_new_current_dispatch();
    test_task5_delivery_only_safety_acks_do_not_create_physical_inflight();
    test_task5_exact_normal_ack_moves_full_dispatch_to_physical_inflight();
    test_task5_normal_completion_canonical_shape_and_shared_ring_capacity();
    test_task5_exact_normal_completion_closes_inflight_for_all_outcomes();
    test_task5_wrong_and_duplicate_normal_completion_are_bounded();
    test_task5_accepted_config_cancels_current_and_quarantines_inflight();
    test_task5_exact_config_retransmission_is_duplicate();
    test_task5_quarantined_inflight_does_not_block_valid_disconnect();
    test_task5_ready_disconnect_cancels_normal_and_quarantines_completion();
    test_task5_disconnect_quarantine_requires_exact_core_terminal_receipt();
    test_task5_rejected_control_receipts_preserve_old_authorization();
    test_task5_full_lifecycle_is_allocation_free();
    test_task4b1_trusted_transition_paths_are_allocation_free();
    test_task4b2_a1_transport_attempt_failed_path_is_allocation_free();
    test_task4b2_a2_transport_disconnected_path_is_allocation_free();
    test_task4b2_b1_operation_completed_path_is_allocation_free();
    test_task4b2_b2_liveness_failure_paths_are_allocation_free();
    test_task4b2_b3_snapshot_receipt_paths_are_allocation_free();
    test_task4c_c1b3a_operation_completed_malformed_paths_are_allocation_free();
    test_task4c_c1b3b_liveness_failure_malformed_paths_are_allocation_free();
    test_task4c_c1b3c_snapshot_malformed_paths_are_allocation_free();
    test_task4c_c1b4_transport_fault_malformed_paths_are_allocation_free();
    test_task4c_c1b2_config_malformed_paths_are_allocation_free();
    test_task4c_c1b1_transport_established_malformed_path_is_allocation_free();
    test_task4c_c1a_begin_malformed_paths_are_allocation_free();

    if (g_failure_count != 0) {
        std::fprintf(
            stderr,
            "RUNTIME_OWNER_ADAPTER_CORE_TEST FAIL checks=%zu failures=%zu\n",
            g_check_count,
            g_failure_count);
        return 1;
    }

    std::printf(
        "RUNTIME_OWNER_ADAPTER_CORE_TEST PASS checks=%zu\n",
        g_check_count);
    return 0;
}
