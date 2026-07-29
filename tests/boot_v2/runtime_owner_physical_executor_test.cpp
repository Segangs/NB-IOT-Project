#include "runtime_owner_physical_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

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

using namespace boot_v2;

struct FakeBackend {
    RuntimeOwnerPhysicalResult next{};
    RuntimeOwnerExecutorCommand observed{};
    std::uint32_t calls{0};
    std::uint32_t deferred_config_sequence{0};

    RuntimeOwnerPhysicalResult execute(
        const RuntimeOwnerExecutorCommand command) noexcept
    {
        observed = command;
        ++calls;
        return next;
    }

    void defer_config_commit(const std::uint32_t sequence) noexcept
    {
        deferred_config_sequence = sequence;
    }

    std::uint32_t pending_config_commit_sequence() const noexcept
    {
        return deferred_config_sequence;
    }

    void clear_pending_config_commit() noexcept
    {
        deferred_config_sequence = 0;
    }
};

struct FakePort {
    RuntimeOwnerExecutorCommand command{};
    RuntimeOwnerExecutorResult peek_result{RuntimeOwnerExecutorResult::Accepted};
    RuntimeOwnerExecutorResult ack_result{RuntimeOwnerExecutorResult::Accepted};
    RuntimeOwnerExecutorResult completion_result{
        RuntimeOwnerExecutorResult::Accepted};
    std::uint32_t ack_calls{0};
    std::uint32_t transport_established_calls{0};
    std::uint32_t transport_failed_calls{0};
    std::uint32_t config_committed_calls{0};
    std::uint32_t liveness_succeeded_calls{0};
    std::uint32_t liveness_failed_calls{0};
    std::uint32_t liveness_deadline_calls{0};
    std::uint32_t snapshot_succeeded_calls{0};
    std::uint32_t snapshot_failed_calls{0};
    std::uint32_t end_boot_calls{0};
    std::uint32_t normal_succeeded_calls{0};
    std::uint32_t normal_failed_calls{0};
    std::uint32_t normal_timed_out_calls{0};
    std::uint32_t normal_cancelled_calls{0};
    std::uint32_t observed_session{0};
    std::uint32_t observed_config_sequence{0};
    std::uint32_t observed_diagnostic{0};

    RuntimeOwnerExecutorResult peek_command(
        RuntimeOwnerExecutorCommand &output) noexcept
    {
        output = command;
        return peek_result;
    }

    RuntimeOwnerExecutorResult acknowledge_command(
        RuntimeOwnerExecutorCommand) noexcept
    {
        ++ack_calls;
        return ack_result;
    }

    RuntimeOwnerExecutorResult transport_established(
        RuntimeOwnerExecutorCommand, const std::uint32_t session) noexcept
    {
        ++transport_established_calls;
        observed_session = session;
        return completion_result;
    }

    RuntimeOwnerExecutorResult transport_failed(
        RuntimeOwnerExecutorCommand, const std::uint32_t diagnostic) noexcept
    {
        ++transport_failed_calls;
        observed_diagnostic = diagnostic;
        return completion_result;
    }

    RuntimeOwnerExecutorResult config_committed(
        const std::uint32_t sequence) noexcept
    {
        ++config_committed_calls;
        observed_config_sequence = sequence;
        return completion_result;
    }

    RuntimeOwnerExecutorResult liveness_succeeded(
        RuntimeOwnerExecutorCommand) noexcept
    {
        ++liveness_succeeded_calls;
        return completion_result;
    }

    RuntimeOwnerExecutorResult liveness_failed(
        RuntimeOwnerExecutorCommand, const std::uint32_t diagnostic) noexcept
    {
        ++liveness_failed_calls;
        observed_diagnostic = diagnostic;
        return completion_result;
    }

    RuntimeOwnerExecutorResult liveness_deadline_expired(
        RuntimeOwnerExecutorCommand, const std::uint32_t diagnostic) noexcept
    {
        ++liveness_deadline_calls;
        observed_diagnostic = diagnostic;
        return completion_result;
    }

    RuntimeOwnerExecutorResult snapshot_succeeded(
        RuntimeOwnerExecutorCommand, BootRuntimeSnapshotV1) noexcept
    {
        ++snapshot_succeeded_calls;
        return completion_result;
    }

    RuntimeOwnerExecutorResult snapshot_failed(
        RuntimeOwnerExecutorCommand, const std::uint32_t diagnostic) noexcept
    {
        ++snapshot_failed_calls;
        observed_diagnostic = diagnostic;
        return completion_result;
    }

    RuntimeOwnerExecutorResult commit_end_boot_delivery(
        RuntimeOwnerExecutorCommand) noexcept
    {
        ++end_boot_calls;
        return completion_result;
    }

    RuntimeOwnerExecutorResult normal_succeeded(
        RuntimeOwnerExecutorCommand) noexcept
    {
        ++normal_succeeded_calls;
        return completion_result;
    }

    RuntimeOwnerExecutorResult normal_failed(
        RuntimeOwnerExecutorCommand, const std::uint32_t diagnostic) noexcept
    {
        ++normal_failed_calls;
        observed_diagnostic = diagnostic;
        return completion_result;
    }

    RuntimeOwnerExecutorResult normal_timed_out(
        RuntimeOwnerExecutorCommand, const std::uint32_t diagnostic) noexcept
    {
        ++normal_timed_out_calls;
        observed_diagnostic = diagnostic;
        return completion_result;
    }

    RuntimeOwnerExecutorResult normal_cancelled(
        RuntimeOwnerExecutorCommand, const std::uint32_t diagnostic) noexcept
    {
        ++normal_cancelled_calls;
        observed_diagnostic = diagnostic;
        return completion_result;
    }
};

RuntimeOwnerExecutorCommand command_for(
    const RuntimeOwnerDeviceOperationKind kind,
    const CompletionPolicy policy) noexcept
{
    RuntimeOwnerExecutorCommand command{};
    command.kind = kind;
    command.completion_policy = policy;
    command.source.kind = policy == CompletionPolicy::NormalCompletion
                              ? AdapterDispatchKind::NormalIntent
                              : AdapterDispatchKind::CoreEffect;
    command.source.dispatch_sequence = 7;
    command.source.enqueue_sequence =
        policy == CompletionPolicy::NormalCompletion ? 9 : 0;
    command.source.effect.kind = RuntimeOwnerEffectKind::StartAtProbe;
    command.source.effect.correlation_id = 11;
    command.source.effect.attempt = {13, 17, 19};
    command.source.normal_intent.kind = NormalIntentKind::RefreshRssi;
    return command;
}

RuntimeOwnerPhysicalResult success() noexcept
{
    RuntimeOwnerPhysicalResult result{};
    result.kind = RuntimeOwnerPhysicalResultKind::Succeeded;
    return result;
}

void test_layout_and_no_command() noexcept
{
    CHECK(std::is_standard_layout<RuntimeOwnerPhysicalResult>::value);
    CHECK(std::is_trivially_copyable<RuntimeOwnerPhysicalResult>::value);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerPhysicalResultKind::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerPhysicalResultKind::Succeeded) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerPhysicalResultKind::Failed) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerPhysicalResultKind::TimedOut) == 3);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerPhysicalResultKind::Cancelled) == 4);

    FakePort port{};
    FakeBackend backend{};
    port.peek_result = RuntimeOwnerExecutorResult::RejectedNoCommand;
    CHECK(runtime_owner_execute_one(port, backend) ==
          RuntimeOwnerPhysicalStepResult::NoCommand);
    CHECK(backend.calls == 0);
    CHECK(port.ack_calls == 0);
}

void test_open_transport_ordered_receipts() noexcept
{
    FakePort port{};
    FakeBackend backend{};
    port.command = command_for(RuntimeOwnerDeviceOperationKind::OpenTransport,
                               CompletionPolicy::TrustedReceipt);
    backend.next = success();
    backend.next.mqtt_session_id = 23;
    backend.next.config_commit_sequence = 29;

    CHECK(runtime_owner_execute_one(port, backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(port.ack_calls == 1);
    CHECK(backend.calls == 1);
    CHECK(port.transport_established_calls == 1);
    CHECK(port.config_committed_calls == 0);
    CHECK(port.observed_session == 23);
    CHECK(backend.pending_config_commit_sequence() == 29);
    CHECK(runtime_owner_submit_deferred_config(port, backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(port.config_committed_calls == 1);
    CHECK(port.observed_config_sequence == 29);
    CHECK(backend.pending_config_commit_sequence() == 0);

    FakePort failed_port{};
    FakeBackend failed_backend{};
    failed_port.command = port.command;
    failed_backend.next.kind = RuntimeOwnerPhysicalResultKind::Failed;
    failed_backend.next.diagnostic_code = 31;
    CHECK(runtime_owner_execute_one(failed_port, failed_backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(failed_port.transport_failed_calls == 1);
    CHECK(failed_port.config_committed_calls == 0);
    CHECK(failed_port.observed_diagnostic == 31);
}

void test_liveness_and_normal_completion_tables() noexcept
{
    constexpr RuntimeOwnerDeviceOperationKind liveness[] = {
        RuntimeOwnerDeviceOperationKind::ProbeAt,
        RuntimeOwnerDeviceOperationKind::PublishProbe,
        RuntimeOwnerDeviceOperationKind::VerifySubscription,
        RuntimeOwnerDeviceOperationKind::PullFollowupConfig,
    };
    for (const auto kind : liveness) {
        FakePort port{};
        FakeBackend backend{};
        port.command = command_for(kind, CompletionPolicy::TrustedReceipt);
        backend.next = success();
        CHECK(runtime_owner_execute_one(port, backend) ==
              RuntimeOwnerPhysicalStepResult::Completed);
        CHECK(port.liveness_succeeded_calls == 1);
        CHECK(backend.calls == 1);
    }

    constexpr RuntimeOwnerDeviceOperationKind normals[] = {
        RuntimeOwnerDeviceOperationKind::PublishTelemetry,
        RuntimeOwnerDeviceOperationKind::RefreshRssi,
        RuntimeOwnerDeviceOperationKind::PullConfig,
        RuntimeOwnerDeviceOperationKind::PullCommand,
    };
    for (const auto kind : normals) {
        FakePort port{};
        FakeBackend backend{};
        port.command = command_for(kind, CompletionPolicy::NormalCompletion);
        backend.next = success();
        CHECK(runtime_owner_execute_one(port, backend) ==
              RuntimeOwnerPhysicalStepResult::Completed);
        CHECK(port.normal_succeeded_calls == 1);
        CHECK(backend.calls == 1);
    }

    struct Case {
        RuntimeOwnerPhysicalResultKind kind;
        std::uint32_t FakePort::*counter;
    };
    constexpr Case cases[] = {
        {RuntimeOwnerPhysicalResultKind::Failed,
         &FakePort::normal_failed_calls},
        {RuntimeOwnerPhysicalResultKind::TimedOut,
         &FakePort::normal_timed_out_calls},
        {RuntimeOwnerPhysicalResultKind::Cancelled,
         &FakePort::normal_cancelled_calls},
    };
    for (const Case item : cases) {
        FakePort port{};
        FakeBackend backend{};
        port.command = command_for(
            RuntimeOwnerDeviceOperationKind::PullCommand,
            CompletionPolicy::NormalCompletion);
        backend.next.kind = item.kind;
        backend.next.diagnostic_code = 37;
        CHECK(runtime_owner_execute_one(port, backend) ==
              RuntimeOwnerPhysicalStepResult::Completed);
        CHECK(port.*(item.counter) == 1);
        CHECK(port.observed_diagnostic == 37);
    }
}

void test_frozen_telemetry_command_reaches_backend_unchanged() noexcept
{
    FakePort port{};
    FakeBackend backend{};
    port.command = command_for(
        RuntimeOwnerDeviceOperationKind::PublishTelemetry,
        CompletionPolicy::NormalCompletion);
    port.command.source.normal_intent = {
        NormalIntentKind::PublishTelemetry,
        0x07,
        -166,
        2,
        91,
    };
    backend.next = success();

    CHECK(runtime_owner_execute_one(port, backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(backend.calls == 1);
    CHECK(backend.observed.source.normal_intent.kind ==
          NormalIntentKind::PublishTelemetry);
    CHECK(backend.observed.source.normal_intent.flags == 0x07);
    CHECK(backend.observed.source.normal_intent.value_deci_celsius == -166);
    CHECK(backend.observed.source.normal_intent.subject_id == 2);
    CHECK(backend.observed.source.normal_intent.snapshot_revision == 91);
    CHECK(port.normal_succeeded_calls == 1);
}

void test_snapshot_end_boot_and_fail_closed() noexcept
{
    FakePort snapshot_port{};
    FakeBackend snapshot_backend{};
    snapshot_port.command = command_for(
        RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot,
        CompletionPolicy::TrustedReceipt);
    snapshot_backend.next = success();
    snapshot_backend.next.boot_snapshot.health = SnapshotHealth::Pass;
    snapshot_backend.next.boot_snapshot.last_completed_stage =
        BootCompletedStage::PostConfigLivenessPassed;
    snapshot_backend.next.boot_snapshot.config_valid = 1;
    snapshot_backend.next.boot_snapshot.transport_ready = 1;
    snapshot_backend.next.boot_snapshot.subscription_alive = 1;
    snapshot_backend.next.boot_snapshot.post_config_liveness = 1;
    snapshot_backend.next.boot_snapshot.hardware_revision = 1;
    snapshot_backend.next.boot_snapshot.firmware_build_id = 1;
    snapshot_backend.next.boot_snapshot.config_version = 1;
    snapshot_backend.next.boot_snapshot.pdp_session_id = 1;
    snapshot_backend.next.boot_snapshot.mqtt_session_id = 13;
    snapshot_backend.next.boot_snapshot.mqtt_generation = 17;
    snapshot_backend.next.boot_snapshot.config_apply_epoch = 19;
    for (auto &sensor : snapshot_backend.next.boot_snapshot.sensors) {
        sensor.health = SnapshotHealth::Pass;
    }
    CHECK(runtime_owner_execute_one(snapshot_port, snapshot_backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(snapshot_port.snapshot_succeeded_calls == 1);

    FakePort end_port{};
    FakeBackend end_backend{};
    end_port.command = command_for(
        RuntimeOwnerDeviceOperationKind::EndBootOrchestration,
        CompletionPolicy::DeliveryOnly);
    end_backend.next = success();
    CHECK(runtime_owner_execute_one(end_port, end_backend) ==
          RuntimeOwnerPhysicalStepResult::Completed);
    CHECK(end_port.ack_calls == 0);
    CHECK(end_port.end_boot_calls == 1);

    FakePort invalid_port{};
    FakeBackend invalid_backend{};
    invalid_port.command = command_for(
        RuntimeOwnerDeviceOperationKind::PullCommand,
        CompletionPolicy::NormalCompletion);
    invalid_backend.next.kind = RuntimeOwnerPhysicalResultKind::Invalid;
    CHECK(runtime_owner_execute_one(invalid_port, invalid_backend) ==
          RuntimeOwnerPhysicalStepResult::Rejected);
    CHECK(invalid_port.normal_succeeded_calls == 0);
    CHECK(invalid_port.normal_failed_calls == 0);

    FakePort rejected_ack{};
    FakeBackend untouched{};
    rejected_ack.command = invalid_port.command;
    rejected_ack.ack_result = RuntimeOwnerExecutorResult::RejectedWrongCommand;
    CHECK(runtime_owner_execute_one(rejected_ack, untouched) ==
          RuntimeOwnerPhysicalStepResult::Rejected);
    CHECK(untouched.calls == 0);
}

} // namespace

int main()
{
    test_layout_and_no_command();
    test_open_transport_ordered_receipts();
    test_liveness_and_normal_completion_tables();
    test_frozen_telemetry_command_reaches_backend_unchanged();
    test_snapshot_end_boot_and_fail_closed();
    if (g_failures != 0) {
        std::fprintf(stderr, "runtime_owner_physical_executor_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("runtime_owner_physical_executor_test: %zu checks passed\n",
                g_checks);
    return 0;
}
