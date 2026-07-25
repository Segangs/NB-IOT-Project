#include "runtime_owner_producer_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>

namespace {

using boot_v2::NormalIntentKind;
using boot_v2::RuntimeOwnerControlKind;
using boot_v2::RuntimeOwnerProducerAuthorizationResult;
using boot_v2::RuntimeOwnerProducerDecision;
using boot_v2::RuntimeOwnerProducerKind;
using boot_v2::RuntimeOwnerProducerRequestKind;
using boot_v2::RuntimeOwnerRtosLane;
using boot_v2::RuntimeOwnerUrgentSource;
using boot_v2::runtime_owner_authorize_producer_request;

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

constexpr bool reserved_is_zero(
    const RuntimeOwnerProducerDecision decision) noexcept
{
    return decision.reserved[0] == 0 && decision.reserved[1] == 0 &&
           decision.reserved[2] == 0;
}

constexpr bool route_is_zero(
    const RuntimeOwnerProducerDecision decision) noexcept
{
    return decision.lane == RuntimeOwnerRtosLane::None &&
           decision.urgent_source == RuntimeOwnerUrgentSource::Invalid &&
           decision.control_kind == RuntimeOwnerControlKind::Invalid &&
           decision.normal_kind == NormalIntentKind::Invalid &&
           reserved_is_zero(decision);
}

constexpr bool decisions_equal(
    const RuntimeOwnerProducerDecision left,
    const RuntimeOwnerProducerDecision right) noexcept
{
    return left.result == right.result && left.lane == right.lane &&
           left.urgent_source == right.urgent_source &&
           left.control_kind == right.control_kind &&
           left.normal_kind == right.normal_kind &&
           reserved_is_zero(left) && reserved_is_zero(right);
}

struct AuthorizedCase {
    RuntimeOwnerProducerKind producer{RuntimeOwnerProducerKind::Invalid};
    RuntimeOwnerProducerRequestKind request{
        RuntimeOwnerProducerRequestKind::Invalid};
    RuntimeOwnerProducerDecision expected{};
};

constexpr std::array<AuthorizedCase, 9> kAuthorizedCases{{
    {RuntimeOwnerProducerKind::Boot,
     RuntimeOwnerProducerRequestKind::RequestTransportAttempt,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Control,
      RuntimeOwnerUrgentSource::Invalid,
      RuntimeOwnerControlKind::RequestTransportAttempt,
      NormalIntentKind::Invalid,
      {}}},
    {RuntimeOwnerProducerKind::Periodic,
     RuntimeOwnerProducerRequestKind::RequestTransportAttempt,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Control,
      RuntimeOwnerUrgentSource::Invalid,
      RuntimeOwnerControlKind::RequestTransportAttempt,
      NormalIntentKind::Invalid,
      {}}},
    {RuntimeOwnerProducerKind::Periodic,
     RuntimeOwnerProducerRequestKind::PublishTelemetry,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Normal,
      RuntimeOwnerUrgentSource::Invalid,
      RuntimeOwnerControlKind::Invalid,
      NormalIntentKind::PublishTelemetry,
      {}}},
    {RuntimeOwnerProducerKind::Periodic,
     RuntimeOwnerProducerRequestKind::RefreshRssi,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Normal,
      RuntimeOwnerUrgentSource::Invalid,
      RuntimeOwnerControlKind::Invalid,
      NormalIntentKind::RefreshRssi,
      {}}},
    {RuntimeOwnerProducerKind::Periodic,
     RuntimeOwnerProducerRequestKind::PullConfig,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Normal,
      RuntimeOwnerUrgentSource::Invalid,
      RuntimeOwnerControlKind::Invalid,
      NormalIntentKind::PullConfig,
      {}}},
    {RuntimeOwnerProducerKind::Periodic,
     RuntimeOwnerProducerRequestKind::PullCommand,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Normal,
      RuntimeOwnerUrgentSource::Invalid,
      RuntimeOwnerControlKind::Invalid,
      NormalIntentKind::PullCommand,
      {}}},
    {RuntimeOwnerProducerKind::PowerButton,
     RuntimeOwnerProducerRequestKind::RequestShutdown,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Urgent,
      RuntimeOwnerUrgentSource::PowerButton,
      RuntimeOwnerControlKind::Invalid,
      NormalIntentKind::Invalid,
      {}}},
    {RuntimeOwnerProducerKind::AdapterMonitor,
     RuntimeOwnerProducerRequestKind::RequestShutdown,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Urgent,
      RuntimeOwnerUrgentSource::AdapterLossCommitted,
      RuntimeOwnerControlKind::Invalid,
      NormalIntentKind::Invalid,
      {}}},
    {RuntimeOwnerProducerKind::AuthenticatedCommand,
     RuntimeOwnerProducerRequestKind::RequestShutdown,
     {RuntimeOwnerProducerAuthorizationResult::Authorized,
      RuntimeOwnerRtosLane::Urgent,
      RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand,
      RuntimeOwnerControlKind::Invalid,
      NormalIntentKind::Invalid,
      {}}},
}};

constexpr bool is_authorized_pair(
    const RuntimeOwnerProducerKind producer,
    const RuntimeOwnerProducerRequestKind request) noexcept
{
    for (const AuthorizedCase &entry : kAuthorizedCases) {
        if (entry.producer == producer && entry.request == request) {
            return true;
        }
    }
    return false;
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

void test_numeric_layout_and_default_contracts()
{
    CHECK((std::is_same<
           std::underlying_type<RuntimeOwnerProducerKind>::type,
           std::uint8_t>::value));
    CHECK((std::is_same<
           std::underlying_type<RuntimeOwnerProducerRequestKind>::type,
           std::uint8_t>::value));
    CHECK((std::is_same<
           std::underlying_type<RuntimeOwnerProducerAuthorizationResult>::type,
           std::uint8_t>::value));
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerKind::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerKind::Boot) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerKind::Periodic) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerKind::PowerButton) == 3);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerKind::AdapterMonitor) == 4);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerKind::AuthenticatedCommand) == 5);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerKind::LocalDebug) == 6);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::RequestTransportAttempt) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::PublishTelemetry) == 2);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::RefreshRssi) == 3);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::PullConfig) == 4);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::PullCommand) == 5);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::RequestShutdown) == 6);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerRequestKind::RawModemCommand) == 7);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerAuthorizationResult::RejectedInvalid) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerAuthorizationResult::RejectedPermission) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerProducerAuthorizationResult::Authorized) == 2);
    CHECK(sizeof(RuntimeOwnerProducerDecision) == 8);
    CHECK(alignof(RuntimeOwnerProducerDecision) == 1);
    CHECK((std::is_standard_layout<RuntimeOwnerProducerDecision>::value));
    CHECK((std::is_trivially_copyable<RuntimeOwnerProducerDecision>::value));
    CHECK(!std::is_pointer<decltype(RuntimeOwnerProducerDecision::result)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerProducerDecision::result)>::value);
    CHECK(!std::is_pointer<decltype(RuntimeOwnerProducerDecision::lane)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerProducerDecision::lane)>::value);
    CHECK(!std::is_pointer<decltype(RuntimeOwnerProducerDecision::urgent_source)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerProducerDecision::urgent_source)>::value);
    CHECK(!std::is_pointer<decltype(RuntimeOwnerProducerDecision::control_kind)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerProducerDecision::control_kind)>::value);
    CHECK(!std::is_pointer<decltype(RuntimeOwnerProducerDecision::normal_kind)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerProducerDecision::normal_kind)>::value);
    CHECK(!std::is_pointer<decltype(RuntimeOwnerProducerDecision::reserved)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerProducerDecision::reserved)>::value);
    const RuntimeOwnerProducerDecision decision{};
    CHECK(decision.result ==
          RuntimeOwnerProducerAuthorizationResult::RejectedInvalid);
    CHECK(route_is_zero(decision));
}

void test_authorized_table_maps_exactly()
{
    for (const AuthorizedCase &entry : kAuthorizedCases) {
        CHECK(decisions_equal(
            runtime_owner_authorize_producer_request(
                entry.producer, entry.request),
            entry.expected));
    }
}

void test_all_other_recognized_pairs_reject_permission()
{
    std::size_t authorized_seen = 0;
    for (std::uint8_t producer_value = 1; producer_value <= 6;
         ++producer_value) {
        for (std::uint8_t request_value = 1; request_value <= 7;
             ++request_value) {
            const auto producer =
                static_cast<RuntimeOwnerProducerKind>(producer_value);
            const auto request =
                static_cast<RuntimeOwnerProducerRequestKind>(request_value);
            if (is_authorized_pair(producer, request)) {
                ++authorized_seen;
                continue;
            }
            const RuntimeOwnerProducerDecision decision =
                runtime_owner_authorize_producer_request(producer, request);
            CHECK(decision.result ==
                  RuntimeOwnerProducerAuthorizationResult::RejectedPermission);
            CHECK(route_is_zero(decision));
        }
    }
    CHECK(authorized_seen == kAuthorizedCases.size());
}

void test_invalid_and_unknown_values_fail_closed()
{
    constexpr std::array<RuntimeOwnerProducerKind, 2> invalid_producers{{
        RuntimeOwnerProducerKind::Invalid,
        static_cast<RuntimeOwnerProducerKind>(0xff),
    }};
    constexpr std::array<RuntimeOwnerProducerRequestKind, 9> all_requests{{
        RuntimeOwnerProducerRequestKind::Invalid,
        RuntimeOwnerProducerRequestKind::RequestTransportAttempt,
        RuntimeOwnerProducerRequestKind::PublishTelemetry,
        RuntimeOwnerProducerRequestKind::RefreshRssi,
        RuntimeOwnerProducerRequestKind::PullConfig,
        RuntimeOwnerProducerRequestKind::PullCommand,
        RuntimeOwnerProducerRequestKind::RequestShutdown,
        RuntimeOwnerProducerRequestKind::RawModemCommand,
        static_cast<RuntimeOwnerProducerRequestKind>(0xff),
    }};
    for (const RuntimeOwnerProducerKind producer : invalid_producers) {
        for (const RuntimeOwnerProducerRequestKind request : all_requests) {
            const RuntimeOwnerProducerDecision decision =
                runtime_owner_authorize_producer_request(producer, request);
            CHECK(decision.result ==
                  RuntimeOwnerProducerAuthorizationResult::RejectedInvalid);
            CHECK(route_is_zero(decision));
        }
    }

    constexpr std::array<RuntimeOwnerProducerRequestKind, 2>
        invalid_requests{{
            RuntimeOwnerProducerRequestKind::Invalid,
            static_cast<RuntimeOwnerProducerRequestKind>(0xff),
        }};
    for (std::uint8_t producer_value = 1; producer_value <= 6;
         ++producer_value) {
        const auto producer =
            static_cast<RuntimeOwnerProducerKind>(producer_value);
        for (const RuntimeOwnerProducerRequestKind request : invalid_requests) {
            const RuntimeOwnerProducerDecision decision =
                runtime_owner_authorize_producer_request(producer, request);
            CHECK(decision.result ==
                  RuntimeOwnerProducerAuthorizationResult::RejectedInvalid);
            CHECK(route_is_zero(decision));
        }
    }
}

void test_local_debug_and_raw_modem_are_denied()
{
    for (std::uint8_t request_value = 1; request_value <= 7;
         ++request_value) {
        const RuntimeOwnerProducerDecision decision =
            runtime_owner_authorize_producer_request(
                RuntimeOwnerProducerKind::LocalDebug,
                static_cast<RuntimeOwnerProducerRequestKind>(request_value));
        CHECK(decision.result ==
              RuntimeOwnerProducerAuthorizationResult::RejectedPermission);
        CHECK(route_is_zero(decision));
    }
    for (std::uint8_t producer_value = 1; producer_value <= 6;
         ++producer_value) {
        const RuntimeOwnerProducerDecision decision =
            runtime_owner_authorize_producer_request(
                static_cast<RuntimeOwnerProducerKind>(producer_value),
                RuntimeOwnerProducerRequestKind::RawModemCommand);
        CHECK(decision.result ==
              RuntimeOwnerProducerAuthorizationResult::RejectedPermission);
        CHECK(route_is_zero(decision));
    }
}

void test_internal_boundary_and_cutover_stop_line()
{
    const std::string contract = read_source(
        NB_IOT_SOURCE_ROOT
        "/src/boot_v2/runtime_owner_producer_contract.hpp");
    CHECK(!contract.empty());
    CHECK(count_occurrences(
              contract, "runtime_owner_authorize_producer_request(") == 1);
    constexpr std::array<const char *, 14> forbidden_dependencies{{
        "FreeRTOS.h", "queue.h", "task.h", "QueueHandle_t",
        "TaskHandle_t", "modem", "mqtt", "uart", "gpio", "flash",
        "malloc", "operator new", "throw", "catch",
    }};
    for (const char *const forbidden : forbidden_dependencies) {
        CHECK(contract.find(forbidden) == std::string::npos);
    }

    const std::string public_header = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.hpp");
    CHECK(!public_header.empty());
    constexpr std::array<const char *, 4> forbidden_public{{
        "RuntimeOwnerProducerKind",
        "RuntimeOwnerProducerRequestKind",
        "runtime_owner_authorize_producer_request",
        "RawModemCommand",
    }};
    for (const char *const forbidden : forbidden_public) {
        CHECK(public_header.find(forbidden) == std::string::npos);
    }

    const std::string root_cmake =
        read_source(NB_IOT_SOURCE_ROOT "/CMakeLists.txt");
    CHECK(!root_cmake.empty());
    CHECK(root_cmake.find("runtime_owner_producer_contract") ==
          std::string::npos);

    constexpr std::array<const char *, 15> production_sources{{
        "/src/lib/flash_logger.cpp",
        "/src/lib/log.cpp",
        "/src/lib/mqtt_payload.cpp",
        "/src/tasks/app_context.cpp",
        "/src/tasks/tasks_boot.cpp",
        "/src/tasks/tasks_buzzer.cpp",
        "/src/tasks/tasks_debug.cpp",
        "/src/tasks/tasks_lcd.cpp",
        "/src/tasks/tasks_led.cpp",
        "/src/tasks/tasks_modem.cpp",
        "/src/tasks/tasks_mqtt.cpp",
        "/src/tasks/tasks_periodic_modem.cpp",
        "/src/tasks/tasks_sensor.cpp",
        "/src/tasks/tasks_sensor_reader.cpp",
        "/main.cpp",
    }};
    for (const char *const relative : production_sources) {
        const std::string source =
            read_source((std::string(NB_IOT_SOURCE_ROOT) + relative).c_str());
        CHECK(!source.empty());
        CHECK(source.find("runtime_owner_try_submit_normal(") ==
                  std::string::npos &&
              source.find("runtime_owner_try_request_transport(") ==
                  std::string::npos &&
              source.find(".executor_port(") == std::string::npos &&
              source.find("RuntimeOwnerExecutorPort") ==
                  std::string::npos);
    }

    const std::string rtos_source = read_source(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    CHECK(!rtos_source.empty());
    CHECK(rtos_source.find("g_status.ingress_enabled = 1") !=
          std::string::npos);
    CHECK(rtos_source.find("g_status.ingress_enabled = 0") !=
          std::string::npos);
    CHECK(rtos_source.find("g_drain_metrics.receiver_ready = 1") !=
          std::string::npos);
    CHECK(rtos_source.find("g_drain_metrics.cutover_ready = 0") !=
          std::string::npos);
    CHECK(rtos_source.find("g_drain_metrics.cutover_ready = 1") !=
          std::string::npos);
    CHECK(rtos_source.find("g_cutover_coordinator.activate(") !=
          std::string::npos);
}

void test_gp14_power_button_producer_is_debounced_and_runtime_ready_guarded()
{
    const std::string led = read_source(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_led.cpp");
    CHECK(!led.empty());
    CHECK(led.find("gpio_init(POWER_INT_PIN)") != std::string::npos);
    CHECK(led.find("gpio_set_dir(POWER_INT_PIN, GPIO_IN)") !=
          std::string::npos);
    CHECK(led.find("gpio_pull_up(POWER_INT_PIN)") !=
          std::string::npos);
    CHECK(led.find("POWER_INT_DEBOUNCE_MS") != std::string::npos);
    CHECK(led.find("gpio_get(POWER_INT_PIN) == 0") !=
          std::string::npos);
    CHECK(led.find("stdio_usb_connected()") == std::string::npos);
    CHECK(led.find(
              "runtime_owner_redacted_status().runtime_ready != 0") !=
          std::string::npos);
    CHECK(led.find("runtime_owner_power_button_request_shutdown(") !=
          std::string::npos);
    CHECK(led.find(
              "RuntimeOwnerIngressResult::AcceptedForDelivery") !=
          std::string::npos);
    CHECK(led.find("power_shutdown_latched = true") !=
          std::string::npos);
    CHECK(led.find("gpio_put(POWER_KILL_PIN") == std::string::npos);
}

}  // namespace

int main()
{
    test_numeric_layout_and_default_contracts();
    test_authorized_table_maps_exactly();
    test_all_other_recognized_pairs_reject_permission();
    test_invalid_and_unknown_values_fail_closed();
    test_local_debug_and_raw_modem_are_denied();
    test_internal_boundary_and_cutover_stop_line();
    test_gp14_power_button_producer_is_debounced_and_runtime_ready_guarded();

    if (failures != 0) {
        std::printf(
            "runtime_owner_producer_contract_test: %d/%d checks failed\n",
            failures,
            checks);
        return 1;
    }
    std::printf(
        "runtime_owner_producer_contract_test: %d checks passed\n",
        checks);
    return 0;
}
