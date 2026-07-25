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

std::size_t count(const std::string &source, const std::string &needle)
{
    std::size_t result = 0;
    std::size_t offset = 0;
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++result;
        offset += needle.size();
    }
    return result;
}

void test_pcb_pinmap_and_startup_config() noexcept
{
    const std::string config =
        read_file(NB_IOT_SOURCE_ROOT "/src/config.h");
    CHECK(config.find("#define LCD_ADDR          0x27") !=
          std::string::npos);
    CHECK(config.find("#define LCD_ADDR_ALT      0x3F") !=
          std::string::npos);
    CHECK(config.find("#define SDA_PIN           16") != std::string::npos);
    CHECK(config.find("#define SCL_PIN           17") != std::string::npos);
    CHECK(config.find("#define SDA_PIN           20") == std::string::npos);
    CHECK(config.find("#define SCL_PIN           21") == std::string::npos);
    CHECK(config.find("#define LCD_POWER_STABILIZE_DELAY_MS 5000") !=
          std::string::npos);
}

void test_lcd_task_owns_delayed_direct_initialization() noexcept
{
    const std::string main_source =
        read_file(NB_IOT_SOURCE_ROOT "/main.cpp");
    const std::string task_source =
        read_file(NB_IOT_SOURCE_ROOT "/src/tasks/tasks_lcd.cpp");
    const std::size_t delay = task_source.find(
        "vTaskDelay(pdMS_TO_TICKS(LCD_POWER_STABILIZE_DELAY_MS))");
    const std::size_t init_start =
        task_source.find("LOG(\"LCD_INIT_START 0x%02X\\n\", lcd_addr)");
    const std::size_t construct =
        task_source.find("static LCD_I2C lcd_device(");

    CHECK(main_source.find("sleep_ms(LCD_POWER_STABILIZE_DELAY_MS)") ==
          std::string::npos);
    CHECK(main_source.find("static LCD_I2C lcd(") == std::string::npos);
    CHECK(count(task_source,
                "vTaskDelay(pdMS_TO_TICKS("
                "LCD_POWER_STABILIZE_DELAY_MS))") == 1);
    CHECK(task_source.find("lcd_i2c_probe(") == std::string::npos);
    CHECK(task_source.find("LCD_SCAN_") == std::string::npos);
    CHECK(task_source.find("const uint8_t lcd_addr = LCD_ADDR;") !=
          std::string::npos);
    CHECK(task_source.find("LOG(\"LCD_INIT_DONE 0x%02X\\n\", lcd_addr)") !=
          std::string::npos);
    CHECK(delay != std::string::npos);
    CHECK(init_start != std::string::npos);
    CHECK(construct != std::string::npos);
    CHECK(delay < init_start);
    CHECK(init_start < construct);
}

void test_driver_matches_verified_five_second_backpack_sequence() noexcept
{
    const std::string driver =
        read_file(NB_IOT_SOURCE_ROOT "/lib/LCD_I2C.cpp");

    CHECK(driver.find("BAUD_RATE = 50'000") != std::string::npos);
    CHECK(driver.find("i2c_write_timeout_us(") != std::string::npos);
    CHECK(driver.find("false, 3000") != std::string::npos);
    CHECK(driver.find("DELAY = 1000") != std::string::npos);
    CHECK(driver.find("high = (val >> 4) & 0x0F") != std::string::npos);
    CHECK(driver.find("low = val & 0x0F") != std::string::npos);
    CHECK(count(driver, "Send_Nibble(0x03, COMMAND)") == 3);
    CHECK(count(driver, "Send_Nibble(0x02, COMMAND)") == 1);
    CHECK(driver.find("Send_Command(0x03)") == std::string::npos);
}

} // namespace

int main()
{
    test_pcb_pinmap_and_startup_config();
    test_lcd_task_owns_delayed_direct_initialization();
    test_driver_matches_verified_five_second_backpack_sequence();
    if (g_failures != 0) {
        std::fprintf(stderr, "lcd_runtime_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("lcd_runtime_contract_test: %zu checks passed\n", g_checks);
    return 0;
}
