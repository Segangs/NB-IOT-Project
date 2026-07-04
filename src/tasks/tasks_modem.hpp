#ifndef TASKS_MODEM_HPP
#define TASKS_MODEM_HPP

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/uart.h"
#include <stdint.h>

// Cooperative yield sleep helper
void modem_sleep(uint32_t ms);

enum MqttConnectionState {
    LTE_DETACHED,
    LTE_ATTACHED,
    TLS_SOCKET_OPENING,
    TLS_SOCKET_OPEN,
    MQTT_CONNECTING,
    MQTT_CONNECTED,
    MQTT_SUBSCRIBED_CONFIG,
    MQTT_READY,
    MQTT_DISCONNECTED,
    MQTT_RECONNECT_WAIT
};

class nb_iot
{
private:
    char rx_buffer[1024];
    int buffer_idx;
    char device_imei[20];
    char device_cimi[20];
    char carrier_name[32];
    bool is_mqtt_connected;
    int last_csq;
    int last_cereg;
    int mqtt_session_id;
    MqttConnectionState mqtt_state;
    
public:
    nb_iot();
    ~nb_iot();
    
    bool is_unauthenticated;

    void modem_PacedWrite(const char *data);
    uint32_t retrieve_network_time();
        // Core Command controls with cooperative delays
    void modem_SendCmd(const char *cmd);
    void modem_SendCmdUserInput();
    void modem_ReadResponse(int check = 0);
    void modem_ClearRxBuffer();
    bool modem_SendCmdWaitOK(const char *cmd, uint32_t timeout_ms = 3000, uint32_t post_delay_ms = 2000);
    bool modem_SendCmdWaitResponse(const char *cmd, const char *expected_resp1, const char *expected_resp2 = nullptr, uint32_t timeout_ms = 3000);

    // Hardware power pulse sequences
    void modem_hw_power_on();
    bool modem_init(int &at_status, int &cpin_status);
    bool modem_configure_txon_indicator();
    
    // Status Checks
    bool check_at_alive();
    int check_sim_status();
    int check_rssi_csq();
    int check_network_registration();
    bool check_operator_name(char *oper_out, int max_len);
    bool retrieve_imei(char *imei_out);
    bool retrieve_cimi(char *cimi_out);

    // MQTTS (TLS 8883) Operations
    bool modem_MqttOpen(const char *host, const char *port, const char *client_id, const char *username, const char *password);
    bool modem_MqttPublish(const char *topic, const char *payload);
    bool modem_MqttSubscribe(const char *topic);
    bool modem_MqttPoll(uint32_t timeout_ms = 100);
    void modem_MqttClose();

    // Diagnostics Getters
    const char* get_rx_buffer() const { return rx_buffer; }
    const char* get_imei() const { return device_imei; }
    const char* get_cimi() const { return device_cimi; }
    const char* get_carrier() const { return carrier_name; }
    int get_last_csq() const { return last_csq; }
    int get_last_cereg() const { return last_cereg; }
    bool is_connected() const { return is_mqtt_connected; }
    MqttConnectionState get_mqtt_state() const { return mqtt_state; }
};

#endif // TASKS_MODEM_HPP
