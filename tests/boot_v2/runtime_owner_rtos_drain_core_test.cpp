#include "runtime_owner_rtos_drain_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>

namespace {

using boot_v2::NormalIntent;
using boot_v2::NormalIntentKind;
using boot_v2::RuntimeOwnerActivationFacts;
using boot_v2::RuntimeOwnerActivationPreflightResult;
using boot_v2::RuntimeOwnerControlKind;
using boot_v2::RuntimeOwnerControlMessage;
using boot_v2::RuntimeOwnerDrainConsumeResult;
using boot_v2::RuntimeOwnerDrainResult;
using boot_v2::RuntimeOwnerDrainStep;
using boot_v2::RuntimeOwnerQueueReadResult;
using boot_v2::RuntimeOwnerRtosDrainMetrics;
using boot_v2::RuntimeOwnerRtosLane;
using boot_v2::RuntimeOwnerStopFacts;
using boot_v2::RuntimeOwnerStopPreflightResult;
using boot_v2::RuntimeOwnerShutdownIntent;
using boot_v2::RuntimeOwnerUrgentMessage;
using boot_v2::RuntimeOwnerUrgentSource;
using boot_v2::runtime_owner_activation_preflight;
using boot_v2::runtime_owner_drain_once;
using boot_v2::runtime_owner_drain_until_idle;
using boot_v2::runtime_owner_is_canonical_control;
using boot_v2::runtime_owner_is_canonical_urgent;
using boot_v2::runtime_owner_record_drain_step;
using boot_v2::runtime_owner_stop_preflight;

int checks = 0;
int failures = 0;

void check(const bool condition, const char *const expression, const int line)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename Value, std::size_t Capacity>
struct FakeLane {
    std::array<Value, Capacity> values{};
    std::size_t size{0};
    std::size_t next{0};
    bool fault{false};

    bool push(const Value value) noexcept
    {
        if (size == Capacity) {
            return false;
        }
        values[size++] = value;
        return true;
    }

    RuntimeOwnerQueueReadResult receive(Value &output) noexcept
    {
        if (fault) {
            return RuntimeOwnerQueueReadResult::Fault;
        }
        if (next == size) {
            return RuntimeOwnerQueueReadResult::Empty;
        }
        output = values[next++];
        return RuntimeOwnerQueueReadResult::Received;
    }
};

struct FakeBackend {
    FakeLane<RuntimeOwnerUrgentMessage, 8> urgent{};
    FakeLane<RuntimeOwnerControlMessage, 8> control{};
    FakeLane<NormalIntent, 8> normal{};
    std::uint32_t urgent_reads{0};
    std::uint32_t control_reads{0};
    std::uint32_t normal_reads{0};

    RuntimeOwnerQueueReadResult receive_urgent(
        RuntimeOwnerUrgentMessage &output) noexcept
    {
        ++urgent_reads;
        return urgent.receive(output);
    }

    RuntimeOwnerQueueReadResult receive_control(
        RuntimeOwnerControlMessage &output) noexcept
    {
        ++control_reads;
        return control.receive(output);
    }

    RuntimeOwnerQueueReadResult receive_normal(NormalIntent &output) noexcept
    {
        ++normal_reads;
        return normal.receive(output);
    }
};

struct FakeSink {
    std::array<RuntimeOwnerRtosLane, 16> lanes{};
    std::array<std::uint32_t, 16> identities{};
    std::size_t calls{0};
    bool reject_normal{false};
    std::uint32_t direct_fallback_calls{0};

    RuntimeOwnerDrainConsumeResult consume_urgent(
        const RuntimeOwnerUrgentMessage input) noexcept
    {
        lanes[calls] = RuntimeOwnerRtosLane::Urgent;
        identities[calls++] = input.producer_sequence;
        return runtime_owner_is_canonical_urgent(input)
                   ? RuntimeOwnerDrainConsumeResult::Processed
                   : RuntimeOwnerDrainConsumeResult::DroppedInvalid;
    }

    RuntimeOwnerDrainConsumeResult consume_control(
        const RuntimeOwnerControlMessage input) noexcept
    {
        lanes[calls] = RuntimeOwnerRtosLane::Control;
        identities[calls++] = 0;
        return runtime_owner_is_canonical_control(input)
                   ? RuntimeOwnerDrainConsumeResult::Processed
                   : RuntimeOwnerDrainConsumeResult::DroppedInvalid;
    }

    RuntimeOwnerDrainConsumeResult consume_normal(
        const NormalIntent input) noexcept
    {
        lanes[calls] = RuntimeOwnerRtosLane::Normal;
        identities[calls++] = input.subject_id;
        return reject_normal
                   ? RuntimeOwnerDrainConsumeResult::DroppedInvalid
                   : RuntimeOwnerDrainConsumeResult::Processed;
    }
};

constexpr RuntimeOwnerUrgentMessage urgent_message(
    const std::uint32_t sequence) noexcept
{
    return {RuntimeOwnerUrgentSource::PowerButton,
            RuntimeOwnerShutdownIntent::AutomaticByUsb,
            {},
            sequence,
            91};
}

constexpr RuntimeOwnerControlMessage control_message() noexcept
{
    return {RuntimeOwnerControlKind::RequestTransportAttempt, {}};
}

constexpr NormalIntent normal_message(const std::uint32_t subject) noexcept
{
    NormalIntent value{};
    value.kind = NormalIntentKind::PublishTelemetry;
    value.subject_id = subject;
    value.snapshot_revision = subject;
    return value;
}

struct RecordingRecorder {
    std::array<RuntimeOwnerDrainStep, 16> steps{};
    std::size_t calls{0};

    void operator()(const RuntimeOwnerDrainStep step) noexcept
    {
        steps[calls++] = step;
    }
};

struct UrgentInjectingSink : FakeSink {
    FakeBackend *backend{nullptr};

    RuntimeOwnerDrainConsumeResult consume_normal(
        const NormalIntent input) noexcept
    {
        const RuntimeOwnerDrainConsumeResult result =
            FakeSink::consume_normal(input);
        (void)backend->urgent.push(urgent_message(77));
        return result;
    }
};

constexpr bool step_is(
    const RuntimeOwnerDrainStep step,
    const RuntimeOwnerDrainResult result,
    const RuntimeOwnerRtosLane lane) noexcept
{
    return step.result == result && step.lane == lane &&
           step.reserved[0] == 0 && step.reserved[1] == 0;
}

std::string read_source(const char *const path)
{
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

std::size_t count_occurrences(
    const std::string &source,
    const std::string &needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = source.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

bool test_rtos_source_includes_and_calls_shared_drain()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    return !source.empty() &&
           source.find("runtime_owner_rtos_drain_core.hpp") !=
               std::string::npos &&
           count_occurrences(
               source, "runtime_owner_drain_until_idle(") == 0 &&
           count_occurrences(source, "runtime_owner_drain_once(") == 1 &&
           source.find("copy_shutdown_context(shutdown_context)") !=
               std::string::npos &&
           source.find("SHUTDOWN_INGRESS_CLOSED") !=
               std::string::npos &&
           source.find("runtime_owner_record_drain_step") !=
               std::string::npos;
}

bool test_urgent_queue_uses_provenance_message()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    return source.find("alignas(RuntimeOwnerUrgentMessage)") !=
               std::string::npos &&
           source.find(
               "kUrgentDepth * sizeof(RuntimeOwnerUrgentMessage)") !=
               std::string::npos &&
           count_occurrences(
               source, "sizeof(RuntimeOwnerUrgentMessage)") == 2;
}

bool test_public_surface_remains_read_only_and_narrow()
{
    const std::string header = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.hpp");
    return header.find("runtime_owner_rtos_drain_metrics") !=
               std::string::npos &&
           header.find("try_submit_urgent") == std::string::npos &&
           header.find("try_request_shutdown") == std::string::npos &&
           header.find("runtime_owner_rtos_activate_atomic") !=
               std::string::npos &&
           header.find("runtime_owner_activate") == std::string::npos &&
           header.find("runtime_owner_deactivate") == std::string::npos &&
           header.find("set_ingress") == std::string::npos &&
           header.find("QueueHandle_t") == std::string::npos &&
           header.find("TaskHandle_t") == std::string::npos;
}

bool test_production_atomic_cutover_is_single_and_ordered()
{
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    const std::size_t ready =
        source.find("g_drain_metrics.cutover_ready = 1");
    const std::size_t ingress =
        source.find("g_status.ingress_enabled = 1");
    return ingress != std::string::npos &&
           source.find("g_status.ingress_enabled = 0") !=
               std::string::npos &&
           source.find("g_drain_metrics.receiver_ready = 1") !=
               std::string::npos &&
           source.find("g_drain_metrics.cutover_ready = 0") !=
               std::string::npos &&
           ready != std::string::npos && ready < ingress &&
           source.find("g_cutover_coordinator.activate(") !=
               std::string::npos;
}

bool test_numeric_layout_and_default_contracts()
{
    const RuntimeOwnerRtosDrainMetrics metrics{};
    return static_cast<std::uint8_t>(RuntimeOwnerRtosLane::None) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerRtosLane::Urgent) == 1 &&
           static_cast<std::uint8_t>(RuntimeOwnerRtosLane::Control) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerRtosLane::Normal) == 3 &&
           static_cast<std::uint8_t>(RuntimeOwnerQueueReadResult::Empty) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerQueueReadResult::Received) == 1 &&
           static_cast<std::uint8_t>(RuntimeOwnerQueueReadResult::Fault) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerDrainResult::Empty) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerDrainResult::Processed) == 1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerDrainResult::DroppedInvalid) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerDrainResult::Fault) == 3 &&
           std::is_same<
               std::underlying_type<RuntimeOwnerDrainConsumeResult>::type,
               std::uint8_t>::value &&
           static_cast<std::uint8_t>(
               RuntimeOwnerDrainConsumeResult::Processed) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerDrainConsumeResult::DroppedInvalid) == 1 &&
           std::is_same<
               std::underlying_type<RuntimeOwnerActivationPreflightResult>::
                   type,
               std::uint8_t>::value &&
           static_cast<std::uint8_t>(
               RuntimeOwnerActivationPreflightResult::RejectedInvalid) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerActivationPreflightResult::RejectedIncomplete) ==
               1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerActivationPreflightResult::Ready) == 2 &&
           std::is_same<
               std::underlying_type<RuntimeOwnerStopPreflightResult>::type,
               std::uint8_t>::value &&
           static_cast<std::uint8_t>(
               RuntimeOwnerStopPreflightResult::RejectedInvalid) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerStopPreflightResult::SafePreAdmissionAbort) == 1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot) == 2 &&
           static_cast<std::uint8_t>(RuntimeOwnerUrgentSource::Invalid) == 0 &&
           static_cast<std::uint8_t>(RuntimeOwnerUrgentSource::PowerButton) == 1 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerUrgentSource::AdapterLossCommitted) == 2 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand) == 3 &&
           static_cast<std::uint8_t>(RuntimeOwnerShutdownIntent::Invalid) == 0 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerShutdownIntent::AutomaticByUsb) == 1 &&
           static_cast<std::uint8_t>(RuntimeOwnerShutdownIntent::Reboot) == 2 &&
           static_cast<std::uint8_t>(
               RuntimeOwnerShutdownIntent::PowerOff) == 3 &&
           sizeof(RuntimeOwnerUrgentMessage) == 12 &&
           alignof(RuntimeOwnerUrgentMessage) == 4 &&
           sizeof(RuntimeOwnerDrainStep) == 4 &&
           alignof(RuntimeOwnerDrainStep) == 1 &&
           sizeof(RuntimeOwnerActivationFacts) == 8 &&
           alignof(RuntimeOwnerActivationFacts) == 1 &&
           sizeof(RuntimeOwnerStopFacts) == 8 &&
           alignof(RuntimeOwnerStopFacts) == 1 &&
           sizeof(RuntimeOwnerRtosDrainMetrics) == 24 &&
           alignof(RuntimeOwnerRtosDrainMetrics) == 4 &&
           std::is_standard_layout<RuntimeOwnerUrgentMessage>::value &&
           std::is_trivially_copyable<RuntimeOwnerUrgentMessage>::value &&
           metrics.urgent_processed_count == 0 &&
           metrics.control_processed_count == 0 &&
           metrics.normal_processed_count == 0 &&
           metrics.dropped_invalid_count == 0 &&
           metrics.receive_fault_count == 0 && metrics.receiver_ready == 0 &&
           metrics.cutover_ready == 0 && metrics.reserved[0] == 0 &&
           metrics.reserved[1] == 0;
}

bool test_all_empty_reads_no_sink()
{
    FakeBackend backend{};
    FakeSink sink{};
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 1 && sink.calls == 0;
}

bool test_urgent_precedes_control_and_normal()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.urgent.push(urgent_message(1));
    (void)backend.control.push(control_message());
    (void)backend.normal.push(normal_message(3));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           backend.urgent_reads == 1 && backend.control_reads == 0 &&
           backend.normal_reads == 0 && sink.calls == 1 &&
           sink.lanes[0] == RuntimeOwnerRtosLane::Urgent;
}

bool test_control_precedes_normal()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.control.push(control_message());
    (void)backend.normal.push(normal_message(4));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Control) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 0 && sink.calls == 1;
}

bool test_normal_only()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.normal.push(normal_message(41));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Normal) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 1 && sink.calls == 1 &&
           sink.identities[0] == 41;
}

bool test_next_pass_rechecks_new_urgent()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.normal.push(normal_message(51));
    const auto first = runtime_owner_drain_once(backend, sink);
    (void)backend.urgent.push(urgent_message(52));
    const auto second = runtime_owner_drain_once(backend, sink);
    return step_is(first, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Normal) &&
           step_is(second, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           sink.calls == 2 &&
           sink.lanes[0] == RuntimeOwnerRtosLane::Normal &&
           sink.lanes[1] == RuntimeOwnerRtosLane::Urgent;
}

bool test_drain_until_idle_records_once_and_rechecks_urgent()
{
    FakeBackend backend{};
    UrgentInjectingSink sink{};
    sink.backend = &backend;
    RecordingRecorder recorder{};
    (void)backend.normal.push(normal_message(51));

    const RuntimeOwnerDrainStep terminal =
        runtime_owner_drain_until_idle(backend, sink, recorder);

    return step_is(terminal,
                   RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           recorder.calls == 3 &&
           step_is(recorder.steps[0],
                   RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Normal) &&
           step_is(recorder.steps[1],
                   RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           step_is(recorder.steps[2],
                   RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           sink.calls == 2;
}

bool test_drain_until_idle_records_fault_once_and_stops()
{
    FakeBackend backend{};
    FakeSink sink{};
    RecordingRecorder recorder{};
    backend.control.fault = true;

    const RuntimeOwnerDrainStep terminal =
        runtime_owner_drain_until_idle(backend, sink, recorder);

    return step_is(terminal,
                   RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Control) &&
           recorder.calls == 1 &&
           step_is(recorder.steps[0],
                   RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Control) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 0 && sink.calls == 0;
}

bool test_drain_until_idle_metrics_count_drop_and_terminal_empty_once()
{
    FakeBackend backend{};
    FakeSink sink{};
    sink.reject_normal = true;
    RuntimeOwnerRtosDrainMetrics metrics{};
    std::size_t recorder_calls = 0;
    (void)backend.normal.push(normal_message(61));

    const RuntimeOwnerDrainStep terminal = runtime_owner_drain_until_idle(
        backend,
        sink,
        [&metrics, &recorder_calls](const RuntimeOwnerDrainStep step) noexcept {
            ++recorder_calls;
            runtime_owner_record_drain_step(metrics, step);
        });

    return step_is(terminal,
                   RuntimeOwnerDrainResult::Empty,
                   RuntimeOwnerRtosLane::None) &&
           recorder_calls == 2 && metrics.dropped_invalid_count == 1 &&
           metrics.normal_processed_count == 0 &&
           metrics.receive_fault_count == 0;
}

bool test_fifo_within_lane()
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.urgent.push(urgent_message(7));
    (void)backend.urgent.push(urgent_message(8));
    const auto first = runtime_owner_drain_once(backend, sink);
    const auto second = runtime_owner_drain_once(backend, sink);
    return step_is(first, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           step_is(second, RuntimeOwnerDrainResult::Processed,
                   RuntimeOwnerRtosLane::Urgent) &&
           sink.calls == 2 && sink.identities[0] == 7 &&
           sink.identities[1] == 8;
}

bool test_urgent_fault_stops_lower_reads()
{
    FakeBackend backend{};
    FakeSink sink{};
    backend.urgent.fault = true;
    (void)backend.control.push(control_message());
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Urgent) &&
           backend.control_reads == 0 && backend.normal_reads == 0 &&
           sink.calls == 0;
}

bool test_control_fault_stops_normal_read()
{
    FakeBackend backend{};
    FakeSink sink{};
    backend.control.fault = true;
    (void)backend.normal.push(normal_message(5));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Control) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 0 && sink.calls == 0;
}

bool test_normal_fault_calls_no_sink()
{
    FakeBackend backend{};
    FakeSink sink{};
    backend.normal.fault = true;
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::Fault,
                   RuntimeOwnerRtosLane::Normal) &&
           backend.urgent_reads == 1 && backend.control_reads == 1 &&
           backend.normal_reads == 1 && sink.calls == 0;
}

bool urgent_drops(const RuntimeOwnerUrgentMessage input)
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.urgent.push(input);
    return step_is(runtime_owner_drain_once(backend, sink),
                   RuntimeOwnerDrainResult::DroppedInvalid,
                   RuntimeOwnerRtosLane::Urgent) &&
           sink.calls == 1;
}

bool test_invalid_urgent_variants_drop()
{
    auto source = urgent_message(1);
    source.source = RuntimeOwnerUrgentSource::Invalid;
    auto intent = urgent_message(1);
    intent.intent = RuntimeOwnerShutdownIntent::Invalid;
    auto local_reboot = urgent_message(1);
    local_reboot.intent = RuntimeOwnerShutdownIntent::Reboot;
    auto authenticated_automatic = urgent_message(1);
    authenticated_automatic.source =
        RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand;
    auto sequence = urgent_message(1);
    sequence.producer_sequence = 0;
    auto correlation = urgent_message(1);
    correlation.incident_correlation_id = 0;
    auto reserved0 = urgent_message(1);
    reserved0.reserved[0] = 1;
    auto reserved1 = urgent_message(1);
    reserved1.reserved[1] = 1;
    return urgent_drops(source) && urgent_drops(intent) &&
           urgent_drops(local_reboot) &&
           urgent_drops(authenticated_automatic) &&
           urgent_drops(sequence) &&
           urgent_drops(correlation) && urgent_drops(reserved0) &&
           urgent_drops(reserved1);
}

bool control_drops(const RuntimeOwnerControlMessage input)
{
    FakeBackend backend{};
    FakeSink sink{};
    (void)backend.control.push(input);
    return step_is(runtime_owner_drain_once(backend, sink),
                   RuntimeOwnerDrainResult::DroppedInvalid,
                   RuntimeOwnerRtosLane::Control) &&
           sink.calls == 1;
}

bool test_invalid_control_variants_drop()
{
    RuntimeOwnerControlMessage kind{};
    auto reserved0 = control_message();
    reserved0.reserved[0] = 1;
    auto reserved1 = control_message();
    reserved1.reserved[1] = 1;
    auto reserved2 = control_message();
    reserved2.reserved[2] = 1;
    return control_drops(kind) && control_drops(reserved0) &&
           control_drops(reserved1) && control_drops(reserved2);
}

bool test_normal_rejection_has_no_fallback()
{
    FakeBackend backend{};
    FakeSink sink{};
    sink.reject_normal = true;
    (void)backend.normal.push(normal_message(61));
    const auto step = runtime_owner_drain_once(backend, sink);
    return step_is(step, RuntimeOwnerDrainResult::DroppedInvalid,
                   RuntimeOwnerRtosLane::Normal) &&
           sink.calls == 1 && sink.direct_fallback_calls == 0;
}

bool metrics_equal(
    const RuntimeOwnerRtosDrainMetrics left,
    const RuntimeOwnerRtosDrainMetrics right) noexcept
{
    return left.urgent_processed_count == right.urgent_processed_count &&
           left.control_processed_count == right.control_processed_count &&
           left.normal_processed_count == right.normal_processed_count &&
           left.dropped_invalid_count == right.dropped_invalid_count &&
           left.receive_fault_count == right.receive_fault_count &&
           left.receiver_ready == right.receiver_ready &&
           left.cutover_ready == right.cutover_ready &&
           left.reserved[0] == right.reserved[0] &&
           left.reserved[1] == right.reserved[1];
}

bool test_metrics_exact_increment()
{
    RuntimeOwnerRtosDrainMetrics metrics{};
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Urgent, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Control, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Normal, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::DroppedInvalid,
                  RuntimeOwnerRtosLane::Urgent, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Fault,
                  RuntimeOwnerRtosLane::Control, {}});
    const RuntimeOwnerRtosDrainMetrics before_empty = metrics;
    runtime_owner_record_drain_step(metrics, {});
    return metrics.urgent_processed_count == 1 &&
           metrics.control_processed_count == 1 &&
           metrics.normal_processed_count == 1 &&
           metrics.dropped_invalid_count == 1 &&
           metrics.receive_fault_count == 1 &&
           metrics_equal(metrics, before_empty);
}

bool test_metrics_saturate_at_uint32_max()
{
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    RuntimeOwnerRtosDrainMetrics metrics{
        maximum, maximum, maximum, maximum, maximum, 0, 0, {}};
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Urgent, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Control, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Processed,
                  RuntimeOwnerRtosLane::Normal, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::DroppedInvalid,
                  RuntimeOwnerRtosLane::None, {}});
    runtime_owner_record_drain_step(
        metrics, {RuntimeOwnerDrainResult::Fault,
                  RuntimeOwnerRtosLane::Normal, {}});
    return metrics.urgent_processed_count == maximum &&
           metrics.control_processed_count == maximum &&
           metrics.normal_processed_count == maximum &&
           metrics.dropped_invalid_count == maximum &&
           metrics.receive_fault_count == maximum;
}

bool test_activation_preflight_table()
{
    RuntimeOwnerActivationFacts invalid_bool{};
    invalid_bool.queue_drain_ready = 2;
    RuntimeOwnerActivationFacts dirty_reserved{};
    dirty_reserved.reserved[2] = 1;
    const RuntimeOwnerActivationFacts partial{1, 0, 0, 0, 0, {}};
    const RuntimeOwnerActivationFacts ready{1, 1, 1, 1, 1, {}};
    return runtime_owner_activation_preflight(invalid_bool, 9) ==
               RuntimeOwnerActivationPreflightResult::RejectedInvalid &&
           runtime_owner_activation_preflight(dirty_reserved, 9) ==
               RuntimeOwnerActivationPreflightResult::RejectedInvalid &&
           runtime_owner_activation_preflight(ready, 0) ==
               RuntimeOwnerActivationPreflightResult::RejectedInvalid &&
           runtime_owner_activation_preflight(partial, 9) ==
               RuntimeOwnerActivationPreflightResult::RejectedIncomplete &&
           runtime_owner_activation_preflight(ready, 9) ==
               RuntimeOwnerActivationPreflightResult::Ready;
}

bool test_stop_preflight_and_dependency_boundary()
{
    RuntimeOwnerStopFacts invalid{};
    invalid.work_started = 2;
    RuntimeOwnerStopFacts dirty{};
    dirty.reserved[3] = 1;
    const RuntimeOwnerStopFacts safe{0, 0, 1, 1, {}};
    const RuntimeOwnerStopFacts admitted{1, 0, 1, 1, {}};
    const RuntimeOwnerStopFacts started{0, 1, 1, 1, {}};
    const RuntimeOwnerStopFacts queued{0, 0, 0, 1, {}};
    const RuntimeOwnerStopFacts inflight{0, 0, 1, 0, {}};
    const std::string source = read_source(
        NB_IOT_SOURCE_ROOT
        "/src/boot_v2/runtime_owner_rtos_drain_core.hpp");
    const bool dependencies_clean =
        !source.empty() && source.find("FreeRTOS.h") == std::string::npos &&
        source.find("queue.h") == std::string::npos &&
        source.find("task.h") == std::string::npos &&
        source.find("new ") == std::string::npos &&
        source.find("malloc") == std::string::npos &&
        source.find("throw") == std::string::npos &&
        source.find("catch") == std::string::npos;
    return runtime_owner_stop_preflight(invalid) ==
               RuntimeOwnerStopPreflightResult::RejectedInvalid &&
           runtime_owner_stop_preflight(dirty) ==
               RuntimeOwnerStopPreflightResult::RejectedInvalid &&
           runtime_owner_stop_preflight(safe) ==
               RuntimeOwnerStopPreflightResult::SafePreAdmissionAbort &&
           runtime_owner_stop_preflight(admitted) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           runtime_owner_stop_preflight(started) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           runtime_owner_stop_preflight(queued) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           runtime_owner_stop_preflight(inflight) ==
               RuntimeOwnerStopPreflightResult::RequiresCleanReboot &&
           dependencies_clean;
}

}  // namespace

int main()
{
    CHECK(test_numeric_layout_and_default_contracts());
    CHECK(test_all_empty_reads_no_sink());
    CHECK(test_urgent_precedes_control_and_normal());
    CHECK(test_control_precedes_normal());
    CHECK(test_normal_only());
    CHECK(test_next_pass_rechecks_new_urgent());
    CHECK(test_drain_until_idle_records_once_and_rechecks_urgent());
    CHECK(test_drain_until_idle_records_fault_once_and_stops());
    CHECK(test_drain_until_idle_metrics_count_drop_and_terminal_empty_once());
    CHECK(test_fifo_within_lane());
    CHECK(test_urgent_fault_stops_lower_reads());
    CHECK(test_control_fault_stops_normal_read());
    CHECK(test_normal_fault_calls_no_sink());
    CHECK(test_invalid_urgent_variants_drop());
    CHECK(test_invalid_control_variants_drop());
    CHECK(test_normal_rejection_has_no_fallback());
    CHECK(test_metrics_exact_increment());
    CHECK(test_metrics_saturate_at_uint32_max());
    CHECK(test_activation_preflight_table());
    CHECK(test_stop_preflight_and_dependency_boundary());
    CHECK(test_rtos_source_includes_and_calls_shared_drain());
    CHECK(test_urgent_queue_uses_provenance_message());
    CHECK(test_public_surface_remains_read_only_and_narrow());
    CHECK(test_production_atomic_cutover_is_single_and_ordered());

    if (failures != 0) {
        std::printf("FAIL checks=%d failures=%d\n", checks, failures);
        return 1;
    }
    std::printf("PASS checks=%d\n", checks);
    return 0;
}
