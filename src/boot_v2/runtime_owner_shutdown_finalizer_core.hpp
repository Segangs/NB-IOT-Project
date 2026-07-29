#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_SHUTDOWN_FINALIZER_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_SHUTDOWN_FINALIZER_CORE_HPP

#include <cstdint>
#include <type_traits>

#include "runtime_owner_rtos_drain_core.hpp"
#include "runtime_snapshot_core.hpp"

namespace boot_v2 {

enum class RuntimeOwnerShutdownCleanupStep : std::uint8_t {
    Invalid = 0,
    StopOutputs = 1,
    PublishDying = 2,
    CloseDeleteSessions = 3,
    ScanSessions = 4,
    DisconnectPdp = 5,
    SetCfun0 = 6,
    PowerOffModem = 7,
};

enum class RuntimeOwnerShutdownStepResult : std::uint8_t {
    Invalid = 0,
    Succeeded = 1,
    Failed = 2,
    TimedOut = 3,
    Skipped = 4,
};

enum class RuntimeOwnerShutdownFinalizeAction : std::uint8_t {
    Idle = 0,
    RunCleanupStep = 1,
    RecheckUsb = 2,
    CommitWatchdog = 3,
    CommitGp15Kill = 4,
    AbortUsbChanged = 5,
    AbortEvidenceMissing = 6,
    Terminal = 7,
};

enum class RuntimeOwnerShutdownStartResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedUnsafe = 1,
    Started = 2,
    AcceptedDuplicate = 3,
};

enum class RuntimeOwnerShutdownCompletionResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedWrongStep = 1,
    RejectedTerminal = 2,
    Accepted = 3,
};

enum class RuntimeOwnerShutdownUsbResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedTerminal = 1,
    UsbChanged = 2,
    EvidenceMissing = 3,
    WatchdogAllowed = 4,
    Gp15Allowed = 5,
};

struct RuntimeOwnerShutdownDirective {
    RuntimeOwnerShutdownFinalizeAction action{
        RuntimeOwnerShutdownFinalizeAction::Idle};
    RuntimeOwnerShutdownCleanupStep step{
        RuntimeOwnerShutdownCleanupStep::Invalid};
    std::uint8_t cleanup_skipped_mask{0};
    std::uint8_t cleanup_timed_out_mask{0};
    std::uint8_t hard_deadline{0};
    std::uint8_t cleanup_failed_mask{0};
    std::uint8_t initial_usb_present{0};
    std::uint8_t reserved[1]{};
    std::uint32_t remaining_ms{0};
};

static_assert(sizeof(RuntimeOwnerShutdownDirective) == 12);
static_assert(alignof(RuntimeOwnerShutdownDirective) == 4);
static_assert(
    std::is_standard_layout<RuntimeOwnerShutdownDirective>::value);
static_assert(
    std::is_trivially_copyable<RuntimeOwnerShutdownDirective>::value);

[[nodiscard]] constexpr std::uint8_t runtime_owner_shutdown_step_mask(
    const RuntimeOwnerShutdownCleanupStep step) noexcept
{
    const auto value = static_cast<std::uint8_t>(step);
    return value >=
                   static_cast<std::uint8_t>(
                       RuntimeOwnerShutdownCleanupStep::StopOutputs) &&
               value <=
                   static_cast<std::uint8_t>(
                       RuntimeOwnerShutdownCleanupStep::PowerOffModem)
               ? static_cast<std::uint8_t>(1u << (value - 1u))
               : 0;
}

class RuntimeOwnerShutdownFinalizerCore {
public:
    [[nodiscard]] RuntimeOwnerShutdownStartResult start(
        RuntimeOwnerUrgentMessage context,
        UsbPowerObservation initial_usb,
        std::uint32_t started_at_monotonic_ms,
        std::uint32_t hard_deadline_duration_ms) noexcept;

    [[nodiscard]] RuntimeOwnerShutdownDirective next(
        std::uint32_t now_monotonic_ms) noexcept;

    [[nodiscard]] RuntimeOwnerShutdownCompletionResult complete(
        RuntimeOwnerShutdownCleanupStep step,
        RuntimeOwnerShutdownStepResult result,
        std::uint32_t now_monotonic_ms) noexcept;

    [[nodiscard]] RuntimeOwnerShutdownUsbResult submit_usb_recheck(
        UsbPowerObservation observation) noexcept;

    [[nodiscard]] bool shutdown_context(
        RuntimeOwnerUrgentMessage &output) const noexcept;

private:
    enum class Phase : std::uint8_t {
        Idle = 0,
        Running = 1,
        AwaitingUsb = 2,
        WatchdogAllowed = 3,
        Gp15Allowed = 4,
    };

    [[nodiscard]] RuntimeOwnerShutdownDirective directive(
        RuntimeOwnerShutdownFinalizeAction action,
        RuntimeOwnerShutdownCleanupStep step,
        std::uint32_t remaining_ms) const noexcept;
    [[nodiscard]] bool deadline_reached(
        std::uint32_t now_monotonic_ms) const noexcept;
    [[nodiscard]] std::uint32_t remaining(
        std::uint32_t now_monotonic_ms) const noexcept;

    Phase phase_{Phase::Idle};
    std::uint8_t step_index_{0};
    std::uint8_t cleanup_skipped_mask_{0};
    std::uint8_t cleanup_timed_out_mask_{0};
    std::uint8_t cleanup_failed_mask_{0};
    std::uint8_t hard_deadline_reached_{0};
    RuntimeOwnerUrgentMessage context_{};
    UsbPowerObservation initial_usb_{};
    std::uint32_t started_at_monotonic_ms_{0};
    std::uint32_t hard_deadline_duration_ms_{0};
};

} // namespace boot_v2

#endif
