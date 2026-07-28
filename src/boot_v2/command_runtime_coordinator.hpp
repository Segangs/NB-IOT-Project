#ifndef NB_IOT_BOOT_V2_COMMAND_RUNTIME_COORDINATOR_HPP
#define NB_IOT_BOOT_V2_COMMAND_RUNTIME_COORDINATOR_HPP

#include <cstdint>

#include "command_ack_core.hpp"
#include "command_boot_effect_core.hpp"
#include "command_journal_store_core.hpp"

namespace boot_v2 {

enum class CommandRuntimePrepareResult : std::uint8_t {
    FailedClosed = 0,
    ReadyBlank = 1,
    ReadyLoaded = 2,
    ReadyRecovered = 3,
};

enum class CommandRuntimeExecutionResult : std::uint8_t {
    Rejected = 0,
    Deferred = 1,
    Expired = 2,
    StatusSucceeded = 3,
    TerminalFailed = 4,
    ShutdownDispatched = 5,
    AwaitingBootEffect = 6,
};

enum class CommandShutdownDispatchResult : std::uint8_t {
    Rejected = 0,
    Accepted = 1,
};

using CommandShutdownDispatchFn =
    CommandShutdownDispatchResult (*)(
        void *, CommandOpcode, std::uint32_t) noexcept;

struct CommandShutdownDispatchPort {
    void *context{nullptr};
    CommandShutdownDispatchFn dispatch{nullptr};
};

using CommandStatusSnapshotFn = bool (*)(void *) noexcept;

struct CommandStatusSnapshotPort {
    void *context{nullptr};
    CommandStatusSnapshotFn validate_fresh{nullptr};
};

struct CommandRuntimeTerminalCommand {
    std::uint32_t cmd_id{0};
    CommandResult result{CommandResult::None};
    CommandError error{CommandError::None};
};

class CommandRuntimeCoordinator {
public:
    explicit CommandRuntimeCoordinator(
        CommandJournalStorePort store) noexcept;
    CommandRuntimeCoordinator(const CommandRuntimeCoordinator &) = delete;
    CommandRuntimeCoordinator &operator=(
        const CommandRuntimeCoordinator &) = delete;
    CommandRuntimeCoordinator(CommandRuntimeCoordinator &&) = delete;
    CommandRuntimeCoordinator &operator=(
        CommandRuntimeCoordinator &&) = delete;
    ~CommandRuntimeCoordinator() noexcept = default;

    [[nodiscard]] CommandRuntimePrepareResult prepare(
        const CommandBootEffectEvidence &evidence = {}) noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] std::uint32_t current_boot_sequence() const noexcept;

    [[nodiscard]] bool begin_poll(
        std::uint32_t request_id,
        std::uint32_t last_cmd_id) noexcept;
    [[nodiscard]] CommandAcceptResult accept_response(
        CommandResponse response,
        std::uint32_t received_at_monotonic_seconds) noexcept;
    [[nodiscard]] CommandTransitionResult prepare_ack(
        CommandAckMessage &message) noexcept;
    [[nodiscard]] CommandTransitionResult record_puback(
        CommandAckPhase phase) noexcept;
    [[nodiscard]] CommandTransitionResult record_receipt(
        CommandAckReceipt receipt) noexcept;
    [[nodiscard]] CommandRuntimeExecutionResult execute_pending(
        std::uint32_t current_monotonic_seconds,
        CommandStatusSnapshotPort status_snapshot,
        CommandShutdownDispatchPort shutdown = {}) noexcept;
    [[nodiscard]] CommandTransitionResult
    clear_final_receipted() noexcept;

    [[nodiscard]] CommandJournalState state() const noexcept;
    [[nodiscard]] const CommandJournalRecord &record() const noexcept;
    [[nodiscard]] std::uint32_t last_completed_cmd_id() const noexcept;
    [[nodiscard]] CommandRuntimeTerminalCommand
    last_terminal_command() const noexcept;

private:
    void capture_terminal_command(
        const CommandJournalRecord &record) noexcept;
    [[nodiscard]] bool persist_transition(
        CommandJournalRecord previous) noexcept;
    [[nodiscard]] bool reload_after_failed_commit(
        CommandJournalRecord previous,
        CommandJournalRecord intended) noexcept;

    CommandJournalStorePort store_{};
    CommandAckCore core_{};
    CommandRuntimeTerminalCommand last_terminal_command_{};
    std::uint32_t current_boot_sequence_{0};
    std::uint8_t ready_{0};
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_COMMAND_RUNTIME_COORDINATOR_HPP
