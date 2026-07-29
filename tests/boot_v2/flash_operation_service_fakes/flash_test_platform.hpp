#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

using BaseType_t = int;
using TickType_t = std::uint32_t;
using UBaseType_t = unsigned int;

struct StaticSemaphore_t {
    std::uint8_t opaque{0};
};

using SemaphoreHandle_t = StaticSemaphore_t *;

inline constexpr BaseType_t pdFALSE = 0;
inline constexpr BaseType_t pdTRUE = 1;

inline constexpr BaseType_t taskSCHEDULER_NOT_STARTED = 0;
inline constexpr BaseType_t taskSCHEDULER_RUNNING = 1;
inline constexpr BaseType_t taskSCHEDULER_SUSPENDED = 2;

#define configTICK_RATE_HZ 1000u

#define FLASH_PAGE_SIZE 256u
#define FLASH_SECTOR_SIZE 4096u
#define PICO_FLASH_SIZE_BYTES 0x400000u

#define PICO_OK 0
#define PICO_ERROR_TIMEOUT (-1)
#define PICO_ERROR_NOT_PERMITTED (-2)
#define PICO_ERROR_INSUFFICIENT_RESOURCES (-3)

#define __no_inline_not_in_flash_func(name) name

std::uint8_t *flash_test_xip_base() noexcept;

#define XIP_BASE \
    (reinterpret_cast<std::uintptr_t>(flash_test_xip_base()))

SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *storage) noexcept;
BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t ticks_to_wait) noexcept;
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) noexcept;

BaseType_t xTaskGetSchedulerState() noexcept;
BaseType_t xPortIsInsideInterrupt() noexcept;

std::uint64_t time_us_64() noexcept;
void tight_loop_contents() noexcept;
unsigned int get_core_num() noexcept;

using absolute_time_t = std::uint64_t;
absolute_time_t get_absolute_time() noexcept;
std::uint32_t to_ms_since_boot(absolute_time_t time) noexcept;

std::uint32_t save_and_disable_interrupts() noexcept;
void restore_interrupts(std::uint32_t saved) noexcept;

void flash_range_erase(
    std::uint32_t offset,
    std::size_t size) noexcept;
void flash_range_program(
    std::uint32_t offset,
    const std::uint8_t *data,
    std::size_t size) noexcept;

struct flash_safety_helper_t {
    bool (*core_init_deinit)(bool init);
    int (*enter_safe_zone_timeout_ms)(std::uint32_t timeout_ms);
    int (*exit_safe_zone_timeout_ms)(std::uint32_t timeout_ms);
};

flash_safety_helper_t *get_flash_safety_helper() noexcept;
int flash_safe_execute(
    void (*callback)(void *),
    void *context,
    std::uint32_t enter_exit_timeout_ms) noexcept;

namespace flash_test {

struct PlatformState {
    BaseType_t scheduler_state{taskSCHEDULER_RUNNING};
    BaseType_t inside_interrupt{pdFALSE};
    unsigned int core_num{0};
    std::uint64_t now_us{0};
    std::uint64_t mutex_take_elapsed_us{0};
    std::uint64_t enter_elapsed_us{0};
    std::uint64_t mutation_elapsed_us{0};
    std::uint64_t exit_elapsed_us{0};
    BaseType_t mutex_take_result{pdTRUE};
    BaseType_t mutex_give_result{pdTRUE};
    int enter_result{PICO_OK};
    int exit_result{PICO_OK};
    bool helper_available{true};
    bool mutex_taken{false};
    bool interrupts_disabled{false};
    bool safe_zone_entered{false};
    bool raw_without_safety{false};
    bool interrupt_after_erase{false};
    bool interrupt_after_program{false};
    std::uint32_t mutex_wait_ticks{0};
    std::uint32_t enter_timeout_ms{0};
    std::uint32_t exit_timeout_ms{0};
    std::size_t mutex_take_calls{0};
    std::size_t mutex_give_calls{0};
    std::size_t enter_calls{0};
    std::size_t exit_calls{0};
    std::size_t flash_safe_execute_calls{0};
    std::size_t disable_irq_calls{0};
    std::size_t restore_irq_calls{0};
    std::size_t erase_calls{0};
    std::size_t program_calls{0};
    std::size_t xip_base_while_mutex_calls{0};
    std::size_t xip_base_without_mutex_calls{0};
    std::uint32_t last_erase_offset{0};
    std::size_t last_erase_size{0};
    std::uint32_t last_program_offset{0};
    std::size_t last_program_size{0};
};

extern PlatformState g_platform;
extern std::array<std::uint8_t, PICO_FLASH_SIZE_BYTES> g_flash;

void reset_platform() noexcept;

} // namespace flash_test
