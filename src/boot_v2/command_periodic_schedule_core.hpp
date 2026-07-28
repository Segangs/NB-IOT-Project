#ifndef NB_IOT_BOOT_V2_COMMAND_PERIODIC_SCHEDULE_CORE_HPP
#define NB_IOT_BOOT_V2_COMMAND_PERIODIC_SCHEDULE_CORE_HPP

#include <cstdint>

namespace boot_v2 {

inline constexpr std::uint32_t COMMAND_CONFIG_PULL_INTERVAL_MS =
    20u * 60u * 1000u;
inline constexpr std::uint32_t COMMAND_PULL_INTERVAL_MS =
    20u * 60u * 1000u;

enum class CommandPeriodicAction : std::uint8_t {
    Idle = 0,
    PullConfig = 1,
    PullCommand = 2,
};

enum class CommandPeriodicStepResult : std::uint8_t {
    Idle = 0,
    ConfigRejected = 1,
    ConfigAccepted = 2,
    CommandRejected = 3,
    CommandAccepted = 4,
};

struct CommandPeriodicScheduleInput {
    std::uint32_t now_ms{0};
    std::uint32_t last_config_ms{0};
    std::uint32_t last_command_ms{0};
    std::uint8_t runtime_ready{0};
    std::uint8_t recovery_pending{0};
    std::uint8_t reserved[2]{};
};

[[nodiscard]] constexpr CommandPeriodicAction command_periodic_schedule(
    const CommandPeriodicScheduleInput input) noexcept
{
    if (input.runtime_ready != 1 || input.recovery_pending != 0 ||
        input.reserved[0] != 0 || input.reserved[1] != 0) {
        return CommandPeriodicAction::Idle;
    }
    if (input.now_ms - input.last_config_ms >=
        COMMAND_CONFIG_PULL_INTERVAL_MS) {
        return CommandPeriodicAction::PullConfig;
    }
    if (input.now_ms - input.last_command_ms >=
        COMMAND_PULL_INTERVAL_MS) {
        return CommandPeriodicAction::PullCommand;
    }
    return CommandPeriodicAction::Idle;
}

using CommandPeriodicPullFn = bool (*)(void *) noexcept;

struct CommandPeriodicStepPort {
    void *context{nullptr};
    CommandPeriodicPullFn pull_config{nullptr};
    CommandPeriodicPullFn pull_command{nullptr};
};

struct CommandPeriodicStepInput {
    std::uint32_t now_ms{0};
    std::uint8_t runtime_ready{0};
    std::uint8_t recovery_pending{0};
    std::uint8_t reserved[2]{};
};

class CommandPeriodicStepper {
public:
    explicit constexpr CommandPeriodicStepper(
        const std::uint32_t initial_ms) noexcept
        : last_config_ms_(initial_ms),
          last_command_ms_(initial_ms)
    {
    }

    [[nodiscard]] CommandPeriodicStepResult step(
        const CommandPeriodicStepInput input,
        const CommandPeriodicStepPort port) noexcept
    {
        const CommandPeriodicAction action =
            command_periodic_schedule({
                input.now_ms,
                last_config_ms_,
                last_command_ms_,
                input.runtime_ready,
                input.recovery_pending,
                {input.reserved[0], input.reserved[1]}});
        if (action == CommandPeriodicAction::PullConfig) {
            if (port.pull_config == nullptr ||
                !port.pull_config(port.context)) {
                return CommandPeriodicStepResult::ConfigRejected;
            }
            last_config_ms_ = input.now_ms;
            return CommandPeriodicStepResult::ConfigAccepted;
        }
        if (action == CommandPeriodicAction::PullCommand) {
            if (port.pull_command == nullptr ||
                !port.pull_command(port.context)) {
                return CommandPeriodicStepResult::CommandRejected;
            }
            last_command_ms_ = input.now_ms;
            return CommandPeriodicStepResult::CommandAccepted;
        }
        return CommandPeriodicStepResult::Idle;
    }

    [[nodiscard]] constexpr std::uint32_t last_config_ms() const noexcept
    {
        return last_config_ms_;
    }

    [[nodiscard]] constexpr std::uint32_t last_command_ms() const noexcept
    {
        return last_command_ms_;
    }

private:
    std::uint32_t last_config_ms_{0};
    std::uint32_t last_command_ms_{0};
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_COMMAND_PERIODIC_SCHEDULE_CORE_HPP
