#include "runtime_owner_rtos_owner_loop.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace boot_v2;

std::size_t checks = 0;
std::size_t failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                         \
                         __FILE__, __LINE__, #condition);                      \
        }                                                                      \
    } while (false)

constexpr RuntimeOwnerUrgentMessage urgent_message(
    const RuntimeOwnerUrgentSource source,
    const RuntimeOwnerShutdownIntent intent,
    const std::uint32_t sequence,
    const std::uint32_t correlation) noexcept
{
    return {source, intent, {}, sequence, correlation};
}

constexpr NormalIntent valid_normal() noexcept
{
    return {NormalIntentKind::PublishTelemetry, 0x01, -166, 41, 9};
}

constexpr NormalIntent invalid_normal() noexcept
{
    return {NormalIntentKind::PublishTelemetry, 0x01, -166, 41, 0};
}

void test_power_mapping()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerRtosOwnerLoop loop{core};
    CHECK(loop.consume_urgent(urgent_message(
              RuntimeOwnerUrgentSource::PowerButton,
              RuntimeOwnerShutdownIntent::AutomaticByUsb,
              11,
              101)) ==
          RuntimeOwnerDrainConsumeResult::Processed);
    CHECK(RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(core)
              .request(11, 101) ==
          RuntimeOwnerShutdownRequestResult::AcceptedDuplicate);
    CHECK(RuntimeOwnerTaskCoreTestPeer::adapter_loss_shutdown_port(core)
              .request(11, 101) ==
          RuntimeOwnerShutdownRequestResult::RejectedTerminal);
    CHECK(RuntimeOwnerTaskCoreTestPeer::authenticated_command_shutdown_port(core)
              .request(11, 101) ==
          RuntimeOwnerShutdownRequestResult::RejectedTerminal);
}

void test_adapter_loss_mapping()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerRtosOwnerLoop loop{core};
    CHECK(loop.consume_urgent(urgent_message(
              RuntimeOwnerUrgentSource::AdapterLossCommitted,
              RuntimeOwnerShutdownIntent::AutomaticByUsb,
              12,
              102)) ==
          RuntimeOwnerDrainConsumeResult::Processed);
    CHECK(RuntimeOwnerTaskCoreTestPeer::adapter_loss_shutdown_port(core)
              .request(12, 102) ==
          RuntimeOwnerShutdownRequestResult::AcceptedDuplicate);
    CHECK(RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(core)
              .request(12, 102) ==
          RuntimeOwnerShutdownRequestResult::RejectedTerminal);
    CHECK(RuntimeOwnerTaskCoreTestPeer::authenticated_command_shutdown_port(core)
              .request(12, 102) ==
          RuntimeOwnerShutdownRequestResult::RejectedTerminal);
}

void test_authenticated_mapping()
{
    constexpr RuntimeOwnerShutdownIntent intents[] = {
        RuntimeOwnerShutdownIntent::Reboot,
        RuntimeOwnerShutdownIntent::PowerOff,
    };
    for (const RuntimeOwnerShutdownIntent intent : intents) {
        RuntimeOwnerTaskCore core{};
        RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
        RuntimeOwnerRtosOwnerLoop loop{core};
        const RuntimeOwnerUrgentMessage accepted = urgent_message(
            RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand,
            intent,
            13,
            103);
        CHECK(loop.consume_urgent(accepted) ==
              RuntimeOwnerDrainConsumeResult::Processed);
        CHECK(RuntimeOwnerTaskCoreTestPeer::authenticated_command_shutdown_port(
                  core).request(13, 103) ==
              RuntimeOwnerShutdownRequestResult::AcceptedDuplicate);
        RuntimeOwnerUrgentMessage copied{};
        CHECK(loop.copy_shutdown_context(copied));
        CHECK(copied.intent == intent);
        CHECK(RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(core)
                  .request(13, 103) ==
              RuntimeOwnerShutdownRequestResult::RejectedTerminal);
        CHECK(RuntimeOwnerTaskCoreTestPeer::adapter_loss_shutdown_port(core)
                  .request(13, 103) ==
              RuntimeOwnerShutdownRequestResult::RejectedTerminal);
    }
}

void test_only_accepted_shutdown_context_is_preserved()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerRtosOwnerLoop loop{core};
    RuntimeOwnerUrgentMessage copied{};
    CHECK(!loop.copy_shutdown_context(copied));

    const RuntimeOwnerUrgentMessage accepted = urgent_message(
        RuntimeOwnerUrgentSource::PowerButton,
        RuntimeOwnerShutdownIntent::AutomaticByUsb,
        21,
        201);
    CHECK(loop.consume_urgent(accepted) ==
          RuntimeOwnerDrainConsumeResult::Processed);
    CHECK(loop.copy_shutdown_context(copied));
    CHECK(copied.source == accepted.source);
    CHECK(copied.intent == accepted.intent);
    CHECK(copied.producer_sequence == accepted.producer_sequence);
    CHECK(copied.incident_correlation_id ==
          accepted.incident_correlation_id);

    const RuntimeOwnerUrgentMessage rejected_terminal = urgent_message(
        RuntimeOwnerUrgentSource::AdapterLossCommitted,
        RuntimeOwnerShutdownIntent::AutomaticByUsb,
        22,
        202);
    CHECK(loop.consume_urgent(rejected_terminal) ==
          RuntimeOwnerDrainConsumeResult::Processed);
    CHECK(loop.copy_shutdown_context(copied));
    CHECK(copied.source == accepted.source);
    CHECK(copied.producer_sequence == accepted.producer_sequence);
    CHECK(copied.incident_correlation_id ==
          accepted.incident_correlation_id);

    CHECK(loop.consume_urgent(accepted) ==
          RuntimeOwnerDrainConsumeResult::Processed);
    CHECK(loop.copy_shutdown_context(copied));
    CHECK(copied.source == accepted.source);
    CHECK(copied.producer_sequence == accepted.producer_sequence);
    CHECK(copied.incident_correlation_id ==
          accepted.incident_correlation_id);
}

void test_invalid_urgent_does_not_reach_a_port_or_cycle()
{
    const RuntimeOwnerUrgentMessage message = urgent_message(
        RuntimeOwnerUrgentSource::Invalid,
        RuntimeOwnerShutdownIntent::AutomaticByUsb,
        14,
        104);

    RuntimeOwnerTaskCore cycle_core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(cycle_core);
    RuntimeOwnerRtosOwnerLoop cycle_loop{cycle_core};
    auto pending =
        RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(cycle_core);
    CHECK(pending.request(9, 99) ==
          RuntimeOwnerShutdownRequestResult::Accepted);
    CHECK(cycle_loop.consume_urgent(message) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(cycle_core) ==
          RuntimeOwnerTaskState::Active);
    CHECK(pending.request(9, 99) ==
          RuntimeOwnerShutdownRequestResult::AcceptedDuplicate);

    RuntimeOwnerTaskCore power_port_core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(power_port_core);
    RuntimeOwnerRtosOwnerLoop power_port_loop{power_port_core};
    CHECK(power_port_loop.consume_urgent(message) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(power_port_core) ==
          RuntimeOwnerTaskState::Active);
    CHECK(RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(
              power_port_core).request(14, 104) ==
          RuntimeOwnerShutdownRequestResult::Accepted);

    RuntimeOwnerTaskCore adapter_loss_port_core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(adapter_loss_port_core);
    RuntimeOwnerRtosOwnerLoop adapter_loss_port_loop{adapter_loss_port_core};
    CHECK(adapter_loss_port_loop.consume_urgent(message) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(adapter_loss_port_core) ==
          RuntimeOwnerTaskState::Active);
    CHECK(RuntimeOwnerTaskCoreTestPeer::adapter_loss_shutdown_port(
              adapter_loss_port_core).request(14, 104) ==
          RuntimeOwnerShutdownRequestResult::Accepted);

    RuntimeOwnerTaskCore authenticated_port_core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(authenticated_port_core);
    RuntimeOwnerRtosOwnerLoop authenticated_port_loop{
        authenticated_port_core};
    CHECK(authenticated_port_loop.consume_urgent(message) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(authenticated_port_core) ==
          RuntimeOwnerTaskState::Active);
    CHECK(RuntimeOwnerTaskCoreTestPeer::authenticated_command_shutdown_port(
              authenticated_port_core).request(14, 104) ==
          RuntimeOwnerShutdownRequestResult::Accepted);
}

void test_normal_classification_is_state_independent()
{
    RuntimeOwnerTaskCore dormant{};
    RuntimeOwnerRtosOwnerLoop dormant_loop{dormant};
    CHECK(dormant_loop.consume_normal(invalid_normal()) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(dormant_loop.consume_normal(valid_normal()) ==
          RuntimeOwnerDrainConsumeResult::Processed);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(dormant) ==
          RuntimeOwnerTaskState::Dormant);

    RuntimeOwnerTaskCore active{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(active);
    RuntimeOwnerRtosOwnerLoop active_loop{active};
    CHECK(active_loop.consume_normal(invalid_normal()) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(!RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(active));
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(active) ==
          RuntimeOwnerTaskState::Active);

    RuntimeOwnerTaskCore terminal{};
    RuntimeOwnerTaskCoreTestPeer::fixture_terminal(terminal);
    RuntimeOwnerRtosOwnerLoop terminal_loop{terminal};
    CHECK(terminal_loop.consume_normal(invalid_normal()) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(terminal) ==
          RuntimeOwnerTaskState::Terminal);
}

void test_control_and_source_shape()
{
    RuntimeOwnerTaskCore valid_core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(valid_core);
    RuntimeOwnerRtosOwnerLoop valid_loop{valid_core};
    CHECK(valid_loop.consume_control(
              {RuntimeOwnerControlKind::RequestTransportAttempt, {}}) ==
          RuntimeOwnerDrainConsumeResult::Processed);

    RuntimeOwnerTaskCore invalid_core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(invalid_core);
    RuntimeOwnerRtosOwnerLoop invalid_loop{invalid_core};
    auto pending =
        RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(invalid_core);
    CHECK(pending.request(8, 88) ==
          RuntimeOwnerShutdownRequestResult::Accepted);
    CHECK(invalid_loop.consume_control(
              {RuntimeOwnerControlKind::Invalid, {}}) ==
          RuntimeOwnerDrainConsumeResult::DroppedInvalid);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(invalid_core) ==
          RuntimeOwnerTaskState::Active);

    std::ifstream stream(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos_owner_loop.hpp");
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    std::size_t calls = 0;
    std::size_t offset = 0;
    const std::string needle{"core_.process_cycle("};
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++calls;
        offset += needle.size();
    }
    CHECK(stream.is_open());
    CHECK(calls == 4);
    CHECK(source.find("FreeRTOS.h") == std::string::npos);
    CHECK(source.find("QueueHandle_t") == std::string::npos);
    CHECK(source.find("TaskHandle_t") == std::string::npos);
}

void test_alarm_delivery_pop_forwards_exact_event()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerRtosOwnerLoop loop{core};
    const TemperatureAlarmDeliveryEvent expected{
        2,
        71,
        TemperatureAlarmTerminalResult::Failed,
        TemperatureAlarmEdge::Clear,
        0,
    };
    CHECK(RuntimeOwnerTaskCoreTestPeer::fixture_push_alarm_delivery(
              core, expected) ==
          TemperatureAlarmDeliveryPushResult::Accepted);

    TemperatureAlarmDeliveryEvent delivered{};
    CHECK(loop.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Popped);
    CHECK(delivered.sensor_id == expected.sensor_id);
    CHECK(delivered.snapshot_revision == expected.snapshot_revision);
    CHECK(delivered.result == expected.result);
    CHECK(delivered.edge == expected.edge);
    CHECK(delivered.reserved == 0);
    CHECK(loop.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);
}

void test_consumer_pop_wakes_owner_for_full_retry()
{
    std::ifstream stream(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    const std::size_t wrapper = source.find(
        "runtime_owner_try_receive_temperature_alarm_delivery(");
    const std::size_t pop = source.find(
        "g_owner_loop.try_pop_alarm_delivery", wrapper);
    const std::size_t popped = source.find(
        "TemperatureAlarmDeliveryPopResult::Popped", pop);
    const std::size_t notify =
        source.find("xTaskNotifyGive(task_handle)", popped);
    const std::size_t wrapper_end = source.find("\n}\n", wrapper);
    const std::size_t awaiting =
        source.find("AdapterStepAction::AwaitingTrustedReceipt");
    CHECK(stream.is_open());
    CHECK(wrapper != std::string::npos);
    CHECK(pop != std::string::npos);
    CHECK(popped != std::string::npos);
    CHECK(notify != std::string::npos);
    CHECK(wrapper_end != std::string::npos);
    CHECK(wrapper < pop);
    CHECK(pop < popped);
    CHECK(popped < notify);
    CHECK(notify < wrapper_end);
    CHECK(awaiting != std::string::npos);
}

void test_transport_config_receipt_is_deferred_until_phase_advance()
{
    struct Backend {
        std::uint32_t pending{0};

        RuntimeOwnerPhysicalResult execute(
            const RuntimeOwnerExecutorCommand command) noexcept
        {
            RuntimeOwnerPhysicalResult result{};
            result.kind = RuntimeOwnerPhysicalResultKind::Succeeded;
            if (command.kind ==
                RuntimeOwnerDeviceOperationKind::OpenTransport) {
                result.mqtt_session_id = 31;
                result.config_commit_sequence = 37;
            }
            return result;
        }

        void defer_config_commit(const std::uint32_t sequence) noexcept
        {
            pending = sequence;
        }

        std::uint32_t pending_config_commit_sequence() const noexcept
        {
            return pending;
        }

        void clear_pending_config_commit() noexcept
        {
            pending = 0;
        }
    } backend{};

    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerRtosOwnerLoop loop{core};
    CHECK(loop.consume_control(
              {RuntimeOwnerControlKind::RequestTransportAttempt, {}}) ==
          RuntimeOwnerDrainConsumeResult::Processed);
    for (std::uint32_t attempt = 0;
         attempt < 4 &&
         RuntimeOwnerTaskCoreTestPeer::adapter_view(core)
                 .current_dispatch.kind == AdapterDispatchKind::None;
         ++attempt) {
        (void)loop.advance();
    }
    CHECK(loop.execute_one(backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(backend.pending == 37);
    (void)loop.advance();
    CHECK(RuntimeOwnerTaskCoreTestPeer::adapter_view(core).core.phase ==
          RuntimeOwnerPhase::AwaitingConfigCommit);
    CHECK(loop.submit_deferred_config(backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(backend.pending == 0);
    (void)loop.advance();
    CHECK(RuntimeOwnerTaskCoreTestPeer::adapter_view(core).core.phase ==
          RuntimeOwnerPhase::LivenessWaiting);
}

} // namespace

int main()
{
    test_power_mapping();
    test_adapter_loss_mapping();
    test_authenticated_mapping();
    test_only_accepted_shutdown_context_is_preserved();
    test_invalid_urgent_does_not_reach_a_port_or_cycle();
    test_normal_classification_is_state_independent();
    test_control_and_source_shape();
    test_alarm_delivery_pop_forwards_exact_event();
    test_consumer_pop_wakes_owner_for_full_retry();
    test_transport_config_receipt_is_deferred_until_phase_advance();
    if (failures != 0) {
        std::printf("RUNTIME_OWNER_RTOS_OWNER_LOOP_TEST FAIL checks=%zu failures=%zu\n",
                    checks, failures);
        return 1;
    }
    std::printf("RUNTIME_OWNER_RTOS_OWNER_LOOP_TEST PASS checks=%zu\n", checks);
    return 0;
}
