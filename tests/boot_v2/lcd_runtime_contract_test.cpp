#include <array>
#include <cctype>
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

std::string replace_once_copy(
    const std::string &source,
    const std::string &from,
    const std::string &to)
{
    std::string result = source;
    const std::size_t position = result.find(from);
    if (position != std::string::npos) {
        result.replace(position, from.size(), to);
    }
    return result;
}

struct SourceSpan {
    std::size_t begin{std::string::npos};
    std::size_t end{std::string::npos};

    [[nodiscard]] bool valid() const noexcept
    {
        return begin != std::string::npos &&
               end != std::string::npos &&
               begin < end;
    }
};

SourceSpan braced_span(
    const std::string &source,
    const std::size_t opening)
{
    if (opening == std::string::npos ||
        opening >= source.size() ||
        source[opening] != '{') {
        return {};
    }
    std::size_t depth = 0;
    for (std::size_t position = opening; position < source.size();
         ++position) {
        if (source[position] == '{') {
            ++depth;
        } else if (source[position] == '}') {
            --depth;
            if (depth == 0) {
                return {opening, position + 1};
            }
        }
    }
    return {};
}

std::string function_definition(
    const std::string &source,
    const std::string &signature)
{
    const std::size_t begin = source.find(signature);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t opening =
        source.find('{', begin + signature.size());
    const SourceSpan body = braced_span(source, opening);
    return body.valid()
               ? source.substr(begin, body.end - begin)
               : std::string{};
}

std::string without_whitespace(const std::string &source)
{
    std::string result;
    result.reserve(source.size());
    for (const unsigned char character : source) {
        if (std::isspace(character) == 0) {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

std::size_t brace_depth_at(
    const std::string &source,
    const std::size_t position)
{
    std::size_t depth = 0;
    for (std::size_t index = 0;
         index < position && index < source.size();
         ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            --depth;
        }
    }
    return depth;
}

bool sensor_display_state_contract_accepts(const std::string &sensor)
{
    const std::string task = without_whitespace(function_definition(
        sensor, "void vSensorTask(void *pvParameters)"));
    constexpr const char *seam =
        "constautolcd_display_state="
        "boot_v2::make_lcd_sensor_display_state("
        "status_ch0,quality0,status_ch1,quality1);";
    if (task.empty() || count(task, seam) != 1) {
        return false;
    }

    const std::size_t seam_position = task.find(seam);
    const std::size_t loop_header = task.rfind("while(true)", seam_position);
    const std::size_t loop_opening = task.find('{', loop_header);
    const SourceSpan loop = braced_span(task, loop_opening);
    if (!loop.valid() ||
        seam_position <= loop.begin ||
        seam_position >= loop.end) {
        return false;
    }
    const std::size_t seam_depth = brace_depth_at(task, seam_position);

    struct ExpectedApplication {
        const char *lhs;
        const char *rhs;
    };
    constexpr std::array<ExpectedApplication, 6> expected{{
        {"lcd_params.current_temperature",
         "lcd_display_state.channel0.value_celsius"},
        {"lcd_params.display_value_valid_ch0",
         "lcd_display_state.channel0.value_valid"},
        {"lcd_params.status_ch0",
         "lcd_display_state.channel0.raw_status"},
        {"lcd_params.current_temperature_ch1",
         "lcd_display_state.channel1.value_celsius"},
        {"lcd_params.display_value_valid_ch1",
         "lcd_display_state.channel1.value_valid"},
        {"lcd_params.status_ch1",
         "lcd_display_state.channel1.raw_status"},
    }};

    for (const ExpectedApplication &application : expected) {
        const std::string assignment_prefix =
            std::string(application.lhs) + "=";
        const std::string assignment =
            assignment_prefix + application.rhs + ";";
        if (count(task, assignment_prefix) != 1 ||
            count(task, assignment) != 1) {
            return false;
        }
        const std::size_t position = task.find(assignment);
        if (position <= seam_position ||
            position >= loop.end ||
            brace_depth_at(task, position) != seam_depth) {
            return false;
        }
    }
    return true;
}

std::string move_display_applications_under_channel0_copy(
    const std::string &source)
{
    constexpr const char *begin =
        "        lcd_params.current_temperature =\n";
    constexpr const char *end =
        "            lcd_display_state.channel1.raw_status;\n";
    const std::size_t first = source.find(begin);
    const std::size_t last_start = source.find(end, first);
    if (first == std::string::npos || last_start == std::string::npos) {
        return source;
    }
    const std::size_t last = last_start + std::string(end).size();
    const std::string block = source.substr(first, last - first);
    return source.substr(0, first) +
           "        if (lcd_display_state.channel0.value_valid) {\n" +
           block +
           "        }\n" +
           source.substr(last);
}

std::string insert_harmless_display_read_copy(const std::string &source)
{
    constexpr const char *anchor =
        "        if (lcd_display_state.channel0.value_valid) {\n";
    return replace_once_copy(
        source,
        anchor,
        "        (void)lcd_params.current_temperature;\n" +
            std::string(anchor));
}

std::string harmless_display_formatting_copy(const std::string &source)
{
    return replace_once_copy(
        source,
        "        lcd_params.current_temperature =\n"
        "            lcd_display_state.channel0.value_celsius;\n",
        "        lcd_params.current_temperature\n"
        "            = lcd_display_state.channel0.value_celsius;\n");
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

void test_sensor_task_forwards_display_validity_per_channel() noexcept
{
    const std::string params =
        read_file(NB_IOT_SOURCE_ROOT "/src/tasks/tasks_lcd.hpp");
    const std::string sensor =
        read_file(NB_IOT_SOURCE_ROOT "/src/tasks/tasks_sensor_reader.cpp");

    CHECK(sensor.find(
              "#include \"../boot_v2/lcd_status_policy.hpp\"") !=
          std::string::npos);
    CHECK(params.find(
              "volatile bool display_value_valid_ch0{false};") !=
          std::string::npos);
    CHECK(params.find(
              "volatile bool display_value_valid_ch1{false};") !=
          std::string::npos);
    CHECK(sensor_display_state_contract_accepts(sensor));
}

void test_sensor_display_state_contract_mutants() noexcept
{
    const std::string source =
        read_file(NB_IOT_SOURCE_ROOT "/src/tasks/tasks_sensor_reader.cpp");
    const std::string conditional =
        move_display_applications_under_channel0_copy(source);
    const std::string harmless_read =
        insert_harmless_display_read_copy(source);
    const std::string harmless_formatting =
        harmless_display_formatting_copy(source);

    CHECK(conditional != source);
    CHECK(harmless_read != source);
    CHECK(harmless_formatting != source);
    CHECK(!sensor_display_state_contract_accepts(conditional));
    CHECK(sensor_display_state_contract_accepts(harmless_read));
    CHECK(sensor_display_state_contract_accepts(harmless_formatting));
}

void test_lcd_visibility_is_not_keyed_only_to_raw_status() noexcept
{
    const std::string task =
        read_file(NB_IOT_SOURCE_ROOT "/src/tasks/tasks_lcd.cpp");

    CHECK(task.find(
              "lcd_temperature_value_visible(\n"
              "                        params->status_ch0,\n"
              "                        params->display_value_valid_ch0)") !=
          std::string::npos);
    CHECK(task.find(
              "lcd_temperature_value_visible(\n"
              "                        params->status_ch1,\n"
              "                        params->display_value_valid_ch1)") !=
          std::string::npos);
    CHECK(task.find("params->status_ch0 == 0") == std::string::npos);
    CHECK(task.find("params->status_ch1 == 0") == std::string::npos);
}

} // namespace

int main()
{
    test_pcb_pinmap_and_startup_config();
    test_lcd_task_owns_delayed_direct_initialization();
    test_driver_matches_verified_five_second_backpack_sequence();
    test_sensor_task_forwards_display_validity_per_channel();
    test_sensor_display_state_contract_mutants();
    test_lcd_visibility_is_not_keyed_only_to_raw_status();
    if (g_failures != 0) {
        std::fprintf(stderr, "lcd_runtime_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("lcd_runtime_contract_test: %zu checks passed\n", g_checks);
    return 0;
}
