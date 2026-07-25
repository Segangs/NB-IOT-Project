#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_EXECUTOR_CONTRACT_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_EXECUTOR_CONTRACT_HPP

#include <cstdint>
#include <type_traits>

#include "runtime_owner_adapter_core.hpp"

namespace boot_v2 {

enum class CompletionPolicy : std::uint8_t {
    Invalid = 0,
    DeliveryOnly = 1,
    TrustedReceipt = 2,
    NormalCompletion = 3,
};

enum class RuntimeOwnerDeviceOperationKind : std::uint8_t {
    Invalid = 0,
    OpenTransport = 1,
    ProbeAt = 2,
    PublishProbe = 3,
    VerifySubscription = 4,
    PullFollowupConfig = 5,
    FreezeBootSnapshot = 6,
    EndBootOrchestration = 7,
    RecordFault = 8,
    EnterRecovery = 9,
    PublishTelemetry = 10,
    RefreshRssi = 11,
    PullConfig = 12,
    PullCommand = 13,
};

enum class RuntimeOwnerExecutorMapResult : std::uint8_t {
    RejectedInvalid = 0,
    Mapped = 1,
};

struct RuntimeOwnerExecutorCommand {
    RuntimeOwnerDeviceOperationKind kind{
        RuntimeOwnerDeviceOperationKind::Invalid};
    AdapterDispatch source{};
    CompletionPolicy completion_policy{CompletionPolicy::Invalid};
};

[[nodiscard]] RuntimeOwnerExecutorMapResult map_runtime_owner_dispatch(
    AdapterDispatch dispatch,
    RuntimeOwnerExecutorCommand &command) noexcept;

namespace runtime_owner_executor_contract_detail {

template <typename Enum>
constexpr bool has_uint8_underlying_type =
    std::is_same<typename std::underlying_type<Enum>::type,
                 std::uint8_t>::value;

template <typename... Fields>
constexpr bool has_only_nonowning_value_fields =
    ((!std::is_pointer<Fields>::value &&
      !std::is_reference<Fields>::value &&
      std::is_trivially_copyable<Fields>::value) && ...);

static_assert(has_uint8_underlying_type<CompletionPolicy>);
static_assert(has_uint8_underlying_type<RuntimeOwnerDeviceOperationKind>);
static_assert(has_uint8_underlying_type<RuntimeOwnerExecutorMapResult>);

static_assert(sizeof(RuntimeOwnerExecutorCommand) == 56);
static_assert(alignof(RuntimeOwnerExecutorCommand) == 4);
static_assert(std::is_standard_layout<RuntimeOwnerExecutorCommand>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerExecutorCommand>::value);
static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerExecutorCommand::kind),
              decltype(RuntimeOwnerExecutorCommand::source),
              decltype(RuntimeOwnerExecutorCommand::completion_policy)>);

} // namespace runtime_owner_executor_contract_detail

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_EXECUTOR_CONTRACT_HPP
