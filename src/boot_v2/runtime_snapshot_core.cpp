#include "runtime_snapshot_core.hpp"

#include <limits>

namespace boot_v2 {
namespace {

constexpr bool is_canonical_flag(const std::uint8_t value) noexcept
{
    return value <= 1;
}

constexpr bool is_known_health(const SnapshotHealth health) noexcept
{
    return health >= SnapshotHealth::Pass && health <= SnapshotHealth::Failed;
}

constexpr bool is_known_source(const SensorValueSource source) noexcept
{
    return source <= SensorValueSource::CrcFallback;
}

constexpr bool is_known_network_state(
    const RuntimeNetworkState state) noexcept
{
    return state >= RuntimeNetworkState::Offline &&
           state <= RuntimeNetworkState::Recovering;
}

constexpr bool is_known_adapter_state(
    const RuntimeAdapterState state) noexcept
{
    return state >= RuntimeAdapterState::Present &&
           state <= RuntimeAdapterState::Absent;
}

constexpr bool is_known_power_state(const RuntimePowerState state) noexcept
{
    return state >= RuntimePowerState::ExternalPower &&
           state <= RuntimePowerState::Off;
}

constexpr bool is_known_alarm_state(const RuntimeAlarmState state) noexcept
{
    return state >= RuntimeAlarmState::Clear &&
           state <= RuntimeAlarmState::Active;
}

constexpr bool is_known_ui_state(const RuntimeUiState state) noexcept
{
    return state >= RuntimeUiState::Boot &&
           state <= RuntimeUiState::Shutdown;
}

constexpr bool is_known_command_result(
    const RuntimeCommandResult result) noexcept
{
    return result >= RuntimeCommandResult::NoCommand &&
           result <= RuntimeCommandResult::Rejected;
}

bool effect_fields_equal(
    const RuntimeOwnerEffect &left,
    const RuntimeOwnerEffect &right) noexcept
{
    return left.kind == right.kind &&
           left.correlation_id == right.correlation_id &&
           left.attempt == right.attempt &&
           left.fault_code == right.fault_code;
}

bool exact_snapshot_effect_is_canonical(
    const RuntimeOwnerEffect effect,
    const RuntimeOwnerEffectKind expected_kind) noexcept
{
    return effect.kind == expected_kind &&
           effect.correlation_id != 0 &&
           effect.attempt.valid() &&
           effect.fault_code == RuntimeOwnerFaultCode::None;
}

} // namespace

bool sensor_quality_snapshot_is_canonical(
    const SensorQualitySnapshotV1 &snapshot) noexcept
{
    if (snapshot.schema_version != 1 ||
        !is_known_health(snapshot.health) ||
        !is_canonical_flag(snapshot.has_value) ||
        !is_known_source(snapshot.value_source) ||
        !is_canonical_flag(snapshot.stale) ||
        !is_canonical_flag(snapshot.clock_valid) ||
        snapshot.reserved[0] != 0 || snapshot.reserved[1] != 0) {
        return false;
    }
    if (snapshot.has_value == 0) {
        if (snapshot.value_source != SensorValueSource::None ||
            snapshot.value_deci_celsius != 0) {
            return false;
        }
    } else if (snapshot.value_source == SensorValueSource::None) {
        return false;
    }
    return snapshot.clock_valid != 0 ||
           snapshot.last_valid_at_unix_seconds == 0;
}

bool boot_runtime_snapshot_is_canonical(
    const BootRuntimeSnapshotV1 &snapshot) noexcept
{
    const bool liveness_passed =
        snapshot.last_completed_stage ==
            BootCompletedStage::PostConfigLivenessPassed &&
        snapshot.post_config_liveness == 1;
    const bool config_handoff =
        snapshot.last_completed_stage ==
            BootCompletedStage::ConfigAppliedHandoff &&
        snapshot.post_config_liveness == 0;
    if (snapshot.schema_version != 1 ||
        !is_known_health(snapshot.health) ||
        (!liveness_passed && !config_handoff) ||
        snapshot.config_valid != 1 || snapshot.transport_ready != 1 ||
        snapshot.subscription_alive != 1 ||
        !is_canonical_flag(snapshot.reboot_guard) ||
        snapshot.reserved[0] != 0 || snapshot.reserved[1] != 0 ||
        snapshot.reserved[2] != 0 || snapshot.reserved[3] != 0 ||
        snapshot.hardware_revision == 0 || snapshot.firmware_build_id == 0 ||
        snapshot.config_version == 0 || snapshot.pdp_session_id == 0 ||
        snapshot.mqtt_session_id == 0 || snapshot.mqtt_generation == 0 ||
        snapshot.config_apply_epoch == 0) {
        return false;
    }
    for (const SensorQualitySnapshotV1 &sensor : snapshot.sensors) {
        if (!sensor_quality_snapshot_is_canonical(sensor)) {
            return false;
        }
    }
    return true;
}

bool runtime_status_snapshot_is_canonical(
    const RuntimeStatusSnapshotV1 &snapshot) noexcept
{
    if (snapshot.schema_version != 1 || snapshot.revision == 0 ||
        !is_known_network_state(snapshot.network_state) ||
        !is_known_adapter_state(snapshot.adapter_state) ||
        !is_known_power_state(snapshot.power_state) ||
        !is_known_alarm_state(snapshot.alarm_state) ||
        !is_known_ui_state(snapshot.ui_state) ||
        !is_known_command_result(snapshot.last_command_result) ||
        snapshot.reserved != 0 || snapshot.config_version == 0) {
        return false;
    }
    const bool no_command =
        snapshot.last_command_result == RuntimeCommandResult::NoCommand;
    if (no_command != (snapshot.last_command_id == 0)) {
        return false;
    }
    for (const SensorQualitySnapshotV1 &sensor : snapshot.sensors) {
        if (!sensor_quality_snapshot_is_canonical(sensor)) {
            return false;
        }
    }
    return true;
}

bool runtime_activation_grant_is_canonical(
    const RuntimeActivationGrant &grant) noexcept
{
    return grant.liveness.valid() &&
           grant.snapshot_correlation_id != 0 &&
           grant.boot_end_correlation_id != 0 &&
           grant.boot_end_dispatch_sequence != 0;
}

bool sensor_quality_snapshots_equal(
    const SensorQualitySnapshotV1 &left,
    const SensorQualitySnapshotV1 &right) noexcept
{
    return left.schema_version == right.schema_version &&
           left.health == right.health &&
           left.has_value == right.has_value &&
           left.value_source == right.value_source &&
           left.stale == right.stale &&
           left.clock_valid == right.clock_valid &&
           left.reserved[0] == right.reserved[0] &&
           left.reserved[1] == right.reserved[1] &&
           left.value_deci_celsius == right.value_deci_celsius &&
           left.consecutive_failures == right.consecutive_failures &&
           left.last_valid_at_unix_seconds ==
               right.last_valid_at_unix_seconds;
}

bool boot_runtime_snapshots_equal(
    const BootRuntimeSnapshotV1 &left,
    const BootRuntimeSnapshotV1 &right) noexcept
{
    if (left.schema_version != right.schema_version ||
        left.health != right.health ||
        left.last_completed_stage != right.last_completed_stage ||
        left.config_valid != right.config_valid ||
        left.transport_ready != right.transport_ready ||
        left.subscription_alive != right.subscription_alive ||
        left.post_config_liveness != right.post_config_liveness ||
        left.reboot_guard != right.reboot_guard ||
        left.reserved[0] != right.reserved[0] ||
        left.reserved[1] != right.reserved[1] ||
        left.reserved[2] != right.reserved[2] ||
        left.reserved[3] != right.reserved[3] ||
        left.hardware_revision != right.hardware_revision ||
        left.firmware_build_id != right.firmware_build_id ||
        left.config_version != right.config_version ||
        left.pdp_session_id != right.pdp_session_id ||
        left.mqtt_session_id != right.mqtt_session_id ||
        left.mqtt_generation != right.mqtt_generation ||
        left.config_apply_epoch != right.config_apply_epoch ||
        left.recovery_summary != right.recovery_summary) {
        return false;
    }
    for (std::size_t index = 0; index < left.sensors.size(); ++index) {
        if (!sensor_quality_snapshots_equal(
                left.sensors[index], right.sensors[index])) {
            return false;
        }
    }
    return true;
}

bool runtime_status_snapshots_equal(
    const RuntimeStatusSnapshotV1 &left,
    const RuntimeStatusSnapshotV1 &right) noexcept
{
    if (left.schema_version != right.schema_version ||
        left.network_state != right.network_state ||
        left.adapter_state != right.adapter_state ||
        left.power_state != right.power_state ||
        left.alarm_state != right.alarm_state ||
        left.ui_state != right.ui_state ||
        left.last_command_result != right.last_command_result ||
        left.reserved != right.reserved || left.revision != right.revision ||
        left.config_version != right.config_version ||
        left.last_command_id != right.last_command_id ||
        left.queue_summary != right.queue_summary ||
        left.drop_summary != right.drop_summary ||
        left.recovery_summary != right.recovery_summary) {
        return false;
    }
    for (std::size_t index = 0; index < left.sensors.size(); ++index) {
        if (!sensor_quality_snapshots_equal(
                left.sensors[index], right.sensors[index])) {
            return false;
        }
    }
    return true;
}

bool runtime_activation_grants_equal(
    const RuntimeActivationGrant &left,
    const RuntimeActivationGrant &right) noexcept
{
    return left.liveness == right.liveness &&
           left.snapshot_correlation_id == right.snapshot_correlation_id &&
           left.boot_end_correlation_id == right.boot_end_correlation_id &&
           left.boot_end_dispatch_sequence ==
               right.boot_end_dispatch_sequence;
}

bool usb_power_observation_is_canonical(
    const UsbPowerObservation &observation) noexcept
{
    return is_canonical_flag(observation.present) &&
           is_canonical_flag(observation.hard_deadline_reached) &&
           observation.sample_sequence != 0 &&
           observation.reserved[0] == 0 &&
           observation.reserved[1] == 0 &&
           observation.reserved[2] == 0 &&
           observation.reserved[3] == 0;
}

bool shutdown_finalization_plan_is_canonical(
    const ShutdownFinalizationPlan &plan) noexcept
{
    return plan.schema_version == 1 &&
           plan.intended_action >= ShutdownFinalAction::Gp15Kill &&
           plan.intended_action <= ShutdownFinalAction::WatchdogReboot &&
           is_canonical_flag(plan.hard_deadline_reached) &&
           plan.live_actuation == 0 &&
           plan.reserved[0] == 0 && plan.reserved[1] == 0 &&
           plan.sample_sequence != 0;
}

bool usb_power_observations_equal(
    const UsbPowerObservation &left,
    const UsbPowerObservation &right) noexcept
{
    return left.present == right.present &&
           left.cleanup_skipped_mask == right.cleanup_skipped_mask &&
           left.cleanup_timed_out_mask == right.cleanup_timed_out_mask &&
           left.hard_deadline_reached == right.hard_deadline_reached &&
           left.sample_sequence == right.sample_sequence &&
           left.sampled_at_monotonic_ms ==
               right.sampled_at_monotonic_ms &&
           left.reserved[0] == right.reserved[0] &&
           left.reserved[1] == right.reserved[1] &&
           left.reserved[2] == right.reserved[2] &&
           left.reserved[3] == right.reserved[3];
}

bool shutdown_finalization_plans_equal(
    const ShutdownFinalizationPlan &left,
    const ShutdownFinalizationPlan &right) noexcept
{
    return left.schema_version == right.schema_version &&
           left.intended_action == right.intended_action &&
           left.cleanup_skipped_mask == right.cleanup_skipped_mask &&
           left.cleanup_timed_out_mask == right.cleanup_timed_out_mask &&
           left.hard_deadline_reached == right.hard_deadline_reached &&
           left.live_actuation == right.live_actuation &&
           left.reserved[0] == right.reserved[0] &&
           left.reserved[1] == right.reserved[1] &&
           left.sample_sequence == right.sample_sequence &&
           left.sampled_at_monotonic_ms ==
               right.sampled_at_monotonic_ms;
}

ShutdownFinalizationPlanResult plan_shutdown_finalization(
    const UsbPowerObservation observation,
    ShutdownFinalizationPlan &output) noexcept
{
    if (!usb_power_observation_is_canonical(observation)) {
        return ShutdownFinalizationPlanResult::RejectedInvalid;
    }
    ShutdownFinalizationPlan plan{};
    plan.intended_action = observation.present != 0
                               ? ShutdownFinalAction::WatchdogReboot
                               : ShutdownFinalAction::Gp15Kill;
    plan.cleanup_skipped_mask = observation.cleanup_skipped_mask;
    plan.cleanup_timed_out_mask = observation.cleanup_timed_out_mask;
    plan.hard_deadline_reached = observation.hard_deadline_reached;
    plan.sample_sequence = observation.sample_sequence;
    plan.sampled_at_monotonic_ms = observation.sampled_at_monotonic_ms;
    output = plan;
    return ShutdownFinalizationPlanResult::Planned;
}

RuntimeSnapshotReadyCommitPermit::RuntimeSnapshotReadyCommitPermit(
    RuntimeSnapshotCore *const owner,
    const ReadyPrepareResult result,
    const RuntimeActivationGrant grant,
    const std::uint8_t holds_gate) noexcept
    : owner_(owner), result_(result), grant_(grant), holds_gate_(holds_gate)
{
}

RuntimeSnapshotReadyCommitPermit::RuntimeSnapshotReadyCommitPermit(
    RuntimeSnapshotReadyCommitPermit &&other) noexcept
    : owner_(other.owner_),
      result_(other.result_),
      grant_(other.grant_),
      holds_gate_(other.holds_gate_)
{
    other.owner_ = nullptr;
    other.holds_gate_ = 0;
}

RuntimeSnapshotReadyCommitPermit &
RuntimeSnapshotReadyCommitPermit::operator=(
    RuntimeSnapshotReadyCommitPermit &&other) noexcept
{
    if (this == &other) {
        return *this;
    }
    if (owner_ != nullptr && holds_gate_ != 0) {
        owner_->abandon_ready(*this);
    }
    owner_ = other.owner_;
    result_ = other.result_;
    grant_ = other.grant_;
    holds_gate_ = other.holds_gate_;
    other.owner_ = nullptr;
    other.holds_gate_ = 0;
    return *this;
}

RuntimeSnapshotReadyCommitPermit::~RuntimeSnapshotReadyCommitPermit() noexcept
{
    if (owner_ != nullptr && holds_gate_ != 0) {
        owner_->abandon_ready(*this);
    }
}

ReadyPrepareResult RuntimeSnapshotReadyCommitPermit::result() const noexcept
{
    return result_;
}

BootSnapshotFreezeResult RuntimeSnapshotCore::freeze_boot(
    const RuntimeOwnerEffect freeze_effect,
    const BootRuntimeSnapshotV1 snapshot) noexcept
{
    if (!exact_snapshot_effect_is_canonical(
            freeze_effect, RuntimeOwnerEffectKind::FreezeBootSnapshot) ||
        !boot_runtime_snapshot_is_canonical(snapshot) ||
        snapshot.mqtt_session_id != freeze_effect.attempt.mqtt_session_id ||
        snapshot.mqtt_generation != freeze_effect.attempt.mqtt_generation ||
        snapshot.config_apply_epoch !=
            freeze_effect.attempt.config_apply_epoch) {
        return BootSnapshotFreezeResult::RejectedInvalid;
    }
    if (boot_copy_gate_.test_and_set(std::memory_order_acquire)) {
        return BootSnapshotFreezeResult::RejectedBusy;
    }

    BootSnapshotFreezeResult result{};
    if (boot_published_ == 0) {
        boot_snapshot_ = snapshot;
        freeze_effect_ = freeze_effect;
        boot_published_ = 1;
        result = BootSnapshotFreezeResult::Accepted;
    } else if (boot_runtime_snapshots_equal(boot_snapshot_, snapshot) &&
               effect_fields_equal(freeze_effect_, freeze_effect)) {
        result = BootSnapshotFreezeResult::AcceptedDuplicate;
    } else {
        result = BootSnapshotFreezeResult::RejectedAlreadyFrozen;
    }
    boot_copy_gate_.clear(std::memory_order_release);
    return result;
}

SnapshotCopyResult RuntimeSnapshotCore::copy_boot(
    BootRuntimeSnapshotV1 &output) const noexcept
{
    if (boot_copy_gate_.test_and_set(std::memory_order_acquire)) {
        return SnapshotCopyResult::RejectedBusy;
    }
    if (boot_published_ == 0) {
        boot_copy_gate_.clear(std::memory_order_release);
        return SnapshotCopyResult::RejectedNotReady;
    }
    output = boot_snapshot_;
    boot_copy_gate_.clear(std::memory_order_release);
    return SnapshotCopyResult::Copied;
}

SnapshotCopyResult RuntimeSnapshotCore::copy_activation_grant(
    RuntimeActivationGrant &output) const noexcept
{
    if (boot_copy_gate_.test_and_set(std::memory_order_acquire)) {
        return SnapshotCopyResult::RejectedBusy;
    }
    if (ready_published_ == 0) {
        boot_copy_gate_.clear(std::memory_order_release);
        return SnapshotCopyResult::RejectedNotReady;
    }
    output = activation_grant_;
    boot_copy_gate_.clear(std::memory_order_release);
    return SnapshotCopyResult::Copied;
}

RuntimeSnapshotReadyCommitPermit RuntimeSnapshotCore::prepare_ready(
    const RuntimeActivationGrant grant,
    const RuntimeOwnerEffect acknowledged_end_boot,
    const std::uint32_t acknowledged_dispatch_sequence) noexcept
{
    const bool candidate_is_exact =
        runtime_activation_grant_is_canonical(grant) &&
        exact_snapshot_effect_is_canonical(
            acknowledged_end_boot,
            RuntimeOwnerEffectKind::EndBootOrchestration) &&
        acknowledged_dispatch_sequence != 0 &&
        grant.liveness == acknowledged_end_boot.attempt &&
        grant.boot_end_correlation_id ==
            acknowledged_end_boot.correlation_id &&
        grant.boot_end_dispatch_sequence == acknowledged_dispatch_sequence;
    if (!candidate_is_exact) {
        return {nullptr, ReadyPrepareResult::RejectedInvalid, {}, 0};
    }
    if (boot_copy_gate_.test_and_set(std::memory_order_acquire)) {
        return {nullptr, ReadyPrepareResult::RejectedBusy, {}, 0};
    }
    if (boot_published_ == 0) {
        boot_copy_gate_.clear(std::memory_order_release);
        return {nullptr, ReadyPrepareResult::RejectedNotFrozen, {}, 0};
    }
    const bool exact_freeze =
        grant.liveness == freeze_effect_.attempt &&
        grant.snapshot_correlation_id == freeze_effect_.correlation_id;
    if (!exact_freeze) {
        boot_copy_gate_.clear(std::memory_order_release);
        return {nullptr, ReadyPrepareResult::RejectedInvalid, {}, 0};
    }
    if (ready_published_ != 0) {
        const ReadyPrepareResult result =
            runtime_activation_grants_equal(activation_grant_, grant)
                ? ReadyPrepareResult::AcceptedDuplicate
                : ReadyPrepareResult::RejectedInvalid;
        boot_copy_gate_.clear(std::memory_order_release);
        return {nullptr, result, {}, 0};
    }

    return {this, ReadyPrepareResult::Prepared, grant, 1};
}

void RuntimeSnapshotCore::commit_ready(
    RuntimeSnapshotReadyCommitPermit &&permit) noexcept
{
    if (permit.owner_ != this || permit.holds_gate_ == 0 ||
        permit.result_ != ReadyPrepareResult::Prepared) {
        return;
    }
    activation_grant_ = permit.grant_;
    ready_published_ = 1;
    permit.owner_ = nullptr;
    permit.holds_gate_ = 0;
    boot_copy_gate_.clear(std::memory_order_release);
}

void RuntimeSnapshotCore::abandon_ready(
    RuntimeSnapshotReadyCommitPermit &permit) noexcept
{
    if (permit.owner_ != this || permit.holds_gate_ == 0) {
        return;
    }
    permit.owner_ = nullptr;
    permit.holds_gate_ = 0;
    boot_copy_gate_.clear(std::memory_order_release);
}

RuntimeSnapshotPublishResult RuntimeSnapshotCore::publish_runtime(
    const RuntimeStatusSnapshotV1 snapshot) noexcept
{
    if (snapshot.revision == 0) {
        return RuntimeSnapshotPublishResult::RejectedRevisionZero;
    }
    if (!runtime_status_snapshot_is_canonical(snapshot)) {
        return RuntimeSnapshotPublishResult::RejectedInvalid;
    }
    if (runtime_copy_gate_.test_and_set(std::memory_order_acquire)) {
        return RuntimeSnapshotPublishResult::RejectedBusy;
    }
    if (boot_copy_gate_.test_and_set(std::memory_order_acquire)) {
        runtime_copy_gate_.clear(std::memory_order_release);
        return RuntimeSnapshotPublishResult::RejectedBusy;
    }
    const bool ready = ready_published_ != 0;
    boot_copy_gate_.clear(std::memory_order_release);
    if (!ready) {
        runtime_copy_gate_.clear(std::memory_order_release);
        return RuntimeSnapshotPublishResult::RejectedNotReady;
    }

    if (runtime_published_ != 0 &&
        runtime_snapshot_.revision ==
            std::numeric_limits<std::uint32_t>::max()) {
        runtime_copy_gate_.clear(std::memory_order_release);
        return RuntimeSnapshotPublishResult::RejectedRevisionSaturated;
    }
    if ((runtime_published_ == 0 && snapshot.revision != 1) ||
        (runtime_published_ != 0 &&
         snapshot.revision <= runtime_snapshot_.revision)) {
        runtime_copy_gate_.clear(std::memory_order_release);
        return RuntimeSnapshotPublishResult::RejectedRevisionNotIncreasing;
    }

    runtime_snapshot_ = snapshot;
    runtime_published_ = 1;
    runtime_copy_gate_.clear(std::memory_order_release);
    return RuntimeSnapshotPublishResult::Accepted;
}

SnapshotCopyResult RuntimeSnapshotCore::copy_runtime(
    RuntimeStatusSnapshotV1 &output) const noexcept
{
    if (runtime_copy_gate_.test_and_set(std::memory_order_acquire)) {
        return SnapshotCopyResult::RejectedBusy;
    }
    if (runtime_published_ == 0) {
        runtime_copy_gate_.clear(std::memory_order_release);
        return SnapshotCopyResult::RejectedNotReady;
    }
    output = runtime_snapshot_;
    runtime_copy_gate_.clear(std::memory_order_release);
    return SnapshotCopyResult::Copied;
}

} // namespace boot_v2
