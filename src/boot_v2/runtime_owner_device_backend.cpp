#include "runtime_owner_device_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "../config.h"
#include "../lib/log.hpp"
#include "../lib/mqtt_payload.hpp"
#include "../lib/flash_logger.hpp"
#include "../tasks/app_context.hpp"
#include "../tasks/tasks_modem.hpp"
#include "../tasks/tasks_sensor_reader.hpp"
#include "command_journal_flash_store.hpp"
#include "mqtt_power_event_codec.hpp"
#include "runtime_owner_producer_facade.hpp"
#include "runtime_owner_rtos.hpp"
#include "runtime_owner_shutdown_record_store.hpp"
#include "runtime_owner_transmit_indicator.hpp"
#include "sensor_quality_core.hpp"

extern nb_iot modem;

namespace boot_v2 {
namespace {

constexpr std::uint32_t kDiagnosticNotPrepared = 1001;
constexpr std::uint32_t kDiagnosticModemInit = 1002;
constexpr std::uint32_t kDiagnosticNetwork = 1003;
constexpr std::uint32_t kDiagnosticMqtt = 1004;
constexpr std::uint32_t kDiagnosticSubscription = 1005;
constexpr std::uint32_t kDiagnosticPublish = 1006;
constexpr std::uint32_t kDiagnosticPoll = 1007;
constexpr std::uint32_t kDiagnosticCounter = 1008;
constexpr std::uint32_t kDiagnosticInvalidCommand = 1009;
constexpr std::uint32_t kDiagnosticJournal = 1010;
constexpr std::uint32_t kCommandWaitMs = 6000;
constexpr std::uint32_t kCommandPollSliceMs = 100;
constexpr std::uint32_t kPowerEventRecoverySettleMs = 5000;
constexpr std::uint32_t kPowerEventAtProbeRetryMs = 3000;
constexpr std::uint8_t kPowerEventAtProbeAttempts = 3;
constexpr std::uint32_t kPowerEventShutdownRecoveryBudgetMs = 75000;

const char *configured_or(
    const char *configured,
    const char *fallback) noexcept
{
    return configured != nullptr && configured[0] != '\0'
               ? configured
               : fallback;
}

const char *mqtt_device_id() noexcept
{
    return configured_or(MQTT_DEVICE_ID, modem.get_imei());
}

const char *mqtt_username() noexcept
{
    return configured_or(MQTT_USERNAME, mqtt_device_id());
}

const char *mqtt_password() noexcept
{
    return configured_or(MQTT_PASSWORD, modem.get_cimi());
}

bool recover_power_event_transport() noexcept
{
    LOG("POWER_EVENT_RECOVERY_START\n");
    modem_sleep(kPowerEventRecoverySettleMs);

    bool at_alive = false;
    for (std::uint8_t attempt = 0;
         attempt < kPowerEventAtProbeAttempts;
         ++attempt) {
        LOG(
            "POWER_EVENT_AT_PROBE %u\n",
            static_cast<unsigned>(attempt + 1));
        if (modem.check_at_alive()) {
            at_alive = true;
            break;
        }
        if (attempt + 1 < kPowerEventAtProbeAttempts) {
            modem_sleep(kPowerEventAtProbeRetryMs);
        }
    }
    if (!at_alive) {
        LOG("POWER_EVENT_AT_UNAVAILABLE\n");
        return false;
    }

    const bool connected = modem.modem_MqttOpen(
        MQTT_BROKER_HOST,
        MQTT_BROKER_PORT,
        mqtt_device_id(),
        mqtt_username(),
        mqtt_password());
    LOG(
        connected
            ? "POWER_EVENT_RECOVERY_OK\n"
            : "POWER_EVENT_RECOVERY_FAIL\n");
    return connected;
}

bool publish_power_payload_with_recovery(
    const char *topic,
    const char *payload,
    const bool allow_recovery) noexcept
{
    if (modem.modem_MqttPublish(topic, payload)) {
        return true;
    }
    LOG("POWER_EVENT_PUBLISH_RETRY\n");
    if (!allow_recovery || !recover_power_event_transport()) {
        return false;
    }
    const bool published = modem.modem_MqttPublish(topic, payload);
    LOG(
        published
            ? "POWER_EVENT_RETRY_OK\n"
            : "POWER_EVENT_RETRY_FAIL\n");
    return published;
}

RuntimeOwnerPhysicalResult succeeded() noexcept
{
    RuntimeOwnerPhysicalResult result{};
    result.kind = RuntimeOwnerPhysicalResultKind::Succeeded;
    return result;
}

RuntimeOwnerPhysicalResult failed(const std::uint32_t diagnostic) noexcept
{
    RuntimeOwnerPhysicalResult result{};
    result.kind = RuntimeOwnerPhysicalResultKind::Failed;
    result.diagnostic_code = diagnostic;
    return result;
}

std::uint32_t remaining_until(const std::uint32_t deadline_ms) noexcept
{
    const std::uint32_t now =
        to_ms_since_boot(get_absolute_time());
    const std::int32_t delta =
        static_cast<std::int32_t>(deadline_ms - now);
    return delta > 0 ? static_cast<std::uint32_t>(delta) : 0;
}

RuntimeOwnerShutdownStepResult shutdown_at_command(
    const char *command,
    const std::uint32_t deadline_ms) noexcept
{
    const std::uint32_t available = remaining_until(deadline_ms);
    if (available <= 1000) {
        return RuntimeOwnerShutdownStepResult::TimedOut;
    }
    const std::uint32_t timeout =
        std::min<std::uint32_t>(3000, available - 1000);
    const bool response = modem.modem_SendCmdWaitResponse(
        command, "OK", "+CME ERROR:", timeout);
    const char *const rx = modem.get_rx_buffer();
    if (std::strstr(rx, "+CME ERROR: 910") != nullptr ||
        std::strstr(rx, "+CME ERROR: 916") != nullptr) {
        return RuntimeOwnerShutdownStepResult::Succeeded;
    }
    if (std::strstr(rx, "+CME ERROR: 922") != nullptr ||
        std::strstr(rx, "+CME ERROR: 924") != nullptr) {
        return RuntimeOwnerShutdownStepResult::Failed;
    }
    if (response && std::strstr(rx, "OK") != nullptr) {
        return RuntimeOwnerShutdownStepResult::Succeeded;
    }
    return remaining_until(deadline_ms) == 0
               ? RuntimeOwnerShutdownStepResult::TimedOut
               : RuntimeOwnerShutdownStepResult::Failed;
}

std::uint8_t shutdown_reason(
    const RuntimeOwnerUrgentSource source) noexcept
{
    switch (source) {
    case RuntimeOwnerUrgentSource::PowerButton:
        return 1;
    case RuntimeOwnerUrgentSource::AdapterLossCommitted:
        return 2;
    case RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand:
        return 3;
    case RuntimeOwnerUrgentSource::Invalid:
    default:
        return 0;
    }
}

} // namespace

RuntimeOwnerDeviceBackend::RuntimeOwnerDeviceBackend() noexcept
    : command_core_(command_journal_flash_port()),
      command_status_snapshot_(
          {this, sample_fresh_command_status_snapshot})
{
}

bool RuntimeOwnerDeviceBackend::prepare() noexcept
{
    CommandBootEffectEvidence evidence{};
    const RuntimeOwnerShutdownRecordV1 *const shutdown_record =
        runtime_owner_shutdown_record_current();
    if (shutdown_record != nullptr) {
        evidence.shutdown_record_present = 1;
        evidence.shutdown_record = *shutdown_record;
    }
    evidence.watchdog_marker_present =
        watchdog_hw->scratch[2] == COMMAND_WATCHDOG_SCRATCH_MAGIC
            ? 1
            : 0;
    evidence.watchdog_cmd_id = watchdog_hw->scratch[3];

    const CommandRuntimePrepareResult command_prepare =
        command_core_.prepare(evidence);
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[3] = 0;
    if (command_prepare == CommandRuntimePrepareResult::FailedClosed) {
        return false;
    }

    const CommandJournalRecord &prepared_command = command_core_.record();
    const bool recovered_success =
        command_prepare == CommandRuntimePrepareResult::ReadyRecovered &&
        prepared_command.state == CommandJournalState::Executed &&
        prepared_command.result == CommandResult::Executed &&
        prepared_command.error == CommandError::None;
    if (recovered_success &&
        (prepared_command.opcode == CommandOpcode::Reboot ||
         prepared_command.opcode == CommandOpcode::PowerOff)) {
        g_boot_reason_code =
            prepared_command.opcode == CommandOpcode::Reboot ? 1 : 3;
        g_boot_cmd_id = static_cast<int>(prepared_command.cmd_id);
    }

    const std::uint32_t boot_command_id =
        g_boot_cmd_id > 0
            ? static_cast<std::uint32_t>(g_boot_cmd_id)
            : 0;
    last_command_.reset_for_boot(
        boot_command_id,
        (g_boot_reason_code == 1 || g_boot_reason_code == 3) &&
            boot_command_id != 0);
    synchronize_last_command_from_runtime();
    gpio_init(MODEM_WAKEUP_PIN);
    gpio_set_dir(MODEM_WAKEUP_PIN, GPIO_OUT);
    gpio_put(MODEM_WAKEUP_PIN, 1);
    prepared_ = 1;
    return true;
}

bool RuntimeOwnerDeviceBackend::prepared() const noexcept
{
    return prepared_ != 0;
}

void RuntimeOwnerDeviceBackend::defer_config_commit(
    const std::uint32_t sequence) noexcept
{
    deferred_config_commit_sequence_ = sequence;
}

std::uint32_t RuntimeOwnerDeviceBackend::pending_config_commit_sequence()
    const noexcept
{
    return deferred_config_commit_sequence_;
}

void RuntimeOwnerDeviceBackend::clear_pending_config_commit() noexcept
{
    deferred_config_commit_sequence_ = 0;
}

CommandShutdownDispatchResult
RuntimeOwnerDeviceBackend::dispatch_authenticated_shutdown(
    void *const context,
    const CommandOpcode opcode,
    const std::uint32_t cmd_id) noexcept
{
    if (context == nullptr || cmd_id == 0) {
        return CommandShutdownDispatchResult::Rejected;
    }
    (void)static_cast<RuntimeOwnerDeviceBackend *>(context);

    RuntimeOwnerIngressResult ingress =
        RuntimeOwnerIngressResult::RejectedInvalid;
    if (opcode == CommandOpcode::Reboot) {
        ingress = runtime_owner_authenticated_request_reboot(
            cmd_id, cmd_id);
    } else if (opcode == CommandOpcode::PowerOff) {
        ingress = runtime_owner_authenticated_request_power_off(
            cmd_id, cmd_id);
    } else {
        return CommandShutdownDispatchResult::Rejected;
    }
    return ingress == RuntimeOwnerIngressResult::AcceptedForDelivery
               ? CommandShutdownDispatchResult::Accepted
               : CommandShutdownDispatchResult::Rejected;
}

void RuntimeOwnerDeviceBackend::synchronize_last_command_from_runtime()
    noexcept
{
    const CommandRuntimeTerminalCommand terminal =
        command_core_.last_terminal_command();
    if (terminal.cmd_id != 0) {
        (void)last_command_.observe_terminal(
            terminal.cmd_id,
            terminal.result,
            terminal.error);
    }
}

bool RuntimeOwnerDeviceBackend::validate_fresh_command_status_snapshot(
    void *const context) noexcept
{
    if (context == nullptr) {
        return false;
    }
    auto &backend =
        *static_cast<RuntimeOwnerDeviceBackend *>(context);
    RuntimeStatusSnapshotV1 snapshot{};
    return backend.command_status_snapshot_.validate_fresh(snapshot);
}

bool RuntimeOwnerDeviceBackend::sample_fresh_command_status_snapshot(
    void *const context,
    CommandStatusSnapshotSample &output) noexcept
{
    if (context == nullptr) {
        return false;
    }
    const auto &backend =
        *static_cast<RuntimeOwnerDeviceBackend *>(context);
    CommandStatusSnapshotSample sample{};
    sample.owner = runtime_owner_redacted_status();
    sample.metrics = runtime_owner_rtos_drain_metrics();
    sample.network_connected = modem.is_connected() ? 1 : 0;
    sample.battery_mode = lcd_params.is_battery_mode ? 1 : 0;
    sample.alarm_active =
        g_buzzer_trigger || g_buzzer_active ? 1 : 0;
    for (std::size_t index = 0; index < sample.sensors.size(); ++index) {
        if (!copy_sensor_quality_snapshot(
                static_cast<std::uint32_t>(index),
                sample.sensors[index])) {
            sample.sensors[index].health = SnapshotHealth::Failed;
            sample.sensors[index].stale = 1;
        }
    }
    sample.config_version =
        backend.config_commit_sequence_ == 0
            ? 1
            : backend.config_commit_sequence_;
    const RuntimeOwnerLastCommand last_command = backend.last_command_.value();
    sample.last_command_id = last_command.command_id;
    sample.last_command_result = last_command.result;
    output = sample;
    return true;
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::open_transport(
    const RuntimeOwnerExecutorCommand) noexcept
{
    lcd_params.is_modem_busy = true;
    if (modem_initialized_ == 0) {
        int at_status = 1;
        int cpin_status = 1;
        if (!modem.modem_init(at_status, cpin_status) || at_status != 0 ||
            cpin_status != 0) {
            lcd_params.is_modem_busy = false;
            return failed(kDiagnosticModemInit);
        }
        at_status_ = static_cast<std::uint8_t>(at_status);
        cpin_status_ = static_cast<std::uint8_t>(cpin_status);
        modem_initialized_ = 1;
    }

    bool network_ready = false;
    for (std::uint32_t attempt = 0; attempt < 45; ++attempt) {
        const int registration = modem.check_network_registration();
        const int csq = modem.check_rssi_csq();
        lcd_params.current_csq = csq;
        lcd_params.is_searching_network = csq == 99 || csq == 0;
        if ((registration == 1 || registration == 5) && csq > 0 &&
            csq != 99) {
            network_ready = true;
            break;
        }
        modem_sleep(2000);
    }
    if (!network_ready) {
        lcd_params.is_modem_busy = false;
        return failed(kDiagnosticNetwork);
    }

    const std::uint32_t network_time = modem.retrieve_network_time();
    if (network_time != 0) {
        const std::uint32_t elapsed =
            to_ms_since_boot(get_absolute_time()) / 1000;
        flash_log_set_boot_epoch(network_time - elapsed);
    }
    char operator_name[32]{};
    if (modem.check_operator_name(operator_name, sizeof(operator_name))) {
        if (std::strcmp(operator_name, "SKT") == 0) {
            operator_number_ = 1;
        } else if (std::strcmp(operator_name, "KT") == 0) {
            operator_number_ = 2;
        } else if (std::strcmp(operator_name, "LGU+") == 0) {
            operator_number_ = 3;
        }
    }

    if (!modem.modem_MqttOpen(
            MQTT_BROKER_HOST,
            MQTT_BROKER_PORT,
            mqtt_device_id(),
            mqtt_username(),
            mqtt_password())) {
        lcd_params.is_unauthenticated = modem.is_unauthenticated;
        lcd_params.is_modem_busy = false;
        return failed(kDiagnosticMqtt);
    }
    const RuntimeOwnerPhysicalResult boot_report = publish_boot_report();
    if (boot_report.kind != RuntimeOwnerPhysicalResultKind::Succeeded) {
        modem.modem_MqttDisconnect();
        lcd_params.is_modem_busy = false;
        return boot_report;
    }

    char topic[80]{};
    std::snprintf(topic, sizeof(topic), "devices/%s/config", mqtt_device_id());
    bool subscribed = modem.modem_MqttSubscribe(topic);
    std::snprintf(
        topic,
        sizeof(topic),
        "devices/%s/cmd/response",
        mqtt_device_id());
    subscribed = subscribed && modem.modem_MqttSubscribe(topic);
    std::snprintf(
        topic,
        sizeof(topic),
        "devices/%s/cmd/ack/receipt",
        mqtt_device_id());
    subscribed = subscribed && modem.modem_MqttSubscribe(topic);
    subscription_ready_ = subscribed ? 1 : 0;
    if (subscription_ready_ == 0) {
        modem.modem_MqttDisconnect();
        lcd_params.is_modem_busy = false;
        return failed(kDiagnosticSubscription);
    }

    const RuntimeOwnerPhysicalResult pulled = pull_config();
    if (pulled.kind != RuntimeOwnerPhysicalResultKind::Succeeded) {
        modem.modem_MqttDisconnect();
        lcd_params.is_modem_busy = false;
        return pulled;
    }
    if (config_commit_sequence_ ==
        std::numeric_limits<std::uint32_t>::max()) {
        lcd_params.is_modem_busy = false;
        return failed(kDiagnosticCounter);
    }
    ++config_commit_sequence_;
    RuntimeOwnerPhysicalResult result = succeeded();
    result.mqtt_session_id = static_cast<std::uint32_t>(
        modem.get_mqtt_session_id());
    result.config_commit_sequence = config_commit_sequence_;
    lcd_params.is_modem_busy = false;
    return result.mqtt_session_id == 0 ? failed(kDiagnosticMqtt) : result;
}

RuntimeOwnerPhysicalResult
RuntimeOwnerDeviceBackend::publish_boot_report() noexcept
{
    char topic[64]{};
    char payload[80]{};
    std::snprintf(topic, sizeof(topic), "devices/%s/boot", mqtt_device_id());
    const int written = std::snprintf(
        payload,
        sizeof(payload),
        "[%.1f,%.1f,%d,0,%u,%u,%d,%u,%d,%d,%d,%d,%d,%d]",
        static_cast<double>(lcd_params.current_vsys_voltage),
        static_cast<double>(g_boot_pico_temperature),
        static_cast<int>(g_boot_flash_integrity),
        static_cast<unsigned>(at_status_),
        static_cast<unsigned>(cpin_status_),
        modem.get_last_csq(),
        static_cast<unsigned>(operator_number_),
        lcd_params.status_ch0,
        lcd_params.status_ch1,
        g_mic1_stream_active ? 0 : 1,
        g_mic2_stream_active ? 0 : 1,
        g_boot_reason_code,
        g_boot_cmd_id);
    if (written <= 0 ||
        static_cast<std::size_t>(written) >= sizeof(payload)) {
        return failed(kDiagnosticInvalidCommand);
    }
    return modem.modem_MqttPublish(topic, payload)
               ? succeeded()
               : failed(kDiagnosticPublish);
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::publish_probe() noexcept
{
    if (!modem.is_connected()) {
        return failed(kDiagnosticMqtt);
    }
    char topic[64]{};
    std::snprintf(
        topic, sizeof(topic), "devices/%s/telemetry/probe", mqtt_device_id());
    LOG("LIVENESS_PROBE_PUB\n");
    const bool published = modem.modem_MqttPublish(topic, "{}");
    if (!published) {
        LOG("LIVENESS_STALL_DIAG_TRACE_CONTINUE\n");
        return failed(kDiagnosticPublish);
    }
    return succeeded();
}

RuntimeOwnerPhysicalResult
RuntimeOwnerDeviceBackend::verify_subscription() noexcept
{
    if (!modem.is_connected()) {
        subscription_ready_ = 0;
        return failed(kDiagnosticMqtt);
    }
    if (subscription_ready_ != 0) {
        return succeeded();
    }
    char topic[80]{};
    std::snprintf(topic, sizeof(topic), "devices/%s/config", mqtt_device_id());
    bool subscribed = modem.modem_MqttSubscribe(topic);
    std::snprintf(
        topic,
        sizeof(topic),
        "devices/%s/cmd/response",
        mqtt_device_id());
    subscribed = subscribed && modem.modem_MqttSubscribe(topic);
    std::snprintf(
        topic,
        sizeof(topic),
        "devices/%s/cmd/ack/receipt",
        mqtt_device_id());
    subscribed = subscribed && modem.modem_MqttSubscribe(topic);
    subscription_ready_ = subscribed ? 1 : 0;
    return subscription_ready_ != 0 ? succeeded()
                                    : failed(kDiagnosticSubscription);
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::pull_config() noexcept
{
    if (!modem.is_connected()) {
        return failed(kDiagnosticMqtt);
    }
    char response_topic[80]{};
    char request_topic[80]{};
    std::snprintf(
        response_topic,
        sizeof(response_topic),
        "devices/%s/config",
        mqtt_device_id());
    std::snprintf(
        request_topic,
        sizeof(request_topic),
        "devices/%s/config/request",
        mqtt_device_id());
    if (!modem.modem_MqttPublish(request_topic, "{}")) {
        return failed(kDiagnosticPublish);
    }

    LOG("CONFIG_WAIT_START\n");
    char payload[512]{};
    bool frame_started = false;
    for (std::uint32_t elapsed = 0; elapsed < 6000; elapsed += 100) {
        if (!modem.modem_MqttPoll(100)) {
            subscription_ready_ = 0;
            return failed(kDiagnosticPoll);
        }
        const char *const rx = modem.get_rx_buffer();
        if (!frame_started && std::strstr(rx, "+KMQTT_DATA:") != nullptr) {
            frame_started = true;
            LOG("CONFIG_FRAME_START %u BUFFER_BYTES=%u\n",
                static_cast<unsigned>(elapsed + 100),
                static_cast<unsigned>(std::strlen(rx)));
        }
        std::size_t frame_bytes = 0;
        std::size_t payload_bytes = 0;
        if (mqtt_kmqtt_data_extract_payload(
                rx,
                response_topic,
                payload,
                sizeof(payload),
                &frame_bytes,
                &payload_bytes)) {
            LOG("CONFIG_FRAME_COMPLETE %u BUFFER_BYTES=%u FRAME_BYTES=%u PAYLOAD_BYTES=%u\n",
                static_cast<unsigned>(elapsed + 100),
                static_cast<unsigned>(std::strlen(rx)),
                static_cast<unsigned>(frame_bytes),
                static_cast<unsigned>(payload_bytes));
            if (!apply_mqtt_config_payload(payload)) {
                LOG("CONFIG_APPLY_FAILED\n");
                return failed(kDiagnosticInvalidCommand);
            }
            return succeeded();
        }
    }
    LOG("CONFIG_FRAME_TIMEOUT %u\n",
        static_cast<unsigned>(std::strlen(modem.get_rx_buffer())));
    return failed(kDiagnosticPoll);
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::pull_command() noexcept
{
    if (!command_core_.ready()) {
        return failed(kDiagnosticJournal);
    }
    if (!modem.is_connected()) {
        return failed(kDiagnosticMqtt);
    }

    char request_topic[80]{};
    char response_topic[80]{};
    char ack_topic[80]{};
    char receipt_topic[80]{};
    std::snprintf(
        request_topic,
        sizeof(request_topic),
        "devices/%s/cmd/request",
        mqtt_device_id());
    std::snprintf(
        response_topic,
        sizeof(response_topic),
        "devices/%s/cmd/response",
        mqtt_device_id());
    std::snprintf(
        ack_topic,
        sizeof(ack_topic),
        "devices/%s/cmd/ack",
        mqtt_device_id());
    std::snprintf(
        receipt_topic,
        sizeof(receipt_topic),
        "devices/%s/cmd/ack/receipt",
        mqtt_device_id());

    if (command_core_.state() == CommandJournalState::Empty) {
        if (command_request_sequence_ ==
            std::numeric_limits<std::uint32_t>::max()) {
            return failed(kDiagnosticCounter);
        }
        ++command_request_sequence_;
        const std::uint32_t last_cmd_id =
            last_command_.value().command_id;
        if (!command_core_.begin_poll(
                command_request_sequence_, last_cmd_id)) {
            return failed(kDiagnosticInvalidCommand);
        }

        char request_payload[81]{};
        if (!mqtt_command_request_build(
                command_request_sequence_,
                last_cmd_id,
                request_payload,
                sizeof(request_payload)) ||
            !modem.modem_MqttPublish(request_topic, request_payload)) {
            return failed(kDiagnosticPublish);
        }

        char response_payload[81]{};
        CommandResponse response{};
        bool response_received = false;
        for (std::uint32_t elapsed = 0;
             elapsed < kCommandWaitMs;
             elapsed += kCommandPollSliceMs) {
            if (!modem.modem_MqttPoll(kCommandPollSliceMs)) {
                subscription_ready_ = 0;
                return failed(kDiagnosticPoll);
            }
            if (mqtt_command_topic_payload_extract(
                    modem.get_rx_buffer(),
                    response_topic,
                    response_payload,
                    sizeof(response_payload),
                    nullptr,
                    nullptr) &&
                mqtt_command_response_parse(
                    response_payload,
                    command_request_sequence_,
                    response) &&
                response.cmd_id <=
                    static_cast<std::uint32_t>(
                        std::numeric_limits<int>::max())) {
                response_received = true;
                break;
            }
        }
        if (!response_received) {
            return failed(kDiagnosticPoll);
        }

        const CommandAcceptResult accepted =
            command_core_.accept_response(
                response,
                to_ms_since_boot(get_absolute_time()) / 1000);
        synchronize_last_command_from_runtime();
        if (accepted == CommandAcceptResult::NoCommand) {
            return succeeded();
        }
        if (accepted != CommandAcceptResult::Accepted &&
            accepted != CommandAcceptResult::RejectedDuplicate) {
            return failed(kDiagnosticInvalidCommand);
        }
    }

    const auto publish_ack_and_wait_receipt =
        [&](const CommandAckPhase phase) noexcept -> bool {
        CommandAckMessage ack{};
        char ack_payload[81]{};
        if (command_core_.prepare_ack(ack) !=
                CommandTransitionResult::Accepted ||
            ack.phase != phase ||
            !mqtt_command_ack_build(
                ack, ack_payload, sizeof(ack_payload)) ||
            !modem.modem_MqttPublish(ack_topic, ack_payload) ||
            command_core_.record_puback(phase) !=
                CommandTransitionResult::Accepted) {
            return false;
        }

        char receipt_payload[81]{};
        for (std::uint32_t elapsed = 0;
             elapsed < kCommandWaitMs;
             elapsed += kCommandPollSliceMs) {
            if (!modem.modem_MqttPoll(kCommandPollSliceMs)) {
                subscription_ready_ = 0;
                return false;
            }
            if (!mqtt_command_topic_payload_extract(
                    modem.get_rx_buffer(),
                    receipt_topic,
                    receipt_payload,
                    sizeof(receipt_payload),
                    nullptr,
                    nullptr)) {
                continue;
            }
            CommandAckReceipt receipt{};
            if (!mqtt_command_ack_receipt_parse(
                    receipt_payload, receipt)) {
                continue;
            }
            const CommandTransitionResult receipt_result =
                command_core_.record_receipt(receipt);
            if (receipt_result ==
                CommandTransitionResult::Accepted) {
                return true;
            }
            if (receipt_result ==
                CommandTransitionResult::RejectedRemote) {
                return false;
            }
        }
        return false;
    };

    if (command_core_.state() <=
        CommandJournalState::AcceptedPuback) {
        if (!publish_ack_and_wait_receipt(
                CommandAckPhase::Accepted)) {
            return failed(kDiagnosticPoll);
        }
    }

    if (command_core_.state() ==
            CommandJournalState::AcceptedReceipted ||
        command_core_.state() == CommandJournalState::ExecuteMarked) {
        const CommandRuntimeExecutionResult execution =
            command_core_.execute_pending(
                to_ms_since_boot(get_absolute_time()) / 1000,
                {this,
                 validate_fresh_command_status_snapshot},
                {this, dispatch_authenticated_shutdown});
        synchronize_last_command_from_runtime();
        if (execution ==
                CommandRuntimeExecutionResult::ShutdownDispatched ||
            execution ==
                CommandRuntimeExecutionResult::AwaitingBootEffect) {
            return succeeded();
        }
        if (execution == CommandRuntimeExecutionResult::Rejected ||
            execution == CommandRuntimeExecutionResult::Deferred) {
            return failed(kDiagnosticJournal);
        }
    }

    if (command_core_.state() >= CommandJournalState::Executed &&
        command_core_.state() <= CommandJournalState::FinalPuback) {
        if (!publish_ack_and_wait_receipt(CommandAckPhase::Final)) {
            return failed(kDiagnosticPoll);
        }
    }
    if (command_core_.state() !=
        CommandJournalState::FinalReceipted) {
        return failed(kDiagnosticInvalidCommand);
    }

    const std::uint32_t completed_cmd_id =
        command_core_.record().cmd_id;
    if (command_core_.clear_final_receipted() !=
        CommandTransitionResult::Accepted) {
        return failed(kDiagnosticInvalidCommand);
    }
    synchronize_last_command_from_runtime();
    g_boot_cmd_id = static_cast<int>(completed_cmd_id);
    return succeeded();
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::freeze_snapshot(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    RuntimeOwnerPhysicalResult result = succeeded();
    BootRuntimeSnapshotV1 &snapshot = result.boot_snapshot;
    snapshot.health = SnapshotHealth::Pass;
    snapshot.last_completed_stage =
        BootCompletedStage::ConfigAppliedHandoff;
    snapshot.config_valid = 1;
    snapshot.transport_ready = 1;
    snapshot.subscription_alive = 1;
    snapshot.post_config_liveness = 0;
    snapshot.hardware_revision = 1;
    snapshot.firmware_build_id = 20260721;
    snapshot.config_version = config_commit_sequence_ == 0
                                  ? 1
                                  : config_commit_sequence_;
    SensorQualitySnapshotV1 temp1{};
    SensorQualitySnapshotV1 temp2{};
    if (!copy_sensor_quality_snapshot(0, temp1)) {
        temp1.health = SnapshotHealth::Failed;
        temp1.stale = 1;
    }
    if (!copy_sensor_quality_snapshot(1, temp2)) {
        temp2.health = SnapshotHealth::Failed;
        temp2.stale = 1;
    }
    snapshot.sensors = {temp1, temp2};
    snapshot.pdp_session_id = command.source.effect.attempt.mqtt_session_id;
    snapshot.mqtt_session_id = command.source.effect.attempt.mqtt_session_id;
    snapshot.mqtt_generation = command.source.effect.attempt.mqtt_generation;
    snapshot.config_apply_epoch =
        command.source.effect.attempt.config_apply_epoch;
    snapshot.health = combined_sensor_health(
        snapshot.sensors[0],
        g_mic1_stream_active,
        snapshot.sensors[1],
        g_mic2_stream_active);
    return result;
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::publish_telemetry(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    const NormalIntent intent = command.source.normal_intent;
    const std::uint32_t sensor_id = intent.subject_id;
    if (intent.kind != NormalIntentKind::PublishTelemetry ||
        !runtime_owner_normal_intent_is_canonical(intent) ||
        sensor_id > 2) {
        return failed(kDiagnosticInvalidCommand);
    }
    const float value =
        static_cast<float>(intent.value_deci_celsius) / 10.0f;
    char topic[64]{};
    char payload[64]{};
    std::snprintf(
        topic, sizeof(topic), "devices/%s/telemetry", mqtt_device_id());
    if (!mqtt_telemetry_payload_build(
            static_cast<int>(sensor_id), value, payload, sizeof(payload)) ||
        !mqtt_telemetry_payload_validate(payload, nullptr, nullptr)) {
        return failed(kDiagnosticInvalidCommand);
    }
    return modem.modem_MqttPublish(topic, payload)
               ? succeeded()
               : failed(kDiagnosticPublish);
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::publish_power_event(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    const NormalIntent intent = command.source.normal_intent;
    MqttPowerEvent event{};
    event.incident_id = intent.subject_id;
    event.sequence = intent.snapshot_revision;
    switch (command.kind) {
    case RuntimeOwnerDeviceOperationKind::PublishAdapterRemoved:
        event.event_type = 4;
        event.state_code = 1;
        event.value0 = 0;
        break;
    case RuntimeOwnerDeviceOperationKind::PublishAdapterRestored:
        event.event_type = 5;
        event.state_code = 0;
        event.value0 = 1;
        break;
    default:
        return failed(kDiagnosticInvalidCommand);
    }

    char topic[64]{};
    char payload[81]{};
    std::snprintf(
        topic, sizeof(topic), "devices/%s/event", mqtt_device_id());
    if (!mqtt_power_event_build(event, payload, sizeof(payload))) {
        return failed(kDiagnosticInvalidCommand);
    }
    return publish_power_payload_with_recovery(topic, payload, true)
               ? succeeded()
               : failed(kDiagnosticPublish);
}

RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::execute(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    if (prepared_ == 0) {
        return failed(kDiagnosticNotPrepared);
    }
    const RuntimeOwnerTransmitIndicatorScope transmit_indicator(
        lcd_params.is_transmitting,
        runtime_owner_operation_uses_transmit_indicator(command.kind));
    lcd_params.is_modem_busy = true;
    RuntimeOwnerPhysicalResult result{};
    switch (command.kind) {
    case RuntimeOwnerDeviceOperationKind::OpenTransport:
        return open_transport(command);
    case RuntimeOwnerDeviceOperationKind::ProbeAt:
        result = modem.check_at_alive() ? succeeded()
                                        : failed(kDiagnosticModemInit);
        break;
    case RuntimeOwnerDeviceOperationKind::PublishProbe:
        result = publish_probe();
        break;
    case RuntimeOwnerDeviceOperationKind::VerifySubscription:
        result = verify_subscription();
        break;
    case RuntimeOwnerDeviceOperationKind::PullFollowupConfig:
    case RuntimeOwnerDeviceOperationKind::PullConfig:
        result = pull_config();
        break;
    case RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot:
        result = freeze_snapshot(command);
        break;
    case RuntimeOwnerDeviceOperationKind::EndBootOrchestration:
        lcd_params.is_booting = false;
        std::strncpy(
            lcd_params.status_text,
            "Ready",
            sizeof(lcd_params.status_text) - 1);
        result = succeeded();
        break;
    case RuntimeOwnerDeviceOperationKind::RecordFault:
        LOG("RUNTIME_OWNER_FAULT\n");
        result = succeeded();
        break;
    case RuntimeOwnerDeviceOperationKind::EnterRecovery:
        subscription_ready_ = 0;
        if (modem.get_mqtt_session_id() > 0) {
            modem.modem_MqttDisconnect();
        }
        LOG("RUNTIME_OWNER_RECOVERY\n");
        result = succeeded();
        break;
    case RuntimeOwnerDeviceOperationKind::PublishTelemetry:
        result = publish_telemetry(command);
        break;
    case RuntimeOwnerDeviceOperationKind::PublishAdapterRemoved:
    case RuntimeOwnerDeviceOperationKind::PublishAdapterRestored:
        result = publish_power_event(command);
        break;
    case RuntimeOwnerDeviceOperationKind::RefreshRssi: {
        const int csq = modem.check_rssi_csq();
        lcd_params.current_csq = csq;
        lcd_params.is_searching_network = csq == 99 || csq == 0;
        result = csq > 0 && csq != 99
                     ? succeeded()
                     : failed(kDiagnosticNetwork);
        break;
    }
    case RuntimeOwnerDeviceOperationKind::PullCommand:
        result = pull_command();
        break;
    case RuntimeOwnerDeviceOperationKind::Invalid:
    default:
        result = failed(kDiagnosticInvalidCommand);
        break;
    }
    lcd_params.is_modem_busy = false;
    return result;
}

RuntimeOwnerShutdownStepResult
RuntimeOwnerDeviceBackend::execute_shutdown_cleanup(
    const RuntimeOwnerShutdownDirective &directive,
    const RuntimeOwnerUrgentMessage context) noexcept
{
    if (!runtime_owner_is_canonical_urgent(context) ||
        directive.action != RuntimeOwnerShutdownFinalizeAction::RunCleanupStep ||
        directive.remaining_ms == 0) {
        return RuntimeOwnerShutdownStepResult::TimedOut;
    }
    const RuntimeOwnerShutdownCleanupStep step = directive.step;
    const std::uint32_t remaining_ms = directive.remaining_ms;
    const std::uint32_t started_at =
        to_ms_since_boot(get_absolute_time());
    const std::uint32_t deadline_ms = started_at + remaining_ms;

    switch (step) {
    case RuntimeOwnerShutdownCleanupStep::StopOutputs:
        g_buzzer_trigger = false;
        g_buzzer_active = false;
        gpio_init(BUZZER_PIN);
        gpio_set_dir(BUZZER_PIN, GPIO_OUT);
        gpio_put(BUZZER_PIN, 0);
        lcd_params.is_transmitting = false;
        lcd_params.is_modem_busy = true;
        std::strncpy(
            lcd_params.status_text,
            "Power Off",
            sizeof(lcd_params.status_text) - 1);
        lcd_params.status_text[sizeof(lcd_params.status_text) - 1] = '\0';
        LOG("SHUTDOWN_OUTPUTS_STOPPED\n");
        return RuntimeOwnerShutdownStepResult::Succeeded;

    case RuntimeOwnerShutdownCleanupStep::PublishDying: {
        if (remaining_ms < 17000) {
            return RuntimeOwnerShutdownStepResult::TimedOut;
        }
        const RuntimeOwnerTransmitIndicatorScope transmit_indicator(
            lcd_params.is_transmitting, true);
        char topic[64]{};
        char payload[81]{};
        if (context.source ==
            RuntimeOwnerUrgentSource::AdapterLossCommitted) {
            std::snprintf(
                topic, sizeof(topic), "devices/%s/event", mqtt_device_id());
            const MqttPowerEvent event{
                6,
                context.incident_correlation_id,
                context.producer_sequence,
                2,
                210,
                90,
                0,
                0};
            if (!mqtt_power_event_build(
                    event, payload, sizeof(payload))) {
                return RuntimeOwnerShutdownStepResult::Failed;
            }
        } else {
            std::snprintf(
                topic, sizeof(topic), "devices/%s/status", mqtt_device_id());
            std::snprintf(
                payload,
                sizeof(payload),
                "[0,%u]",
                static_cast<unsigned>(shutdown_reason(context.source)));
        }
        const bool allow_recovery =
            context.source ==
                RuntimeOwnerUrgentSource::AdapterLossCommitted &&
            remaining_ms >= kPowerEventShutdownRecoveryBudgetMs;
        return publish_power_payload_with_recovery(
                   topic, payload, allow_recovery)
                   ? RuntimeOwnerShutdownStepResult::Succeeded
                   : RuntimeOwnerShutdownStepResult::Failed;
    }

    case RuntimeOwnerShutdownCleanupStep::CloseDeleteSessions: {
        bool failed_any = false;
        char command[40]{};
        for (int session_id = 1; session_id <= 6; ++session_id) {
            std::snprintf(
                command,
                sizeof(command),
                "AT+KMQTTCLOSE=%d",
                session_id);
            const RuntimeOwnerShutdownStepResult closed =
                shutdown_at_command(command, deadline_ms);
            if (closed == RuntimeOwnerShutdownStepResult::TimedOut) {
                return closed;
            }
            failed_any =
                failed_any ||
                closed == RuntimeOwnerShutdownStepResult::Failed;

            std::snprintf(
                command,
                sizeof(command),
                "AT+KMQTTDEL=%d",
                session_id);
            const RuntimeOwnerShutdownStepResult deleted =
                shutdown_at_command(command, deadline_ms);
            if (deleted == RuntimeOwnerShutdownStepResult::TimedOut) {
                return deleted;
            }
            failed_any =
                failed_any ||
                deleted == RuntimeOwnerShutdownStepResult::Failed;
        }
        return failed_any ? RuntimeOwnerShutdownStepResult::Failed
                          : RuntimeOwnerShutdownStepResult::Succeeded;
    }

    case RuntimeOwnerShutdownCleanupStep::ScanSessions:
        return shutdown_at_command("AT+KMQTTCFG?", deadline_ms);

    case RuntimeOwnerShutdownCleanupStep::DisconnectPdp:
        return shutdown_at_command("AT+KCNXDOWN=1", deadline_ms);

    case RuntimeOwnerShutdownCleanupStep::SetCfun0:
        return shutdown_at_command("AT+CFUN=0", deadline_ms);

    case RuntimeOwnerShutdownCleanupStep::PowerOffModem: {
        modem.modem_SendCmd("AT+CPWROFF");
        gpio_put(MODEM_WAKEUP_PIN, 0);
        const std::uint32_t wait_ms =
            std::min<std::uint32_t>(remaining_ms, 5000);
        bool power_off_accepted = false;
        for (std::uint32_t elapsed = 0; elapsed < wait_ms; ++elapsed) {
            modem_sleep(1);
            modem.modem_ReadResponse(0);
            const char *const rx = modem.get_rx_buffer();
            if (std::strstr(rx, "OK") != nullptr) {
                power_off_accepted = true;
                break;
            }
            if (std::strstr(rx, "ERROR") != nullptr) {
                return RuntimeOwnerShutdownStepResult::Failed;
            }
        }
        if (!power_off_accepted) {
            return RuntimeOwnerShutdownStepResult::TimedOut;
        }

        std::uint32_t available = remaining_until(deadline_ms);
        if (available <= MODEM_POWEROFF_FINALIZE_RESERVE_MS) {
            return RuntimeOwnerShutdownStepResult::TimedOut;
        }

        RuntimeOwnerShutdownRecordInput record_input{};
        record_input.producer_sequence = context.producer_sequence;
        record_input.incident_correlation_id =
            context.incident_correlation_id;
        record_input.elapsed_ms =
            SHUTDOWN_HARD_DEADLINE_MS - available;
        record_input.reason = shutdown_reason(context.source);
        record_input.initial_usb_present =
            directive.initial_usb_present;
        record_input.planned_action =
            (context.intent == RuntimeOwnerShutdownIntent::Reboot ||
             directive.initial_usb_present != 0)
                ? RuntimeOwnerShutdownPlannedAction::WatchdogReboot
                : RuntimeOwnerShutdownPlannedAction::Gp15Kill;
        record_input.hard_deadline_reached = directive.hard_deadline;
        const std::uint8_t poweroff_mask =
            runtime_owner_shutdown_step_mask(
                RuntimeOwnerShutdownCleanupStep::PowerOffModem);
        const std::uint8_t non_success_mask = static_cast<std::uint8_t>(
            directive.cleanup_failed_mask |
            directive.cleanup_timed_out_mask |
            directive.cleanup_skipped_mask);
        record_input.cleanup_succeeded_mask = static_cast<std::uint8_t>(
            0x7Fu & ~non_success_mask);
        record_input.cleanup_succeeded_mask =
            static_cast<std::uint8_t>(
                record_input.cleanup_succeeded_mask | poweroff_mask);
        record_input.cleanup_failed_mask =
            directive.cleanup_failed_mask;
        record_input.cleanup_timed_out_mask =
            directive.cleanup_timed_out_mask;
        record_input.cleanup_skipped_mask =
            directive.cleanup_skipped_mask;

        const std::uint32_t record_timeout_ms =
            std::min<std::uint32_t>(
                2000,
                available - MODEM_POWEROFF_FINALIZE_RESERVE_MS);
        const bool record_committed =
            runtime_owner_shutdown_record_commit(
                record_input, record_timeout_ms);
        LOG(
            record_committed ? "SHUTDOWN_RECORD_OK\n"
                             : "SHUTDOWN_RECORD_FAIL\n");

        available = remaining_until(deadline_ms);
        if (available <= MODEM_POWEROFF_FINALIZE_RESERVE_MS) {
            return RuntimeOwnerShutdownStepResult::TimedOut;
        }
        const std::uint32_t settle_ms =
            available - MODEM_POWEROFF_FINALIZE_RESERVE_MS;
        LOG("MODEM_POWEROFF_SETTLE %lu\n",
            static_cast<unsigned long>(settle_ms));
        modem_sleep(settle_ms);
        return record_committed
                   ? RuntimeOwnerShutdownStepResult::Succeeded
                   : RuntimeOwnerShutdownStepResult::Failed;
    }

    case RuntimeOwnerShutdownCleanupStep::Invalid:
    default:
        return RuntimeOwnerShutdownStepResult::Failed;
    }
}

} // namespace boot_v2
