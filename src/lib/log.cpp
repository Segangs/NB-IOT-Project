#include "log.hpp"

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

namespace
{
constexpr size_t LOG_MESSAGE_MAX = 192;
constexpr UBaseType_t LOG_QUEUE_DEPTH = 64;

struct LogMessage
{
    char text[LOG_MESSAGE_MAX];
};

StaticQueue_t g_log_queue_control;
uint8_t g_log_queue_storage[LOG_QUEUE_DEPTH * sizeof(LogMessage)];
QueueHandle_t g_log_queue = nullptr;
volatile bool g_log_enabled = true;
}

void app_log_init()
{
    if (g_log_queue != nullptr)
    {
        return;
    }

    g_log_queue = xQueueCreateStatic(
        LOG_QUEUE_DEPTH,
        sizeof(LogMessage),
        g_log_queue_storage,
        &g_log_queue_control);
}

void app_log_vprintf(const char *fmt, va_list args)
{
    if (!g_log_enabled || fmt == nullptr)
    {
        return;
    }

    if (g_log_queue == nullptr)
    {
        app_log_init();
    }

    LogMessage message = {};
    vsnprintf(message.text, sizeof(message.text), fmt, args);

    if (g_log_queue != nullptr)
    {
        (void)xQueueSend(g_log_queue, &message, 0);
    }
}

void app_log_set_enabled(bool enabled)
{
    g_log_enabled = enabled;
}

void app_log_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    app_log_vprintf(fmt, args);
    va_end(args);
}

void vLogTask(void *pvParameters)
{
    (void)pvParameters;
    app_log_init();

    LogMessage message = {};
    while (true)
    {
        if (xQueueReceive(g_log_queue, &message, portMAX_DELAY) == pdTRUE)
        {
            printf("%s", message.text);
        }
    }
}
