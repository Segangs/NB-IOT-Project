#include "runtime_owner_adapter_core.hpp"
#include "runtime_owner_core.hpp"
#include "runtime_snapshot_core.hpp"
#include "runtime_owner_task_core.hpp"

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(const bool condition, const char *expression, const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, line,
                     expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

boot_v2::RuntimeOwnerInput input(
    const boot_v2::RuntimeOwnerInputKind kind,
    const boot_v2::RuntimeOwnerEffectKind receipt_kind,
    const std::uint32_t correlation_id,
    const std::uint32_t mqtt_session_id,
    const std::uint32_t mqtt_generation,
    const std::uint32_t config_commit_sequence,
    const std::uint32_t config_apply_epoch) noexcept
{
    return {kind,
            receipt_kind,
            correlation_id,
            mqtt_session_id,
            mqtt_generation,
            config_commit_sequence,
            config_apply_epoch};
}

std::string read_file(const char *path)
{
    std::ifstream source(path);
    return {std::istreambuf_iterator<char>(source),
            std::istreambuf_iterator<char>()};
}

boot_v2::SensorQualitySnapshotV1 sensor(const std::int16_t deci_celsius)
{
    boot_v2::SensorQualitySnapshotV1 value{};
    value.health = boot_v2::SnapshotHealth::Pass;
    value.has_value = 1;
    value.value_source = boot_v2::SensorValueSource::Fresh;
    value.value_deci_celsius = deci_celsius;
    return value;
}

void test_config_handoff_skips_post_config_commands() noexcept
{
    using namespace boot_v2;

    RuntimeOwnerCore core{};
    RuntimeOwnerTransition transition = core.submit(input(
        RuntimeOwnerInputKind::BeginTransportAttempt,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        0,
        0,
        0));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.effect_count == 1);
    CHECK(transition.effects[0].kind ==
          RuntimeOwnerEffectKind::StartTransportAttempt);

    const std::uint32_t generation = core.view().mqtt_generation_counter;
    transition = core.submit(input(
        RuntimeOwnerInputKind::TransportEstablished,
        RuntimeOwnerEffectKind::None,
        0,
        1,
        generation,
        0,
        0));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(core.view().phase == RuntimeOwnerPhase::AwaitingConfigCommit);

    transition = core.submit(input(
        RuntimeOwnerInputKind::ConfigActivationCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        1,
        generation,
        1,
        0));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.phase_after == RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(transition.effect_count == 1);
    CHECK(transition.effects[0].kind ==
          RuntimeOwnerEffectKind::FreezeBootSnapshot);
    CHECK(transition.effects[0].attempt == core.view().active_attempt);
    CHECK(core.view().active_attempt.mqtt_session_id == 1);
    CHECK(core.view().active_attempt.mqtt_generation == generation);
    CHECK(core.view().active_attempt.config_apply_epoch == 1);
    CHECK(transition.effects[0].kind != RuntimeOwnerEffectKind::StartAtProbe);
    CHECK(transition.effects[0].kind !=
          RuntimeOwnerEffectKind::StartProbePublish);
    CHECK(transition.effects[0].kind !=
          RuntimeOwnerEffectKind::VerifySubscription);
    CHECK(transition.effects[0].kind !=
          RuntimeOwnerEffectKind::PullFollowupConfig);

    const RuntimeOwnerEffect freeze = transition.effects[0];
    transition = core.submit(input(
        RuntimeOwnerInputKind::SnapshotFreezeSucceeded,
        RuntimeOwnerEffectKind::FreezeBootSnapshot,
        freeze.correlation_id,
        freeze.attempt.mqtt_session_id,
        freeze.attempt.mqtt_generation,
        0,
        freeze.attempt.config_apply_epoch));
    CHECK(transition.disposition == RuntimeOwnerDisposition::Accepted);
    CHECK(transition.phase_after == RuntimeOwnerPhase::RuntimeReady);
    CHECK(transition.effect_count == 1);
    CHECK(transition.effects[0].kind ==
          RuntimeOwnerEffectKind::EndBootOrchestration);
}

void test_handoff_snapshot_does_not_claim_liveness() noexcept
{
    using namespace boot_v2;

    BootRuntimeSnapshotV1 snapshot{};
    snapshot.health = SnapshotHealth::Pass;
    snapshot.last_completed_stage = BootCompletedStage::ConfigAppliedHandoff;
    snapshot.config_valid = 1;
    snapshot.transport_ready = 1;
    snapshot.subscription_alive = 1;
    snapshot.post_config_liveness = 0;
    snapshot.hardware_revision = 1;
    snapshot.firmware_build_id = 20260722;
    snapshot.config_version = 1;
    snapshot.sensors = {sensor(215), sensor(218)};
    snapshot.pdp_session_id = 1;
    snapshot.mqtt_session_id = 1;
    snapshot.mqtt_generation = 1;
    snapshot.config_apply_epoch = 1;

    CHECK(boot_runtime_snapshot_is_canonical(snapshot));
    snapshot.post_config_liveness = 1;
    CHECK(!boot_runtime_snapshot_is_canonical(snapshot));
}

void test_adapter_queues_only_the_direct_handoff_effect() noexcept
{
    using namespace boot_v2;

    RuntimeOwnerAdapterCore adapter{};
    CHECK(adapter.request_transport_attempt() == OwnerRequestResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch transport = adapter.peek_dispatch();
    CHECK(transport.effect.kind == RuntimeOwnerEffectKind::StartTransportAttempt);
    CHECK(adapter.acknowledge_dispatch(transport.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);

    auto receipts = adapter.trusted_receipt_port();
    CHECK(receipts.submit({
              TrustedReceiptKind::TransportEstablished,
              RuntimeOwnerEffectKind::StartTransportAttempt,
              0,
              transport.effect.correlation_id,
              7,
              1,
              0,
              0,
              0,
          }) == TrustedIngressResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().core.phase == RuntimeOwnerPhase::AwaitingConfigCommit);
    const std::uint32_t last_dispatch_before_config =
        adapter.view().last_dispatch_sequence;
    const TrustedReceipt receipt{
        TrustedReceiptKind::ConfigCommitted,
        RuntimeOwnerEffectKind::None,
        0,
        0,
        7,
        1,
        1,
        0,
        0,
    };
    CHECK(receipts.submit(receipt) == TrustedIngressResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::CoreTransitionApplied);

    const RuntimeOwnerAdapterView after = adapter.view();
    CHECK(after.core.phase == RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(after.pending_effect_count == 1);
    CHECK(after.last_dispatch_sequence == last_dispatch_before_config + 1);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    CHECK(adapter.peek_dispatch().effect.kind ==
          RuntimeOwnerEffectKind::FreezeBootSnapshot);
}

void test_task_core_reaches_ready_after_direct_handoff() noexcept
{
    using namespace boot_v2;

    RuntimeOwnerTaskCore core{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_activate(core, 0x71u) ==
          RuntimeOwnerTaskActivationResult::Activated);
    auto executor = RuntimeOwnerTaskCoreTestPeer::executor_port(core);

    RuntimeOwnerTaskCycleInput transport_cycle{};
    transport_cycle.transport_pending = 1;
    CHECK(RuntimeOwnerTaskCoreTestPeer::process_cycle(core, transport_cycle)
              .transport_result == OwnerRequestResult::Accepted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {})
              .step_result.action == AdapterStepAction::DispatchPrepared);

    RuntimeOwnerExecutorCommand transport{};
    CHECK(executor.peek_command(transport) == RuntimeOwnerExecutorResult::Accepted);
    CHECK(transport.kind == RuntimeOwnerDeviceOperationKind::OpenTransport);
    CHECK(executor.acknowledge_command(transport) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(executor.transport_established(transport, 7) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {})
              .step_result.phase_after ==
          RuntimeOwnerPhase::AwaitingConfigCommit);

    CHECK(executor.config_committed(1) == RuntimeOwnerExecutorResult::Accepted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {})
              .step_result.phase_after ==
          RuntimeOwnerPhase::SnapshotFreezePending);
    CHECK(RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {})
              .step_result.action == AdapterStepAction::DispatchPrepared);

    RuntimeOwnerExecutorCommand freeze{};
    CHECK(executor.peek_command(freeze) == RuntimeOwnerExecutorResult::Accepted);
    CHECK(freeze.kind == RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot);
    CHECK(executor.acknowledge_command(freeze) ==
          RuntimeOwnerExecutorResult::Accepted);

    BootRuntimeSnapshotV1 snapshot{};
    snapshot.health = SnapshotHealth::Pass;
    snapshot.last_completed_stage = BootCompletedStage::ConfigAppliedHandoff;
    snapshot.config_valid = 1;
    snapshot.transport_ready = 1;
    snapshot.subscription_alive = 1;
    snapshot.post_config_liveness = 0;
    snapshot.hardware_revision = 1;
    snapshot.firmware_build_id = 20260722;
    snapshot.config_version = 1;
    snapshot.sensors = {sensor(215), sensor(218)};
    snapshot.pdp_session_id = 1;
    snapshot.mqtt_session_id = freeze.source.effect.attempt.mqtt_session_id;
    snapshot.mqtt_generation = freeze.source.effect.attempt.mqtt_generation;
    snapshot.config_apply_epoch =
        freeze.source.effect.attempt.config_apply_epoch;
    CHECK(executor.snapshot_succeeded(freeze, snapshot) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {})
              .step_result.phase_after == RuntimeOwnerPhase::RuntimeReady);
    CHECK(RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {})
              .step_result.action == AdapterStepAction::DispatchPrepared);

    RuntimeOwnerExecutorCommand end_boot{};
    CHECK(executor.peek_command(end_boot) == RuntimeOwnerExecutorResult::Accepted);
    CHECK(end_boot.kind == RuntimeOwnerDeviceOperationKind::EndBootOrchestration);
    CHECK(executor.commit_end_boot_delivery(end_boot) ==
          RuntimeOwnerExecutorResult::Accepted);
    CHECK(core.redacted_status().runtime_ready == 1);
}

void test_periodic_first_publish_contract()
{
    const std::string periodic = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_periodic_modem.cpp");
    const std::string backend = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string root = read_file(NB_IOT_SOURCE_ROOT "/CMakeLists.txt");
    CHECK(!periodic.empty());
    CHECK(!backend.empty());
    CHECK(!root.empty());
    CHECK(periodic.find("kPostConfigFirstTelemetryDelayMs = 30000") !=
          std::string::npos);
    CHECK(periodic.find("PERIODIC_FIRST_TELEMETRY") != std::string::npos);
    CHECK(periodic.find("runtime_owner_periodic_publish_telemetry(\n                1, telemetry_revision)") != std::string::npos);
    CHECK(backend.find("ConfigAppliedHandoff") != std::string::npos);
    CHECK(backend.find("snapshot.post_config_liveness = 0") !=
          std::string::npos);
    CHECK(root.find("NB_IOT_POST_CONFIG_HANDOFF_TRIAL=1") !=
          std::string::npos);
}

} // namespace

int main()
{
    test_config_handoff_skips_post_config_commands();
    test_handoff_snapshot_does_not_claim_liveness();
    test_adapter_queues_only_the_direct_handoff_effect();
    test_task_core_reaches_ready_after_direct_handoff();
    test_periodic_first_publish_contract();

    if (g_failures != 0) {
        std::fprintf(stderr,
                     "runtime_owner_post_config_handoff_test: %zu / %zu checks failed\n",
                     g_failures,
                     g_checks);
        return 1;
    }
    std::printf("runtime_owner_post_config_handoff_test: %zu checks passed\n",
                g_checks);
    return 0;
}
