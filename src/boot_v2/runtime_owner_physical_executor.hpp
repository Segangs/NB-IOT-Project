#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_PHYSICAL_EXECUTOR_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_PHYSICAL_EXECUTOR_HPP

#include <cstdint>
#include <type_traits>

#include "runtime_owner_task_core.hpp"

namespace boot_v2 {

enum class RuntimeOwnerPhysicalResultKind : std::uint8_t {
    Invalid = 0,
    Succeeded = 1,
    Failed = 2,
    TimedOut = 3,
    Cancelled = 4,
};

struct RuntimeOwnerPhysicalResult {
    RuntimeOwnerPhysicalResultKind kind{RuntimeOwnerPhysicalResultKind::Invalid};
    std::uint8_t reserved[3]{};
    std::uint32_t diagnostic_code{0};
    std::uint32_t mqtt_session_id{0};
    std::uint32_t config_commit_sequence{0};
    BootRuntimeSnapshotV1 boot_snapshot{};
};

enum class RuntimeOwnerPhysicalStepResult : std::uint8_t {
    NoCommand = 0,
    Completed = 1,
    Rejected = 2,
    Terminal = 3,
};

namespace runtime_owner_physical_executor_detail {

[[nodiscard]] constexpr bool result_kind_known(
    const RuntimeOwnerPhysicalResultKind kind) noexcept
{
    return kind >= RuntimeOwnerPhysicalResultKind::Succeeded &&
           kind <= RuntimeOwnerPhysicalResultKind::Cancelled;
}

[[nodiscard]] constexpr bool result_header_is_canonical(
    const RuntimeOwnerPhysicalResult &result) noexcept
{
    if (!result_kind_known(result.kind) || result.reserved[0] != 0 ||
        result.reserved[1] != 0 || result.reserved[2] != 0) {
        return false;
    }
    return result.kind == RuntimeOwnerPhysicalResultKind::Succeeded
               ? result.diagnostic_code == 0
               : result.diagnostic_code != 0;
}

[[nodiscard]] inline bool result_is_canonical_for(
    const RuntimeOwnerExecutorCommand &command,
    const RuntimeOwnerPhysicalResult &result) noexcept
{
    if (!result_header_is_canonical(result)) {
        return false;
    }
    if (command.kind == RuntimeOwnerDeviceOperationKind::OpenTransport) {
        return result.kind != RuntimeOwnerPhysicalResultKind::Succeeded ||
               (result.mqtt_session_id != 0 &&
                result.config_commit_sequence != 0);
    }
    if (result.mqtt_session_id != 0 || result.config_commit_sequence != 0) {
        return false;
    }
    if (command.kind ==
            RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot &&
        result.kind == RuntimeOwnerPhysicalResultKind::Succeeded) {
        return boot_runtime_snapshot_is_canonical(result.boot_snapshot) &&
               result.boot_snapshot.mqtt_session_id ==
                   command.source.effect.attempt.mqtt_session_id &&
               result.boot_snapshot.mqtt_generation ==
                   command.source.effect.attempt.mqtt_generation &&
               result.boot_snapshot.config_apply_epoch ==
                   command.source.effect.attempt.config_apply_epoch;
    }
    return true;
}

[[nodiscard]] constexpr bool accepted(
    const RuntimeOwnerExecutorResult result) noexcept
{
    return result == RuntimeOwnerExecutorResult::Accepted ||
           result == RuntimeOwnerExecutorResult::AcceptedDuplicate;
}

template <typename Port>
[[nodiscard]] RuntimeOwnerExecutorResult submit_failure(
    Port &port,
    const RuntimeOwnerExecutorCommand &command,
    const RuntimeOwnerPhysicalResult &result) noexcept
{
    if (command.completion_policy == CompletionPolicy::NormalCompletion) {
        if (result.kind == RuntimeOwnerPhysicalResultKind::TimedOut) {
            return port.normal_timed_out(command, result.diagnostic_code);
        }
        if (result.kind == RuntimeOwnerPhysicalResultKind::Cancelled) {
            return port.normal_cancelled(command, result.diagnostic_code);
        }
        return port.normal_failed(command, result.diagnostic_code);
    }
    if (command.kind == RuntimeOwnerDeviceOperationKind::OpenTransport) {
        return port.transport_failed(command, result.diagnostic_code);
    }
    if (command.kind ==
        RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot) {
        return port.snapshot_failed(command, result.diagnostic_code);
    }
    if (result.kind == RuntimeOwnerPhysicalResultKind::TimedOut) {
        return port.liveness_deadline_expired(
            command, result.diagnostic_code);
    }
    return port.liveness_failed(command, result.diagnostic_code);
}

} // namespace runtime_owner_physical_executor_detail

template <typename Port, typename Backend>
[[nodiscard]] RuntimeOwnerPhysicalStepResult
runtime_owner_submit_deferred_config(
    Port &port,
    Backend &backend) noexcept
{
    const std::uint32_t sequence =
        backend.pending_config_commit_sequence();
    if (sequence == 0) {
        return RuntimeOwnerPhysicalStepResult::NoCommand;
    }
    const RuntimeOwnerExecutorResult submitted =
        port.config_committed(sequence);
    if (runtime_owner_physical_executor_detail::accepted(submitted)) {
        backend.clear_pending_config_commit();
        return RuntimeOwnerPhysicalStepResult::Completed;
    }
    return submitted == RuntimeOwnerExecutorResult::RejectedTerminalDropped
               ? RuntimeOwnerPhysicalStepResult::Terminal
               : RuntimeOwnerPhysicalStepResult::Rejected;
}

template <typename Port, typename Backend>
[[nodiscard]] RuntimeOwnerPhysicalStepResult runtime_owner_execute_one(
    Port &port,
    Backend &backend) noexcept
{
    RuntimeOwnerExecutorCommand command{};
    const RuntimeOwnerExecutorResult peek = port.peek_command(command);
    if (peek == RuntimeOwnerExecutorResult::RejectedNoCommand) {
        return RuntimeOwnerPhysicalStepResult::NoCommand;
    }
    if (peek == RuntimeOwnerExecutorResult::RejectedTerminalDropped) {
        return RuntimeOwnerPhysicalStepResult::Terminal;
    }
    if (peek != RuntimeOwnerExecutorResult::Accepted) {
        return RuntimeOwnerPhysicalStepResult::Rejected;
    }

    const bool end_boot = command.kind ==
        RuntimeOwnerDeviceOperationKind::EndBootOrchestration;
    if (!end_boot) {
        const RuntimeOwnerExecutorResult acknowledged =
            port.acknowledge_command(command);
        if (!runtime_owner_physical_executor_detail::accepted(acknowledged)) {
            return acknowledged ==
                           RuntimeOwnerExecutorResult::RejectedTerminalDropped
                       ? RuntimeOwnerPhysicalStepResult::Terminal
                       : RuntimeOwnerPhysicalStepResult::Rejected;
        }
    }

    const RuntimeOwnerPhysicalResult result = backend.execute(command);
    if (!runtime_owner_physical_executor_detail::result_is_canonical_for(
            command, result)) {
        return RuntimeOwnerPhysicalStepResult::Rejected;
    }

    RuntimeOwnerExecutorResult submitted{
        RuntimeOwnerExecutorResult::RejectedWrongCommand};
    if (end_boot) {
        if (result.kind != RuntimeOwnerPhysicalResultKind::Succeeded) {
            return RuntimeOwnerPhysicalStepResult::Rejected;
        }
        submitted = port.commit_end_boot_delivery(command);
    } else if (result.kind != RuntimeOwnerPhysicalResultKind::Succeeded) {
        if (command.completion_policy == CompletionPolicy::DeliveryOnly) {
            return RuntimeOwnerPhysicalStepResult::Rejected;
        }
        submitted = runtime_owner_physical_executor_detail::submit_failure(
            port, command, result);
    } else if (command.kind ==
               RuntimeOwnerDeviceOperationKind::OpenTransport) {
        submitted = port.transport_established(
            command, result.mqtt_session_id);
        if (!runtime_owner_physical_executor_detail::accepted(submitted)) {
            return submitted ==
                           RuntimeOwnerExecutorResult::RejectedTerminalDropped
                       ? RuntimeOwnerPhysicalStepResult::Terminal
                       : RuntimeOwnerPhysicalStepResult::Rejected;
        }
        backend.defer_config_commit(result.config_commit_sequence);
        return RuntimeOwnerPhysicalStepResult::Completed;
    } else if (command.kind ==
               RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot) {
        submitted = port.snapshot_succeeded(command, result.boot_snapshot);
    } else if (command.completion_policy ==
               CompletionPolicy::NormalCompletion) {
        submitted = port.normal_succeeded(command);
    } else if (command.completion_policy ==
               CompletionPolicy::TrustedReceipt) {
        submitted = port.liveness_succeeded(command);
    } else if (command.completion_policy == CompletionPolicy::DeliveryOnly) {
        return RuntimeOwnerPhysicalStepResult::Completed;
    }

    if (runtime_owner_physical_executor_detail::accepted(submitted)) {
        return RuntimeOwnerPhysicalStepResult::Completed;
    }
    return submitted == RuntimeOwnerExecutorResult::RejectedTerminalDropped
               ? RuntimeOwnerPhysicalStepResult::Terminal
               : RuntimeOwnerPhysicalStepResult::Rejected;
}

namespace runtime_owner_physical_executor_contract_detail {

static_assert(std::is_same<
              typename std::underlying_type<
                  RuntimeOwnerPhysicalResultKind>::type,
              std::uint8_t>::value);
static_assert(std::is_same<
              typename std::underlying_type<
                  RuntimeOwnerPhysicalStepResult>::type,
              std::uint8_t>::value);
static_assert(sizeof(RuntimeOwnerPhysicalResult) == 92);
static_assert(alignof(RuntimeOwnerPhysicalResult) == 4);
static_assert(std::is_standard_layout<RuntimeOwnerPhysicalResult>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerPhysicalResult>::value);

} // namespace runtime_owner_physical_executor_contract_detail

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_PHYSICAL_EXECUTOR_HPP
