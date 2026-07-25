#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_DEVICE_BACKEND_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_DEVICE_BACKEND_HPP

#include <cstdint>

#include "command_ack_core.hpp"
#include "runtime_owner_physical_executor.hpp"
#include "runtime_owner_shutdown_finalizer_core.hpp"

namespace boot_v2 {

class RuntimeOwnerRtosOwnerLoop;

class RuntimeOwnerDeviceBackend {
public:
    RuntimeOwnerDeviceBackend() noexcept = default;
    RuntimeOwnerDeviceBackend(const RuntimeOwnerDeviceBackend &) = delete;
    RuntimeOwnerDeviceBackend &operator=(const RuntimeOwnerDeviceBackend &) =
        delete;
    RuntimeOwnerDeviceBackend(RuntimeOwnerDeviceBackend &&) = delete;
    RuntimeOwnerDeviceBackend &operator=(RuntimeOwnerDeviceBackend &&) = delete;
    ~RuntimeOwnerDeviceBackend() noexcept = default;

    [[nodiscard]] bool prepare() noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult execute(
        RuntimeOwnerExecutorCommand command) noexcept;
    void defer_config_commit(std::uint32_t sequence) noexcept;
    [[nodiscard]] std::uint32_t
    pending_config_commit_sequence() const noexcept;
    void clear_pending_config_commit() noexcept;

private:
    friend class RuntimeOwnerRtosOwnerLoop;

    [[nodiscard]] RuntimeOwnerShutdownStepResult execute_shutdown_cleanup(
        const RuntimeOwnerShutdownDirective &directive,
        RuntimeOwnerUrgentMessage context) noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult open_transport(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult publish_boot_report() noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult publish_probe() noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult verify_subscription() noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult pull_config() noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult pull_command() noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult freeze_snapshot(
        RuntimeOwnerExecutorCommand command) noexcept;
    [[nodiscard]] RuntimeOwnerPhysicalResult publish_telemetry(
        RuntimeOwnerExecutorCommand command) noexcept;

    std::uint32_t config_commit_sequence_{0};
    std::uint32_t command_request_sequence_{0};
    std::uint32_t deferred_config_commit_sequence_{0};
    CommandAckCore command_core_{};
    std::uint8_t prepared_{0};
    std::uint8_t modem_initialized_{0};
    std::uint8_t subscription_ready_{0};
    std::uint8_t at_status_{1};
    std::uint8_t cpin_status_{1};
    std::uint8_t operator_number_{0};
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_DEVICE_BACKEND_HPP
