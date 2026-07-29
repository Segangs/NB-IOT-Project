#include "runtime_owner_task_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace {

std::size_t g_check_count = 0;
std::size_t g_failure_count = 0;
std::size_t g_allocation_count = 0;
std::size_t g_deallocation_count = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++g_check_count;                                                       \
        if (!(condition)) {                                                    \
            ++g_failure_count;                                                 \
            std::fprintf(                                                      \
                stderr,                                                        \
                "CHECK failed at %s:%d: %s\n",                                \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #condition);                                                   \
        }                                                                      \
    } while (false)

template <typename... Fields>
constexpr bool has_only_nonowning_value_fields =
    ((!std::is_pointer<Fields>::value &&
      !std::is_reference<Fields>::value &&
      std::is_trivially_copyable<Fields>::value) && ...);

using namespace boot_v2;

constexpr bool is_ascii_whitespace(const char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f' || value == '\v';
}

std::string normalized_task_core_source()
{
    const std::string test_source_path{__FILE__};
    const std::size_t filename_separator =
        test_source_path.find_last_of("/\\");
    const std::string task_core_source_path =
        filename_separator == std::string::npos
            ? std::string{}
            : test_source_path.substr(0, filename_separator + 1) +
                  "../../src/boot_v2/runtime_owner_task_core.cpp";
    std::ifstream source{task_core_source_path};
    CHECK(source.is_open());

    std::string normalized{};
    char value = '\0';
    while (source.get(value)) {
        if (!is_ascii_whitespace(value)) {
            normalized.push_back(value);
        }
    }
    return normalized;
}

std::string normalized_task_core_header()
{
    const std::string test_source_path{__FILE__};
    const std::size_t filename_separator =
        test_source_path.find_last_of("/\\");
    const std::string task_core_header_path =
        filename_separator == std::string::npos
            ? std::string{}
            : test_source_path.substr(0, filename_separator + 1) +
                  "../../src/boot_v2/runtime_owner_task_core.hpp";
    std::ifstream source{task_core_header_path};
    CHECK(source.is_open());

    std::string normalized{};
    char value = '\0';
    while (source.get(value)) {
        if (!is_ascii_whitespace(value)) {
            normalized.push_back(value);
        }
    }
    return normalized;
}

std::size_t count_nonoverlapping_occurrences(
    const std::string &text,
    const std::string &needle) noexcept
{
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void test_contract_numeric_values_and_defaults()
{
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerIngressResult::RejectedNotStarted) == 0);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerIngressResult::RejectedInactive) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerIngressResult::RejectedFull) ==
          2);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerIngressResult::AcceptedForDelivery) == 3);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerIngressResult::RejectedInvalid) == 4);

    CHECK(static_cast<std::uint8_t>(RuntimeOwnerStartResult::Failed) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerStartResult::Started) == 1);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerStartResult::AlreadyStarted) == 2);

    CHECK(static_cast<std::uint8_t>(RuntimeOwnerControlKind::Invalid) == 0);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerControlKind::RequestTransportAttempt) == 1);

    CHECK(static_cast<std::uint8_t>(RuntimeOwnerTaskState::Dormant) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerTaskState::Active) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerTaskState::Terminal) == 2);

    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerTaskCycleDisposition::RejectedInactive) == 0);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerTaskCycleDisposition::RejectedTerminal) == 1);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerTaskCycleDisposition::Processed) == 2);

    CHECK(static_cast<std::uint8_t>(RuntimeOwnerTaskWorkKind::None) == 0);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerTaskWorkKind::RequestTransportAttempt) == 1);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerTaskWorkKind::NormalIntent) == 2);

    const RuntimeOwnerControlMessage control{};
    CHECK(control.kind == RuntimeOwnerControlKind::Invalid);
    CHECK(control.reserved[0] == 0);
    CHECK(control.reserved[1] == 0);
    CHECK(control.reserved[2] == 0);

    const RuntimeOwnerRtosStatus status{};
    CHECK(status.wake_count == 0);
    CHECK(status.rejected_inactive_count == 0);
    CHECK(status.rejected_full_count == 0);
    CHECK(status.started == 0);
    CHECK(status.ingress_enabled == 0);
    CHECK(status.start_failed == 0);
    CHECK(status.reserved == 0);

    const RuntimeOwnerTaskCycleInput input{};
    CHECK(input.reserved_source_only == 0);
    CHECK(input.transport_pending == 0);
    CHECK(input.normal_pending == 0);
    CHECK(input.reserved == 0);
    CHECK(input.normal.kind == NormalIntentKind::Invalid);
    CHECK(input.normal.flags == 0);
    CHECK(input.normal.value_deci_celsius == 0);
    CHECK(input.normal.subject_id == 0);
    CHECK(input.normal.snapshot_revision == 0);

    const RuntimeOwnerTaskCycleResult result{};
    CHECK(result.disposition ==
          RuntimeOwnerTaskCycleDisposition::RejectedInactive);
    CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::None);
    CHECK(result.transport_result ==
          OwnerRequestResult::RejectedNotAllowed);
    CHECK(result.normal_result == NormalSubmitResult::RejectedNotReady);
    CHECK(result.urgent_recheck_required == 0);
    CHECK(result.dispatch_pending == 0);
    CHECK(result.reserved == 0);
    CHECK(result.step_result.action == AdapterStepAction::Invalid);
    CHECK(result.step_result.core_disposition ==
          RuntimeOwnerDisposition::Rejected);
    CHECK(result.step_result.phase_before == RuntimeOwnerPhase::ColdStart);
    CHECK(result.step_result.phase_after == RuntimeOwnerPhase::ColdStart);
    CHECK(result.step_result.consumed_ingress_sequence == 0);
    CHECK(result.step_result.consumed_enqueue_sequence == 0);
    CHECK(result.step_result.prepared_dispatch_sequence == 0);
}

void test_contract_layout_traits_and_api()
{
    static_assert(std::is_same<
                  typename std::underlying_type<
                      RuntimeOwnerIngressResult>::type,
                  std::uint8_t>::value);
    static_assert(std::is_same<
                  typename std::underlying_type<
                      RuntimeOwnerStartResult>::type,
                  std::uint8_t>::value);
    static_assert(std::is_same<
                  typename std::underlying_type<
                      RuntimeOwnerControlKind>::type,
                  std::uint8_t>::value);
    static_assert(std::is_same<
                  typename std::underlying_type<RuntimeOwnerTaskState>::type,
                  std::uint8_t>::value);
    static_assert(std::is_same<
                  typename std::underlying_type<
                      RuntimeOwnerTaskCycleDisposition>::type,
                  std::uint8_t>::value);
    static_assert(std::is_same<
                  typename std::underlying_type<
                      RuntimeOwnerTaskWorkKind>::type,
                  std::uint8_t>::value);

    static_assert(sizeof(RuntimeOwnerControlMessage) == 4);
    static_assert(alignof(RuntimeOwnerControlMessage) == 1);
    static_assert(sizeof(RuntimeOwnerRtosStatus) == 16);
    static_assert(alignof(RuntimeOwnerRtosStatus) == 4);
    static_assert(sizeof(RuntimeOwnerTaskCycleInput) == 16);
    static_assert(alignof(RuntimeOwnerTaskCycleInput) == 4);
    static_assert(sizeof(RuntimeOwnerTaskCycleResult) == 24);
    static_assert(alignof(RuntimeOwnerTaskCycleResult) == 4);

    static_assert(std::is_standard_layout<RuntimeOwnerControlMessage>::value);
    static_assert(
        std::is_trivially_copyable<RuntimeOwnerControlMessage>::value);
    static_assert(std::is_standard_layout<RuntimeOwnerRtosStatus>::value);
    static_assert(std::is_trivially_copyable<RuntimeOwnerRtosStatus>::value);
    static_assert(std::is_standard_layout<RuntimeOwnerTaskCycleInput>::value);
    static_assert(
        std::is_trivially_copyable<RuntimeOwnerTaskCycleInput>::value);
    static_assert(std::is_standard_layout<RuntimeOwnerTaskCycleResult>::value);
    static_assert(
        std::is_trivially_copyable<RuntimeOwnerTaskCycleResult>::value);

    static_assert(has_only_nonowning_value_fields<
                  decltype(RuntimeOwnerControlMessage::kind),
                  decltype(RuntimeOwnerControlMessage::reserved)>);
    static_assert(has_only_nonowning_value_fields<
                  decltype(RuntimeOwnerRtosStatus::wake_count),
                  decltype(RuntimeOwnerRtosStatus::rejected_inactive_count),
                  decltype(RuntimeOwnerRtosStatus::rejected_full_count),
                  decltype(RuntimeOwnerRtosStatus::started),
                  decltype(RuntimeOwnerRtosStatus::ingress_enabled),
                  decltype(RuntimeOwnerRtosStatus::start_failed),
                  decltype(RuntimeOwnerRtosStatus::reserved)>);
    static_assert(has_only_nonowning_value_fields<
                  decltype(RuntimeOwnerTaskCycleInput::reserved_source_only),
                  decltype(RuntimeOwnerTaskCycleInput::transport_pending),
                  decltype(RuntimeOwnerTaskCycleInput::normal_pending),
                  decltype(RuntimeOwnerTaskCycleInput::reserved),
                  decltype(RuntimeOwnerTaskCycleInput::normal)>);
    static_assert(has_only_nonowning_value_fields<
                  decltype(RuntimeOwnerTaskCycleResult::disposition),
                  decltype(RuntimeOwnerTaskCycleResult::selected_work),
                  decltype(RuntimeOwnerTaskCycleResult::transport_result),
                  decltype(RuntimeOwnerTaskCycleResult::normal_result),
                  decltype(RuntimeOwnerTaskCycleResult::urgent_recheck_required),
                  decltype(RuntimeOwnerTaskCycleResult::dispatch_pending),
                  decltype(RuntimeOwnerTaskCycleResult::reserved),
                  decltype(RuntimeOwnerTaskCycleResult::step_result)>);

    static_assert(std::is_default_constructible<RuntimeOwnerTaskCore>::value);
    static_assert(std::is_nothrow_default_constructible<
                  RuntimeOwnerTaskCore>::value);
    static_assert(std::is_nothrow_destructible<RuntimeOwnerTaskCore>::value);
    static_assert(!std::is_copy_constructible<RuntimeOwnerTaskCore>::value);
    static_assert(!std::is_copy_assignable<RuntimeOwnerTaskCore>::value);
    static_assert(!std::is_move_constructible<RuntimeOwnerTaskCore>::value);
    static_assert(!std::is_move_assignable<RuntimeOwnerTaskCore>::value);

    using ProcessCycleSignature = RuntimeOwnerTaskCycleResult (*)(
        RuntimeOwnerTaskCore &,
        RuntimeOwnerTaskCycleInput) noexcept;
    static_assert(std::is_same<
                  decltype(&RuntimeOwnerTaskCoreTestPeer::process_cycle),
                  ProcessCycleSignature>::value);
    static_assert(noexcept(RuntimeOwnerTaskCoreTestPeer::process_cycle(
        std::declval<RuntimeOwnerTaskCore &>(),
        RuntimeOwnerTaskCycleInput{})));

    CHECK(sizeof(RuntimeOwnerControlMessage) == 4);
    CHECK(alignof(RuntimeOwnerControlMessage) == 1);
    CHECK(sizeof(RuntimeOwnerRtosStatus) == 16);
    CHECK(alignof(RuntimeOwnerRtosStatus) == 4);
    CHECK(sizeof(RuntimeOwnerTaskCycleInput) == 16);
    CHECK(alignof(RuntimeOwnerTaskCycleInput) == 4);
    CHECK(sizeof(RuntimeOwnerTaskCycleResult) == 24);
    CHECK(alignof(RuntimeOwnerTaskCycleResult) == 4);
}

void test_ingress_gate_precedence_truth_table()
{
    static_assert(std::is_same<
                  typename std::underlying_type<
                      RuntimeOwnerIngressGateResult>::type,
                  std::uint8_t>::value);
    static_assert(noexcept(runtime_owner_ingress_gate(false, false)));

    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerIngressGateResult::RejectedNotStarted) == 0);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerIngressGateResult::RejectedInactive) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerIngressGateResult::Open) == 2);
    CHECK(runtime_owner_ingress_gate(false, false) ==
          RuntimeOwnerIngressGateResult::RejectedNotStarted);
    CHECK(runtime_owner_ingress_gate(false, true) ==
          RuntimeOwnerIngressGateResult::RejectedNotStarted);
    CHECK(runtime_owner_ingress_gate(true, false) ==
          RuntimeOwnerIngressGateResult::RejectedInactive);
    CHECK(runtime_owner_ingress_gate(true, true) ==
          RuntimeOwnerIngressGateResult::Open);
}

void test_default_task_state_is_dormant()
{
    RuntimeOwnerTaskCore core{};
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(core) ==
          RuntimeOwnerTaskState::Dormant);
}

void check_safe_unprocessed_result(
    const RuntimeOwnerTaskCycleResult result,
    const RuntimeOwnerTaskCycleDisposition expected_disposition)
{
    CHECK(result.disposition == expected_disposition);
    CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::None);
    CHECK(result.transport_result ==
          OwnerRequestResult::RejectedNotAllowed);
    CHECK(result.normal_result == NormalSubmitResult::RejectedNotReady);
    CHECK(result.urgent_recheck_required == 0);
    CHECK(result.dispatch_pending == 0);
    CHECK(result.reserved == 0);
    CHECK(result.step_result.action == AdapterStepAction::Invalid);
    CHECK(result.step_result.core_disposition ==
          RuntimeOwnerDisposition::Rejected);
    CHECK(result.step_result.phase_before == RuntimeOwnerPhase::ColdStart);
    CHECK(result.step_result.phase_after == RuntimeOwnerPhase::ColdStart);
    CHECK(result.step_result.consumed_ingress_sequence == 0);
    CHECK(result.step_result.consumed_enqueue_sequence == 0);
    CHECK(result.step_result.prepared_dispatch_sequence == 0);
}

constexpr bool normal_intents_equal(
    const NormalIntent left,
    const NormalIntent right) noexcept
{
    return left.kind == right.kind &&
           left.flags == right.flags &&
           left.value_deci_celsius == right.value_deci_celsius &&
           left.subject_id == right.subject_id &&
           left.snapshot_revision == right.snapshot_revision;
}

constexpr bool runtime_owner_effects_equal(
    const RuntimeOwnerEffect left,
    const RuntimeOwnerEffect right) noexcept
{
    return left.kind == right.kind &&
           left.correlation_id == right.correlation_id &&
           left.attempt == right.attempt &&
           left.fault_code == right.fault_code;
}

constexpr bool adapter_dispatches_equal(
    const AdapterDispatch left,
    const AdapterDispatch right) noexcept
{
    return left.kind == right.kind &&
           left.reserved == right.reserved &&
           left.dispatch_sequence == right.dispatch_sequence &&
           left.enqueue_sequence == right.enqueue_sequence &&
           runtime_owner_effects_equal(left.effect, right.effect) &&
           normal_intents_equal(left.normal_intent, right.normal_intent);
}

constexpr bool runtime_owner_views_equal(
    const RuntimeOwnerView left,
    const RuntimeOwnerView right) noexcept
{
    return left.phase == right.phase &&
           left.mqtt_session_id == right.mqtt_session_id &&
           left.mqtt_generation == right.mqtt_generation &&
           left.mqtt_generation_counter == right.mqtt_generation_counter &&
           left.config_apply_epoch_counter ==
               right.config_apply_epoch_counter &&
           left.last_config_commit_sequence ==
               right.last_config_commit_sequence &&
           left.last_correlation_id == right.last_correlation_id &&
           left.active_attempt == right.active_attempt &&
           left.boot_orchestration_ended ==
               right.boot_orchestration_ended &&
           left.last_fault == right.last_fault;
}

constexpr bool critical_ledgers_equal(
    const AdapterCriticalLedger left,
    const AdapterCriticalLedger right) noexcept
{
    return left.first_reason == right.first_reason &&
           left.last_reason == right.last_reason &&
           left.reserved == right.reserved &&
           left.reason_mask == right.reason_mask &&
           left.first_ingress_sequence == right.first_ingress_sequence &&
           left.last_ingress_sequence == right.last_ingress_sequence &&
           left.first_diagnostic_code == right.first_diagnostic_code &&
           left.last_diagnostic_code == right.last_diagnostic_code &&
           left.occurrence_count == right.occurrence_count;
}

constexpr bool adapter_views_equal(
    const RuntimeOwnerAdapterView &left,
    const RuntimeOwnerAdapterView &right) noexcept
{
    return runtime_owner_views_equal(left.core, right.core) &&
           adapter_dispatches_equal(
               left.current_dispatch, right.current_dispatch) &&
           adapter_dispatches_equal(
               left.physical_inflight, right.physical_inflight) &&
           critical_ledgers_equal(left.critical, right.critical) &&
           left.last_normal_enqueue_sequence ==
               right.last_normal_enqueue_sequence &&
           left.last_trusted_ingress_sequence ==
               right.last_trusted_ingress_sequence &&
           left.last_dispatch_sequence == right.last_dispatch_sequence &&
           left.last_ack_dispatch_sequence ==
               right.last_ack_dispatch_sequence &&
           left.last_trusted_diagnostic_ingress_sequence ==
               right.last_trusted_diagnostic_ingress_sequence &&
           left.last_trusted_diagnostic_code ==
               right.last_trusted_diagnostic_code &&
           left.normal_coalesced_count == right.normal_coalesced_count &&
           left.normal_rejected_full_count ==
               right.normal_rejected_full_count &&
           left.normal_cancelled_count == right.normal_cancelled_count &&
           left.trusted_rejected_full_count ==
               right.trusted_rejected_full_count &&
           left.trusted_protocol_violation_count ==
               right.trusted_protocol_violation_count &&
           left.trusted_stale_count == right.trusted_stale_count &&
           left.trusted_duplicate_count == right.trusted_duplicate_count &&
           left.trusted_cancelled_count == right.trusted_cancelled_count &&
           left.effect_cancelled_count == right.effect_cancelled_count &&
           left.dispatch_rejected_ack_count ==
               right.dispatch_rejected_ack_count &&
           left.normal_completion_stale_count ==
               right.normal_completion_stale_count &&
           left.normal_depth == right.normal_depth &&
           left.normal_high_water == right.normal_high_water &&
           left.trusted_depth == right.trusted_depth &&
           left.trusted_high_water == right.trusted_high_water &&
           left.pending_effect_count == right.pending_effect_count &&
           left.transport_request_pending ==
               right.transport_request_pending &&
           left.shutdown_pending == right.shutdown_pending &&
           left.shutdown_terminal_override_latched ==
               right.shutdown_terminal_override_latched &&
           left.critical_pending == right.critical_pending &&
           left.boot_end_released == right.boot_end_released &&
           left.core_fail_closed_latched ==
               right.core_fail_closed_latched &&
           left.core_adapter_fatal_latched ==
               right.core_adapter_fatal_latched &&
           left.sequence_fatal_latched == right.sequence_fatal_latched &&
           left.dispatch_fatal_latched == right.dispatch_fatal_latched &&
           left.safety_delivery_blocked ==
               right.safety_delivery_blocked &&
           left.physical_inflight_cancel_pending ==
               right.physical_inflight_cancel_pending;
}

constexpr bool adapter_step_results_equal(
    const AdapterStepResult left,
    const AdapterStepResult right) noexcept
{
    return left.action == right.action &&
           left.core_disposition == right.core_disposition &&
           left.phase_before == right.phase_before &&
           left.phase_after == right.phase_after &&
           left.consumed_ingress_sequence ==
               right.consumed_ingress_sequence &&
           left.consumed_enqueue_sequence ==
               right.consumed_enqueue_sequence &&
           left.prepared_dispatch_sequence ==
               right.prepared_dispatch_sequence;
}

std::uint8_t dispatch_present(
    const RuntimeOwnerAdapterCore &adapter) noexcept
{
    return adapter.peek_dispatch().kind == AdapterDispatchKind::None ? 0 : 1;
}

void test_dormant_empty_cycle_is_rejected_without_adapter_mutation()
{
    RuntimeOwnerTaskCore core{};
    const RuntimeOwnerAdapterView before =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {});

    const RuntimeOwnerAdapterView after =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    check_safe_unprocessed_result(
        result,
        RuntimeOwnerTaskCycleDisposition::RejectedInactive);
    CHECK(adapter_views_equal(before, after));
}

void test_dormant_normal_cycle_is_rejected_without_adapter_mutation()
{
    RuntimeOwnerTaskCore core{};
    const RuntimeOwnerAdapterView before =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    const RuntimeOwnerTaskCycleInput input{
        0,
        0,
        1,
        0,
        {
            NormalIntentKind::PublishTelemetry,
            0x01,
            0,
            41,
            9,
        },
    };

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    const RuntimeOwnerAdapterView after =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    check_safe_unprocessed_result(
        result,
        RuntimeOwnerTaskCycleDisposition::RejectedInactive);
    CHECK(adapter_views_equal(before, after));
}

void test_dormant_transport_cycle_is_rejected_without_adapter_mutation()
{
    RuntimeOwnerTaskCore core{};
    const RuntimeOwnerAdapterView before =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    const RuntimeOwnerTaskCycleInput input{
        0,
        1,
        0,
        0,
        {},
    };

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    const RuntimeOwnerAdapterView after =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    check_safe_unprocessed_result(
        result,
        RuntimeOwnerTaskCycleDisposition::RejectedInactive);
    CHECK(adapter_views_equal(before, after));
}

void test_dormant_shutdown_cycle_is_rejected_without_adapter_mutation()
{
    RuntimeOwnerTaskCore core{};
    const RuntimeOwnerAdapterView before =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    const RuntimeOwnerTaskCycleInput input{
        1,
        0,
        0,
        0,
        {},
    };

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    const RuntimeOwnerAdapterView after =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    check_safe_unprocessed_result(
        result,
        RuntimeOwnerTaskCycleDisposition::RejectedInactive);
    CHECK(adapter_views_equal(before, after));
}

void test_dormant_malformed_normal_is_rejected_without_adapter_mutation()
{
    RuntimeOwnerTaskCore core{};
    const RuntimeOwnerAdapterView before =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    const RuntimeOwnerTaskCycleInput input{
        0,
        0,
        1,
        0,
        {
            NormalIntentKind::Invalid,
            255,
            -1,
            UINT32_MAX,
            UINT32_MAX,
        },
    };

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    const RuntimeOwnerAdapterView after =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    check_safe_unprocessed_result(
        result,
        RuntimeOwnerTaskCycleDisposition::RejectedInactive);
    CHECK(adapter_views_equal(before, after));
}

void test_active_source_scoped_shutdown_blocks_control_and_normal()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    auto shutdown =
        RuntimeOwnerTaskCoreTestPeer::power_button_shutdown_port(core);
    CHECK(shutdown.request(1, 51) ==
          RuntimeOwnerShutdownRequestResult::Accepted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(core));
    const RuntimeOwnerTaskCycleInput input{
        0,
        1,
        1,
        0,
        {
            NormalIntentKind::PublishTelemetry,
            0x01,
            0,
            51,
            11,
        },
    };

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    CHECK(result.disposition == RuntimeOwnerTaskCycleDisposition::Processed);
    CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::None);
    CHECK(result.transport_result ==
          OwnerRequestResult::RejectedNotAllowed);
    CHECK(result.normal_result == NormalSubmitResult::RejectedNotReady);
    CHECK(result.urgent_recheck_required == 1);
    CHECK(result.reserved == 0);
    CHECK(result.step_result.action ==
          AdapterStepAction::CoreTransitionApplied);
    CHECK(result.step_result.phase_after ==
          RuntimeOwnerPhase::ShutdownCommitted);
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(core) ==
          RuntimeOwnerTaskState::Terminal);
    CHECK(RuntimeOwnerTaskCoreTestPeer::shutdown_invariant_holds(core));
    CHECK(core.redacted_status().shutdown_latched == 1);
}

void test_active_transport_and_normal_selects_transport_only_and_translates()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerAdapterCore reference{};
    const RuntimeOwnerTaskCycleInput input{
        0,
        1,
        1,
        0,
        {
            NormalIntentKind::PublishTelemetry,
            0x01,
            0,
            61,
            13,
        },
    };

    const OwnerRequestResult expected_transport =
        reference.request_transport_attempt();
    const AdapterStepResult expected_step = reference.step();
    const std::uint8_t expected_dispatch = dispatch_present(reference);
    const RuntimeOwnerAdapterView expected_view = reference.view();

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    CHECK(result.disposition == RuntimeOwnerTaskCycleDisposition::Processed);
    CHECK(result.selected_work ==
          RuntimeOwnerTaskWorkKind::RequestTransportAttempt);
    CHECK(result.transport_result == expected_transport);
    CHECK(result.normal_result == NormalSubmitResult::RejectedNotReady);
    CHECK(result.urgent_recheck_required == 1);
    CHECK(result.dispatch_pending == expected_dispatch);
    CHECK(result.reserved == 0);
    CHECK(adapter_step_results_equal(result.step_result, expected_step));
    CHECK(adapter_views_equal(
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core), expected_view));

    const bool expected_terminal =
        expected_step.action == AdapterStepAction::Terminal ||
        expected_view.core.phase == RuntimeOwnerPhase::ShutdownCommitted;
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(core) ==
          (expected_terminal ? RuntimeOwnerTaskState::Terminal
                             : RuntimeOwnerTaskState::Active));
}

void test_active_normal_only_selects_normal_and_translates_adapter()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerAdapterCore reference{};
    const RuntimeOwnerTaskCycleInput input{
        0,
        0,
        1,
        0,
        {
            NormalIntentKind::PublishTelemetry,
            0x01,
            0,
            71,
            17,
        },
    };

    const NormalSubmitResult expected_normal =
        reference.normal_port().submit(input.normal);
    const AdapterStepResult expected_step = reference.step();
    const std::uint8_t expected_dispatch = dispatch_present(reference);
    const RuntimeOwnerAdapterView expected_view = reference.view();

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    CHECK(result.disposition == RuntimeOwnerTaskCycleDisposition::Processed);
    CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::NormalIntent);
    CHECK(result.transport_result ==
          OwnerRequestResult::RejectedNotAllowed);
    CHECK(result.normal_result == expected_normal);
    CHECK(result.urgent_recheck_required == 1);
    CHECK(result.dispatch_pending == expected_dispatch);
    CHECK(result.reserved == 0);
    CHECK(adapter_step_results_equal(result.step_result, expected_step));
    CHECK(adapter_views_equal(
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core), expected_view));

    const bool expected_terminal =
        expected_step.action == AdapterStepAction::Terminal ||
        expected_view.core.phase == RuntimeOwnerPhase::ShutdownCommitted;
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(core) ==
          (expected_terminal ? RuntimeOwnerTaskState::Terminal
                             : RuntimeOwnerTaskState::Active));
}

void test_active_malformed_normal_rejects_before_admission_and_steps_once()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerAdapterCore reference{};
    RuntimeOwnerTaskCycleInput input{};
    input.normal_pending = 1;
    input.normal = {
        NormalIntentKind::PublishTelemetry, 0x01, 0, 71, 0};

    const AdapterStepResult expected_step = reference.step();
    const RuntimeOwnerAdapterView expected_view = reference.view();
    CHECK(!RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(core));

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    CHECK(result.disposition == RuntimeOwnerTaskCycleDisposition::Processed);
    CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::NormalIntent);
    CHECK(result.normal_result == NormalSubmitResult::RejectedInvalid);
    CHECK(result.urgent_recheck_required == 1);
    CHECK(result.reserved == 0);
    CHECK(adapter_step_results_equal(result.step_result, expected_step));
    CHECK(adapter_views_equal(
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core), expected_view));
    CHECK(!RuntimeOwnerTaskCoreTestPeer::runtime_admission_open(core));
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(core) ==
          RuntimeOwnerTaskState::Active);
}

void test_active_empty_cycle_steps_once_and_translates_adapter()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_activate(core);
    RuntimeOwnerAdapterCore reference{};

    const AdapterStepResult expected_step = reference.step();
    const std::uint8_t expected_dispatch = dispatch_present(reference);
    const RuntimeOwnerAdapterView expected_view = reference.view();

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, {});

    CHECK(result.disposition == RuntimeOwnerTaskCycleDisposition::Processed);
    CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::None);
    CHECK(result.transport_result ==
          OwnerRequestResult::RejectedNotAllowed);
    CHECK(result.normal_result == NormalSubmitResult::RejectedNotReady);
    CHECK(result.urgent_recheck_required == 1);
    CHECK(result.dispatch_pending == expected_dispatch);
    CHECK(result.reserved == 0);
    CHECK(adapter_step_results_equal(result.step_result, expected_step));
    CHECK(adapter_views_equal(
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core), expected_view));

    const bool expected_terminal =
        expected_step.action == AdapterStepAction::Terminal ||
        expected_view.core.phase == RuntimeOwnerPhase::ShutdownCommitted;
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(core) ==
          (expected_terminal ? RuntimeOwnerTaskState::Terminal
                             : RuntimeOwnerTaskState::Active));
}

void test_task_core_source_has_exactly_one_adapter_step_callsite()
{
    const std::string normalized_source = normalized_task_core_source();
    CHECK(count_nonoverlapping_occurrences(
              normalized_source, "adapter_.step()") == 1);
}

void test_task_core_header_has_no_generic_shutdown_surface()
{
    const std::string normalized_header = normalized_task_core_header();
    CHECK(normalized_header.find("RequestShutdown") == std::string::npos);
    CHECK(normalized_header.find("shutdown_result") == std::string::npos);

    const std::size_t task_core =
        normalized_header.find("classRuntimeOwnerTaskCore{");
    const std::size_t process_cycle =
        normalized_header.find("process_cycle(", task_core);
    const std::size_t public_before =
        normalized_header.rfind("public:", process_cycle);
    const std::size_t private_before =
        normalized_header.rfind("private:", process_cycle);
    CHECK(task_core != std::string::npos);
    CHECK(process_cycle != std::string::npos);
    CHECK(private_before > public_before);
}

void test_terminal_all_pending_is_rejected_without_adapter_mutation()
{
    RuntimeOwnerTaskCore core{};
    RuntimeOwnerTaskCoreTestPeer::fixture_terminal(core);
    const RuntimeOwnerAdapterView before =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    const RuntimeOwnerTaskCycleInput input{
        1,
        1,
        1,
        0,
        {
            NormalIntentKind::PublishTelemetry,
            0x01,
            0,
            81,
            19,
        },
    };

    const RuntimeOwnerTaskCycleResult result =
        RuntimeOwnerTaskCoreTestPeer::process_cycle(core, input);

    const RuntimeOwnerAdapterView after =
        RuntimeOwnerTaskCoreTestPeer::adapter_view(core);
    check_safe_unprocessed_result(
        result,
        RuntimeOwnerTaskCycleDisposition::RejectedTerminal);
    CHECK(adapter_views_equal(before, after));
    CHECK(RuntimeOwnerTaskCoreTestPeer::state(core) ==
          RuntimeOwnerTaskState::Terminal);
}

void test_construction_and_dormant_active_cycles_are_allocation_free()
{
    static_assert(!std::is_copy_constructible<RuntimeOwnerTaskCore>::value);
    static_assert(!std::is_copy_assignable<RuntimeOwnerTaskCore>::value);
    static_assert(!std::is_move_constructible<RuntimeOwnerTaskCore>::value);
    static_assert(!std::is_move_assignable<RuntimeOwnerTaskCore>::value);

    const std::size_t allocations_before = g_allocation_count;
    const std::size_t deallocations_before = g_deallocation_count;
    {
        RuntimeOwnerTaskCore dormant{};
        for (std::size_t index = 0; index < 128; ++index) {
            const RuntimeOwnerTaskCycleResult result =
                RuntimeOwnerTaskCoreTestPeer::process_cycle(dormant, {});
            CHECK(result.disposition ==
                  RuntimeOwnerTaskCycleDisposition::RejectedInactive);
            CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::None);
        }

        RuntimeOwnerTaskCore active{};
        RuntimeOwnerTaskCoreTestPeer::fixture_activate(active);
        for (std::size_t index = 0; index < 128; ++index) {
            const RuntimeOwnerTaskCycleResult result =
                RuntimeOwnerTaskCoreTestPeer::process_cycle(active, {});
            CHECK(result.disposition ==
                  RuntimeOwnerTaskCycleDisposition::Processed);
            CHECK(result.selected_work == RuntimeOwnerTaskWorkKind::None);
            CHECK(result.step_result.action == AdapterStepAction::Idle);
            CHECK(result.urgent_recheck_required == 1);
        }
    }
    CHECK(g_allocation_count == allocations_before);
    CHECK(g_deallocation_count == deallocations_before);
}

} // namespace

void *operator new(const std::size_t size)
{
    ++g_allocation_count;
    if (void *const memory = std::malloc(size)) {
        return memory;
    }
    std::abort();
}

void *operator new[](const std::size_t size)
{
    ++g_allocation_count;
    if (void *const memory = std::malloc(size)) {
        return memory;
    }
    std::abort();
}

void operator delete(void *const memory) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete[](void *const memory) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete(void *const memory, const std::size_t) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete[](void *const memory, const std::size_t) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

int main()
{
    test_contract_numeric_values_and_defaults();
    test_contract_layout_traits_and_api();
    test_ingress_gate_precedence_truth_table();
    test_default_task_state_is_dormant();
    test_dormant_empty_cycle_is_rejected_without_adapter_mutation();
    test_dormant_normal_cycle_is_rejected_without_adapter_mutation();
    test_dormant_transport_cycle_is_rejected_without_adapter_mutation();
    test_dormant_shutdown_cycle_is_rejected_without_adapter_mutation();
    test_dormant_malformed_normal_is_rejected_without_adapter_mutation();
    test_active_source_scoped_shutdown_blocks_control_and_normal();
    test_active_transport_and_normal_selects_transport_only_and_translates();
    test_active_normal_only_selects_normal_and_translates_adapter();
    test_active_malformed_normal_rejects_before_admission_and_steps_once();
    test_active_empty_cycle_steps_once_and_translates_adapter();
    test_task_core_source_has_exactly_one_adapter_step_callsite();
    test_task_core_header_has_no_generic_shutdown_surface();
    test_terminal_all_pending_is_rejected_without_adapter_mutation();
    test_construction_and_dormant_active_cycles_are_allocation_free();

    if (g_failure_count != 0) {
        std::fprintf(
            stderr,
            "RUNTIME_OWNER_TASK_CORE_TEST FAIL checks=%zu failures=%zu\n",
            g_check_count,
            g_failure_count);
        return 1;
    }

    std::printf(
        "RUNTIME_OWNER_TASK_CORE_TEST PASS checks=%zu\n",
        g_check_count);
    return 0;
}
