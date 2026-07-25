#ifndef NB_IOT_BOOT_V2_MQTT_COMMAND_CODEC_HPP
#define NB_IOT_BOOT_V2_MQTT_COMMAND_CODEC_HPP

#include <cstddef>
#include <cstdint>

namespace boot_v2 {

enum class CommandOpcode : std::uint8_t {
    None = 0,
    Reboot = 1,
    PowerOff = 2,
    RequestStatus = 3,
    FotaPrepare = 4,
};

enum class CommandAckPhase : std::uint8_t {
    Invalid = 0,
    Accepted = 1,
    Final = 2,
};

enum class CommandResult : std::uint8_t {
    None = 0,
    Accepted = 1,
    Executed = 2,
    Failed = 3,
    Expired = 4,
};

enum class CommandError : std::uint8_t {
    None = 0,
    InvalidOpcode = 1,
    Duplicate = 2,
    Execution = 3,
    Journal = 4,
    Expired = 5,
};

enum class CommandAckReceiptCode : std::uint8_t {
    Invalid = 0,
    Ingested = 1,
    Rejected = 2,
    Mismatch = 3,
};

struct CommandResponse {
    std::uint32_t request_id{0};
    std::uint32_t cmd_id{0};
    CommandOpcode opcode{CommandOpcode::None};
    std::uint32_t job_id{0};
    std::uint32_t ttl_seconds{0};
};

struct CommandAckMessage {
    std::uint32_t cmd_id{0};
    CommandAckPhase phase{CommandAckPhase::Invalid};
    CommandResult result{CommandResult::None};
    CommandError error{CommandError::None};
    std::uint32_t unix_seconds{0};
    std::uint8_t clock_valid{0};
};

struct CommandAckReceipt {
    std::uint32_t cmd_id{0};
    CommandAckPhase phase{CommandAckPhase::Invalid};
    CommandResult result{CommandResult::None};
    CommandAckReceiptCode receipt{CommandAckReceiptCode::Invalid};
    std::uint8_t error{0};
};

[[nodiscard]] bool mqtt_command_request_build(
    std::uint32_t request_id,
    std::uint32_t last_cmd_id,
    char *output,
    std::size_t output_size) noexcept;

[[nodiscard]] bool mqtt_command_response_parse(
    const char *payload,
    std::uint32_t expected_request_id,
    CommandResponse &output) noexcept;

[[nodiscard]] bool mqtt_command_ack_build(
    CommandAckMessage message,
    char *output,
    std::size_t output_size) noexcept;

[[nodiscard]] bool mqtt_command_ack_receipt_parse(
    const char *payload,
    CommandAckReceipt &output) noexcept;

[[nodiscard]] bool mqtt_command_topic_payload_extract(
    const char *frame,
    const char *expected_topic,
    char *output,
    std::size_t output_size,
    std::size_t *frame_bytes,
    std::size_t *payload_bytes) noexcept;

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_MQTT_COMMAND_CODEC_HPP
