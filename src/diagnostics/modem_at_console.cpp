#include "../config.h"

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::size_t kReadGuardBytes = 256;
constexpr std::size_t kResponseBytes = 2048;
constexpr std::size_t kCertificateChunkBytes = 200;
constexpr std::uint32_t kAtSettleMs = 1000;

// ISRG Root X2 is the existing public root used by the product firmware.
// Keep the certificate payload off USB logs; only its byte count is reported.
constexpr char kLeRootX2Certificate[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

char g_response[kResponseBytes]{};
std::size_t g_response_length = 0;
std::uint32_t g_last_at_activity_ms = 0;
bool g_at_activity_seen = false;

std::uint32_t now_ms() noexcept
{
    return to_ms_since_boot(get_absolute_time());
}

void note_at_activity() noexcept
{
    g_last_at_activity_ms = now_ms();
    g_at_activity_seen = true;
}

void clear_response() noexcept
{
    g_response_length = 0;
    g_response[0] = '\0';
}

void append_response(const char value) noexcept
{
    if (g_response_length + 1 >= sizeof(g_response)) {
        return;
    }
    g_response[g_response_length++] = value;
    g_response[g_response_length] = '\0';
}

void drain_modem_response() noexcept
{
    std::size_t read_count = 0;
    while (read_count < kReadGuardBytes && uart_is_readable(UART_ID)) {
        const char value = static_cast<char>(uart_getc(UART_ID));
        append_response(value);
        putchar_raw(value);
        ++read_count;
    }
    if (read_count != 0) {
        note_at_activity();
        stdio_flush();
    }
}

void wait_for_at_command_slot() noexcept
{
    while (g_at_activity_seen &&
           now_ms() - g_last_at_activity_ms < kAtSettleMs) {
        drain_modem_response();
        sleep_ms(1);
    }
}

void send_at_command(const char *command) noexcept
{
    wait_for_at_command_slot();
    std::printf("AT TX %s\\r\n", command);
    uart_puts(UART_ID, command);
    uart_putc_raw(UART_ID, '\r');
    note_at_activity();
    stdio_flush();
}

bool wait_for_response(
    const char *expected,
    const std::uint32_t timeout_ms) noexcept
{
    const std::uint32_t started_ms = now_ms();
    while (now_ms() - started_ms < timeout_ms) {
        drain_modem_response();
        if (std::strstr(g_response, expected) != nullptr) {
            return true;
        }
        if (std::strstr(g_response, "ERROR") != nullptr) {
            return false;
        }
        sleep_ms(1);
    }
    std::printf("AT RX <TIMEOUT %lu ms>\n",
                static_cast<unsigned long>(timeout_ms));
    return false;
}

bool send_and_wait(
    const char *command,
    const char *expected,
    const std::uint32_t timeout_ms) noexcept
{
    clear_response();
    send_at_command(command);
    return wait_for_response(expected, timeout_ms);
}

bool command_has_at_prefix(const char *command) noexcept
{
    return command[0] == 'A' && command[1] == 'T';
}

void power_on_modem() noexcept
{
    std::printf("MODEM_PWR_ON\n");
    gpio_put(PWR_ON_PIN, 1);
    sleep_ms(1000);
    gpio_put(PWR_ON_PIN, 0);
    sleep_ms(1500);
    gpio_put(PWR_ON_PIN, 1);
    std::printf("MODEM_BOOT_WAIT 30000 ms\n");
    sleep_ms(30000);
}

void initialize_uart_and_power_pin() noexcept
{
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(UART_TX_PIN);
    gpio_pull_up(UART_RX_PIN);
    uart_set_hw_flow(UART_ID, false, false);

    gpio_init(PWR_ON_PIN);
    gpio_set_dir(PWR_ON_PIN, GPIO_OUT);
    gpio_put(PWR_ON_PIN, 1);
}

bool write_certificate() noexcept
{
    std::printf("CERT_WRITE_START\n");
    (void)send_and_wait("AT+KCERTDELETE=0,0", "OK", 3000);
    sleep_ms(1000);

    const std::size_t certificate_length = std::strlen(kLeRootX2Certificate);
    char command[64]{};
    std::snprintf(command, sizeof(command), "AT+KCERTSTORE=0,%u,0",
                  static_cast<unsigned>(certificate_length));
    if (!send_and_wait(command, "CONNECT", 5000)) {
        std::printf("CERT_WRITE_FAIL\n");
        return false;
    }

    sleep_ms(200);
    clear_response();
    std::printf("AT TX DATA <CERTIFICATE REDACTED bytes=%u>\n",
                static_cast<unsigned>(certificate_length));
    for (std::size_t offset = 0; offset < certificate_length;
         offset += kCertificateChunkBytes) {
        const std::size_t remaining = certificate_length - offset;
        const std::size_t chunk = remaining < kCertificateChunkBytes
            ? remaining
            : kCertificateChunkBytes;
        for (std::size_t index = 0; index < chunk; ++index) {
            uart_putc_raw(UART_ID, kLeRootX2Certificate[offset + index]);
            sleep_us(500);
        }
        note_at_activity();
        sleep_ms(500);
    }
    sleep_ms(1500);
    const bool stored = wait_for_response("OK", 5000);
    std::printf(stored ? "CERT_WRITE_OK\n" : "CERT_WRITE_FAIL\n");
    return stored;
}

void run_initialization() noexcept
{
    power_on_modem();
    (void)send_and_wait("AT", "OK", 5000);
    (void)send_and_wait("ATE0", "OK", 3000);
    (void)send_and_wait("AT&K0", "OK", 3000);
    (void)send_and_wait("AT+IFC=0,0", "OK", 3000);
    (void)send_and_wait("AT+CMEE=1", "OK", 3000);
    (void)send_and_wait("AT+CFUN=1", "OK", 5000);
    sleep_ms(30000);
    (void)send_and_wait("AT+CPIN?", "OK", 5000);
    (void)send_and_wait("AT+CGSN", "OK", 5000);
    (void)send_and_wait("AT+CIMI", "OK", 5000);
    (void)write_certificate();
}

void run_manual_console() noexcept
{
    std::printf("MANUAL_AT_READY input=USB terminator=CR only_AT=1\n");
    char command[512]{};
    std::size_t length = 0;
    bool overflow = false;

    for (;;) {
        drain_modem_response();
        const int input = getchar_timeout_us(0);
        if (input == PICO_ERROR_TIMEOUT) {
            sleep_ms(1);
            continue;
        }

        const char value = static_cast<char>(input);
        if (value == '\r' || value == '\n') {
            putchar_raw('\r');
            putchar_raw('\n');
            stdio_flush();
            if (overflow || length == 0) {
                if (overflow) {
                    std::printf("AT INPUT_REJECTED overflow\n");
                }
            } else if (!command_has_at_prefix(command)) {
                std::printf("AT INPUT_REJECTED prefix\n");
            } else {
                clear_response();
                send_at_command(command);
            }
            length = 0;
            overflow = false;
            command[0] = '\0';
            continue;
        }

        if (value == '\b' || value == 0x7f) {
            if (length != 0) {
                --length;
                command[length] = '\0';
                std::printf("\b \b");
                stdio_flush();
            }
            continue;
        }

        if (std::isprint(static_cast<unsigned char>(value))) {
            if (length + 1 < sizeof(command)) {
                command[length++] = value;
                command[length] = '\0';
                putchar_raw(value);
                stdio_flush();
            } else {
                overflow = true;
            }
        }
    }
}

} // namespace

int main()
{
    stdio_init_all();
    sleep_ms(1000);
    std::printf("MODEM_AT_CONSOLE_BOOT\n");
    initialize_uart_and_power_pin();
    run_initialization();
    run_manual_console();
}
