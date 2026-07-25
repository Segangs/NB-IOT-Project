#include "runtime_snapshot_core.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace boot_v2 {

#if !defined(NB_IOT_RUNTIME_SNAPSHOT_TESTING)
#error "runtime_snapshot_core_test requires NB_IOT_RUNTIME_SNAPSHOT_TESTING"
#endif

class RuntimeSnapshotCoreTestPeer {
public:
    [[nodiscard]] static RuntimeSnapshotReadyCommitPermit prepare_ready(
        RuntimeSnapshotCore &core,
        const RuntimeActivationGrant grant,
        const RuntimeOwnerEffect acknowledged_end_boot,
        const std::uint32_t acknowledged_dispatch_sequence) noexcept
    {
        return core.prepare_ready(
            grant, acknowledged_end_boot, acknowledged_dispatch_sequence);
    }

    static void commit_ready(
        RuntimeSnapshotCore &core,
        RuntimeSnapshotReadyCommitPermit &&permit) noexcept
    {
        core.commit_ready(std::move(permit));
    }

    [[nodiscard]] static bool hold_boot_gate(
        RuntimeSnapshotCore &core) noexcept
    {
        return !core.boot_copy_gate_.test_and_set(std::memory_order_acquire);
    }

    static void release_boot_gate(RuntimeSnapshotCore &core) noexcept
    {
        core.boot_copy_gate_.clear(std::memory_order_release);
    }

    [[nodiscard]] static bool hold_runtime_gate(
        RuntimeSnapshotCore &core) noexcept
    {
        return !core.runtime_copy_gate_.test_and_set(
            std::memory_order_acquire);
    }

    static void release_runtime_gate(RuntimeSnapshotCore &core) noexcept
    {
        core.runtime_copy_gate_.clear(std::memory_order_release);
    }
};

} // namespace boot_v2

namespace {

std::size_t g_check_count = 0;
std::size_t g_failure_count = 0;

void check_impl(
    const bool condition,
    const char *const expression,
    const char *const file,
    const int line) noexcept
{
    ++g_check_count;
    if (!condition) {
        ++g_failure_count;
        std::fprintf(
            stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
    }
}

#define CHECK(...) check_impl((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

using namespace boot_v2;

template <typename... Fields>
constexpr bool has_only_nonowning_value_fields =
    ((!std::is_pointer<Fields>::value &&
      !std::is_reference<Fields>::value &&
      std::is_trivially_copyable<Fields>::value) && ...);

template <typename Enum>
constexpr bool has_uint8_underlying =
    std::is_same<typename std::underlying_type<Enum>::type,
                 std::uint8_t>::value;

template <typename Type, typename = void>
struct HasPublicPrepareReady : std::false_type {
};

template <typename Type>
struct HasPublicPrepareReady<Type, std::void_t<decltype(
    std::declval<Type &>().prepare_ready(
        RuntimeActivationGrant{}, RuntimeOwnerEffect{}, std::uint32_t{}))>>
    : std::true_type {
};

template <typename Type, typename = void>
struct HasPublicCommitReady : std::false_type {
};

template <typename Type>
struct HasPublicCommitReady<Type, std::void_t<decltype(
    std::declval<Type &>().commit_ready(
        std::declval<RuntimeSnapshotReadyCommitPermit &&>()))>>
    : std::true_type {
};

constexpr SensorQualitySnapshotV1 make_sensor(
    const std::int16_t value,
    const std::uint32_t sampled_at) noexcept
{
    SensorQualitySnapshotV1 sensor{};
    sensor.health = SnapshotHealth::Pass;
    sensor.has_value = 1;
    sensor.value_source = SensorValueSource::Fresh;
    sensor.clock_valid = 1;
    sensor.value_deci_celsius = value;
    sensor.last_valid_at_unix_seconds = sampled_at;
    return sensor;
}

constexpr BootRuntimeSnapshotV1 make_boot_snapshot(
    const LivenessAttemptToken token = {41, 7, 3}) noexcept
{
    BootRuntimeSnapshotV1 snapshot{};
    snapshot.health = SnapshotHealth::Pass;
    snapshot.last_completed_stage =
        BootCompletedStage::PostConfigLivenessPassed;
    snapshot.config_valid = 1;
    snapshot.transport_ready = 1;
    snapshot.subscription_alive = 1;
    snapshot.post_config_liveness = 1;
    snapshot.hardware_revision = 12;
    snapshot.firmware_build_id = 20260721;
    snapshot.config_version = 19;
    snapshot.sensors = {make_sensor(215, 1001), make_sensor(223, 1002)};
    snapshot.pdp_session_id = 31;
    snapshot.mqtt_session_id = token.mqtt_session_id;
    snapshot.mqtt_generation = token.mqtt_generation;
    snapshot.config_apply_epoch = token.config_apply_epoch;
    snapshot.recovery_summary = 2;
    return snapshot;
}

constexpr RuntimeOwnerEffect make_freeze_effect(
    const LivenessAttemptToken token = {41, 7, 3},
    const std::uint32_t correlation_id = 90) noexcept
{
    return {
        RuntimeOwnerEffectKind::FreezeBootSnapshot,
        correlation_id,
        token,
        RuntimeOwnerFaultCode::None,
    };
}

constexpr RuntimeOwnerEffect make_end_boot_effect(
    const LivenessAttemptToken token = {41, 7, 3},
    const std::uint32_t correlation_id = 91) noexcept
{
    return {
        RuntimeOwnerEffectKind::EndBootOrchestration,
        correlation_id,
        token,
        RuntimeOwnerFaultCode::None,
    };
}

constexpr RuntimeActivationGrant make_grant(
    const LivenessAttemptToken token = {41, 7, 3},
    const std::uint32_t snapshot_correlation_id = 90,
    const std::uint32_t boot_end_correlation_id = 91,
    const std::uint32_t dispatch_sequence = 17) noexcept
{
    return {
        token,
        snapshot_correlation_id,
        boot_end_correlation_id,
        dispatch_sequence,
    };
}

RuntimeStatusSnapshotV1 make_runtime_snapshot(
    const std::uint32_t revision) noexcept
{
    RuntimeStatusSnapshotV1 snapshot{};
    snapshot.network_state = RuntimeNetworkState::Online;
    snapshot.adapter_state = RuntimeAdapterState::Present;
    snapshot.power_state = RuntimePowerState::ExternalPower;
    snapshot.alarm_state = RuntimeAlarmState::Clear;
    snapshot.ui_state = RuntimeUiState::PeriodicReady;
    snapshot.last_command_result = RuntimeCommandResult::Succeeded;
    snapshot.revision = revision;
    snapshot.sensors = {
        make_sensor(
            static_cast<std::int16_t>(revision % 3000u), revision),
        make_sensor(
            static_cast<std::int16_t>((revision + 1u) % 3000u),
            revision + 1u),
    };
    snapshot.config_version = revision;
    snapshot.last_command_id = revision;
    snapshot.queue_summary = revision ^ UINT32_C(0x55aa55aa);
    snapshot.drop_summary = revision + 17u;
    snapshot.recovery_summary = revision + 31u;
    return snapshot;
}

bool runtime_pattern_is_consistent(
    const RuntimeStatusSnapshotV1 &snapshot) noexcept
{
    const std::uint32_t revision = snapshot.revision;
    return runtime_status_snapshot_is_canonical(snapshot) &&
           snapshot.config_version == revision &&
           snapshot.last_command_id == revision &&
           snapshot.queue_summary == (revision ^ UINT32_C(0x55aa55aa)) &&
           snapshot.drop_summary == revision + 17u &&
           snapshot.recovery_summary == revision + 31u &&
           snapshot.sensors[0].value_deci_celsius ==
               static_cast<std::int16_t>(revision % 3000u) &&
           snapshot.sensors[0].last_valid_at_unix_seconds == revision &&
           snapshot.sensors[1].value_deci_celsius ==
               static_cast<std::int16_t>((revision + 1u) % 3000u) &&
           snapshot.sensors[1].last_valid_at_unix_seconds == revision + 1u;
}

void freeze_and_ready(RuntimeSnapshotCore &core)
{
    CHECK(core.freeze_boot(make_freeze_effect(), make_boot_snapshot()) ==
          BootSnapshotFreezeResult::Accepted);
    auto permit = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, make_grant(), make_end_boot_effect(), 17);
    CHECK(permit.result() == ReadyPrepareResult::Prepared);
    RuntimeSnapshotCoreTestPeer::commit_ready(core, std::move(permit));
}

void test_numeric_defaults_and_layout()
{
    static_assert(has_uint8_underlying<SnapshotHealth>);
    static_assert(has_uint8_underlying<BootCompletedStage>);
    static_assert(has_uint8_underlying<SensorValueSource>);
    static_assert(has_uint8_underlying<RuntimeNetworkState>);
    static_assert(has_uint8_underlying<RuntimeAdapterState>);
    static_assert(has_uint8_underlying<RuntimePowerState>);
    static_assert(has_uint8_underlying<RuntimeAlarmState>);
    static_assert(has_uint8_underlying<RuntimeUiState>);
    static_assert(has_uint8_underlying<RuntimeCommandResult>);
    static_assert(has_uint8_underlying<BootSnapshotFreezeResult>);
    static_assert(has_uint8_underlying<SnapshotCopyResult>);
    static_assert(has_uint8_underlying<ReadyPrepareResult>);
    static_assert(has_uint8_underlying<RuntimeSnapshotPublishResult>);
    static_assert(has_uint8_underlying<ShutdownFinalAction>);
    static_assert(has_uint8_underlying<ShutdownFinalizationPlanResult>);

    CHECK(static_cast<std::uint8_t>(SnapshotHealth::Unknown) == 0);
    CHECK(static_cast<std::uint8_t>(SnapshotHealth::Pass) == 1);
    CHECK(static_cast<std::uint8_t>(SnapshotHealth::Degraded) == 2);
    CHECK(static_cast<std::uint8_t>(SnapshotHealth::Failed) == 3);
    CHECK(static_cast<std::uint8_t>(BootCompletedStage::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(BootCompletedStage::TransportEstablished) == 1);
    CHECK(static_cast<std::uint8_t>(BootCompletedStage::ConfigActivated) == 2);
    CHECK(static_cast<std::uint8_t>(BootCompletedStage::PostConfigLivenessPassed) == 3);
    CHECK(static_cast<std::uint8_t>(SensorValueSource::None) == 0);
    CHECK(static_cast<std::uint8_t>(SensorValueSource::Fresh) == 1);
    CHECK(static_cast<std::uint8_t>(SensorValueSource::CrcFallback) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeNetworkState::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeNetworkState::Offline) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeNetworkState::Online) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeNetworkState::Recovering) == 3);
    CHECK(static_cast<std::uint8_t>(RuntimeAdapterState::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeAdapterState::Present) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeAdapterState::Absent) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimePowerState::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimePowerState::ExternalPower) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimePowerState::Grace) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimePowerState::Committed) == 3);
    CHECK(static_cast<std::uint8_t>(RuntimePowerState::Cleanup) == 4);
    CHECK(static_cast<std::uint8_t>(RuntimePowerState::Off) == 5);
    CHECK(static_cast<std::uint8_t>(RuntimeAlarmState::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeAlarmState::Clear) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeAlarmState::Active) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeUiState::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeUiState::Boot) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeUiState::PeriodicReady) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeUiState::Shutdown) == 3);
    CHECK(static_cast<std::uint8_t>(RuntimeCommandResult::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeCommandResult::NoCommand) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeCommandResult::Succeeded) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeCommandResult::Failed) == 3);
    CHECK(static_cast<std::uint8_t>(RuntimeCommandResult::Rejected) == 4);
    CHECK(static_cast<std::uint8_t>(BootSnapshotFreezeResult::RejectedInvalid) == 0);
    CHECK(static_cast<std::uint8_t>(BootSnapshotFreezeResult::RejectedBusy) == 1);
    CHECK(static_cast<std::uint8_t>(BootSnapshotFreezeResult::RejectedAlreadyFrozen) == 2);
    CHECK(static_cast<std::uint8_t>(BootSnapshotFreezeResult::Accepted) == 3);
    CHECK(static_cast<std::uint8_t>(BootSnapshotFreezeResult::AcceptedDuplicate) == 4);
    CHECK(static_cast<std::uint8_t>(SnapshotCopyResult::RejectedBusy) == 0);
    CHECK(static_cast<std::uint8_t>(SnapshotCopyResult::RejectedNotReady) == 1);
    CHECK(static_cast<std::uint8_t>(SnapshotCopyResult::Copied) == 2);
    CHECK(static_cast<std::uint8_t>(ReadyPrepareResult::RejectedInvalid) == 0);
    CHECK(static_cast<std::uint8_t>(ReadyPrepareResult::RejectedBusy) == 1);
    CHECK(static_cast<std::uint8_t>(ReadyPrepareResult::RejectedNotFrozen) == 2);
    CHECK(static_cast<std::uint8_t>(ReadyPrepareResult::Prepared) == 3);
    CHECK(static_cast<std::uint8_t>(ReadyPrepareResult::AcceptedDuplicate) == 4);
    CHECK(static_cast<std::uint8_t>(RuntimeSnapshotPublishResult::RejectedInvalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeSnapshotPublishResult::RejectedBusy) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeSnapshotPublishResult::RejectedNotReady) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeSnapshotPublishResult::RejectedRevisionZero) == 3);
    CHECK(static_cast<std::uint8_t>(
              RuntimeSnapshotPublishResult::RejectedRevisionNotIncreasing) ==
          4);
    CHECK(static_cast<std::uint8_t>(RuntimeSnapshotPublishResult::RejectedRevisionSaturated) == 5);
    CHECK(static_cast<std::uint8_t>(RuntimeSnapshotPublishResult::Accepted) == 6);
    CHECK(static_cast<std::uint8_t>(ShutdownFinalAction::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(ShutdownFinalAction::Gp15Kill) == 1);
    CHECK(static_cast<std::uint8_t>(ShutdownFinalAction::WatchdogReboot) == 2);
    CHECK(static_cast<std::uint8_t>(
              ShutdownFinalizationPlanResult::RejectedInvalid) == 0);
    CHECK(static_cast<std::uint8_t>(
              ShutdownFinalizationPlanResult::Planned) == 1);

    const SensorQualitySnapshotV1 sensor{};
    CHECK(sensor.schema_version == 1);
    CHECK(sensor.health == SnapshotHealth::Unknown);
    CHECK(sensor.has_value == 0);
    CHECK(sensor.value_source == SensorValueSource::None);
    CHECK(sensor.stale == 0);
    CHECK(sensor.clock_valid == 0);
    CHECK(sensor.reserved[0] == 0 && sensor.reserved[1] == 0);
    CHECK(sensor.value_deci_celsius == 0);
    CHECK(sensor.consecutive_failures == 0);
    CHECK(sensor.last_valid_at_unix_seconds == 0);

    const BootRuntimeSnapshotV1 boot{};
    CHECK(boot.schema_version == 1);
    CHECK(boot.health == SnapshotHealth::Unknown);
    CHECK(boot.last_completed_stage == BootCompletedStage::Invalid);
    CHECK(boot.config_valid == 0 && boot.transport_ready == 0);
    CHECK(boot.subscription_alive == 0 && boot.post_config_liveness == 0);
    CHECK(boot.reboot_guard == 0);
    CHECK(boot.reserved[0] == 0 && boot.reserved[1] == 0 &&
          boot.reserved[2] == 0 && boot.reserved[3] == 0);
    CHECK(boot.hardware_revision == 0 && boot.firmware_build_id == 0);
    CHECK(boot.config_version == 0 && boot.pdp_session_id == 0);
    CHECK(boot.mqtt_session_id == 0 && boot.mqtt_generation == 0);
    CHECK(boot.config_apply_epoch == 0 && boot.recovery_summary == 0);

    const RuntimeStatusSnapshotV1 runtime{};
    CHECK(runtime.schema_version == 1);
    CHECK(runtime.network_state == RuntimeNetworkState::Invalid);
    CHECK(runtime.adapter_state == RuntimeAdapterState::Invalid);
    CHECK(runtime.power_state == RuntimePowerState::Invalid);
    CHECK(runtime.alarm_state == RuntimeAlarmState::Invalid);
    CHECK(runtime.ui_state == RuntimeUiState::Invalid);
    CHECK(runtime.last_command_result == RuntimeCommandResult::Invalid);
    CHECK(runtime.reserved == 0 && runtime.revision == 0);
    CHECK(runtime.config_version == 0 && runtime.last_command_id == 0);
    CHECK(runtime.queue_summary == 0 && runtime.drop_summary == 0 &&
          runtime.recovery_summary == 0);

    const RuntimeActivationGrant grant{};
    CHECK(grant.liveness == LivenessAttemptToken{});
    CHECK(grant.snapshot_correlation_id == 0);
    CHECK(grant.boot_end_correlation_id == 0);
    CHECK(grant.boot_end_dispatch_sequence == 0);

    const UsbPowerObservation observation{};
    CHECK(observation.present == 0);
    CHECK(observation.cleanup_skipped_mask == 0);
    CHECK(observation.cleanup_timed_out_mask == 0);
    CHECK(observation.hard_deadline_reached == 0);
    CHECK(observation.sample_sequence == 0);
    CHECK(observation.sampled_at_monotonic_ms == 0);
    CHECK(observation.reserved[0] == 0 && observation.reserved[1] == 0 &&
          observation.reserved[2] == 0 && observation.reserved[3] == 0);

    const ShutdownFinalizationPlan plan{};
    CHECK(plan.schema_version == 1);
    CHECK(plan.intended_action == ShutdownFinalAction::Invalid);
    CHECK(plan.cleanup_skipped_mask == 0);
    CHECK(plan.cleanup_timed_out_mask == 0);
    CHECK(plan.hard_deadline_reached == 0);
    CHECK(plan.live_actuation == 0);
    CHECK(plan.reserved[0] == 0 && plan.reserved[1] == 0);
    CHECK(plan.sample_sequence == 0 && plan.sampled_at_monotonic_ms == 0);

    static_assert(sizeof(SensorQualitySnapshotV1) == 16);
    static_assert(alignof(SensorQualitySnapshotV1) == 4);
    static_assert(sizeof(BootRuntimeSnapshotV1) == 76);
    static_assert(alignof(BootRuntimeSnapshotV1) == 4);
    static_assert(sizeof(RuntimeStatusSnapshotV1) == 64);
    static_assert(alignof(RuntimeStatusSnapshotV1) == 4);
    static_assert(sizeof(RuntimeActivationGrant) == 24);
    static_assert(alignof(RuntimeActivationGrant) == 4);
    static_assert(sizeof(UsbPowerObservation) == 16);
    static_assert(alignof(UsbPowerObservation) == 4);
    static_assert(sizeof(ShutdownFinalizationPlan) == 16);
    static_assert(alignof(ShutdownFinalizationPlan) == 4);

    static_assert(std::is_standard_layout<SensorQualitySnapshotV1>::value);
    static_assert(std::is_trivially_copyable<SensorQualitySnapshotV1>::value);
    static_assert(std::is_standard_layout<BootRuntimeSnapshotV1>::value);
    static_assert(std::is_trivially_copyable<BootRuntimeSnapshotV1>::value);
    static_assert(std::is_standard_layout<RuntimeStatusSnapshotV1>::value);
    static_assert(std::is_trivially_copyable<RuntimeStatusSnapshotV1>::value);
    static_assert(std::is_standard_layout<RuntimeActivationGrant>::value);
    static_assert(std::is_trivially_copyable<RuntimeActivationGrant>::value);
    static_assert(std::is_standard_layout<UsbPowerObservation>::value);
    static_assert(std::is_trivially_copyable<UsbPowerObservation>::value);
    static_assert(std::is_standard_layout<ShutdownFinalizationPlan>::value);
    static_assert(std::is_trivially_copyable<ShutdownFinalizationPlan>::value);

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

    static_assert(!std::is_default_constructible<
                  RuntimeSnapshotReadyCommitPermit>::value);
    static_assert(!std::is_copy_constructible<
                  RuntimeSnapshotReadyCommitPermit>::value);
    static_assert(std::is_nothrow_move_constructible<
                  RuntimeSnapshotReadyCommitPermit>::value);
    static_assert(!std::is_copy_constructible<RuntimeSnapshotCore>::value);
    static_assert(!std::is_move_constructible<RuntimeSnapshotCore>::value);
    static_assert(!std::is_constructible<
                  RuntimeSnapshotReadyCommitPermit,
                  RuntimeSnapshotCore *,
                  ReadyPrepareResult,
                  RuntimeActivationGrant,
                  std::uint8_t>::value);
    static_assert(!HasPublicPrepareReady<RuntimeSnapshotCore>::value);
    static_assert(!HasPublicCommitReady<RuntimeSnapshotCore>::value);
}

void test_canonical_validation_and_fieldwise_equality()
{
    const SensorQualitySnapshotV1 sensor = make_sensor(215, 1001);
    CHECK(sensor_quality_snapshot_is_canonical(sensor));
    CHECK(sensor_quality_snapshots_equal(sensor, sensor));

    SensorQualitySnapshotV1 changed_sensor = sensor;
    changed_sensor.reserved[1] = 1;
    CHECK(!sensor_quality_snapshot_is_canonical(changed_sensor));
    CHECK(!sensor_quality_snapshots_equal(sensor, changed_sensor));
    changed_sensor = sensor;
    changed_sensor.has_value = 2;
    CHECK(!sensor_quality_snapshot_is_canonical(changed_sensor));
    changed_sensor = sensor;
    changed_sensor.value_source = SensorValueSource::None;
    CHECK(!sensor_quality_snapshot_is_canonical(changed_sensor));
    changed_sensor = sensor;
    changed_sensor.clock_valid = 0;
    CHECK(!sensor_quality_snapshot_is_canonical(changed_sensor));
    changed_sensor = sensor;
    changed_sensor.health = static_cast<SnapshotHealth>(0xff);
    CHECK(!sensor_quality_snapshot_is_canonical(changed_sensor));

    const BootRuntimeSnapshotV1 boot = make_boot_snapshot();
    CHECK(boot_runtime_snapshot_is_canonical(boot));
    CHECK(boot_runtime_snapshots_equal(boot, boot));
    BootRuntimeSnapshotV1 changed_boot = boot;
    changed_boot.reserved[2] = 1;
    CHECK(!boot_runtime_snapshot_is_canonical(changed_boot));
    CHECK(!boot_runtime_snapshots_equal(boot, changed_boot));
    changed_boot = boot;
    changed_boot.post_config_liveness = 0;
    CHECK(!boot_runtime_snapshot_is_canonical(changed_boot));
    changed_boot = boot;
    changed_boot.last_completed_stage = BootCompletedStage::ConfigActivated;
    CHECK(!boot_runtime_snapshot_is_canonical(changed_boot));
    changed_boot = boot;
    changed_boot.sensors[1].schema_version = 2;
    CHECK(!boot_runtime_snapshot_is_canonical(changed_boot));

    const RuntimeStatusSnapshotV1 runtime = make_runtime_snapshot(1);
    CHECK(runtime_status_snapshot_is_canonical(runtime));
    CHECK(runtime_status_snapshots_equal(runtime, runtime));
    RuntimeStatusSnapshotV1 changed_runtime = runtime;
    changed_runtime.reserved = 1;
    CHECK(!runtime_status_snapshot_is_canonical(changed_runtime));
    CHECK(!runtime_status_snapshots_equal(runtime, changed_runtime));
    changed_runtime = runtime;
    changed_runtime.network_state = RuntimeNetworkState::Invalid;
    CHECK(!runtime_status_snapshot_is_canonical(changed_runtime));
    changed_runtime = runtime;
    changed_runtime.last_command_id = 0;
    CHECK(!runtime_status_snapshot_is_canonical(changed_runtime));
    changed_runtime = runtime;
    changed_runtime.sensors[0].value_source = SensorValueSource::None;
    CHECK(!runtime_status_snapshot_is_canonical(changed_runtime));

    const RuntimeActivationGrant grant = make_grant();
    CHECK(runtime_activation_grant_is_canonical(grant));
    CHECK(runtime_activation_grants_equal(grant, grant));
    RuntimeActivationGrant changed_grant = grant;
    changed_grant.boot_end_dispatch_sequence = 0;
    CHECK(!runtime_activation_grant_is_canonical(changed_grant));
    CHECK(!runtime_activation_grants_equal(grant, changed_grant));
}

void test_boot_freeze_is_exact_one_shot()
{
    RuntimeSnapshotCore core{};
    BootRuntimeSnapshotV1 copied{};
    CHECK(core.copy_boot(copied) == SnapshotCopyResult::RejectedNotReady);

    RuntimeOwnerEffect invalid = make_freeze_effect();
    invalid.attempt = {};
    CHECK(core.freeze_boot(invalid, make_boot_snapshot()) ==
          BootSnapshotFreezeResult::RejectedInvalid);
    invalid = make_freeze_effect();
    invalid.kind = RuntimeOwnerEffectKind::StartAtProbe;
    CHECK(core.freeze_boot(invalid, make_boot_snapshot()) ==
          BootSnapshotFreezeResult::RejectedInvalid);
    invalid = make_freeze_effect();
    invalid.correlation_id = 0;
    CHECK(core.freeze_boot(invalid, make_boot_snapshot()) ==
          BootSnapshotFreezeResult::RejectedInvalid);
    invalid = make_freeze_effect();
    invalid.fault_code = RuntimeOwnerFaultCode::SnapshotFailure;
    CHECK(core.freeze_boot(invalid, make_boot_snapshot()) ==
          BootSnapshotFreezeResult::RejectedInvalid);

    BootRuntimeSnapshotV1 mismatched = make_boot_snapshot();
    ++mismatched.mqtt_generation;
    CHECK(core.freeze_boot(make_freeze_effect(), mismatched) ==
          BootSnapshotFreezeResult::RejectedInvalid);

    const BootRuntimeSnapshotV1 original = make_boot_snapshot();
    const RuntimeOwnerEffect freeze = make_freeze_effect();
    CHECK(core.freeze_boot(freeze, original) ==
          BootSnapshotFreezeResult::Accepted);
    CHECK(core.copy_boot(copied) == SnapshotCopyResult::Copied);
    CHECK(boot_runtime_snapshots_equal(copied, original));
    CHECK(core.freeze_boot(freeze, original) ==
          BootSnapshotFreezeResult::AcceptedDuplicate);

    BootRuntimeSnapshotV1 different = original;
    ++different.recovery_summary;
    CHECK(core.freeze_boot(freeze, different) ==
          BootSnapshotFreezeResult::RejectedAlreadyFrozen);
    RuntimeOwnerEffect different_effect = freeze;
    ++different_effect.correlation_id;
    CHECK(core.freeze_boot(different_effect, original) ==
          BootSnapshotFreezeResult::RejectedAlreadyFrozen);
    CHECK(core.copy_boot(copied) == SnapshotCopyResult::Copied);
    CHECK(boot_runtime_snapshots_equal(copied, original));
}

void test_ready_prepare_exactness_and_failure_free_commit()
{
    RuntimeSnapshotCore core{};
    RuntimeActivationGrant copied_grant{};
    auto before_freeze = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, make_grant(), make_end_boot_effect(), 17);
    CHECK(before_freeze.result() == ReadyPrepareResult::RejectedNotFrozen);
    CHECK(core.copy_activation_grant(copied_grant) ==
          SnapshotCopyResult::RejectedNotReady);

    CHECK(core.freeze_boot(make_freeze_effect(), make_boot_snapshot()) ==
          BootSnapshotFreezeResult::Accepted);

    const RuntimeActivationGrant grant = make_grant();
    const RuntimeOwnerEffect end_boot = make_end_boot_effect();

    RuntimeActivationGrant wrong_grant = grant;
    ++wrong_grant.liveness.mqtt_session_id;
    auto wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, wrong_grant, end_boot, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);
    wrong_grant = grant;
    ++wrong_grant.liveness.mqtt_generation;
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, wrong_grant, end_boot, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);
    wrong_grant = grant;
    ++wrong_grant.liveness.config_apply_epoch;
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, wrong_grant, end_boot, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);
    wrong_grant = grant;
    ++wrong_grant.snapshot_correlation_id;
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, wrong_grant, end_boot, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);
    wrong_grant = grant;
    ++wrong_grant.boot_end_correlation_id;
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, wrong_grant, end_boot, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);
    wrong_grant = grant;
    ++wrong_grant.boot_end_dispatch_sequence;
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, wrong_grant, end_boot, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);

    RuntimeOwnerEffect wrong_effect = end_boot;
    wrong_effect.kind = RuntimeOwnerEffectKind::FreezeBootSnapshot;
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, grant, wrong_effect, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);
    wrong_effect = end_boot;
    ++wrong_effect.correlation_id;
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, grant, wrong_effect, 17);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);
    wrong = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, grant, end_boot, 18);
    CHECK(wrong.result() == ReadyPrepareResult::RejectedInvalid);

    {
        auto abandoned = RuntimeSnapshotCoreTestPeer::prepare_ready(
            core, grant, end_boot, 17);
        CHECK(abandoned.result() == ReadyPrepareResult::Prepared);
        BootRuntimeSnapshotV1 blocked{};
        CHECK(core.copy_boot(blocked) == SnapshotCopyResult::RejectedBusy);
        CHECK(core.copy_activation_grant(copied_grant) ==
              SnapshotCopyResult::RejectedBusy);
    }
    CHECK(core.copy_activation_grant(copied_grant) ==
          SnapshotCopyResult::RejectedNotReady);

    BootRuntimeSnapshotV1 before_commit{};
    CHECK(core.copy_boot(before_commit) == SnapshotCopyResult::Copied);
    auto permit = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, grant, end_boot, 17);
    CHECK(permit.result() == ReadyPrepareResult::Prepared);
    RuntimeSnapshotCoreTestPeer::commit_ready(core, std::move(permit));
    CHECK(core.copy_activation_grant(copied_grant) == SnapshotCopyResult::Copied);
    CHECK(runtime_activation_grants_equal(copied_grant, grant));

    BootRuntimeSnapshotV1 after_commit{};
    CHECK(core.copy_boot(after_commit) == SnapshotCopyResult::Copied);
    CHECK(boot_runtime_snapshots_equal(before_commit, after_commit));

    auto duplicate = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, grant, end_boot, 17);
    CHECK(duplicate.result() == ReadyPrepareResult::AcceptedDuplicate);
    wrong_grant = grant;
    ++wrong_grant.boot_end_correlation_id;
    auto different = RuntimeSnapshotCoreTestPeer::prepare_ready(
        core, wrong_grant, end_boot, 17);
    CHECK(different.result() == ReadyPrepareResult::RejectedInvalid);
}

void test_nonblocking_busy_precedence_and_latest_revision()
{
    RuntimeSnapshotCore not_ready{};
    RuntimeStatusSnapshotV1 copied{};
    CHECK(not_ready.copy_runtime(copied) ==
          SnapshotCopyResult::RejectedNotReady);
    CHECK(RuntimeSnapshotCoreTestPeer::hold_runtime_gate(not_ready));
    CHECK(not_ready.publish_runtime(make_runtime_snapshot(1)) ==
          RuntimeSnapshotPublishResult::RejectedBusy);
    CHECK(not_ready.copy_runtime(copied) == SnapshotCopyResult::RejectedBusy);
    RuntimeSnapshotCoreTestPeer::release_runtime_gate(not_ready);
    CHECK(not_ready.publish_runtime(make_runtime_snapshot(1)) ==
          RuntimeSnapshotPublishResult::RejectedNotReady);

    CHECK(RuntimeSnapshotCoreTestPeer::hold_boot_gate(not_ready));
    BootRuntimeSnapshotV1 boot_copy{};
    CHECK(not_ready.copy_boot(boot_copy) == SnapshotCopyResult::RejectedBusy);
    CHECK(not_ready.publish_runtime(make_runtime_snapshot(1)) ==
          RuntimeSnapshotPublishResult::RejectedBusy);
    RuntimeSnapshotCoreTestPeer::release_boot_gate(not_ready);

    RuntimeSnapshotCore core{};
    freeze_and_ready(core);
    CHECK(core.publish_runtime(make_runtime_snapshot(1)) ==
          RuntimeSnapshotPublishResult::Accepted);
    CHECK(core.copy_runtime(copied) == SnapshotCopyResult::Copied);
    CHECK(runtime_status_snapshots_equal(copied, make_runtime_snapshot(1)));

    RuntimeStatusSnapshotV1 zero = make_runtime_snapshot(1);
    zero.revision = 0;
    CHECK(core.publish_runtime(zero) ==
          RuntimeSnapshotPublishResult::RejectedRevisionZero);
    CHECK(core.publish_runtime(make_runtime_snapshot(1)) ==
          RuntimeSnapshotPublishResult::RejectedRevisionNotIncreasing);
    CHECK(core.publish_runtime(make_runtime_snapshot(2)) ==
          RuntimeSnapshotPublishResult::Accepted);

    CHECK(RuntimeSnapshotCoreTestPeer::hold_runtime_gate(core));
    CHECK(core.publish_runtime(make_runtime_snapshot(3)) ==
          RuntimeSnapshotPublishResult::RejectedBusy);
    RuntimeSnapshotCoreTestPeer::release_runtime_gate(core);
    CHECK(core.copy_runtime(copied) == SnapshotCopyResult::Copied);
    CHECK(runtime_status_snapshots_equal(copied, make_runtime_snapshot(2)));

    CHECK(core.publish_runtime(make_runtime_snapshot(
              std::numeric_limits<std::uint32_t>::max())) ==
          RuntimeSnapshotPublishResult::Accepted);
    CHECK(core.publish_runtime(make_runtime_snapshot(
              std::numeric_limits<std::uint32_t>::max())) ==
          RuntimeSnapshotPublishResult::RejectedRevisionSaturated);
    CHECK(core.publish_runtime(make_runtime_snapshot(1)) ==
          RuntimeSnapshotPublishResult::RejectedRevisionSaturated);
    CHECK(core.copy_runtime(copied) == SnapshotCopyResult::Copied);
    CHECK(copied.revision == std::numeric_limits<std::uint32_t>::max());
}

void test_concurrent_latest_copy_has_no_torn_combination()
{
    RuntimeSnapshotCore core{};
    freeze_and_ready(core);
    CHECK(core.publish_runtime(make_runtime_snapshot(1)) ==
          RuntimeSnapshotPublishResult::Accepted);

    constexpr std::uint32_t kLastRevision = 20000;
    std::atomic<bool> writer_done{false};
    std::atomic<bool> failed{false};

    std::thread writer([&]() {
        for (std::uint32_t revision = 2; revision <= kLastRevision;) {
            const RuntimeSnapshotPublishResult result =
                core.publish_runtime(make_runtime_snapshot(revision));
            if (result == RuntimeSnapshotPublishResult::Accepted) {
                ++revision;
            } else if (result == RuntimeSnapshotPublishResult::RejectedBusy) {
                std::this_thread::yield();
            } else {
                failed.store(true, std::memory_order_relaxed);
                break;
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    std::thread reader([&]() {
        std::uint32_t last_seen = 0;
        do {
            RuntimeStatusSnapshotV1 snapshot{};
            const SnapshotCopyResult result = core.copy_runtime(snapshot);
            if (result == SnapshotCopyResult::Copied) {
                if (!runtime_pattern_is_consistent(snapshot) ||
                    snapshot.revision < last_seen) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
                last_seen = snapshot.revision;
            } else if (result != SnapshotCopyResult::RejectedBusy) {
                failed.store(true, std::memory_order_relaxed);
                break;
            }
        } while (!writer_done.load(std::memory_order_acquire));
    });

    writer.join();
    reader.join();
    CHECK(!failed.load(std::memory_order_relaxed));

    RuntimeStatusSnapshotV1 latest{};
    CHECK(core.copy_runtime(latest) == SnapshotCopyResult::Copied);
    CHECK(latest.revision == kLastRevision);
    CHECK(runtime_pattern_is_consistent(latest));
}

void test_shutdown_finalization_is_typed_and_dry_run()
{
    ShutdownFinalizationPlan output{};
    UsbPowerObservation invalid{};
    CHECK(plan_shutdown_finalization(invalid, output) ==
          ShutdownFinalizationPlanResult::RejectedInvalid);
    CHECK(output.intended_action == ShutdownFinalAction::Invalid);

    invalid.sample_sequence = 1;
    invalid.present = 2;
    CHECK(plan_shutdown_finalization(invalid, output) ==
          ShutdownFinalizationPlanResult::RejectedInvalid);
    invalid.present = 1;
    invalid.hard_deadline_reached = 2;
    CHECK(plan_shutdown_finalization(invalid, output) ==
          ShutdownFinalizationPlanResult::RejectedInvalid);
    invalid.hard_deadline_reached = 1;
    invalid.reserved[3] = 1;
    CHECK(plan_shutdown_finalization(invalid, output) ==
          ShutdownFinalizationPlanResult::RejectedInvalid);

    UsbPowerObservation absent{};
    absent.present = 0;
    absent.cleanup_skipped_mask = 0x12;
    absent.cleanup_timed_out_mask = 0x24;
    absent.hard_deadline_reached = 1;
    absent.sample_sequence = 77;
    absent.sampled_at_monotonic_ms = 9001;
    CHECK(usb_power_observation_is_canonical(absent));
    CHECK(usb_power_observations_equal(absent, absent));
    CHECK(plan_shutdown_finalization(absent, output) ==
          ShutdownFinalizationPlanResult::Planned);
    CHECK(output.intended_action == ShutdownFinalAction::Gp15Kill);
    CHECK(output.cleanup_skipped_mask == absent.cleanup_skipped_mask);
    CHECK(output.cleanup_timed_out_mask == absent.cleanup_timed_out_mask);
    CHECK(output.hard_deadline_reached == absent.hard_deadline_reached);
    CHECK(output.sample_sequence == absent.sample_sequence);
    CHECK(output.sampled_at_monotonic_ms ==
          absent.sampled_at_monotonic_ms);
    CHECK(output.live_actuation == 0);
    CHECK(shutdown_finalization_plan_is_canonical(output));
    CHECK(shutdown_finalization_plans_equal(output, output));

    ShutdownFinalizationPlan live_actuation_forbidden = output;
    live_actuation_forbidden.live_actuation = 1;
    CHECK(!shutdown_finalization_plan_is_canonical(
        live_actuation_forbidden));
    CHECK(!shutdown_finalization_plans_equal(
        output, live_actuation_forbidden));

    UsbPowerObservation present = absent;
    present.present = 1;
    present.hard_deadline_reached = 0;
    present.sample_sequence = 78;
    CHECK(!usb_power_observations_equal(absent, present));
    const ShutdownFinalizationPlan absent_plan = output;
    CHECK(plan_shutdown_finalization(present, output) ==
          ShutdownFinalizationPlanResult::Planned);
    CHECK(output.intended_action == ShutdownFinalAction::WatchdogReboot);
    CHECK(output.sample_sequence == 78);
    CHECK(output.hard_deadline_reached == 0);
    CHECK(output.live_actuation == 0);
    CHECK(!shutdown_finalization_plans_equal(absent_plan, output));
}

std::string read_product_source(const char *const filename)
{
    const std::string test_path{__FILE__};
    const std::size_t separator = test_path.find_last_of("/\\");
    const std::string source_path =
        test_path.substr(0, separator + 1) + "../../src/boot_v2/" + filename;
    std::ifstream source{source_path};
    CHECK(source.is_open());
    return std::string{
        std::istreambuf_iterator<char>{source},
        std::istreambuf_iterator<char>{}};
}

std::string without_ascii_whitespace(const std::string &source)
{
    std::string result{};
    for (const char value : source) {
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n' &&
            value != '\f' && value != '\v') {
            result.push_back(value);
        }
    }
    return result;
}

void test_product_source_has_only_nonblocking_value_mechanisms()
{
    const std::string header = read_product_source("runtime_snapshot_core.hpp");
    const std::string source = read_product_source("runtime_snapshot_core.cpp");
    const std::string combined = header + source;
    const std::string normalized = without_ascii_whitespace(combined);

    const char *const forbidden[] = {
        "memcmp", "std::thread", "std::mutex", "malloc(", "calloc(",
        "realloc(", "operatornew", "FreeRTOS", "uart_", "modem_",
        "gpio_put", "watchdog_reboot", "flash_", "sleep_for", "clock_gettime",
    };
    for (const char *const needle : forbidden) {
        CHECK(combined.find(needle) == std::string::npos);
    }
    CHECK(normalized.find("boot_copy_gate_=ATOMIC_FLAG_INIT;") !=
          std::string::npos);
    CHECK(normalized.find("runtime_copy_gate_=ATOMIC_FLAG_INIT;") !=
          std::string::npos);
    CHECK(normalized.find("while(") == std::string::npos);
}

} // namespace

int main()
{
    test_numeric_defaults_and_layout();
    test_canonical_validation_and_fieldwise_equality();
    test_boot_freeze_is_exact_one_shot();
    test_ready_prepare_exactness_and_failure_free_commit();
    test_nonblocking_busy_precedence_and_latest_revision();
    test_concurrent_latest_copy_has_no_torn_combination();
    test_shutdown_finalization_is_typed_and_dry_run();
    test_product_source_has_only_nonblocking_value_mechanisms();

    if (g_failure_count != 0) {
        std::fprintf(
            stderr,
            "runtime_snapshot_core_test: %zu/%zu checks failed\n",
            g_failure_count,
            g_check_count);
        return 1;
    }
    std::printf(
        "runtime_snapshot_core_test: %zu checks passed\n", g_check_count);
    return 0;
}
