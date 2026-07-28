#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_PRODUCER_FACADE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_PRODUCER_FACADE_HPP

#include <cstdint>

#include "runtime_owner_rtos.hpp"

namespace boot_v2 {

[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_boot_request_transport() noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_periodic_request_transport() noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_periodic_publish_telemetry(
    std::uint32_t sensor_id,
    std::uint32_t snapshot_revision) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_sensor_publish_telemetry(
    std::uint32_t sensor_id,
    std::uint32_t snapshot_revision) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_periodic_refresh_rssi() noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_periodic_pull_config() noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_periodic_pull_command() noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_power_publish_adapter_removed(
    std::uint32_t incident_id,
    std::uint32_t producer_sequence) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_power_publish_adapter_restored(
    std::uint32_t incident_id,
    std::uint32_t producer_sequence) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_power_button_request_shutdown(
    std::uint32_t producer_sequence,
    std::uint32_t incident_correlation_id) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_adapter_loss_request_shutdown(
    std::uint32_t producer_sequence,
    std::uint32_t incident_correlation_id) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_authenticated_request_reboot(
    std::uint32_t producer_sequence,
    std::uint32_t incident_correlation_id) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult
runtime_owner_authenticated_request_power_off(
    std::uint32_t producer_sequence,
    std::uint32_t incident_correlation_id) noexcept;

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_PRODUCER_FACADE_HPP
