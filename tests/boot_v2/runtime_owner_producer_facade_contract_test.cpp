#include "runtime_owner_producer_facade.hpp"
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

RuntimeOwnerIngressResult submit_control(
    const RuntimeOwnerControlMessage value) noexcept
{
    g_lane = RuntimeOwnerRtosLane::Control;
    g_control = value;
    ++g_calls;
    return RuntimeOwnerIngressResult::AcceptedForDelivery;
}

RuntimeOwnerIngressResult submit_normal(const NormalIntent value) noexcept
{
    g_lane = RuntimeOwnerRtosLane::Normal;
    g_normal = value;
    ++g_calls;
    return RuntimeOwnerIngressResult::AcceptedForDelivery;
}

RuntimeOwnerIngressResult submit_urgent(
    const RuntimeOwnerUrgentMessage value) noexcept
{
    g_lane = RuntimeOwnerRtosLane::Urgent;
    g_urgent = value;
    ++g_calls;
    return RuntimeOwnerIngressResult::AcceptedForDelivery;
}

void reset() noexcept
{
    g_lane = RuntimeOwnerRtosLane::None;
    g_control = {};
    g_normal = {};
    g_urgent = {};
    g_calls = 0;
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
    CHECK(runtime_owner_periodic_publish_telemetry(2, 91) ==
          RuntimeOwnerIngressResult::AcceptedForDelivery);
    CHECK(runtime_owner_rtos_detail::g_calls == 1);
    CHECK(runtime_owner_rtos_detail::g_normal.kind ==
          NormalIntentKind::PublishTelemetry);
    CHECK(runtime_owner_rtos_detail::g_normal.subject_id == 2);
    CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 91);

    runtime_owner_rtos_detail::reset();
    CHECK(runtime_owner_periodic_publish_telemetry(0, 91) ==
          RuntimeOwnerIngressResult::RejectedInvalid);
    CHECK(runtime_owner_rtos_detail::g_calls == 0);
    CHECK(runtime_owner_periodic_publish_telemetry(2, 0) ==
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
        CHECK(runtime_owner_rtos_detail::g_normal.subject_id == 0);
        CHECK(runtime_owner_rtos_detail::g_normal.snapshot_revision == 0);
    }
}

void test_exact_shutdown_sources_and_invalid_identity() noexcept
{
    struct Case {
        RuntimeOwnerIngressResult (*call)(std::uint32_t, std::uint32_t) noexcept;
        RuntimeOwnerUrgentSource expected;
    };
    constexpr Case cases[] = {
        {runtime_owner_power_button_request_shutdown,
         RuntimeOwnerUrgentSource::PowerButton},
        {runtime_owner_adapter_loss_request_shutdown,
         RuntimeOwnerUrgentSource::AdapterLossCommitted},
        {runtime_owner_authenticated_request_shutdown,
         RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand},
    };
    for (const Case item : cases) {
        runtime_owner_rtos_detail::reset();
        CHECK(item.call(7, 11) ==
              RuntimeOwnerIngressResult::AcceptedForDelivery);
        CHECK(runtime_owner_rtos_detail::g_calls == 1);
        CHECK(runtime_owner_rtos_detail::g_lane == RuntimeOwnerRtosLane::Urgent);
        CHECK(runtime_owner_rtos_detail::g_urgent.source == item.expected);
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
