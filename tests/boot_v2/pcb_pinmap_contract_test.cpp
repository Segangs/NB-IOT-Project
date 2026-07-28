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

void test_config_matches_final_pcb_gpio_map() noexcept
{
    const std::string config =
        read_file(NB_IOT_SOURCE_ROOT "/src/config.h");
    const std::string power_task =
        read_file(NB_IOT_SOURCE_ROOT "/src/tasks/tasks_led.cpp");
    const char *const required[] = {
        "#define UART_TX_PIN       0",
        "#define UART_RX_PIN       1",
        "#define MODEM_WAKEUP_PIN  2",
        "#define MODEM_RESET_PIN   3",
        "#define PWR_ON_PIN        4",
        "#define MODEM_TXON_INPUT_PIN            5",
        "#define BUZZER_PIN                      6",
        "#define POWER_ADAPTER_DETECT_PIN         7",
        "#define STATUS_LED_RED_PIN              8",
        "#define STATUS_LED_GREEN_PIN            9",
        "#define RJ45_PORT1_TEMP_LED_PIN         10",
        "#define RJ45_PORT1_MIC_LED_PIN          11",
        "#define RJ45_PORT2_TEMP_LED_PIN         12",
        "#define RJ45_PORT2_MIC_LED_PIN          13",
        "#define POWER_INT_PIN                    14",
        "#define POWER_KILL_PIN                   15",
        "#define SDA_PIN           16",
        "#define SCL_PIN           17",
        "#define MIC_I2S_BCLK_PIN  18",
        "#define MIC_I2S_LRCLK_PIN 19",
        "#define MIC1_DOUT_PIN     20",
        "#define MIC2_DOUT_PIN     21",
        "#define TEMP1_SENSOR_PIN  22",
        "#define TEMP2_SENSOR_PIN  26",
        "#define TXON_LED_PIN                    28",
    };
    for (const char *const definition : required) {
        CHECK(config.find(definition) != std::string::npos);
    }

    CHECK(config.find("#define PWR_ON_PIN        15") ==
          std::string::npos);
    CHECK(config.find("#define POWER_KILL_PIN                   4") ==
          std::string::npos);
    CHECK(config.find("#define SDA_PIN           20") == std::string::npos);
    CHECK(config.find("#define SCL_PIN           21") == std::string::npos);

    CHECK(config.find("#define POWER_ADAPTER_DEBOUNCE_MS 1000") !=
          std::string::npos);
    CHECK(config.find("#define POWER_ADAPTER_SHUTDOWN_COMMIT_MS 210000") !=
          std::string::npos);
    CHECK(config.find("#define POWER_ADAPTER_ABSOLUTE_OFF_MS 300000") !=
          std::string::npos);

    CHECK(power_task.find(
              "gpio_disable_pulls(POWER_ADAPTER_DETECT_PIN)") !=
          std::string::npos);
    const std::size_t probe_function =
        power_task.find("PowerAdapterProbeSample probe_power_adapter_input");
    const std::size_t probe_pull_down =
        power_task.find("gpio_pull_down(POWER_ADAPTER_DETECT_PIN)",
                        probe_function);
    const std::size_t probe_pull_restore =
        power_task.find("gpio_disable_pulls(POWER_ADAPTER_DETECT_PIN)",
                        probe_pull_down);
    CHECK(probe_function != std::string::npos);
    CHECK(probe_pull_down != std::string::npos);
    CHECK(probe_pull_restore != std::string::npos);
    CHECK(probe_function < probe_pull_down);
    CHECK(probe_pull_down < probe_pull_restore);
    CHECK(power_task.find("adapter_present = probe.floating_high") !=
          std::string::npos);
    CHECK(power_task.find("POWER_ADAPTER_PROBE FLOAT=") ==
          std::string::npos);
    CHECK(power_task.find("POWER_ADAPTER_EDGE FALL=") ==
          std::string::npos);
    CHECK(power_task.find("PowerStateCore") != std::string::npos);
    CHECK(power_task.find(
              "runtime_owner_power_publish_adapter_removed") !=
          std::string::npos);
    CHECK(power_task.find(
              "runtime_owner_power_publish_adapter_restored") !=
          std::string::npos);
    CHECK(power_task.find(
              "runtime_owner_adapter_loss_request_shutdown") !=
          std::string::npos);
    CHECK(power_task.find("lcd_params.is_battery_mode = false") ==
          std::string::npos);
    CHECK(power_task.find("gpio_put(POWER_KILL_PIN") == std::string::npos);
}

} // namespace

int main()
{
    test_config_matches_final_pcb_gpio_map();
    if (g_failures != 0) {
        std::fprintf(stderr, "pcb_pinmap_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("pcb_pinmap_contract_test: %zu checks passed\n", g_checks);
    return 0;
}
