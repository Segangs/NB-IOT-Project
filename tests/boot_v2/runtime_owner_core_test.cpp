#include "runtime_owner_core.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace boot_v2 {

#if !defined(NB_IOT_RUNTIME_OWNER_TESTING)
#error "runtime_owner_core_test requires NB_IOT_RUNTIME_OWNER_TESTING"
#endif

struct RuntimeOwnerPrivateSnapshot {
    RuntimeOwnerPhase phase{};
    RuntimeOwnerFaultCode last_fault{};
    std::uint32_t mqtt_generation_counter{0};
    std::uint32_t active_mqtt_session_id{0};
    std::uint32_t active_mqtt_generation{0};
    std::uint32_t config_apply_epoch_counter{0};
    std::uint32_t last_config_commit_sequence{0};
    std::uint32_t correlation_id_counter{0};
    LivenessAttemptToken active_attempt{};
    std::array<RuntimeOwnerEffect, 4> active_tickets{};
    std::uint8_t accepted_liveness_mask{0};
    std::uint32_t pending_snapshot_effect_id{0};
    std::uint32_t pending_boot_end_effect_id{0};
    bool boot_orchestration_ended{false};
    bool fatal_latched{false};
    bool has_last_failure{false};
    RuntimeOwnerInput last_failure{};
    LivenessGateStatus boundary_status{LivenessGateStatus::NotStarted};
    LivenessAttemptToken boundary_active_attempt{};
};

class RuntimeOwnerCoreTestPeer {
public:
    static void seed_counters(
        RuntimeOwnerCore &core,
        const std::uint32_t mqtt_generation,
        const std::uint32_t config_apply_epoch,
        const std::uint32_t correlation_id,
        const std::uint32_t config_commit_sequence) noexcept
    {
        core.mqtt_generation_counter_ = mqtt_generation;
        core.config_apply_epoch_counter_ = config_apply_epoch;
        core.correlation_id_counter_ = correlation_id;
        core.last_config_commit_sequence_ = config_commit_sequence;
    }

    [[nodiscard]] static RuntimeOwnerPrivateSnapshot snapshot(
        const RuntimeOwnerCore &core) noexcept
    {
        return {
            core.phase_,
            core.last_fault_,
            core.mqtt_generation_counter_,
            core.active_mqtt_session_id_,
            core.active_mqtt_generation_,
            core.config_apply_epoch_counter_,
            core.last_config_commit_sequence_,
            core.correlation_id_counter_,
            core.active_attempt_,
            core.active_tickets_,
            core.accepted_liveness_mask_,
            core.pending_snapshot_effect_id_,
            core.pending_boot_end_effect_id_,
            core.boot_orchestration_ended_,
            core.fatal_latched_,
            core.has_last_failure_,
            core.last_failure_,
            core.liveness_boundary_.status(),
            core.liveness_boundary_.active_attempt(),
        };
    }
};

} // namespace boot_v2

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

using boot_v2::LivenessAttemptToken;
using boot_v2::RuntimeOwnerCore;
using boot_v2::RuntimeOwnerCoreTestPeer;
using boot_v2::RuntimeOwnerEffect;
using boot_v2::RuntimeOwnerEffectKind;
using boot_v2::RuntimeOwnerFaultCode;
using boot_v2::RuntimeOwnerInput;
using boot_v2::RuntimeOwnerInputKind;
using boot_v2::RuntimeOwnerPhase;
using boot_v2::RuntimeOwnerPrivateSnapshot;
using boot_v2::RuntimeOwnerTransition;
using boot_v2::RuntimeOwnerDisposition;
using boot_v2::RuntimeOwnerView;

template <typename Type, typename = void>
struct HasPublicBoundary : std::false_type {
};

template <typename Type>
struct HasPublicBoundary<Type, std::void_t<decltype(
    std::declval<const Type &>().boundary())>> : std::true_type {
};

template <typename Type, typename = void>
struct HasPublicLivenessBoundary : std::false_type {
};

template <typename Type>
struct HasPublicLivenessBoundary<Type, std::void_t<decltype(
    std::declval<const Type &>().liveness_boundary())>> : std::true_type {
};

template <typename Type, typename = void>
struct HasPublicTickets : std::false_type {
};

template <typename Type>
struct HasPublicTickets<Type, std::void_t<decltype(
    std::declval<const Type &>().tickets())>> : std::true_type {
};

template <typename Type, typename = void>
struct HasPublicSeedCounters : std::false_type {
};

template <typename Type>
struct HasPublicSeedCounters<Type, std::void_t<decltype(
    std::declval<Type &>().seed_counters(
        std::uint32_t{}, std::uint32_t{}, std::uint32_t{}, std::uint32_t{}))>>
    : std::true_type {
};

template <typename Type, typename Command, typename = void>
struct HasPublicSubmitFor : std::false_type {
};

template <typename Type, typename Command>
struct HasPublicSubmitFor<Type, Command, std::void_t<decltype(
    std::declval<Type &>().submit(std::declval<Command>()))>> : std::true_type {
};

constexpr std::array<RuntimeOwnerEffectKind, 4> kLivenessKinds{
    RuntimeOwnerEffectKind::StartAtProbe,
    RuntimeOwnerEffectKind::StartProbePublish,
    RuntimeOwnerEffectKind::VerifySubscription,
    RuntimeOwnerEffectKind::PullFollowupConfig,
};

enum InputFieldMask : std::uint8_t {
    kInputEffect = 0x01,
    kInputCorrelation = 0x02,
    kInputSession = 0x04,
    kInputGeneration = 0x08,
    kInputCommit = 0x10,
    kInputEpoch = 0x20,
};

constexpr std::array<std::uint8_t, 6> kInputFieldBits{
    kInputEffect,
    kInputCorrelation,
    kInputSession,
    kInputGeneration,
    kInputCommit,
    kInputEpoch,
};

constexpr RuntimeOwnerInput make_input(
    const RuntimeOwnerInputKind kind,
    const RuntimeOwnerEffectKind receipt_kind = RuntimeOwnerEffectKind::None,
    const std::uint32_t correlation_id = 0,
    const std::uint32_t mqtt_session_id = 0,
    const std::uint32_t mqtt_generation = 0,
    const std::uint32_t config_commit_sequence = 0,
    const std::uint32_t config_apply_epoch = 0) noexcept
{
    return {
        kind,
        receipt_kind,
        correlation_id,
        mqtt_session_id,
        mqtt_generation,
        config_commit_sequence,
        config_apply_epoch,
    };
}

constexpr bool is_zero_token(const LivenessAttemptToken token) noexcept
{
    return token == LivenessAttemptToken{};
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

constexpr bool inputs_equal(
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

bool private_snapshots_equal(
    const RuntimeOwnerPrivateSnapshot &left,
    const RuntimeOwnerPrivateSnapshot &right)
{
    if (left.phase != right.phase ||
        left.last_fault != right.last_fault ||
        left.mqtt_generation_counter != right.mqtt_generation_counter ||
        left.active_mqtt_session_id != right.active_mqtt_session_id ||
        left.active_mqtt_generation != right.active_mqtt_generation ||
        left.config_apply_epoch_counter != right.config_apply_epoch_counter ||
        left.last_config_commit_sequence !=
            right.last_config_commit_sequence ||
        left.correlation_id_counter != right.correlation_id_counter ||
        left.active_attempt != right.active_attempt ||
        left.accepted_liveness_mask != right.accepted_liveness_mask ||
        left.pending_snapshot_effect_id !=
            right.pending_snapshot_effect_id ||
        left.pending_boot_end_effect_id != right.pending_boot_end_effect_id ||
        left.boot_orchestration_ended != right.boot_orchestration_ended ||
        left.fatal_latched != right.fatal_latched ||
        left.has_last_failure != right.has_last_failure ||
        !inputs_equal(left.last_failure, right.last_failure) ||
        left.boundary_status != right.boundary_status ||
        left.boundary_active_attempt != right.boundary_active_attempt) {
        return false;
    }
    for (std::size_t index = 0; index < left.active_tickets.size(); ++index) {
        if (!effects_equal(
                left.active_tickets[index], right.active_tickets[index])) {
            return false;
        }
    }
    return true;
}

void check_zero_effect(const RuntimeOwnerEffect effect)
{
    CHECK(effect.kind == RuntimeOwnerEffectKind::None);
    CHECK(effect.correlation_id == 0);
    CHECK(is_zero_token(effect.attempt));
    CHECK(effect.fault_code == RuntimeOwnerFaultCode::None);
}

void check_effect_canonical(const RuntimeOwnerEffect effect)
{
    switch (effect.kind) {
    case RuntimeOwnerEffectKind::None:
        check_zero_effect(effect);
        break;
    case RuntimeOwnerEffectKind::StartTransportAttempt:
        CHECK(effect.correlation_id == 0);
        CHECK(effect.attempt.mqtt_session_id == 0);
        CHECK(effect.attempt.mqtt_generation != 0);
        CHECK(effect.attempt.config_apply_epoch == 0);
        CHECK(effect.fault_code == RuntimeOwnerFaultCode::None);
        break;
    case RuntimeOwnerEffectKind::StartAtProbe:
    case RuntimeOwnerEffectKind::StartProbePublish:
    case RuntimeOwnerEffectKind::VerifySubscription:
    case RuntimeOwnerEffectKind::PullFollowupConfig:
    case RuntimeOwnerEffectKind::FreezeBootSnapshot:
    case RuntimeOwnerEffectKind::EndBootOrchestration:
        CHECK(effect.correlation_id != 0);
        CHECK(effect.attempt.valid());
        CHECK(effect.fault_code == RuntimeOwnerFaultCode::None);
        break;
    case RuntimeOwnerEffectKind::RecordFault:
    case RuntimeOwnerEffectKind::EnterRecovery:
        CHECK(effect.fault_code != RuntimeOwnerFaultCode::None);
        CHECK(is_zero_token(effect.attempt) || effect.attempt.valid());
        break;
    default:
        CHECK(false);
        break;
    }
}

void check_transition_shape(const RuntimeOwnerTransition &transition)
{
    CHECK(transition.effect_count <= transition.effects.size());
    for (std::size_t index = transition.effect_count;
         index < transition.effects.size();
         ++index) {
        check_zero_effect(transition.effects[index]);
    }
    for (std::size_t index = 0; index < transition.effect_count; ++index) {
        CHECK(transition.effects[index].kind != RuntimeOwnerEffectKind::None);
        check_effect_canonical(transition.effects[index]);
    }
}

RuntimeOwnerTransition submit_checked(
    RuntimeOwnerCore &core,
    const RuntimeOwnerInput input)
{
    const RuntimeOwnerView before = core.view();
    const RuntimeOwnerPrivateSnapshot private_before =
        RuntimeOwnerCoreTestPeer::snapshot(core);
    const RuntimeOwnerTransition transition = core.submit(input);
    const RuntimeOwnerView after = core.view();
    const RuntimeOwnerPrivateSnapshot private_after =
        RuntimeOwnerCoreTestPeer::snapshot(core);
    CHECK(transition.phase_before == before.phase);
    CHECK(transition.phase_after == after.phase);
    check_transition_shape(transition);
    if (transition.disposition == RuntimeOwnerDisposition::Rejected ||
        transition.disposition == RuntimeOwnerDisposition::AcceptedDuplicate) {
        CHECK(transition.effect_count == 0);
        CHECK(transition.phase_before == transition.phase_after);
        CHECK(private_snapshots_equal(private_before, private_after));
    } else if (transition.disposition == RuntimeOwnerDisposition::FailClosed) {
        CHECK(transition.phase_after == RuntimeOwnerPhase::RecoveryPending);
        CHECK(transition.effect_count == 2);
        CHECK(transition.effects[0].kind ==
              RuntimeOwnerEffectKind::RecordFault);
        CHECK(transition.effects[1].kind ==
              RuntimeOwnerEffectKind::EnterRecovery);
        CHECK(transition.effects[0].fault_code != RuntimeOwnerFaultCode::None);
        CHECK(transition.effects[0].fault_code ==
              transition.effects[1].fault_code);
        CHECK(transition.effects[0].correlation_id ==
              transition.effects[1].correlation_id);
        CHECK(transition.effects[0].attempt ==
              transition.effects[1].attempt);
        CHECK(private_after.phase == RuntimeOwnerPhase::RecoveryPending);
        CHECK(private_after.last_fault == transition.effects[0].fault_code);
        CHECK(private_after.active_mqtt_session_id == 0);
        CHECK(private_after.active_mqtt_generation == 0);
        CHECK(!private_after.active_attempt.valid());
        for (const auto ticket : private_after.active_tickets) {
            CHECK(effects_equal(ticket, RuntimeOwnerEffect{}));
        }
        CHECK(private_after.accepted_liveness_mask == 0);
        CHECK(private_after.pending_snapshot_effect_id == 0);
        CHECK(private_after.pending_boot_end_effect_id == 0);
        CHECK(private_after.boot_orchestration_ended ==
              private_before.boot_orchestration_ended);
        CHECK(private_after.fatal_latched);
        CHECK(!private_after.has_last_failure);
        CHECK(private_after.mqtt_generation_counter ==
              private_before.mqtt_generation_counter);
        CHECK(private_after.config_apply_epoch_counter ==
              private_before.config_apply_epoch_counter);
        CHECK(private_after.last_config_commit_sequence ==
              private_before.last_config_commit_sequence);
        CHECK(private_after.correlation_id_counter ==
              private_before.correlation_id_counter);
        if (private_after.last_fault == RuntimeOwnerFaultCode::CounterSaturation) {
            CHECK(private_after.boundary_status == private_before.boundary_status);
            CHECK(private_after.boundary_active_attempt ==
                  private_before.boundary_active_attempt);
        }
    }
    return transition;
}

void expect_no_state_mutation(
    const RuntimeOwnerView &before,
    const RuntimeOwnerView &after)
{
    CHECK(after.phase == before.phase);
    CHECK(after.mqtt_session_id == before.mqtt_session_id);
    CHECK(after.mqtt_generation == before.mqtt_generation);
    CHECK(after.mqtt_generation_counter == before.mqtt_generation_counter);
    CHECK(after.config_apply_epoch_counter == before.config_apply_epoch_counter);
    CHECK(after.last_config_commit_sequence ==
          before.last_config_commit_sequence);
    CHECK(after.last_correlation_id == before.last_correlation_id);
    CHECK(after.active_attempt == before.active_attempt);
    CHECK(after.boot_orchestration_ended == before.boot_orchestration_ended);
    CHECK(after.last_fault == before.last_fault);
}

RuntimeOwnerTransition begin_transport(RuntimeOwnerCore &core)
{
    const RuntimeOwnerTransition transition = submit_checked(
        core, make_input(RuntimeOwnerInputKind::BeginTransportAttempt));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.phase_before == RuntimeOwnerPhase::ColdStart ||
          transition.phase_before == RuntimeOwnerPhase::RecoveryPending);
    CHECK(transition.phase_after == RuntimeOwnerPhase::TransportConnecting);
    CHECK(transition.effect_count == 1);
    CHECK(transition.effects[0].kind ==
          RuntimeOwnerEffectKind::StartTransportAttempt);
    CHECK(transition.effects[0].attempt.mqtt_session_id == 0);
    CHECK(transition.effects[0].attempt.mqtt_generation ==
          core.view().mqtt_generation_counter);
    CHECK(transition.effects[0].attempt.config_apply_epoch == 0);
    return transition;
}

void establish(RuntimeOwnerCore &core, const std::uint32_t session_id)
{
    const RuntimeOwnerView before = core.view();
    const RuntimeOwnerTransition transition = submit_checked(
        core,
        make_input(
            RuntimeOwnerInputKind::TransportEstablished,
            RuntimeOwnerEffectKind::None,
            0,
            session_id,
            before.mqtt_generation_counter));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.effect_count == 0);
    CHECK(transition.phase_after == RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(core.view().mqtt_session_id == session_id);
    CHECK(core.view().mqtt_generation == before.mqtt_generation_counter);
}

std::array<RuntimeOwnerEffect, 4> commit_config(
    RuntimeOwnerCore &core,
    const std::uint32_t sequence)
{
    const RuntimeOwnerView before = core.view();
    const RuntimeOwnerTransition transition = submit_checked(
        core,
        make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            before.mqtt_session_id,
            before.mqtt_generation,
            sequence,
            0));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.phase_after == RuntimeOwnerPhase::LivenessWaiting);
    CHECK(transition.effect_count == 4);
    CHECK(core.view().active_attempt.valid());
    CHECK(core.view().active_attempt.mqtt_session_id == before.mqtt_session_id);
    CHECK(core.view().active_attempt.mqtt_generation == before.mqtt_generation);
    CHECK(core.view().active_attempt.config_apply_epoch ==
          before.config_apply_epoch_counter + 1);
    CHECK(core.view().last_config_commit_sequence == sequence);
    std::array<RuntimeOwnerEffect, 4> effects{};
    for (std::size_t index = 0; index < effects.size(); ++index) {
        effects[index] = transition.effects[index];
        CHECK(effects[index].kind == kLivenessKinds[index]);
        CHECK(effects[index].correlation_id != 0);
        CHECK(effects[index].attempt == core.view().active_attempt);
        if (index != 0) {
            CHECK(effects[index].correlation_id ==
                  effects[index - 1].correlation_id + 1);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            CHECK(effects[index].correlation_id != effects[prior].correlation_id);
        }
    }
    const RuntimeOwnerPrivateSnapshot private_state =
        RuntimeOwnerCoreTestPeer::snapshot(core);
    CHECK(private_state.pending_snapshot_effect_id ==
          effects.back().correlation_id + 1);
    CHECK(private_state.pending_boot_end_effect_id ==
          effects.back().correlation_id + 2);
    CHECK(private_state.correlation_id_counter ==
          private_state.pending_boot_end_effect_id);
    return effects;
}

std::array<RuntimeOwnerEffect, 4> prepare_liveness(
    RuntimeOwnerCore &core,
    const std::uint32_t session_id = 7,
    const std::uint32_t commit_sequence = 1)
{
    begin_transport(core);
    establish(core, session_id);
    return commit_config(core, commit_sequence);
}

RuntimeOwnerInput completion_input(const RuntimeOwnerEffect ticket) noexcept
{
    return make_input(
        RuntimeOwnerInputKind::LivenessOperationCompleted,
        ticket.kind,
        ticket.correlation_id,
        ticket.attempt.mqtt_session_id,
        ticket.attempt.mqtt_generation,
        0,
        ticket.attempt.config_apply_epoch);
}

RuntimeOwnerInput operation_failure_input(
    const RuntimeOwnerInputKind kind,
    const RuntimeOwnerEffect ticket) noexcept
{
    return make_input(
        kind,
        ticket.kind,
        ticket.correlation_id,
        ticket.attempt.mqtt_session_id,
        ticket.attempt.mqtt_generation,
        0,
        ticket.attempt.config_apply_epoch);
}

RuntimeOwnerEffect complete_all(
    RuntimeOwnerCore &core,
    const std::array<RuntimeOwnerEffect, 4> &tickets)
{
    RuntimeOwnerEffect freeze{};
    for (std::size_t index = 0; index < tickets.size(); ++index) {
        const RuntimeOwnerTransition transition = submit_checked(
            core, completion_input(tickets[index]));
        CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
        if (index + 1 == tickets.size()) {
            CHECK(transition.phase_after ==
                  RuntimeOwnerPhase::SnapshotFreezePending);
            CHECK(transition.effect_count == 1);
            CHECK(transition.effects[0].kind ==
                  RuntimeOwnerEffectKind::FreezeBootSnapshot);
            CHECK(transition.effects[0].attempt == tickets[index].attempt);
            freeze = transition.effects[0];
        } else {
            CHECK(transition.phase_after == RuntimeOwnerPhase::LivenessWaiting);
            CHECK(transition.effect_count == 0);
        }
    }
    CHECK(freeze.correlation_id != 0);
    return freeze;
}

RuntimeOwnerInput snapshot_input(
    const RuntimeOwnerInputKind kind,
    const RuntimeOwnerEffect freeze) noexcept
{
    return make_input(
        kind,
        RuntimeOwnerEffectKind::FreezeBootSnapshot,
        freeze.correlation_id,
        freeze.attempt.mqtt_session_id,
        freeze.attempt.mqtt_generation,
        0,
        freeze.attempt.config_apply_epoch);
}

RuntimeOwnerInput with_nonzero_field(
    RuntimeOwnerInput input,
    const std::uint8_t field)
{
    switch (field) {
    case kInputEffect:
        input.receipt_kind = RuntimeOwnerEffectKind::StartAtProbe;
        break;
    case kInputCorrelation:
        input.correlation_id = 1;
        break;
    case kInputSession:
        input.mqtt_session_id = 1;
        break;
    case kInputGeneration:
        input.mqtt_generation = 1;
        break;
    case kInputCommit:
        input.config_commit_sequence = 1;
        break;
    case kInputEpoch:
        input.config_apply_epoch = 1;
        break;
    default:
        CHECK(false);
        break;
    }
    return input;
}

RuntimeOwnerInput with_zero_field(
    RuntimeOwnerInput input,
    const std::uint8_t field)
{
    switch (field) {
    case kInputEffect:
        input.receipt_kind = RuntimeOwnerEffectKind::None;
        break;
    case kInputCorrelation:
        input.correlation_id = 0;
        break;
    case kInputSession:
        input.mqtt_session_id = 0;
        break;
    case kInputGeneration:
        input.mqtt_generation = 0;
        break;
    case kInputCommit:
        input.config_commit_sequence = 0;
        break;
    case kInputEpoch:
        input.config_apply_epoch = 0;
        break;
    default:
        CHECK(false);
        break;
    }
    return input;
}

void check_input_field_matrix(
    RuntimeOwnerCore &core,
    const RuntimeOwnerInput valid,
    const std::uint8_t unused_fields,
    const std::uint8_t required_nonzero_fields)
{
    for (const std::uint8_t field : kInputFieldBits) {
        if ((unused_fields & field) != 0) {
            const RuntimeOwnerTransition rejected = submit_checked(
                core, with_nonzero_field(valid, field));
            CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
        }
        if ((required_nonzero_fields & field) != 0) {
            const RuntimeOwnerTransition rejected = submit_checked(
                core, with_zero_field(valid, field));
            CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
        }
    }
}

RuntimeOwnerEffect finish_snapshot(
    RuntimeOwnerCore &core,
    const RuntimeOwnerEffect freeze)
{
    const RuntimeOwnerTransition transition = submit_checked(
        core,
        snapshot_input(RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.phase_after == RuntimeOwnerPhase::RuntimeReady);
    CHECK(transition.effect_count == 1);
    CHECK(transition.effects[0].kind ==
          RuntimeOwnerEffectKind::EndBootOrchestration);
    CHECK(transition.effects[0].attempt == freeze.attempt);
    CHECK(core.view().boot_orchestration_ended);
    return transition.effects[0];
}

void reach_phase(
    RuntimeOwnerCore &core,
    const RuntimeOwnerPhase target,
    std::array<RuntimeOwnerEffect, 4> &tickets,
    RuntimeOwnerEffect &freeze)
{
    if (target == RuntimeOwnerPhase::ColdStart) {
        return;
    }
    begin_transport(core);
    if (target == RuntimeOwnerPhase::TransportConnecting) {
        return;
    }
    establish(core, 7);
    if (target == RuntimeOwnerPhase::AwaitingConfigCommit) {
        return;
    }
    tickets = commit_config(core, 1);
    if (target == RuntimeOwnerPhase::LivenessWaiting) {
        return;
    }
    freeze = complete_all(core, tickets);
    if (target == RuntimeOwnerPhase::SnapshotFreezePending) {
        return;
    }
    CHECK(target == RuntimeOwnerPhase::RuntimeReady);
    finish_snapshot(core, freeze);
}

void test_type_contract()
{
    CHECK(std::is_same_v<
          std::underlying_type_t<RuntimeOwnerPhase>, std::uint8_t>);
    CHECK(std::is_same_v<
          std::underlying_type_t<RuntimeOwnerInputKind>, std::uint8_t>);
    CHECK(std::is_same_v<
          std::underlying_type_t<RuntimeOwnerEffectKind>, std::uint8_t>);
    CHECK(std::is_same_v<
          std::underlying_type_t<RuntimeOwnerDisposition>, std::uint8_t>);
    CHECK(std::is_same_v<
          std::underlying_type_t<RuntimeOwnerFaultCode>, std::uint8_t>);
    constexpr std::array<RuntimeOwnerPhase, 8> phases{
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RuntimeReady,
        RuntimeOwnerPhase::RecoveryPending,
        RuntimeOwnerPhase::ShutdownCommitted,
    };
    for (std::size_t index = 0; index < phases.size(); ++index) {
        CHECK(static_cast<std::uint8_t>(phases[index]) == index);
    }
    constexpr std::array<RuntimeOwnerInputKind, 13> input_kinds{
        RuntimeOwnerInputKind::Invalid,
        RuntimeOwnerInputKind::BeginTransportAttempt,
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerInputKind::TransportAttemptFailed,
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerInputKind::LivenessOperationCompleted,
        RuntimeOwnerInputKind::LivenessOperationFailed,
        RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
        RuntimeOwnerInputKind::SnapshotFreezeFailed,
        RuntimeOwnerInputKind::TransportDisconnected,
        RuntimeOwnerInputKind::DeadlineExpired,
        RuntimeOwnerInputKind::CriticalIngressFault,
        RuntimeOwnerInputKind::ShutdownCommitted,
    };
    for (std::size_t index = 0; index < input_kinds.size(); ++index) {
        CHECK(static_cast<std::uint8_t>(input_kinds[index]) == index);
    }
    constexpr std::array<RuntimeOwnerEffectKind, 10> effect_kinds{
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
    };
    for (std::size_t index = 0; index < effect_kinds.size(); ++index) {
        CHECK(static_cast<std::uint8_t>(effect_kinds[index]) == index);
    }
    constexpr std::array<RuntimeOwnerDisposition, 4> dispositions{
        RuntimeOwnerDisposition::Rejected,
        RuntimeOwnerDisposition::Accepted,
        RuntimeOwnerDisposition::AcceptedDuplicate,
        RuntimeOwnerDisposition::FailClosed,
    };
    for (std::size_t index = 0; index < dispositions.size(); ++index) {
        CHECK(static_cast<std::uint8_t>(dispositions[index]) == index);
    }
    constexpr std::array<RuntimeOwnerFaultCode, 9> faults{
        RuntimeOwnerFaultCode::None,
        RuntimeOwnerFaultCode::TransportFailure,
        RuntimeOwnerFaultCode::LivenessFailure,
        RuntimeOwnerFaultCode::SnapshotFailure,
        RuntimeOwnerFaultCode::TransportDisconnected,
        RuntimeOwnerFaultCode::DeadlineExpired,
        RuntimeOwnerFaultCode::CriticalIngress,
        RuntimeOwnerFaultCode::CounterSaturation,
        RuntimeOwnerFaultCode::InternalInvariant,
    };
    for (std::size_t index = 0; index < faults.size(); ++index) {
        CHECK(static_cast<std::uint8_t>(faults[index]) == index);
    }
    CHECK(std::is_standard_layout_v<RuntimeOwnerInput>);
    CHECK(std::is_trivially_copyable_v<RuntimeOwnerInput>);
    CHECK(std::is_standard_layout_v<RuntimeOwnerEffect>);
    CHECK(std::is_trivially_copyable_v<RuntimeOwnerEffect>);
    CHECK(std::is_standard_layout_v<RuntimeOwnerTransition>);
    CHECK(std::is_trivially_copyable_v<RuntimeOwnerTransition>);
    CHECK(std::is_standard_layout_v<RuntimeOwnerView>);
    CHECK(std::is_trivially_copyable_v<RuntimeOwnerView>);
    CHECK(!std::is_copy_constructible_v<RuntimeOwnerCore>);
    CHECK(!std::is_copy_assignable_v<RuntimeOwnerCore>);
    CHECK(!std::is_move_constructible_v<RuntimeOwnerCore>);
    CHECK(!std::is_move_assignable_v<RuntimeOwnerCore>);
    CHECK(std::is_nothrow_default_constructible_v<RuntimeOwnerCore>);
    CHECK(std::is_nothrow_destructible_v<RuntimeOwnerCore>);
    CHECK(!std::is_polymorphic_v<RuntimeOwnerCore>);
    CHECK(!HasPublicBoundary<RuntimeOwnerCore>::value);
    CHECK(!HasPublicLivenessBoundary<RuntimeOwnerCore>::value);
    CHECK(!HasPublicTickets<RuntimeOwnerCore>::value);
    CHECK(!HasPublicSeedCounters<RuntimeOwnerCore>::value);
    CHECK(HasPublicSubmitFor<RuntimeOwnerCore, RuntimeOwnerInput>::value);
    CHECK(!HasPublicSubmitFor<
          RuntimeOwnerCore, boot_v2::LivenessServiceCommand>::value);
    CHECK(std::is_same_v<
          decltype(std::declval<RuntimeOwnerCore &>().submit(
              RuntimeOwnerInput{})),
          RuntimeOwnerTransition>);
    CHECK(std::is_same_v<
          decltype(std::declval<const RuntimeOwnerCore &>().view()),
          RuntimeOwnerView>);
    CHECK(std::is_same_v<
          decltype(RuntimeOwnerTransition{}.effect_count), std::uint8_t>);
    CHECK(noexcept(std::declval<RuntimeOwnerCore &>().submit(RuntimeOwnerInput{})));
    CHECK(noexcept(std::declval<const RuntimeOwnerCore &>().view()));
    CHECK(std::tuple_size<decltype(RuntimeOwnerTransition{}.effects)>::value == 4);
}

void test_safe_defaults_unknown_enums_and_canonical_fields()
{
    RuntimeOwnerCore core;
    const RuntimeOwnerView initial = core.view();
    CHECK(initial.phase == RuntimeOwnerPhase::ColdStart);
    CHECK(initial.mqtt_session_id == 0);
    CHECK(initial.mqtt_generation == 0);
    CHECK(initial.mqtt_generation_counter == 0);
    CHECK(initial.config_apply_epoch_counter == 0);
    CHECK(initial.last_config_commit_sequence == 0);
    CHECK(initial.last_correlation_id == 0);
    CHECK(!initial.active_attempt.valid());
    CHECK(!initial.boot_orchestration_ended);
    CHECK(initial.last_fault == RuntimeOwnerFaultCode::None);

    RuntimeOwnerTransition transition = submit_checked(core, RuntimeOwnerInput{});
    CHECK(transition.disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(transition.effect_count == 0);
    expect_no_state_mutation(initial, core.view());

    for (std::uint16_t value = 13; value <= 255; ++value) {
        transition = submit_checked(core, make_input(
            static_cast<RuntimeOwnerInputKind>(value)));
        CHECK(transition.disposition == RuntimeOwnerDisposition::Rejected);
    }
    for (std::uint16_t value = 10; value <= 255; ++value) {
        transition = submit_checked(core, make_input(
            RuntimeOwnerInputKind::BeginTransportAttempt,
            static_cast<RuntimeOwnerEffectKind>(value)));
        CHECK(transition.disposition == RuntimeOwnerDisposition::Rejected);
    }
    expect_no_state_mutation(initial, core.view());

    constexpr std::array<RuntimeOwnerInput, 6> invalid_begin_fields{
        make_input(RuntimeOwnerInputKind::BeginTransportAttempt,
                   RuntimeOwnerEffectKind::StartAtProbe),
        make_input(RuntimeOwnerInputKind::BeginTransportAttempt,
                   RuntimeOwnerEffectKind::None, 1),
        make_input(RuntimeOwnerInputKind::BeginTransportAttempt,
                   RuntimeOwnerEffectKind::None, 0, 1),
        make_input(RuntimeOwnerInputKind::BeginTransportAttempt,
                   RuntimeOwnerEffectKind::None, 0, 0, 1),
        make_input(RuntimeOwnerInputKind::BeginTransportAttempt,
                   RuntimeOwnerEffectKind::None, 0, 0, 0, 1),
        make_input(RuntimeOwnerInputKind::BeginTransportAttempt,
                   RuntimeOwnerEffectKind::None, 0, 0, 0, 0, 1),
    };
    for (const auto invalid : invalid_begin_fields) {
        transition = submit_checked(core, invalid);
        CHECK(transition.disposition == RuntimeOwnerDisposition::Rejected);
    }
    expect_no_state_mutation(initial, core.view());
}

void test_per_kind_input_field_matrix()
{
    RuntimeOwnerCore core;
    begin_transport(core);
    const RuntimeOwnerView connecting = core.view();

    constexpr std::array<RuntimeOwnerInput, 4> invalid_established_fields{
        make_input(RuntimeOwnerInputKind::TransportEstablished,
                   RuntimeOwnerEffectKind::StartTransportAttempt, 0, 7, 1),
        make_input(RuntimeOwnerInputKind::TransportEstablished,
                   RuntimeOwnerEffectKind::None, 1, 7, 1),
        make_input(RuntimeOwnerInputKind::TransportEstablished,
                   RuntimeOwnerEffectKind::None, 0, 7, 1, 1),
        make_input(RuntimeOwnerInputKind::TransportEstablished,
                   RuntimeOwnerEffectKind::None, 0, 7, 1, 0, 1),
    };
    for (const auto invalid : invalid_established_fields) {
        CHECK(submit_checked(core, invalid).disposition ==
              RuntimeOwnerDisposition::Rejected);
        expect_no_state_mutation(connecting, core.view());
    }

    CHECK(submit_checked(core, make_input(
              RuntimeOwnerInputKind::TransportAttemptFailed,
              RuntimeOwnerEffectKind::None,
              0,
              0,
              connecting.mqtt_generation_counter)).disposition ==
          RuntimeOwnerDisposition::Rejected);
    expect_no_state_mutation(connecting, core.view());

    establish(core, 7);
    const RuntimeOwnerView awaiting = core.view();
    CHECK(submit_checked(core, make_input(
              RuntimeOwnerInputKind::LivenessOperationCompleted,
              RuntimeOwnerEffectKind::StartAtProbe,
              1,
              awaiting.mqtt_session_id,
              awaiting.mqtt_generation,
              0,
              1)).disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(submit_checked(core, make_input(
              RuntimeOwnerInputKind::ConfigActivationCommitted,
              RuntimeOwnerEffectKind::StartAtProbe,
              0,
              awaiting.mqtt_session_id,
              awaiting.mqtt_generation,
              1)).disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(submit_checked(core, make_input(
              RuntimeOwnerInputKind::ConfigActivationCommitted,
              RuntimeOwnerEffectKind::None,
              1,
              awaiting.mqtt_session_id,
              awaiting.mqtt_generation,
              1)).disposition == RuntimeOwnerDisposition::Rejected);
    expect_no_state_mutation(awaiting, core.view());

    const auto tickets = commit_config(core, 1);
    RuntimeOwnerInput noncanonical = completion_input(tickets[0]);
    noncanonical.config_commit_sequence = 1;
    const RuntimeOwnerView waiting = core.view();
    CHECK(submit_checked(core, noncanonical).disposition ==
          RuntimeOwnerDisposition::Rejected);
    noncanonical = operation_failure_input(
        RuntimeOwnerInputKind::LivenessOperationFailed, tickets[0]);
    noncanonical.receipt_kind = RuntimeOwnerEffectKind::None;
    CHECK(submit_checked(core, noncanonical).disposition ==
          RuntimeOwnerDisposition::Rejected);
    CHECK(submit_checked(core, make_input(
              RuntimeOwnerInputKind::TransportDisconnected,
              RuntimeOwnerEffectKind::None,
              1,
              waiting.mqtt_session_id,
              waiting.mqtt_generation)).disposition ==
          RuntimeOwnerDisposition::Rejected);
    CHECK(submit_checked(core, make_input(
              RuntimeOwnerInputKind::CriticalIngressFault,
              RuntimeOwnerEffectKind::None,
              0,
              0,
              0,
              0,
              1)).disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(submit_checked(core, make_input(
              RuntimeOwnerInputKind::ShutdownCommitted,
              RuntimeOwnerEffectKind::EnterRecovery)).disposition ==
          RuntimeOwnerDisposition::Rejected);
    expect_no_state_mutation(waiting, core.view());
}

void test_generated_input_field_matrix()
{
    {
        RuntimeOwnerCore core;
        check_input_field_matrix(
            core,
            make_input(RuntimeOwnerInputKind::Invalid),
            static_cast<std::uint8_t>(
                kInputEffect | kInputCorrelation | kInputSession |
                kInputGeneration | kInputCommit | kInputEpoch),
            0);
        check_input_field_matrix(
            core,
            make_input(RuntimeOwnerInputKind::BeginTransportAttempt),
            static_cast<std::uint8_t>(
                kInputEffect | kInputCorrelation | kInputSession |
                kInputGeneration | kInputCommit | kInputEpoch),
            0);
        check_input_field_matrix(
            core,
            make_input(RuntimeOwnerInputKind::CriticalIngressFault),
            static_cast<std::uint8_t>(
                kInputEffect | kInputCorrelation | kInputSession |
                kInputGeneration | kInputCommit | kInputEpoch),
            0);
        check_input_field_matrix(
            core,
            make_input(RuntimeOwnerInputKind::ShutdownCommitted),
            static_cast<std::uint8_t>(
                kInputEffect | kInputCorrelation | kInputSession |
                kInputGeneration | kInputCommit | kInputEpoch),
            0);
    }

    {
        RuntimeOwnerCore core;
        begin_transport(core);
        const std::uint32_t generation = core.view().mqtt_generation_counter;
        check_input_field_matrix(
            core,
            make_input(
                RuntimeOwnerInputKind::TransportEstablished,
                RuntimeOwnerEffectKind::None,
                0,
                7,
                generation),
            static_cast<std::uint8_t>(
                kInputEffect | kInputCorrelation | kInputCommit | kInputEpoch),
            static_cast<std::uint8_t>(kInputSession | kInputGeneration));
        check_input_field_matrix(
            core,
            make_input(
                RuntimeOwnerInputKind::TransportAttemptFailed,
                RuntimeOwnerEffectKind::StartTransportAttempt,
                0,
                0,
                generation),
            static_cast<std::uint8_t>(
                kInputCorrelation | kInputSession | kInputCommit | kInputEpoch),
            static_cast<std::uint8_t>(kInputEffect | kInputGeneration));
    }

    {
        RuntimeOwnerCore core;
        begin_transport(core);
        establish(core, 7);
        const RuntimeOwnerView active = core.view();
        check_input_field_matrix(
            core,
            make_input(
                RuntimeOwnerInputKind::ConfigActivationCommitted,
                RuntimeOwnerEffectKind::None,
                0,
                active.mqtt_session_id,
                active.mqtt_generation,
                1),
            static_cast<std::uint8_t>(
                kInputEffect | kInputCorrelation | kInputEpoch),
            static_cast<std::uint8_t>(
                kInputSession | kInputGeneration | kInputCommit));
    }

    {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        for (const auto kind : {
                 RuntimeOwnerInputKind::LivenessOperationCompleted,
                 RuntimeOwnerInputKind::LivenessOperationFailed,
                 RuntimeOwnerInputKind::DeadlineExpired,
             }) {
            check_input_field_matrix(
                core,
                operation_failure_input(kind, tickets[0]),
                kInputCommit,
                static_cast<std::uint8_t>(
                    kInputEffect | kInputCorrelation | kInputSession |
                    kInputGeneration | kInputEpoch));
        }
        const RuntimeOwnerInput liveness = completion_input(tickets[0]);
        for (std::uint16_t value = 10; value <= 255; ++value) {
            RuntimeOwnerInput unknown = liveness;
            unknown.receipt_kind =
                static_cast<RuntimeOwnerEffectKind>(value);
            CHECK(submit_checked(core, unknown).disposition ==
                  RuntimeOwnerDisposition::Rejected);
        }
    }

    {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        const RuntimeOwnerEffect freeze = complete_all(core, tickets);
        for (const auto kind : {
                 RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
                 RuntimeOwnerInputKind::SnapshotFreezeFailed,
             }) {
            check_input_field_matrix(
                core,
                snapshot_input(kind, freeze),
                kInputCommit,
                static_cast<std::uint8_t>(
                    kInputEffect | kInputCorrelation | kInputSession |
                    kInputGeneration | kInputEpoch));
        }
        RuntimeOwnerInput snapshot = snapshot_input(
            RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze);
        for (std::uint16_t value = 10; value <= 255; ++value) {
            snapshot.receipt_kind = static_cast<RuntimeOwnerEffectKind>(value);
            CHECK(submit_checked(core, snapshot).disposition ==
                  RuntimeOwnerDisposition::Rejected);
        }
    }

    {
        RuntimeOwnerCore core;
        prepare_liveness(core);
        const RuntimeOwnerView active = core.view();
        check_input_field_matrix(
            core,
            make_input(
                RuntimeOwnerInputKind::TransportDisconnected,
                RuntimeOwnerEffectKind::None,
                0,
                active.mqtt_session_id,
                active.mqtt_generation),
            static_cast<std::uint8_t>(
                kInputEffect | kInputCorrelation | kInputCommit | kInputEpoch),
            static_cast<std::uint8_t>(kInputSession | kInputGeneration));
    }
}

void test_transport_generation_establishment_and_failure()
{
    RuntimeOwnerCore core;
    const RuntimeOwnerTransition first = begin_transport(core);
    const std::uint32_t first_generation = core.view().mqtt_generation_counter;
    CHECK(first_generation == 1);
    CHECK(first.effects[0].correlation_id == 0);

    const RuntimeOwnerView connecting = core.view();
    RuntimeOwnerTransition rejected = submit_checked(core, make_input(
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        first_generation));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    expect_no_state_mutation(connecting, core.view());
    rejected = submit_checked(core, make_input(
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerEffectKind::None,
        0,
        7,
        first_generation + 1));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    expect_no_state_mutation(connecting, core.view());

    const RuntimeOwnerInput failed = make_input(
        RuntimeOwnerInputKind::TransportAttemptFailed,
        RuntimeOwnerEffectKind::StartTransportAttempt,
        0,
        0,
        first_generation);
    RuntimeOwnerTransition failure = submit_checked(core, failed);
    CHECK(failure.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(failure.phase_after == RuntimeOwnerPhase::RecoveryPending);
    CHECK(failure.effect_count == 2);
    CHECK(failure.effects[0].kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(failure.effects[1].kind == RuntimeOwnerEffectKind::EnterRecovery);
    CHECK(failure.effects[0].correlation_id == 0);
    CHECK(is_zero_token(failure.effects[0].attempt));
    CHECK(failure.effects[0].fault_code == RuntimeOwnerFaultCode::TransportFailure);
    CHECK(core.view().last_fault == RuntimeOwnerFaultCode::TransportFailure);

    RuntimeOwnerTransition duplicate = submit_checked(core, failed);
    CHECK(duplicate.disposition == RuntimeOwnerDisposition::AcceptedDuplicate);
    CHECK(duplicate.effect_count == 0);

    rejected = submit_checked(core, make_input(
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerEffectKind::None,
        0,
        7,
        first_generation));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);

    const RuntimeOwnerTransition second = begin_transport(core);
    CHECK(core.view().mqtt_generation_counter == first_generation + 1);
    CHECK(second.effects[0].attempt.mqtt_generation == first_generation + 1);
    establish(core, 7);
    CHECK(core.view().mqtt_session_id == 7);

    rejected = submit_checked(core, failed);
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(core.view().phase == RuntimeOwnerPhase::AwaitingConfigCommit);
}

void test_config_commit_binding_and_idempotence()
{
    RuntimeOwnerCore core;
    begin_transport(core);
    establish(core, 9);
    const RuntimeOwnerView before = core.view();

    constexpr std::array<std::uint32_t, 3> wrong_sessions{0, 8, 10};
    for (const auto session : wrong_sessions) {
        const RuntimeOwnerTransition rejected = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            session,
            before.mqtt_generation,
            1));
        CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    }
    RuntimeOwnerTransition rejected = submit_checked(core, make_input(
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        before.mqtt_session_id,
        before.mqtt_generation + 1,
        1));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    rejected = submit_checked(core, make_input(
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        before.mqtt_session_id,
        before.mqtt_generation,
        1,
        1));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    rejected = submit_checked(core, make_input(
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        before.mqtt_session_id,
        before.mqtt_generation,
        0));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    expect_no_state_mutation(before, core.view());

    const auto tickets = commit_config(core, 4);
    const RuntimeOwnerView committed = core.view();
    RuntimeOwnerTransition duplicate = submit_checked(core, make_input(
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        committed.mqtt_session_id,
        committed.mqtt_generation,
        4));
    CHECK(duplicate.disposition == RuntimeOwnerDisposition::AcceptedDuplicate);
    CHECK(duplicate.effect_count == 0);
    expect_no_state_mutation(committed, core.view());

    rejected = submit_checked(core, make_input(
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        committed.mqtt_session_id,
        committed.mqtt_generation,
        3));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    expect_no_state_mutation(committed, core.view());
    CHECK(tickets[3].correlation_id + 2 == committed.last_correlation_id);
}

void test_all_subsets_and_permutations()
{
    for (std::uint8_t subset = 0; subset < 16; ++subset) {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        std::size_t accepted_count = 0;
        for (std::size_t index = 0; index < tickets.size(); ++index) {
            if ((subset & static_cast<std::uint8_t>(1u << index)) != 0) {
                const RuntimeOwnerTransition transition = submit_checked(
                    core, completion_input(tickets[index]));
                CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
                ++accepted_count;
                CHECK(transition.effect_count ==
                      (accepted_count == tickets.size() ? 1 : 0));
            }
        }
        CHECK(core.view().phase ==
              (subset == 15 ? RuntimeOwnerPhase::SnapshotFreezePending
                            : RuntimeOwnerPhase::LivenessWaiting));
    }

    std::array<std::size_t, 4> order{0, 1, 2, 3};
    std::size_t permutation_count = 0;
    do {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        for (std::size_t index = 0; index < order.size(); ++index) {
            const RuntimeOwnerTransition transition = submit_checked(
                core, completion_input(tickets[order[index]]));
            CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
            CHECK(transition.effect_count == (index + 1 == order.size() ? 1 : 0));
        }
        CHECK(core.view().phase == RuntimeOwnerPhase::SnapshotFreezePending);
        ++permutation_count;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutation_count == 24);
}

void test_rejected_receipts_leave_no_hidden_evidence()
{
    for (std::size_t target = 0; target < kLivenessKinds.size(); ++target) {
        for (std::size_t variant = 0; variant < 6; ++variant) {
            RuntimeOwnerCore core;
            const auto tickets = prepare_liveness(core);
            for (std::size_t index = 0; index < tickets.size(); ++index) {
                if (index != target) {
                    const RuntimeOwnerTransition accepted = submit_checked(
                        core, completion_input(tickets[index]));
                    CHECK(accepted.disposition ==
                          RuntimeOwnerDisposition::Accepted);
                    CHECK(accepted.effect_count == 0);
                }
            }

            RuntimeOwnerInput wrong = completion_input(tickets[target]);
            switch (variant) {
            case 0:
                wrong.receipt_kind =
                    kLivenessKinds[(target + 1) % kLivenessKinds.size()];
                break;
            case 1:
                wrong.receipt_kind =
                    static_cast<RuntimeOwnerEffectKind>(10);
                break;
            case 2:
                ++wrong.correlation_id;
                break;
            case 3:
                ++wrong.mqtt_session_id;
                break;
            case 4:
                ++wrong.mqtt_generation;
                break;
            case 5:
                ++wrong.config_apply_epoch;
                break;
            default:
                break;
            }
            const RuntimeOwnerView before = core.view();
            const RuntimeOwnerTransition rejected = submit_checked(core, wrong);
            CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
            expect_no_state_mutation(before, core.view());

            const RuntimeOwnerTransition final = submit_checked(
                core, completion_input(tickets[target]));
            CHECK(final.disposition == RuntimeOwnerDisposition::Accepted);
            CHECK(final.effect_count == 1);
            CHECK(final.effects[0].kind ==
                  RuntimeOwnerEffectKind::FreezeBootSnapshot);
        }
    }
}

void test_snapshot_receipt_mutation_matrix()
{
    for (const auto outcome : {
             RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
             RuntimeOwnerInputKind::SnapshotFreezeFailed,
         }) {
        for (std::size_t variant = 0; variant < 7; ++variant) {
            RuntimeOwnerCore core;
            const auto tickets = prepare_liveness(core);
            const RuntimeOwnerEffect freeze = complete_all(core, tickets);
            RuntimeOwnerInput wrong = snapshot_input(outcome, freeze);
            switch (variant) {
            case 0:
                wrong.receipt_kind =
                    RuntimeOwnerEffectKind::EndBootOrchestration;
                break;
            case 1:
                wrong.receipt_kind =
                    static_cast<RuntimeOwnerEffectKind>(10);
                break;
            case 2:
                ++wrong.correlation_id;
                break;
            case 3:
                ++wrong.mqtt_session_id;
                break;
            case 4:
                ++wrong.mqtt_generation;
                break;
            case 5:
                ++wrong.config_apply_epoch;
                break;
            case 6:
                wrong.config_commit_sequence = 1;
                break;
            default:
                break;
            }
            CHECK(submit_checked(core, wrong).disposition ==
                  RuntimeOwnerDisposition::Rejected);
            const RuntimeOwnerTransition exact = submit_checked(
                core, snapshot_input(outcome, freeze));
            CHECK(exact.disposition == RuntimeOwnerDisposition::Accepted);
            CHECK(exact.effect_count ==
                  (outcome == RuntimeOwnerInputKind::SnapshotFreezeSucceeded
                       ? 1
                       : 2));
        }
    }
}

void test_duplicate_completion_and_snapshot_one_shot()
{
    RuntimeOwnerCore core;
    const auto tickets = prepare_liveness(core);
    RuntimeOwnerTransition first = submit_checked(
        core, completion_input(tickets[0]));
    CHECK(first.disposition == RuntimeOwnerDisposition::Accepted);
    RuntimeOwnerTransition duplicate = submit_checked(
        core, completion_input(tickets[0]));
    CHECK(duplicate.disposition == RuntimeOwnerDisposition::AcceptedDuplicate);
    CHECK(duplicate.effect_count == 0);

    CHECK(submit_checked(core, completion_input(tickets[1])).effect_count == 0);
    CHECK(submit_checked(core, completion_input(tickets[2])).effect_count == 0);
    const RuntimeOwnerTransition final = submit_checked(
        core, completion_input(tickets[3]));
    CHECK(final.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(final.effect_count == 1);
    const RuntimeOwnerEffect freeze = final.effects[0];
    CHECK(freeze.kind == RuntimeOwnerEffectKind::FreezeBootSnapshot);

    RuntimeOwnerInput wrong = snapshot_input(
        RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze);
    ++wrong.correlation_id;
    RuntimeOwnerTransition rejected = submit_checked(core, wrong);
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    wrong = snapshot_input(RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze);
    ++wrong.mqtt_generation;
    rejected = submit_checked(core, wrong);
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    wrong = snapshot_input(RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze);
    ++wrong.config_apply_epoch;
    rejected = submit_checked(core, wrong);
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(core.view().phase == RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(!core.view().boot_orchestration_ended);

    const RuntimeOwnerEffect boot_end = finish_snapshot(core, freeze);
    CHECK(boot_end.correlation_id != freeze.correlation_id);
    const RuntimeOwnerView ready = core.view();
    duplicate = submit_checked(
        core,
        snapshot_input(RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze));
    CHECK(duplicate.disposition == RuntimeOwnerDisposition::AcceptedDuplicate);
    CHECK(duplicate.effect_count == 0);
    expect_no_state_mutation(ready, core.view());

    rejected = submit_checked(core, completion_input(tickets[1]));
    CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
    CHECK(core.view().boot_orchestration_ended);
}

void test_new_config_restarts_pending_authorization()
{
    {
        RuntimeOwnerCore core;
        const auto old_tickets = prepare_liveness(core);
        CHECK(submit_checked(core, completion_input(old_tickets[0])).disposition ==
              RuntimeOwnerDisposition::Accepted);
        const auto new_tickets = commit_config(core, 2);
        CHECK(new_tickets[0].attempt.config_apply_epoch ==
              old_tickets[0].attempt.config_apply_epoch + 1);
        CHECK(new_tickets[0].correlation_id ==
              old_tickets.back().correlation_id + 3);
        CHECK(submit_checked(core, completion_input(old_tickets[1])).disposition ==
              RuntimeOwnerDisposition::Rejected);
        for (std::size_t index = 0; index < new_tickets.size() - 1; ++index) {
            const RuntimeOwnerTransition accepted = submit_checked(
                core, completion_input(new_tickets[index]));
            CHECK(accepted.disposition == RuntimeOwnerDisposition::Accepted);
            CHECK(accepted.effect_count == 0);
        }
        const RuntimeOwnerTransition final = submit_checked(
            core, completion_input(new_tickets.back()));
        CHECK(final.effect_count == 1);
    }

    {
        RuntimeOwnerCore core;
        const auto old_tickets = prepare_liveness(core);
        const RuntimeOwnerEffect old_freeze = complete_all(core, old_tickets);
        const auto new_tickets = commit_config(core, 2);
        CHECK(new_tickets[0].correlation_id > old_freeze.correlation_id);
        CHECK(submit_checked(
                  core,
                  snapshot_input(
                      RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
                      old_freeze)).disposition == RuntimeOwnerDisposition::Rejected);
        const RuntimeOwnerEffect new_freeze = complete_all(core, new_tickets);
        finish_snapshot(core, new_freeze);
    }

    {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        const RuntimeOwnerEffect freeze = complete_all(core, tickets);
        finish_snapshot(core, freeze);
        const RuntimeOwnerView ready = core.view();
        const RuntimeOwnerTransition rejected = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            ready.mqtt_session_id,
            ready.mqtt_generation,
            2));
        CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
        expect_no_state_mutation(ready, core.view());
    }

    {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        CHECK(submit_checked(
                  core,
                  operation_failure_input(
                      RuntimeOwnerInputKind::LivenessOperationFailed,
                      tickets[0])).disposition == RuntimeOwnerDisposition::Accepted);
        const RuntimeOwnerView recovery = core.view();
        const RuntimeOwnerTransition rejected = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            tickets[0].attempt.mqtt_session_id,
            tickets[0].attempt.mqtt_generation,
            2));
        CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
        CHECK(rejected.effect_count == 0);
        expect_no_state_mutation(recovery, core.view());
    }

    {
        RuntimeOwnerCore core;
        begin_transport(core);
        establish(core, 7);
        const RuntimeOwnerView active = core.view();
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::ShutdownCommitted)).disposition ==
              RuntimeOwnerDisposition::Accepted);
        const RuntimeOwnerView shutdown = core.view();
        const RuntimeOwnerTransition rejected = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            active.mqtt_session_id,
            active.mqtt_generation,
            1));
        CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
        CHECK(rejected.effect_count == 0);
        expect_no_state_mutation(shutdown, core.view());
    }
}

void check_recovery_effects(
    const RuntimeOwnerTransition &transition,
    const RuntimeOwnerFaultCode fault)
{
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.phase_after == RuntimeOwnerPhase::RecoveryPending);
    CHECK(transition.effect_count == 2);
    CHECK(transition.effects[0].kind == RuntimeOwnerEffectKind::RecordFault);
    CHECK(transition.effects[1].kind == RuntimeOwnerEffectKind::EnterRecovery);
    CHECK(transition.effects[0].correlation_id ==
          transition.effects[1].correlation_id);
    CHECK(transition.effects[0].attempt == transition.effects[1].attempt);
    CHECK(transition.effects[0].fault_code == fault);
    CHECK(transition.effects[1].fault_code == fault);
}

void test_exact_operation_failure_and_deadline()
{
    for (const auto failure_kind : {
             RuntimeOwnerInputKind::LivenessOperationFailed,
             RuntimeOwnerInputKind::DeadlineExpired,
         }) {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        RuntimeOwnerInput wrong = operation_failure_input(failure_kind, tickets[0]);
        ++wrong.config_apply_epoch;
        const RuntimeOwnerView before = core.view();
        RuntimeOwnerTransition rejected = submit_checked(core, wrong);
        CHECK(rejected.disposition == RuntimeOwnerDisposition::Rejected);
        expect_no_state_mutation(before, core.view());

        const RuntimeOwnerInput exact = operation_failure_input(
            failure_kind, tickets[0]);
        const RuntimeOwnerTransition failed = submit_checked(core, exact);
        check_recovery_effects(
            failed,
            failure_kind == RuntimeOwnerInputKind::DeadlineExpired
                ? RuntimeOwnerFaultCode::DeadlineExpired
                : RuntimeOwnerFaultCode::LivenessFailure);
        CHECK(failed.effects[0].correlation_id == tickets[0].correlation_id);
        CHECK(failed.effects[0].attempt == tickets[0].attempt);
        CHECK(core.view().last_fault ==
              (failure_kind == RuntimeOwnerInputKind::DeadlineExpired
                   ? RuntimeOwnerFaultCode::DeadlineExpired
                   : RuntimeOwnerFaultCode::LivenessFailure));
        const RuntimeOwnerTransition duplicate = submit_checked(core, exact);
        CHECK(duplicate.disposition == RuntimeOwnerDisposition::AcceptedDuplicate);
        CHECK(duplicate.effect_count == 0);
        CHECK(submit_checked(core, completion_input(tickets[0])).disposition ==
              RuntimeOwnerDisposition::Rejected);
    }

    RuntimeOwnerCore completed_core;
    const auto tickets = prepare_liveness(completed_core);
    CHECK(submit_checked(completed_core, completion_input(tickets[0])).disposition ==
          RuntimeOwnerDisposition::Accepted);
    CHECK(submit_checked(
              completed_core,
              operation_failure_input(
                  RuntimeOwnerInputKind::LivenessOperationFailed,
                  tickets[0])).disposition == RuntimeOwnerDisposition::Rejected);
}

void test_disconnect_phase_invalidation_matrix()
{
    constexpr std::array<RuntimeOwnerPhase, 4> phases{
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RuntimeReady,
    };
    for (const auto target : phases) {
        RuntimeOwnerCore core;
        std::array<RuntimeOwnerEffect, 4> tickets{};
        RuntimeOwnerEffect freeze{};
        reach_phase(core, target, tickets, freeze);
        const RuntimeOwnerView active = core.view();
        const bool sticky_before = active.boot_orchestration_ended;
        const RuntimeOwnerInput disconnect = make_input(
            RuntimeOwnerInputKind::TransportDisconnected,
            RuntimeOwnerEffectKind::None,
            0,
            active.mqtt_session_id,
            active.mqtt_generation);
        const RuntimeOwnerTransition failed = submit_checked(core, disconnect);
        check_recovery_effects(
            failed, RuntimeOwnerFaultCode::TransportDisconnected);
        CHECK(core.view().boot_orchestration_ended == sticky_before);
        CHECK(core.view().mqtt_session_id == 0);
        CHECK(core.view().mqtt_generation == 0);
        CHECK(!core.view().active_attempt.valid());

        if (target == RuntimeOwnerPhase::AwaitingConfigCommit) {
            CHECK(submit_checked(core, make_input(
                      RuntimeOwnerInputKind::ConfigActivationCommitted,
                      RuntimeOwnerEffectKind::None,
                      0,
                      active.mqtt_session_id,
                      active.mqtt_generation,
                      1)).disposition == RuntimeOwnerDisposition::Rejected);
        } else {
            CHECK(submit_checked(core, completion_input(tickets[0])).disposition ==
                  RuntimeOwnerDisposition::Rejected);
        }
        if (target == RuntimeOwnerPhase::SnapshotFreezePending ||
            target == RuntimeOwnerPhase::RuntimeReady) {
            CHECK(submit_checked(
                      core,
                      snapshot_input(
                          RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
                          freeze)).disposition == RuntimeOwnerDisposition::Rejected);
        }
    }
}

void test_critical_fault_phase_matrix()
{
    constexpr std::array<RuntimeOwnerPhase, 6> phases{
        RuntimeOwnerPhase::ColdStart,
        RuntimeOwnerPhase::TransportConnecting,
        RuntimeOwnerPhase::AwaitingConfigCommit,
        RuntimeOwnerPhase::LivenessWaiting,
        RuntimeOwnerPhase::SnapshotFreezePending,
        RuntimeOwnerPhase::RuntimeReady,
    };
    for (const auto target : phases) {
        RuntimeOwnerCore core;
        std::array<RuntimeOwnerEffect, 4> tickets{};
        RuntimeOwnerEffect freeze{};
        reach_phase(core, target, tickets, freeze);
        const RuntimeOwnerPrivateSnapshot before =
            RuntimeOwnerCoreTestPeer::snapshot(core);
        const RuntimeOwnerInput fault =
            make_input(RuntimeOwnerInputKind::CriticalIngressFault);
        const RuntimeOwnerTransition failed = submit_checked(core, fault);
        check_recovery_effects(failed, RuntimeOwnerFaultCode::CriticalIngress);
        CHECK(core.view().boot_orchestration_ended ==
              before.boot_orchestration_ended);
        CHECK(failed.effects[0].correlation_id == 0);
        CHECK(failed.effects[0].attempt == before.active_attempt);
        if (target == RuntimeOwnerPhase::LivenessWaiting ||
            target == RuntimeOwnerPhase::SnapshotFreezePending ||
            target == RuntimeOwnerPhase::RuntimeReady) {
            CHECK(submit_checked(core, completion_input(tickets[0])).disposition ==
                  RuntimeOwnerDisposition::Rejected);
        }
        if (target == RuntimeOwnerPhase::SnapshotFreezePending ||
            target == RuntimeOwnerPhase::RuntimeReady) {
            CHECK(submit_checked(
                      core,
                      snapshot_input(
                          RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
                          freeze)).disposition == RuntimeOwnerDisposition::Rejected);
        }
        if (target == RuntimeOwnerPhase::RuntimeReady) {
            CHECK(core.view().boot_orchestration_ended);
            CHECK(submit_checked(
                      core,
                      make_input(
                          RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
                  RuntimeOwnerDisposition::Rejected);
        }
    }
}

void test_failure_duplicate_and_restart_ledger_matrix()
{
    for (std::uint8_t failure_case = 0; failure_case < 6; ++failure_case) {
        RuntimeOwnerCore core;
        std::array<RuntimeOwnerEffect, 4> tickets{};
        RuntimeOwnerEffect freeze{};
        RuntimeOwnerInput exact{};
        RuntimeOwnerInput different{};

        switch (failure_case) {
        case 0:
            begin_transport(core);
            exact = make_input(
                RuntimeOwnerInputKind::TransportAttemptFailed,
                RuntimeOwnerEffectKind::StartTransportAttempt,
                0,
                0,
                core.view().mqtt_generation_counter);
            different = exact;
            ++different.mqtt_generation;
            break;
        case 1:
            tickets = prepare_liveness(core);
            exact = operation_failure_input(
                RuntimeOwnerInputKind::LivenessOperationFailed, tickets[0]);
            different = operation_failure_input(
                RuntimeOwnerInputKind::LivenessOperationFailed, tickets[1]);
            break;
        case 2:
            tickets = prepare_liveness(core);
            exact = operation_failure_input(
                RuntimeOwnerInputKind::DeadlineExpired, tickets[0]);
            different = operation_failure_input(
                RuntimeOwnerInputKind::DeadlineExpired, tickets[1]);
            break;
        case 3:
            tickets = prepare_liveness(core);
            freeze = complete_all(core, tickets);
            exact = snapshot_input(
                RuntimeOwnerInputKind::SnapshotFreezeFailed, freeze);
            different = exact;
            ++different.correlation_id;
            break;
        case 4: {
            tickets = prepare_liveness(core);
            const RuntimeOwnerView active = core.view();
            exact = make_input(
                RuntimeOwnerInputKind::TransportDisconnected,
                RuntimeOwnerEffectKind::None,
                0,
                active.mqtt_session_id,
                active.mqtt_generation);
            different = exact;
            ++different.mqtt_session_id;
            break;
        }
        case 5: {
            tickets = prepare_liveness(core);
            const RuntimeOwnerView active = core.view();
            exact = make_input(RuntimeOwnerInputKind::CriticalIngressFault);
            different = make_input(
                RuntimeOwnerInputKind::TransportDisconnected,
                RuntimeOwnerEffectKind::None,
                0,
                active.mqtt_session_id,
                active.mqtt_generation);
            break;
        }
        default:
            CHECK(false);
            break;
        }

        const RuntimeOwnerTransition failed = submit_checked(core, exact);
        CHECK(failed.disposition == RuntimeOwnerDisposition::Accepted);
        CHECK(failed.phase_after == RuntimeOwnerPhase::RecoveryPending);
        CHECK(submit_checked(core, exact).disposition ==
              RuntimeOwnerDisposition::AcceptedDuplicate);
        CHECK(submit_checked(core, different).disposition ==
              RuntimeOwnerDisposition::Rejected);

        begin_transport(core);
        const RuntimeOwnerTransition after_restart = submit_checked(core, exact);
        if (failure_case == 5) {
            CHECK(after_restart.disposition == RuntimeOwnerDisposition::Accepted);
            CHECK(after_restart.phase_after == RuntimeOwnerPhase::RecoveryPending);
        } else {
            CHECK(after_restart.disposition == RuntimeOwnerDisposition::Rejected);
            CHECK(core.view().phase == RuntimeOwnerPhase::TransportConnecting);
        }
    }
}

void test_direct_runtime_ready_noise_and_sticky_latch()
{
    for (std::uint8_t noise_case = 0; noise_case < 7; ++noise_case) {
        RuntimeOwnerCore core;
        std::array<RuntimeOwnerEffect, 4> tickets{};
        RuntimeOwnerEffect freeze{};
        reach_phase(core, RuntimeOwnerPhase::RuntimeReady, tickets, freeze);
        const RuntimeOwnerView ready = core.view();
        CHECK(ready.boot_orchestration_ended);

        RuntimeOwnerInput input{};
        switch (noise_case) {
        case 0:
            input = make_input(RuntimeOwnerInputKind::BeginTransportAttempt);
            break;
        case 1:
            input = make_input(
                RuntimeOwnerInputKind::TransportEstablished,
                RuntimeOwnerEffectKind::None,
                0,
                ready.mqtt_session_id,
                ready.mqtt_generation);
            break;
        case 2:
            input = make_input(
                RuntimeOwnerInputKind::ConfigActivationCommitted,
                RuntimeOwnerEffectKind::None,
                0,
                ready.mqtt_session_id,
                ready.mqtt_generation,
                2);
            break;
        case 3:
            input = completion_input(tickets[0]);
            break;
        case 4:
            input = snapshot_input(
                RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze);
            ++input.correlation_id;
            break;
        case 5:
            input = snapshot_input(
                RuntimeOwnerInputKind::SnapshotFreezeSucceeded, freeze);
            break;
        case 6:
            input = make_input(RuntimeOwnerInputKind::CriticalIngressFault);
            break;
        default:
            CHECK(false);
            break;
        }

        const RuntimeOwnerTransition transition = submit_checked(core, input);
        if (noise_case <= 4) {
            CHECK(transition.disposition == RuntimeOwnerDisposition::Rejected);
        } else if (noise_case == 5) {
            CHECK(transition.disposition ==
                  RuntimeOwnerDisposition::AcceptedDuplicate);
        } else {
            check_recovery_effects(
                transition, RuntimeOwnerFaultCode::CriticalIngress);
        }
        CHECK(core.view().boot_orchestration_ended);
        for (std::size_t index = 0; index < transition.effect_count; ++index) {
            CHECK(transition.effects[index].kind !=
                  RuntimeOwnerEffectKind::FreezeBootSnapshot);
            CHECK(transition.effects[index].kind !=
                  RuntimeOwnerEffectKind::EndBootOrchestration);
        }
    }
}

void test_snapshot_failure_disconnect_critical_and_shutdown()
{
    {
        RuntimeOwnerCore core;
        const RuntimeOwnerTransition failed = submit_checked(
            core, make_input(RuntimeOwnerInputKind::CriticalIngressFault));
        check_recovery_effects(failed, RuntimeOwnerFaultCode::CriticalIngress);
        CHECK(failed.effects[0].correlation_id == 0);
        CHECK(is_zero_token(failed.effects[0].attempt));
        CHECK(begin_transport(core).disposition ==
              RuntimeOwnerDisposition::Accepted);
    }

    {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        const RuntimeOwnerEffect freeze = complete_all(core, tickets);
        RuntimeOwnerInput stale = snapshot_input(
            RuntimeOwnerInputKind::SnapshotFreezeFailed, freeze);
        ++stale.mqtt_session_id;
        const RuntimeOwnerView before = core.view();
        CHECK(submit_checked(core, stale).disposition ==
              RuntimeOwnerDisposition::Rejected);
        expect_no_state_mutation(before, core.view());
        const RuntimeOwnerInput exact = snapshot_input(
            RuntimeOwnerInputKind::SnapshotFreezeFailed, freeze);
        const RuntimeOwnerTransition failed = submit_checked(core, exact);
        check_recovery_effects(failed, RuntimeOwnerFaultCode::SnapshotFailure);
        CHECK(failed.effects[0].correlation_id == freeze.correlation_id);
        CHECK(failed.effects[0].attempt == freeze.attempt);
        CHECK(core.view().last_fault == RuntimeOwnerFaultCode::SnapshotFailure);
        CHECK(submit_checked(core, exact).disposition ==
              RuntimeOwnerDisposition::AcceptedDuplicate);
        CHECK(submit_checked(
                  core,
                  snapshot_input(
                      RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
                      freeze)).disposition == RuntimeOwnerDisposition::Rejected);
        CHECK(!core.view().boot_orchestration_ended);
    }

    {
        RuntimeOwnerCore core;
        prepare_liveness(core);
        const RuntimeOwnerView active = core.view();
        const RuntimeOwnerInput disconnect = make_input(
            RuntimeOwnerInputKind::TransportDisconnected,
            RuntimeOwnerEffectKind::None,
            0,
            active.mqtt_session_id,
            active.mqtt_generation);
        const RuntimeOwnerTransition failed = submit_checked(core, disconnect);
        check_recovery_effects(
            failed, RuntimeOwnerFaultCode::TransportDisconnected);
        CHECK(failed.effects[0].correlation_id == 0);
        CHECK(failed.effects[0].attempt == active.active_attempt);
        CHECK(core.view().mqtt_session_id == 0);
        CHECK(core.view().mqtt_generation == 0);
        CHECK(!core.view().active_attempt.valid());
        CHECK(submit_checked(core, disconnect).disposition ==
              RuntimeOwnerDisposition::AcceptedDuplicate);
    }

    {
        RuntimeOwnerCore core;
        const auto tickets = prepare_liveness(core);
        const RuntimeOwnerEffect freeze = complete_all(core, tickets);
        finish_snapshot(core, freeze);
        const RuntimeOwnerView ready = core.view();
        const RuntimeOwnerInput disconnect = make_input(
            RuntimeOwnerInputKind::TransportDisconnected,
            RuntimeOwnerEffectKind::None,
            0,
            ready.mqtt_session_id,
            ready.mqtt_generation);
        check_recovery_effects(
            submit_checked(core, disconnect),
            RuntimeOwnerFaultCode::TransportDisconnected);
        CHECK(core.view().boot_orchestration_ended);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
              RuntimeOwnerDisposition::Rejected);
        CHECK(submit_checked(core, make_input(
                  RuntimeOwnerInputKind::TransportEstablished,
                  RuntimeOwnerEffectKind::None,
                  0,
                  ready.mqtt_session_id,
                  ready.mqtt_generation)).disposition ==
              RuntimeOwnerDisposition::Rejected);
        CHECK(submit_checked(core, make_input(
                  RuntimeOwnerInputKind::ConfigActivationCommitted,
                  RuntimeOwnerEffectKind::None,
                  0,
                  ready.mqtt_session_id,
                  ready.mqtt_generation,
                  2)).disposition == RuntimeOwnerDisposition::Rejected);
        CHECK(submit_checked(core, completion_input(tickets[0])).disposition ==
              RuntimeOwnerDisposition::Rejected);
        CHECK(submit_checked(
                  core,
                  snapshot_input(
                      RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
                      freeze)).disposition == RuntimeOwnerDisposition::Rejected);
        CHECK(core.view().boot_orchestration_ended);
    }

    {
        RuntimeOwnerCore core;
        prepare_liveness(core);
        check_recovery_effects(
            submit_checked(
                core, make_input(RuntimeOwnerInputKind::CriticalIngressFault)),
            RuntimeOwnerFaultCode::CriticalIngress);
        CHECK(core.view().last_fault == RuntimeOwnerFaultCode::CriticalIngress);
    }

    for (std::uint8_t target_phase = 0; target_phase < 7; ++target_phase) {
        RuntimeOwnerCore core;
        RuntimeOwnerEffect freeze{};
        std::array<RuntimeOwnerEffect, 4> tickets{};
        if (target_phase >= 1) {
            begin_transport(core);
        }
        if (target_phase >= 2) {
            establish(core, 7);
        }
        if (target_phase >= 3) {
            tickets = commit_config(core, 1);
            if (target_phase >= 4) {
                freeze = complete_all(core, tickets);
            }
        }
        if (target_phase >= 5) {
            finish_snapshot(core, freeze);
        }
        if (target_phase >= 6) {
            const RuntimeOwnerView ready = core.view();
            CHECK(submit_checked(core, make_input(
                      RuntimeOwnerInputKind::TransportDisconnected,
                      RuntimeOwnerEffectKind::None,
                      0,
                      ready.mqtt_session_id,
                      ready.mqtt_generation)).disposition ==
                  RuntimeOwnerDisposition::Accepted);
        }
        const bool latch_before = core.view().boot_orchestration_ended;
        const RuntimeOwnerTransition shutdown = submit_checked(
            core, make_input(RuntimeOwnerInputKind::ShutdownCommitted));
        CHECK(shutdown.disposition == RuntimeOwnerDisposition::Accepted);
        CHECK(shutdown.phase_after == RuntimeOwnerPhase::ShutdownCommitted);
        CHECK(shutdown.effect_count == 0);
        CHECK(core.view().boot_orchestration_ended == latch_before);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::ShutdownCommitted)).disposition ==
              RuntimeOwnerDisposition::AcceptedDuplicate);
        CHECK(freeze.kind == RuntimeOwnerEffectKind::None ||
              freeze.kind == RuntimeOwnerEffectKind::FreezeBootSnapshot);
    }
}

void test_same_slot_new_generation_rejects_old_ticket()
{
    RuntimeOwnerCore core;
    const auto old_tickets = prepare_liveness(core, 11, 1);
    const RuntimeOwnerView first = core.view();
    const RuntimeOwnerInput disconnect = make_input(
        RuntimeOwnerInputKind::TransportDisconnected,
        RuntimeOwnerEffectKind::None,
        0,
        first.mqtt_session_id,
        first.mqtt_generation);
    CHECK(submit_checked(core, disconnect).disposition ==
          RuntimeOwnerDisposition::Accepted);
    begin_transport(core);
    establish(core, 11);
    const auto new_tickets = commit_config(core, 2);
    CHECK(new_tickets[0].attempt.mqtt_session_id ==
          old_tickets[0].attempt.mqtt_session_id);
    CHECK(new_tickets[0].attempt.mqtt_generation !=
          old_tickets[0].attempt.mqtt_generation);
    CHECK(submit_checked(core, completion_input(old_tickets[0])).disposition ==
          RuntimeOwnerDisposition::Rejected);
    CHECK(submit_checked(core, completion_input(new_tickets[0])).disposition ==
          RuntimeOwnerDisposition::Accepted);
}

void test_counter_saturation_is_atomic_and_fail_closed()
{
    constexpr std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();

    {
        RuntimeOwnerCore core;
        RuntimeOwnerCoreTestPeer::seed_counters(core, maximum, 0, 0, 0);
        const RuntimeOwnerTransition saturated = submit_checked(
            core, make_input(RuntimeOwnerInputKind::BeginTransportAttempt));
        CHECK(saturated.disposition == RuntimeOwnerDisposition::FailClosed);
        CHECK(saturated.phase_after == RuntimeOwnerPhase::RecoveryPending);
        CHECK(saturated.effect_count == 2);
        CHECK(core.view().mqtt_generation_counter == maximum);
        CHECK(core.view().last_fault == RuntimeOwnerFaultCode::CounterSaturation);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
              RuntimeOwnerDisposition::Rejected);
    }

    {
        RuntimeOwnerCore core;
        RuntimeOwnerCoreTestPeer::seed_counters(core, maximum - 1, 0, 0, 0);
        const RuntimeOwnerTransition last_generation = begin_transport(core);
        CHECK(last_generation.effects[0].attempt.mqtt_generation == maximum);
        const RuntimeOwnerInput failed = make_input(
            RuntimeOwnerInputKind::TransportAttemptFailed,
            RuntimeOwnerEffectKind::StartTransportAttempt,
            0,
            0,
            maximum);
        CHECK(submit_checked(core, failed).disposition ==
              RuntimeOwnerDisposition::Accepted);
        const RuntimeOwnerTransition saturated = submit_checked(
            core, make_input(RuntimeOwnerInputKind::BeginTransportAttempt));
        CHECK(saturated.disposition == RuntimeOwnerDisposition::FailClosed);
        CHECK(saturated.effect_count == 2);
        CHECK(core.view().mqtt_generation_counter == maximum);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
              RuntimeOwnerDisposition::Rejected);
    }

    {
        RuntimeOwnerCore core;
        RuntimeOwnerCoreTestPeer::seed_counters(core, 0, 0, maximum, 0);
        const RuntimeOwnerTransition begin = submit_checked(
            core, make_input(RuntimeOwnerInputKind::BeginTransportAttempt));
        CHECK(begin.disposition == RuntimeOwnerDisposition::Accepted);
        CHECK(begin.effect_count == 1);
        CHECK(core.view().last_correlation_id == maximum);
        establish(core, 7);
        const RuntimeOwnerView before = core.view();
        const RuntimeOwnerTransition saturated = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            before.mqtt_session_id,
            before.mqtt_generation,
            1));
        CHECK(saturated.disposition == RuntimeOwnerDisposition::FailClosed);
        CHECK(saturated.effect_count == 2);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
              RuntimeOwnerDisposition::Rejected);
    }

    {
        RuntimeOwnerCore core;
        begin_transport(core);
        establish(core, 7);
        RuntimeOwnerCoreTestPeer::seed_counters(
            core,
            core.view().mqtt_generation_counter,
            maximum,
            core.view().last_correlation_id,
            0);
        const RuntimeOwnerView before = core.view();
        const RuntimeOwnerTransition saturated = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            before.mqtt_session_id,
            before.mqtt_generation,
            1));
        CHECK(saturated.disposition == RuntimeOwnerDisposition::FailClosed);
        CHECK(saturated.effect_count == 2);
        CHECK(core.view().config_apply_epoch_counter == maximum);
        CHECK(core.view().last_config_commit_sequence == 0);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
              RuntimeOwnerDisposition::Rejected);
    }

    {
        RuntimeOwnerCore core;
        begin_transport(core);
        establish(core, 7);
        RuntimeOwnerCoreTestPeer::seed_counters(
            core,
            core.view().mqtt_generation_counter,
            maximum - 1,
            core.view().last_correlation_id,
            0);
        const auto tickets = commit_config(core, 1);
        CHECK(tickets[0].attempt.config_apply_epoch == maximum);
        const RuntimeOwnerView before = core.view();
        const RuntimeOwnerTransition saturated = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            before.mqtt_session_id,
            before.mqtt_generation,
            2));
        CHECK(saturated.disposition == RuntimeOwnerDisposition::FailClosed);
        CHECK(saturated.effect_count == 2);
        CHECK(core.view().config_apply_epoch_counter == maximum);
        CHECK(submit_checked(core, completion_input(tickets[0])).disposition ==
              RuntimeOwnerDisposition::Rejected);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
              RuntimeOwnerDisposition::Rejected);
    }

    {
        RuntimeOwnerCore core;
        begin_transport(core);
        establish(core, 7);
        RuntimeOwnerCoreTestPeer::seed_counters(
            core,
            core.view().mqtt_generation_counter,
            0,
            maximum - 5,
            0);
        const RuntimeOwnerView before = core.view();
        const RuntimeOwnerTransition saturated = submit_checked(core, make_input(
            RuntimeOwnerInputKind::ConfigActivationCommitted,
            RuntimeOwnerEffectKind::None,
            0,
            before.mqtt_session_id,
            before.mqtt_generation,
            1));
        CHECK(saturated.disposition == RuntimeOwnerDisposition::FailClosed);
        CHECK(saturated.effect_count == 2);
        CHECK(core.view().last_correlation_id == maximum - 5);
        CHECK(core.view().config_apply_epoch_counter == 0);
        CHECK(core.view().last_config_commit_sequence == 0);
        CHECK(submit_checked(
                  core,
                  make_input(RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
              RuntimeOwnerDisposition::Rejected);
    }

    {
        RuntimeOwnerCore core;
        begin_transport(core);
        establish(core, 7);
        RuntimeOwnerCoreTestPeer::seed_counters(
            core,
            core.view().mqtt_generation_counter,
            0,
            maximum - 6,
            0);
        const auto tickets = commit_config(core, 1);
        CHECK(core.view().last_correlation_id == maximum);
        const RuntimeOwnerEffect freeze = complete_all(core, tickets);
        CHECK(freeze.correlation_id == maximum - 1);
        const RuntimeOwnerEffect boot_end = finish_snapshot(core, freeze);
        CHECK(boot_end.correlation_id == maximum);
    }
}

void test_allocation_free_submit_and_view()
{
    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerCore core;
            CHECK(core.view().phase == RuntimeOwnerPhase::ColdStart);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }

    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerCore core;
            const auto tickets = prepare_liveness(core);
            const RuntimeOwnerEffect freeze = complete_all(core, tickets);
            finish_snapshot(core, freeze);
            CHECK(core.view().phase == RuntimeOwnerPhase::RuntimeReady);
            CHECK(submit_checked(
                      core,
                      snapshot_input(
                          RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
                          freeze)).disposition ==
                  RuntimeOwnerDisposition::AcceptedDuplicate);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }

    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerCore core;
            const auto old_tickets = prepare_liveness(core);
            RuntimeOwnerInput wrong = completion_input(old_tickets[0]);
            ++wrong.mqtt_generation;
            CHECK(submit_checked(core, wrong).disposition ==
                  RuntimeOwnerDisposition::Rejected);
            CHECK(submit_checked(
                      core,
                      completion_input(old_tickets[0])).disposition ==
                  RuntimeOwnerDisposition::Accepted);
            CHECK(submit_checked(
                      core,
                      completion_input(old_tickets[0])).disposition ==
                  RuntimeOwnerDisposition::AcceptedDuplicate);
            const auto new_tickets = commit_config(core, 2);
            CHECK(submit_checked(
                      core,
                      completion_input(old_tickets[1])).disposition ==
                  RuntimeOwnerDisposition::Rejected);
            complete_all(core, new_tickets);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }

    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerCore core;
            const auto tickets = prepare_liveness(core);
            const RuntimeOwnerInput failure = operation_failure_input(
                RuntimeOwnerInputKind::LivenessOperationFailed, tickets[0]);
            CHECK(submit_checked(core, failure).disposition ==
                  RuntimeOwnerDisposition::Accepted);
            CHECK(submit_checked(core, failure).disposition ==
                  RuntimeOwnerDisposition::AcceptedDuplicate);
            begin_transport(core);
            CHECK(submit_checked(core, failure).disposition ==
                  RuntimeOwnerDisposition::Rejected);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }

    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerCore core;
            const auto tickets = prepare_liveness(core);
            const RuntimeOwnerEffect freeze = complete_all(core, tickets);
            CHECK(submit_checked(
                      core,
                      snapshot_input(
                          RuntimeOwnerInputKind::SnapshotFreezeFailed,
                          freeze)).disposition == RuntimeOwnerDisposition::Accepted);
            begin_transport(core);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }

    {
        const std::size_t allocations_before = g_allocation_count;
        const std::size_t deallocations_before = g_deallocation_count;
        {
            RuntimeOwnerCore core;
            RuntimeOwnerCoreTestPeer::seed_counters(
                core,
                std::numeric_limits<std::uint32_t>::max(),
                0,
                0,
                0);
            CHECK(submit_checked(
                      core,
                      make_input(
                          RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
                  RuntimeOwnerDisposition::FailClosed);
            CHECK(submit_checked(
                      core,
                      make_input(
                          RuntimeOwnerInputKind::BeginTransportAttempt)).disposition ==
                  RuntimeOwnerDisposition::Rejected);
        }
        CHECK(g_allocation_count == allocations_before);
        CHECK(g_deallocation_count == deallocations_before);
    }
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
    test_type_contract();
    test_safe_defaults_unknown_enums_and_canonical_fields();
    test_per_kind_input_field_matrix();
    test_generated_input_field_matrix();
    test_transport_generation_establishment_and_failure();
    test_config_commit_binding_and_idempotence();
    test_all_subsets_and_permutations();
    test_rejected_receipts_leave_no_hidden_evidence();
    test_snapshot_receipt_mutation_matrix();
    test_duplicate_completion_and_snapshot_one_shot();
    test_new_config_restarts_pending_authorization();
    test_exact_operation_failure_and_deadline();
    test_disconnect_phase_invalidation_matrix();
    test_critical_fault_phase_matrix();
    test_failure_duplicate_and_restart_ledger_matrix();
    test_direct_runtime_ready_noise_and_sticky_latch();
    test_snapshot_failure_disconnect_critical_and_shutdown();
    test_same_slot_new_generation_rejects_old_ticket();
    test_counter_saturation_is_atomic_and_fail_closed();
    test_allocation_free_submit_and_view();

    if (g_failure_count != 0) {
        std::fprintf(
            stderr,
            "%zu of %zu checks failed\n",
            g_failure_count,
            g_check_count);
        return 1;
    }
    std::printf("runtime owner core: %zu checks passed\n", g_check_count);
    return 0;
}
