#include "tasks_modem.hpp"
#include "../config.h"
#include "../lib/log.hpp"
#include <stdio.h>
#include <string.h>

bool nb_iot::modem_MqttOpen(const char *host, const char *port, const char *client_id, const char *username, const char *password)
{
    LOG("MQTT_CONNECT\n");
    this->is_unauthenticated = false;
    this->is_mqtt_connected = false;
    this->mqtt_state = LTE_DETACHED;

    auto reset_mqtt_sessions = [this]() {
        LOG("MQTT_SESSION_RESET\n");
        this->mqtt_state = MQTT_DISCONNECTED;
        for (int sid = 1; sid <= 6; sid++)
        {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+KMQTTCLOSE=%d", sid);
            this->modem_SendCmdWaitResponse(cmd, "OK", "ERROR", 1500);
            modem_sleep(100);

            snprintf(cmd, sizeof(cmd), "AT+KMQTTDEL=%d", sid);
            this->modem_SendCmdWaitResponse(cmd, "OK", "ERROR", 1500);
            modem_sleep(100);
        }
        this->mqtt_session_id = 0;
        LOG("MQTT_SESSION_RESET_OK\n");
    };

    reset_mqtt_sessions();

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
        LOG("MQTT_CFG_RETRY\n");
        reset_mqtt_sessions();
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
            LOG("MQTT_AUTH_FAIL\n");
            this->is_unauthenticated = true;
            this->mqtt_state = MQTT_RECONNECT_WAIT;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!connected)
    {
        LOG("MQTT_CONNECT_FAIL\n");
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

    if (!this->modem_SendCmdWaitOK(pub_cmd, 5000))
    {
        LOG("MQTT_PUB_FAIL\n");
        this->mqtt_state = MQTT_RECONNECT_WAIT;
        return false;
    }

    uint32_t elapsed = 0;
    bool pub_success = false;
    char expected_urc1[32], expected_urc2[32];
    snprintf(expected_urc1, sizeof(expected_urc1), "+KMQTT_IND: %d,4", this->mqtt_session_id);
    snprintf(expected_urc2, sizeof(expected_urc2), "+KMQTT_IND: %d,3", this->mqtt_session_id);

    while (elapsed < 10000)
    {
        modem_sleep(100);
        elapsed += 100;
        this->modem_ReadResponse(0);
        if (strstr(this->rx_buffer, expected_urc1) != nullptr || strstr(this->rx_buffer, expected_urc2) != nullptr)
        {
            pub_success = true;
            break;
        }
        if (this->buffer_idx > 800)
            this->modem_ClearRxBuffer();
    }

    if (!pub_success)
    {
        LOG("MQTT_PUB_FAIL\n");
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

    char sub_cmd[256];
    snprintf(sub_cmd, sizeof(sub_cmd), "AT+KMQTTSUB=%d,\"%s\",1", this->mqtt_session_id, topic);

    if (!this->modem_SendCmdWaitOK(sub_cmd, 5000))
    {
        LOG("MQTT_SUB_FAIL\n");
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
        LOG("MQTT_SUB_FAIL\n");
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
    const uint32_t step_ms = 50;
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
