#ifndef NB_IOT_BOOT_V2_COMMAND_ACK_CORE_HPP
#define NB_IOT_BOOT_V2_COMMAND_ACK_CORE_HPP

#include <cstdint>

#include "mqtt_command_codec.hpp"

namespace boot_v2 {

enum class CommandJournalState : std::uint8_t {
    Empty = 0,
    AcceptedPersisted = 1,
    AcceptedPublishPending = 2,
    AcceptedPuback = 3,
    AcceptedReceipted = 4,
    ExecuteMarked = 5,
    Executed = 6,
    FinalPersisted = 7,
    FinalPublishPending = 8,
    FinalPuback = 9,
    FinalReceipted = 10,
};

enum class CommandAcceptResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedNoPoll = 1,
    RejectedStaleRequest = 2,
    RejectedDuplicate = 3,
    NoCommand = 4,
    Accepted = 5,
};

enum class CommandTransitionResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedState = 1,
    RejectedMismatch = 2,
    RejectedRemote = 3,
    Accepted = 4,
};

enum class CommandExecutionDecision : std::uint8_t {
    Rejected = 0,
    Expired = 1,
    Dispatch = 2,
};

enum class CommandExpectedEffect : std::uint8_t {
    None = 0,
    Reset = 1,
    PowerOff = 2,
};

struct CommandJournalRecord {
    std::uint8_t schema_version{1};
    CommandJournalState state{CommandJournalState::Empty};
    CommandOpcode opcode{CommandOpcode::None};
    CommandAckPhase phase{CommandAckPhase::Invalid};
    CommandResult result{CommandResult::None};
    CommandError error{CommandError::None};
    std::uint8_t retry_count{0};
    CommandExpectedEffect expected_effect{CommandExpectedEffect::None};
    std::uint8_t ttl_checkpoint_clock_valid{0};
    std::uint8_t dispatch_latched{0};
    std::uint8_t reserved{0};
    std::uint32_t cmd_id{0};
    std::uint32_t job_id{0};
    std::uint32_t ttl_seconds{0};
    std::uint32_t remaining_ttl_seconds{0};
    std::uint32_t ttl_checkpoint_monotonic_seconds{0};
    std::uint32_t ttl_checkpoint_unix_seconds{0};
    std::uint32_t ttl_checkpoint_boot_sequence{0};
    std::uint32_t boot_sequence_before_execute{0};
};

class CommandAckCore {
public:
    CommandAckCore() noexcept = default;
    CommandAckCore(const CommandAckCore &) = delete;
    CommandAckCore &operator=(const CommandAckCore &) = delete;
    CommandAckCore(CommandAckCore &&) = delete;
    CommandAckCore &operator=(CommandAckCore &&) = delete;
    ~CommandAckCore() noexcept = default;

    [[nodiscard]] bool begin_poll(
        std::uint32_t request_id,
        std::uint32_t last_cmd_id) noexcept;
    [[nodiscard]] CommandAcceptResult accept_response(
        CommandResponse response,
        std::uint32_t received_at_monotonic_seconds,
        std::uint32_t boot_sequence) noexcept;
    [[nodiscard]] CommandTransitionResult prepare_ack(
        CommandAckMessage &message) noexcept;
    [[nodiscard]] CommandTransitionResult record_puback(
        CommandAckPhase phase) noexcept;
    [[nodiscard]] CommandTransitionResult record_receipt(
        CommandAckReceipt receipt) noexcept;
    [[nodiscard]] CommandExecutionDecision mark_execute(
        std::uint32_t current_monotonic_seconds) noexcept;
    [[nodiscard]] CommandTransitionResult complete_execution(
        bool succeeded,
        CommandError error) noexcept;
    [[nodiscard]] CommandTransitionResult clear_final_receipted() noexcept;
    [[nodiscard]] CommandTransitionResult restore_after_boot(
        CommandJournalRecord record,
        std::uint32_t current_boot_sequence,
        bool effect_confirmed) noexcept;
    [[nodiscard]] bool synchronize_from_journal(
        CommandJournalRecord record) noexcept;

    [[nodiscard]] CommandJournalState state() const noexcept;
    [[nodiscard]] const CommandJournalRecord &record() const noexcept;
    [[nodiscard]] std::uint32_t last_completed_cmd_id() const noexcept;

private:
    std::uint32_t pending_request_id_{0};
    std::uint32_t last_completed_cmd_id_{0};
    CommandJournalRecord record_{};
};

[[nodiscard]] bool command_journal_record_is_canonical(
    CommandJournalRecord record) noexcept;

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_COMMAND_ACK_CORE_HPP
