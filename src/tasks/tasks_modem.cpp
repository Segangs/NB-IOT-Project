#include "tasks_modem.hpp"
#include "../config.h"
#include "../lib/log.hpp"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// FreeRTOS support
#include "FreeRTOS.h"
#include "task.h"

// Let's Encrypt ISRG Root X2 (Self-Signed) certificate for p.zxcx.io verification (790 bytes with \n newlines)
static const char *LE_ROOT_YE_CERT =
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


// ====================================================================================
// Cooperative Sleep Helper: Performs non-blocking yields when FreeRTOS is active
// ====================================================================================
void modem_sleep(uint32_t ms)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
    {
        sleep_ms(ms);
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

nb_iot::nb_iot()
{
    buffer_idx = 0;
    is_mqtt_connected = false;
    last_csq = 99;
    last_cereg = -1;
    mqtt_session_id = 0;
    mqtt_state = LTE_DETACHED;
    mqtt_boot_cleanup_done = false;
    at_trace_enabled = true;
    last_at_activity_ms = 0;
    at_command_started = false;
    at_command_settle_bypass = false;
    is_unauthenticated = false;

    memset(rx_buffer, 0, sizeof(rx_buffer));
    strcpy(device_imei, "0");
    strcpy(device_cimi, "0");
    strcpy(carrier_name, "Unknown");

    // Initialize UART0 using standard Pico SDK driver (proven 100% robust)
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // 강제 내부 풀업(Pull-up) 설정을 통해 플로팅 노이즈에 의한 모뎀 무한 ERROR 피드백 방지
    gpio_pull_up(UART_TX_PIN);
    gpio_pull_up(UART_RX_PIN);

    // Disable hardware flow control
    uart_set_hw_flow(UART_ID, false, false);

    // Initialize PWR_ON control pin
    gpio_init(PWR_ON_PIN);
    gpio_set_dir(PWR_ON_PIN, GPIO_OUT);
    gpio_put(PWR_ON_PIN, 1); // Set HIGH (inactive)
}

nb_iot::~nb_iot()
{
    uart_deinit(UART_ID);
}

void nb_iot::modem_TraceTxCommand(const char *cmd)
{
    if (!at_trace_enabled || cmd == nullptr)
    {
        return;
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (strncmp(cmd, "AT+KMQTTCFG=", 12) == 0)
    {
        LOG("[%06lu.%03lu] AT TX AT+KMQTTCFG=<REDACTED bytes=%u>\\r\n",
            static_cast<unsigned long>(now_ms / 1000),
            static_cast<unsigned long>(now_ms % 1000),
            static_cast<unsigned>(strlen(cmd)));
        return;
    }

    constexpr size_t chunk_size = 120;
    const size_t command_length = strlen(cmd);
    for (size_t offset = 0; offset < command_length; offset += chunk_size)
    {
        const size_t remaining = command_length - offset;
        const size_t current = remaining < chunk_size ? remaining : chunk_size;
        const bool final_chunk = offset + current == command_length;
        LOG("[%06lu.%03lu] AT TX%s %.*s%s\n",
            static_cast<unsigned long>(now_ms / 1000),
            static_cast<unsigned long>(now_ms % 1000),
            offset == 0 ? "" : "+",
            static_cast<int>(current),
            cmd + offset,
            final_chunk ? "\\r" : "");
    }
}

void nb_iot::modem_TraceRxBytes(const char *data, size_t length)
{
    if (!at_trace_enabled || data == nullptr || length == 0)
    {
        return;
    }

    constexpr size_t raw_chunk_size = 32;
    static constexpr char hex[] = "0123456789ABCDEF";
    for (size_t offset = 0; offset < length; offset += raw_chunk_size)
    {
        const size_t remaining = length - offset;
        const size_t current =
            remaining < raw_chunk_size ? remaining : raw_chunk_size;
        char escaped[raw_chunk_size * 4 + 1]{};
        size_t write = 0;
        for (size_t index = 0; index < current; ++index)
        {
            const unsigned char value =
                static_cast<unsigned char>(data[offset + index]);
            if (value == '\r' || value == '\n' || value == '\t' ||
                value == '\\')
            {
                escaped[write++] = '\\';
                escaped[write++] = value == '\r' ? 'r'
                                     : value == '\n' ? 'n'
                                     : value == '\t' ? 't'
                                                     : '\\';
            }
            else if (value >= 0x20 && value <= 0x7E)
            {
                escaped[write++] = static_cast<char>(value);
            }
            else
            {
                escaped[write++] = '\\';
                escaped[write++] = 'x';
                escaped[write++] = hex[(value >> 4) & 0x0F];
                escaped[write++] = hex[value & 0x0F];
            }
        }
        escaped[write] = '\0';
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        LOG("[%06lu.%03lu] AT RX%s %s\n",
            static_cast<unsigned long>(now_ms / 1000),
            static_cast<unsigned long>(now_ms % 1000),
            offset == 0 ? "" : "+",
            escaped);
    }
}

void nb_iot::modem_TraceTimeout(const uint32_t timeout_ms)
{
    if (!at_trace_enabled)
    {
        return;
    }
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    LOG("[%06lu.%03lu] AT RX <TIMEOUT %lu ms bytes=%d>\n",
        static_cast<unsigned long>(now_ms / 1000),
        static_cast<unsigned long>(now_ms % 1000),
        static_cast<unsigned long>(timeout_ms),
        buffer_idx);
}

void nb_iot::modem_TraceRedactedData(const char *label, const size_t length)
{
    if (!at_trace_enabled || label == nullptr)
    {
        return;
    }
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    LOG("[%06lu.%03lu] AT TX DATA <%s REDACTED bytes=%u>\n",
        static_cast<unsigned long>(now_ms / 1000),
        static_cast<unsigned long>(now_ms % 1000),
        label,
        static_cast<unsigned>(length));
}

void nb_iot::modem_MarkAtCommandTimeout()
{
    this->last_at_activity_ms = to_ms_since_boot(get_absolute_time());
}

void nb_iot::modem_ClearRxBuffer()
{
    // UART 하드웨어 FIFO 버퍼에 대기 중인 잔여 바이트들을 완전히 읽어내어 버림으로써 이전 명령의 URC/에러 찌꺼기 유입을 차단합니다.
    char trace_bytes[64]{};
    size_t trace_length = 0;
    bool received_any = false;
    while (uart_is_readable(UART_ID))
    {
        received_any = true;
        trace_bytes[trace_length++] = uart_getc(UART_ID);
        if (trace_length == sizeof(trace_bytes))
        {
            modem_TraceRxBytes(trace_bytes, trace_length);
            trace_length = 0;
        }
    }
    if (trace_length != 0)
    {
        modem_TraceRxBytes(trace_bytes, trace_length);
    }
    if (received_any)
    {
        last_at_activity_ms = to_ms_since_boot(get_absolute_time());
    }
    memset(this->rx_buffer, 0, sizeof(this->rx_buffer));
    this->buffer_idx = 0;
}

void nb_iot::modem_WaitForAtCommandSlot()
{
    if (!at_command_started || at_command_settle_bypass)
    {
        modem_ClearRxBuffer();
        return;
    }

    while (true)
    {
        modem_ReadResponse(0);
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        const uint32_t quiet_ms = now_ms - last_at_activity_ms;
        if (quiet_ms >= kAtCommandSettleMs &&
            !uart_is_readable(UART_ID))
        {
            if (at_trace_enabled)
            {
                LOG("[%06lu.%03lu] AT SETTLE %lu ms\n",
                    static_cast<unsigned long>(now_ms / 1000),
                    static_cast<unsigned long>(now_ms % 1000),
                    static_cast<unsigned long>(quiet_ms));
            }
            break;
        }

        const uint32_t remaining_ms =
            quiet_ms < kAtCommandSettleMs
                ? kAtCommandSettleMs - quiet_ms
                : 1;
        modem_sleep(remaining_ms > 10 ? 10 : remaining_ms);
    }

    memset(this->rx_buffer, 0, sizeof(this->rx_buffer));
    this->buffer_idx = 0;
}

void nb_iot::modem_SendCmd(const char *cmd)
{
    this->modem_WaitForAtCommandSlot();
    this->modem_TraceTxCommand(cmd);

    // Send AT command
    uart_puts(UART_ID, cmd);
    uart_puts(UART_ID, "\r");
    this->last_at_activity_ms = to_ms_since_boot(get_absolute_time());
    this->at_command_started = true;

}

void nb_iot::modem_PacedWrite(const char *data)
{
    int len = strlen(data);
    for (int i = 0; i < len; i++)
    {
        uart_putc(UART_ID, data[i]);
        sleep_us(2000); // 2ms 정밀 페이싱 (UART 하드웨어 버퍼 오버런 완벽 차단)
    }
}

void nb_iot::modem_SendCmdUserInput()
{
    int pc_char = getchar_timeout_us(0); // PC 입력 확인 (Non-blocking)
    if (pc_char != PICO_ERROR_TIMEOUT)
    {
        uart_putc(UART_ID, (char)pc_char);
    }
}

// ====================================================================================
// [수정 포인트 1] 노이즈 필터링 위치 보정
// 문자열 수집 중 간헐적으로 튀는 하드웨어 Null(0x00) 및 0xFF 상시 필터링
// ====================================================================================
void nb_iot::modem_ReadResponse(int check)
{
    (void)check;
    int max_bytes = 256; // 무한 루프 방지를 위해 한 번에 최대 256바이트만 수집
    int count = 0;
    char trace_bytes[256]{};
    size_t trace_length = 0;

    while (uart_is_readable(UART_ID) && count < max_bytes)
    {
        count++;
        if (this->buffer_idx < 1023)
        {
            char response = uart_getc(UART_ID);
            trace_bytes[trace_length++] = response;

            // 인덱스 위치와 상관없이 시리얼 노이즈 데이터 상시 필터링
            if (response == 0x00 || (unsigned char)response == 0xFF)
            {
                continue;
            }

            // 버퍼가 비어있을 때 들어오는 선행 개행문자만 스킵
            if (this->buffer_idx == 0 && (response == '\r' || response == '\n'))
            {
                continue;
            }

            this->rx_buffer[this->buffer_idx] = response;
            this->buffer_idx++;
            this->rx_buffer[this->buffer_idx] = '\0';
        }
        else
        {
            this->buffer_idx = 0; // Prevent buffer overrun
        }
    }
    this->modem_TraceRxBytes(trace_bytes, trace_length);
    if (trace_length != 0)
    {
        this->last_at_activity_ms = to_ms_since_boot(get_absolute_time());
    }
}

bool nb_iot::modem_SendCmdWaitResponse(const char *cmd, const char *expected_resp1, const char *expected_resp2, uint32_t timeout_ms)
{
    this->modem_SendCmd(cmd);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 1;

    while (elapsed < timeout_ms)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;

        this->modem_ReadResponse(0); // Echo to serial monitor

        if (expected_resp1 && strstr(this->rx_buffer, expected_resp1) != nullptr)
        {
            return true;
        }
        if (expected_resp2 && strstr(this->rx_buffer, expected_resp2) != nullptr)
        {
            return true;
        }

        // Fail early on generic ERROR unless the caller explicitly expects it
        if (strstr(this->rx_buffer, "ERROR") != nullptr)
        {
            if ((expected_resp1 && strstr(expected_resp1, "ERROR") != nullptr) ||
                (expected_resp2 && strstr(expected_resp2, "ERROR") != nullptr) ||
                (expected_resp1 && strstr(expected_resp1, "910") != nullptr) ||
                (expected_resp2 && strstr(expected_resp2, "910") != nullptr))
            {
                return true;
            }
            return false;
        }
    }
    this->modem_MarkAtCommandTimeout();
    this->modem_TraceTimeout(timeout_ms);
    return false;
}

bool nb_iot::modem_SendCmdWaitOK(const char *cmd, uint32_t timeout_ms, uint32_t post_delay_ms)
{
    return this->modem_SendCmdWaitResponse(cmd, "OK", nullptr, timeout_ms);
}

void nb_iot::modem_hw_power_on()
{
    LOG("MODEM_PWR_ON\n");

    // 1. 초기 상태: 확실히 HIGH(비활성)로 유지
    gpio_put(PWR_ON_PIN, 1);
    modem_sleep(1000);

    // 2. LOW로 1.5초(1500ms) 동안 유지하여 모뎀 켜기 트리거
    gpio_put(PWR_ON_PIN, 0);
    modem_sleep(1500);

    // 3. 다시 HIGH(비활성)로 복구하여 릴리즈
    gpio_put(PWR_ON_PIN, 1);

    LOG("BOOT 30s\n");
    modem_sleep(30000);

    LOG("MODEM_PWR_OK\n");
}

bool nb_iot::modem_configure_txon_indicator()
{
    LOG("TXON_CFG\n");
    if (this->modem_SendCmdWaitResponse("AT+KHWIOCFG?", "+KHWIOCFG:", nullptr, 3000))
    {
        if (strstr(this->rx_buffer, "+KHWIOCFG: 5,1") != nullptr ||
            strstr(this->rx_buffer, "+KHWIOCFG:5,1") != nullptr)
        {
            LOG("TXON_CFG_OK\n");
            return true;
        }

        if (strstr(this->rx_buffer, "+KHWIOCFG: 5,0") != nullptr ||
            strstr(this->rx_buffer, "+KHWIOCFG:5,0") != nullptr)
        {
            LOG("TXON_CFG_SET\n");
        }
        else
        {
            LOG("TXON_CFG_SET\n");
        }
    }
    else
    {
        LOG("TXON_CFG_SET\n");
    }

    if (!this->modem_SendCmdWaitOK("AT+KHWIOCFG=5,1", 3000, 1000))
    {
        LOG("TXON_CFG_FAIL\n");
        return false;
    }

    LOG("TXON_CFG_OK\n");
    return true;
}

bool nb_iot::modem_init(int &at_status, int &cpin_status)
{
    modem_hw_power_on();

    // ====================================================================================
    // [Baudrate Auto-Negotiation & 115200bps 상향 고정 시퀀스]
    // 115200bps 또는 9600bps 중 모뎀이 현재 어떤 속도로 설정되어 있든 감지해 115200bps로 통신을 동기화합니다.
    // ====================================================================================
    bool alive = false;

    // 1. 115200bps 접속 시도 (이미 모뎀이 115200bps로 설정되어 있는 경우 대비)
    LOG("MODEM_SYNC\n");
    uart_set_baudrate(UART_ID, 115200);
    this->modem_ClearRxBuffer();

    for (int i = 0; i < 2; i++)
    {
        if (check_at_alive())
        {
            alive = true;
            LOG("MODEM_AT_OK\n");
            break;
        }
        modem_sleep(1000);
    }

    // 2. 115200bps 응답이 없는 경우 9600bps로 접속한 뒤 115200bps로 상향 재지정
    if (!alive)
    {
        LOG("MODEM_BAUD_FIX\n");
        uart_set_baudrate(UART_ID, 9600);
        this->modem_ClearRxBuffer();

        for (int i = 0; i < 3; i++)
        {
            if (check_at_alive())
            {
                alive = true;
                LOG("MODEM_BAUD_SET\n");

                // 모뎀 통신 속도 115200bps 변경 명령 송출
                this->modem_SendCmd("AT+IPR=115200");
                modem_sleep(500);

                // 피코 UART도 115200bps로 상향 조정
                uart_set_baudrate(UART_ID, 115200);
                modem_sleep(500);

                // 115200bps 속도 전환 정상 완료 여부 검증
                if (check_at_alive())
                {
                    LOG("MODEM_BAUD_OK\n");
                    this->modem_SendCmd("AT&W"); // 비휘발성 저장
                    modem_sleep(500);
                }
                else
                {
                    alive = false;
                }
                break;
            }
            modem_sleep(1000);
        }
    }

    at_status = alive ? 0 : 1;
    if (!alive)
    {
        LOG("MODEM_AT_FAIL\n");
        cpin_status = 1;
        return false;
    }

    // 1-1. ATE0 (Echo Off) 설정 및 2초 대기 (가장 먼저 전송!)
    LOG("MODEM_ECHO_OFF\n");
    modem_SendCmdWaitOK("ATE0", 3000, 2000);

    // The current two-wire PCB leaves modem CTS unconnected and ties RTS low,
    // so restore both Rev29 flow-control settings to none before further AT
    // traffic. This is also required after a factory-profile reset.
    if (!modem_SendCmdWaitOK("AT&K0", 3000, 0) ||
        !modem_SendCmdWaitOK("AT+IFC=0,0", 3000, 0))
    {
        LOG("MODEM_FLOW_CFG_FAIL\n");
        cpin_status = 1;
        return false;
    }
    LOG("MODEM_FLOW_CFG_OK\n");

    // Step 2: Enable detailed error reporting (+CMEE=1) 및 2초 대기
    modem_SendCmdWaitOK("AT+CMEE=1", 3000, 2000);

    // Step 2-0: Enable RM78 TX_ON indicator output so GP5 can receive TX_ON pulses.
    modem_configure_txon_indicator();

    // Step 3: Turn on full modem capabilities and wait 30s as specified
    LOG("MODEM_CFUN\n");
    modem_SendCmdWaitOK("AT+CFUN=1", 5000, 30000);

    // Step 4: Validate SIM card status & wait 2s
    cpin_status = check_sim_status();
    modem_sleep(2000);

    // Step 5: Pull IMEI and wait 2s
    retrieve_imei(device_imei);
    modem_sleep(2000);

    // Step 6: Pull CIMI and wait 2s
    retrieve_cimi(device_cimi);
    modem_sleep(2000);

    // 💡 [트러블슈팅] HL7811 모뎀의 NVRAM 인증서 저장 한계(1024바이트) 극복 방안:
    // Let's Encrypt의 790바이트 ISRG Root X2(Root YE) 진짜 인증서를 압축하여 0번 슬롯에 저장하고,
    // 실제 통신 시에는 KSSLCFG=0,3(Full Verification) 설정을 통해 MQTTS 보안 검증을 수행합니다.
    LOG("CERT_WRITE_START\n");
    this->modem_SendCmdWaitResponse("AT+KCERTDELETE=0,0", "OK", "ERROR", 3000);
    modem_sleep(1000);

    char cert_cmd[64];
    int cert_len = strlen(LE_ROOT_YE_CERT);
    snprintf(cert_cmd, sizeof(cert_cmd), "AT+KCERTSTORE=0,%d,0", cert_len);

    bool cert_prompt_ok = this->modem_SendCmdWaitResponse(cert_cmd, "CONNECT", nullptr, 5000);
    bool store_ok = false;

    if (cert_prompt_ok)
    {
        modem_sleep(200);
        this->modem_ClearRxBuffer();
        this->modem_TraceRedactedData("CERTIFICATE", cert_len);
        app_log_set_enabled(false);

        int chunk_size = 200;
        for (int i = 0; i < cert_len; i += chunk_size)
        {
            int current_chunk = (cert_len - i < chunk_size) ? (cert_len - i) : chunk_size;
            for (int j = 0; j < current_chunk; j++)
            {
                uart_putc(UART_ID, LE_ROOT_YE_CERT[i + j]);
                sleep_us(500); // 바이트 간 2ms 페이싱 딜레이로 오버런 완벽 차단
            }
            modem_sleep(500); // 청크 간 500ms 대기 시간을 두어 모뎀 처리 여유 제공
        }
        modem_sleep(1500);
        app_log_set_enabled(true);

        uint32_t elapsed = 0;
        const uint32_t step_ms = 100;
        while (elapsed < 5000)
        {
            modem_sleep(step_ms);
            elapsed += step_ms;
            this->modem_ReadResponse(0);
            if (strstr(this->rx_buffer, "OK") != nullptr)
            {
                store_ok = true;
                break;
            }
            if (strstr(this->rx_buffer, "ERROR") != nullptr)
            {
                break;
            }
        }
    }

    app_log_set_enabled(true);
    if (cert_prompt_ok && store_ok)
    {
        LOG("CERT_WRITE_OK\n");
    }
    else if (cert_prompt_ok)
    {
        LOG("CERT_WRITE_FAIL\n");
    }
    else
    {
        LOG("CERT_WRITE_FAIL\n");
    }
    modem_sleep(2000);

    return (cpin_status == 0);
}

bool nb_iot::check_at_alive()
{
    this->modem_SendCmd("AT");
    modem_sleep(2000);
    this->modem_ReadResponse();
    return (strstr(this->rx_buffer, "OK") != nullptr);
}

int nb_iot::check_sim_status()
{
    this->modem_SendCmd("AT+CPIN?");
    modem_sleep(2000);
    this->modem_ReadResponse();
    return (strstr(this->rx_buffer, "+CPIN: READY") != nullptr) ? 0 : 1;
}

int nb_iot::check_rssi_csq()
{
    this->modem_SendCmd("AT+CSQ");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char *p = strstr(this->rx_buffer, "+CSQ:");
    if (p != nullptr)
    {
        int rssi = 99, ber = 99;
        if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) == 2)
        {
            last_csq = rssi;
            return rssi;
        }
    }
    last_csq = 99;
    return 99;
}

int nb_iot::check_network_registration()
{
    this->modem_SendCmd("AT+CEREG?");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char *p = strstr(this->rx_buffer, "+CEREG:");
    if (p != nullptr)
    {
        int n = 0, stat = -1;
        if (sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2 || sscanf(p, "+CEREG: %d", &stat) == 1)
        {
            last_cereg = stat;
            return stat;
        }
    }
    last_cereg = -1;
    return -1;
}

bool nb_iot::check_operator_name(char *oper_out, int max_len)
{
    // 1. 통신사 정보를 숫자(Numeric PLMN) 포맷으로 출력하도록 설정
    this->modem_SendCmd("AT+COPS=3,2");
    modem_sleep(500);
    this->modem_ReadResponse(); // OK 응답 소거

    // 2. 현재 가입/접속된 통신사 정보 조회
    this->modem_SendCmd("AT+COPS?");
    modem_sleep(1500);
    this->modem_ReadResponse();

    strcpy(oper_out, "Unknown");
    char *p = strstr(this->rx_buffer, "+COPS:");
    if (p != nullptr)
    {
        char *quote_start = strchr(p, '\"');
        if (quote_start != nullptr)
        {
            char *quote_end = strchr(quote_start + 1, '\"');
            if (quote_end != nullptr)
            {
                int len = quote_end - (quote_start + 1);
                if (len > 0)
                {
                    char plmn[32] = {0};
                    if (len < (int)sizeof(plmn))
                    {
                        memcpy(plmn, quote_start + 1, len);
                        plmn[len] = '\0';

                        // PLMN 코드를 친숙한 통신사 브랜드 명칭으로 치환
                        if (strcmp(plmn, "45005") == 0)
                        {
                            strncpy(oper_out, "SKT", max_len - 1);
                        }
                        else if (strcmp(plmn, "45008") == 0)
                        {
                            strncpy(oper_out, "KT", max_len - 1);
                        }
                        else if (strcmp(plmn, "45006") == 0 || strcmp(plmn, "45003") == 0)
                        {
                            strncpy(oper_out, "LGU+", max_len - 1);
                        }
                        else
                        {
                            // 알려지지 않은 국적/통신사는 PLMN 코드 그대로 저장
                            strncpy(oper_out, plmn, max_len - 1);
                        }
                        oper_out[max_len - 1] = '\0';

                        strncpy(carrier_name, oper_out, sizeof(carrier_name) - 1);
                        carrier_name[sizeof(carrier_name) - 1] = '\0';
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool nb_iot::retrieve_imei(char *imei_out)
{
    strcpy(imei_out, "000000000000000");
    this->modem_SendCmd("AT+CGSN");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char digits[20];
    int digit_count = 0;
    memset(digits, 0, sizeof(digits));

    for (int i = 0; this->rx_buffer[i] != '\0'; i++)
    {
        if (this->rx_buffer[i] >= '0' && this->rx_buffer[i] <= '9')
        {
            digits[digit_count++] = this->rx_buffer[i];
            if (digit_count == 15)
            {
                digits[15] = '\0';
                strcpy(imei_out, digits);
                strcpy(this->device_imei, digits);
                return true;
            }
        }
        else if (digit_count > 0)
        {
            digit_count = 0;
            memset(digits, 0, sizeof(digits));
        }
    }
    return false;
}

bool nb_iot::retrieve_cimi(char *cimi_out)
{
    strcpy(cimi_out, "000000000000000");
    this->modem_SendCmd("AT+CIMI");
    modem_sleep(2000);
    this->modem_ReadResponse();

    char digits[20];
    int digit_count = 0;
    memset(digits, 0, sizeof(digits));

    for (int i = 0; this->rx_buffer[i] != '\0'; i++)
    {
        if (this->rx_buffer[i] >= '0' && this->rx_buffer[i] <= '9')
        {
            digits[digit_count++] = this->rx_buffer[i];
            if (digit_count == 15)
            {
                digits[15] = '\0';
                strcpy(cimi_out, digits);
                strcpy(this->device_cimi, digits);
                return true;
            }
        }
        else if (digit_count > 0)
        {
            digit_count = 0;
            memset(digits, 0, sizeof(digits));
        }
    }
    return false;
}

uint32_t nb_iot::retrieve_network_time()
{
    this->modem_ClearRxBuffer();
    // CCLK 조회를 위해 AT+CCLK? 전송
    if (!this->modem_SendCmdWaitResponse("AT+CCLK?", "+CCLK:", nullptr, 2000)) {
        return 0;
    }

    // rx_buffer에서 "+CCLK:" 위치 찾기
    const char *pos = strstr(this->rx_buffer, "+CCLK:");
    if (!pos) return 0;

    pos = strchr(pos, '"');
    if (!pos) return 0;
    pos++; // 따옴표 내부 시각 문자열 시작

    int year, month, day, hour, minute, second;
    // yy/mm/dd,hh:mm:ss 파싱
    if (sscanf(pos, "%d/%d/%d,%d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
        struct tm t;
        t.tm_year = (year >= 70) ? (year) : (year + 100); // 26 -> 126 (2026년)
        t.tm_mon = month - 1; // 0-11
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = second;
        t.tm_isdst = -1;

        time_t epoch = mktime(&t);
        if (epoch != -1) {
            return (uint32_t)epoch;
        }
    }
    return 0;
}
