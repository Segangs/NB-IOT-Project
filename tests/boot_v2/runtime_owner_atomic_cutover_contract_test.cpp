#include "runtime_owner_rtos.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
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

void test_numeric_and_public_surface() noexcept
{
    using boot_v2::RuntimeOwnerAtomicCutoverResult;
    CHECK(std::is_same<
          typename std::underlying_type<RuntimeOwnerAtomicCutoverResult>::type,
          std::uint8_t>::value);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerAtomicCutoverResult::RejectedInvalid) == 0);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerAtomicCutoverResult::RejectedNotStarted) == 1);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerAtomicCutoverResult::RejectedNotQuiesced) == 2);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerAtomicCutoverResult::RejectedNotReady) == 3);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerAtomicCutoverResult::Activated) == 4);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerAtomicCutoverResult::AlreadyActive) == 5);

    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.hpp");
    CHECK(count(header, "runtime_owner_rtos_activate_atomic()") == 1);
    CHECK(header.find("RuntimeOwnerActivationFacts") == std::string::npos);
    CHECK(header.find("RuntimeOwnerCutoverPermit") == std::string::npos);
    CHECK(header.find("stable_identity") == std::string::npos);
    CHECK(header.find("set_ingress") == std::string::npos);
}

void test_atomic_order_and_failure_closed() noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    const std::size_t prepare_backend = source.find("g_device_backend.prepare()");
    const std::size_t scheduler = source.find("xTaskGetSchedulerState()");
    const std::size_t preflight = source.find(
        "if (runtime_owner_activation_preflight(");
    const std::size_t prepare = source.find("g_cutover_core.prepare(");
    const std::size_t activate = source.find("g_cutover_coordinator.activate(");
    const std::size_t commit = source.find("g_cutover_core.commit(");
    const std::size_t ready = source.find("g_drain_metrics.cutover_ready = 1");
    const std::size_t barrier = source.find("std::atomic_signal_fence(");
    const std::size_t ingress = source.find("g_status.ingress_enabled = 1");
    CHECK(prepare_backend != std::string::npos);
    CHECK(scheduler != std::string::npos);
    CHECK(preflight != std::string::npos);
    CHECK(prepare != std::string::npos);
    CHECK(activate != std::string::npos);
    CHECK(commit != std::string::npos);
    CHECK(ready != std::string::npos);
    CHECK(barrier != std::string::npos);
    CHECK(ingress != std::string::npos);
    CHECK(prepare_backend < scheduler);
    CHECK(scheduler < preflight);
    CHECK(preflight < prepare);
    CHECK(prepare < activate);
    CHECK(activate < commit);
    CHECK(commit < ready);
    CHECK(ready < barrier);
    CHECK(barrier < ingress);
    CHECK(count(source, "g_status.ingress_enabled = 1") == 1);
    CHECK(count(source, "g_drain_metrics.cutover_ready = 1") == 1);
    CHECK(source.find("taskSCHEDULER_NOT_STARTED") != std::string::npos);
    CHECK(source.find("uxQueueMessagesWaiting(g_urgent_queue) == 0") !=
          std::string::npos);
    CHECK(source.find("uxQueueMessagesWaiting(g_control_queue) == 0") !=
          std::string::npos);
    CHECK(source.find("uxQueueMessagesWaiting(g_normal_queue) == 0") !=
          std::string::npos);
    CHECK(source.find("g_status.ingress_enabled = 0") != std::string::npos);
    CHECK(source.find("g_drain_metrics.cutover_ready = 0") !=
          std::string::npos);
}

void test_main_activates_once_before_scheduler() noexcept
{
    const std::string main_source = read_file(NB_IOT_SOURCE_ROOT "/main.cpp");
    const std::size_t activation =
        main_source.find("runtime_owner_rtos_activate_atomic()");
    const std::size_t scheduler = main_source.find("vTaskStartScheduler()");
    CHECK(count(main_source, "runtime_owner_rtos_activate_atomic()") == 1);
    CHECK(activation != std::string::npos);
    CHECK(scheduler != std::string::npos);
    CHECK(activation < scheduler);
    CHECK(main_source.find("FATAL_RUNTIME_OWNER_CUTOVER") !=
          std::string::npos);
}

void test_lcd_initializes_after_three_second_power_stabilization() noexcept
{
    const std::string config = read_file(NB_IOT_SOURCE_ROOT "/src/config.h");
    const std::string main_source = read_file(NB_IOT_SOURCE_ROOT "/main.cpp");
    const std::string lcd_source = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_lcd.cpp");
    const std::size_t delay = lcd_source.find(
        "vTaskDelay(pdMS_TO_TICKS(LCD_POWER_STABILIZE_DELAY_MS))");
    const std::size_t lcd = lcd_source.find("static LCD_I2C lcd_device(");
    CHECK(main_source.find(
              "sleep_ms(LCD_POWER_STABILIZE_DELAY_MS)") ==
          std::string::npos);
    CHECK(main_source.find("static LCD_I2C lcd(") == std::string::npos);
    CHECK(config.find("#define LCD_POWER_STABILIZE_DELAY_MS 5000") !=
          std::string::npos);
    CHECK(count(lcd_source,
                "vTaskDelay(pdMS_TO_TICKS("
                "LCD_POWER_STABILIZE_DELAY_MS))") == 1);
    CHECK(delay != std::string::npos);
    CHECK(lcd != std::string::npos);
    CHECK(delay < lcd);
}

void test_permit_is_internal_to_coordinator() noexcept
{
    const std::string task_header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_task_core.hpp");
    CHECK(task_header.find("friend class RuntimeOwnerCutoverCoordinator") !=
          std::string::npos);
    CHECK(task_header.find("RuntimeOwnerCutoverPermit() = delete") !=
          std::string::npos);
}

void test_shutdown_finalizer_owns_watchdog_and_gp15_fail_closed_actions()
    noexcept
{
    const std::string root = read_file(NB_IOT_SOURCE_ROOT "/CMakeLists.txt");
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos.cpp");
    const std::string backend = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string producer = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_led.cpp");
    const std::string main_source =
        read_file(NB_IOT_SOURCE_ROOT "/main.cpp");
    const std::string boot_source = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_boot.cpp");
    CHECK(root.find("PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1") !=
          std::string::npos);
    CHECK(source.find("RuntimeOwnerShutdownFinalizerCore") !=
          std::string::npos);
    CHECK(source.find("runtime_owner_drain_once(") != std::string::npos);
    CHECK(source.find("SHUTDOWN_INGRESS_CLOSED") !=
          std::string::npos);
    CHECK(source.find("g_status.ingress_enabled = 0") !=
          std::string::npos);
    CHECK(source.find("stdio_usb_connected()") != std::string::npos);
    CHECK(source.find("runtime_owner_usb_power_present() noexcept") !=
          std::string::npos);
    CHECK(count(source, "stdio_usb_connected()") == 1);
    CHECK(source.find("SHUTDOWN_HARD_DEADLINE_MS") !=
          std::string::npos);
    CHECK(source.find("g_owner_loop.execute_shutdown_cleanup(") !=
          std::string::npos);
    CHECK(source.find("g_shutdown_finalizer.complete(") !=
          std::string::npos);
    CHECK(source.find("g_shutdown_finalizer.submit_usb_recheck(") !=
          std::string::npos);

    const std::size_t watchdog_branch = source.find(
        "case RuntimeOwnerShutdownFinalizeAction::CommitWatchdog:");
    const std::size_t scratch_magic =
        source.find(
            "COMMAND_WATCHDOG_SCRATCH_MAGIC",
            watchdog_branch);
    const std::size_t scratch_context =
        source.find("watchdog_hw->scratch[3] =", scratch_magic);
    const std::size_t commit_log =
        source.find("SHUTDOWN_WATCHDOG_COMMIT", scratch_context);
    const std::size_t reboot =
        source.find("watchdog_reboot(0, 0, 100)", commit_log);
    CHECK(watchdog_branch != std::string::npos);
    CHECK(scratch_magic != std::string::npos);
    CHECK(scratch_context != std::string::npos);
    CHECK(commit_log != std::string::npos);
    CHECK(reboot != std::string::npos);
    CHECK(watchdog_branch < scratch_magic);
    CHECK(scratch_magic < scratch_context);
    CHECK(scratch_context < commit_log);
    CHECK(commit_log < reboot);

    const std::size_t gp15_branch = source.find(
        "case RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill:");
    const std::size_t gp15_init =
        source.find("gpio_init(POWER_KILL_PIN)", gp15_branch);
    const std::size_t gp15_inactive = source.find(
        "gpio_put(POWER_KILL_PIN, POWER_KILL_INACTIVE_LEVEL)", gp15_init);
    const std::size_t gp15_output =
        source.find("gpio_set_dir(POWER_KILL_PIN, GPIO_OUT)", gp15_inactive);
    const std::size_t gp15_log =
        source.find("SHUTDOWN_GP15_COMMIT", gp15_output);
    const std::size_t gp15_active = source.find(
        "gpio_put(POWER_KILL_PIN, POWER_KILL_ACTIVE_LEVEL)", gp15_log);
    CHECK(gp15_branch != std::string::npos);
    CHECK(gp15_init != std::string::npos);
    CHECK(gp15_inactive != std::string::npos);
    CHECK(gp15_output != std::string::npos);
    CHECK(gp15_log != std::string::npos);
    CHECK(gp15_active != std::string::npos);
    CHECK(gp15_branch < gp15_init);
    CHECK(gp15_init < gp15_inactive);
    CHECK(gp15_inactive < gp15_output);
    CHECK(gp15_output < gp15_log);
    CHECK(gp15_log < gp15_active);
    CHECK(count(
              source,
              "gpio_put(POWER_KILL_PIN, POWER_KILL_ACTIVE_LEVEL)") == 1);
    CHECK(backend.find("gpio_put(POWER_KILL_PIN") == std::string::npos);
    CHECK(backend.find("watchdog_reboot") == std::string::npos);

    CHECK(source.find(
              "case RuntimeOwnerShutdownFinalizeAction::AbortUsbChanged:") !=
          std::string::npos);
    CHECK(source.find("SHUTDOWN_USB_CHANGED_ABORT") != std::string::npos);
    CHECK(source.find(
              "case RuntimeOwnerShutdownFinalizeAction::"
              "AbortEvidenceMissing:") != std::string::npos);
    CHECK(source.find("SHUTDOWN_EVIDENCE_MISSING_ABORT") !=
          std::string::npos);

    CHECK(producer.find("stdio_usb_connected()") == std::string::npos);
    CHECK(producer.find("runtime_owner_usb_power_present()") !=
          std::string::npos);
    CHECK(producer.find(
              "runtime_owner_redacted_status().runtime_ready != 0") !=
          std::string::npos);
    const std::size_t usb_attach_delay = boot_source.find(
        "vTaskDelay(pdMS_TO_TICKS(1000))");
    const std::size_t last_shutdown = boot_source.find(
        "runtime_owner_shutdown_record_log_current()", usb_attach_delay);
    const std::size_t selftest = boot_source.find("SELFTEST", last_shutdown);
    CHECK(main_source.find("runtime_owner_shutdown_record_log_current()") ==
          std::string::npos);
    CHECK(usb_attach_delay != std::string::npos);
    CHECK(last_shutdown != std::string::npos);
    CHECK(selftest != std::string::npos);
    CHECK(usb_attach_delay < last_shutdown);
    CHECK(last_shutdown < selftest);
    CHECK(boot_source.find("\"Start Owner\"") == std::string::npos);
    CHECK(boot_source.find("\"Check Pico\"") != std::string::npos);
    CHECK(boot_source.find("\"Boot Error\"") != std::string::npos);
}

} // namespace

int main()
{
    test_numeric_and_public_surface();
    test_atomic_order_and_failure_closed();
    test_main_activates_once_before_scheduler();
    test_lcd_initializes_after_three_second_power_stabilization();
    test_permit_is_internal_to_coordinator();
    test_shutdown_finalizer_owns_watchdog_and_gp15_fail_closed_actions();
    if (g_failures != 0) {
        std::fprintf(stderr,
                     "runtime_owner_atomic_cutover_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf(
        "runtime_owner_atomic_cutover_contract_test: %zu checks passed\n",
        g_checks);
    return 0;
}
