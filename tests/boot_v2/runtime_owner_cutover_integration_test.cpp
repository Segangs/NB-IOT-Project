#include "runtime_owner_rtos_owner_loop.hpp"
#include "runtime_owner_task_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace {

std::size_t g_check_count = 0;
std::size_t g_failure_count = 0;
std::size_t g_allocation_count = 0;
std::size_t g_deallocation_count = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++g_check_count;                                                       \
        if (!(condition)) {                                                    \
            ++g_failure_count;                                                 \
            std::fprintf(                                                      \
                stderr,                                                        \
                "CHECK failed at %s:%d: %s\n",                               \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #condition);                                                   \
        }                                                                      \
    } while (false)

using namespace boot_v2;

SensorQualitySnapshotV1 make_sensor(
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

BootRuntimeSnapshotV1 make_boot_snapshot(
    const RuntimeOwnerExecutorCommand &freeze) noexcept
{
    BootRuntimeSnapshotV1 snapshot{};
    snapshot.health = SnapshotHealth::Pass;
    snapshot.last_completed_stage =
        BootCompletedStage::PostConfigLivenessPassed;
    snapshot.config_valid = 1;
    snapshot.transport_ready = 1;
    snapshot.subscription_alive = 1;
    snapshot.post_config_liveness = 1;
    snapshot.hardware_revision = 2;
    snapshot.firmware_build_id = 210721;
    snapshot.config_version = 1;
    snapshot.sensors[0] = make_sensor(231, 1700000001);
    snapshot.sensors[1] = make_sensor(244, 1700000002);
    snapshot.pdp_session_id = 17;
    snapshot.mqtt_session_id =
        freeze.source.effect.attempt.mqtt_session_id;
    snapshot.mqtt_generation =
        freeze.source.effect.attempt.mqtt_generation;
    snapshot.config_apply_epoch =
        freeze.source.effect.attempt.config_apply_epoch;
    return snapshot;
}

RuntimeStatusSnapshotV1 make_runtime_snapshot() noexcept
{
    RuntimeStatusSnapshotV1 snapshot{};
    snapshot.network_state = RuntimeNetworkState::Online;
    snapshot.adapter_state = RuntimeAdapterState::Present;
    snapshot.power_state = RuntimePowerState::ExternalPower;
    snapshot.alarm_state = RuntimeAlarmState::Clear;
    snapshot.ui_state = RuntimeUiState::PeriodicReady;
    snapshot.last_command_result = RuntimeCommandResult::NoCommand;
    snapshot.revision = 1;
    snapshot.sensors[0] = make_sensor(232, 1700000011);
    snapshot.sensors[1] = make_sensor(245, 1700000012);
    snapshot.config_version = 1;
    return snapshot;
}

RuntimeOwnerExecutorCommand require_command(
    RuntimeOwnerExecutorPort &executor,
    const RuntimeOwnerDeviceOperationKind expected_kind)
{
    RuntimeOwnerExecutorCommand command{};
    CHECK(executor.peek_command(command) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(command.kind == expected_kind);
    return command;
}

RuntimeOwnerTaskCycleResult process_cycle(
    RuntimeOwnerTaskCore &core,
    const RuntimeOwnerTaskCycleInput input = {}) noexcept
{
    return RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);
}

void step_expect_processed(RuntimeOwnerTaskCore &core)
{
    const RuntimeOwnerTaskCycleResult cycle = process_cycle(core);
    CHECK(cycle.disposition == RuntimeOwnerTaskCycleDisposition::Processed);
}

void drive_to_awaiting_config(
    RuntimeOwnerTaskCore &core,
    RuntimeOwnerExecutorPort &executor,
    const std::uint32_t mqtt_session_id)
{
    RuntimeOwnerTaskCycleInput transport_cycle{};
    transport_cycle.transport_pending = 1;
    CHECK(process_cycle(core, transport_cycle).transport_result ==
          OwnerRequestResult::Accepted);
    step_expect_processed(core);
    const RuntimeOwnerExecutorCommand transport = require_command(
        executor, RuntimeOwnerDeviceOperationKind::OpenTransport);
    CHECK(executor.acknowledge_command(transport) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(executor.transport_established(transport, mqtt_session_id) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(core);
    CHECK(core.redacted_status().phase ==
          RuntimeOwnerPhase::AwaitingConfigCommit);
}

void test_capabilities_are_not_forgeable()
{
    static_assert(!std::is_default_constructible<
                  RuntimeOwnerCutoverPermit>::value);
    static_assert(!std::is_copy_constructible<
                  RuntimeOwnerCutoverPermit>::value);
    static_assert(!std::is_move_constructible<
                  RuntimeOwnerCutoverPermit>::value);
    static_assert(!std::is_constructible<
                  RuntimeOwnerCutoverPermit, std::uint32_t>::value);

    static_assert(!std::is_default_constructible<
                  RuntimeOwnerExecutorPort>::value);
    static_assert(!std::is_copy_constructible<
                  RuntimeOwnerExecutorPort>::value);
    static_assert(!std::is_move_constructible<
                  RuntimeOwnerExecutorPort>::value);
    static_assert(!std::is_constructible<
                  RuntimeOwnerExecutorPort, RuntimeOwnerTaskCore *>::value);

    static_assert(!std::is_default_constructible<
                  RuntimeOwnerPowerButtonShutdownPort>::value);
    static_assert(!std::is_default_constructible<
                  RuntimeOwnerAdapterLossShutdownPort>::value);
    static_assert(!std::is_default_constructible<
                  RuntimeOwnerAuthenticatedCommandShutdownPort>::value);
}

void test_activation_identity_contract()
{
    static_assert(static_cast<std::uint8_t>(
                      RuntimeOwnerTaskActivationResult::RejectedInvalid) == 0);
    static_assert(static_cast<std::uint8_t>(
                      RuntimeOwnerTaskActivationResult::Activated) == 1);
    static_assert(static_cast<std::uint8_t>(
                      RuntimeOwnerTaskActivationResult::AlreadyActive) == 2);
    static_assert(static_cast<std::uint8_t>(
                      RuntimeOwnerTaskActivationResult::RejectedTerminal) == 3);

    RuntimeOwnerTaskCore core{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(core, 0) ==
          RuntimeOwnerTaskActivationResult::RejectedInvalid);
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(core, 0x13579bdu) ==
          RuntimeOwnerTaskActivationResult::Activated);
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(core, 0x13579bdu) ==
          RuntimeOwnerTaskActivationResult::AlreadyActive);
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(core, 0x2468aceu) ==
          RuntimeOwnerTaskActivationResult::RejectedInvalid);

    RuntimeOwnerTaskCore terminal{};
    RuntimeOwnerTaskCoreTestPeer::fixture_terminal(terminal);
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(terminal, 0x1u) ==
          RuntimeOwnerTaskActivationResult::RejectedTerminal);
}

void test_dormant_capability_operations_are_closed()
{
    RuntimeOwnerTaskCore core{};
    auto executor = RuntimeOwnerTaskCoreTestPeer::executor_port(core);
    auto power_button =
        RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(core);
    auto adapter_loss =
        RuntimeOwnerTaskCoreTestPeer::adapter_loss_shutdown_port(core);
    auto authenticated =
        RuntimeOwnerTaskCoreTestPeer::authenticated_command_shutdown_port(core);
    RuntimeOwnerExecutorCommand command{};
    CHECK(executor.peek_command(command) ==
          RuntimeOwnerExecutorResult::RejectedInactive);
    CHECK(power_button.request(1, 1) ==
          RuntimeOwnerShutdownRequestResult::RejectedInactive);
    CHECK(adapter_loss.request(1, 1) ==
          RuntimeOwnerShutdownRequestResult::RejectedInactive);
    CHECK(authenticated.request(1, 1) ==
          RuntimeOwnerShutdownRequestResult::RejectedInactive);
}

void test_deadline_and_disconnect_typed_bridges_use_canonical_context()
{
    RuntimeOwnerTaskCore disconnected{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(disconnected, 0x81u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto disconnected_executor =
        RuntimeOwnerTaskCoreTestPeer::executor_port(disconnected);
    drive_to_awaiting_config(disconnected, disconnected_executor, 61);
    CHECK(disconnected_executor.transport_disconnected(0, 1, 0) ==
          RuntimeOwnerExecutorResult::RejectedInvalid);
    CHECK(disconnected_executor.transport_disconnected(60, 1, 0) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    CHECK(disconnected_executor.transport_disconnected(61, 2, 0) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    CHECK(disconnected_executor.transport_disconnected(61, 1, 0) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(disconnected_executor.transport_disconnected(61, 1, 0) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(disconnected);
    step_expect_processed(disconnected);
    CHECK(disconnected.redacted_status().phase ==
          RuntimeOwnerPhase::RecoveryPending);

    RuntimeOwnerTaskCore deadline{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(deadline, 0x82u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto deadline_executor =
        RuntimeOwnerTaskCoreTestPeer::executor_port(deadline);
    drive_to_awaiting_config(deadline, deadline_executor, 62);
    CHECK(deadline_executor.config_committed(9) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(deadline);
    step_expect_processed(deadline);
    const RuntimeOwnerExecutorCommand probe = require_command(
        deadline_executor, RuntimeOwnerDeviceOperationKind::ProbeAt);
    CHECK(deadline_executor.acknowledge_command(probe) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(deadline_executor.liveness_deadline_expired(probe, 0) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(deadline);
    CHECK(deadline.redacted_status().phase ==
          RuntimeOwnerPhase::RecoveryPending);
}

template <typename ShutdownPort>
void exercise_shutdown_source_contract(
    RuntimeOwnerTaskCore &core,
    ShutdownPort &port,
    const std::uint32_t incident_correlation_id)
{
    CHECK(port.request(0, incident_correlation_id) ==
          RuntimeOwnerShutdownRequestResult::RejectedInvalid);
    CHECK(port.request(2, incident_correlation_id) ==
          RuntimeOwnerShutdownRequestResult::Accepted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(core));
    CHECK(port.request(2, incident_correlation_id) ==
          RuntimeOwnerShutdownRequestResult::AcceptedDuplicate);
    CHECK(port.request(2, incident_correlation_id + 1) ==
          RuntimeOwnerShutdownRequestResult::RejectedStale);
    CHECK(port.request(1, incident_correlation_id) ==
          RuntimeOwnerShutdownRequestResult::RejectedStale);
    CHECK(port.request(3, incident_correlation_id) ==
          RuntimeOwnerShutdownRequestResult::RejectedTerminal);
    step_expect_processed(core);
    CHECK(core.redacted_status().state == RuntimeOwnerTaskState::Terminal);
    CHECK(RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(core));
}

void test_three_shutdown_sources_have_independent_exact_provenance()
{
    RuntimeOwnerTaskCore power_button_core{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(
              power_button_core, 0x91u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto power_button = RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(
        power_button_core);
    exercise_shutdown_source_contract(power_button_core, power_button, 101);

    RuntimeOwnerTaskCore authenticated_command_core{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(
              authenticated_command_core, 0x92u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto authenticated_command =
        RuntimeOwnerTaskCoreTestPeer::authenticated_command_shutdown_port(
            authenticated_command_core);
    exercise_shutdown_source_contract(
        authenticated_command_core, authenticated_command, 201);

    RuntimeOwnerTaskCore adapter_loss_core{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(
              adapter_loss_core, 0x93u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto adapter_loss =
        RuntimeOwnerTaskCoreTestPeer::adapter_loss_shutdown_port(
            adapter_loss_core);
    exercise_shutdown_source_contract(
        adapter_loss_core, adapter_loss, 301);
}

void test_composed_cutover_flow_is_allocation_free()
{
    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;

    RuntimeOwnerTaskCore core{};
    CHECK(core.redacted_status().state == RuntimeOwnerTaskState::Dormant);
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(core, 0x71u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto executor = RuntimeOwnerTaskCoreTestPeer::executor_port(core);
    RuntimeOwnerTaskCycleInput transport_cycle{};
    transport_cycle.transport_pending = 1;
    CHECK(process_cycle(core, transport_cycle).transport_result ==
          OwnerRequestResult::Accepted);
    step_expect_processed(core);
    const RuntimeOwnerExecutorCommand transport = require_command(
        executor, RuntimeOwnerDeviceOperationKind::OpenTransport);
    CHECK(executor.acknowledge_command(transport) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(executor.transport_established(transport, 41) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(core);

    CHECK(executor.config_committed(7) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(core);

    constexpr RuntimeOwnerDeviceOperationKind liveness_kinds[] = {
        RuntimeOwnerDeviceOperationKind::ProbeAt,
        RuntimeOwnerDeviceOperationKind::PublishProbe,
        RuntimeOwnerDeviceOperationKind::VerifySubscription,
        RuntimeOwnerDeviceOperationKind::PullFollowupConfig,
    };
    for (const RuntimeOwnerDeviceOperationKind kind : liveness_kinds) {
        step_expect_processed(core);
        const RuntimeOwnerExecutorCommand command =
            require_command(executor, kind);
        CHECK(executor.acknowledge_command(command) ==
              RuntimeOwnerExecutorResult::Accepted);
        CHECK(executor.liveness_succeeded(command) ==
              RuntimeOwnerExecutorResult::Accepted);
        step_expect_processed(core);
    }

    step_expect_processed(core);
    const RuntimeOwnerExecutorCommand freeze = require_command(
        executor, RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot);
    CHECK(executor.acknowledge_command(freeze) ==
          RuntimeOwnerExecutorResult::Accepted);

    BootRuntimeSnapshotV1 invalid_snapshot = make_boot_snapshot(freeze);
    invalid_snapshot.config_version = 0;
    CHECK(executor.snapshot_succeeded(freeze, invalid_snapshot) ==
          RuntimeOwnerExecutorResult::RejectedSnapshotStore);
    CHECK(executor.snapshot_failed(freeze, 0x51u) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(core);
    CHECK(core.redacted_status().phase ==
          RuntimeOwnerPhase::RecoveryPending);

    RuntimeOwnerTaskCore success_core{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(success_core, 0x72u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto success_executor =
        RuntimeOwnerTaskCoreTestPeer::executor_port(success_core);
    auto success_power_button =
        RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(success_core);

    RuntimeOwnerTaskCycleInput success_transport_cycle{};
    success_transport_cycle.transport_pending = 1;
    CHECK(process_cycle(success_core, success_transport_cycle).transport_result ==
          OwnerRequestResult::Accepted);
    step_expect_processed(success_core);
    const RuntimeOwnerExecutorCommand success_transport = require_command(
        success_executor, RuntimeOwnerDeviceOperationKind::OpenTransport);
    CHECK(success_executor.acknowledge_command(success_transport) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(success_executor.transport_established(success_transport, 43) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(success_core);
    CHECK(success_executor.config_committed(8) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(success_core);
    for (const RuntimeOwnerDeviceOperationKind kind : liveness_kinds) {
        step_expect_processed(success_core);
        const RuntimeOwnerExecutorCommand command =
            require_command(success_executor, kind);
        CHECK(success_executor.acknowledge_command(command) ==
              RuntimeOwnerExecutorResult::Accepted);
        CHECK(success_executor.liveness_succeeded(command) ==
              RuntimeOwnerExecutorResult::Accepted);
        step_expect_processed(success_core);
    }
    step_expect_processed(success_core);
    const RuntimeOwnerExecutorCommand success_freeze = require_command(
        success_executor,
        RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot);
    CHECK(success_executor.acknowledge_command(success_freeze) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(success_executor.snapshot_succeeded(
              success_freeze, make_boot_snapshot(success_freeze)) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(success_core);
    step_expect_processed(success_core);

    const RuntimeOwnerExecutorCommand end_boot = require_command(
        success_executor,
        RuntimeOwnerDeviceOperationKind::EndBootOrchestration);
    RuntimeOwnerTaskCycleInput premature_normal{};
    premature_normal.normal_pending = 1;
    premature_normal.normal = {
        NormalIntentKind::PublishTelemetry, 0x01, 215, 17, 1};
    CHECK(process_cycle(success_core, premature_normal).normal_result ==
          NormalSubmitResult::RejectedNotReady);
    RuntimeOwnerExecutorCommand wrong_end_boot = end_boot;
    ++wrong_end_boot.source.dispatch_sequence;
    CHECK(success_executor.commit_end_boot_delivery(wrong_end_boot) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    CHECK(success_core.redacted_status().runtime_ready == 0);
    CHECK(success_executor.acknowledge_command(end_boot) ==
          RuntimeOwnerExecutorResult::RejectedEndBootRequiresCommit);
    CHECK(success_executor.commit_end_boot_delivery(end_boot) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(success_executor.commit_end_boot_delivery(end_boot) ==
          RuntimeOwnerExecutorResult::AcceptedDuplicate);
    RuntimeOwnerExecutorCommand duplicate_wrong_correlation = end_boot;
    ++duplicate_wrong_correlation.source.effect.correlation_id;
    CHECK(success_executor.commit_end_boot_delivery(
              duplicate_wrong_correlation) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    RuntimeOwnerExecutorCommand duplicate_wrong_sequence = end_boot;
    ++duplicate_wrong_sequence.source.dispatch_sequence;
    CHECK(success_executor.commit_end_boot_delivery(
              duplicate_wrong_sequence) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    RuntimeOwnerExecutorCommand duplicate_wrong_token = end_boot;
    ++duplicate_wrong_token.source.effect.attempt.mqtt_generation;
    CHECK(success_executor.commit_end_boot_delivery(duplicate_wrong_token) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    CHECK(success_core.redacted_status().runtime_ready == 1);
    CHECK(!RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(success_core));
    RuntimeOwnerTaskCycleInput malformed_ready_cycle{};
    malformed_ready_cycle.normal_pending = 1;
    malformed_ready_cycle.normal = {
        NormalIntentKind::PublishTelemetry, 0x01, 215, 17, 0};
    const RuntimeOwnerTaskCycleResult malformed_ready_result =
        process_cycle(success_core, malformed_ready_cycle);
    CHECK(malformed_ready_result.disposition ==
          RuntimeOwnerTaskCycleDisposition::Processed);
    CHECK(malformed_ready_result.selected_work ==
          RuntimeOwnerTaskWorkKind::NormalIntent);
    CHECK(malformed_ready_result.normal_result ==
          NormalSubmitResult::RejectedInvalid);
    CHECK(malformed_ready_result.urgent_recheck_required == 1);
    CHECK(!RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(success_core));
    CHECK(success_core.redacted_status().runtime_ready == 1);
    RuntimeOwnerRtosOwnerLoop ready_owner_loop{success_core};
    CHECK(ready_owner_loop.consume_normal(malformed_ready_cycle.normal) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(!RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(success_core));
    CHECK(success_core.redacted_status().runtime_ready == 1);
    CHECK(success_executor.publish_runtime(make_runtime_snapshot()) ==
          RuntimeOwnerExecutorResult::Accepted);

    RuntimeOwnerTaskCycleInput normal_cycle{};
    normal_cycle.normal_pending = 1;
    normal_cycle.normal = {
        NormalIntentKind::PublishTelemetry, 0x01, 215, 17, 1};
    CHECK(process_cycle(success_core, normal_cycle).normal_result ==
          NormalSubmitResult::Accepted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(success_core));
    const RuntimeOwnerExecutorCommand normal = require_command(
        success_executor,
        RuntimeOwnerDeviceOperationKind::PublishTelemetry);
    CHECK(success_executor.acknowledge_command(normal) ==
          RuntimeOwnerExecutorResult::Accepted);

    const RuntimeOwnerRedactedStatus ready_before_late_duplicate =
        success_core.redacted_status();
    RuntimeOwnerExecutorCommand duplicate_wrong_policy = end_boot;
    duplicate_wrong_policy.completion_policy = CompletionPolicy::TrustedReceipt;
    CHECK(success_executor.commit_end_boot_delivery(end_boot) ==
          RuntimeOwnerExecutorResult::AcceptedDuplicate);
    CHECK(success_executor.commit_end_boot_delivery(
              duplicate_wrong_correlation) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    CHECK(success_executor.commit_end_boot_delivery(
              duplicate_wrong_sequence) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    CHECK(success_executor.commit_end_boot_delivery(duplicate_wrong_token) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    CHECK(success_executor.commit_end_boot_delivery(duplicate_wrong_policy) ==
          RuntimeOwnerExecutorResult::RejectedWrongCommand);
    const RuntimeOwnerRedactedStatus ready_after_late_duplicate =
        success_core.redacted_status();
    CHECK(ready_after_late_duplicate.state ==
          ready_before_late_duplicate.state);
    CHECK(ready_after_late_duplicate.phase ==
          ready_before_late_duplicate.phase);
    CHECK(ready_after_late_duplicate.runtime_ready ==
          ready_before_late_duplicate.runtime_ready);
    CHECK(ready_after_late_duplicate.shutdown_latched ==
          ready_before_late_duplicate.shutdown_latched);
    CHECK(ready_after_late_duplicate.normal_cancelled_count ==
          ready_before_late_duplicate.normal_cancelled_count);
    CHECK(ready_after_late_duplicate.effect_cancelled_count ==
          ready_before_late_duplicate.effect_cancelled_count);

    CHECK(success_executor.normal_succeeded(normal) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(success_core);
    const RuntimeOwnerRedactedStatus ready_after_normal_completion =
        success_core.redacted_status();
    CHECK(success_executor.commit_end_boot_delivery(end_boot) ==
          RuntimeOwnerExecutorResult::AcceptedDuplicate);
    const RuntimeOwnerRedactedStatus ready_after_completed_duplicate =
        success_core.redacted_status();
    CHECK(ready_after_completed_duplicate.state ==
          ready_after_normal_completion.state);
    CHECK(ready_after_completed_duplicate.phase ==
          ready_after_normal_completion.phase);
    CHECK(ready_after_completed_duplicate.runtime_ready ==
          ready_after_normal_completion.runtime_ready);

    RuntimeOwnerTaskCycleInput timeout_cycle{};
    timeout_cycle.normal_pending = 1;
    timeout_cycle.normal = {NormalIntentKind::RefreshRssi, 0, 0, 0, 0};
    CHECK(process_cycle(success_core, timeout_cycle).normal_result ==
          NormalSubmitResult::Accepted);
    const RuntimeOwnerExecutorCommand timeout = require_command(
        success_executor, RuntimeOwnerDeviceOperationKind::RefreshRssi);
    CHECK(success_executor.acknowledge_command(timeout) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(success_executor.normal_timed_out(timeout, 0) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(success_core);

    RuntimeOwnerTaskCycleInput cancelled_cycle{};
    cancelled_cycle.normal_pending = 1;
    cancelled_cycle.normal = {NormalIntentKind::PullCommand, 0, 0, 0, 0};
    CHECK(process_cycle(success_core, cancelled_cycle).normal_result ==
          NormalSubmitResult::Accepted);
    const RuntimeOwnerExecutorCommand cancelled = require_command(
        success_executor, RuntimeOwnerDeviceOperationKind::PullCommand);
    CHECK(success_executor.acknowledge_command(cancelled) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(success_executor.normal_cancelled(cancelled, 0) ==
          RuntimeOwnerExecutorResult::Accepted);
    step_expect_processed(success_core);

    RuntimeOwnerTaskCycleInput inflight_cycle{};
    inflight_cycle.normal_pending = 1;
    inflight_cycle.normal = {NormalIntentKind::PullConfig, 0, 0, 0, 0};
    CHECK(process_cycle(success_core, inflight_cycle).normal_result ==
          NormalSubmitResult::Accepted);
    const RuntimeOwnerExecutorCommand shutdown_inflight = require_command(
        success_executor, RuntimeOwnerDeviceOperationKind::PullConfig);
    CHECK(success_executor.acknowledge_command(shutdown_inflight) ==
          RuntimeOwnerExecutorResult::Accepted);
    const std::uint32_t cancelled_before_shutdown =
        success_core.redacted_status().normal_cancelled_count;

    CHECK(success_power_button.request(0, 99) ==
          RuntimeOwnerShutdownRequestResult::RejectedInvalid);
    CHECK(success_power_button.request(2, 99) ==
          RuntimeOwnerShutdownRequestResult::Accepted);
    CHECK(success_power_button.request(2, 99) ==
          RuntimeOwnerShutdownRequestResult::AcceptedDuplicate);
    CHECK(RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(
        success_core));
    CHECK(success_power_button.request(1, 99) ==
          RuntimeOwnerShutdownRequestResult::RejectedStale);
    CHECK(RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(
        success_core));
    const RuntimeOwnerPhase phase_before_late =
        success_core.redacted_status().phase;
    CHECK(success_executor.normal_succeeded(shutdown_inflight) ==
          RuntimeOwnerExecutorResult::RejectedTerminalDropped);
    CHECK(success_executor.transport_disconnected(
              end_boot.source.effect.attempt.mqtt_session_id,
              end_boot.source.effect.attempt.mqtt_generation,
              0) == RuntimeOwnerExecutorResult::RejectedTerminalDropped);
    CHECK(success_core.redacted_status().phase == phase_before_late);
    CHECK(success_core.redacted_status().shutdown_latched == 1);

    RuntimeOwnerTaskCycleInput blocked{};
    blocked.transport_pending = 1;
    blocked.normal_pending = 1;
    blocked.normal = normal_cycle.normal;
    const RuntimeOwnerTaskCycleResult shutdown_cycle =
        process_cycle(success_core, blocked);
    CHECK(shutdown_cycle.selected_work == RuntimeOwnerTaskWorkKind::None);
    CHECK(success_core.redacted_status().state ==
          RuntimeOwnerTaskState::Terminal);
    CHECK(success_core.redacted_status().normal_cancelled_count ==
          cancelled_before_shutdown + 1);
    CHECK(RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(
        success_core));
    const RuntimeOwnerRedactedStatus terminal_before_exact_late =
        success_core.redacted_status();
    CHECK(success_executor.normal_succeeded(shutdown_inflight) ==
          RuntimeOwnerExecutorResult::RejectedTerminalDropped);
    const RuntimeOwnerRedactedStatus terminal_after_exact_late =
        success_core.redacted_status();
    CHECK(terminal_after_exact_late.phase ==
          terminal_before_exact_late.phase);
    CHECK(terminal_after_exact_late.runtime_ready ==
          terminal_before_exact_late.runtime_ready);
    CHECK(terminal_after_exact_late.normal_cancelled_count ==
          terminal_before_exact_late.normal_cancelled_count);
    CHECK(terminal_after_exact_late.effect_cancelled_count ==
          terminal_before_exact_late.effect_cancelled_count);
    CHECK(success_executor.liveness_succeeded(success_freeze) ==
          RuntimeOwnerExecutorResult::RejectedTerminalDropped);

    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
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
    test_capabilities_are_not_forgeable();
    test_activation_identity_contract();
    test_dormant_capability_operations_are_closed();
    test_deadline_and_disconnect_typed_bridges_use_canonical_context();
    test_three_shutdown_sources_have_independent_exact_provenance();
    test_composed_cutover_flow_is_allocation_free();

    if (g_failure_count != 0) {
        std::fprintf(
            stderr,
            "RUNTIME_OWNER_CUTOVER_INTEGRATION_TEST FAIL checks=%zu failures=%zu\n",
            g_check_count,
            g_failure_count);
        return 1;
    }
    std::printf(
        "RUNTIME_OWNER_CUTOVER_INTEGRATION_TEST PASS checks=%zu\n",
        g_check_count);
    return 0;
}
