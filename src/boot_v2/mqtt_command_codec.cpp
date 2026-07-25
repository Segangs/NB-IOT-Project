#include "mqtt_command_codec.hpp"

#include <cstdio>
#include <cstring>
#include <limits>

namespace boot_v2 {
namespace {

constexpr std::uint32_t kMaximumCommandTtlSeconds = 86400;
constexpr std::size_t kMaximumMqttPayloadBytes = 80;

bool parse_uint32(
    const char *&cursor,
    std::uint32_t &value) noexcept
{
    if (cursor == nullptr || *cursor == '\0') {
        return false;
    }
    std::uint64_t parsed = 0;
    if (*cursor == '0') {
        ++cursor;
        if (*cursor >= '0' && *cursor <= '9') {
            return false;
        }
        value = 0;
        return true;
    }
    if (*cursor < '1' || *cursor > '9') {
        return false;
    }
    do {
        parsed = parsed * 10u +
                 static_cast<std::uint64_t>(*cursor - '0');
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        ++cursor;
    } while (*cursor >= '0' && *cursor <= '9');
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

template <std::size_t Count>
bool parse_uint32_array(
    const char *payload,
    std::uint32_t (&values)[Count]) noexcept
{
    if (payload == nullptr || *payload != '[') {
        return false;
    }
    const char *cursor = payload + 1;
    std::uint32_t parsed[Count]{};
    for (std::size_t index = 0; index < Count; ++index) {
        if (!parse_uint32(cursor, parsed[index])) {
            return false;
        }
        const char expected =
            index + 1 == Count ? ']' : ',';
        if (*cursor != expected) {
            return false;
        }
        ++cursor;
    }
    if (*cursor != '\0') {
        return false;
    }
    for (std::size_t index = 0; index < Count; ++index) {
        values[index] = parsed[index];
    }
    return true;
}

bool known_opcode(const CommandOpcode opcode) noexcept
{
    return opcode >= CommandOpcode::None &&
           opcode <= CommandOpcode::FotaPrepare;
}

bool valid_response(const CommandResponse response) noexcept
{
    if (response.request_id == 0 || !known_opcode(response.opcode)) {
        return false;
    }
    if (response.opcode == CommandOpcode::None) {
        return response.cmd_id == 0 && response.job_id == 0 &&
               response.ttl_seconds == 0;
    }
    return response.cmd_id != 0 && response.ttl_seconds != 0 &&
           response.ttl_seconds <= kMaximumCommandTtlSeconds;
}

bool valid_ack(const CommandAckMessage message) noexcept
{
    if (message.cmd_id == 0 || message.clock_valid > 1 ||
        (message.clock_valid == 0 && message.unix_seconds != 0)) {
        return false;
    }
    if (message.phase == CommandAckPhase::Accepted) {
        return message.result == CommandResult::Accepted &&
               message.error == CommandError::None;
    }
    if (message.phase != CommandAckPhase::Final) {
        return false;
    }
    if (message.result == CommandResult::Executed) {
        return message.error == CommandError::None;
    }
    if (message.result == CommandResult::Failed) {
        return message.error >= CommandError::InvalidOpcode &&
               message.error <= CommandError::Journal;
    }
    return message.result == CommandResult::Expired &&
           message.error == CommandError::Expired;
}

bool valid_receipt(const CommandAckReceipt receipt) noexcept
{
    if (receipt.cmd_id == 0 ||
        receipt.phase < CommandAckPhase::Accepted ||
        receipt.phase > CommandAckPhase::Final) {
        return false;
    }
    if (receipt.phase == CommandAckPhase::Accepted &&
        receipt.result != CommandResult::Accepted) {
        return false;
    }
    if (receipt.phase == CommandAckPhase::Final &&
        (receipt.result < CommandResult::Executed ||
         receipt.result > CommandResult::Expired)) {
        return false;
    }
    switch (receipt.receipt) {
    case CommandAckReceiptCode::Ingested:
        return receipt.error == 0;
    case CommandAckReceiptCode::Rejected:
        return receipt.error == 1;
    case CommandAckReceiptCode::Mismatch:
        return receipt.error >= 2 && receipt.error <= 4;
    case CommandAckReceiptCode::Invalid:
    default:
        return false;
    }
}

bool write_payload(
    const int written,
    const std::size_t output_size) noexcept
{
    return written > 0 &&
           static_cast<std::size_t>(written) < output_size &&
           static_cast<std::size_t>(written) <= kMaximumMqttPayloadBytes;
}

} // namespace

bool mqtt_command_request_build(
    const std::uint32_t request_id,
    const std::uint32_t last_cmd_id,
    char *const output,
    const std::size_t output_size) noexcept
{
    if (request_id == 0 || output == nullptr || output_size == 0) {
        return false;
    }
    const int written = std::snprintf(
        output,
        output_size,
        "[%lu,%lu]",
        static_cast<unsigned long>(request_id),
        static_cast<unsigned long>(last_cmd_id));
    return write_payload(written, output_size);
}

bool mqtt_command_response_parse(
    const char *const payload,
    const std::uint32_t expected_request_id,
    CommandResponse &output) noexcept
{
    if (expected_request_id == 0) {
        return false;
    }
    std::uint32_t values[5]{};
    if (!parse_uint32_array(payload, values)) {
        return false;
    }
    const CommandResponse parsed{
        values[0],
        values[1],
        static_cast<CommandOpcode>(values[2]),
        values[3],
        values[4],
    };
    if (parsed.request_id != expected_request_id ||
        !valid_response(parsed)) {
        return false;
    }
    output = parsed;
    return true;
}

bool mqtt_command_ack_build(
    const CommandAckMessage message,
    char *const output,
    const std::size_t output_size) noexcept
{
    if (!valid_ack(message) || output == nullptr || output_size == 0) {
        return false;
    }
    const int written = std::snprintf(
        output,
        output_size,
        "[%lu,%u,%u,%u,%lu,%u]",
        static_cast<unsigned long>(message.cmd_id),
        static_cast<unsigned>(message.phase),
        static_cast<unsigned>(message.result),
        static_cast<unsigned>(message.error),
        static_cast<unsigned long>(message.unix_seconds),
        static_cast<unsigned>(message.clock_valid));
    return write_payload(written, output_size);
}

bool mqtt_command_ack_receipt_parse(
    const char *const payload,
    CommandAckReceipt &output) noexcept
{
    std::uint32_t values[5]{};
    if (!parse_uint32_array(payload, values) ||
        values[1] > std::numeric_limits<std::uint8_t>::max() ||
        values[2] > std::numeric_limits<std::uint8_t>::max() ||
        values[3] > std::numeric_limits<std::uint8_t>::max() ||
        values[4] > std::numeric_limits<std::uint8_t>::max()) {
        return false;
    }
    const CommandAckReceipt parsed{
        values[0],
        static_cast<CommandAckPhase>(values[1]),
        static_cast<CommandResult>(values[2]),
        static_cast<CommandAckReceiptCode>(values[3]),
        static_cast<std::uint8_t>(values[4]),
    };
    if (!valid_receipt(parsed)) {
        return false;
    }
    output = parsed;
    return true;
}

bool mqtt_command_topic_payload_extract(
    const char *const frame,
    const char *const expected_topic,
    char *const output,
    const std::size_t output_size,
    std::size_t *const frame_bytes,
    std::size_t *const payload_bytes) noexcept
{
    constexpr const char *marker = "+KMQTT_DATA:";
    constexpr std::size_t marker_size = 12;
    if (frame == nullptr || expected_topic == nullptr ||
        expected_topic[0] == '\0' || output == nullptr ||
        output_size == 0) {
        return false;
    }

    const std::size_t expected_topic_size =
        std::strlen(expected_topic);
    const char *search = frame;
    bool extracted = false;
    while ((search = std::strstr(search, marker)) != nullptr) {
        const char *const frame_begin = search;
        const char *cursor = search + marker_size;
        while (*cursor == ' ') {
            ++cursor;
        }
        std::uint32_t session_id = 0;
        if (!parse_uint32(cursor, session_id) || session_id == 0 ||
            *cursor != ',' || cursor[1] != '"') {
            search += marker_size;
            continue;
        }
        cursor += 2;
        const char *const topic_begin = cursor;
        const char *const topic_end = std::strchr(topic_begin, '"');
        if (topic_end == nullptr) {
            return extracted;
        }
        const std::size_t topic_size =
            static_cast<std::size_t>(topic_end - topic_begin);
        const bool topic_matches =
            topic_size == expected_topic_size &&
            std::memcmp(topic_begin, expected_topic, topic_size) == 0;
        if (!topic_matches) {
            search = topic_end + 1;
            continue;
        }
        if (topic_end[1] != ',' || topic_end[2] != '"' ||
            topic_end[3] != '[') {
            return extracted;
        }

        const char *const payload_begin = topic_end + 3;
        const char *const payload_end = std::strchr(payload_begin, ']');
        if (payload_end == nullptr || payload_end[1] != '"' ||
            payload_end[2] != '\r' || payload_end[3] != '\n') {
            return extracted;
        }
        for (const char *scan = payload_begin; scan <= payload_end; ++scan) {
            const char value = *scan;
            if (!((value >= '0' && value <= '9') ||
                  value == '[' || value == ']' || value == ',')) {
                return extracted;
            }
        }

        const std::size_t extracted_payload_bytes =
            static_cast<std::size_t>(payload_end - payload_begin) + 1;
        if (extracted_payload_bytes >= output_size ||
            extracted_payload_bytes > kMaximumMqttPayloadBytes) {
            return extracted;
        }
        std::memcpy(output, payload_begin, extracted_payload_bytes);
        output[extracted_payload_bytes] = '\0';
        if (frame_bytes != nullptr) {
            *frame_bytes = static_cast<std::size_t>(
                payload_end + 4 - frame_begin);
        }
        if (payload_bytes != nullptr) {
            *payload_bytes = extracted_payload_bytes;
        }
        extracted = true;
        search = payload_end + 4;
    }
    return extracted;
}

} // namespace boot_v2
