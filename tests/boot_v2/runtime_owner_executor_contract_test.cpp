#include "runtime_owner_executor_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

using boot_v2::AdapterDispatch;
using boot_v2::AdapterDispatchKind;
using boot_v2::CompletionPolicy;
using boot_v2::LivenessAttemptToken;
using boot_v2::NormalIntent;
using boot_v2::NormalIntentKind;
using boot_v2::RuntimeOwnerDeviceOperationKind;
using boot_v2::RuntimeOwnerEffect;
using boot_v2::RuntimeOwnerEffectKind;
using boot_v2::RuntimeOwnerExecutorCommand;
using boot_v2::RuntimeOwnerExecutorMapResult;
using boot_v2::RuntimeOwnerFaultCode;
using boot_v2::map_runtime_owner_dispatch;

constexpr std::size_t kRuntimeOwnerEffectKindCount =
    static_cast<std::size_t>(RuntimeOwnerEffectKind::EnterRecovery) + 1;
constexpr std::size_t kNormalIntentKindCount =
    static_cast<std::size_t>(NormalIntentKind::PublishAdapterRestored) + 1;
constexpr std::size_t kRuntimeOwnerDeviceOperationKindCount =
    static_cast<std::size_t>(
        RuntimeOwnerDeviceOperationKind::PublishAdapterRestored) + 1;
constexpr std::size_t kCompletionPolicyCount =
    static_cast<std::size_t>(CompletionPolicy::NormalCompletion) + 1;

static_assert(static_cast<std::uint8_t>(RuntimeOwnerEffectKind::None) == 0);
static_assert(
    static_cast<std::uint8_t>(RuntimeOwnerEffectKind::EnterRecovery) == 9);
static_assert(kRuntimeOwnerEffectKindCount == 10);
static_assert(static_cast<std::uint8_t>(NormalIntentKind::Invalid) == 0);
static_assert(
    static_cast<std::uint8_t>(
        NormalIntentKind::PublishAdapterRestored) == 6);
static_assert(kNormalIntentKindCount == 7);
static_assert(
    static_cast<std::uint8_t>(RuntimeOwnerDeviceOperationKind::Invalid) == 0);
static_assert(
    static_cast<std::uint8_t>(
        RuntimeOwnerDeviceOperationKind::PublishAdapterRestored) == 15);
static_assert(kRuntimeOwnerDeviceOperationKindCount == 16);
static_assert(
    (kRuntimeOwnerEffectKindCount - 1) +
            (kNormalIntentKindCount - 1) +
            1 ==
        kRuntimeOwnerDeviceOperationKindCount);
static_assert(static_cast<std::uint8_t>(CompletionPolicy::Invalid) == 0);
static_assert(
    static_cast<std::uint8_t>(CompletionPolicy::NormalCompletion) == 3);
static_assert(kCompletionPolicyCount == 4);

int checks = 0;
int failures = 0;

void check(const bool condition, const char *const expression, const int line)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

constexpr bool tokens_equal(
    const LivenessAttemptToken left,
    const LivenessAttemptToken right) noexcept
{
    return left.mqtt_session_id == right.mqtt_session_id &&
           left.mqtt_generation == right.mqtt_generation &&
           left.config_apply_epoch == right.config_apply_epoch;
}

constexpr bool effects_equal(
    const RuntimeOwnerEffect left,
    const RuntimeOwnerEffect right) noexcept
{
    return left.kind == right.kind &&
           left.correlation_id == right.correlation_id &&
           tokens_equal(left.attempt, right.attempt) &&
           left.fault_code == right.fault_code;
}

constexpr bool intents_equal(
    const NormalIntent left,
    const NormalIntent right) noexcept
{
    return left.kind == right.kind && left.flags == right.flags &&
           left.value_deci_celsius == right.value_deci_celsius &&
           left.subject_id == right.subject_id &&
           left.snapshot_revision == right.snapshot_revision;
}

constexpr bool dispatches_equal(
    const AdapterDispatch left,
    const AdapterDispatch right) noexcept
{
    return left.kind == right.kind && left.reserved == right.reserved &&
           left.dispatch_sequence == right.dispatch_sequence &&
           left.enqueue_sequence == right.enqueue_sequence &&
           effects_equal(left.effect, right.effect) &&
           intents_equal(left.normal_intent, right.normal_intent);
}

constexpr bool is_invalid_command(
    const RuntimeOwnerExecutorCommand command) noexcept
{
    return command.kind == RuntimeOwnerDeviceOperationKind::Invalid &&
           command.completion_policy == CompletionPolicy::Invalid &&
           dispatches_equal(command.source, AdapterDispatch{});
}

struct EffectCase {
    RuntimeOwnerEffect effect{};
    RuntimeOwnerDeviceOperationKind expected_kind{
        RuntimeOwnerDeviceOperationKind::Invalid};
    CompletionPolicy expected_policy{CompletionPolicy::Invalid};
};

struct NormalCase {
    NormalIntent intent{};
    RuntimeOwnerDeviceOperationKind expected_kind{
        RuntimeOwnerDeviceOperationKind::Invalid};
};

void test_numeric_and_layout_contracts()
{
    CHECK(static_cast<std::uint8_t>(CompletionPolicy::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(CompletionPolicy::DeliveryOnly) == 1);
    CHECK(static_cast<std::uint8_t>(CompletionPolicy::TrustedReceipt) == 2);
    CHECK(static_cast<std::uint8_t>(CompletionPolicy::NormalCompletion) == 3);
    CHECK((std::is_same<
           std::underlying_type<CompletionPolicy>::type,
           std::uint8_t>::value));
    CHECK((std::is_standard_layout<RuntimeOwnerExecutorCommand>::value));
    CHECK((std::is_trivially_copyable<RuntimeOwnerExecutorCommand>::value));
    CHECK(!std::is_pointer<decltype(RuntimeOwnerExecutorCommand::kind)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerExecutorCommand::kind)>::value);
    CHECK(!std::is_pointer<decltype(RuntimeOwnerExecutorCommand::source)>::value);
    CHECK(!std::is_reference<decltype(RuntimeOwnerExecutorCommand::source)>::value);
    CHECK(!std::is_pointer<
          decltype(RuntimeOwnerExecutorCommand::completion_policy)>::value);
    CHECK(!std::is_reference<
          decltype(RuntimeOwnerExecutorCommand::completion_policy)>::value);
    CHECK(sizeof(RuntimeOwnerExecutorCommand) == 56);
    CHECK(alignof(RuntimeOwnerExecutorCommand) == 4);
    CHECK(is_invalid_command(RuntimeOwnerExecutorCommand{}));
}

void test_all_core_effects_map_exactly()
{
    const LivenessAttemptToken attempt{31, 7, 9};
    const std::array<EffectCase, kRuntimeOwnerEffectKindCount - 1> cases{{
        {{RuntimeOwnerEffectKind::StartTransportAttempt,
          0,
          {0, 6, 0},
          RuntimeOwnerFaultCode::None},
         RuntimeOwnerDeviceOperationKind::OpenTransport,
         CompletionPolicy::TrustedReceipt},
        {{RuntimeOwnerEffectKind::StartAtProbe,
          101,
          attempt,
          RuntimeOwnerFaultCode::None},
         RuntimeOwnerDeviceOperationKind::ProbeAt,
         CompletionPolicy::TrustedReceipt},
        {{RuntimeOwnerEffectKind::StartProbePublish,
          102,
          attempt,
          RuntimeOwnerFaultCode::None},
         RuntimeOwnerDeviceOperationKind::PublishProbe,
         CompletionPolicy::TrustedReceipt},
        {{RuntimeOwnerEffectKind::VerifySubscription,
          103,
          attempt,
          RuntimeOwnerFaultCode::None},
         RuntimeOwnerDeviceOperationKind::VerifySubscription,
         CompletionPolicy::TrustedReceipt},
        {{RuntimeOwnerEffectKind::PullFollowupConfig,
          104,
          attempt,
          RuntimeOwnerFaultCode::None},
         RuntimeOwnerDeviceOperationKind::PullFollowupConfig,
         CompletionPolicy::TrustedReceipt},
        {{RuntimeOwnerEffectKind::FreezeBootSnapshot,
          105,
          attempt,
          RuntimeOwnerFaultCode::None},
         RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot,
         CompletionPolicy::TrustedReceipt},
        {{RuntimeOwnerEffectKind::EndBootOrchestration,
          106,
          attempt,
          RuntimeOwnerFaultCode::None},
         RuntimeOwnerDeviceOperationKind::EndBootOrchestration,
         CompletionPolicy::DeliveryOnly},
        {{RuntimeOwnerEffectKind::RecordFault,
          0,
          {},
          RuntimeOwnerFaultCode::TransportFailure},
         RuntimeOwnerDeviceOperationKind::RecordFault,
         CompletionPolicy::DeliveryOnly},
        {{RuntimeOwnerEffectKind::EnterRecovery,
          107,
          attempt,
          RuntimeOwnerFaultCode::LivenessFailure},
         RuntimeOwnerDeviceOperationKind::EnterRecovery,
         CompletionPolicy::DeliveryOnly},
    }};
    static_assert(cases.size() + 1 == kRuntimeOwnerEffectKindCount);

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const AdapterDispatch dispatch{
            AdapterDispatchKind::CoreEffect,
            {},
            static_cast<std::uint32_t>(700 + index),
            0,
            cases[index].effect,
            {},
        };
        RuntimeOwnerExecutorCommand command{};
        CHECK(map_runtime_owner_dispatch(dispatch, command) ==
              RuntimeOwnerExecutorMapResult::Mapped);
        CHECK(command.kind == cases[index].expected_kind);
        CHECK(command.completion_policy == cases[index].expected_policy);
        CHECK(dispatches_equal(command.source, dispatch));
        CHECK(command.source.dispatch_sequence ==
              static_cast<std::uint32_t>(700 + index));
        CHECK(effects_equal(command.source.effect, cases[index].effect));
    }
}

void test_all_normal_intents_map_exactly()
{
    const std::array<NormalCase, kNormalIntentKindCount - 1> cases{{
        {{NormalIntentKind::PublishTelemetry, 0x01, -166, 42, 77},
         RuntimeOwnerDeviceOperationKind::PublishTelemetry},
        {{NormalIntentKind::RefreshRssi, 0, 0, 0, 0},
         RuntimeOwnerDeviceOperationKind::RefreshRssi},
        {{NormalIntentKind::PullConfig, 0, 0, 0, 0},
         RuntimeOwnerDeviceOperationKind::PullConfig},
        {{NormalIntentKind::PullCommand, 0, 0, 0, 0},
         RuntimeOwnerDeviceOperationKind::PullCommand},
        {{NormalIntentKind::PublishAdapterRemoved, 0, 0, 42, 1},
         RuntimeOwnerDeviceOperationKind::PublishAdapterRemoved},
        {{NormalIntentKind::PublishAdapterRestored, 0, 0, 42, 2},
         RuntimeOwnerDeviceOperationKind::PublishAdapterRestored},
    }};
    static_assert(cases.size() + 1 == kNormalIntentKindCount);

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const AdapterDispatch dispatch{
            AdapterDispatchKind::NormalIntent,
            {},
            static_cast<std::uint32_t>(900 + index),
            static_cast<std::uint32_t>(300 + index),
            {},
            cases[index].intent,
        };
        RuntimeOwnerExecutorCommand command{};
        CHECK(map_runtime_owner_dispatch(dispatch, command) ==
              RuntimeOwnerExecutorMapResult::Mapped);
        CHECK(command.kind == cases[index].expected_kind);
        CHECK(command.completion_policy == CompletionPolicy::NormalCompletion);
        CHECK(dispatches_equal(command.source, dispatch));
        CHECK(command.source.dispatch_sequence ==
              static_cast<std::uint32_t>(900 + index));
        CHECK(command.source.enqueue_sequence ==
              static_cast<std::uint32_t>(300 + index));
        CHECK(intents_equal(command.source.normal_intent, cases[index].intent));
    }
}

void expect_rejected(const AdapterDispatch dispatch)
{
    RuntimeOwnerExecutorCommand command{
        RuntimeOwnerDeviceOperationKind::PullCommand,
        {AdapterDispatchKind::NormalIntent,
         {},
         99,
         88,
         {},
         {NormalIntentKind::PullCommand, 0, 0, 0, 0}},
        CompletionPolicy::NormalCompletion,
    };
    CHECK(map_runtime_owner_dispatch(dispatch, command) ==
          RuntimeOwnerExecutorMapResult::RejectedInvalid);
    CHECK(is_invalid_command(command));
}

void test_malformed_and_unknown_dispatches_are_rejected()
{
    const LivenessAttemptToken attempt{31, 7, 9};
    const RuntimeOwnerEffect liveness{
        RuntimeOwnerEffectKind::StartAtProbe,
        101,
        attempt,
        RuntimeOwnerFaultCode::None,
    };
    const NormalIntent pull_command{
        NormalIntentKind::PullCommand, 0, 0, 0, 0};

    expect_rejected({});
    expect_rejected({static_cast<AdapterDispatchKind>(0xff), {}, 1, 0, {}, {}});
    expect_rejected({AdapterDispatchKind::CoreEffect, {1, 0, 0}, 1, 0, liveness, {}});
    expect_rejected({AdapterDispatchKind::CoreEffect, {}, 0, 0, liveness, {}});
    expect_rejected({AdapterDispatchKind::CoreEffect, {}, 1, 2, liveness, {}});
    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     {RuntimeOwnerEffectKind::None, 0, {}, RuntimeOwnerFaultCode::None},
                     {}});
    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     {static_cast<RuntimeOwnerEffectKind>(0xff),
                      101,
                      attempt,
                      RuntimeOwnerFaultCode::None},
                     {}});
    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     liveness,
                     pull_command});

    expect_rejected({AdapterDispatchKind::NormalIntent, {}, 1, 0, {}, pull_command});
    expect_rejected({AdapterDispatchKind::NormalIntent, {}, 0, 1, {}, pull_command});
    expect_rejected({AdapterDispatchKind::NormalIntent,
                     {},
                     1,
                     1,
                     liveness,
                     pull_command});
    expect_rejected({AdapterDispatchKind::NormalIntent,
                     {},
                     1,
                     1,
                     {},
                     {NormalIntentKind::Invalid, 0, 0, 0, 0}});
    expect_rejected({AdapterDispatchKind::NormalIntent,
                     {},
                     1,
                     1,
                     {},
                     {static_cast<NormalIntentKind>(0xff), 0, 0, 0, 0}});
    expect_rejected({AdapterDispatchKind::NormalIntent,
                     {},
                     1,
                     1,
                     {},
                     {NormalIntentKind::PullCommand, 1, 0, 0, 0}});
    expect_rejected({AdapterDispatchKind::NormalIntent,
                     {},
                     1,
                     1,
                     {},
                     {NormalIntentKind::PullCommand, 0, 1, 0, 0}});
    expect_rejected({AdapterDispatchKind::NormalIntent,
                     {},
                     1,
                     1,
                     {},
                     {NormalIntentKind::PublishTelemetry, 0, 0, 0, 0}});
    expect_rejected({AdapterDispatchKind::NormalIntent,
                     {},
                     1,
                     1,
                     {},
                     {NormalIntentKind::RefreshRssi, 0, 0, 1, 0}});

    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     {RuntimeOwnerEffectKind::StartTransportAttempt,
                      1,
                      {1, 2, 3},
                      RuntimeOwnerFaultCode::None},
                     {}});
    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     {RuntimeOwnerEffectKind::StartAtProbe,
                      0,
                      attempt,
                      RuntimeOwnerFaultCode::None},
                     {}});
    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     {RuntimeOwnerEffectKind::FreezeBootSnapshot,
                      101,
                      {31, 0, 9},
                      RuntimeOwnerFaultCode::None},
                     {}});
    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     {RuntimeOwnerEffectKind::RecordFault,
                      0,
                      {},
                      RuntimeOwnerFaultCode::None},
                     {}});
    expect_rejected({AdapterDispatchKind::CoreEffect,
                     {},
                     1,
                     0,
                     {RuntimeOwnerEffectKind::EnterRecovery,
                      0,
                      {},
                      static_cast<RuntimeOwnerFaultCode>(0xff)},
                     {}});
}

} // namespace

int main()
{
    test_numeric_and_layout_contracts();
    test_all_core_effects_map_exactly();
    test_all_normal_intents_map_exactly();
    test_malformed_and_unknown_dispatches_are_rejected();

    if (failures != 0) {
        std::printf(
            "runtime_owner_executor_contract_test: %d/%d checks failed\n",
            failures,
            checks);
        return 1;
    }
    std::printf(
        "runtime_owner_executor_contract_test: %d checks passed\n",
        checks);
    return 0;
}
