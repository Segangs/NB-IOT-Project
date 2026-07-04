#ifndef APP_LOG_HPP
#define APP_LOG_HPP

#include <stdarg.h>
#include <stddef.h>

#define LOG(...) app_log_printf(__VA_ARGS__)

void app_log_init();
void app_log_set_enabled(bool enabled);
void app_log_printf(const char *fmt, ...);
void app_log_vprintf(const char *fmt, va_list args);
void vLogTask(void *pvParameters);

#endif // APP_LOG_HPP
