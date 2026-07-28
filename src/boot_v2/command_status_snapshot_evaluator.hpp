#ifndef NB_IOT_BOOT_V2_COMMAND_STATUS_SNAPSHOT_EVALUATOR_HPP
#define NB_IOT_BOOT_V2_COMMAND_STATUS_SNAPSHOT_EVALUATOR_HPP

#include <array>
#include <cstdint>

#include "mqtt_command_codec.hpp"
#include "runtime_owner_rtos.hpp"
#include "runtime_snapshot_core.hpp"

namespace boot_v2 {

struct RuntimeOwnerLastCommand {
    std::uint32_t command_id{0};
    RuntimeCommandResult result{RuntimeCommandResult::NoCommand};
};

class RuntimeOwnerLastCommandState {
public:
    void reset_for_boot(
        std::uint32_t command_id,
        bool known_succeeded) noexcept;
    [[nodiscard]] bool observe_terminal(
        std::uint32_t command_id,
        CommandResult result,
        CommandError error) noexcept;
    [[nodiscard]] RuntimeOwnerLastCommand value() const noexcept;

private:
    RuntimeOwnerLastCommand value_{};
};

struct CommandStatusSnapshotSample {
    RuntimeOwnerRedactedStatus owner{};
    RuntimeOwnerRtosDrainMetrics metrics{};
    std::array<SensorQualitySnapshotV1, 2> sensors{};
    std::uint32_t config_version{0};
    std::uint32_t last_command_id{0};
    RuntimeCommandResult last_command_result{
        RuntimeCommandResult::NoCommand};
    std::uint8_t network_connected{0};
    std::uint8_t battery_mode{0};
    std::uint8_t alarm_active{0};
    std::uint8_t reserved{0};
};

using CommandStatusSnapshotSampleFn = bool (*)(
    void *,
    CommandStatusSnapshotSample &) noexcept;

struct CommandStatusSnapshotSourcePort {
    void *context{nullptr};
    CommandStatusSnapshotSampleFn sample{nullptr};
};

class CommandStatusSnapshotEvaluator {
public:
    explicit CommandStatusSnapshotEvaluator(
        CommandStatusSnapshotSourcePort source) noexcept;
    CommandStatusSnapshotEvaluator(
        const CommandStatusSnapshotEvaluator &) = delete;
    CommandStatusSnapshotEvaluator &operator=(
        const CommandStatusSnapshotEvaluator &) = delete;
    CommandStatusSnapshotEvaluator(
        CommandStatusSnapshotEvaluator &&) = delete;
    CommandStatusSnapshotEvaluator &operator=(
        CommandStatusSnapshotEvaluator &&) = delete;
    ~CommandStatusSnapshotEvaluator() noexcept = default;

    [[nodiscard]] bool validate_fresh(
        RuntimeStatusSnapshotV1 &output) noexcept;

private:
    CommandStatusSnapshotSourcePort source_{};
    std::uint32_t revision_{0};
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_COMMAND_STATUS_SNAPSHOT_EVALUATOR_HPP
