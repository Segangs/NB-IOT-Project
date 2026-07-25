#include "runtime_owner_device_backend.hpp"

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>
#include <type_traits>

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(const bool condition, const char *expression, const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, line,
                     expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

std::string read_file(const char *path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::size_t count(const std::string &source, const std::string &needle)
{
    std::size_t found = 0;
    std::size_t offset = 0;
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++found;
        offset += needle.size();
    }
    return found;
}

bool is_cpp_identifier_char(const char value) noexcept
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

std::size_t count_identifier(
    const std::string &source,
    const std::string &identifier)
{
    std::size_t found = 0;
    std::size_t offset = 0;
    while ((offset = source.find(identifier, offset)) != std::string::npos) {
        const bool starts_at_boundary =
            offset == 0 || !is_cpp_identifier_char(source[offset - 1]);
        const std::size_t after = offset + identifier.size();
        const bool ends_at_boundary =
            after == source.size() || !is_cpp_identifier_char(source[after]);
        if (starts_at_boundary && ends_at_boundary) {
            ++found;
        }
        offset = after;
    }
    return found;
}

std::string section(
    const std::string &source,
    const char *begin,
    const char *end)
{
    const std::size_t first = source.find(begin);
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = source.find(end, first + 1);
    if (last == std::string::npos) {
        return {};
    }
    return source.substr(first, last - first);
}

std::string replace_once_copy(
    const std::string &source,
    const std::string &from,
    const std::string &to)
{
    std::string result = source;
    const std::size_t position = result.find(from);
    if (position != std::string::npos) {
        result.replace(position, from.size(), to);
    }
    return result;
}

bool compact_config_apply_contract_accepts(const std::string &apply) noexcept
{
    constexpr const char *forbidden[] = {
        "extract_json_int",
        "parse_sensors_json",
        "extract_json_float",
        "g_temp_lower_limit",
        ".temp_lower_limit",
        "watchdog_reboot",
        "safe_reboot",
        "safe_power_off",
        "taskENTER_CRITICAL",
        "gpio_put",
        "CMD_REBOOT",
        "LIMIT_UPDATE",
        "LOW_LIMIT_UPDATE",
    };
    for (const char *needle : forbidden) {
        if (apply.find(needle) != std::string::npos) {
            return false;
        }
    }

    const std::size_t extract_guard =
        apply.find("if (!mqtt_config_payload_extract_object(");
    const std::size_t extract_ignore =
        apply.find("CONFIG_IGNORE", extract_guard);
    const std::size_t extract_failure_return =
        apply.find("return false;", extract_ignore);
    const std::size_t parser_guard =
        apply.find("if (!mqtt_config_compact_limits_parse(",
                   extract_failure_return);
    const std::size_t parser_ignore =
        apply.find("CONFIG_IGNORE", parser_guard);
    const std::size_t parser_failure_return =
        apply.find("return false;", parser_ignore);
    const std::size_t sensor0 =
        apply.find("g_sensors[0].temp_upper_limit = temp1_upper");
    const std::size_t sensor1 =
        apply.find("g_sensors[1].temp_upper_limit = temp2_upper");
    const std::size_t channel0 =
        apply.find("g_temp_upper_limit_ch0 = temp1_upper");
    const std::size_t channel1 =
        apply.find("g_temp_upper_limit_ch1 = temp2_upper");
    const std::size_t legacy =
        apply.find("g_temp_upper_limit = temp1_upper");
    const std::size_t success_log = apply.find("CONFIG_LIMIT_OK");
    const std::size_t success_return = apply.find("return true;");
    return extract_guard != std::string::npos &&
           extract_ignore != std::string::npos &&
           extract_failure_return != std::string::npos &&
           parser_guard != std::string::npos &&
           parser_ignore != std::string::npos &&
           parser_failure_return != std::string::npos &&
           sensor0 != std::string::npos &&
           sensor1 != std::string::npos &&
           channel0 != std::string::npos &&
           channel1 != std::string::npos &&
           legacy != std::string::npos &&
           success_log != std::string::npos &&
           success_return != std::string::npos &&
           count(apply, "mqtt_config_payload_extract_object") == 1 &&
           count(apply, "mqtt_config_compact_limits_parse") == 1 &&
           count(apply, "CONFIG_IGNORE") == 2 &&
           count(apply, "return false;") == 2 &&
           count(apply, "g_sensors[0].temp_upper_limit") == 1 &&
           count(apply, "g_sensors[1].temp_upper_limit") == 1 &&
           count_identifier(apply, "g_temp_upper_limit_ch0") == 1 &&
           count_identifier(apply, "g_temp_upper_limit_ch1") == 1 &&
           count_identifier(apply, "g_temp_upper_limit") == 1 &&
           count(apply, "g_sensors[0].temp_upper_limit = temp1_upper") == 1 &&
           count(apply, "g_sensors[1].temp_upper_limit = temp2_upper") == 1 &&
           count(apply, "g_temp_upper_limit_ch0 = temp1_upper") == 1 &&
           count(apply, "g_temp_upper_limit_ch1 = temp2_upper") == 1 &&
           count(apply, "g_temp_upper_limit = temp1_upper") == 1 &&
           count(apply, "CONFIG_LIMIT_OK") == 1 &&
           count(apply, "return true;") == 1 &&
           extract_guard < extract_ignore &&
           extract_ignore < extract_failure_return &&
           extract_failure_return < parser_guard &&
           parser_guard < parser_ignore &&
           parser_ignore < parser_failure_return &&
           parser_failure_return < sensor0 &&
           parser_failure_return < sensor1 &&
           parser_failure_return < channel0 &&
           parser_failure_return < channel1 &&
           parser_failure_return < legacy &&
           sensor0 < success_log && sensor1 < success_log &&
           channel0 < success_log && channel1 < success_log &&
           legacy < success_log && success_log < success_return;
}

bool pull_config_apply_contract_accepts(
    const std::string &pull_config) noexcept
{
    if (pull_config.find("(void)apply_mqtt_config_payload(payload)") !=
        std::string::npos) {
        return false;
    }

    const std::size_t apply_guard =
        pull_config.find("if (!apply_mqtt_config_payload(payload))");
    const std::size_t apply_failure_log =
        pull_config.find("CONFIG_APPLY_FAILED", apply_guard);
    const std::size_t apply_failure_return = pull_config.find(
        "return failed(kDiagnosticInvalidCommand);", apply_failure_log);
    const std::size_t success_return =
        pull_config.find("return succeeded();", apply_failure_return);
    return apply_guard != std::string::npos &&
           apply_failure_log != std::string::npos &&
           apply_failure_return != std::string::npos &&
           success_return != std::string::npos &&
           count(pull_config, "apply_mqtt_config_payload(payload)") == 1 &&
           count(pull_config, "if (!apply_mqtt_config_payload(payload))") ==
               1 &&
           count(pull_config, "CONFIG_APPLY_FAILED") == 1 &&
           pull_config.find("extract_json_int") == std::string::npos &&
           pull_config.find("allow_authenticated_command") ==
               std::string::npos &&
           pull_config.find("runtime_owner_authenticated_request_shutdown") ==
               std::string::npos &&
           apply_guard < apply_failure_log &&
           apply_failure_log < apply_failure_return &&
           apply_failure_return < success_return;
}

bool dedicated_command_contract_accepts(
    const std::string &pull_command) noexcept
{
    constexpr const char *required[] = {
        "devices/%s/cmd/request",
        "devices/%s/cmd/response",
        "devices/%s/cmd/ack",
        "devices/%s/cmd/ack/receipt",
        "mqtt_command_request_build",
        "mqtt_command_response_parse",
        "mqtt_command_ack_build",
        "mqtt_command_ack_receipt_parse",
        "command_core_.begin_poll",
        "command_core_.accept_response",
        "command_core_.prepare_ack",
        "command_core_.record_puback",
        "command_core_.record_receipt",
        "command_core_.mark_execute",
        "command_core_.complete_execution",
        "command_core_.clear_final_receipted",
    };
    for (const char *needle : required) {
        if (pull_command.find(needle) == std::string::npos) {
            return false;
        }
    }
    constexpr const char *forbidden[] = {
        "apply_mqtt_config_payload",
        "extract_json_int",
        "parse_sensors_json",
        "allow_authenticated_command",
        "watchdog_reboot",
        "gpio_put(POWER_KILL_PIN",
        "runtime_owner_authenticated_request_shutdown",
    };
    for (const char *needle : forbidden) {
        if (pull_command.find(needle) != std::string::npos) {
            return false;
        }
    }
    return true;
}

void test_header_contract() noexcept
{
    using boot_v2::RuntimeOwnerDeviceBackend;
    CHECK(std::is_default_constructible<RuntimeOwnerDeviceBackend>::value);
    CHECK(!std::is_copy_constructible<RuntimeOwnerDeviceBackend>::value);
    CHECK(!std::is_copy_assignable<RuntimeOwnerDeviceBackend>::value);
}

void test_single_firmware_backend_graph() noexcept
{
    const std::string root = read_file(NB_IOT_SOURCE_ROOT "/CMakeLists.txt");
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    CHECK(!root.empty());
    CHECK(!source.empty());
    CHECK(count(root, "src/boot_v2/runtime_owner_device_backend.cpp") == 1);
    CHECK(count(root, "src/boot_v2/runtime_owner_producer_facade.cpp") == 1);
    CHECK(count(root, "src/boot_v2/runtime_owner_cutover_core.cpp") == 1);
    CHECK(count(
              root,
              "src/boot_v2/runtime_owner_shutdown_record_core.cpp") == 1);
    CHECK(count(
              root,
              "src/boot_v2/runtime_owner_shutdown_record_store.cpp") == 1);
    CHECK(count(root, "src/boot_v2/mqtt_command_codec.cpp") == 1);
    CHECK(count(root, "src/boot_v2/command_ack_core.cpp") == 1);
    CHECK(source.find("modem.") != std::string::npos);
    CHECK(source.find("modem_MqttOpen") != std::string::npos);
    CHECK(source.find("modem_MqttPublish") != std::string::npos);
    CHECK(source.find("modem_MqttSubscribe") != std::string::npos);
    CHECK(source.find("modem_MqttPoll") != std::string::npos);
    CHECK(source.find("HTTP") == std::string::npos);
    CHECK(source.find("AT+KHTTP") == std::string::npos);
    CHECK(source.find("safe_power_off") == std::string::npos);
    CHECK(source.find("safe_reboot") == std::string::npos);
    CHECK(source.find("gpio_put(POWER_KILL_PIN") == std::string::npos);
    CHECK(source.find("watchdog_reboot") == std::string::npos);
    CHECK(source.find(
              "(void)runtime_owner_authenticated_request_shutdown") ==
          std::string::npos);
    CHECK(source.find("runtime_owner_authenticated_request_shutdown") ==
          std::string::npos);
}

void test_mqtt_poll_drains_uart_before_fifo_can_overflow() noexcept
{
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string modem = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.cpp");
    CHECK(mqtt.find("bool nb_iot::modem_MqttPoll") != std::string::npos);
    CHECK(mqtt.find("const uint32_t step_ms = 1;") != std::string::npos);
    CHECK(mqtt.find("const uint32_t step_ms = 50;") == std::string::npos);
    CHECK(modem.find("int max_bytes = 256;") != std::string::npos);
    const std::string response_reader = section(
        modem,
        "void nb_iot::modem_ReadResponse",
        "bool nb_iot::modem_SendCmdWaitResponse");
    CHECK(!response_reader.empty());
    CHECK(response_reader.find("putchar(response)") == std::string::npos);
}

void test_mqtt_publish_drains_uart_during_command_and_puback_waits() noexcept
{
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string modem = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.cpp");
    const std::string command_wait = section(
        modem,
        "bool nb_iot::modem_SendCmdWaitResponse",
        "bool nb_iot::modem_SendCmdWaitOK");
    const std::string publish_wait = section(
        mqtt,
        "bool nb_iot::modem_MqttPublish",
        "bool nb_iot::modem_MqttSubscribe");
    CHECK(!command_wait.empty());
    CHECK(!publish_wait.empty());
    CHECK(command_wait.find("const uint32_t step_ms = 1;") !=
          std::string::npos);
    CHECK(publish_wait.find("const uint32_t step_ms = 1;") !=
          std::string::npos);
    CHECK(command_wait.find("modem_sleep(step_ms);") != std::string::npos);
    CHECK(publish_wait.find("modem_sleep(step_ms);") != std::string::npos);
    CHECK(publish_wait.find("elapsed += step_ms;") != std::string::npos);
}

void test_mqtt_publish_failure_reports_redacted_modem_reason() noexcept
{
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string publish = section(
        mqtt,
        "bool nb_iot::modem_MqttPublish",
        "bool nb_iot::modem_MqttSubscribe");
    CHECK(!publish.empty());
    CHECK(publish.find("+CME ERROR:") != std::string::npos);
    CHECK(publish.find("MQTT_PUB_CME %d") != std::string::npos);
    CHECK(publish.find("MQTT_PUB_GENERIC_ERROR") != std::string::npos);
    CHECK(publish.find("MQTT_PUB_EARLY_ACK") != std::string::npos);
    CHECK(publish.find("MQTT_PUB_NO_OK RX_BYTES=%d CLASS=%d") !=
          std::string::npos);
    CHECK(publish.find("LOG(\"%s\", this->rx_buffer)") ==
          std::string::npos);
}

void test_mqtt_publish_accepts_only_rev16_publish_status() noexcept
{
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string publish = section(
        mqtt,
        "bool nb_iot::modem_MqttPublish",
        "bool nb_iot::modem_MqttSubscribe");
    CHECK(!publish.empty());
    CHECK(publish.find("+KMQTT_IND: %d,4") != std::string::npos);
    CHECK(publish.find("+KMQTT_IND: %d,3") == std::string::npos);
    CHECK(publish.find("modem_SendCmdWaitOK(pub_cmd, 5000)") !=
          std::string::npos);
}

void test_modem_flow_control_matches_two_wire_pcb() noexcept
{
    const std::string modem = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.cpp");
    const std::string init = section(
        modem,
        "bool nb_iot::modem_init",
        "bool nb_iot::check_at_alive");
    CHECK(!init.empty());
    CHECK(modem.find("uart_set_hw_flow(UART_ID, false, false)") !=
          std::string::npos);
    const std::size_t echo_off = init.find("\"ATE0\"");
    const std::size_t flow_none = init.find("\"AT&K0\"");
    const std::size_t interface_flow_none =
        init.find("\"AT+IFC=0,0\"");
    const std::size_t txon = init.find("modem_configure_txon_indicator");
    CHECK(echo_off != std::string::npos);
    CHECK(flow_none != std::string::npos);
    CHECK(interface_flow_none != std::string::npos);
    CHECK(txon != std::string::npos);
    CHECK(echo_off < flow_none);
    CHECK(flow_none < interface_flow_none);
    CHECK(interface_flow_none < txon);
    CHECK(init.find("MODEM_FLOW_CFG_OK") != std::string::npos);
    CHECK(init.find("MODEM_FLOW_CFG_FAIL") != std::string::npos);
    CHECK(init.find("\"AT&K1\"") == std::string::npos);
    CHECK(init.find("\"AT&K3\"") == std::string::npos);
}

void test_mqtt_identity_override_drives_authentication_and_topics() noexcept
{
    const std::string config = read_file(
        NB_IOT_SOURCE_ROOT "/src/config.h");
    const std::string backend = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string open_transport = section(
        backend,
        "RuntimeOwnerDeviceBackend::open_transport",
        "RuntimeOwnerDeviceBackend::publish_boot_report");

    CHECK(!config.empty());
    CHECK(!backend.empty());
    CHECK(!open_transport.empty());
    CHECK(config.find("#ifndef MQTT_DEVICE_ID") != std::string::npos);
    CHECK(config.find("#ifndef MQTT_USERNAME") != std::string::npos);
    CHECK(config.find("#ifndef MQTT_PASSWORD") != std::string::npos);
    CHECK(backend.find("const char *mqtt_device_id() noexcept") !=
          std::string::npos);
    CHECK(backend.find("const char *mqtt_username() noexcept") !=
          std::string::npos);
    CHECK(backend.find("const char *mqtt_password() noexcept") !=
          std::string::npos);
    CHECK(open_transport.find(
              "mqtt_device_id(),\n"
              "            mqtt_username(),\n"
              "            mqtt_password()") != std::string::npos);
    CHECK(count(backend, "mqtt_device_id()") >= 9);
    CHECK(count(backend, "modem.get_imei()") == 1);
    CHECK(count(backend, "modem.get_cimi()") == 1);
}

void test_config_subscription_qos0_is_an_explicit_firmware_trial() noexcept
{
    const std::string root = read_file(NB_IOT_SOURCE_ROOT "/CMakeLists.txt");
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string subscribe = section(
        mqtt,
        "bool nb_iot::modem_MqttSubscribe",
        "bool nb_iot::modem_MqttPoll");

    CHECK(!root.empty());
    CHECK(!subscribe.empty());
    CHECK(root.find("NB_IOT_CONFIG_QOS0_TRIAL=1") != std::string::npos);
    CHECK(subscribe.find("#if defined(NB_IOT_CONFIG_QOS0_TRIAL)") !=
          std::string::npos);
    CHECK(subscribe.find("kConfigSubscriptionQos = 0") !=
          std::string::npos);
    CHECK(subscribe.find("kConfigSubscriptionQos = 1") !=
          std::string::npos);
    CHECK(subscribe.find(
              "\"AT+KMQTTSUB=%d,\\\"%s\\\",%d\"") !=
          std::string::npos);
    CHECK(subscribe.find(
              "this->mqtt_session_id, topic, kConfigSubscriptionQos") !=
          std::string::npos);
}

void test_boot_report_precedes_subscription_and_liveness_uses_probe_topic()
    noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.hpp");
    const std::string open_transport = section(
        source,
        "RuntimeOwnerDeviceBackend::open_transport",
        "RuntimeOwnerDeviceBackend::publish_boot_report");
    const std::string boot_report = section(
        source,
        "RuntimeOwnerDeviceBackend::publish_boot_report",
        "RuntimeOwnerDeviceBackend::publish_probe");
    const std::string liveness_probe = section(
        source,
        "RuntimeOwnerDeviceBackend::publish_probe",
        "RuntimeOwnerDeviceBackend::verify_subscription");
    CHECK(!open_transport.empty());
    CHECK(!boot_report.empty());
    CHECK(!liveness_probe.empty());
    const std::size_t mqtt_open = open_transport.find("modem.modem_MqttOpen");
    const std::size_t report = open_transport.find("publish_boot_report()");
    const std::size_t subscribe =
        open_transport.find("modem.modem_MqttSubscribe");
    CHECK(mqtt_open != std::string::npos);
    CHECK(report != std::string::npos);
    CHECK(subscribe != std::string::npos);
    CHECK(mqtt_open < report);
    CHECK(report < subscribe);
    CHECK(header.find("publish_boot_report() noexcept") != std::string::npos);
    CHECK(boot_report.find("devices/%s/boot") != std::string::npos);
    CHECK(liveness_probe.find("devices/%s/telemetry/probe") !=
          std::string::npos);
    CHECK(liveness_probe.find("devices/%s/boot") == std::string::npos);
    CHECK(liveness_probe.find("modem.modem_MqttPublish(topic, \"{}\")") !=
          std::string::npos);
}

void test_config_waits_for_complete_kmqtt_data_and_logs_true_lengths() noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string pull_config = section(
        source,
        "RuntimeOwnerDeviceBackend::pull_config",
        "RuntimeOwnerDeviceBackend::pull_command");
    const std::string pull_command = section(
        source,
        "RuntimeOwnerDeviceBackend::pull_command",
        "RuntimeOwnerDeviceBackend::freeze_snapshot");
    CHECK(!pull_config.empty());
    CHECK(!pull_command.empty());
    CHECK(source.find("bool extract_config_payload") == std::string::npos);
    CHECK(pull_config.find("mqtt_kmqtt_data_extract_payload") !=
          std::string::npos);
    CHECK(pull_config.find("std::size_t payload_bytes = 0") !=
          std::string::npos);
    CHECK(pull_config.find("std::size_t frame_bytes = 0") !=
          std::string::npos);
    CHECK(pull_config.find("BUFFER_BYTES=%u") != std::string::npos);
    CHECK(pull_config.find("FRAME_BYTES=%u") != std::string::npos);
    CHECK(pull_config.find("PAYLOAD_BYTES=%u") != std::string::npos);
}

void test_post_config_probe_reuses_connected_session_without_control_query()
    noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string publish_probe = section(
        source,
        "RuntimeOwnerDeviceBackend::publish_probe",
        "RuntimeOwnerDeviceBackend::verify_subscription");
    CHECK(!publish_probe.empty());
    CHECK(publish_probe.find("if (!modem.is_connected())") !=
          std::string::npos);
    CHECK(publish_probe.find("modem.modem_MqttPublish") !=
          std::string::npos);
    CHECK(publish_probe.find("wait_for_mqtt_post_data_quiet") ==
          std::string::npos);
    CHECK(publish_probe.find("AT+KMQTTCFG?") == std::string::npos);
    CHECK(publish_probe.find("modem_SendCmdWaitResponse") ==
          std::string::npos);
    CHECK(publish_probe.find("MQTT_CFG_QUERY") == std::string::npos);
    CHECK(publish_probe.find("MQTT_CTRL_STALL") == std::string::npos);
}

void test_silent_liveness_publish_keeps_trace_for_at_and_close_diagnostics()
    noexcept
{
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string backend = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string periodic = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_periodic_modem.cpp");
    const std::string publish = section(
        mqtt,
        "bool nb_iot::modem_MqttPublish(",
        "bool nb_iot::modem_MqttSubscribe(");
    const std::string disconnect = section(
        mqtt,
        "void nb_iot::modem_MqttDisconnect(",
        "bool nb_iot::modem_MqttOpen(");
    const std::string publish_probe = section(
        backend,
        "RuntimeOwnerDeviceBackend::publish_probe",
        "RuntimeOwnerDeviceBackend::verify_subscription");

    CHECK(!publish.empty());
    CHECK(!disconnect.empty());
    CHECK(!publish_probe.empty());
    CHECK(!periodic.empty());
    CHECK(publish.find(
              "const bool silent_command_timeout = this->buffer_idx == 0") !=
          std::string::npos);
    CHECK(publish.find(
              "const bool liveness_probe =\n"
              "            strstr(topic, \"/telemetry/probe\") != nullptr") !=
          std::string::npos);
    CHECK(publish.find(
              "if (this->at_trace_enabled && silent_command_timeout &&\n"
              "            liveness_probe)") !=
          std::string::npos);
    const std::size_t diag_at_start =
        publish.find("MQTT_PUB_STALL_DIAG_AT_START");
    const std::size_t diag_at_command =
        publish.find("modem_SendCmdWaitOK(\"AT\", 5000)");
    CHECK(diag_at_start != std::string::npos);
    CHECK(diag_at_command != std::string::npos);
    CHECK(diag_at_start < diag_at_command);
    CHECK(publish.find("MQTT_PUB_STALL_DIAG_AT_OK") != std::string::npos);
    CHECK(publish.find("MQTT_PUB_STALL_DIAG_AT_ERROR") !=
          std::string::npos);
    CHECK(publish.find("MQTT_PUB_STALL_DIAG_AT_TIMEOUT") !=
          std::string::npos);
    CHECK(publish.find("AT+KMQTTCNX?") == std::string::npos);

    const std::size_t failed_branch =
        publish_probe.find("if (!published)");
    const std::size_t trace_continue =
        publish_probe.find("LIVENESS_STALL_DIAG_TRACE_CONTINUE");
    const std::size_t failed_return =
        publish_probe.find("return failed(kDiagnosticPublish)");
    const std::size_t periodic_ready = periodic.find("PERIODIC_READY");
    CHECK(failed_branch != std::string::npos);
    CHECK(trace_continue != std::string::npos);
    CHECK(failed_return != std::string::npos);
    CHECK(periodic_ready != std::string::npos);
    CHECK(failed_branch < trace_continue);
    CHECK(trace_continue < failed_return);
    CHECK(count(publish_probe, "modem.set_at_trace_enabled(false)") == 0);
    CHECK(count(periodic, "modem.set_at_trace_enabled(false)") == 0);
    CHECK(periodic.find("#include \"tasks_modem.hpp\"") ==
          std::string::npos);
    CHECK(periodic.find("extern nb_iot modem") == std::string::npos);
    CHECK(disconnect.find("AT+KMQTTCLOSE=%d") != std::string::npos);
    CHECK(disconnect.find("modem_SendCmdWaitResponse") !=
          std::string::npos);
}

void test_post_config_at_probe_uses_the_common_configurable_settle() noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string modem_header = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.hpp");
    const std::string execute = section(
        source,
        "RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::execute(",
        "} // namespace boot_v2");
    const std::string probe_at = section(
        execute,
        "case RuntimeOwnerDeviceOperationKind::ProbeAt:",
        "case RuntimeOwnerDeviceOperationKind::PublishProbe:");
    CHECK(!probe_at.empty());
    CHECK(probe_at.find("modem.check_at_alive()") != std::string::npos);
    CHECK(probe_at.find("modem_sleep(1000);") == std::string::npos);
    CHECK(count(modem_header, "kAtCommandSettleMs = 1000") == 1);
}

void test_boot_through_periodic_ready_at_trace_is_timestamped_and_secret_safe()
    noexcept
{
    const std::string modem = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.cpp");
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.hpp");
    const std::string backend = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string periodic = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_periodic_modem.cpp");
    const std::string send = section(
        modem,
        "void nb_iot::modem_SendCmd(",
        "void nb_iot::modem_PacedWrite(");
    const std::string read = section(
        modem,
        "void nb_iot::modem_ReadResponse(",
        "bool nb_iot::modem_SendCmdWaitResponse(");
    const std::string wait = section(
        modem,
        "bool nb_iot::modem_SendCmdWaitResponse(",
        "bool nb_iot::modem_SendCmdWaitOK(");
    const std::string trace_tx = section(
        modem,
        "void nb_iot::modem_TraceTxCommand(",
        "void nb_iot::modem_TraceRxBytes(");
    const std::string trace_rx = section(
        modem,
        "void nb_iot::modem_TraceRxBytes(",
        "void nb_iot::modem_TraceTimeout(");
    const std::string init = section(
        modem,
        "bool nb_iot::modem_init(",
        "bool nb_iot::check_at_alive(");
    const std::string probe = section(
        backend,
        "RuntimeOwnerDeviceBackend::publish_probe",
        "RuntimeOwnerDeviceBackend::verify_subscription");
    CHECK(!send.empty());
    CHECK(!read.empty());
    CHECK(!wait.empty());
    CHECK(!trace_tx.empty());
    CHECK(!trace_rx.empty());
    CHECK(!init.empty());
    CHECK(!probe.empty());
    CHECK(!periodic.empty());
    CHECK(header.find("bool at_trace_enabled") != std::string::npos);
    CHECK(header.find("set_at_trace_enabled(bool enabled)") !=
          std::string::npos);
    CHECK(modem.find("at_trace_enabled = true;") != std::string::npos);
    const std::size_t trace_before_send =
        send.find("modem_TraceTxCommand(cmd)");
    const std::size_t uart_send = send.find("uart_puts(UART_ID, cmd)");
    CHECK(trace_before_send != std::string::npos);
    CHECK(uart_send != std::string::npos);
    CHECK(trace_before_send < uart_send);
    CHECK(read.find("modem_TraceRxBytes(trace_bytes, trace_length)") !=
          std::string::npos);
    CHECK(wait.find("modem_TraceTimeout(timeout_ms)") !=
          std::string::npos);
    CHECK(trace_tx.find("to_ms_since_boot(get_absolute_time())") !=
          std::string::npos);
    CHECK(trace_rx.find("to_ms_since_boot(get_absolute_time())") !=
          std::string::npos);
    CHECK(trace_tx.find("AT+KMQTTCFG=") != std::string::npos);
    CHECK(trace_tx.find("<REDACTED") != std::string::npos);
    const std::size_t cert_marker =
        init.find("modem_TraceRedactedData(\"CERTIFICATE\"");
    const std::size_t log_disable = init.find("app_log_set_enabled(false)");
    const std::size_t cert_write = init.find("uart_putc(UART_ID");
    const std::size_t log_enable = init.find("app_log_set_enabled(true)");
    const std::size_t response_wait = init.find("while (elapsed < 5000)");
    CHECK(cert_marker != std::string::npos);
    CHECK(log_disable != std::string::npos);
    CHECK(cert_write != std::string::npos);
    CHECK(log_enable != std::string::npos);
    CHECK(response_wait != std::string::npos);
    CHECK(cert_marker < log_disable);
    CHECK(log_disable < cert_write);
    CHECK(cert_write < log_enable);
    CHECK(log_enable < response_wait);
    CHECK(probe.find("modem.modem_MqttPublish") != std::string::npos);
    CHECK(probe.find("modem.set_at_trace_enabled(false)") ==
          std::string::npos);
    CHECK(probe.find("AT_TRACE_STOP") == std::string::npos);
    const std::size_t periodic_ready = periodic.find("PERIODIC_READY");
    CHECK(periodic_ready != std::string::npos);
    CHECK(periodic.find("modem.set_at_trace_enabled(false)") ==
          std::string::npos);
    CHECK(periodic.find("AT_TRACE_STOP") == std::string::npos);
    CHECK(periodic.find("#include \"tasks_modem.hpp\"") ==
          std::string::npos);
}

void test_all_at_commands_use_configurable_one_second_settle_except_reset_all()
    noexcept
{
    const std::string modem = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.cpp");
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.hpp");
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string gate = section(
        modem,
        "void nb_iot::modem_WaitForAtCommandSlot(",
        "void nb_iot::modem_SendCmd(");
    const std::string send = section(
        modem,
        "void nb_iot::modem_SendCmd(",
        "void nb_iot::modem_PacedWrite(");
    const std::string clear = section(
        modem,
        "void nb_iot::modem_ClearRxBuffer(",
        "void nb_iot::modem_WaitForAtCommandSlot(");
    const std::string read = section(
        modem,
        "void nb_iot::modem_ReadResponse(",
        "bool nb_iot::modem_SendCmdWaitResponse(");
    const std::string wait = section(
        modem,
        "bool nb_iot::modem_SendCmdWaitResponse(",
        "bool nb_iot::modem_SendCmdWaitOK(");
    const std::string reset = section(
        mqtt,
        "void nb_iot::modem_MqttResetAllSessions(",
        "void nb_iot::modem_MqttCloseDeleteSession(");
    const std::string target_cleanup = section(
        mqtt,
        "void nb_iot::modem_MqttCloseDeleteSession(",
        "void nb_iot::modem_MqttBootCleanStart(");
    const std::string reconnect = section(
        mqtt,
        "bool nb_iot::modem_MqttReconnect(",
        "bool nb_iot::modem_MqttPublish(");
    const std::string publish = section(
        mqtt,
        "bool nb_iot::modem_MqttPublish(",
        "bool nb_iot::modem_MqttSubscribe(");
    const std::string subscribe = section(
        mqtt,
        "bool nb_iot::modem_MqttSubscribe(",
        "bool nb_iot::modem_MqttPoll(");

    CHECK(!modem.empty());
    CHECK(!header.empty());
    CHECK(!mqtt.empty());
    CHECK(!gate.empty());
    CHECK(!send.empty());
    CHECK(!clear.empty());
    CHECK(!read.empty());
    CHECK(!wait.empty());
    CHECK(!reset.empty());
    CHECK(!target_cleanup.empty());
    CHECK(!reconnect.empty());
    CHECK(!publish.empty());
    CHECK(!subscribe.empty());
    CHECK(count(header, "kAtCommandSettleMs = 1000") == 1);
    CHECK(header.find("Tune to 500") != std::string::npos);
    CHECK(header.find("last_at_activity_ms") != std::string::npos);
    CHECK(header.find("at_command_started") != std::string::npos);
    CHECK(header.find("at_command_settle_bypass") != std::string::npos);
    CHECK(header.find("void modem_WaitForAtCommandSlot()") !=
          std::string::npos);
    CHECK(header.find("void modem_MarkAtCommandTimeout()") !=
          std::string::npos);
    CHECK(gate.find(
              "if (!at_command_started || at_command_settle_bypass)") !=
          std::string::npos);
    CHECK(gate.find("modem_ClearRxBuffer()") != std::string::npos);
    CHECK(gate.find("modem_ReadResponse(0)") != std::string::npos);
    CHECK(gate.find("quiet_ms >= kAtCommandSettleMs") !=
          std::string::npos);
    CHECK(gate.find("!uart_is_readable(UART_ID)") != std::string::npos);
    CHECK(gate.find("modem_sleep") != std::string::npos);
    CHECK(gate.find("AT SETTLE") != std::string::npos);
    const std::size_t gate_call =
        send.find("modem_WaitForAtCommandSlot()");
    const std::size_t trace_call = send.find("modem_TraceTxCommand(cmd)");
    const std::size_t uart_send = send.find("uart_puts(UART_ID, cmd)");
    const std::size_t activity_update =
        send.find("last_at_activity_ms =");
    const std::size_t command_started =
        send.find("at_command_started = true");
    CHECK(gate_call != std::string::npos);
    CHECK(trace_call != std::string::npos);
    CHECK(uart_send != std::string::npos);
    CHECK(activity_update != std::string::npos);
    CHECK(command_started != std::string::npos);
    CHECK(gate_call < trace_call);
    CHECK(trace_call < uart_send);
    CHECK(uart_send < activity_update);
    CHECK(activity_update < command_started);
    CHECK(count(modem, "uart_puts(UART_ID") == 2);
    CHECK(clear.find("received_any") != std::string::npos);
    CHECK(clear.find("last_at_activity_ms =") != std::string::npos);
    CHECK(read.find("trace_length != 0") != std::string::npos);
    CHECK(read.find("last_at_activity_ms =") != std::string::npos);
    const std::size_t timeout_boundary =
        wait.find("modem_MarkAtCommandTimeout()");
    const std::size_t timeout_trace =
        wait.find("modem_TraceTimeout(timeout_ms)");
    CHECK(timeout_boundary != std::string::npos);
    CHECK(timeout_trace != std::string::npos);
    CHECK(timeout_boundary < timeout_trace);
    const std::size_t save_bypass =
        reset.find("previous_settle_bypass");
    const std::size_t enable_bypass =
        reset.find("at_command_settle_bypass = true");
    const std::size_t reset_loop = reset.find("for (int session_id = 1;");
    const std::size_t restore_bypass = reset.find(
        "at_command_settle_bypass = previous_settle_bypass");
    CHECK(save_bypass != std::string::npos);
    CHECK(enable_bypass != std::string::npos);
    CHECK(reset_loop != std::string::npos);
    CHECK(restore_bypass != std::string::npos);
    CHECK(save_bypass < enable_bypass);
    CHECK(enable_bypass < reset_loop);
    CHECK(reset_loop < restore_bypass);
    CHECK(count(reset, "modem_sleep(100);") == 2);
    CHECK(target_cleanup.find("at_command_settle_bypass") ==
          std::string::npos);
    CHECK(count(reconnect, "modem_MarkAtCommandTimeout()") == 1);
    CHECK(count(publish, "modem_MarkAtCommandTimeout()") == 1);
    CHECK(count(subscribe, "modem_MarkAtCommandTimeout()") == 1);
}

void test_complete_kmqtt_data_handoffs_without_quiet_or_session_teardown()
    noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.hpp");
    const std::string pull_config = section(
        source,
        "RuntimeOwnerDeviceBackend::pull_config",
        "RuntimeOwnerDeviceBackend::pull_command");
    const std::string pull_command = section(
        source,
        "RuntimeOwnerDeviceBackend::pull_command",
        "RuntimeOwnerDeviceBackend::freeze_snapshot");
    CHECK(!pull_config.empty());
    CHECK(!pull_command.empty());
    CHECK(source.find("kMqttPostDataQuietMs") == std::string::npos);
    CHECK(source.find("kMqttPostDataPollSliceMs") == std::string::npos);
    CHECK(source.find("wait_for_mqtt_post_data_quiet") == std::string::npos);
    CHECK(header.find("wait_for_mqtt_post_data_quiet") == std::string::npos);
    CHECK(header.find("mqtt_post_data_received_at_ms_") ==
          std::string::npos);
    CHECK(header.find("mqtt_post_data_quiet_pending_") ==
          std::string::npos);
    const std::size_t extract =
        pull_config.find("mqtt_kmqtt_data_extract_payload");
    CHECK(extract != std::string::npos);
    CHECK(pull_config_apply_contract_accepts(pull_config));
    CHECK(dedicated_command_contract_accepts(pull_command));

    const std::string apply_guard_without_negation = replace_once_copy(
        pull_config,
        "if (!apply_mqtt_config_payload(payload))",
        "if (apply_mqtt_config_payload(payload))");
    CHECK(apply_guard_without_negation != pull_config);
    CHECK(!pull_config_apply_contract_accepts(apply_guard_without_negation));

    const std::string void_apply_call_restored = replace_once_copy(
        pull_config,
        "            if (!apply_mqtt_config_payload(payload)) {\n"
        "                LOG(\"CONFIG_APPLY_FAILED\\n\");\n"
        "                return failed(kDiagnosticInvalidCommand);\n"
        "            }\n",
        "            (void)apply_mqtt_config_payload(payload);\n");
    CHECK(void_apply_call_restored != pull_config);
    CHECK(!pull_config_apply_contract_accepts(void_apply_call_restored));

    const std::string command_side_effect_inserted = replace_once_copy(
        pull_config,
        "            if (!apply_mqtt_config_payload(payload)) {",
        "            extract_json_int(payload, \"cmd\");\n"
        "            if (!apply_mqtt_config_payload(payload)) {");
    CHECK(command_side_effect_inserted != pull_config);
    CHECK(!pull_config_apply_contract_accepts(command_side_effect_inserted));

    const std::size_t apply =
        pull_config.find("apply_mqtt_config_payload(payload)");
    CHECK(apply != std::string::npos);
    CHECK(extract < apply);
    CHECK(pull_config.find("AT+KMQTTCFG?") == std::string::npos);
    CHECK(pull_config.find("modem_MqttClose") == std::string::npos);
    CHECK(pull_config.find("modem_MqttDisconnect") == std::string::npos);
}

void test_compact_config_limits_are_applied_atomically() noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/app_context.cpp");
    const std::string apply = section(
        source,
        "bool apply_mqtt_config_payload",
        "// ====================================================================================");
    CHECK(!apply.empty());
    CHECK(compact_config_apply_contract_accepts(apply));

    const std::string extractor_guard_removed = replace_once_copy(
        apply,
        "if (!mqtt_config_payload_extract_object",
        "if (mqtt_config_payload_extract_object");
    CHECK(extractor_guard_removed != apply);
    CHECK(!compact_config_apply_contract_accepts(extractor_guard_removed));

    const std::string parser_guard_removed = replace_once_copy(
        apply,
        "if (!mqtt_config_compact_limits_parse",
        "if (mqtt_config_compact_limits_parse");
    CHECK(parser_guard_removed != apply);
    CHECK(!compact_config_apply_contract_accepts(parser_guard_removed));

    const std::string forbidden_call_inserted = replace_once_copy(
        apply,
        "    float temp1_upper = 0.0f;",
        "    extract_json_int(config, \"userSensorId\");\n"
        "    float temp1_upper = 0.0f;");
    CHECK(forbidden_call_inserted != apply);
    CHECK(!compact_config_apply_contract_accepts(forbidden_call_inserted));

    const std::string early_other_rhs_write = replace_once_copy(
        apply,
        "    if (!mqtt_config_compact_limits_parse",
        "    g_temp_upper_limit_ch0 = 123.0f;\n"
        "    if (!mqtt_config_compact_limits_parse");
    CHECK(early_other_rhs_write != apply);
    CHECK(!compact_config_apply_contract_accepts(early_other_rhs_write));

    const std::string direct_side_effect_inserted = replace_once_copy(
        apply,
        "    float temp1_upper = 0.0f;",
        "    watchdog_reboot(0, 0, 1);\n"
        "    float temp1_upper = 0.0f;");
    CHECK(direct_side_effect_inserted != apply);
    CHECK(!compact_config_apply_contract_accepts(direct_side_effect_inserted));

    std::string assignment_before_parser = replace_once_copy(
        apply,
        "    g_sensors[0].temp_upper_limit = temp1_upper;\n",
        "");
    assignment_before_parser = replace_once_copy(
        assignment_before_parser,
        "    if (!mqtt_config_compact_limits_parse",
        "    g_sensors[0].temp_upper_limit = temp1_upper;\n"
        "    if (!mqtt_config_compact_limits_parse");
    CHECK(assignment_before_parser != apply);
    CHECK(!compact_config_apply_contract_accepts(assignment_before_parser));
}

void test_followup_mqtt_operations_do_not_use_post_data_control_barrier()
    noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string publish_probe = section(
        source,
        "RuntimeOwnerDeviceBackend::publish_probe",
        "RuntimeOwnerDeviceBackend::verify_subscription");
    const std::string verify_subscription = section(
        source,
        "RuntimeOwnerDeviceBackend::verify_subscription",
        "RuntimeOwnerDeviceBackend::pull_config");
    const std::string pull_config = section(
        source,
        "RuntimeOwnerDeviceBackend::pull_config",
        "RuntimeOwnerDeviceBackend::pull_command");
    const std::string pull_command = section(
        source,
        "RuntimeOwnerDeviceBackend::pull_command",
        "RuntimeOwnerDeviceBackend::freeze_snapshot");
    const std::string publish_telemetry = section(
        source,
        "RuntimeOwnerDeviceBackend::publish_telemetry",
        "RuntimeOwnerDeviceBackend::execute");
    CHECK(!publish_probe.empty());
    CHECK(!verify_subscription.empty());
    CHECK(!pull_config.empty());
    CHECK(!pull_command.empty());
    CHECK(!publish_telemetry.empty());
    CHECK(publish_probe.find("wait_for_mqtt_post_data_quiet") ==
          std::string::npos);
    CHECK(verify_subscription.find("wait_for_mqtt_post_data_quiet") ==
          std::string::npos);
    CHECK(pull_config.find("wait_for_mqtt_post_data_quiet") ==
          std::string::npos);
    CHECK(pull_command.find("wait_for_mqtt_post_data_quiet") ==
          std::string::npos);
    CHECK(publish_telemetry.find("wait_for_mqtt_post_data_quiet") ==
          std::string::npos);
}

void test_refresh_rssi_does_not_publish_orphan_alert() noexcept
{
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.hpp");
    const std::string refresh_rssi = section(
        source,
        "case RuntimeOwnerDeviceOperationKind::RefreshRssi:",
        "case RuntimeOwnerDeviceOperationKind::PullCommand:");
    CHECK(!source.empty());
    CHECK(!header.empty());
    CHECK(!refresh_rssi.empty());
    CHECK(source.find("publish_alert_if_needed") == std::string::npos);
    CHECK(header.find("publish_alert_if_needed") == std::string::npos);
    CHECK(source.find("devices/%s/alert") == std::string::npos);
    CHECK(source.find("{\\\"alert\\\":1}") == std::string::npos);
    CHECK(refresh_rssi.find("modem_MqttPublish") == std::string::npos);
    CHECK(refresh_rssi.find("modem.check_rssi_csq()") !=
          std::string::npos);
    CHECK(refresh_rssi.find("csq > 0 && csq != 99") !=
          std::string::npos);
}

void test_temperature_channel_offsets_match_the_approved_calibration() noexcept
{
    const std::string config = read_file(
        NB_IOT_SOURCE_ROOT "/src/config.h");
    const std::string sensor = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_sensor.cpp");
    CHECK(!config.empty());
    CHECK(!sensor.empty());
    CHECK(config.find("#define TEMP1_CAL_OFFSET_C 5.0f") !=
          std::string::npos);
    CHECK(config.find("#define TEMP2_CAL_OFFSET_C 0.0f") !=
          std::string::npos);
    CHECK(sensor.find("if (status_ch0 == 0)") != std::string::npos);
    CHECK(sensor.find("temp_ch0 += TEMP1_CAL_OFFSET_C;") !=
          std::string::npos);
    CHECK(sensor.find("if (status_ch1 == 0)") != std::string::npos);
    CHECK(sensor.find("temp_ch1 += TEMP2_CAL_OFFSET_C;") !=
          std::string::npos);
}

void test_mqtt_session_cleanup_is_boot_only_and_recovery_is_targeted()
    noexcept
{
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.hpp");
    const std::string backend = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string boot_clean = section(
        mqtt,
        "void nb_iot::modem_MqttBootCleanStart",
        "void nb_iot::modem_MqttDisconnect");
    const std::string disconnect = section(
        mqtt,
        "void nb_iot::modem_MqttDisconnect",
        "bool nb_iot::modem_MqttOpen");
    const std::string open = section(
        mqtt,
        "bool nb_iot::modem_MqttOpen",
        "bool nb_iot::modem_MqttReconnect");
    const std::string reconnect = section(
        mqtt,
        "bool nb_iot::modem_MqttReconnect",
        "bool nb_iot::modem_MqttPublish");
    const std::string publish = section(
        mqtt,
        "bool nb_iot::modem_MqttPublish",
        "bool nb_iot::modem_MqttSubscribe");
    const std::string subscribe = section(
        mqtt,
        "bool nb_iot::modem_MqttSubscribe",
        "bool nb_iot::modem_MqttPoll");
    const std::string open_transport = section(
        backend,
        "RuntimeOwnerPhysicalResult RuntimeOwnerDeviceBackend::open_transport(",
        "RuntimeOwnerDeviceBackend::publish_boot_report");
    const std::string recovery = section(
        backend,
        "case RuntimeOwnerDeviceOperationKind::EnterRecovery:",
        "case RuntimeOwnerDeviceOperationKind::PublishTelemetry:");
    CHECK(header.find("bool mqtt_boot_cleanup_done") != std::string::npos);
    CHECK(header.find("void modem_MqttBootCleanStart()") !=
          std::string::npos);
    CHECK(header.find("void modem_MqttDisconnect()") != std::string::npos);
    CHECK(header.find("bool modem_MqttReconnect()") != std::string::npos);
    CHECK(header.find("void modem_MqttResetAllSessions()") !=
          std::string::npos);
    CHECK(header.find("void modem_MqttCloseDeleteSession(int session_id)") !=
          std::string::npos);
    CHECK(!boot_clean.empty());
    CHECK(!disconnect.empty());
    CHECK(!open.empty());
    CHECK(!reconnect.empty());
    CHECK(!publish.empty());
    CHECK(!subscribe.empty());
    CHECK(!open_transport.empty());
    CHECK(!recovery.empty());
    CHECK(boot_clean.find("modem_MqttResetAllSessions()") !=
          std::string::npos);
    CHECK(boot_clean.find("mqtt_boot_cleanup_done = true") !=
          std::string::npos);
    CHECK(open.find("if (!this->mqtt_boot_cleanup_done)") !=
          std::string::npos);
    CHECK(count(open, "modem_MqttBootCleanStart()") == 1);
    CHECK(open.find("if (this->mqtt_session_id > 0)") !=
          std::string::npos);
    const std::size_t reconnect_first =
        open.find("this->modem_MqttReconnect()");
    const std::size_t target_cleanup =
        open.find("this->modem_MqttCloseDeleteSession(stale_session_id)");
    const std::size_t new_config = open.find("AT+KMQTTCFG=1,1");
    CHECK(reconnect_first != std::string::npos);
    CHECK(target_cleanup != std::string::npos);
    CHECK(new_config != std::string::npos);
    CHECK(reconnect_first < target_cleanup);
    CHECK(target_cleanup < new_config);
    CHECK(open.find("MQTT_CFG_LAST_RESORT_RESET") != std::string::npos);
    CHECK(count(open, "modem_MqttResetAllSessions()") == 1);
    CHECK(disconnect.find("AT+KMQTTCLOSE=%d") != std::string::npos);
    CHECK(disconnect.find("AT+KMQTTDEL") == std::string::npos);
    CHECK(disconnect.find("MQTT_DISCONNECT_KEEP_SESSION") !=
          std::string::npos);
    CHECK(reconnect.find("AT+KMQTTCNX=%d") != std::string::npos);
    CHECK(reconnect.find("MQTT_RECONNECT %d") != std::string::npos);
    CHECK(reconnect.find("MQTT_CONNECT_ABORT") != std::string::npos);
    CHECK(reconnect.find("+CME ERROR: 907") != std::string::npos);
    CHECK(reconnect.find("this->is_unauthenticated = true") !=
          std::string::npos);
    const std::size_t abort_log = reconnect.find("MQTT_CONNECT_ABORT");
    const std::size_t auth_evidence = reconnect.find("+CME ERROR: 907");
    CHECK(abort_log < auth_evidence);
    CHECK(count(publish, "this->is_mqtt_connected = false") == 2);
    CHECK(count(subscribe, "this->is_mqtt_connected = false") == 2);
    CHECK(count(open_transport, "modem.modem_MqttDisconnect()") == 3);
    CHECK(open_transport.find("modem.modem_MqttClose()") ==
          std::string::npos);
    CHECK(recovery.find("modem.get_mqtt_session_id() > 0") !=
          std::string::npos);
    CHECK(recovery.find("modem.modem_MqttDisconnect()") !=
          std::string::npos);
    CHECK(recovery.find("modem.modem_MqttClose()") == std::string::npos);
    CHECK(mqtt.find("auto reset_mqtt_sessions") == std::string::npos);
}

void test_manual_at_console_remains_available_but_product_build_is_automatic()
    noexcept
{
    const std::string mqtt = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_mqtt.cpp");
    const std::string modem_header = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_modem.hpp");
    const std::string debug = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_debug.cpp");
    const std::string debug_header = read_file(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_debug.hpp");
    const std::string root = read_file(NB_IOT_SOURCE_ROOT "/CMakeLists.txt");
    const std::string open = section(
        mqtt,
        "bool nb_iot::modem_MqttOpen(",
        "bool nb_iot::modem_MqttReconnect(");
    const std::string manual_console = section(
        mqtt,
        "[[noreturn]] void nb_iot::modem_ManualAtConsole(",
        "bool nb_iot::modem_MqttOpen(");

    CHECK(!open.empty());
    CHECK(!manual_console.empty());
    CHECK(!modem_header.empty());
    CHECK(!debug.empty());
    CHECK(!debug_header.empty());
    CHECK(!root.empty());
    CHECK(root.find("set(PICO_BOARD pico2)") != std::string::npos);
    CHECK(root.find("set(PICO_BOARD pico2_w)") == std::string::npos);
    CHECK(root.find("NB_IOT_POST_CONFIG_HANDOFF_TRIAL=1") !=
          std::string::npos);
    CHECK(root.find("NB_IOT_MANUAL_AT_SESSION_RESET_TRIAL=1") ==
          std::string::npos);
    CHECK(root.find("add_executable(modem_at_console") !=
          std::string::npos);

    const std::size_t cleanup =
        open.find("this->modem_MqttBootCleanStart()");
    const std::size_t enable = open.find("manual_at_mode_enable()");
    const std::size_t console = open.find("this->modem_ManualAtConsole()");
    const std::size_t automatic_pdp = open.find("AT+KCNXCFG=1");
    const std::size_t guard = open.find(
        "#if defined(NB_IOT_MANUAL_AT_SESSION_RESET_TRIAL)");
    const std::size_t guard_end = open.find("#endif", console);
    CHECK(cleanup != std::string::npos);
    CHECK(automatic_pdp != std::string::npos);
    CHECK(enable != std::string::npos);
    CHECK(console != std::string::npos);
    CHECK(guard != std::string::npos);
    CHECK(guard_end != std::string::npos);
    CHECK(cleanup < guard);
    CHECK(guard < enable);
    CHECK(enable < console);
    CHECK(console < guard_end);
    CHECK(guard_end < automatic_pdp);

    CHECK(modem_header.find("[[noreturn]] void modem_ManualAtConsole()") !=
          std::string::npos);
    CHECK(debug_header.find("void manual_at_mode_enable() noexcept") !=
          std::string::npos);
    CHECK(debug_header.find("bool manual_at_mode_enabled() noexcept") !=
          std::string::npos);
    CHECK(debug.find("std::atomic<bool>") != std::string::npos);
    const std::size_t debug_gate =
        debug.find("if (manual_at_mode_enabled())");
    const std::size_t debug_input = debug.find("getchar_timeout_us(0)");
    CHECK(debug_gate != std::string::npos);
    CHECK(debug_input != std::string::npos);
    CHECK(debug_gate < debug_input);
    CHECK(debug.find("#include \"tasks_modem.hpp\"") ==
          std::string::npos);
    CHECK(debug.find("modem.modem_") == std::string::npos);

    CHECK(manual_console.find("MANUAL_AT_READY") != std::string::npos);
    CHECK(manual_console.find("char command[512]{}") != std::string::npos);
    CHECK(manual_console.find("command_has_at_prefix(command)") !=
          std::string::npos);
    CHECK(manual_console.find("this->modem_SendCmd(command)") !=
          std::string::npos);
    const std::size_t manual_uart_drain =
        manual_console.find("this->modem_ReadResponse(0)");
    const std::size_t manual_poll_delay =
        manual_console.find("modem_sleep(1)");
    CHECK(manual_uart_drain != std::string::npos);
    CHECK(manual_poll_delay != std::string::npos);
    CHECK(manual_poll_delay != std::string::npos &&
          manual_uart_drain < manual_poll_delay);
    CHECK(manual_console.find("modem_sleep(10)") == std::string::npos);
    CHECK(manual_console.find("command_equals(command, \"reboot\")") !=
          std::string::npos);
    CHECK(manual_console.find("watchdog_reboot") != std::string::npos);
    CHECK(manual_console.find("getchar_timeout_us(0)") !=
          std::string::npos);
    CHECK(manual_console.find("putchar(character)") == std::string::npos);
    CHECK(manual_console.find("strcat(command") == std::string::npos);
    CHECK(manual_console.find("modem_PacedWrite") == std::string::npos);
}

void test_shutdown_cleanup_is_private_bounded_and_ordered() noexcept
{
    const std::string header = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.hpp");
    const std::string source = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_device_backend.cpp");
    const std::string owner_loop = read_file(
        NB_IOT_SOURCE_ROOT "/src/boot_v2/runtime_owner_rtos_owner_loop.hpp");
    const std::string config = read_file(
        NB_IOT_SOURCE_ROOT "/src/config.h");
    const std::string store = read_file(
        NB_IOT_SOURCE_ROOT
        "/src/boot_v2/runtime_owner_shutdown_record_store.cpp");
    const std::string cleanup = section(
        source,
        "RuntimeOwnerDeviceBackend::execute_shutdown_cleanup",
        "} // namespace boot_v2");
    const std::string prepare = section(
        source,
        "bool RuntimeOwnerDeviceBackend::prepare()",
        "bool RuntimeOwnerDeviceBackend::prepared()");

    CHECK(!header.empty());
    CHECK(!source.empty());
    CHECK(!owner_loop.empty());
    CHECK(!config.empty());
    CHECK(!store.empty());
    CHECK(!cleanup.empty());
    CHECK(!prepare.empty());
    CHECK(header.find("friend class RuntimeOwnerRtosOwnerLoop") !=
          std::string::npos);
    CHECK(header.find(
              "RuntimeOwnerShutdownStepResult execute_shutdown_cleanup(") !=
          std::string::npos);
    CHECK(header.find(
              "const RuntimeOwnerShutdownDirective &directive") !=
          std::string::npos);
    CHECK(owner_loop.find(
              "return backend.execute_shutdown_cleanup(") !=
          std::string::npos);
    CHECK(owner_loop.find("directive, context") != std::string::npos);
    CHECK(config.find("#define POWER_INT_DEBOUNCE_MS 500") !=
          std::string::npos);
    CHECK(config.find("#define SHUTDOWN_HARD_DEADLINE_MS 90000") !=
          std::string::npos);
    CHECK(config.find(
              "#define MODEM_POWEROFF_FINALIZE_RESERVE_MS 1000") !=
          std::string::npos);

    const std::size_t wake_init =
        prepare.find("gpio_init(MODEM_WAKEUP_PIN)");
    const std::size_t wake_output =
        prepare.find("gpio_set_dir(MODEM_WAKEUP_PIN, GPIO_OUT)");
    const std::size_t wake_high =
        prepare.find("gpio_put(MODEM_WAKEUP_PIN, 1)");
    const std::size_t prepared = prepare.find("prepared_ = 1");
    CHECK(wake_init != std::string::npos);
    CHECK(wake_output != std::string::npos);
    CHECK(wake_high != std::string::npos);
    CHECK(prepared != std::string::npos);
    CHECK(wake_init < wake_output);
    CHECK(wake_output < wake_high);
    CHECK(wake_high < prepared);

    CHECK(cleanup.find(
              "case RuntimeOwnerShutdownCleanupStep::StopOutputs") !=
          std::string::npos);
    CHECK(cleanup.find(
              "case RuntimeOwnerShutdownCleanupStep::PublishDying") !=
          std::string::npos);
    CHECK(cleanup.find(
              "case RuntimeOwnerShutdownCleanupStep::CloseDeleteSessions") !=
          std::string::npos);
    CHECK(cleanup.find(
              "case RuntimeOwnerShutdownCleanupStep::ScanSessions") !=
          std::string::npos);
    CHECK(cleanup.find(
              "case RuntimeOwnerShutdownCleanupStep::DisconnectPdp") !=
          std::string::npos);
    CHECK(cleanup.find(
              "case RuntimeOwnerShutdownCleanupStep::SetCfun0") !=
          std::string::npos);
    CHECK(cleanup.find(
              "case RuntimeOwnerShutdownCleanupStep::PowerOffModem") !=
          std::string::npos);
    CHECK(cleanup.find("remaining_ms < 17000") != std::string::npos);
    CHECK(cleanup.find("devices/%s/status") != std::string::npos);
    CHECK(cleanup.find("\"[0,%u]\"") != std::string::npos);
    CHECK(cleanup.find("for (int session_id = 1; session_id <= 6;") !=
          std::string::npos);
    CHECK(cleanup.find("\"AT+KMQTTCLOSE=%d\"") !=
          std::string::npos);
    CHECK(cleanup.find("\"AT+KMQTTDEL=%d\"") !=
          std::string::npos);
    CHECK(source.find("+CME ERROR: 910") != std::string::npos);
    CHECK(source.find("+CME ERROR: 916") != std::string::npos);
    CHECK(source.find("+CME ERROR: 922") != std::string::npos);
    CHECK(source.find("+CME ERROR: 924") != std::string::npos);
    CHECK(cleanup.find("\"AT+KMQTTCFG?\"") != std::string::npos);
    CHECK(cleanup.find("\"AT+KCNXDOWN=1\"") != std::string::npos);
    CHECK(cleanup.find("\"AT+CFUN=0\"") != std::string::npos);

    const std::size_t power_off =
        cleanup.find("modem.modem_SendCmd(\"AT+CPWROFF\")");
    const std::size_t wake_low =
        cleanup.find("gpio_put(MODEM_WAKEUP_PIN, 0)", power_off);
    const std::size_t response =
        cleanup.find("modem.modem_ReadResponse(0)", wake_low);
    const std::size_t accepted =
        cleanup.find("power_off_accepted = true", response);
    const std::size_t record_input =
        cleanup.find("RuntimeOwnerShutdownRecordInput", accepted);
    const std::size_t record_commit =
        cleanup.find("runtime_owner_shutdown_record_commit(", record_input);
    const std::size_t settle =
        cleanup.find("MODEM_POWEROFF_SETTLE", record_commit);
    const std::size_t settle_sleep =
        cleanup.find("modem_sleep(settle_ms)", settle);
    CHECK(power_off != std::string::npos);
    CHECK(wake_low != std::string::npos);
    CHECK(response != std::string::npos);
    CHECK(accepted != std::string::npos);
    CHECK(record_input != std::string::npos);
    CHECK(record_commit != std::string::npos);
    CHECK(settle != std::string::npos);
    CHECK(settle_sleep != std::string::npos);
    CHECK(power_off < wake_low);
    CHECK(wake_low < response);
    CHECK(response < accepted);
    CHECK(accepted < record_input);
    CHECK(record_input < record_commit);
    CHECK(record_commit < settle);
    CHECK(settle < settle_sleep);
    CHECK(cleanup.find(
              "if (std::strstr(rx, \"OK\") != nullptr) {\n"
              "                return RuntimeOwnerShutdownStepResult::Succeeded;") ==
          std::string::npos);
    CHECK(cleanup.find("gpio_put(POWER_KILL_PIN") ==
          std::string::npos);
    CHECK(cleanup.find("watchdog_reboot") == std::string::npos);

    CHECK(store.find("flash_safe_execute(") != std::string::npos);
    CHECK(store.find("flash_range_erase(") != std::string::npos);
    CHECK(store.find("flash_range_program(") != std::string::npos);
    CHECK(store.find("flash_partition::shutdown_record_a_offset") !=
          std::string::npos);
    CHECK(store.find("flash_partition::shutdown_record_b_offset") !=
          std::string::npos);
    CHECK(store.find("runtime_owner_shutdown_record_valid(") !=
          std::string::npos);
    CHECK(store.find("std::memcmp(") != std::string::npos);
}

} // namespace

int main()
{
    test_header_contract();
    test_single_firmware_backend_graph();
    test_mqtt_poll_drains_uart_before_fifo_can_overflow();
    test_mqtt_publish_drains_uart_during_command_and_puback_waits();
    test_mqtt_publish_failure_reports_redacted_modem_reason();
    test_mqtt_publish_accepts_only_rev16_publish_status();
    test_modem_flow_control_matches_two_wire_pcb();
    test_mqtt_identity_override_drives_authentication_and_topics();
    test_config_subscription_qos0_is_an_explicit_firmware_trial();
    test_boot_report_precedes_subscription_and_liveness_uses_probe_topic();
    test_config_waits_for_complete_kmqtt_data_and_logs_true_lengths();
    test_post_config_probe_reuses_connected_session_without_control_query();
    test_silent_liveness_publish_keeps_trace_for_at_and_close_diagnostics();
    test_post_config_at_probe_uses_the_common_configurable_settle();
    test_boot_through_periodic_ready_at_trace_is_timestamped_and_secret_safe();
    test_all_at_commands_use_configurable_one_second_settle_except_reset_all();
    test_complete_kmqtt_data_handoffs_without_quiet_or_session_teardown();
    test_compact_config_limits_are_applied_atomically();
    test_followup_mqtt_operations_do_not_use_post_data_control_barrier();
    test_refresh_rssi_does_not_publish_orphan_alert();
    test_temperature_channel_offsets_match_the_approved_calibration();
    test_mqtt_session_cleanup_is_boot_only_and_recovery_is_targeted();
    test_manual_at_console_remains_available_but_product_build_is_automatic();
    test_shutdown_cleanup_is_private_bounded_and_ordered();
    if (g_failures != 0) {
        std::fprintf(stderr,
                     "runtime_owner_device_backend_contract_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf(
        "runtime_owner_device_backend_contract_test: %zu checks passed\n",
        g_checks);
    return 0;
}
