#include "flash_test_platform.hpp"

#include <csetjmp>
#include <cstring>

namespace flash_test {

PlatformState g_platform{};
alignas(FLASH_PAGE_SIZE)
std::array<std::uint8_t, PICO_FLASH_SIZE_BYTES> g_flash{};

void reset_platform() noexcept
{
    g_platform = {};
    g_platform.scheduler_state = taskSCHEDULER_RUNNING;
    g_platform.mutex_take_result = pdTRUE;
    g_platform.mutex_give_result = pdTRUE;
    g_platform.enter_result = PICO_OK;
    g_platform.exit_result = PICO_OK;
    g_platform.helper_available = true;
    g_flash.fill(0xFFu);
}

} // namespace flash_test

namespace {

int fake_enter_safe_zone(
    const std::uint32_t timeout_ms) noexcept
{
    ++flash_test::g_platform.enter_calls;
    flash_test::g_platform.enter_timeout_ms = timeout_ms;
    flash_test::g_platform.now_us +=
        flash_test::g_platform.enter_elapsed_us;
    if (flash_test::g_platform.enter_result == PICO_OK) {
        flash_test::g_platform.safe_zone_entered = true;
    }
    return flash_test::g_platform.enter_result;
}

int fake_exit_safe_zone(
    const std::uint32_t timeout_ms) noexcept
{
    ++flash_test::g_platform.exit_calls;
    flash_test::g_platform.exit_timeout_ms = timeout_ms;
    flash_test::g_platform.now_us +=
        flash_test::g_platform.exit_elapsed_us;
    flash_test::g_platform.safe_zone_entered = false;
    return flash_test::g_platform.exit_result;
}

flash_safety_helper_t g_helper{
    nullptr,
    fake_enter_safe_zone,
    fake_exit_safe_zone,
};

std::jmp_buf g_callback_interruption;
bool g_callback_interruption_active = false;

void interrupt_raw_callback_if(
    const bool should_interrupt) noexcept
{
    if (should_interrupt &&
        g_callback_interruption_active) {
        std::longjmp(g_callback_interruption, 1);
    }
}

} // namespace

std::uint8_t *flash_test_xip_base() noexcept
{
    if (flash_test::g_platform.mutex_taken) {
        ++flash_test::g_platform.xip_base_while_mutex_calls;
    } else {
        ++flash_test::g_platform.xip_base_without_mutex_calls;
    }
    return flash_test::g_flash.data();
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *const storage) noexcept
{
    return storage;
}

BaseType_t xSemaphoreTake(
    SemaphoreHandle_t,
    const TickType_t ticks_to_wait) noexcept
{
    ++flash_test::g_platform.mutex_take_calls;
    flash_test::g_platform.mutex_wait_ticks = ticks_to_wait;
    flash_test::g_platform.now_us +=
        flash_test::g_platform.mutex_take_elapsed_us;
    flash_test::g_platform.mutex_taken =
        flash_test::g_platform.mutex_take_result == pdTRUE;
    return flash_test::g_platform.mutex_take_result;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t) noexcept
{
    ++flash_test::g_platform.mutex_give_calls;
    flash_test::g_platform.mutex_taken = false;
    return flash_test::g_platform.mutex_give_result;
}

BaseType_t xTaskGetSchedulerState() noexcept
{
    return flash_test::g_platform.scheduler_state;
}

BaseType_t xPortIsInsideInterrupt() noexcept
{
    return flash_test::g_platform.inside_interrupt;
}

std::uint64_t time_us_64() noexcept
{
    return flash_test::g_platform.now_us;
}

void tight_loop_contents() noexcept
{
    ++flash_test::g_platform.now_us;
}

unsigned int get_core_num() noexcept
{
    return flash_test::g_platform.core_num;
}

absolute_time_t get_absolute_time() noexcept
{
    return flash_test::g_platform.now_us;
}

std::uint32_t to_ms_since_boot(
    const absolute_time_t time) noexcept
{
    return static_cast<std::uint32_t>(time / 1000u);
}

std::uint32_t save_and_disable_interrupts() noexcept
{
    ++flash_test::g_platform.disable_irq_calls;
    flash_test::g_platform.interrupts_disabled = true;
    return 0xA5A5u;
}

void restore_interrupts(const std::uint32_t) noexcept
{
    ++flash_test::g_platform.restore_irq_calls;
    flash_test::g_platform.interrupts_disabled = false;
}

void flash_range_erase(
    const std::uint32_t offset,
    const std::size_t size) noexcept
{
    ++flash_test::g_platform.erase_calls;
    flash_test::g_platform.last_erase_offset = offset;
    flash_test::g_platform.last_erase_size = size;
    flash_test::g_platform.raw_without_safety =
        flash_test::g_platform.raw_without_safety ||
        (!flash_test::g_platform.interrupts_disabled &&
         !flash_test::g_platform.safe_zone_entered);
    flash_test::g_platform.now_us +=
        flash_test::g_platform.mutation_elapsed_us;
    std::memset(
        flash_test::g_flash.data() + offset, 0xFF, size);
    interrupt_raw_callback_if(
        flash_test::g_platform.interrupt_after_erase);
}

void flash_range_program(
    const std::uint32_t offset,
    const std::uint8_t *const data,
    const std::size_t size) noexcept
{
    ++flash_test::g_platform.program_calls;
    flash_test::g_platform.last_program_offset = offset;
    flash_test::g_platform.last_program_size = size;
    flash_test::g_platform.raw_without_safety =
        flash_test::g_platform.raw_without_safety ||
        (!flash_test::g_platform.interrupts_disabled &&
         !flash_test::g_platform.safe_zone_entered);
    flash_test::g_platform.now_us +=
        flash_test::g_platform.mutation_elapsed_us;
    for (std::size_t index = 0; index < size; ++index) {
        flash_test::g_flash[offset + index] &= data[index];
    }
    interrupt_raw_callback_if(
        flash_test::g_platform.interrupt_after_program);
}

flash_safety_helper_t *get_flash_safety_helper() noexcept
{
    return flash_test::g_platform.helper_available
               ? &g_helper
               : nullptr;
}

int flash_safe_execute(
    void (*const callback)(void *),
    void *const context,
    const std::uint32_t enter_exit_timeout_ms) noexcept
{
    ++flash_test::g_platform.flash_safe_execute_calls;
    flash_safety_helper_t *const helper =
        get_flash_safety_helper();
    if (helper == nullptr) {
        return PICO_ERROR_NOT_PERMITTED;
    }
    const int enter_result =
        helper->enter_safe_zone_timeout_ms(
            enter_exit_timeout_ms);
    if (enter_result != PICO_OK) {
        return enter_result;
    }
    if (setjmp(g_callback_interruption) != 0) {
        g_callback_interruption_active = false;
        return PICO_ERROR_TIMEOUT;
    }
    g_callback_interruption_active = true;
    callback(context);
    g_callback_interruption_active = false;
    return helper->exit_safe_zone_timeout_ms(
        enter_exit_timeout_ms);
}
