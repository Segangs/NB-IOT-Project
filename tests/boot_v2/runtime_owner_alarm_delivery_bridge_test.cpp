#include "runtime_owner_adapter_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(
    const bool condition,
    const char *const expression,
    const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(
            stderr,
            "CHECK failed: %s:%d: %s\n",
            __FILE__,
            line,
            expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

using namespace boot_v2;

constexpr NormalIntent alarm_intent(
    const std::uint32_t sensor_id,
    const std::uint32_t revision,
    const TemperatureAlarmEdge edge) noexcept
{
    return {
        NormalIntentKind::PublishTelemetry,
        static_cast<std::uint8_t>(
            kNormalIntentFlagFrozenValue |
            kNormalIntentFlagAlarmEdge |
            (edge == TemperatureAlarmEdge::High
                 ? kNormalIntentFlagAlarmHigh
                 : 0u)),
        275,
        sensor_id,
        revision,
    };
}

constexpr NormalIntent periodic_intent(
    const std::uint32_t sensor_id,
    const std::uint32_t revision) noexcept
{
    return {
        NormalIntentKind::PublishTelemetry,
        kNormalIntentFlagFrozenValue,
        275,
        sensor_id,
        revision,
    };
}

bool prepare_runtime_ready(RuntimeOwnerAdapterCore &adapter) noexcept
{
    if (adapter.request_transport_attempt() !=
            OwnerRequestResult::Accepted ||
        adapter.step().action != AdapterStepAction::CoreTransitionApplied ||
        adapter.step().action != AdapterStepAction::DispatchPrepared) {
        return false;
    }
    const AdapterDispatch open = adapter.peek_dispatch();
    if (open.effect.kind !=
            RuntimeOwnerEffectKind::StartTransportAttempt ||
        adapter.acknowledge_dispatch(open.dispatch_sequence) !=
            DispatchAckResult::AcceptedOperationInflight) {
        return false;
    }

    TrustedReceipt established{};
    established.kind = TrustedReceiptKind::TransportEstablished;
    established.effect_kind =
        RuntimeOwnerEffectKind::StartTransportAttempt;
    established.mqtt_session_id = 23;
    established.mqtt_generation =
        open.effect.attempt.mqtt_generation;
    if (adapter.trusted_receipt_port().submit(established) !=
            TrustedIngressResult::Accepted ||
        adapter.step().action != AdapterStepAction::CoreTransitionApplied) {
        return false;
    }

    TrustedReceipt configured{};
    configured.kind = TrustedReceiptKind::ConfigCommitted;
    configured.mqtt_session_id = 23;
    configured.mqtt_generation =
        open.effect.attempt.mqtt_generation;
    configured.config_commit_sequence = 29;
    if (adapter.trusted_receipt_port().submit(configured) !=
            TrustedIngressResult::Accepted ||
        adapter.step().action != AdapterStepAction::CoreTransitionApplied) {
        return false;
    }

    constexpr std::array<RuntimeOwnerEffectKind, 4> liveness_kinds{{
        RuntimeOwnerEffectKind::StartAtProbe,
        RuntimeOwnerEffectKind::StartProbePublish,
        RuntimeOwnerEffectKind::VerifySubscription,
        RuntimeOwnerEffectKind::PullFollowupConfig,
    }};
    for (const RuntimeOwnerEffectKind expected : liveness_kinds) {
        if (adapter.step().action !=
            AdapterStepAction::DispatchPrepared) {
            return false;
        }
        const AdapterDispatch dispatch = adapter.peek_dispatch();
        if (dispatch.effect.kind != expected ||
            adapter.acknowledge_dispatch(dispatch.dispatch_sequence) !=
                DispatchAckResult::AcceptedOperationInflight) {
            return false;
        }
        TrustedReceipt completed{};
        completed.kind = TrustedReceiptKind::OperationCompleted;
        completed.effect_kind = dispatch.effect.kind;
        completed.correlation_id = dispatch.effect.correlation_id;
        completed.mqtt_session_id =
            dispatch.effect.attempt.mqtt_session_id;
        completed.mqtt_generation =
            dispatch.effect.attempt.mqtt_generation;
        completed.config_apply_epoch =
            dispatch.effect.attempt.config_apply_epoch;
        if (adapter.trusted_receipt_port().submit(completed) !=
                TrustedIngressResult::Accepted ||
            adapter.step().action !=
                AdapterStepAction::CoreTransitionApplied) {
            return false;
        }
    }

    if (adapter.step().action != AdapterStepAction::DispatchPrepared) {
        return false;
    }
    const AdapterDispatch freeze = adapter.peek_dispatch();
    if (freeze.effect.kind != RuntimeOwnerEffectKind::FreezeBootSnapshot ||
        adapter.acknowledge_dispatch(freeze.dispatch_sequence) !=
            DispatchAckResult::AcceptedOperationInflight) {
        return false;
    }
    TrustedReceipt snapshot{};
    snapshot.kind = TrustedReceiptKind::SnapshotSucceeded;
    snapshot.effect_kind = freeze.effect.kind;
    snapshot.correlation_id = freeze.effect.correlation_id;
    snapshot.mqtt_session_id = freeze.effect.attempt.mqtt_session_id;
    snapshot.mqtt_generation = freeze.effect.attempt.mqtt_generation;
    snapshot.config_apply_epoch =
        freeze.effect.attempt.config_apply_epoch;
    if (adapter.trusted_receipt_port().submit(snapshot) !=
            TrustedIngressResult::Accepted ||
        adapter.step().action != AdapterStepAction::CoreTransitionApplied ||
        adapter.step().action != AdapterStepAction::DispatchPrepared) {
        return false;
    }
    const AdapterDispatch end_boot = adapter.peek_dispatch();
    return end_boot.effect.kind ==
               RuntimeOwnerEffectKind::EndBootOrchestration &&
           adapter.acknowledge_dispatch(end_boot.dispatch_sequence) ==
               DispatchAckResult::AcceptedDelivery &&
           adapter.view().core.phase == RuntimeOwnerPhase::RuntimeReady &&
           adapter.view().boot_end_released == 1;
}

AdapterDispatch start_normal(
    RuntimeOwnerAdapterCore &adapter,
    const NormalIntent intent) noexcept
{
    CHECK(adapter.normal_port().submit(intent) ==
          NormalSubmitResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    const AdapterDispatch dispatch = adapter.peek_dispatch();
    CHECK(dispatch.kind == AdapterDispatchKind::NormalIntent);
    CHECK(adapter.acknowledge_dispatch(dispatch.dispatch_sequence) ==
          DispatchAckResult::AcceptedOperationInflight);
    return dispatch;
}

NormalCompletion completion(
    const AdapterDispatch dispatch,
    const NormalCompletionKind kind) noexcept
{
    NormalCompletion result{};
    result.kind = kind;
    result.dispatch_sequence = dispatch.dispatch_sequence;
    result.enqueue_sequence = dispatch.enqueue_sequence;
    result.diagnostic_code =
        kind == NormalCompletionKind::Succeeded ? 0 : 71;
    return result;
}

TemperatureAlarmTerminalResult mapped(
    const NormalCompletionKind kind) noexcept
{
    switch (kind) {
    case NormalCompletionKind::Succeeded:
        return TemperatureAlarmTerminalResult::Succeeded;
    case NormalCompletionKind::Failed:
        return TemperatureAlarmTerminalResult::Failed;
    case NormalCompletionKind::TimedOut:
        return TemperatureAlarmTerminalResult::TimedOut;
    case NormalCompletionKind::Cancelled:
        return TemperatureAlarmTerminalResult::Cancelled;
    case NormalCompletionKind::Invalid:
    default:
        return TemperatureAlarmTerminalResult::Invalid;
    }
}

TrustedReceipt disconnect_receipt(
    const RuntimeOwnerAdapterCore &adapter) noexcept
{
    const RuntimeOwnerView core = adapter.view().core;
    TrustedReceipt result{};
    result.kind = TrustedReceiptKind::TransportDisconnected;
    result.mqtt_session_id = core.mqtt_session_id;
    result.mqtt_generation = core.mqtt_generation;
    result.diagnostic_code = 91;
    return result;
}

bool alarm_events_equal(
    const TemperatureAlarmDeliveryEvent left,
    const TemperatureAlarmDeliveryEvent right) noexcept
{
    return left.sensor_id == right.sensor_id &&
           left.snapshot_revision == right.snapshot_revision &&
           left.result == right.result && left.edge == right.edge &&
           left.reserved == right.reserved;
}

void acknowledge_pending_safety_dispatches(
    RuntimeOwnerAdapterCore &adapter) noexcept
{
    while (adapter.view().pending_effect_count != 0) {
        CHECK(adapter.step().action ==
              AdapterStepAction::DispatchPrepared);
        const AdapterDispatch dispatch = adapter.peek_dispatch();
        CHECK(dispatch.kind == AdapterDispatchKind::CoreEffect);
        CHECK(
            dispatch.effect.kind == RuntimeOwnerEffectKind::RecordFault ||
            dispatch.effect.kind == RuntimeOwnerEffectKind::EnterRecovery);
        CHECK(adapter.acknowledge_dispatch(
                  dispatch.dispatch_sequence) ==
              DispatchAckResult::AcceptedDelivery);
    }
}

void complete_alarm(
    RuntimeOwnerAdapterCore &adapter,
    const std::uint32_t revision) noexcept
{
    const std::uint32_t sensor_id = revision % 2 == 0 ? 2u : 1u;
    const TemperatureAlarmEdge edge =
        revision % 3 == 0 ? TemperatureAlarmEdge::Clear
                          : TemperatureAlarmEdge::High;
    const AdapterDispatch dispatch =
        start_normal(adapter, alarm_intent(sensor_id, revision, edge));
    CHECK(adapter.normal_completion_port().submit(completion(
              dispatch,
              NormalCompletionKind::Succeeded)) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
}

void test_task8b_alarm_helper_accepts_only_canonical_alarm_intents() noexcept
{
    TemperatureAlarmEdge edge = TemperatureAlarmEdge::Invalid;
    CHECK(runtime_owner_alarm_intent_edge(
        alarm_intent(1, 7, TemperatureAlarmEdge::Clear), edge));
    CHECK(edge == TemperatureAlarmEdge::Clear);
    CHECK(runtime_owner_alarm_intent_edge(
        alarm_intent(2, 8, TemperatureAlarmEdge::High), edge));
    CHECK(edge == TemperatureAlarmEdge::High);
    CHECK(!runtime_owner_alarm_intent_edge(periodic_intent(1, 9), edge));
    CHECK(!runtime_owner_alarm_intent_edge(
        {NormalIntentKind::PublishTelemetry, 0x07, 275, 3, 10},
        edge));
    CHECK(!runtime_owner_alarm_intent_edge(
        {NormalIntentKind::PublishTelemetry, 0x05, 275, 1, 10},
        edge));
}

void test_four_exact_physical_terminals_emit_exact_alarm_event() noexcept
{
    constexpr std::array<NormalCompletionKind, 4> outcomes{{
        NormalCompletionKind::Succeeded,
        NormalCompletionKind::Failed,
        NormalCompletionKind::TimedOut,
        NormalCompletionKind::Cancelled,
    }};
    std::uint32_t revision = 41;
    for (const NormalCompletionKind outcome : outcomes) {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(prepare_runtime_ready(adapter));
        const TemperatureAlarmEdge edge =
            revision % 2 == 0 ? TemperatureAlarmEdge::Clear
                              : TemperatureAlarmEdge::High;
        const std::uint32_t sensor_id =
            revision % 2 == 0 ? 2u : 1u;
        const AdapterDispatch dispatch = start_normal(
            adapter, alarm_intent(sensor_id, revision, edge));
        CHECK(adapter.normal_completion_port().submit(
                  completion(dispatch, outcome)) ==
              TrustedIngressResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::TrustedReceiptDiscarded);

        TemperatureAlarmDeliveryEvent delivered{};
        CHECK(adapter.try_pop_alarm_delivery(delivered) ==
              TemperatureAlarmDeliveryPopResult::Popped);
        CHECK(delivered.sensor_id == sensor_id);
        CHECK(delivered.snapshot_revision == revision);
        CHECK(delivered.result == mapped(outcome));
        CHECK(delivered.edge == edge);
        CHECK(delivered.reserved == 0);
        CHECK(adapter.try_pop_alarm_delivery(delivered) ==
              TemperatureAlarmDeliveryPopResult::Empty);
        ++revision;
    }
}

void test_stale_duplicate_and_non_alarm_terminals_emit_zero_events() noexcept
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(prepare_runtime_ready(adapter));
    const AdapterDispatch dispatch = start_normal(
        adapter, alarm_intent(1, 77, TemperatureAlarmEdge::High));
    NormalCompletion stale =
        completion(dispatch, NormalCompletionKind::Failed);
    ++stale.enqueue_sequence;
    CHECK(adapter.normal_completion_port().submit(stale) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    TemperatureAlarmDeliveryEvent delivered{};
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);

    const NormalCompletion exact =
        completion(dispatch, NormalCompletionKind::Succeeded);
    CHECK(adapter.normal_completion_port().submit(exact) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Popped);
    CHECK(delivered.snapshot_revision == 77);
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);

    CHECK(adapter.normal_completion_port().submit(exact) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);

    RuntimeOwnerAdapterCore periodic{};
    CHECK(prepare_runtime_ready(periodic));
    const AdapterDispatch periodic_dispatch =
        start_normal(periodic, periodic_intent(1, 91));
    CHECK(periodic.normal_completion_port().submit(completion(
              periodic_dispatch,
              NormalCompletionKind::Succeeded)) ==
          TrustedIngressResult::Accepted);
    CHECK(periodic.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(periodic.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);
}

void test_disconnect_emits_current_and_queued_cancellations_before_removal()
    noexcept
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(prepare_runtime_ready(adapter));
    CHECK(adapter.normal_port().submit(
              alarm_intent(1, 101, TemperatureAlarmEdge::High)) ==
          NormalSubmitResult::Accepted);
    CHECK(adapter.step().action == AdapterStepAction::DispatchPrepared);
    CHECK(adapter.normal_port().submit(
              alarm_intent(2, 102, TemperatureAlarmEdge::Clear)) ==
          NormalSubmitResult::Accepted);
    CHECK(adapter.normal_port().submit(
              alarm_intent(1, 103, TemperatureAlarmEdge::High)) ==
          NormalSubmitResult::Accepted);

    CHECK(adapter.trusted_receipt_port().submit(
              disconnect_receipt(adapter)) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().core.phase ==
          RuntimeOwnerPhase::RecoveryPending);
    CHECK(adapter.view().current_dispatch.kind ==
          AdapterDispatchKind::None);
    CHECK(adapter.view().normal_depth == 0);

    constexpr std::array<TemperatureAlarmDeliveryEvent, 3> expected{{
        {1,
         101,
         TemperatureAlarmTerminalResult::Cancelled,
         TemperatureAlarmEdge::High,
         0},
        {2,
         102,
         TemperatureAlarmTerminalResult::Cancelled,
         TemperatureAlarmEdge::Clear,
         0},
        {1,
         103,
         TemperatureAlarmTerminalResult::Cancelled,
         TemperatureAlarmEdge::High,
         0},
    }};
    TemperatureAlarmDeliveryEvent delivered{};
    for (const auto item : expected) {
        CHECK(adapter.try_pop_alarm_delivery(delivered) ==
              TemperatureAlarmDeliveryPopResult::Popped);
        CHECK(alarm_events_equal(delivered, item));
    }
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);
}

void test_inflight_quarantine_emits_once_and_late_terminal_is_cleanup_only()
    noexcept
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(prepare_runtime_ready(adapter));
    const AdapterDispatch inflight = start_normal(
        adapter, alarm_intent(2, 211, TemperatureAlarmEdge::Clear));

    CHECK(adapter.trusted_receipt_port().submit(
              disconnect_receipt(adapter)) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);

    TemperatureAlarmDeliveryEvent delivered{};
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Popped);
    CHECK(alarm_events_equal(
        delivered,
        {2,
         211,
         TemperatureAlarmTerminalResult::Cancelled,
         TemperatureAlarmEdge::Clear,
         0}));
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);

    acknowledge_pending_safety_dispatches(adapter);
    CHECK(adapter.normal_completion_port().submit(completion(
              inflight,
              NormalCompletionKind::Succeeded)) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.view().physical_inflight.kind ==
          AdapterDispatchKind::None);
    CHECK(adapter.view().physical_inflight_cancel_pending == 0);
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);
}

void test_full_mailbox_retains_exact_terminal_until_consumer_frees_slot()
    noexcept
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(prepare_runtime_ready(adapter));
    for (std::uint32_t revision = 1; revision <= 16; ++revision) {
        complete_alarm(adapter, revision);
    }
    CHECK(adapter.alarm_delivery_mailbox_view().depth == 16);

    const AdapterDispatch blocked = start_normal(
        adapter, alarm_intent(1, 17, TemperatureAlarmEdge::High));
    CHECK(adapter.normal_completion_port().submit(completion(
              blocked,
              NormalCompletionKind::Failed)) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::AwaitingTrustedReceipt);
    const RuntimeOwnerAdapterView retained = adapter.view();
    CHECK(retained.trusted_depth == 1);
    CHECK(retained.physical_inflight.kind ==
          AdapterDispatchKind::NormalIntent);
    CHECK(retained.physical_inflight.dispatch_sequence ==
          blocked.dispatch_sequence);
    CHECK(adapter.alarm_delivery_mailbox_view().depth == 16);
    CHECK(adapter.alarm_delivery_mailbox_view().overflow_count == 1);
    CHECK(adapter.alarm_delivery_mailbox_view().overflow_latched == 1);
    CHECK(adapter.take_alarm_delivery_overflow_log_pending());
    CHECK(!adapter.take_alarm_delivery_overflow_log_pending());

    TemperatureAlarmDeliveryEvent delivered{};
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Popped);
    CHECK(delivered.snapshot_revision == 1);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.view().trusted_depth == 0);
    CHECK(adapter.view().physical_inflight.kind ==
          AdapterDispatchKind::None);

    for (std::uint32_t revision = 2; revision <= 16; ++revision) {
        CHECK(adapter.try_pop_alarm_delivery(delivered) ==
              TemperatureAlarmDeliveryPopResult::Popped);
        CHECK(delivered.snapshot_revision == revision);
    }
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Popped);
    CHECK(alarm_events_equal(
        delivered,
        {1,
         17,
         TemperatureAlarmTerminalResult::Failed,
         TemperatureAlarmEdge::High,
         0}));
    CHECK(adapter.try_pop_alarm_delivery(delivered) ==
          TemperatureAlarmDeliveryPopResult::Empty);
}

void test_full_mailbox_does_not_block_safety_quarantine() noexcept
{
    RuntimeOwnerAdapterCore adapter{};
    CHECK(prepare_runtime_ready(adapter));
    for (std::uint32_t revision = 1; revision <= 16; ++revision) {
        complete_alarm(adapter, revision);
    }
    const AdapterDispatch inflight = start_normal(
        adapter, alarm_intent(2, 301, TemperatureAlarmEdge::Clear));

    CHECK(adapter.trusted_receipt_port().submit(
              disconnect_receipt(adapter)) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::CoreTransitionApplied);
    CHECK(adapter.view().core.phase ==
          RuntimeOwnerPhase::RecoveryPending);
    CHECK(adapter.view().physical_inflight_cancel_pending == 1);
    const TemperatureAlarmDeliveryMailboxView overflow =
        adapter.alarm_delivery_mailbox_view();
    CHECK(overflow.depth == 16);
    CHECK(overflow.overflow_count == 1);
    CHECK(overflow.overflow_latched == 1);
    CHECK(adapter.take_alarm_delivery_overflow_log_pending());
    CHECK(!adapter.take_alarm_delivery_overflow_log_pending());

    acknowledge_pending_safety_dispatches(adapter);
    CHECK(adapter.normal_completion_port().submit(completion(
              inflight,
              NormalCompletionKind::Cancelled)) ==
          TrustedIngressResult::Accepted);
    CHECK(adapter.step().action ==
          AdapterStepAction::TrustedReceiptDiscarded);
    CHECK(adapter.alarm_delivery_mailbox_view().depth == 16);
}

void test_full_mailbox_does_not_block_shutdown_or_critical_cancellation()
    noexcept
{
    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(prepare_runtime_ready(adapter));
        for (std::uint32_t revision = 1; revision <= 16; ++revision) {
            complete_alarm(adapter, revision);
        }
        (void)start_normal(
            adapter,
            alarm_intent(1, 401, TemperatureAlarmEdge::High));
        CHECK(adapter.shutdown_port().request() ==
              UrgentRequestResult::Accepted);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.view().core.phase ==
              RuntimeOwnerPhase::ShutdownCommitted);
        CHECK(adapter.view().physical_inflight.kind ==
              AdapterDispatchKind::None);
        CHECK(adapter.alarm_delivery_mailbox_view().depth == 16);
        CHECK(
            adapter.alarm_delivery_mailbox_view().overflow_count == 1);
        CHECK(adapter.alarm_delivery_mailbox_view().overflow_latched == 1);
        CHECK(adapter.take_alarm_delivery_overflow_log_pending());
        CHECK(!adapter.take_alarm_delivery_overflow_log_pending());
    }

    {
        RuntimeOwnerAdapterCore adapter{};
        CHECK(prepare_runtime_ready(adapter));
        for (std::uint32_t revision = 1; revision <= 16; ++revision) {
            complete_alarm(adapter, revision);
        }
        (void)start_normal(
            adapter,
            alarm_intent(2, 402, TemperatureAlarmEdge::Clear));
        CHECK(adapter.trusted_receipt_port().submit({}) ==
              TrustedIngressResult::RejectedInvalid);
        CHECK(adapter.view().critical_pending == 1);
        CHECK(adapter.step().action ==
              AdapterStepAction::CoreTransitionApplied);
        CHECK(adapter.view().core.phase ==
              RuntimeOwnerPhase::RecoveryPending);
        CHECK(adapter.view().physical_inflight_cancel_pending == 1);
        CHECK(adapter.alarm_delivery_mailbox_view().depth == 16);
        CHECK(
            adapter.alarm_delivery_mailbox_view().overflow_count == 1);
        CHECK(adapter.alarm_delivery_mailbox_view().overflow_latched == 1);
        CHECK(adapter.take_alarm_delivery_overflow_log_pending());
        CHECK(!adapter.take_alarm_delivery_overflow_log_pending());
    }
}

} // namespace

int main()
{
    test_task8b_alarm_helper_accepts_only_canonical_alarm_intents();
    test_four_exact_physical_terminals_emit_exact_alarm_event();
    test_stale_duplicate_and_non_alarm_terminals_emit_zero_events();
    test_disconnect_emits_current_and_queued_cancellations_before_removal();
    test_inflight_quarantine_emits_once_and_late_terminal_is_cleanup_only();
    test_full_mailbox_retains_exact_terminal_until_consumer_frees_slot();
    test_full_mailbox_does_not_block_safety_quarantine();
    test_full_mailbox_does_not_block_shutdown_or_critical_cancellation();
    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "runtime_owner_alarm_delivery_bridge_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "runtime_owner_alarm_delivery_bridge_test: %zu checks passed\n",
        g_checks);
    return 0;
}
