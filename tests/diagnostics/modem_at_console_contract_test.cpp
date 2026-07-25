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

std::string section(
    const std::string &source,
    const char *begin,
    const char *end)
{
    const std::size_t first = source.find(begin);
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = source.find(end, first + 1);
    if (last == std::string::npos) {
        return {};
    }
    return source.substr(first, last - first);
}

void test_modem_at_console_target_is_independent() noexcept
{
    const std::string cmake = read_file(NB_IOT_SOURCE_ROOT "/CMakeLists.txt");
    const std::string target = section(
        cmake,
        "add_executable(modem_at_console",
        "pico_add_extra_outputs(modem_at_console)");

    CHECK(!cmake.empty());
    CHECK(!target.empty());
    CHECK(target.find("src/diagnostics/modem_at_console.cpp") !=
          std::string::npos);
    CHECK(target.find("pico_stdlib") != std::string::npos);
    CHECK(target.find("pico_stdio_usb") != std::string::npos);
    CHECK(target.find("hardware_uart") != std::string::npos);
    CHECK(target.find("FreeRTOS-Kernel") == std::string::npos);
    CHECK(target.find("nb_iot_project") == std::string::npos);
    CHECK(target.find("pico_enable_stdio_usb(modem_at_console 1)") !=
          std::string::npos);
    CHECK(target.find("pico_enable_stdio_uart(modem_at_console 0)") !=
          std::string::npos);
}

void test_modem_at_console_keeps_only_modem_and_usb_paths() noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/diagnostics/modem_at_console.cpp");

    CHECK(!source.empty());
    CHECK(source.find("#include \"../config.h\"") != std::string::npos);
    CHECK(source.find("#include <cstdint>") != std::string::npos);
    CHECK(source.find("FreeRTOS") == std::string::npos);
    CHECK(source.find("RuntimeOwner") == std::string::npos);
    CHECK(source.find("MQTT") == std::string::npos);
    CHECK(source.find("DS18B20") == std::string::npos);
    CHECK(source.find("uart_init(UART_ID, BAUD_RATE)") !=
          std::string::npos);
    CHECK(source.find("gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART)") !=
          std::string::npos);
    CHECK(source.find("gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART)") !=
          std::string::npos);
    CHECK(source.find("uart_set_hw_flow(UART_ID, false, false)") !=
          std::string::npos);
    CHECK(source.find("gpio_put(PWR_ON_PIN, 1)") != std::string::npos);
    CHECK(source.find("gpio_put(PWR_ON_PIN, 0)") != std::string::npos);
}

void test_modem_at_console_initializes_and_then_accepts_manual_at() noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/diagnostics/modem_at_console.cpp");

    const std::size_t initialize = source.find("run_initialization()");
    const std::size_t manual = source.find("run_manual_console()");
    const std::size_t flow_none = source.find("\"AT&K0\"");
    const std::size_t interface_flow_none =
        source.find("\"AT+IFC=0,0\"");
    CHECK(source.find("AT+CPIN?") != std::string::npos);
    CHECK(source.find("AT+CGSN") != std::string::npos);
    CHECK(source.find("AT+CIMI") != std::string::npos);
    CHECK(source.find("AT+KCERTDELETE=0,0") != std::string::npos);
    CHECK(source.find("AT+KCERTSTORE=0,%u,0") != std::string::npos);
    CHECK(source.find("CERTIFICATE REDACTED") != std::string::npos);
    CHECK(source.find("kCertificateChunkBytes = 200") !=
          std::string::npos);
    CHECK(source.find("sleep_us(500)") != std::string::npos);
    CHECK(source.find("MANUAL_AT_READY") != std::string::npos);
    CHECK(source.find("char command[512]{}") != std::string::npos);
    CHECK(source.find("command_has_at_prefix") != std::string::npos);
    CHECK(source.find("uart_putc_raw(UART_ID, '\\r')") !=
          std::string::npos);
    CHECK(source.find("kReadGuardBytes = 256") != std::string::npos);
    CHECK(flow_none != std::string::npos);
    CHECK(interface_flow_none != std::string::npos);
    CHECK(flow_none < interface_flow_none);
    CHECK(initialize != std::string::npos);
    CHECK(manual != std::string::npos);
    CHECK(initialize < manual);
}

} // namespace

int main()
{
    test_modem_at_console_target_is_independent();
    test_modem_at_console_keeps_only_modem_and_usb_paths();
    test_modem_at_console_initializes_and_then_accepts_manual_at();

    if (g_failures != 0) {
        std::fprintf(stderr,
                     "modem_at_console_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("modem_at_console_contract_test: %zu checks passed\n",
                g_checks);
    return 0;
}
