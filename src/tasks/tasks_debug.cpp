#include "tasks_debug.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "../lib/flash_logger.hpp"
#include "../lib/log.hpp"

namespace {

std::atomic<bool> manual_at_mode{false};

void trim_command(char *command) noexcept
{
    std::size_t length = std::strlen(command);
    while (length > 0 &&
           (command[length - 1] == ' ' || command[length - 1] == '\t' ||
            command[length - 1] == '\r' || command[length - 1] == '\n')) {
        command[--length] = '\0';
    }
}

bool command_equals(const char *left, const char *right) noexcept
{
    while (*left != '\0' && *right != '\0') {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

} // namespace

void manual_at_mode_enable() noexcept
{
    manual_at_mode.store(true, std::memory_order_release);
}

bool manual_at_mode_enabled() noexcept
{
    return manual_at_mode.load(std::memory_order_acquire);
}

void vDebugTask(void *)
{
    LOG("DEBUG_READY\n");
    char command[64]{};
    std::size_t length = 0;

    for (;;) {
        if (manual_at_mode_enabled()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const int input = getchar_timeout_us(0);
        if (input != PICO_ERROR_TIMEOUT) {
            const char character = static_cast<char>(input);
            putchar(character);
            if (character == '\r' || character == '\n') {
                if (length != 0) {
                    command[length] = '\0';
                    trim_command(command);
                    if (command_equals(command, "dump_csv")) {
                        flash_log_dump_csv();
                    } else if (command_equals(command, "clear_csv")) {
                        flash_log_clear();
                    } else if (command[0] != '\0') {
                        LOG("DEBUG_DISABLED\n");
                    }
                }
                length = 0;
                std::memset(command, 0, sizeof(command));
            } else if (character == '\b' || input == 127) {
                if (length != 0) {
                    command[--length] = '\0';
                }
            } else if (length < sizeof(command) - 1) {
                command[length++] = character;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
