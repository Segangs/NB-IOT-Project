#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_TRANSMIT_INDICATOR_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_TRANSMIT_INDICATOR_HPP

#include "runtime_owner_executor_contract.hpp"

namespace boot_v2 {

[[nodiscard]] constexpr bool
runtime_owner_operation_uses_transmit_indicator(
    const RuntimeOwnerDeviceOperationKind kind) noexcept
{
    switch (kind) {
    case RuntimeOwnerDeviceOperationKind::PublishProbe:
    case RuntimeOwnerDeviceOperationKind::VerifySubscription:
    case RuntimeOwnerDeviceOperationKind::PullFollowupConfig:
    case RuntimeOwnerDeviceOperationKind::EnterRecovery:
    case RuntimeOwnerDeviceOperationKind::PublishTelemetry:
    case RuntimeOwnerDeviceOperationKind::PullConfig:
    case RuntimeOwnerDeviceOperationKind::PullCommand:
    case RuntimeOwnerDeviceOperationKind::PublishAdapterRemoved:
    case RuntimeOwnerDeviceOperationKind::PublishAdapterRestored:
        return true;

    case RuntimeOwnerDeviceOperationKind::Invalid:
    case RuntimeOwnerDeviceOperationKind::OpenTransport:
    case RuntimeOwnerDeviceOperationKind::ProbeAt:
    case RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot:
    case RuntimeOwnerDeviceOperationKind::EndBootOrchestration:
    case RuntimeOwnerDeviceOperationKind::RecordFault:
    case RuntimeOwnerDeviceOperationKind::RefreshRssi:
        return false;
    }
    return false;
}

class RuntimeOwnerTransmitIndicatorScope {
public:
    RuntimeOwnerTransmitIndicatorScope(
        volatile bool &indicator,
        const bool active) noexcept
        : indicator_(&indicator),
          previous_(indicator),
          active_(active)
    {
        if (active_) {
            *indicator_ = true;
        }
    }

    ~RuntimeOwnerTransmitIndicatorScope() noexcept
    {
        if (active_) {
            *indicator_ = previous_;
        }
    }

    RuntimeOwnerTransmitIndicatorScope(
        const RuntimeOwnerTransmitIndicatorScope &) = delete;
    RuntimeOwnerTransmitIndicatorScope &operator=(
        const RuntimeOwnerTransmitIndicatorScope &) = delete;

private:
    volatile bool *indicator_;
    bool previous_;
    bool active_;
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_TRANSMIT_INDICATOR_HPP
