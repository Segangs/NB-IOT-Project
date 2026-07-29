#include "runtime_owner_producer_facade.hpp"
#include "power_state_runtime.hpp"
#include "runtime_owner_rtos_drain_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

namespace boot_v2::runtime_owner_rtos_detail {

RuntimeOwnerRtosLane g_lane{RuntimeOwnerRtosLane::None};
RuntimeOwnerControlMessage g_control{};
NormalIntent g_normal{};
RuntimeOwnerUrgentMessage g_urgent{};
std::uint32_t g_calls{0};
RuntimeOwnerIngressResult g_next_result{
    RuntimeOwnerIngressResult::AcceptedForDelivery};

RuntimeOwnerIngressResult submit_control(
    const RuntimeOwnerControlMessage value) noexcept
{
    g_lane = RuntimeOwnerRtosLane::Control;
    g_control = value;
    ++g_calls;
    return g_next_result;
}

RuntimeOwnerIngressResult submit_normal(const NormalIntent value) noexcept
{
    g_lane = RuntimeOwnerRtosLane::Normal;
    g_normal = value;
    ++g_calls;
    return g_next_result;
}

RuntimeOwnerIngressResult submit_urgent(
    const RuntimeOwnerUrgentMessage value) noexcept
{
    g_lane = RuntimeOwnerRtosLane::Urgent;
    g_urgent = value;
    ++g_calls;
    return g_next_result;
}

void reset() noexcept
{
    g_lane = RuntimeOwnerRtosLane::None;
    g_control = {};
    g_normal = {};
    g_urgent = {};
    g_calls = 0;
    g_next_result = RuntimeOwnerIngressResult::AcceptedForDelivery;
}

} // namespace boot_v2::runtime_owner_rtos_detail

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

std::string read_file(const char *path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::size_t count_occurrences(
    const std::string &source,
    const std::string &needle) noexcept
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = source.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void test_boot_and_periodic_control_routes() noexcept
{
    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_boot_request_transport() ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
    CHECK(runtime_owner_rtos_detail::g_lane == RuntimeOwnerRtosLane::Control);
    CHECK(runtime_owner_rtos_detail::g_control.kind ==
          RuntimeOwnerControlKind::RequestTransportAttempt);

    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_periodic_request_transport() ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
    CHECK(runtime_owner_rtos_detail::g_lane == RuntimeOwnerRtosLane::Control);
}

void test_periodic_normal_routes() noexcept
{
    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_periodic_publish_telemetry(2, 91, -166) ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
    CHECK(runtime_owner_rtos_detail::g_normal.kind ==
          NormalIntentKind::PublishTelemetry);
    CHECK(runtime_owner_rtos_detail::g_normal.flags == 0x01);
    CHECK(runtime_owner_rtos_detail::g_normal.value_deci_celsius == -166);
    CHECK(runtime_owner_rtos_detail::g_normal.subject_id == 2);
    CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 91);

    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_periodic_publish_telemetry(0, 91, 100) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_rtos_detail::g_calls == 0);
    CHECK(runtime_owner_periodic_publish_telemetry(3, 91, 100) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_rtos_detail::g_calls == 0);
    CHECK(runtime_owner_periodic_publish_telemetry(2, 0, 100) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_rtos_detail::g_calls == 0);

    struct Case {
        RuntimeOwnerIngressResult (*call)() noexcept;
        NormalIntentKind expected;
    };
    constexpr Case cases[] = {
        {runtime_owner_periodic_refresh_rssi, NormalIntentKind::RefreshRssi},
        {runtime_owner_periodic_pull_config, NormalIntentKind::PullConfig},
        {runtime_owner_periodic_pull_command, NormalIntentKind::PullCommand},
    };
    for (const Case item : cases) {
        runtime_owner_rtos_detail::reset();
        CHECK(item.call() == RuntimeOwnerIngressResult::AcceptedForDelivery);
        CHECK(runtime_owner_rtos_detail::g_calls == 1);
        CHECK(runtime_owner_rtos_detail::g_lane == RuntimeOwnerRtosLane::Normal);
        CHECK(runtime_owner_rtos_detail::g_normal.kind == item.expected);
        CHECK(runtime_owner_rtos_detail::g_normal.flags == 0);
        CHECK(runtime_owner_rtos_detail::g_normal.value_deci_celsius == 0);
        CHECK(runtime_owner_rtos_detail::g_normal.subject_id == 0);
        CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 0);
    }
}

void test_battery_grace_blocks_only_periodic_work() noexcept
{
    power_state_set_battery_grace(true);

    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_periodic_request_transport() ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_periodic_publish_telemetry(1, 7, 225) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_periodic_refresh_rssi() ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_periodic_pull_config() ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_periodic_pull_command() ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_rtos_detail::g_calls == 0);

    CHECK(runtime_owner_sensor_publish_alarm(
              1, 7, 225, TemperatureAlarmEdge::High) ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);

    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_power_publish_adapter_removed(42, 1) ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);

    power_state_set_battery_grace(false);
    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_periodic_pull_config() ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
}

void test_sensor_alarm_routes_freeze_edge_revision_and_value() noexcept
{
    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_sensor_publish_alarm(
              1, 17, -155, TemperatureAlarmEdge::High) ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
    CHECK(runtime_owner_rtos_detail::g_lane == RuntimeOwnerRtosLane::Normal);
    CHECK(runtime_owner_rtos_detail::g_normal.kind ==
          NormalIntentKind::PublishTelemetry);
    CHECK(runtime_owner_rtos_detail::g_normal.flags == 0x07);
    CHECK(runtime_owner_rtos_detail::g_normal.value_deci_celsius == -155);
    CHECK(runtime_owner_rtos_detail::g_normal.subject_id == 1);
    CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 17);

    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_sensor_publish_alarm(
              2, 18, 231, TemperatureAlarmEdge::Clear) ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
    CHECK(runtime_owner_rtos_detail::g_normal.flags == 0x03);
    CHECK(runtime_owner_rtos_detail::g_normal.value_deci_celsius == 231);
    CHECK(runtime_owner_rtos_detail::g_normal.subject_id == 2);
    CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 18);

    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_sensor_publish_alarm(
              0, 17, 200, TemperatureAlarmEdge::High) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_sensor_publish_alarm(
              3, 17, 200, TemperatureAlarmEdge::High) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_sensor_publish_alarm(
              1, 0, 200, TemperatureAlarmEdge::High) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_sensor_publish_alarm(
              1, 17, 200, TemperatureAlarmEdge::Invalid) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_sensor_publish_alarm(
              1,
              17,
              200,
              static_cast<TemperatureAlarmEdge>(0xff)) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_rtos_detail::g_calls == 0);
}

void test_normal_facade_propagates_admission_failure() noexcept
{
    runtime_owner_rtos_detail::reset();
    runtime_owner_rtos_detail::g_next_result =
        RuntimeOwnerIngressResult::RejectedFull;
    CHECK(runtime_owner_sensor_publish_alarm(
              1, 77, 345, TemperatureAlarmEdge::Clear) ==
          RuntimeOwnerIngressResult::RejectedFull);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
    CHECK(runtime_owner_rtos_detail::g_normal.flags == 0x03);
    CHECK(runtime_owner_rtos_detail::g_normal.value_deci_celsius == 345);
    CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 77);
}

void test_sensor_reader_freezes_exact_alarm_offer_before_marking_inflight()
    noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_sensor_reader.cpp");
    CHECK(!source.empty());
    CHECK(count_occurrences(
              source, "runtime_owner_sensor_publish_alarm(") == 2);
    CHECK(count_occurrences(source, ".mark_enqueued(") == 2);
    CHECK(source.find(".confirm_submitted(") == std::string::npos);

    const std::size_t call0 =
        source.find("runtime_owner_sensor_publish_alarm(");
    const std::size_t observe0 =
        source.find("alert_publish_ch0.observe(");
    const std::size_t observed_value0 =
        source.find("quality0.snapshot.value_deci_celsius", observe0);
    const std::size_t revision0 =
        source.find("alert0.snapshot_revision", call0);
    const std::size_t value0 =
        source.find("alert0.value_deci_celsius", revision0);
    const std::size_t edge0 = source.find("alert0.edge", value0);
    const std::size_t accepted0 = source.find(
        "RuntimeOwnerIngressResult::AcceptedForDelivery", edge0);
    const std::size_t accepted_block_open0 =
        source.find('{', accepted0);
    const std::size_t mark0 =
        source.find("alert_publish_ch0.mark_enqueued(", accepted0);
    const std::size_t accepted_block_close0 =
        source.find('}', accepted_block_open0);
    const std::size_t marked_revision0 =
        source.find("alert0.snapshot_revision", mark0);
    const std::size_t marked_edge0 =
        source.find("alert0.edge", marked_revision0);

    const std::size_t call1 =
        source.find("runtime_owner_sensor_publish_alarm(", call0 + 1);
    const std::size_t observe1 =
        source.find("alert_publish_ch1.observe(", call0);
    const std::size_t observed_value1 =
        source.find("quality1.snapshot.value_deci_celsius", observe1);
    const std::size_t revision1 =
        source.find("alert1.snapshot_revision", call1);
    const std::size_t value1 =
        source.find("alert1.value_deci_celsius", revision1);
    const std::size_t edge1 = source.find("alert1.edge", value1);
    const std::size_t accepted1 = source.find(
        "RuntimeOwnerIngressResult::AcceptedForDelivery", edge1);
    const std::size_t accepted_block_open1 =
        source.find('{', accepted1);
    const std::size_t mark1 =
        source.find("alert_publish_ch1.mark_enqueued(", accepted1);
    const std::size_t accepted_block_close1 =
        source.find('}', accepted_block_open1);
    const std::size_t marked_revision1 =
        source.find("alert1.snapshot_revision", mark1);
    const std::size_t marked_edge1 =
        source.find("alert1.edge", marked_revision1);

    CHECK(call0 != std::string::npos);
    CHECK(observe0 != std::string::npos);
    CHECK(observed_value0 != std::string::npos);
    CHECK(observe0 < observed_value0 && observed_value0 < call0);
    CHECK(call0 < revision0 && revision0 < value0 && value0 < edge0);
    CHECK(edge0 < accepted0 && accepted0 < accepted_block_open0);
    CHECK(accepted_block_open0 < mark0 &&
          mark0 < accepted_block_close0);
    CHECK(mark0 < marked_revision0 && marked_revision0 < marked_edge0);
    CHECK(marked_edge0 < call1);
    CHECK(observe1 != std::string::npos);
    CHECK(observed_value1 != std::string::npos);
    CHECK(observe1 < observed_value1 && observed_value1 < call1);
    CHECK(call1 < revision1 && revision1 < value1 && value1 < edge1);
    CHECK(edge1 < accepted1 && accepted1 < accepted_block_open1);
    CHECK(accepted_block_open1 < mark1 &&
          mark1 < accepted_block_close1);
    CHECK(mark1 < marked_revision1 && marked_revision1 < marked_edge1);
}

void test_adapter_monitor_power_event_routes() noexcept
{
    struct Case {
        RuntimeOwnerIngressResult (*call)(
            std::uint32_t,
            std::uint32_t) noexcept;
        NormalIntentKind expected;
    };
    constexpr Case cases[] = {
        {runtime_owner_power_publish_adapter_removed,
         NormalIntentKind::PublishAdapterRemoved},
        {runtime_owner_power_publish_adapter_restored,
         NormalIntentKind::PublishAdapterRestored},
    };
    for (const Case item : cases) {
        runtime_owner_rtos_detail::reset();
        CHECK(item.call(42, 2) ==
              RuntimeOwnerIngressResult::AcceptedForDelivery);
        CHECK(runtime_owner_rtos_detail::g_calls == 1);
        CHECK(runtime_owner_rtos_detail::g_lane ==
              RuntimeOwnerRtosLane::Normal);
        CHECK(runtime_owner_rtos_detail::g_normal.kind == item.expected);
        CHECK(runtime_owner_rtos_detail::g_normal.flags == 0);
        CHECK(runtime_owner_rtos_detail::g_normal.value_deci_celsius == 0);
        CHECK(runtime_owner_rtos_detail::g_normal.subject_id == 42);
        CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 2);

        runtime_owner_rtos_detail::reset();
        CHECK(item.call(0, 2) ==
              RuntimeOwnerIngressResult::RejectedInvalid);
        CHECK(item.call(42, 0) ==
              RuntimeOwnerIngressResult::RejectedInvalid);
        CHECK(runtime_owner_rtos_detail::g_calls == 0);
    }
}

void test_exact_shutdown_sources_and_invalid_identity() noexcept
{
    struct Case {
        RuntimeOwnerIngressResult (*call)(std::uint32_t, std::uint32_t) noexcept;
        RuntimeOwnerUrgentSource expected;
        RuntimeOwnerShutdownIntent expected_intent;
    };
    constexpr Case cases[] = {
        {runtime_owner_power_button_request_shutdown,
         RuntimeOwnerUrgentSource::PowerButton,
         RuntimeOwnerShutdownIntent::AutomaticByUsb},
        {runtime_owner_adapter_loss_request_shutdown,
         RuntimeOwnerUrgentSource::AdapterLossCommitted,
         RuntimeOwnerShutdownIntent::AutomaticByUsb},
        {runtime_owner_authenticated_request_reboot,
         RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand,
         RuntimeOwnerShutdownIntent::Reboot},
        {runtime_owner_authenticated_request_power_off,
         RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand,
         RuntimeOwnerShutdownIntent::PowerOff},
    };
    for (const Case item : cases) {
        runtime_owner_rtos_detail::reset();
        CHECK(item.call(7, 11) ==
              RuntimeOwnerIngressResult::AcceptedForDelivery);
        CHECK(runtime_owner_rtos_detail::g_calls == 1);
        CHECK(runtime_owner_rtos_detail::g_lane == RuntimeOwnerRtosLane::Urgent);
        CHECK(runtime_owner_rtos_detail::g_urgent.source == item.expected);
        CHECK(runtime_owner_rtos_detail::g_urgent.intent ==
              item.expected_intent);
        CHECK(runtime_owner_rtos_detail::g_urgent.producer_sequence == 7);
        CHECK(runtime_owner_rtos_detail::g_urgent.incident_correlation_id == 11);

        runtime_owner_rtos_detail::reset();
        CHECK(item.call(0, 11) == RuntimeOwnerIngressResult::RejectedInvalid);
        CHECK(runtime_owner_rtos_detail::g_calls == 0);
        CHECK(item.call(7, 0) == RuntimeOwnerIngressResult::RejectedInvalid);
        CHECK(runtime_owner_rtos_detail::g_calls == 0);
    }
}

void test_public_surface_is_nonforgeable() noexcept
{
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_producer_facade.hpp");
    const std::string rtos_header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.hpp");
    CHECK(!header.empty());
    CHECK(!rtos_header.empty());
    CHECK(header.find("RuntimeOwnerProducerKind") == std::string::npos);
    CHECK(header.find("RuntimeOwnerProducerRequestKind") == std::string::npos);
    CHECK(header.find("RuntimeOwnerUrgentMessage") == std::string::npos);
    CHECK(header.find("NormalIntent") == std::string::npos);
    CHECK(header.find("QueueHandle_t") == std::string::npos);
    CHECK(header.find("RawModemCommand") == std::string::npos);
    CHECK(rtos_header.find("runtime_owner_try_submit_normal") ==
          std::string::npos);
    CHECK(rtos_header.find("runtime_owner_try_request_transport") ==
          std::string::npos);
}

} // namespace

int main()
{
    test_boot_and_periodic_control_routes();
    test_periodic_normal_routes();
    test_battery_grace_blocks_only_periodic_work();
    test_sensor_alarm_routes_freeze_edge_revision_and_value();
    test_normal_facade_propagates_admission_failure();
    test_sensor_reader_freezes_exact_alarm_offer_before_marking_inflight();
    test_adapter_monitor_power_event_routes();
    test_exact_shutdown_sources_and_invalid_identity();
    test_public_surface_is_nonforgeable();
    if (g_failures != 0) {
        std::fprintf(stderr,
                     "runtime_owner_producer_facade_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf(
        "runtime_owner_producer_facade_contract_test: %zu checks passed\n",
        g_checks);
    return 0;
}
