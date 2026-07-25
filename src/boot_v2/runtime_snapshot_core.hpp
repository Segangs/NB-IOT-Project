#ifndef NB_IOT_BOOT_V2_RUNTIME_SNAPSHOT_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_SNAPSHOT_CORE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "runtime_owner_core.hpp"

namespace boot_v2 {

enum class SnapshotHealth : std::uint8_t {
    Unknown = 0,
    Pass = 1,
    Degraded = 2,
    Failed = 3,
};

enum class BootCompletedStage : std::uint8_t {
    Invalid = 0,
    TransportEstablished = 1,
    ConfigActivated = 2,
    PostConfigLivenessPassed = 3,
    ConfigAppliedHandoff = 4,
};

enum class SensorValueSource : std::uint8_t {
    None = 0,
    Fresh = 1,
    CrcFallback = 2,
};

enum class RuntimeNetworkState : std::uint8_t {
    Invalid = 0,
    Offline = 1,
    Online = 2,
    Recovering = 3,
};

enum class RuntimeAdapterState : std::uint8_t {
    Invalid = 0,
    Present = 1,
    Absent = 2,
};

enum class RuntimePowerState : std::uint8_t {
    Invalid = 0,
    ExternalPower = 1,
    Grace = 2,
    Committed = 3,
    Cleanup = 4,
    Off = 5,
};

enum class RuntimeAlarmState : std::uint8_t {
    Invalid = 0,
    Clear = 1,
    Active = 2,
};

enum class RuntimeUiState : std::uint8_t {
    Invalid = 0,
    Boot = 1,
    PeriodicReady = 2,
    Shutdown = 3,
};

enum class RuntimeCommandResult : std::uint8_t {
    Invalid = 0,
    NoCommand = 1,
    Succeeded = 2,
    Failed = 3,
    Rejected = 4,
};

struct SensorQualitySnapshotV1 {
    std::uint8_t schema_version{1};
    SnapshotHealth health{SnapshotHealth::Unknown};
    std::uint8_t has_value{0};
    SensorValueSource value_source{SensorValueSource::None};
    std::uint8_t stale{0};
    std::uint8_t clock_valid{0};
    std::uint8_t reserved[2]{};
    std::int16_t value_deci_celsius{0};
    std::uint16_t consecutive_failures{0};
    std::uint32_t last_valid_at_unix_seconds{0};
};

struct BootRuntimeSnapshotV1 {
    std::uint8_t schema_version{1};
    SnapshotHealth health{SnapshotHealth::Unknown};
    BootCompletedStage last_completed_stage{BootCompletedStage::Invalid};
    std::uint8_t config_valid{0};
    std::uint8_t transport_ready{0};
    std::uint8_t subscription_alive{0};
    std::uint8_t post_config_liveness{0};
    std::uint8_t reboot_guard{0};
    std::uint8_t reserved[4]{};
    std::uint32_t hardware_revision{0};
    std::uint32_t firmware_build_id{0};
    std::uint32_t config_version{0};
    std::array<SensorQualitySnapshotV1, 2> sensors{};
    std::uint32_t pdp_session_id{0};
    std::uint32_t mqtt_session_id{0};
    std::uint32_t mqtt_generation{0};
    std::uint32_t config_apply_epoch{0};
    std::uint32_t recovery_summary{0};
};

struct RuntimeStatusSnapshotV1 {
    std::uint8_t schema_version{1};
    RuntimeNetworkState network_state{RuntimeNetworkState::Invalid};
    RuntimeAdapterState adapter_state{RuntimeAdapterState::Invalid};
    RuntimePowerState power_state{RuntimePowerState::Invalid};
    RuntimeAlarmState alarm_state{RuntimeAlarmState::Invalid};
    RuntimeUiState ui_state{RuntimeUiState::Invalid};
    RuntimeCommandResult last_command_result{RuntimeCommandResult::Invalid};
    std::uint8_t reserved{0};
    std::uint32_t revision{0};
    std::array<SensorQualitySnapshotV1, 2> sensors{};
    std::uint32_t config_version{0};
    std::uint32_t last_command_id{0};
    std::uint32_t queue_summary{0};
    std::uint32_t drop_summary{0};
    std::uint32_t recovery_summary{0};
};

struct RuntimeActivationGrant {
    LivenessAttemptToken liveness{};
    std::uint32_t snapshot_correlation_id{0};
    std::uint32_t boot_end_correlation_id{0};
    std::uint32_t boot_end_dispatch_sequence{0};
};

[[nodiscard]] bool sensor_quality_snapshot_is_canonical(
    const SensorQualitySnapshotV1 &snapshot) noexcept;
[[nodiscard]] bool boot_runtime_snapshot_is_canonical(
    const BootRuntimeSnapshotV1 &snapshot) noexcept;
[[nodiscard]] bool runtime_status_snapshot_is_canonical(
    const RuntimeStatusSnapshotV1 &snapshot) noexcept;
[[nodiscard]] bool runtime_activation_grant_is_canonical(
    const RuntimeActivationGrant &grant) noexcept;

[[nodiscard]] bool sensor_quality_snapshots_equal(
    const SensorQualitySnapshotV1 &left,
    const SensorQualitySnapshotV1 &right) noexcept;
[[nodiscard]] bool boot_runtime_snapshots_equal(
    const BootRuntimeSnapshotV1 &left,
    const BootRuntimeSnapshotV1 &right) noexcept;
[[nodiscard]] bool runtime_status_snapshots_equal(
    const RuntimeStatusSnapshotV1 &left,
    const RuntimeStatusSnapshotV1 &right) noexcept;
[[nodiscard]] bool runtime_activation_grants_equal(
    const RuntimeActivationGrant &left,
    const RuntimeActivationGrant &right) noexcept;

enum class BootSnapshotFreezeResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedBusy = 1,
    RejectedAlreadyFrozen = 2,
    Accepted = 3,
    AcceptedDuplicate = 4,
};

enum class SnapshotCopyResult : std::uint8_t {
    RejectedBusy = 0,
    RejectedNotReady = 1,
    Copied = 2,
};

enum class ReadyPrepareResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedBusy = 1,
    RejectedNotFrozen = 2,
    Prepared = 3,
    AcceptedDuplicate = 4,
};

enum class RuntimeSnapshotPublishResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedBusy = 1,
    RejectedNotReady = 2,
    RejectedRevisionZero = 3,
    RejectedRevisionNotIncreasing = 4,
    RejectedRevisionSaturated = 5,
    Accepted = 6,
};

enum class ShutdownFinalAction : std::uint8_t {
    Invalid = 0,
    Gp15Kill = 1,
    WatchdogReboot = 2,
};

enum class ShutdownFinalizationPlanResult : std::uint8_t {
    RejectedInvalid = 0,
    Planned = 1,
};

struct UsbPowerObservation {
    std::uint8_t present{0};
    std::uint8_t cleanup_skipped_mask{0};
    std::uint8_t cleanup_timed_out_mask{0};
    std::uint8_t hard_deadline_reached{0};
    std::uint32_t sample_sequence{0};
    std::uint32_t sampled_at_monotonic_ms{0};
    std::uint8_t reserved[4]{};
};

struct ShutdownFinalizationPlan {
    std::uint8_t schema_version{1};
    ShutdownFinalAction intended_action{ShutdownFinalAction::Invalid};
    std::uint8_t cleanup_skipped_mask{0};
    std::uint8_t cleanup_timed_out_mask{0};
    std::uint8_t hard_deadline_reached{0};
    std::uint8_t live_actuation{0};
    std::uint8_t reserved[2]{};
    std::uint32_t sample_sequence{0};
    std::uint32_t sampled_at_monotonic_ms{0};
};

[[nodiscard]] bool usb_power_observation_is_canonical(
    const UsbPowerObservation &observation) noexcept;
[[nodiscard]] bool shutdown_finalization_plan_is_canonical(
    const ShutdownFinalizationPlan &plan) noexcept;
[[nodiscard]] bool usb_power_observations_equal(
    const UsbPowerObservation &left,
    const UsbPowerObservation &right) noexcept;
[[nodiscard]] bool shutdown_finalization_plans_equal(
    const ShutdownFinalizationPlan &left,
    const ShutdownFinalizationPlan &right) noexcept;
[[nodiscard]] ShutdownFinalizationPlanResult plan_shutdown_finalization(
    UsbPowerObservation observation,
    ShutdownFinalizationPlan &output) noexcept;

class RuntimeSnapshotCore;
class RuntimeOwnerTaskCore;
#if defined(NB_IOT_RUNTIME_SNAPSHOT_TESTING)
class RuntimeSnapshotCoreTestPeer;
#endif

class RuntimeSnapshotReadyCommitPermit {
public:
    RuntimeSnapshotReadyCommitPermit() = delete;
    RuntimeSnapshotReadyCommitPermit(
        const RuntimeSnapshotReadyCommitPermit &) = delete;
    RuntimeSnapshotReadyCommitPermit &operator=(
        const RuntimeSnapshotReadyCommitPermit &) = delete;
    RuntimeSnapshotReadyCommitPermit(
        RuntimeSnapshotReadyCommitPermit &&other) noexcept;
    RuntimeSnapshotReadyCommitPermit &operator=(
        RuntimeSnapshotReadyCommitPermit &&other) noexcept;
    ~RuntimeSnapshotReadyCommitPermit() noexcept;

    [[nodiscard]] ReadyPrepareResult result() const noexcept;

private:
    friend class RuntimeSnapshotCore;

    RuntimeSnapshotReadyCommitPermit(
        RuntimeSnapshotCore *owner,
        ReadyPrepareResult result,
        RuntimeActivationGrant grant,
        std::uint8_t holds_gate) noexcept;

    RuntimeSnapshotCore *owner_;
    ReadyPrepareResult result_;
    RuntimeActivationGrant grant_;
    std::uint8_t holds_gate_;
};

class RuntimeSnapshotCore {
public:
    RuntimeSnapshotCore() noexcept = default;
    RuntimeSnapshotCore(const RuntimeSnapshotCore &) = delete;
    RuntimeSnapshotCore &operator=(const RuntimeSnapshotCore &) = delete;
    RuntimeSnapshotCore(RuntimeSnapshotCore &&) = delete;
    RuntimeSnapshotCore &operator=(RuntimeSnapshotCore &&) = delete;
    ~RuntimeSnapshotCore() noexcept = default;

    [[nodiscard]] BootSnapshotFreezeResult freeze_boot(
        RuntimeOwnerEffect freeze_effect,
        BootRuntimeSnapshotV1 snapshot) noexcept;
    [[nodiscard]] SnapshotCopyResult copy_boot(
        BootRuntimeSnapshotV1 &output) const noexcept;
    [[nodiscard]] SnapshotCopyResult copy_activation_grant(
        RuntimeActivationGrant &output) const noexcept;
    [[nodiscard]] RuntimeSnapshotPublishResult publish_runtime(
        RuntimeStatusSnapshotV1 snapshot) noexcept;
    [[nodiscard]] SnapshotCopyResult copy_runtime(
        RuntimeStatusSnapshotV1 &output) const noexcept;

private:
    friend class RuntimeOwnerTaskCore;
    friend class RuntimeSnapshotReadyCommitPermit;
#if defined(NB_IOT_RUNTIME_SNAPSHOT_TESTING)
    friend class RuntimeSnapshotCoreTestPeer;
#endif

    [[nodiscard]] RuntimeSnapshotReadyCommitPermit prepare_ready(
        RuntimeActivationGrant grant,
        RuntimeOwnerEffect acknowledged_end_boot,
        std::uint32_t acknowledged_dispatch_sequence) noexcept;
    void commit_ready(RuntimeSnapshotReadyCommitPermit &&permit) noexcept;
    void abandon_ready(RuntimeSnapshotReadyCommitPermit &permit) noexcept;

    mutable std::atomic_flag boot_copy_gate_ = ATOMIC_FLAG_INIT;
    mutable std::atomic_flag runtime_copy_gate_ = ATOMIC_FLAG_INIT;
    BootRuntimeSnapshotV1 boot_snapshot_{};
    RuntimeOwnerEffect freeze_effect_{};
    RuntimeActivationGrant activation_grant_{};
    RuntimeStatusSnapshotV1 runtime_snapshot_{};
    std::uint8_t boot_published_{0};
    std::uint8_t ready_published_{0};
    std::uint8_t runtime_published_{0};
};

namespace runtime_snapshot_detail {

template <typename Enum>
constexpr bool has_uint8_underlying_type =
    std::is_same<typename std::underlying_type<Enum>::type,
                 std::uint8_t>::value;

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

static_assert(has_uint8_underlying_type<SnapshotHealth>);
static_assert(has_uint8_underlying_type<BootCompletedStage>);
static_assert(has_uint8_underlying_type<SensorValueSource>);
static_assert(has_uint8_underlying_type<RuntimeNetworkState>);
static_assert(has_uint8_underlying_type<RuntimeAdapterState>);
static_assert(has_uint8_underlying_type<RuntimePowerState>);
static_assert(has_uint8_underlying_type<RuntimeAlarmState>);
static_assert(has_uint8_underlying_type<RuntimeUiState>);
static_assert(has_uint8_underlying_type<RuntimeCommandResult>);
static_assert(has_uint8_underlying_type<BootSnapshotFreezeResult>);
static_assert(has_uint8_underlying_type<SnapshotCopyResult>);
static_assert(has_uint8_underlying_type<ReadyPrepareResult>);
static_assert(has_uint8_underlying_type<RuntimeSnapshotPublishResult>);
static_assert(has_uint8_underlying_type<ShutdownFinalAction>);
static_assert(has_uint8_underlying_type<ShutdownFinalizationPlanResult>);

static_assert(has_fixed_dto_contract<SensorQualitySnapshotV1, 16, 4>);
static_assert(has_fixed_dto_contract<BootRuntimeSnapshotV1, 76, 4>);
static_assert(has_fixed_dto_contract<RuntimeStatusSnapshotV1, 64, 4>);
static_assert(has_fixed_dto_contract<RuntimeActivationGrant, 24, 4>);
static_assert(has_fixed_dto_contract<UsbPowerObservation, 16, 4>);
static_assert(has_fixed_dto_contract<ShutdownFinalizationPlan, 16, 4>);

static_assert(has_only_nonowning_value_fields<
              decltype(SensorQualitySnapshotV1::schema_version),
              decltype(SensorQualitySnapshotV1::health),
              decltype(SensorQualitySnapshotV1::has_value),
              decltype(SensorQualitySnapshotV1::value_source),
              decltype(SensorQualitySnapshotV1::stale),
              decltype(SensorQualitySnapshotV1::clock_valid),
              decltype(SensorQualitySnapshotV1::reserved),
              decltype(SensorQualitySnapshotV1::value_deci_celsius),
              decltype(SensorQualitySnapshotV1::consecutive_failures),
              decltype(SensorQualitySnapshotV1::last_valid_at_unix_seconds)>);

static_assert(has_only_nonowning_value_fields<
              decltype(BootRuntimeSnapshotV1::schema_version),
              decltype(BootRuntimeSnapshotV1::health),
              decltype(BootRuntimeSnapshotV1::last_completed_stage),
              decltype(BootRuntimeSnapshotV1::config_valid),
              decltype(BootRuntimeSnapshotV1::transport_ready),
              decltype(BootRuntimeSnapshotV1::subscription_alive),
              decltype(BootRuntimeSnapshotV1::post_config_liveness),
              decltype(BootRuntimeSnapshotV1::reboot_guard),
              decltype(BootRuntimeSnapshotV1::reserved),
              decltype(BootRuntimeSnapshotV1::hardware_revision),
              decltype(BootRuntimeSnapshotV1::firmware_build_id),
              decltype(BootRuntimeSnapshotV1::config_version),
              decltype(BootRuntimeSnapshotV1::sensors),
              decltype(BootRuntimeSnapshotV1::pdp_session_id),
              decltype(BootRuntimeSnapshotV1::mqtt_session_id),
              decltype(BootRuntimeSnapshotV1::mqtt_generation),
              decltype(BootRuntimeSnapshotV1::config_apply_epoch),
              decltype(BootRuntimeSnapshotV1::recovery_summary)>);

static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeStatusSnapshotV1::schema_version),
              decltype(RuntimeStatusSnapshotV1::network_state),
              decltype(RuntimeStatusSnapshotV1::adapter_state),
              decltype(RuntimeStatusSnapshotV1::power_state),
              decltype(RuntimeStatusSnapshotV1::alarm_state),
              decltype(RuntimeStatusSnapshotV1::ui_state),
              decltype(RuntimeStatusSnapshotV1::last_command_result),
              decltype(RuntimeStatusSnapshotV1::reserved),
              decltype(RuntimeStatusSnapshotV1::revision),
              decltype(RuntimeStatusSnapshotV1::sensors),
              decltype(RuntimeStatusSnapshotV1::config_version),
              decltype(RuntimeStatusSnapshotV1::last_command_id),
              decltype(RuntimeStatusSnapshotV1::queue_summary),
              decltype(RuntimeStatusSnapshotV1::drop_summary),
              decltype(RuntimeStatusSnapshotV1::recovery_summary)>);

static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeActivationGrant::liveness),
              decltype(RuntimeActivationGrant::snapshot_correlation_id),
              decltype(RuntimeActivationGrant::boot_end_correlation_id),
              decltype(RuntimeActivationGrant::boot_end_dispatch_sequence)>);

static_assert(has_only_nonowning_value_fields<
              decltype(UsbPowerObservation::present),
              decltype(UsbPowerObservation::cleanup_skipped_mask),
              decltype(UsbPowerObservation::cleanup_timed_out_mask),
              decltype(UsbPowerObservation::hard_deadline_reached),
              decltype(UsbPowerObservation::sample_sequence),
              decltype(UsbPowerObservation::sampled_at_monotonic_ms),
              decltype(UsbPowerObservation::reserved)>);

static_assert(has_only_nonowning_value_fields<
              decltype(ShutdownFinalizationPlan::schema_version),
              decltype(ShutdownFinalizationPlan::intended_action),
              decltype(ShutdownFinalizationPlan::cleanup_skipped_mask),
              decltype(ShutdownFinalizationPlan::cleanup_timed_out_mask),
              decltype(ShutdownFinalizationPlan::hard_deadline_reached),
              decltype(ShutdownFinalizationPlan::live_actuation),
              decltype(ShutdownFinalizationPlan::reserved),
              decltype(ShutdownFinalizationPlan::sample_sequence),
              decltype(ShutdownFinalizationPlan::sampled_at_monotonic_ms)>);

} // namespace runtime_snapshot_detail

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_SNAPSHOT_CORE_HPP
