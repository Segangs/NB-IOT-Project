#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_CORE_HPP

#include <array>
#include <cstdint>

#include "liveness_service.hpp"

namespace boot_v2 {

enum class RuntimeOwnerPhase : std::uint8_t {
    ColdStart = 0,
    TransportConnecting = 1,
    AwaitingConfigCommit = 2,
    LivenessWaiting = 3,
    SnapshotFreezePending = 4,
    RuntimeReady = 5,
    RecoveryPending = 6,
    ShutdownCommitted = 7,
};

enum class RuntimeOwnerInputKind : std::uint8_t {
    Invalid = 0,
    BeginTransportAttempt = 1,
    TransportEstablished = 2,
    TransportAttemptFailed = 3,
    ConfigActivationCommitted = 4,
    LivenessOperationCompleted = 5,
    LivenessOperationFailed = 6,
    SnapshotFreezeSucceeded = 7,
    SnapshotFreezeFailed = 8,
    TransportDisconnected = 9,
    DeadlineExpired = 10,
    CriticalIngressFault = 11,
    ShutdownCommitted = 12,
};

enum class RuntimeOwnerEffectKind : std::uint8_t {
    None = 0,
    StartTransportAttempt = 1,
    StartAtProbe = 2,
    StartProbePublish = 3,
    VerifySubscription = 4,
    PullFollowupConfig = 5,
    FreezeBootSnapshot = 6,
    EndBootOrchestration = 7,
    RecordFault = 8,
    EnterRecovery = 9,
};

enum class RuntimeOwnerDisposition : std::uint8_t {
    Rejected = 0,
    Accepted = 1,
    AcceptedDuplicate = 2,
    FailClosed = 3,
};

enum class RuntimeOwnerFaultCode : std::uint8_t {
    None = 0,
    TransportFailure = 1,
    LivenessFailure = 2,
    SnapshotFailure = 3,
    TransportDisconnected = 4,
    DeadlineExpired = 5,
    CriticalIngress = 6,
    CounterSaturation = 7,
    InternalInvariant = 8,
};

struct RuntimeOwnerInput {
    RuntimeOwnerInputKind kind{RuntimeOwnerInputKind::Invalid};
    RuntimeOwnerEffectKind receipt_kind{RuntimeOwnerEffectKind::None};
    std::uint32_t correlation_id{0};
    std::uint32_t mqtt_session_id{0};
    std::uint32_t mqtt_generation{0};
    std::uint32_t config_commit_sequence{0};
    std::uint32_t config_apply_epoch{0};
};

struct RuntimeOwnerEffect {
    RuntimeOwnerEffectKind kind{RuntimeOwnerEffectKind::None};
    std::uint32_t correlation_id{0};
    LivenessAttemptToken attempt{};
    RuntimeOwnerFaultCode fault_code{RuntimeOwnerFaultCode::None};
};

struct RuntimeOwnerTransition {
    RuntimeOwnerDisposition disposition{RuntimeOwnerDisposition::Rejected};
    RuntimeOwnerPhase phase_before{RuntimeOwnerPhase::ColdStart};
    RuntimeOwnerPhase phase_after{RuntimeOwnerPhase::ColdStart};
    std::uint8_t effect_count{0};
    std::array<RuntimeOwnerEffect, 4> effects{};
};

struct RuntimeOwnerView {
    RuntimeOwnerPhase phase{RuntimeOwnerPhase::ColdStart};
    std::uint32_t mqtt_session_id{0};
    std::uint32_t mqtt_generation{0};
    std::uint32_t mqtt_generation_counter{0};
    std::uint32_t config_apply_epoch_counter{0};
    std::uint32_t last_config_commit_sequence{0};
    std::uint32_t last_correlation_id{0};
    LivenessAttemptToken active_attempt{};
    bool boot_orchestration_ended{false};
    RuntimeOwnerFaultCode last_fault{RuntimeOwnerFaultCode::None};
};

// Pure transition oracle for a future lifecycle owner. All outputs are values;
// this class performs no external side effects.
class RuntimeOwnerCore {
public:
    RuntimeOwnerCore() noexcept = default;
    RuntimeOwnerCore(const RuntimeOwnerCore &) = delete;
    RuntimeOwnerCore &operator=(const RuntimeOwnerCore &) = delete;
    RuntimeOwnerCore(RuntimeOwnerCore &&) = delete;
    RuntimeOwnerCore &operator=(RuntimeOwnerCore &&) = delete;
    ~RuntimeOwnerCore() noexcept = default;

    [[nodiscard]] RuntimeOwnerTransition submit(
        RuntimeOwnerInput input) noexcept;
    [[nodiscard]] RuntimeOwnerView view() const noexcept;

private:
#if defined(NB_IOT_RUNTIME_OWNER_TESTING)
    friend class RuntimeOwnerCoreTestPeer;
#endif

    [[nodiscard]] static bool input_has_canonical_fields(
        RuntimeOwnerInput input) noexcept;
    [[nodiscard]] static bool input_equals(
        RuntimeOwnerInput left,
        RuntimeOwnerInput right) noexcept;
    [[nodiscard]] static bool is_liveness_effect(
        RuntimeOwnerEffectKind kind) noexcept;
    [[nodiscard]] static std::uint8_t liveness_index(
        RuntimeOwnerEffectKind kind) noexcept;
    [[nodiscard]] static LivenessServiceCommandKind liveness_command_kind(
        RuntimeOwnerEffectKind kind) noexcept;

    [[nodiscard]] RuntimeOwnerTransition rejected(
        RuntimeOwnerPhase before) const noexcept;
    [[nodiscard]] RuntimeOwnerTransition fail_closed(
        RuntimeOwnerPhase before,
        RuntimeOwnerFaultCode fault,
        std::uint32_t source_correlation,
        LivenessAttemptToken source_attempt) noexcept;
    [[nodiscard]] RuntimeOwnerTransition accept_failure(
        RuntimeOwnerPhase before,
        RuntimeOwnerInput input,
        RuntimeOwnerFaultCode fault,
        std::uint32_t source_correlation,
        LivenessAttemptToken source_attempt) noexcept;

    void invalidate_authorization(bool clear_transport) noexcept;
    static void append_effect(
        RuntimeOwnerTransition &transition,
        RuntimeOwnerEffectKind kind,
        std::uint32_t correlation_id,
        LivenessAttemptToken attempt,
        RuntimeOwnerFaultCode fault) noexcept;

    PostConfigLivenessCommandBoundary liveness_boundary_{};
    RuntimeOwnerPhase phase_{RuntimeOwnerPhase::ColdStart};
    RuntimeOwnerFaultCode last_fault_{RuntimeOwnerFaultCode::None};
    std::uint32_t mqtt_generation_counter_{0};
    std::uint32_t active_mqtt_session_id_{0};
    std::uint32_t active_mqtt_generation_{0};
    std::uint32_t config_apply_epoch_counter_{0};
    std::uint32_t last_config_commit_sequence_{0};
    std::uint32_t correlation_id_counter_{0};
    LivenessAttemptToken active_attempt_{};
    std::array<RuntimeOwnerEffect, 4> active_tickets_{};
    std::uint8_t accepted_liveness_mask_{0};
    std::uint32_t pending_snapshot_effect_id_{0};
    std::uint32_t pending_boot_end_effect_id_{0};
    bool boot_orchestration_ended_{false};
    bool fatal_latched_{false};
    bool has_last_failure_{false};
    RuntimeOwnerInput last_failure_{};
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_CORE_HPP
