#include "runtime_owner_task_core.hpp"

namespace boot_v2 {
namespace {

constexpr bool normal_intents_equal(
    const NormalIntent left,
    const NormalIntent right) noexcept
{
    return left.kind == right.kind && left.flags == right.flags &&
           left.reserved == right.reserved &&
           left.subject_id == right.subject_id &&
           left.snapshot_revision == right.snapshot_revision;
}

constexpr bool effects_equal(
    const RuntimeOwnerEffect left,
    const RuntimeOwnerEffect right) noexcept
{
    return left.kind == right.kind &&
           left.correlation_id == right.correlation_id &&
           left.attempt == right.attempt &&
           left.fault_code == right.fault_code;
}

constexpr bool dispatches_equal(
    const AdapterDispatch left,
    const AdapterDispatch right) noexcept
{
    return left.kind == right.kind && left.reserved == right.reserved &&
           left.dispatch_sequence == right.dispatch_sequence &&
           left.enqueue_sequence == right.enqueue_sequence &&
           effects_equal(left.effect, right.effect) &&
           normal_intents_equal(left.normal_intent, right.normal_intent);
}

constexpr bool commands_equal(
    const RuntimeOwnerExecutorCommand left,
    const RuntimeOwnerExecutorCommand right) noexcept
{
    return left.kind == right.kind &&
           dispatches_equal(left.source, right.source) &&
           left.completion_policy == right.completion_policy;
}

bool map_exact_dispatch(
    const AdapterDispatch dispatch,
    const RuntimeOwnerExecutorCommand command) noexcept
{
    RuntimeOwnerExecutorCommand expected{};
    return map_runtime_owner_dispatch(dispatch, expected) ==
               RuntimeOwnerExecutorMapResult::Mapped &&
           commands_equal(expected, command);
}

bool exact_committed_end_boot_command(
    const RuntimeOwnerExecutorCommand command,
    const RuntimeActivationGrant grant) noexcept
{
    return runtime_activation_grant_is_canonical(grant) &&
           command.kind ==
               RuntimeOwnerDeviceOperationKind::EndBootOrchestration &&
           command.completion_policy == CompletionPolicy::DeliveryOnly &&
           map_exact_dispatch(command.source, command) &&
           command.source.dispatch_sequence ==
               grant.boot_end_dispatch_sequence &&
           command.source.effect.kind ==
               RuntimeOwnerEffectKind::EndBootOrchestration &&
           command.source.effect.correlation_id ==
               grant.boot_end_correlation_id &&
           command.source.effect.attempt == grant.liveness;
}

constexpr bool is_liveness_operation(
    const RuntimeOwnerDeviceOperationKind kind) noexcept
{
    return kind == RuntimeOwnerDeviceOperationKind::ProbeAt ||
           kind == RuntimeOwnerDeviceOperationKind::PublishProbe ||
           kind == RuntimeOwnerDeviceOperationKind::VerifySubscription ||
           kind == RuntimeOwnerDeviceOperationKind::PullFollowupConfig;
}

RuntimeOwnerExecutorResult ingress_result(
    const TrustedIngressResult result) noexcept
{
    switch (result) {
    case TrustedIngressResult::Accepted:
        return RuntimeOwnerExecutorResult::Accepted;
    case TrustedIngressResult::RejectedInvalid:
        return RuntimeOwnerExecutorResult::RejectedInvalid;
    case TrustedIngressResult::RejectedNotAllowed:
    case TrustedIngressResult::RejectedFull:
    case TrustedIngressResult::RejectedSequenceSaturated:
        return RuntimeOwnerExecutorResult::RejectedNotAllowed;
    }
    return RuntimeOwnerExecutorResult::RejectedInvalid;
}

} // namespace

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::peek_command(
    RuntimeOwnerExecutorCommand &command) const noexcept
{
    if (owner_ == nullptr) {
        command = {};
        return RuntimeOwnerExecutorResult::RejectedInvalid;
    }
    return owner_->executor_peek_command(command);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::acknowledge_command(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_acknowledge_command(command);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::transport_established(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t mqtt_session_id) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_transport_established(
                     command, mqtt_session_id);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::transport_failed(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_transport_failed(
                     command, diagnostic_code);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::transport_disconnected(
    const std::uint32_t mqtt_session_id,
    const std::uint32_t mqtt_generation,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_transport_disconnected(
                     mqtt_session_id, mqtt_generation, diagnostic_code);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::config_committed(
    const std::uint32_t config_commit_sequence) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_config_committed(
                     config_commit_sequence);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::liveness_succeeded(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_liveness_result(
                     command, TrustedReceiptKind::OperationCompleted, 0);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::liveness_failed(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_liveness_result(
                     command,
                     TrustedReceiptKind::OperationFailed,
                     diagnostic_code);
}

RuntimeOwnerExecutorResult
RuntimeOwnerExecutorPort::liveness_deadline_expired(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_liveness_result(
                     command,
                     TrustedReceiptKind::DeadlineExpired,
                     diagnostic_code);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::snapshot_succeeded(
    const RuntimeOwnerExecutorCommand command,
    const BootRuntimeSnapshotV1 snapshot) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_snapshot_succeeded(command, snapshot);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::snapshot_failed(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_snapshot_failed(
                     command, diagnostic_code);
}

RuntimeOwnerExecutorResult
RuntimeOwnerExecutorPort::commit_end_boot_delivery(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->commit_end_boot_delivery(command);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::publish_runtime(
    const RuntimeStatusSnapshotV1 snapshot) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_publish_runtime(snapshot);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::normal_succeeded(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_normal_result(
                     command, NormalCompletionKind::Succeeded, 0);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::normal_failed(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_normal_result(
                     command,
                     NormalCompletionKind::Failed,
                     diagnostic_code);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::normal_timed_out(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_normal_result(
                     command,
                     NormalCompletionKind::TimedOut,
                     diagnostic_code);
}

RuntimeOwnerExecutorResult RuntimeOwnerExecutorPort::normal_cancelled(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerExecutorResult::RejectedInvalid
               : owner_->executor_normal_result(
                     command,
                     NormalCompletionKind::Cancelled,
                     diagnostic_code);
}

RuntimeOwnerShutdownRequestResult
RuntimeOwnerPowerButtonShutdownPort::request(
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerShutdownRequestResult::RejectedInvalid
               : owner_->request_shutdown(
                     RuntimeOwnerTaskCore::ShutdownProducer::PowerButton,
                     producer_sequence,
                     incident_correlation_id);
}

RuntimeOwnerShutdownRequestResult RuntimeOwnerAdapterLossShutdownPort::request(
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerShutdownRequestResult::RejectedInvalid
               : owner_->request_shutdown(
                     RuntimeOwnerTaskCore::ShutdownProducer::AdapterLossCommitted,
                     producer_sequence,
                     incident_correlation_id);
}

RuntimeOwnerShutdownRequestResult
RuntimeOwnerAuthenticatedCommandShutdownPort::request(
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    return owner_ == nullptr
               ? RuntimeOwnerShutdownRequestResult::RejectedInvalid
               : owner_->request_shutdown(
                     RuntimeOwnerTaskCore::ShutdownProducer::
                         AuthenticatedRemoteCommand,
                     producer_sequence,
                     incident_correlation_id);
}

RuntimeOwnerTaskActivationResult RuntimeOwnerTaskCore::activate(
    const RuntimeOwnerCutoverPermit &permit) noexcept
{
    if (state_ == RuntimeOwnerTaskState::Terminal) {
        return RuntimeOwnerTaskActivationResult::RejectedTerminal;
    }
    if (permit.stable_identity_ == 0) {
        return RuntimeOwnerTaskActivationResult::RejectedInvalid;
    }
    if (state_ == RuntimeOwnerTaskState::Dormant) {
        active_permit_identity_ = permit.stable_identity_;
        state_ = RuntimeOwnerTaskState::Active;
        return RuntimeOwnerTaskActivationResult::Activated;
    }
    return active_permit_identity_ == permit.stable_identity_
               ? RuntimeOwnerTaskActivationResult::AlreadyActive
               : RuntimeOwnerTaskActivationResult::RejectedInvalid;
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_state_gate() const
    noexcept
{
    if (shutdown_provenance_valid_ != 0 ||
        state_ == RuntimeOwnerTaskState::Terminal) {
        return RuntimeOwnerExecutorResult::RejectedTerminalDropped;
    }
    if (state_ != RuntimeOwnerTaskState::Active) {
        return RuntimeOwnerExecutorResult::RejectedInactive;
    }
    return RuntimeOwnerExecutorResult::Accepted;
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_peek_command(
    RuntimeOwnerExecutorCommand &command) const noexcept
{
    command = {};
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const AdapterDispatch dispatch = adapter_.peek_dispatch();
    if (dispatch.kind == AdapterDispatchKind::None) {
        return RuntimeOwnerExecutorResult::RejectedNoCommand;
    }
    if (map_runtime_owner_dispatch(dispatch, command) !=
        RuntimeOwnerExecutorMapResult::Mapped) {
        command = {};
        return RuntimeOwnerExecutorResult::RejectedInvalid;
    }
    return RuntimeOwnerExecutorResult::Accepted;
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_acknowledge_command(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    if (!map_exact_dispatch(adapter_.peek_dispatch(), command)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    if (command.kind ==
        RuntimeOwnerDeviceOperationKind::EndBootOrchestration) {
        return RuntimeOwnerExecutorResult::RejectedEndBootRequiresCommit;
    }
    const DispatchAckResult result = adapter_.acknowledge_dispatch(
        command.source.dispatch_sequence);
    if (result == DispatchAckResult::AcceptedDelivery ||
        result == DispatchAckResult::AcceptedOperationInflight) {
        return RuntimeOwnerExecutorResult::Accepted;
    }
    if (result == DispatchAckResult::AcceptedDuplicate) {
        return RuntimeOwnerExecutorResult::AcceptedDuplicate;
    }
    return RuntimeOwnerExecutorResult::RejectedWrongCommand;
}

RuntimeOwnerExecutorResult
RuntimeOwnerTaskCore::executor_transport_established(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t mqtt_session_id) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const AdapterDispatch inflight = adapter_.view().physical_inflight;
    if (mqtt_session_id == 0 ||
        command.kind != RuntimeOwnerDeviceOperationKind::OpenTransport ||
        command.completion_policy != CompletionPolicy::TrustedReceipt ||
        !map_exact_dispatch(inflight, command)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    TrustedReceipt receipt{};
    receipt.kind = TrustedReceiptKind::TransportEstablished;
    receipt.effect_kind = RuntimeOwnerEffectKind::StartTransportAttempt;
    receipt.mqtt_session_id = mqtt_session_id;
    receipt.mqtt_generation = command.source.effect.attempt.mqtt_generation;
    return ingress_result(adapter_.trusted_receipt_port().submit(receipt));
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_transport_failed(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const AdapterDispatch inflight = adapter_.view().physical_inflight;
    if (diagnostic_code == 0 ||
        command.kind != RuntimeOwnerDeviceOperationKind::OpenTransport ||
        !map_exact_dispatch(inflight, command)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    TrustedReceipt receipt{};
    receipt.kind = TrustedReceiptKind::TransportAttemptFailed;
    receipt.effect_kind = RuntimeOwnerEffectKind::StartTransportAttempt;
    receipt.mqtt_generation = command.source.effect.attempt.mqtt_generation;
    receipt.diagnostic_code = diagnostic_code;
    return ingress_result(adapter_.trusted_receipt_port().submit(receipt));
}

RuntimeOwnerExecutorResult
RuntimeOwnerTaskCore::executor_transport_disconnected(
    const std::uint32_t mqtt_session_id,
    const std::uint32_t mqtt_generation,
    const std::uint32_t diagnostic_code) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    if (mqtt_session_id == 0 || mqtt_generation == 0) {
        return RuntimeOwnerExecutorResult::RejectedInvalid;
    }
    const RuntimeOwnerView view = adapter_.view().core;
    const bool phase_allowed =
        view.phase == RuntimeOwnerPhase::AwaitingConfigCommit ||
        view.phase == RuntimeOwnerPhase::LivenessWaiting ||
        view.phase == RuntimeOwnerPhase::SnapshotFreezePending ||
        view.phase == RuntimeOwnerPhase::RuntimeReady;
    if (!phase_allowed) {
        return RuntimeOwnerExecutorResult::RejectedNotAllowed;
    }
    if (view.mqtt_session_id != mqtt_session_id ||
        view.mqtt_generation != mqtt_generation) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    TrustedReceipt receipt{};
    receipt.kind = TrustedReceiptKind::TransportDisconnected;
    receipt.mqtt_session_id = mqtt_session_id;
    receipt.mqtt_generation = mqtt_generation;
    receipt.diagnostic_code = diagnostic_code;
    return ingress_result(adapter_.trusted_receipt_port().submit(receipt));
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_config_committed(
    const std::uint32_t config_commit_sequence) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const RuntimeOwnerView view = adapter_.view().core;
    if (config_commit_sequence == 0 ||
        view.phase != RuntimeOwnerPhase::AwaitingConfigCommit ||
        view.mqtt_session_id == 0 || view.mqtt_generation == 0) {
        return RuntimeOwnerExecutorResult::RejectedNotAllowed;
    }
    TrustedReceipt receipt{};
    receipt.kind = TrustedReceiptKind::ConfigCommitted;
    receipt.mqtt_session_id = view.mqtt_session_id;
    receipt.mqtt_generation = view.mqtt_generation;
    receipt.config_commit_sequence = config_commit_sequence;
    return ingress_result(adapter_.trusted_receipt_port().submit(receipt));
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_liveness_result(
    const RuntimeOwnerExecutorCommand command,
    const TrustedReceiptKind receipt_kind,
    const std::uint32_t diagnostic_code) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const AdapterDispatch inflight = adapter_.view().physical_inflight;
    const bool succeeded =
        receipt_kind == TrustedReceiptKind::OperationCompleted;
    const bool failure_kind =
        receipt_kind == TrustedReceiptKind::OperationFailed ||
        receipt_kind == TrustedReceiptKind::DeadlineExpired;
    if ((!succeeded && !failure_kind) ||
        !is_liveness_operation(command.kind) ||
        command.completion_policy != CompletionPolicy::TrustedReceipt ||
        !map_exact_dispatch(inflight, command) ||
        (succeeded && diagnostic_code != 0)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    TrustedReceipt receipt{};
    receipt.kind = receipt_kind;
    receipt.effect_kind = command.source.effect.kind;
    receipt.correlation_id = command.source.effect.correlation_id;
    receipt.mqtt_session_id = command.source.effect.attempt.mqtt_session_id;
    receipt.mqtt_generation = command.source.effect.attempt.mqtt_generation;
    receipt.config_apply_epoch =
        command.source.effect.attempt.config_apply_epoch;
    receipt.diagnostic_code = diagnostic_code;
    return ingress_result(adapter_.trusted_receipt_port().submit(receipt));
}

RuntimeOwnerExecutorResult
RuntimeOwnerTaskCore::executor_snapshot_succeeded(
    const RuntimeOwnerExecutorCommand command,
    const BootRuntimeSnapshotV1 snapshot) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const AdapterDispatch inflight = adapter_.view().physical_inflight;
    if (command.kind !=
            RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot ||
        command.completion_policy != CompletionPolicy::TrustedReceipt ||
        !map_exact_dispatch(inflight, command)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }

    const BootSnapshotFreezeResult frozen = snapshot_.freeze_boot(
        command.source.effect, snapshot);
    if (frozen != BootSnapshotFreezeResult::Accepted &&
        frozen != BootSnapshotFreezeResult::AcceptedDuplicate) {
        return RuntimeOwnerExecutorResult::RejectedSnapshotStore;
    }

    snapshot_frozen_latched_ = 1;
    pending_activation_grant_.liveness = command.source.effect.attempt;
    pending_activation_grant_.snapshot_correlation_id =
        command.source.effect.correlation_id;

    TrustedReceipt receipt{};
    receipt.kind = TrustedReceiptKind::SnapshotSucceeded;
    receipt.effect_kind = RuntimeOwnerEffectKind::FreezeBootSnapshot;
    receipt.correlation_id = command.source.effect.correlation_id;
    receipt.mqtt_session_id = command.source.effect.attempt.mqtt_session_id;
    receipt.mqtt_generation = command.source.effect.attempt.mqtt_generation;
    receipt.config_apply_epoch =
        command.source.effect.attempt.config_apply_epoch;
    return ingress_result(adapter_.trusted_receipt_port().submit(receipt));
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_snapshot_failed(
    const RuntimeOwnerExecutorCommand command,
    const std::uint32_t diagnostic_code) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const AdapterDispatch inflight = adapter_.view().physical_inflight;
    if (diagnostic_code == 0 ||
        command.kind !=
            RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot ||
        command.completion_policy != CompletionPolicy::TrustedReceipt ||
        !map_exact_dispatch(inflight, command)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    TrustedReceipt receipt{};
    receipt.kind = TrustedReceiptKind::SnapshotFailed;
    receipt.effect_kind = RuntimeOwnerEffectKind::FreezeBootSnapshot;
    receipt.correlation_id = command.source.effect.correlation_id;
    receipt.mqtt_session_id = command.source.effect.attempt.mqtt_session_id;
    receipt.mqtt_generation = command.source.effect.attempt.mqtt_generation;
    receipt.config_apply_epoch =
        command.source.effect.attempt.config_apply_epoch;
    receipt.diagnostic_code = diagnostic_code;
    return ingress_result(adapter_.trusted_receipt_port().submit(receipt));
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::commit_end_boot_delivery(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    if (ready_commit_latched_ != 0) {
        if (!exact_committed_end_boot_command(
                command, pending_activation_grant_)) {
            return RuntimeOwnerExecutorResult::RejectedWrongCommand;
        }
        RuntimeSnapshotReadyCommitPermit duplicate = snapshot_.prepare_ready(
            pending_activation_grant_,
            command.source.effect,
            command.source.dispatch_sequence);
        if (duplicate.result() != ReadyPrepareResult::AcceptedDuplicate) {
            return RuntimeOwnerExecutorResult::RejectedSnapshotStore;
        }
        return adapter_.view().boot_end_released != 0
                   ? RuntimeOwnerExecutorResult::AcceptedDuplicate
                   : RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    if (snapshot_frozen_latched_ == 0 ||
        command.kind !=
            RuntimeOwnerDeviceOperationKind::EndBootOrchestration ||
        command.completion_policy != CompletionPolicy::DeliveryOnly ||
        !map_exact_dispatch(adapter_.peek_dispatch(), command)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }

    RuntimeActivationGrant grant = pending_activation_grant_;
    grant.boot_end_correlation_id = command.source.effect.correlation_id;
    grant.boot_end_dispatch_sequence = command.source.dispatch_sequence;
    RuntimeSnapshotReadyCommitPermit ready = snapshot_.prepare_ready(
        grant,
        command.source.effect,
        command.source.dispatch_sequence);
    if (ready.result() != ReadyPrepareResult::Prepared) {
        return RuntimeOwnerExecutorResult::RejectedSnapshotStore;
    }

    const DispatchAckResult acknowledged = adapter_.acknowledge_dispatch(
        command.source.dispatch_sequence);
    if (acknowledged != DispatchAckResult::AcceptedDelivery) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }

    snapshot_.commit_ready(static_cast<
                           RuntimeSnapshotReadyCommitPermit &&>(ready));
    pending_activation_grant_ = grant;
    ready_commit_latched_ = 1;
    return RuntimeOwnerExecutorResult::Accepted;
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_publish_runtime(
    const RuntimeStatusSnapshotV1 snapshot) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    return snapshot_.publish_runtime(snapshot) ==
                   RuntimeSnapshotPublishResult::Accepted
               ? RuntimeOwnerExecutorResult::Accepted
               : RuntimeOwnerExecutorResult::RejectedSnapshotStore;
}

RuntimeOwnerExecutorResult RuntimeOwnerTaskCore::executor_normal_result(
    const RuntimeOwnerExecutorCommand command,
    const NormalCompletionKind kind,
    const std::uint32_t diagnostic_code) noexcept
{
    const RuntimeOwnerExecutorResult gate = executor_state_gate();
    if (gate != RuntimeOwnerExecutorResult::Accepted) {
        return gate;
    }
    const AdapterDispatch inflight = adapter_.view().physical_inflight;
    const bool known_kind =
        kind == NormalCompletionKind::Succeeded ||
        kind == NormalCompletionKind::Failed ||
        kind == NormalCompletionKind::TimedOut ||
        kind == NormalCompletionKind::Cancelled;
    if (!known_kind ||
        command.completion_policy != CompletionPolicy::NormalCompletion ||
        !map_exact_dispatch(inflight, command) ||
        (kind == NormalCompletionKind::Succeeded && diagnostic_code != 0)) {
        return RuntimeOwnerExecutorResult::RejectedWrongCommand;
    }
    NormalCompletion completion{};
    completion.kind = kind;
    completion.dispatch_sequence = command.source.dispatch_sequence;
    completion.enqueue_sequence = command.source.enqueue_sequence;
    completion.diagnostic_code = diagnostic_code;
    return ingress_result(
        adapter_.normal_completion_port().submit(completion));
}

RuntimeOwnerShutdownRequestResult RuntimeOwnerTaskCore::request_shutdown(
    const ShutdownProducer producer,
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    const std::uint8_t producer_value = static_cast<std::uint8_t>(producer);
    if (producer_value == 0 || producer_value > 3 ||
        producer_sequence == 0 || incident_correlation_id == 0) {
        return RuntimeOwnerShutdownRequestResult::RejectedInvalid;
    }
    const std::uint8_t index =
        static_cast<std::uint8_t>(producer_value - 1);
    if (shutdown_provenance_valid_ != 0 &&
        shutdown_producer_ == producer &&
        shutdown_producer_sequence_[index] == producer_sequence &&
        shutdown_incident_correlation_id_ == incident_correlation_id) {
        return RuntimeOwnerShutdownRequestResult::AcceptedDuplicate;
    }
    if (producer_sequence <= shutdown_producer_sequence_[index]) {
        return RuntimeOwnerShutdownRequestResult::RejectedStale;
    }
    if (shutdown_provenance_valid_ != 0 ||
        state_ == RuntimeOwnerTaskState::Terminal) {
        return RuntimeOwnerShutdownRequestResult::RejectedTerminal;
    }
    if (state_ != RuntimeOwnerTaskState::Active) {
        return RuntimeOwnerShutdownRequestResult::RejectedInactive;
    }

    const UrgentRequestResult accepted = adapter_.shutdown_port().request();
    if (accepted != UrgentRequestResult::Accepted) {
        return accepted == UrgentRequestResult::AcceptedDuplicate
                   ? RuntimeOwnerShutdownRequestResult::AcceptedDuplicate
                   : RuntimeOwnerShutdownRequestResult::RejectedTerminal;
    }

    shutdown_producer_sequence_[index] = producer_sequence;
    shutdown_incident_correlation_id_ = incident_correlation_id;
    shutdown_producer_ = producer;
    shutdown_provenance_valid_ = 1;
    return RuntimeOwnerShutdownRequestResult::Accepted;
}

bool RuntimeOwnerTaskCore::shutdown_invariant_holds() const noexcept
{
    const RuntimeOwnerAdapterView view = adapter_.view();
    const bool adapter_latched =
        view.shutdown_pending != 0 ||
        view.shutdown_terminal_override_latched != 0 ||
        view.core.phase == RuntimeOwnerPhase::ShutdownCommitted;
    return adapter_latched == (shutdown_provenance_valid_ != 0);
}

void RuntimeOwnerTaskCore::arm_runtime_admission_for_cycle() noexcept
{
    if (runtime_admission_open_ != 0 || ready_commit_latched_ == 0 ||
        shutdown_provenance_valid_ != 0) {
        return;
    }
    RuntimeActivationGrant grant{};
    const RuntimeOwnerAdapterView view = adapter_.view();
    if (view.boot_end_released != 0 &&
        view.core.boot_orchestration_ended &&
        snapshot_.copy_activation_grant(grant) == SnapshotCopyResult::Copied &&
        runtime_activation_grants_equal(grant, pending_activation_grant_)) {
        runtime_admission_open_ = 1;
    }
}

RuntimeOwnerTaskCycleResult RuntimeOwnerTaskCore::process_cycle(
    const RuntimeOwnerTaskCycleInput input) noexcept
{
    RuntimeOwnerTaskCycleResult result{};
    if (state_ == RuntimeOwnerTaskState::Dormant) {
        return result;
    }
    if (state_ == RuntimeOwnerTaskState::Terminal) {
        result.disposition =
            RuntimeOwnerTaskCycleDisposition::RejectedTerminal;
        return result;
    }

    const bool source_only_reserved_is_canonical =
        input.reserved_source_only == 0 && input.reserved == 0;
    const bool transport_selected =
        source_only_reserved_is_canonical &&
        shutdown_provenance_valid_ == 0 && input.transport_pending != 0;
    const bool normal_selected =
        source_only_reserved_is_canonical &&
        shutdown_provenance_valid_ == 0 && input.transport_pending == 0 &&
        input.normal_pending != 0;
    const bool selected_normal_is_invalid =
        normal_selected &&
        !runtime_owner_normal_intent_is_canonical(input.normal);

    if (!selected_normal_is_invalid) {
        arm_runtime_admission_for_cycle();
    }
    result.disposition = RuntimeOwnerTaskCycleDisposition::Processed;
    if (transport_selected) {
        result.selected_work =
            RuntimeOwnerTaskWorkKind::RequestTransportAttempt;
        result.transport_result = adapter_.request_transport_attempt();
    } else if (normal_selected) {
        result.selected_work = RuntimeOwnerTaskWorkKind::NormalIntent;
        result.normal_result = selected_normal_is_invalid
                                   ? NormalSubmitResult::RejectedInvalid
                                   : runtime_admission_open_ != 0
                                         ? adapter_.normal_port().submit(input.normal)
                                         : NormalSubmitResult::RejectedNotReady;
    }

    result.step_result = adapter_.step();
    result.urgent_recheck_required = 1;
    result.dispatch_pending =
        adapter_.peek_dispatch().kind == AdapterDispatchKind::None ? 0 : 1;
    if (result.step_result.action == AdapterStepAction::Terminal ||
        adapter_.view().core.phase == RuntimeOwnerPhase::ShutdownCommitted) {
        state_ = RuntimeOwnerTaskState::Terminal;
    }
    last_result_ = result;
    return result;
}

RuntimeOwnerRedactedStatus RuntimeOwnerTaskCore::redacted_status() const
    noexcept
{
    const RuntimeOwnerAdapterView view = adapter_.view();
    RuntimeActivationGrant grant{};
    const bool snapshot_ready =
        snapshot_.copy_activation_grant(grant) == SnapshotCopyResult::Copied &&
        runtime_activation_grants_equal(grant, pending_activation_grant_);
    RuntimeOwnerRedactedStatus status{};
    status.state = state_;
    status.phase = view.core.phase;
    status.runtime_ready = static_cast<std::uint8_t>(
        ready_commit_latched_ != 0 && snapshot_ready &&
        view.boot_end_released != 0);
    status.shutdown_latched = shutdown_provenance_valid_;
    status.normal_cancelled_count = view.normal_cancelled_count;
    status.effect_cancelled_count = view.effect_cancelled_count;
    return status;
}

#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
RuntimeOwnerTaskCycleResult RuntimeOwnerTaskCoreTestPeer::process_cycle(
    RuntimeOwnerTaskCore &core,
    const RuntimeOwnerTaskCycleInput input) noexcept
{
    return core.process_cycle(input);
}

void RuntimeOwnerTaskCoreTestPeer::fixture_activate(
    RuntimeOwnerTaskCore &core) noexcept
{
    (void)fixture_activate(core, 1);
}

RuntimeOwnerTaskActivationResult RuntimeOwnerTaskCoreTestPeer::fixture_activate(
    RuntimeOwnerTaskCore &core,
    const std::uint32_t stable_identity) noexcept
{
    if (stable_identity == 0) {
        return RuntimeOwnerTaskActivationResult::RejectedInvalid;
    }
    const RuntimeOwnerCutoverPermit permit{stable_identity};
    return core.activate(permit);
}

void RuntimeOwnerTaskCoreTestPeer::fixture_terminal(
    RuntimeOwnerTaskCore &core) noexcept
{
    core.state_ = RuntimeOwnerTaskState::Terminal;
}

RuntimeOwnerTaskState RuntimeOwnerTaskCoreTestPeer::state(
    const RuntimeOwnerTaskCore &core) noexcept
{
    return core.state_;
}

bool RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(
    const RuntimeOwnerTaskCore &core) noexcept
{
    return core.runtime_admission_open_ != 0;
}

RuntimeOwnerAdapterView RuntimeOwnerTaskCoreTestPeer::adapter_view(
    const RuntimeOwnerTaskCore &core) noexcept
{
    return core.adapter_.view();
}

RuntimeOwnerExecutorPort RuntimeOwnerTaskCoreTestPeer::executor_port(
    RuntimeOwnerTaskCore &core) noexcept
{
    return core.executor_port();
}

RuntimeOwnerPowerButtonShutdownPort
RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(
    RuntimeOwnerTaskCore &core) noexcept
{
    return core.power_button_shutdown_port();
}

RuntimeOwnerAdapterLossShutdownPort
RuntimeOwnerTaskCoreTestPeer::adapter_loss_shutdown_port(
    RuntimeOwnerTaskCore &core) noexcept
{
    return core.adapter_loss_shutdown_port();
}

RuntimeOwnerAuthenticatedCommandShutdownPort
RuntimeOwnerTaskCoreTestPeer::authenticated_command_shutdown_port(
    RuntimeOwnerTaskCore &core) noexcept
{
    return core.authenticated_command_shutdown_port();
}

bool RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(
    const RuntimeOwnerTaskCore &core) noexcept
{
    return core.shutdown_invariant_holds();
}
#endif

} // namespace boot_v2
