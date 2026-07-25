#include "runtime_owner_executor_contract.hpp"

namespace boot_v2 {

namespace {

constexpr bool token_is_zero(const LivenessAttemptToken token) noexcept
{
    return token.mqtt_session_id == 0 && token.mqtt_generation == 0 &&
           token.config_apply_epoch == 0;
}

constexpr bool effect_is_zero(const RuntimeOwnerEffect effect) noexcept
{
    return effect.kind == RuntimeOwnerEffectKind::None &&
           effect.correlation_id == 0 && token_is_zero(effect.attempt) &&
           effect.fault_code == RuntimeOwnerFaultCode::None;
}

constexpr bool normal_intent_is_zero(const NormalIntent intent) noexcept
{
    return intent.kind == NormalIntentKind::Invalid && intent.flags == 0 &&
           intent.reserved == 0 && intent.subject_id == 0 &&
           intent.snapshot_revision == 0;
}

constexpr bool fault_code_is_known_nonzero(
    const RuntimeOwnerFaultCode fault) noexcept
{
    switch (fault) {
    case RuntimeOwnerFaultCode::TransportFailure:
    case RuntimeOwnerFaultCode::LivenessFailure:
    case RuntimeOwnerFaultCode::SnapshotFailure:
    case RuntimeOwnerFaultCode::TransportDisconnected:
    case RuntimeOwnerFaultCode::DeadlineExpired:
    case RuntimeOwnerFaultCode::CriticalIngress:
    case RuntimeOwnerFaultCode::CounterSaturation:
    case RuntimeOwnerFaultCode::InternalInvariant:
        return true;
    case RuntimeOwnerFaultCode::None:
    default:
        return false;
    }
}

constexpr bool fault_effect_is_canonical(
    const RuntimeOwnerEffect effect) noexcept
{
    const bool token_valid = effect.attempt.valid();
    return fault_code_is_known_nonzero(effect.fault_code) &&
           (token_is_zero(effect.attempt) || token_valid) &&
           (effect.correlation_id == 0 || token_valid);
}

bool map_effect(
    const RuntimeOwnerEffect effect,
    RuntimeOwnerDeviceOperationKind &kind,
    CompletionPolicy &policy) noexcept
{
    const bool trusted_canonical = effect.correlation_id != 0 &&
                                   effect.attempt.valid() &&
                                   effect.fault_code ==
                                       RuntimeOwnerFaultCode::None;
    switch (effect.kind) {
    case RuntimeOwnerEffectKind::StartTransportAttempt:
        if (effect.correlation_id != 0 ||
            effect.attempt.mqtt_session_id != 0 ||
            effect.attempt.mqtt_generation == 0 ||
            effect.attempt.config_apply_epoch != 0 ||
            effect.fault_code != RuntimeOwnerFaultCode::None) {
            return false;
        }
        kind = RuntimeOwnerDeviceOperationKind::OpenTransport;
        policy = CompletionPolicy::TrustedReceipt;
        return true;
    case RuntimeOwnerEffectKind::StartAtProbe:
        kind = RuntimeOwnerDeviceOperationKind::ProbeAt;
        policy = CompletionPolicy::TrustedReceipt;
        return trusted_canonical;
    case RuntimeOwnerEffectKind::StartProbePublish:
        kind = RuntimeOwnerDeviceOperationKind::PublishProbe;
        policy = CompletionPolicy::TrustedReceipt;
        return trusted_canonical;
    case RuntimeOwnerEffectKind::VerifySubscription:
        kind = RuntimeOwnerDeviceOperationKind::VerifySubscription;
        policy = CompletionPolicy::TrustedReceipt;
        return trusted_canonical;
    case RuntimeOwnerEffectKind::PullFollowupConfig:
        kind = RuntimeOwnerDeviceOperationKind::PullFollowupConfig;
        policy = CompletionPolicy::TrustedReceipt;
        return trusted_canonical;
    case RuntimeOwnerEffectKind::FreezeBootSnapshot:
        kind = RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot;
        policy = CompletionPolicy::TrustedReceipt;
        return trusted_canonical;
    case RuntimeOwnerEffectKind::EndBootOrchestration:
        kind = RuntimeOwnerDeviceOperationKind::EndBootOrchestration;
        policy = CompletionPolicy::DeliveryOnly;
        return trusted_canonical;
    case RuntimeOwnerEffectKind::RecordFault:
        kind = RuntimeOwnerDeviceOperationKind::RecordFault;
        policy = CompletionPolicy::DeliveryOnly;
        return fault_effect_is_canonical(effect);
    case RuntimeOwnerEffectKind::EnterRecovery:
        kind = RuntimeOwnerDeviceOperationKind::EnterRecovery;
        policy = CompletionPolicy::DeliveryOnly;
        return fault_effect_is_canonical(effect);
    case RuntimeOwnerEffectKind::None:
    default:
        return false;
    }
}

bool map_normal_intent(
    const NormalIntent intent,
    RuntimeOwnerDeviceOperationKind &kind) noexcept
{
    if (!runtime_owner_normal_intent_is_canonical(intent)) {
        return false;
    }

    switch (intent.kind) {
    case NormalIntentKind::PublishTelemetry:
        kind = RuntimeOwnerDeviceOperationKind::PublishTelemetry;
        return true;
    case NormalIntentKind::RefreshRssi:
        kind = RuntimeOwnerDeviceOperationKind::RefreshRssi;
        return true;
    case NormalIntentKind::PullConfig:
        kind = RuntimeOwnerDeviceOperationKind::PullConfig;
        return true;
    case NormalIntentKind::PullCommand:
        kind = RuntimeOwnerDeviceOperationKind::PullCommand;
        return true;
    case NormalIntentKind::Invalid:
    default:
        return false;
    }
}

} // namespace

RuntimeOwnerExecutorMapResult map_runtime_owner_dispatch(
    const AdapterDispatch dispatch,
    RuntimeOwnerExecutorCommand &command) noexcept
{
    command = {};
    if (dispatch.reserved[0] != 0 || dispatch.reserved[1] != 0 ||
        dispatch.reserved[2] != 0 ||
        dispatch.dispatch_sequence == 0) {
        return RuntimeOwnerExecutorMapResult::RejectedInvalid;
    }

    RuntimeOwnerDeviceOperationKind kind{
        RuntimeOwnerDeviceOperationKind::Invalid};
    CompletionPolicy policy{CompletionPolicy::Invalid};
    switch (dispatch.kind) {
    case AdapterDispatchKind::CoreEffect:
        if (dispatch.enqueue_sequence != 0 ||
            !normal_intent_is_zero(dispatch.normal_intent) ||
            !map_effect(dispatch.effect, kind, policy)) {
            return RuntimeOwnerExecutorMapResult::RejectedInvalid;
        }
        break;
    case AdapterDispatchKind::NormalIntent:
        if (dispatch.enqueue_sequence == 0 || !effect_is_zero(dispatch.effect) ||
            !map_normal_intent(dispatch.normal_intent, kind)) {
            return RuntimeOwnerExecutorMapResult::RejectedInvalid;
        }
        policy = CompletionPolicy::NormalCompletion;
        break;
    case AdapterDispatchKind::None:
    default:
        return RuntimeOwnerExecutorMapResult::RejectedInvalid;
    }

    command.kind = kind;
    command.source = dispatch;
    command.completion_policy = policy;
    return RuntimeOwnerExecutorMapResult::Mapped;
}

} // namespace boot_v2
