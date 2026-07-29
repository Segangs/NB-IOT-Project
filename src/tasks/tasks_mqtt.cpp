#include "tasks_modem.hpp"
#include "tasks_debug.hpp"
#include "../config.h"
#include "../lib/log.hpp"
#include "hardware/watchdog.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace {

void trim_command(char *command)
{
    size_t length = strlen(command);
    while (length > 0 &&
           (command[length - 1] == ' ' || command[length - 1] == '\t' ||
            command[length - 1] == '\r' || command[length - 1] == '\n')) {
        command[--length] = '\0';
    }
}

bool command_equals(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower(static_cast<unsigned char>(*left)) !=
            tolower(static_cast<unsigned char>(*right))) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

bool command_has_at_prefix(const char *command)
{
    return command != nullptr &&
           tolower(static_cast<unsigned char>(command[0])) == 'a' &&
           tolower(static_cast<unsigned char>(command[1])) == 't';
}

} // namespace

void nb_iot::modem_MqttResetAllSessions()
{
    LOG("MQTT_SESSION_RESET_ALL\n");
    this->is_mqtt_connected = false;
    this->mqtt_state = MQTT_DISCONNECTED;
    const bool previous_settle_bypass = this->at_command_settle_bypass;
    this->at_command_settle_bypass = true;
    for (int session_id = 1; session_id <= 6; ++session_id)
    {
        char command[64];
        snprintf(command, sizeof(command), "AT+KMQTTCLOSE=%d", session_id);
        this->modem_SendCmdWaitResponse(command, "OK", "910", 2000);
        modem_sleep(100);

        snprintf(command, sizeof(command), "AT+KMQTTDEL=%d", session_id);
        this->modem_SendCmdWaitResponse(command, "OK", "910", 2000);
        modem_sleep(100);
    }
    this->at_command_settle_bypass = previous_settle_bypass;
    this->mqtt_session_id = 0;
    LOG("MQTT_SESSION_RESET_ALL_OK\n");
}

void nb_iot::modem_MqttCloseDeleteSession(const int session_id)
{
    if (session_id <= 0) return;

    LOG("MQTT_SESSION_TARGET_CLEAN %d\n", session_id);
    char command[64];
    snprintf(command, sizeof(command), "AT+KMQTTCLOSE=%d", session_id);
    this->modem_SendCmdWaitResponse(command, "OK", "910", 5000);
    modem_sleep(1000);

    snprintf(command, sizeof(command), "AT+KMQTTDEL=%d", session_id);
    this->modem_SendCmdWaitResponse(command, "OK", "910", 5000);
    modem_sleep(1000);

    if (this->mqtt_session_id == session_id) {
        this->mqtt_session_id = 0;
    }
    this->is_mqtt_connected = false;
    this->mqtt_state = MQTT_DISCONNECTED;
    LOG("MQTT_SESSION_TARGET_CLEAN_OK %d\n", session_id);
}

void nb_iot::modem_MqttBootCleanStart()
{
    if (this->mqtt_boot_cleanup_done) return;

    LOG("MQTT_BOOT_CLEAN\n");
    this->modem_MqttResetAllSessions();
    this->mqtt_boot_cleanup_done = true;
    LOG("MQTT_BOOT_CLEAN_OK\n");
}

void nb_iot::modem_MqttDisconnect()
{
    if (this->mqtt_session_id <= 0) {
        this->is_mqtt_connected = false;
        this->mqtt_state = MQTT_DISCONNECTED;
        return;
    }

    const int session_id = this->mqtt_session_id;
    LOG("MQTT_DISCONNECT %d\n", session_id);
    char command[64];
    snprintf(command, sizeof(command), "AT+KMQTTCLOSE=%d", session_id);
    const bool close_completed = this->modem_SendCmdWaitResponse(
        command, "OK", "910", 10000);
    const bool session_gone =
        strstr(this->rx_buffer, "+CME ERROR: 910") != nullptr ||
        strstr(this->rx_buffer, "+CME ERROR: 916") != nullptr;
    if (session_gone) {
        this->mqtt_session_id = 0;
        LOG("MQTT_DISCONNECT_SESSION_GONE %d\n", session_id);
    } else if (close_completed) {
        LOG("MQTT_DISCONNECT_KEEP_SESSION %d\n", session_id);
    } else {
        LOG("MQTT_DISCONNECT_TIMEOUT_KEEP_SESSION %d\n", session_id);
    }
    this->is_mqtt_connected = false;
    this->mqtt_state = MQTT_RECONNECT_WAIT;
}

[[noreturn]] void nb_iot::modem_ManualAtConsole()
{
    LOG("MANUAL_AT_READY input=USB terminator=CR reboot=LOCAL\n");
    char command[512]{};
    size_t length = 0;
    bool input_overflow = false;

    for (;;) {
        const int input = getchar_timeout_us(0);
        if (input != PICO_ERROR_TIMEOUT) {
            const char character = static_cast<char>(input);
            if (character == '\r' || character == '\n') {
                if (input_overflow) {
                    LOG("MANUAL_AT_REJECT reason=INPUT_TOO_LONG\n");
                } else if (length != 0) {
                    command[length] = '\0';
                    trim_command(command);
                    if (command_equals(command, "reboot")) {
                        LOG("MANUAL_AT_REBOOT\n");
                        watchdog_reboot(0, 0, 100);
                    } else if (command_has_at_prefix(command)) {
                        this->modem_SendCmd(command);
                    } else {
                        LOG("MANUAL_AT_REJECT reason=AT_PREFIX_REQUIRED\n");
                    }
                }
                length = 0;
                input_overflow = false;
                memset(command, 0, sizeof(command));
            } else if (character == '\b' || input == 127) {
                if (!input_overflow && length != 0) {
                    command[--length] = '\0';
                }
            } else if (length < sizeof(command) - 1) {
                command[length++] = character;
            } else {
                input_overflow = true;
            }
        }
        this->modem_ReadResponse(0);
        modem_sleep(1);
    }
}

bool nb_iot::modem_MqttOpen(const char *host, const char *port, const char *client_id, const char *username, const char *password)
{
    LOG("MQTT_CONNECT\n");
    this->is_unauthenticated = false;
    this->is_mqtt_connected = false;

    if (!this->mqtt_boot_cleanup_done) {
        this->modem_MqttBootCleanStart();
#if defined(NB_IOT_MANUAL_AT_SESSION_RESET_TRIAL)
        manual_at_mode_enable();
        this->modem_ManualAtConsole();
#endif
    }

    if (this->mqtt_session_id > 0)
    {
        if (this->modem_MqttReconnect()) return true;
        if (this->is_unauthenticated) return false;

        const int stale_session_id = this->mqtt_session_id;
        if (stale_session_id > 0) {
            this->modem_MqttCloseDeleteSession(stale_session_id);
        }
    }

    this->mqtt_state = LTE_DETACHED;

    char cnx_cmd[256];
    snprintf(cnx_cmd, sizeof(cnx_cmd), "AT+KCNXCFG=1,\"GPRS\",\"%s\"", APN_NAME);

    if (!this->modem_SendCmdWaitOK(cnx_cmd, 5000))
    {
        LOG("GPRS_APN_WARN\n");
    }
    modem_sleep(1000);

    if (!this->modem_SendCmdWaitOK("AT+KCNXPROFILE=1", 5000))
    {
        LOG("GPRS_PROFILE_WARN\n");
    }
    modem_sleep(1000);

    this->modem_SendCmd("AT+KCNXUP=1");
    uint32_t cnx_elapsed = 0;
    this->mqtt_state = LTE_ATTACHED;

    while (cnx_elapsed < 20000)
    {
        modem_sleep(100);
        cnx_elapsed += 100;
        this->modem_ReadResponse(0);

        if (strstr(this->rx_buffer, "+KCNX_IND: 1,1") != nullptr)
        {
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }
    modem_sleep(2000);

    LOG("TLS_CFG\n");
    this->mqtt_state = TLS_SOCKET_OPENING;
    this->modem_SendCmdWaitOK("AT+KSSLCFG=0,3", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitOK("AT+KSSLCFG=2,0", 5000);
    modem_sleep(1000);
    this->modem_SendCmdWaitOK("AT+KSSLCRYPTO=0,8,3,25392,12,4,1,0", 5000);
    modem_sleep(1000);

    char cfg_cmd[512];
    snprintf(cfg_cmd, sizeof(cfg_cmd),
        "AT+KMQTTCFG=1,1,\"%s\",%s,4,\"%s\",120,1,0,\"\",\"\",0,0,\"%s\",\"%s\",0",
        host, port, client_id, username, password);

    if (!this->modem_SendCmdWaitOK(cfg_cmd, 5000))
    {
        LOG("MQTT_CFG_LAST_RESORT_RESET\n");
        this->modem_MqttResetAllSessions();
        if (!this->modem_SendCmdWaitOK(cfg_cmd, 5000))
        {
            LOG("MQTT_CFG_FAIL\n");
            this->mqtt_state = MQTT_RECONNECT_WAIT;
            return false;
        }
    }
    this->mqtt_state = TLS_SOCKET_OPEN;

    char *p = strstr(this->rx_buffer, "+KMQTTCFG:");
    if (p != nullptr)
    {
        int parsed_id = 0;
        if (sscanf(p, "+KMQTTCFG: %d", &parsed_id) == 1)
            this->mqtt_session_id = parsed_id;
    }
    else
    {
        this->mqtt_session_id = 1;
    }
    modem_sleep(1000);

    if (this->modem_MqttReconnect()) return true;
    if (!this->is_unauthenticated && this->mqtt_session_id > 0) {
        const int stale_session_id = this->mqtt_session_id;
        this->modem_MqttCloseDeleteSession(stale_session_id);
    }
    return false;
}

bool nb_iot::modem_MqttReconnect()
{
    if (this->mqtt_session_id <= 0) return false;

    LOG("MQTT_RECONNECT %d\n", this->mqtt_session_id);
    this->is_mqtt_connected = false;
    char cnx_mqtt[64];
    snprintf(cnx_mqtt, sizeof(cnx_mqtt), "AT+KMQTTCNX=%d", this->mqtt_session_id);
    this->mqtt_state = MQTT_CONNECTING;
    this->modem_SendCmd(cnx_mqtt);

    uint32_t elapsed = 0;
    const uint32_t step_ms = 100;
    bool connected = false;
    char expected_urc[32], failure_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KMQTT_IND: %d,1", this->mqtt_session_id);
    snprintf(failure_urc, sizeof(failure_urc), "+KMQTT_IND: %d,0", this->mqtt_session_id);

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
        {
            LOG("MQTT_CONNECT_ABORT\n");
            this->mqtt_state = MQTT_RECONNECT_WAIT;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!connected)
    {
        if (elapsed >= 30000)
        {
            this->modem_MarkAtCommandTimeout();
        }
        LOG("MQTT_CONNECT_FAIL\n");
        if (strstr(this->rx_buffer, "+CME ERROR: 907") != nullptr)
        {
            LOG("MQTT_AUTH_FAIL\n");
            this->is_unauthenticated = true;
        }
        if (strstr(this->rx_buffer, "+CME ERROR: 910") != nullptr ||
            strstr(this->rx_buffer, "+CME ERROR: 916") != nullptr)
        {
            this->mqtt_session_id = 0;
        }
        this->mqtt_state = MQTT_RECONNECT_WAIT;
        return false;
    }

    LOG("MQTT_CONNECT_OK\n");
    this->is_mqtt_connected = true;
    this->mqtt_state = MQTT_CONNECTED;
    return true;
}

bool nb_iot::modem_MqttPublish(const char *topic, const char *payload)
{
    if (!this->is_mqtt_connected || this->mqtt_session_id == 0) return false;

    int payload_len = strlen(payload);
    if (payload_len > 80)
    {
        LOG("MQTT_PAYLOAD_TOO_BIG %d\n", payload_len);
        return false;
    }

    LOG("MQTT_PUB\n");

    char pub_cmd[512];
    snprintf(pub_cmd, sizeof(pub_cmd), "AT+KMQTTPUB=%d,\"%s\",1,0,\"%s\"", this->mqtt_session_id, topic, payload);

    char expected_urc[32], abort_urc[32], generic_error_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KMQTT_IND: %d,4", this->mqtt_session_id);
    snprintf(abort_urc, sizeof(abort_urc), "+KMQTT_IND: %d,0", this->mqtt_session_id);
    snprintf(generic_error_urc, sizeof(generic_error_urc), "+KMQTT_IND: %d,5", this->mqtt_session_id);

    if (!this->modem_SendCmdWaitOK(pub_cmd, 5000, 0))
    {
        const bool silent_command_timeout = this->buffer_idx == 0;
        const bool liveness_probe =
            strstr(topic, "/telemetry/probe") != nullptr;
        if (strstr(this->rx_buffer, expected_urc) != nullptr)
        {
            LOG("MQTT_PUB_EARLY_ACK\n");
            LOG("MQTT_PUB_OK\n");
            return true;
        }
        const char *const cme = strstr(this->rx_buffer, "+CME ERROR:");
        int cme_error = 0;
        if (cme != nullptr && sscanf(cme, "+CME ERROR: %d", &cme_error) == 1)
        {
            LOG("MQTT_PUB_CME %d\n", cme_error);
        }
        else if (strstr(this->rx_buffer, "ERROR") != nullptr)
        {
            LOG("MQTT_PUB_GENERIC_ERROR\n");
        }
        else
        {
            int response_class = 0;
            if (strstr(this->rx_buffer, abort_urc) != nullptr)
            {
                response_class = 1;
            }
            else if (strstr(this->rx_buffer, generic_error_urc) != nullptr)
            {
                response_class = 5;
            }
            else if (strstr(this->rx_buffer, "+KMQTT_DATA:") != nullptr)
            {
                response_class = 6;
            }
            else if (this->buffer_idx > 0)
            {
                response_class = 9;
            }
            LOG("MQTT_PUB_NO_OK RX_BYTES=%d CLASS=%d\n",
                this->buffer_idx,
                response_class);
        }
        LOG("MQTT_PUB_CMD_FAIL\n");
        LOG("MQTT_PUB_FAIL\n");
        if (this->at_trace_enabled && silent_command_timeout &&
            liveness_probe)
        {
            LOG("MQTT_PUB_STALL_DIAG_AT_START\n");
            const bool at_ok = this->modem_SendCmdWaitOK("AT", 5000);
            if (at_ok)
            {
                LOG("MQTT_PUB_STALL_DIAG_AT_OK\n");
            }
            else if (strstr(this->rx_buffer, "ERROR") != nullptr)
            {
                LOG("MQTT_PUB_STALL_DIAG_AT_ERROR\n");
            }
            else
            {
                LOG("MQTT_PUB_STALL_DIAG_AT_TIMEOUT\n");
            }
        }
        this->is_mqtt_connected = false;
        this->mqtt_state = MQTT_RECONNECT_WAIT;
        return false;
    }

    uint32_t elapsed = 0;
    bool pub_success = false;
    const uint32_t step_ms = 1;

    while (elapsed < 10000)
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, expected_urc) != nullptr)
        {
            pub_success = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!pub_success)
    {
        this->modem_MarkAtCommandTimeout();
        LOG("MQTT_PUB_ACK_TIMEOUT\n");
        LOG("MQTT_PUB_FAIL\n");
        this->is_mqtt_connected = false;
        this->mqtt_state = MQTT_RECONNECT_WAIT;
        return false;
    }

    LOG("MQTT_PUB_OK\n");
    return true;
}

bool nb_iot::modem_MqttSubscribe(const char *topic)
{
    if (!this->is_mqtt_connected || this->mqtt_session_id == 0) return false;

    LOG("MQTT_SUB\n");

#if defined(NB_IOT_CONFIG_QOS0_TRIAL)
    constexpr int kConfigSubscriptionQos = 0;
#else
    constexpr int kConfigSubscriptionQos = 1;
#endif
    char sub_cmd[256];
    snprintf(
        sub_cmd,
        sizeof(sub_cmd),
        "AT+KMQTTSUB=%d,\"%s\",%d",
        this->mqtt_session_id, topic, kConfigSubscriptionQos);

    if (!this->modem_SendCmdWaitOK(sub_cmd, 5000))
    {
        LOG("MQTT_SUB_FAIL\n");
        this->is_mqtt_connected = false;
        this->mqtt_state = MQTT_RECONNECT_WAIT;
        return false;
    }

    uint32_t elapsed = 0;
    bool sub_success = false;
    char expected_urc[32];
    snprintf(expected_urc, sizeof(expected_urc), "+KMQTT_IND: %d,2", this->mqtt_session_id);

    while (elapsed < 10000)
    {
        modem_sleep(100);
        elapsed += 100;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, expected_urc) != nullptr)
        {
            sub_success = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!sub_success)
    {
        this->modem_MarkAtCommandTimeout();
        LOG("MQTT_SUB_FAIL\n");
        this->is_mqtt_connected = false;
        this->mqtt_state = MQTT_RECONNECT_WAIT;
        return false;
    }

    LOG("MQTT_SUB_OK\n");
    this->mqtt_state = MQTT_SUBSCRIBED_CONFIG;
    return true;
}

bool nb_iot::modem_MqttPoll(uint32_t timeout_ms)
{
    if (!this->is_mqtt_connected || this->mqtt_session_id == 0) return false;

    uint32_t elapsed = 0;
    const uint32_t step_ms = 1;
    char abort_urc[32], generic_error_urc[32];
    snprintf(abort_urc, sizeof(abort_urc), "+KMQTT_IND: %d,0", this->mqtt_session_id);
    snprintf(generic_error_urc, sizeof(generic_error_urc), "+KMQTT_IND: %d,5", this->mqtt_session_id);

    do
    {
        modem_sleep(step_ms);
        elapsed += step_ms;
        this->modem_ReadResponse(0);

        if (strstr(this->rx_buffer, abort_urc) != nullptr || strstr(this->rx_buffer, generic_error_urc) != nullptr)
        {
            LOG("MQTT_DISCONNECTED\n");
            this->is_mqtt_connected = false;
            this->mqtt_state = MQTT_RECONNECT_WAIT;
            return false;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    } while (elapsed < timeout_ms);

    if (this->mqtt_state == MQTT_SUBSCRIBED_CONFIG || this->mqtt_state == MQTT_CONNECTED)
    {
        this->mqtt_state = MQTT_READY;
    }
    return true;
}

void nb_iot::modem_MqttClose()
{
    if (this->mqtt_session_id == 0) return;

    LOG("MQTT_CLOSE\n");

    char close_cmd[64];
    snprintf(close_cmd, sizeof(close_cmd), "AT+KMQTTCLOSE=%d", this->mqtt_session_id);
    this->modem_SendCmdWaitResponse(close_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    char del_cmd[64];
    snprintf(del_cmd, sizeof(del_cmd), "AT+KMQTTDEL=%d", this->mqtt_session_id);
    this->modem_SendCmdWaitResponse(del_cmd, "OK", "910", 5000);
    modem_sleep(1000);

    this->mqtt_session_id = 0;
    this->is_mqtt_connected = false;
    this->mqtt_state = MQTT_DISCONNECTED;
    LOG("MQTT_CLOSE_OK\n");
}
