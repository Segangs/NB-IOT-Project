#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_TASK_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_TASK_CORE_HPP

#include <cstdint>
#include <type_traits>

#include "runtime_owner_adapter_core.hpp"
#include "runtime_owner_executor_contract.hpp"
#include "runtime_snapshot_core.hpp"

namespace boot_v2 {

enum class RuntimeOwnerIngressResult : std::uint8_t {
    RejectedNotStarted = 0,
    RejectedInactive = 1,
    RejectedFull = 2,
    AcceptedForDelivery = 3,
    RejectedInvalid = 4,
};

enum class RuntimeOwnerIngressGateResult : std::uint8_t {
    RejectedNotStarted = 0,
    RejectedInactive = 1,
    Open = 2,
};

[[nodiscard]] constexpr RuntimeOwnerIngressGateResult
runtime_owner_ingress_gate(bool started, bool ingress_enabled) noexcept
{
    if (!started) {
        return RuntimeOwnerIngressGateResult::RejectedNotStarted;
    }
    return ingress_enabled ? RuntimeOwnerIngressGateResult::Open
                           : RuntimeOwnerIngressGateResult::RejectedInactive;
}

enum class RuntimeOwnerStartResult : std::uint8_t {
    Failed = 0,
    Started = 1,
    AlreadyStarted = 2,
};

enum class RuntimeOwnerControlKind : std::uint8_t {
    Invalid = 0,
    RequestTransportAttempt = 1,
};

struct RuntimeOwnerControlMessage {
    RuntimeOwnerControlKind kind{RuntimeOwnerControlKind::Invalid};
    std::uint8_t reserved[3]{};
};

struct RuntimeOwnerRtosStatus {
    std::uint32_t wake_count{0};
    std::uint32_t rejected_inactive_count{0};
    std::uint32_t rejected_full_count{0};
    std::uint8_t started{0};
    std::uint8_t ingress_enabled{0};
    std::uint8_t start_failed{0};
    std::uint8_t reserved{0};
};

enum class RuntimeOwnerTaskState : std::uint8_t {
    Dormant = 0,
    Active = 1,
    Terminal = 2,
};

enum class RuntimeOwnerTaskActivationResult : std::uint8_t {
    RejectedInvalid = 0,
    Activated = 1,
    AlreadyActive = 2,
    RejectedTerminal = 3,
};

enum class RuntimeOwnerExecutorResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedInactive = 1,
    RejectedNoCommand = 2,
    RejectedWrongCommand = 3,
    RejectedEndBootRequiresCommit = 4,
    RejectedNotAllowed = 5,
    RejectedSnapshotStore = 6,
    RejectedTerminalDropped = 7,
    Accepted = 8,
    AcceptedDuplicate = 9,
};

enum class RuntimeOwnerShutdownRequestResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedInactive = 1,
    RejectedStale = 2,
    RejectedTerminal = 3,
    Accepted = 4,
    AcceptedDuplicate = 5,
};

enum class RuntimeOwnerTaskCycleDisposition : std::uint8_t {
    RejectedInactive = 0,
    RejectedTerminal = 1,
    Processed = 2,
};

enum class RuntimeOwnerTaskWorkKind : std::uint8_t {
    None = 0,
    RequestTransportAttempt = 1,
    NormalIntent = 2,
};

struct RuntimeOwnerTaskCycleInput {
    std::uint8_t reserved_source_only{0};
    std::uint8_t transport_pending{0};
    std::uint8_t normal_pending{0};
    std::uint8_t reserved{0};
    NormalIntent normal{};
};

struct RuntimeOwnerRedactedStatus {
    RuntimeOwnerTaskState state{RuntimeOwnerTaskState::Dormant};
    RuntimeOwnerPhase phase{RuntimeOwnerPhase::ColdStart};
    std::uint8_t runtime_ready{0};
    std::uint8_t shutdown_latched{0};
    std::uint32_t normal_cancelled_count{0};
    std::uint32_t effect_cancelled_count{0};
};

class RuntimeOwnerTaskCore;
class RuntimeOwnerRtosOwnerLoop;
class RuntimeOwnerCutoverCoordinator;

class RuntimeOwnerCutoverPermit {
public:
    RuntimeOwnerCutoverPermit() = delete;
    RuntimeOwnerCutoverPermit(const RuntimeOwnerCutoverPermit &) = delete;
    RuntimeOwnerCutoverPermit &operator=(
        const RuntimeOwnerCutoverPermit &) = delete;
    RuntimeOwnerCutoverPermit(RuntimeOwnerCutoverPermit &&) = delete;
    RuntimeOwnerCutoverPermit &operator=(RuntimeOwnerCutoverPermit &&) = delete;
    ~RuntimeOwnerCutoverPermit() noexcept = default;

private:
    friend class RuntimeOwnerTaskCore;
    friend class RuntimeOwnerRtosOwnerLoop;
    friend class RuntimeOwnerCutoverCoordinator;
#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
    friend class RuntimeOwnerTaskCoreTestPeer;
#endif

    explicit constexpr RuntimeOwnerCutoverPermit(
        const std::uint32_t stable_identity) noexcept
        : stable_identity_(stable_identity)
    {
    }

    const std::uint32_t stable_identity_;
};

class RuntimeOwnerExecutorPort {
public:
    RuntimeOwnerExecutorPort() = delete;
    RuntimeOwnerExecutorPort(const RuntimeOwnerExecutorPort &) = delete;
    RuntimeOwnerExecutorPort &operator=(const RuntimeOwnerExecutorPort &) =
        delete;
    RuntimeOwnerExecutorPort(RuntimeOwnerExecutorPort &&) = delete;
    RuntimeOwnerExecutorPort &operator=(RuntimeOwnerExecutorPort &&) = delete;
    ~RuntimeOwnerExecutorPort() noexcept = default;

    [[nodiscard]] RuntimeOwnerExecutorResult peek_command(
        RuntimeOwnerExecutorCommand &command) const noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult acknowledge_command(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult transport_established(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t mqtt_session_id) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult transport_failed(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult transport_disconnected(
        std::uint32_t mqtt_session_id,
        std::uint32_t mqtt_generation,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult config_committed(
        std::uint32_t config_commit_sequence) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult liveness_succeeded(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult liveness_failed(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult liveness_deadline_expired(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult snapshot_succeeded(
        RuntimeOwnerExecutorCommand command,
        BootRuntimeSnapshotV1 snapshot) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult snapshot_failed(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult commit_end_boot_delivery(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult publish_runtime(
        RuntimeStatusSnapshotV1 snapshot) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult normal_succeeded(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult normal_failed(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult normal_timed_out(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult normal_cancelled(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;

private:
    friend class RuntimeOwnerTaskCore;
    friend class RuntimeOwnerRtosOwnerLoop;
#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
    friend class RuntimeOwnerTaskCoreTestPeer;
#endif

    explicit RuntimeOwnerExecutorPort(RuntimeOwnerTaskCore *owner) noexcept
        : owner_(owner)
    {
    }

    RuntimeOwnerTaskCore *owner_;
};

class RuntimeOwnerPowerButtonShutdownPort {
public:
    RuntimeOwnerPowerButtonShutdownPort() = delete;
    RuntimeOwnerPowerButtonShutdownPort(
        const RuntimeOwnerPowerButtonShutdownPort &) = delete;
    RuntimeOwnerPowerButtonShutdownPort &operator=(
        const RuntimeOwnerPowerButtonShutdownPort &) = delete;
    RuntimeOwnerPowerButtonShutdownPort(
        RuntimeOwnerPowerButtonShutdownPort &&) = delete;
    RuntimeOwnerPowerButtonShutdownPort &operator=(
        RuntimeOwnerPowerButtonShutdownPort &&) = delete;
    ~RuntimeOwnerPowerButtonShutdownPort() noexcept = default;

    [[nodiscard]] RuntimeOwnerShutdownRequestResult request(
        std::uint32_t producer_sequence,
        std::uint32_t incident_correlation_id) noexcept;

private:
    friend class RuntimeOwnerTaskCore;
    friend class RuntimeOwnerRtosOwnerLoop;
#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
    friend class RuntimeOwnerTaskCoreTestPeer;
#endif
    explicit RuntimeOwnerPowerButtonShutdownPort(
        RuntimeOwnerTaskCore *owner) noexcept : owner_(owner) {}
    RuntimeOwnerTaskCore *owner_;
};

class RuntimeOwnerAdapterLossShutdownPort {
public:
    RuntimeOwnerAdapterLossShutdownPort() = delete;
    RuntimeOwnerAdapterLossShutdownPort(
        const RuntimeOwnerAdapterLossShutdownPort &) = delete;
    RuntimeOwnerAdapterLossShutdownPort &operator=(
        const RuntimeOwnerAdapterLossShutdownPort &) = delete;
    RuntimeOwnerAdapterLossShutdownPort(
        RuntimeOwnerAdapterLossShutdownPort &&) = delete;
    RuntimeOwnerAdapterLossShutdownPort &operator=(
        RuntimeOwnerAdapterLossShutdownPort &&) = delete;
    ~RuntimeOwnerAdapterLossShutdownPort() noexcept = default;

    [[nodiscard]] RuntimeOwnerShutdownRequestResult request(
        std::uint32_t producer_sequence,
        std::uint32_t incident_correlation_id) noexcept;

private:
    friend class RuntimeOwnerTaskCore;
    friend class RuntimeOwnerRtosOwnerLoop;
#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
    friend class RuntimeOwnerTaskCoreTestPeer;
#endif
    explicit RuntimeOwnerAdapterLossShutdownPort(
        RuntimeOwnerTaskCore *owner) noexcept : owner_(owner) {}
    RuntimeOwnerTaskCore *owner_;
};

class RuntimeOwnerAuthenticatedCommandShutdownPort {
public:
    RuntimeOwnerAuthenticatedCommandShutdownPort() = delete;
    RuntimeOwnerAuthenticatedCommandShutdownPort(
        const RuntimeOwnerAuthenticatedCommandShutdownPort &) = delete;
    RuntimeOwnerAuthenticatedCommandShutdownPort &operator=(
        const RuntimeOwnerAuthenticatedCommandShutdownPort &) = delete;
    RuntimeOwnerAuthenticatedCommandShutdownPort(
        RuntimeOwnerAuthenticatedCommandShutdownPort &&) = delete;
    RuntimeOwnerAuthenticatedCommandShutdownPort &operator=(
        RuntimeOwnerAuthenticatedCommandShutdownPort &&) = delete;
    ~RuntimeOwnerAuthenticatedCommandShutdownPort() noexcept = default;

    [[nodiscard]] RuntimeOwnerShutdownRequestResult request(
        std::uint32_t producer_sequence,
        std::uint32_t incident_correlation_id) noexcept;

private:
    friend class RuntimeOwnerTaskCore;
    friend class RuntimeOwnerRtosOwnerLoop;
#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
    friend class RuntimeOwnerTaskCoreTestPeer;
#endif
    explicit RuntimeOwnerAuthenticatedCommandShutdownPort(
        RuntimeOwnerTaskCore *owner) noexcept : owner_(owner) {}
    RuntimeOwnerTaskCore *owner_;
};

struct RuntimeOwnerTaskCycleResult {
    RuntimeOwnerTaskCycleDisposition disposition{
        RuntimeOwnerTaskCycleDisposition::RejectedInactive};
    RuntimeOwnerTaskWorkKind selected_work{RuntimeOwnerTaskWorkKind::None};
    OwnerRequestResult transport_result{
        OwnerRequestResult::RejectedNotAllowed};
    NormalSubmitResult normal_result{NormalSubmitResult::RejectedNotReady};
    std::uint8_t urgent_recheck_required{0};
    std::uint8_t dispatch_pending{0};
    std::uint8_t reserved{0};
    AdapterStepResult step_result{};
};

#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
class RuntimeOwnerTaskCoreTestPeer;
#endif

class RuntimeOwnerTaskCore {
public:
    explicit RuntimeOwnerTaskCore() noexcept = default;
    RuntimeOwnerTaskCore(const RuntimeOwnerTaskCore &) = delete;
    RuntimeOwnerTaskCore &operator=(const RuntimeOwnerTaskCore &) = delete;
    RuntimeOwnerTaskCore(RuntimeOwnerTaskCore &&) = delete;
    RuntimeOwnerTaskCore &operator=(RuntimeOwnerTaskCore &&) = delete;
    ~RuntimeOwnerTaskCore() noexcept = default;

    [[nodiscard]] RuntimeOwnerTaskActivationResult activate(
        const RuntimeOwnerCutoverPermit &permit) noexcept;
    [[nodiscard]] RuntimeOwnerRedactedStatus redacted_status() const noexcept;

private:
    friend class RuntimeOwnerRtosOwnerLoop;
    friend class RuntimeOwnerExecutorPort;
    friend class RuntimeOwnerPowerButtonShutdownPort;
    friend class RuntimeOwnerAdapterLossShutdownPort;
    friend class RuntimeOwnerAuthenticatedCommandShutdownPort;
#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
    friend class RuntimeOwnerTaskCoreTestPeer;
#endif

    enum class ShutdownProducer : std::uint8_t {
        Invalid = 0,
        PowerButton = 1,
        AdapterLossCommitted = 2,
        AuthenticatedRemoteCommand = 3,
    };

    [[nodiscard]] RuntimeOwnerTaskCycleResult process_cycle(
        RuntimeOwnerTaskCycleInput input) noexcept;

    [[nodiscard]] RuntimeOwnerExecutorPort executor_port() & noexcept;
    [[nodiscard]] RuntimeOwnerExecutorPort executor_port() && = delete;
    [[nodiscard]] RuntimeOwnerPowerButtonShutdownPort
        power_button_shutdown_port() & noexcept;
    [[nodiscard]] RuntimeOwnerPowerButtonShutdownPort
        power_button_shutdown_port() && = delete;
    [[nodiscard]] RuntimeOwnerAdapterLossShutdownPort
        adapter_loss_shutdown_port() & noexcept;
    [[nodiscard]] RuntimeOwnerAdapterLossShutdownPort
        adapter_loss_shutdown_port() && = delete;
    [[nodiscard]] RuntimeOwnerAuthenticatedCommandShutdownPort
        authenticated_command_shutdown_port() & noexcept;
    [[nodiscard]] RuntimeOwnerAuthenticatedCommandShutdownPort
        authenticated_command_shutdown_port() && = delete;

    [[nodiscard]] RuntimeOwnerExecutorResult executor_peek_command(
        RuntimeOwnerExecutorCommand &command) const noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_acknowledge_command(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_transport_established(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t mqtt_session_id) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_transport_failed(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_transport_disconnected(
        std::uint32_t mqtt_session_id,
        std::uint32_t mqtt_generation,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_config_committed(
        std::uint32_t config_commit_sequence) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_liveness_result(
        RuntimeOwnerExecutorCommand command,
        TrustedReceiptKind receipt_kind,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_snapshot_succeeded(
        RuntimeOwnerExecutorCommand command,
        BootRuntimeSnapshotV1 snapshot) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_snapshot_failed(
        RuntimeOwnerExecutorCommand command,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult commit_end_boot_delivery(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_publish_runtime(
        RuntimeStatusSnapshotV1 snapshot) noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_normal_result(
        RuntimeOwnerExecutorCommand command,
        NormalCompletionKind kind,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] RuntimeOwnerShutdownRequestResult request_shutdown(
        ShutdownProducer producer,
        std::uint32_t producer_sequence,
        std::uint32_t incident_correlation_id) noexcept;
    [[nodiscard]] bool shutdown_invariant_holds() const noexcept;
    [[nodiscard]] RuntimeOwnerExecutorResult executor_state_gate() const noexcept;
    void arm_runtime_admission_for_cycle() noexcept;

    RuntimeOwnerAdapterCore adapter_{};
    RuntimeSnapshotCore snapshot_{};
    RuntimeOwnerTaskState state_{RuntimeOwnerTaskState::Dormant};
    RuntimeOwnerTaskCycleResult last_result_{};
    RuntimeActivationGrant pending_activation_grant_{};
    std::uint32_t active_permit_identity_{0};
    std::uint32_t shutdown_producer_sequence_[3]{};
    std::uint32_t shutdown_incident_correlation_id_{0};
    ShutdownProducer shutdown_producer_{ShutdownProducer::Invalid};
    std::uint8_t snapshot_frozen_latched_{0};
    std::uint8_t ready_commit_latched_{0};
    std::uint8_t runtime_admission_open_{0};
    std::uint8_t shutdown_provenance_valid_{0};
};

inline RuntimeOwnerExecutorPort RuntimeOwnerTaskCore::executor_port() & noexcept
{
    return RuntimeOwnerExecutorPort{this};
}

inline RuntimeOwnerPowerButtonShutdownPort
RuntimeOwnerTaskCore::power_button_shutdown_port() & noexcept
{
    return RuntimeOwnerPowerButtonShutdownPort{this};
}

inline RuntimeOwnerAdapterLossShutdownPort
RuntimeOwnerTaskCore::adapter_loss_shutdown_port() & noexcept
{
    return RuntimeOwnerAdapterLossShutdownPort{this};
}

inline RuntimeOwnerAuthenticatedCommandShutdownPort
RuntimeOwnerTaskCore::authenticated_command_shutdown_port() & noexcept
{
    return RuntimeOwnerAuthenticatedCommandShutdownPort{this};
}

#if defined(NB_IOT_RUNTIME_OWNER_TASK_TESTING)
class RuntimeOwnerTaskCoreTestPeer {
public:
    [[nodiscard]] static RuntimeOwnerTaskCycleResult process_cycle(
        RuntimeOwnerTaskCore &core,
        RuntimeOwnerTaskCycleInput input) noexcept;
    static void fixture_activate(RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static RuntimeOwnerTaskActivationResult fixture_activate(
        RuntimeOwnerTaskCore &core,
        std::uint32_t stable_identity) noexcept;
    static void fixture_terminal(RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static RuntimeOwnerTaskState state(
        const RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static bool runtime_admission_open(
        const RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static RuntimeOwnerAdapterView adapter_view(
        const RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static RuntimeOwnerExecutorPort executor_port(
        RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static RuntimeOwnerPowerButtonShutdownPort
        power_button_shutdown_port(RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static RuntimeOwnerAdapterLossShutdownPort
        adapter_loss_shutdown_port(RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static RuntimeOwnerAuthenticatedCommandShutdownPort
        authenticated_command_shutdown_port(
            RuntimeOwnerTaskCore &core) noexcept;
    [[nodiscard]] static bool shutdown_invariant_holds(
        const RuntimeOwnerTaskCore &core) noexcept;
};
#endif

namespace runtime_owner_task_detail {

template <typename... Fields>
constexpr bool has_only_nonowning_value_fields =
    ((!std::is_pointer<Fields>::value &&
      !std::is_reference<Fields>::value &&
      std::is_trivially_copyable<Fields>::value) && ...);

template <typename Enum>
constexpr bool has_uint8_underlying_type =
    std::is_same<typename std::underlying_type<Enum>::type,
                 std::uint8_t>::value;

static_assert(has_uint8_underlying_type<RuntimeOwnerIngressResult>);
static_assert(has_uint8_underlying_type<RuntimeOwnerIngressGateResult>);
static_assert(has_uint8_underlying_type<RuntimeOwnerStartResult>);
static_assert(has_uint8_underlying_type<RuntimeOwnerControlKind>);
static_assert(has_uint8_underlying_type<RuntimeOwnerTaskState>);
static_assert(has_uint8_underlying_type<RuntimeOwnerTaskActivationResult>);
static_assert(has_uint8_underlying_type<RuntimeOwnerExecutorResult>);
static_assert(has_uint8_underlying_type<RuntimeOwnerShutdownRequestResult>);
static_assert(static_cast<std::uint8_t>(
                  RuntimeOwnerTaskActivationResult::RejectedInvalid) == 0);
static_assert(static_cast<std::uint8_t>(
                  RuntimeOwnerTaskActivationResult::Activated) == 1);
static_assert(static_cast<std::uint8_t>(
                  RuntimeOwnerTaskActivationResult::AlreadyActive) == 2);
static_assert(static_cast<std::uint8_t>(
                  RuntimeOwnerTaskActivationResult::RejectedTerminal) == 3);
static_assert(has_uint8_underlying_type<RuntimeOwnerTaskCycleDisposition>);
static_assert(has_uint8_underlying_type<RuntimeOwnerTaskWorkKind>);

static_assert(sizeof(RuntimeOwnerControlMessage) == 4);
static_assert(alignof(RuntimeOwnerControlMessage) == 1);
static_assert(sizeof(RuntimeOwnerRtosStatus) == 16);
static_assert(alignof(RuntimeOwnerRtosStatus) == 4);
static_assert(sizeof(RuntimeOwnerTaskCycleInput) == 16);
static_assert(alignof(RuntimeOwnerTaskCycleInput) == 4);
static_assert(sizeof(RuntimeOwnerTaskCycleResult) == 24);
static_assert(alignof(RuntimeOwnerTaskCycleResult) == 4);
static_assert(sizeof(RuntimeOwnerRedactedStatus) == 12);
static_assert(alignof(RuntimeOwnerRedactedStatus) == 4);

static_assert(std::is_standard_layout<RuntimeOwnerControlMessage>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerControlMessage>::value);
static_assert(std::is_standard_layout<RuntimeOwnerRtosStatus>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerRtosStatus>::value);
static_assert(std::is_standard_layout<RuntimeOwnerTaskCycleInput>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerTaskCycleInput>::value);
static_assert(std::is_standard_layout<RuntimeOwnerTaskCycleResult>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerTaskCycleResult>::value);
static_assert(std::is_standard_layout<RuntimeOwnerRedactedStatus>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerRedactedStatus>::value);

static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerControlMessage::kind),
              decltype(RuntimeOwnerControlMessage::reserved)>);
static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerRtosStatus::wake_count),
              decltype(RuntimeOwnerRtosStatus::rejected_inactive_count),
              decltype(RuntimeOwnerRtosStatus::rejected_full_count),
              decltype(RuntimeOwnerRtosStatus::started),
              decltype(RuntimeOwnerRtosStatus::ingress_enabled),
              decltype(RuntimeOwnerRtosStatus::start_failed),
              decltype(RuntimeOwnerRtosStatus::reserved)>);
static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerTaskCycleInput::reserved_source_only),
              decltype(RuntimeOwnerTaskCycleInput::transport_pending),
              decltype(RuntimeOwnerTaskCycleInput::normal_pending),
              decltype(RuntimeOwnerTaskCycleInput::reserved),
              decltype(RuntimeOwnerTaskCycleInput::normal)>);
static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerTaskCycleResult::disposition),
              decltype(RuntimeOwnerTaskCycleResult::selected_work),
              decltype(RuntimeOwnerTaskCycleResult::transport_result),
              decltype(RuntimeOwnerTaskCycleResult::normal_result),
              decltype(
                  RuntimeOwnerTaskCycleResult::urgent_recheck_required),
              decltype(RuntimeOwnerTaskCycleResult::dispatch_pending),
              decltype(RuntimeOwnerTaskCycleResult::reserved),
              decltype(RuntimeOwnerTaskCycleResult::step_result)>);
static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerRedactedStatus::state),
              decltype(RuntimeOwnerRedactedStatus::phase),
              decltype(RuntimeOwnerRedactedStatus::runtime_ready),
              decltype(RuntimeOwnerRedactedStatus::shutdown_latched),
              decltype(RuntimeOwnerRedactedStatus::normal_cancelled_count),
              decltype(RuntimeOwnerRedactedStatus::effect_cancelled_count)>);

} // namespace runtime_owner_task_detail

} // namespace boot_v2

#endif
