#include <array>
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

std::string read_file(const char *path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void test_legacy_producers_are_facade_only() noexcept
{
    constexpr std::array<const char *, 3> files{{
        "/src/tasks/tasks_boot.cpp",
        "/src/tasks/tasks_periodic_modem.cpp",
        "/src/tasks/tasks_debug.cpp",
    }};
    constexpr std::array<const char *, 11> forbidden{{
        "#include \"tasks_modem.hpp\"",
        "modem.modem_",
        "modem.check_",
        "modem.get_",
        "modem.is_",
        "safe_reboot(",
        "safe_power_off(",
        "modem_ReadResponse",
        "modem_PacedWrite",
        "modem_Mqtt",
        "get_rx_buffer",
    }};
    for (const char *relative : files) {
        const std::string source =
            read_file((std::string(NB_IOT_SOURCE_ROOT) + relative).c_str());
        CHECK(!source.empty());
        for (const char *needle : forbidden) {
            CHECK(source.find(needle) == std::string::npos);
        }
    }

    const std::string boot = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_boot.cpp");
    const std::string periodic = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_periodic_modem.cpp");
    const std::string debug = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_debug.cpp");
    CHECK(boot.find("runtime_owner_boot_request_transport") !=
          std::string::npos);
    CHECK(periodic.find("runtime_owner_periodic_request_transport") !=
          std::string::npos);
    CHECK(periodic.find("runtime_owner_periodic_publish_telemetry") !=
          std::string::npos);
    CHECK(periodic.find("runtime_owner_periodic_refresh_rssi") !=
          std::string::npos);
    const std::string config_pull =
        "runtime_owner_periodic_pull_config()";
    const std::size_t config_pull_position =
        periodic.find(config_pull);
    CHECK(config_pull_position != std::string::npos);
    CHECK(config_pull_position != std::string::npos &&
          periodic.find(
              config_pull,
              config_pull_position + config_pull.size()) ==
              std::string::npos);
    CHECK(periodic.find("runtime_owner_periodic_pull_command()") ==
          std::string::npos);
    CHECK(debug.find("DEBUG_DISABLED") != std::string::npos);
}

void test_public_context_no_longer_exposes_modem_or_command_side_effect() noexcept
{
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/app_context.hpp");
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/app_context.cpp");
    const std::string backend = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    CHECK(header.find("extern nb_iot modem") == std::string::npos);
    CHECK(source.find("if (allow_commands && cmd == 10)") ==
          std::string::npos);
    CHECK(source.find("safe_reboot(100, cmd_id") == std::string::npos);
    CHECK(backend.find("extern nb_iot modem;") != std::string::npos);
    CHECK(backend.find("runtime_owner_authenticated_request_shutdown") ==
          std::string::npos);
}

} // namespace

int main()
{
    test_legacy_producers_are_facade_only();
    test_public_context_no_longer_exposes_modem_or_command_side_effect();
    if (g_failures != 0) {
        std::fprintf(stderr,
                     "runtime_owner_legacy_cutover_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf(
        "runtime_owner_legacy_cutover_contract_test: %zu checks passed\n",
        g_checks);
    return 0;
}
