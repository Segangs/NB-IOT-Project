#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_PRODUCER_CONTRACT_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_PRODUCER_CONTRACT_HPP

#include <cstdint>
#include <type_traits>

#include "runtime_owner_rtos_drain_core.hpp"

namespace boot_v2 {

enum class RuntimeOwnerProducerKind : std::uint8_t {
    Invalid = 0,
    Boot = 1,
    Periodic = 2,
    PowerButton = 3,
    AdapterMonitor = 4,
    AuthenticatedCommand = 5,
    LocalDebug = 6,
};

enum class RuntimeOwnerProducerRequestKind : std::uint8_t {
    Invalid = 0,
    RequestTransportAttempt = 1,
    PublishTelemetry = 2,
    RefreshRssi = 3,
    PullConfig = 4,
    PullCommand = 5,
    RequestShutdown = 6,
    RawModemCommand = 7,
};

enum class RuntimeOwnerProducerAuthorizationResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedPermission = 1,
    Authorized = 2,
};

struct RuntimeOwnerProducerDecision {
    RuntimeOwnerProducerAuthorizationResult result{
        RuntimeOwnerProducerAuthorizationResult::RejectedInvalid};
    RuntimeOwnerRtosLane lane{RuntimeOwnerRtosLane::None};
    RuntimeOwnerUrgentSource urgent_source{
        RuntimeOwnerUrgentSource::Invalid};
    RuntimeOwnerControlKind control_kind{
        RuntimeOwnerControlKind::Invalid};
    NormalIntentKind normal_kind{NormalIntentKind::Invalid};
    std::uint8_t reserved[3]{};
};

namespace runtime_owner_producer_contract_detail {

template <typename Enum>
constexpr bool has_uint8_underlying_type =
    std::is_same<typename std::underlying_type<Enum>::type,
                 std::uint8_t>::value;

template <typename... Fields>
constexpr bool has_only_nonowning_value_fields =
    ((!std::is_pointer<Fields>::value &&
      !std::is_reference<Fields>::value &&
      std::is_trivially_copyable<Fields>::value) && ...);

static_assert(has_uint8_underlying_type<RuntimeOwnerProducerKind>);
static_assert(has_uint8_underlying_type<RuntimeOwnerProducerRequestKind>);
static_assert(
    has_uint8_underlying_type<RuntimeOwnerProducerAuthorizationResult>);
static_assert(sizeof(RuntimeOwnerProducerDecision) == 8);
static_assert(alignof(RuntimeOwnerProducerDecision) == 1);
static_assert(std::is_standard_layout<RuntimeOwnerProducerDecision>::value);
static_assert(
    std::is_trivially_copyable<RuntimeOwnerProducerDecision>::value);
static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerProducerDecision::result),
              decltype(RuntimeOwnerProducerDecision::lane),
              decltype(RuntimeOwnerProducerDecision::urgent_source),
              decltype(RuntimeOwnerProducerDecision::control_kind),
              decltype(RuntimeOwnerProducerDecision::normal_kind),
              decltype(RuntimeOwnerProducerDecision::reserved)>);

}  // namespace runtime_owner_producer_contract_detail

[[nodiscard]] constexpr RuntimeOwnerProducerDecision
runtime_owner_authorize_producer_request(
    const RuntimeOwnerProducerKind producer,
    const RuntimeOwnerProducerRequestKind request) noexcept
{
    constexpr std::uint8_t kFirstProducer =
        static_cast<std::uint8_t>(RuntimeOwnerProducerKind::Boot);
    constexpr std::uint8_t kLastProducer =
        static_cast<std::uint8_t>(RuntimeOwnerProducerKind::LocalDebug);
    constexpr std::uint8_t kFirstRequest = static_cast<std::uint8_t>(
        RuntimeOwnerProducerRequestKind::RequestTransportAttempt);
    constexpr std::uint8_t kLastRequest = static_cast<std::uint8_t>(
        RuntimeOwnerProducerRequestKind::RawModemCommand);
    const std::uint8_t producer_value =
        static_cast<std::uint8_t>(producer);
    const std::uint8_t request_value =
        static_cast<std::uint8_t>(request);
    if (producer_value < kFirstProducer || producer_value > kLastProducer ||
        request_value < kFirstRequest || request_value > kLastRequest) {
        return {};
    }

    if ((producer == RuntimeOwnerProducerKind::Boot ||
         producer == RuntimeOwnerProducerKind::Periodic) &&
        request ==
            RuntimeOwnerProducerRequestKind::RequestTransportAttempt) {
        return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                RuntimeOwnerRtosLane::Control,
                RuntimeOwnerUrgentSource::Invalid,
                RuntimeOwnerControlKind::RequestTransportAttempt,
                NormalIntentKind::Invalid,
                {}};
    }
    if (producer == RuntimeOwnerProducerKind::Periodic) {
        if (request ==
            RuntimeOwnerProducerRequestKind::PublishTelemetry) {
            return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                    RuntimeOwnerRtosLane::Normal,
                    RuntimeOwnerUrgentSource::Invalid,
                    RuntimeOwnerControlKind::Invalid,
                    NormalIntentKind::PublishTelemetry,
                    {}};
        }
        if (request == RuntimeOwnerProducerRequestKind::RefreshRssi) {
            return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                    RuntimeOwnerRtosLane::Normal,
                    RuntimeOwnerUrgentSource::Invalid,
                    RuntimeOwnerControlKind::Invalid,
                    NormalIntentKind::RefreshRssi,
                    {}};
        }
        if (request == RuntimeOwnerProducerRequestKind::PullConfig) {
            return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                    RuntimeOwnerRtosLane::Normal,
                    RuntimeOwnerUrgentSource::Invalid,
                    RuntimeOwnerControlKind::Invalid,
                    NormalIntentKind::PullConfig,
                    {}};
        }
        if (request == RuntimeOwnerProducerRequestKind::PullCommand) {
            return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                    RuntimeOwnerRtosLane::Normal,
                    RuntimeOwnerUrgentSource::Invalid,
                    RuntimeOwnerControlKind::Invalid,
                    NormalIntentKind::PullCommand,
                    {}};
        }
    }
    if (request == RuntimeOwnerProducerRequestKind::RequestShutdown) {
        if (producer == RuntimeOwnerProducerKind::PowerButton) {
            return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                    RuntimeOwnerRtosLane::Urgent,
                    RuntimeOwnerUrgentSource::PowerButton,
                    RuntimeOwnerControlKind::Invalid,
                    NormalIntentKind::Invalid,
                    {}};
        }
        if (producer == RuntimeOwnerProducerKind::AdapterMonitor) {
            return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                    RuntimeOwnerRtosLane::Urgent,
                    RuntimeOwnerUrgentSource::AdapterLossCommitted,
                    RuntimeOwnerControlKind::Invalid,
                    NormalIntentKind::Invalid,
                    {}};
        }
        if (producer == RuntimeOwnerProducerKind::AuthenticatedCommand) {
            return {RuntimeOwnerProducerAuthorizationResult::Authorized,
                    RuntimeOwnerRtosLane::Urgent,
                    RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand,
                    RuntimeOwnerControlKind::Invalid,
                    NormalIntentKind::Invalid,
                    {}};
        }
    }

    RuntimeOwnerProducerDecision rejected{};
    rejected.result =
        RuntimeOwnerProducerAuthorizationResult::RejectedPermission;
    return rejected;
}

}  // namespace boot_v2

#endif  // NB_IOT_BOOT_V2_RUNTIME_OWNER_PRODUCER_CONTRACT_HPP
