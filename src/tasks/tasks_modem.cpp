#include "tasks_modem.hpp" 
#include "../config.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// FreeRTOS support
#include "FreeRTOS.h"
#include "task.h"

// Google Trust Services (GTS) Root R4 certificate for Cloudflare / Supabase API endpoints
static const char *GTS_ROOT_R4_CERT =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
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
    is_socket_open = false;
    last_csq = 99;
    last_cereg = -1;
    http_session_id = 1;

    memset(rx_buffer, 0, sizeof(rx_buffer));
    strcpy(device_imei, "0");
    strcpy(device_cimi, "0");
    strcpy(carrier_name, "Unknown");

    // Initialize UART0 using standard Pico SDK driver (proven 100% robust)
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

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

void nb_iot::modem_ClearRxBuffer()
{
    // UART 하드웨어 FIFO 버퍼에 대기 중인 잔여 바이트들을 완전히 읽어내어 버림으로써 이전 명령의 URC/에러 찌꺼기 유입을 차단합니다.
    while (uart_is_readable(UART_ID))
    {
        (void)uart_getc(UART_ID);
    }
    memset(this->rx_buffer, 0, sizeof(this->rx_buffer));
    this->buffer_idx = 0;
}

void nb_iot::modem_SendCmd(const char *cmd)
{
    this->modem_ClearRxBuffer(); // Clear RX buffer before character write

    // Send AT command
    uart_puts(UART_ID, cmd);
    uart_puts(UART_ID, "\r\n");

    printf("[AT Tx] %s\n", cmd);
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
    while (uart_is_readable(UART_ID))
    {
        if (this->buffer_idx < 1023)
        {
            char response = uart_getc(UART_ID);

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

            if (!check)
            {
                putchar(response);
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
}

bool nb_iot::modem_SendCmdWaitResponse(const char *cmd, const char *expected_resp1, const char *expected_resp2, uint32_t timeout_ms)
{
    this->modem_SendCmd(cmd);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;

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
    return false;
}

bool nb_iot::modem_SendCmdWaitOK(const char *cmd, uint32_t timeout_ms, uint32_t post_delay_ms)
{
    return this->modem_SendCmdWaitResponse(cmd, "OK", nullptr, timeout_ms);
}

void nb_iot::modem_hw_power_on()
{
    printf("[System] 모뎀 하드웨어 부팅 시퀀스 개시...\n");

    modem_sleep(5000);

    gpio_put(PWR_ON_PIN, 1);
    modem_sleep(500);

    gpio_put(PWR_ON_PIN, 0);
    modem_sleep(1000);

    gpio_put(PWR_ON_PIN, 1);
    modem_sleep(2000);

    gpio_put(PWR_ON_PIN, 0);
    printf("[System] 모뎀 OS 부팅 완료 대기 중 (30초)...\n");
    modem_sleep(30000);

    printf("[System] 모뎀 하드웨어 부팅 시퀀스 완료.\n");
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
    printf("[System] 115200bps 속도로 모뎀 접속 테스트 시도...\n");
    uart_set_baudrate(UART_ID, 115200);
    this->modem_ClearRxBuffer();
    
    for (int i = 0; i < 2; i++)
    {
        if (check_at_alive())
        {
            alive = true;
            printf("[System] 모뎀이 115200bps로 응답했습니다. 정상 동기화 완료.\n");
            break;
        }
        modem_sleep(1000);
    }
    
    // 2. 115200bps 응답이 없는 경우 9600bps로 접속한 뒤 115200bps로 상향 재지정
    if (!alive)
    {
        printf("[System] 9600bps 속도로 모뎀 접속 시도 및 115200bps 강제 상향 개시...\n");
        uart_set_baudrate(UART_ID, 9600);
        this->modem_ClearRxBuffer();
        
        for (int i = 0; i < 3; i++)
        {
            if (check_at_alive())
            {
                alive = true;
                printf("[System] 모뎀이 9600bps로 접속되었습니다. 115200bps로 속도를 전환합니다...\n");
                
                // 모뎀 통신 속도 115200bps 변경 명령 송출
                this->modem_SendCmd("AT+IPR=115200");
                modem_sleep(500);
                
                // 피코 UART도 115200bps로 상향 조정
                uart_set_baudrate(UART_ID, 115200);
                modem_sleep(500);
                
                // 115200bps 속도 전환 정상 완료 여부 검증
                if (check_at_alive())
                {
                    printf("[System] 모뎀 115200bps 전환 검증 성공. 설정을 영구 저장(AT&W)합니다...\n");
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
        printf("[Error] 모뎀이 응답하지 않습니다 (Baudrate 동기화 실패).\n");
        cpin_status = 1;
        return false;
    }

    // 1-1. ATE0 (Echo Off) 설정 및 2초 대기 (가장 먼저 전송!)
    printf("[System] ATE0 (Echo OFF) 전송 및 2초 대기...\n");
    modem_SendCmdWaitOK("ATE0", 3000, 2000);

    // Step 2: Enable detailed error reporting (+CMEE=1) 및 2초 대기
    modem_SendCmdWaitOK("AT+CMEE=1", 3000, 2000);

    // Step 2-1: 모뎀의 잔존 유령 HTTP 세션 일괄 강제 정리 (CME ERROR: 912 원천 차단)
    printf("[System] 모뎀 잔존 유령 HTTP 세션 강제 소거 개시 (1~10번)...\n");
    for (int sid = 1; sid <= 10; sid++)
    {
        char clean_cmd[64];
        snprintf(clean_cmd, sizeof(clean_cmd), "AT+KHTTPCLOSE=%d", sid);
        this->modem_SendCmdWaitResponse(clean_cmd, "OK", "910", 1000);
        modem_sleep(100);

        snprintf(clean_cmd, sizeof(clean_cmd), "AT+KHTTPDEL=%d", sid);
        this->modem_SendCmdWaitResponse(clean_cmd, "OK", "910", 1000);
        modem_sleep(100);
    }
    printf("[System] 모뎀 유령 HTTP 세션 소거 완료.\n");

    // Step 3: Turn on full modem capabilities and wait 30s as specified
    printf("[System] 모뎀 풀 기능 가동 중 (AT+CFUN=1) 후 30초 대기...\n");
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

    // Supabase Root 2021 CA 인증서 주입 시퀀스
    printf("[System] SSL Root CA 인증서 주입 개시 (0번 슬롯)...\n");
    this->modem_SendCmdWaitResponse("AT+KCERTDELETE=0,0", "OK", "ERROR", 3000);
    modem_sleep(1000);

    char cert_cmd[64];
    int cert_len = strlen(GTS_ROOT_R4_CERT);
    snprintf(cert_cmd, sizeof(cert_cmd), "AT+KCERTSTORE=0,%d,0", cert_len);

    if (this->modem_SendCmdWaitResponse(cert_cmd, "CONNECT", nullptr, 5000))
    {
        modem_sleep(200);
        this->modem_ClearRxBuffer();

        printf("[System] 인증서 데이터 스트림 안전 전송 시작 (%d 바이트)...\n", cert_len);
        for (int i = 0; i < cert_len; i++)
        {
            uart_putc(UART_ID, GTS_ROOT_R4_CERT[i]);
            sleep_us(200);
            if (i > 0 && i % 200 == 0)
            {
                printf("[%d/%d 바이트 송신 완료]\n", i, cert_len);
            }
        }
        printf("\n[System] 데이터 본문 전송 완료! 1.5초 대기 후 최종 OK 수집...\n");
        modem_sleep(1500);

        uint32_t elapsed = 0;
        const uint32_t step_ms = 100;
        bool store_ok = false;
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
        if (store_ok)
        {
            printf("\n[System] SSL Root CA 인증서 주입 및 저장 완료 (Index 0)!\n");
        }
        else
        {
            printf("\n[System] 에러: SSL Root CA 인증서 주입 실패.\n");
        }
    }
    else
    {
        printf("[System] 에러: SSL Root CA 인증서 주입 프롬프트(CONNECT) 획득 실패.\n");
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

bool nb_iot::modem_SocketOpen(const char *ip, const char *port)
{
    printf("[Socket] TCP 소켓 연결 프로세스 개시 (%s:%s)...\n", ip, port);
    this->modem_SendCmdWaitResponse("AT+KTCPCLOSE=1", "OK", "910", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitResponse("AT+KTCPDEL=1", "OK", "910", 5000);
    modem_sleep(1000);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+KCNXCFG=1,\"GPRS\",\"%s\"", APN_NAME);
    if (!this->modem_SendCmdWaitOK(cmd, 5000))
        return false;
    modem_sleep(1000);

    snprintf(cmd, sizeof(cmd), "AT+KTCPCFG=1,0,\"%s\",%s", ip, port);
    if (!this->modem_SendCmdWaitOK(cmd, 5000))
        return false;
    modem_sleep(1000);

    if (!this->modem_SendCmdWaitOK("AT+KTCPCNX=1", 10000))
        return false;

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool connection_established = false;
    this->modem_ClearRxBuffer();

    while (elapsed < 40000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, "+KTCP_IND: 1,1") != nullptr)
        {
            connection_established = true;
            break;
        }
        if (strstr(this->rx_buffer, "+KTCP_IND: 1,3") != nullptr || strstr(this->rx_buffer, "+KTCP_IND: 1,0") != nullptr)
        {
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!connection_established)
        return false;
    modem_sleep(1000);
    is_socket_open = true;
    return true;
}

bool nb_iot::modem_SocketSend(const char *data)
{
    if (!is_socket_open)
        return false;
    modem_sleep(1000);

    int data_len = strlen(data);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+KTCPSND=1,%d", data_len);
    if (!this->modem_SendCmdWaitResponse(cmd, "CONNECT", nullptr, 10000))
        return false;

    modem_sleep(5000);
    this->modem_PacedWrite(data);
    modem_sleep(1000);
    this->modem_PacedWrite("--EOF--Pattern--");

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool send_ok = false;
    while (elapsed < 10000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, "OK") != nullptr)
        {
            send_ok = true;
            break;
        }
        if (strstr(this->rx_buffer, "ERROR") != nullptr)
            break;
    }
    modem_sleep(1000);
    return send_ok;
}

void nb_iot::modem_SocketClose()
{
    if (!is_socket_open)
        return;
    this->modem_SendCmdWaitResponse("AT+KTCPCLOSE=1", "OK", "910", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitResponse("AT+KTCPDEL=1", "OK", "910", 5000);
    modem_sleep(1000);
    is_socket_open = false;
}

bool nb_iot::modem_HttpOpen(const char *host, const char *port)
{
    printf("[HTTPS] Supabase 직접 HTTPS 연동 시퀀스 개시 (%s:%s)...\n", host, port);
    char cnx_cmd[256];
    snprintf(cnx_cmd, sizeof(cnx_cmd), "AT+KCNXCFG=1,\"GPRS\",\"%s\"", APN_NAME);
    
    // GPRS 커넥션이 이미 활성화되어 있으면 ERROR가 발생하므로 실패를 리턴하지 않고 경고만 출력합니다.
    if (!this->modem_SendCmdWaitOK(cnx_cmd, 5000))
    {
        printf("[HTTPS] 경고: GPRS APN 설정 실패 (이미 활성화되었을 가능성 있음)\n");
    }
    modem_sleep(1000);

    if (!this->modem_SendCmdWaitOK("AT+KCNXPROFILE=1", 5000))
    {
        printf("[HTTPS] 경고: GPRS 프로필 활성화 실패 (이미 활성화되었을 가능성 있음)\n");
    }
    modem_sleep(1000);

    this->modem_SendCmd("AT+KCNXUP=1");
    uint32_t cnx_elapsed = 0;
    bool bearer_ok = false;

    while (cnx_elapsed < 20000)
    {
        modem_sleep(100);
        cnx_elapsed += 100;
        this->modem_ReadResponse(0);
        
        // 망 연결 성공 URC(+KCNX_IND: 1,1)가 확실히 수신될 때까지 안전하게 대기합니다.
        if (strstr(this->rx_buffer, "+KCNX_IND: 1,1") != nullptr)
        {
            bearer_ok = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }
    modem_sleep(2000);

    if (this->http_session_id > 0)
    {
        char close_cmd[64];
        snprintf(close_cmd, sizeof(close_cmd), "AT+KHTTPCLOSE=%d", this->http_session_id);
        this->modem_SendCmdWaitResponse(close_cmd, "OK", "910", 5000);
        modem_sleep(1000);

        char del_cmd[64];
        snprintf(del_cmd, sizeof(del_cmd), "AT+KHTTPDEL=%d", this->http_session_id);
        this->modem_SendCmdWaitResponse(del_cmd, "OK", "910", 5000);
        modem_sleep(1000);
    }

    this->modem_SendCmdWaitOK("AT+KSSLCFG=0,3", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitOK("AT+KSSLCRYPTO=0,8,3,25392,12,4,1,0", 5000);
    modem_sleep(1000);

    char cmd[384];
    snprintf(cmd, sizeof(cmd), "AT+KHTTPCFG=1,\"%s\",443,2,\"\",\"\",1,0,0", host);
    if (!this->modem_SendCmdWaitOK(cmd, 5000))
        return false;

    char *p = strstr(this->rx_buffer, "+KHTTPCFG:");
    if (p != nullptr)
    {
        int parsed_id = 0;
        if (sscanf(p, "+KHTTPCFG: %d", &parsed_id) == 1)
            this->http_session_id = parsed_id;
    }
    modem_sleep(1000);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool connected = false;
    char expected_urc[32], failure_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KHTTP_IND: %d,1", this->http_session_id);
    snprintf(failure_urc, sizeof(failure_urc), "+KHTTP_IND: %d,0", this->http_session_id);

    if (strstr(this->rx_buffer, expected_urc) != nullptr)
    {
        connected = true;
    }
    else
    {
        while (elapsed < 30000)
        {
            modem_sleep(step_ms);
            elapsed += step_ms;
            this->modem_ReadResponse(0);
            if (strstr(this->rx_buffer, expected_urc) != nullptr)
            {
                connected = true;
                break;
            }
            if (strstr(this->rx_buffer, failure_urc) != nullptr)
                break;
            if (this->buffer_idx > 800)
                this->modem_ClearRxBuffer();
        }
    }

    if (!connected)
        return false;
    modem_sleep(1000);
    is_socket_open = true;
    return true;
}

bool nb_iot::modem_HttpPost(const char *request_uri, const char *payload, char *response_buf, size_t response_buf_len)
{
    if (!is_socket_open) return false;

    // 1. 보낼 JSON 본문의 길이를 바이트 단위로 정확히 계산
    int payload_len = strlen(payload); 

    printf("[HTTPS] Send Payload Body: %s (Length: %d)\n", payload, payload_len);
    printf("[HTTPS] HTTP 헤더 주입 개시 (AT+KHTTPHEADER=%d)...\n", this->http_session_id);

    char header_cmd[64];
    snprintf(header_cmd, sizeof(header_cmd), "AT+KHTTPHEADER=%d", this->http_session_id);
    if (!this->modem_SendCmdWaitResponse(header_cmd, "CONNECT", nullptr, 5000)) return false;
    modem_sleep(200);

    // 2. 헤더에 Content-Length 및 Prefer: return=representation 추가!!
    char header_buf[512];
    snprintf(header_buf, sizeof(header_buf),
             "apikey: %s\r\n"
             "Authorization: Bearer %s\r\n"
             "Content-Type: application/json\r\n"
             "Prefer: return=representation\r\n"
             "Content-Length: %d\r\n\r\n",
             SUPABASE_ANON_KEY, SUPABASE_ANON_KEY, payload_len);

    printf("[HTTPS] 헤더 주입 송출:\n%s", header_buf);
    this->modem_PacedWrite(header_buf);
    modem_sleep(200);

    // 3. 헤더 마감 패턴 송출
    this->modem_PacedWrite("--EOF--Pattern--");
    modem_sleep(500);


    // 종료 패턴 역시 모뎀이 처리할 수 있도록 대기 추가
    modem_sleep(1000);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool header_ok = false;
    while (elapsed < 5000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, "OK") != nullptr)
        {
            header_ok = true;
            break;
        }
    }

    if (!header_ok)
    {
        printf("[HTTPS] 에러: HTTP 헤더 완료(OK) 수신 실패.\n");
        return false;
    }
    modem_sleep(1000);

    // // 디버그: 주입된 헤더 현황 확인
    // printf("[HTTPS] 주입된 헤더 현황 조회 (AT+KHTTPHEADER?)... \n");
    // this->modem_SendCmd("AT+KHTTPHEADER?");
    // modem_sleep(1000);
    // this->modem_ReadResponse(0);
    // modem_sleep(1000);

    // Step 2: HTTP POST 요청 개시 (포맷을 0으로 설정하여 무의미한 헤더 덤프를 차단하고 상태 라인만 간결히 수신)
    printf("[HTTPS] HTTP POST 요청 개시 (Endpoint: %s, Session: %d)...\n", request_uri, this->http_session_id);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+KHTTPPOST=%d,,\"%s\",0", this->http_session_id, request_uri);

    if (!this->modem_SendCmdWaitResponse(cmd, "CONNECT", nullptr, 10000))
    {
        printf("[HTTPS] 에러: HTTP POST 요청 프롬프트(CONNECT) 획득 실패.\n");
        return false;
    }

    // modem_sleep(200);

    // printf("[HTTPS] Payload 본문 송출\n");
    // this->modem_PacedWrite(payload);
    // modem_sleep(100);

    // printf("[HTTPS] --EOF--Pattern-- 전송\n");
    // this->modem_PacedWrite("--EOF--Pattern--");

    // // ====================================================================================
    // // [수정 포인트 3] 페이로드 송출 직후 수신 버퍼 조기 소거(Clear) 코드 제거 및 수신 조건 개선
    // // 모뎀이 응답 URC(+KHTTP_IND)를 띄우기도 전에 버퍼를 지우던 치명적 버그 수정
    // // ====================================================================================
    // elapsed = 0;
    // bool post_ok = false;
    // bool response_status_2xx = false;

    // 데이터 모드 진입 안착을 위한 200ms 정밀 대기
    modem_sleep(200);
    
    // ====================================================================================
    // [변경 부분] Payload 본문 송출 시 시리얼 모니터에 데이터 실시간 출력 시각화
    // ====================================================================================
    printf("[HTTPS] >>> Payload 본문 송출 시작 >>>\n");
    
    printf("%s\n", payload); // 시리얼 모니터에 MCU가 보낼 JSON 본문을 그대로 출력 
    // 모뎀으로 실제 데이터 전송
    //this->modem_PacedWrite(payload);
    char total_packet[1024];
// JSON 본문 바로 뒤에 공백/개행 없이 EOF 패턴을 붙여버림
snprintf(total_packet, sizeof(total_packet), "%s--EOF--Pattern--", payload);

// 모뎀으로 통째로 한 번에 전송
this->modem_PacedWrite(total_packet);
    //modem_sleep(100);
    
    // 본문 전송 완료 (EOF 패턴 전송)
    
    //this->modem_PacedWrite("--EOF--Pattern--");
    printf("[HTTPS] [Tx Pattern] --EOF--Pattern--\n"); // 패턴 송출 현황 시각화
    
    // ====================================================================================
    
    // OK 및 HTTP 전송 완료 URC 대기 (이하 원본 로직 유지)
    elapsed = 0;
    bool post_ok = false;
    bool response_status_2xx = false;








    // ★ 기존 원본에 있던 이 위치의 this->modem_ClearRxBuffer() 제거 완료!
    // (서버가 보낸 패킷이 읽히기도 전에 삭제되는 현상 방지)

    char status_prefix[32];
    snprintf(status_prefix, sizeof(status_prefix), "+KHTTP_IND: %d,3,", this->http_session_id);

    uint32_t elapsed_ms = 0;
    while (elapsed_ms < 15000)
    {
        modem_sleep(2); // 2ms polling sleep to prevent overflow
        elapsed_ms += 2;
        this->modem_ReadResponse(0);

        // [Fast Pass] 서버의 2xx 응답 헤더(HTTP/1.1 200, 201, 204 등)가 감지되면 즉시 성공 처리 후 탈출합니다.
        if (strstr(this->rx_buffer, "HTTP/1.1 204") != nullptr)
        {
            response_status_2xx = true;
            post_ok = true; 
            printf("\n[HTTPS] HTTP 204 No Content 감지 성공 (Fast Pass)!\n");
            break;
        }
        else if (strstr(this->rx_buffer, "HTTP/1.1 2") != nullptr)
        {
            response_status_2xx = true;
            post_ok = true; 
            printf("\n[HTTPS] HTTP 2xx 응답 감지 성공 (Fast Pass)!\n");
            break;
        }

        if (strstr(this->rx_buffer, "OK") != nullptr)
        {
            post_ok = true;
        }

        char *urc_p = strstr(this->rx_buffer, status_prefix);
        if (urc_p != nullptr)
        {
            // status_prefix는 "+KHTTP_IND: <session_id>,3," 형태이므로, 
            // 그 뒤에는 "data_len,status_code,..."가 연결됩니다. (예: "191,204,\"No Content\"")
            char *data_len_start = urc_p + strlen(status_prefix);
            char *comma = strchr(data_len_start, ',');
            if (comma != nullptr)
            {
                // comma + 1 은 status_code의 시작점을 가리킵니다. (예: "204,\"No Content\"")
                int status_code = atoi(comma + 1);
                if (status_code >= 200 && status_code < 300)
                {
                    response_status_2xx = true;
                    post_ok = true; // 서버의 HTTP 성공 응답은 최종 데이터 송신 처리가 완료됨을 뜻하므로 post_ok 도 강제 승인합니다.
                    break;
                }
                else
                {
                    printf("\n[HTTPS] non-2xx URC (Status: %d). Waiting for dump...\n", status_code);
                    for (int f = 0; f < 1000; f++) {
                        modem_sleep(2);
                        this->modem_ReadResponse(0);
                    }
                    break;
                }
            }
        }

        if (this->buffer_idx > 950)
        {
            // 버퍼를 완전히 비우면 유입 중인 URC 문자열 중간이 찢겨나갈 수 있으므로,
            // 최근 150바이트만 남기고 앞으로 복사하여 URC 조각 유실을 방지합니다.
            memmove(this->rx_buffer, this->rx_buffer + 800, this->buffer_idx - 800);
            this->buffer_idx -= 800;
            this->rx_buffer[this->buffer_idx] = '\0';
        }
    }

    modem_sleep(1000);

    // 💡 [구제책] URC가 시리얼 노이즈로 인해 깨진 경우라도, 
    // Supabase의 정상 응답 데이터 형식(예: "sensorId" 또는 "cmd")이 버퍼에 이미 존재하고
    // 최종적으로 모뎀으로부터 "OK"를 수신했다면 성공으로 간주하여 구제합니다.
    if (!response_status_2xx && post_ok)
    {
        if (strstr(this->rx_buffer, "sensorId") != nullptr || strstr(this->rx_buffer, "cmd") != nullptr)
        {
            response_status_2xx = true;
            printf("\n[HTTPS] 경고: URC 찌그러짐/누락이 발생했으나, 수신 버퍼 내 Supabase 데이터 및 OK 매칭으로 성공 구제 완료!\n");
        }
    }

    if (post_ok && response_status_2xx)
    {
        printf("[HTTPS] Supabase 데이터 직접 적재 성공!\n");
        
        // Extract JSON response if buffer is provided
        if (response_buf && response_buf_len > 0)
        {
            response_buf[0] = '\0';
            const char *json_start = strchr(this->rx_buffer, '[');
            while (json_start != nullptr)
            {
                const char *json_end = strchr(json_start, ']');
                if (json_end != nullptr)
                {
                    size_t len = json_end - json_start + 1;
                    if (len < response_buf_len)
                    {
                        // Copy to temporary buffer to check
                        char temp[256];
                        strncpy(temp, json_start, len);
                        temp[len] = '\0';
                        
                        // JSON 형식이 수집되면 무조건 수집 (cmd 필터 완화)
                        strcpy(response_buf, temp);
                        printf("[HTTPS] Parsed response body: %s\n", response_buf);
                        break;
                    }
                    json_start = strchr(json_end + 1, '[');
                }
                else
                {
                    break;
                }
            }
        }

        this->modem_ClearRxBuffer(); // 프로세스 완료 후 버퍼 정리
        return true;
    }
    else
    {
        printf("[HTTPS] 에러: 데이터 송출 완료 혹은 HTTP 2xx 응답 확인 실패.\n");
        this->modem_ClearRxBuffer();
        return false;
    }
}

void nb_iot::modem_HttpClose()
{
    // 소켓이 비정상적으로 닫히거나 연결 도중 실패하여 is_socket_open이 false여도, 
    // http_session_id가 존재한다면 모뎀의 세션 누수 방지를 위해 정리 절차를 강제 수행합니다.
    if (!is_socket_open && this->http_session_id <= 0)
        return;

    printf("[HTTPS] HTTPS 세션 정리 개시...\n");
    char close_cmd[64];
    snprintf(close_cmd, sizeof(close_cmd), "AT+KHTTPCLOSE=%d", this->http_session_id);
    this->modem_SendCmdWaitResponse(close_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    char del_cmd[64];
    snprintf(del_cmd, sizeof(del_cmd), "AT+KHTTPDEL=%d", this->http_session_id);
    this->modem_SendCmdWaitResponse(del_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    is_socket_open = false;
    printf("[HTTPS] HTTPS 세션 정리 완료.\n");
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