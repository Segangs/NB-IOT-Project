#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_ADAPTER_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_ADAPTER_CORE_HPP

#include <array>
#include <cstdint>
#include <type_traits>

#include "runtime_owner_core.hpp"

namespace boot_v2 {

enum class NormalSubmitResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedNotReady = 1,
    RejectedFull = 2,
    RejectedSequenceSaturated = 3,
    Accepted = 4,
    AcceptedCoalesced = 5,
};

enum class TrustedIngressResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedNotAllowed = 1,
    RejectedFull = 2,
    RejectedSequenceSaturated = 3,
    Accepted = 4,
};

enum class UrgentRequestResult : std::uint8_t {
    Invalid = 0,
    Accepted = 1,
    AcceptedDuplicate = 2,
    AlreadyTerminal = 3,
};

enum class OwnerRequestResult : std::uint8_t {
    RejectedNotAllowed = 0,
    RejectedFatal = 1,
    Accepted = 2,
    AcceptedDuplicate = 3,
};

enum class DispatchAckResult : std::uint8_t {
    RejectedNoDispatch = 0,
    RejectedWrongSequence = 1,
    AcceptedDelivery = 2,
    AcceptedOperationInflight = 3,
    AcceptedDuplicate = 4,
};

enum class AdapterStepAction : std::uint8_t {
    Invalid = 0,
    Idle = 1,
    AwaitingDispatchAck = 2,
    AwaitingTrustedReceipt = 3,
    CoreTransitionApplied = 4,
    DispatchPrepared = 6,
    TrustedReceiptDiscarded = 7,
    CriticalLedgerHandled = 8,
    CoreAdapterFatalHandled = 9,
    Terminal = 10,
};

struct AdapterStepResult {
    AdapterStepAction action{AdapterStepAction::Invalid};
    RuntimeOwnerDisposition core_disposition{RuntimeOwnerDisposition::Rejected};
    RuntimeOwnerPhase phase_before{RuntimeOwnerPhase::ColdStart};
    RuntimeOwnerPhase phase_after{RuntimeOwnerPhase::ColdStart};
    std::uint32_t consumed_ingress_sequence{0};
    std::uint32_t consumed_enqueue_sequence{0};
    std::uint32_t prepared_dispatch_sequence{0};
};

enum class NormalIntentKind : std::uint8_t {
    Invalid = 0,
    PublishTelemetry = 1,
    RefreshRssi = 2,
    PullConfig = 3,
    PullCommand = 4,
    PublishAdapterRemoved = 5,
    PublishAdapterRestored = 6,
};

struct NormalIntent {
    NormalIntentKind kind{NormalIntentKind::Invalid};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};
    std::uint32_t subject_id{0};
    std::uint32_t snapshot_revision{0};
};

[[nodiscard]] constexpr bool runtime_owner_normal_intent_is_canonical(
    const NormalIntent input) noexcept
{
    if (input.flags != 0 || input.reserved != 0) {
        return false;
    }
    switch (input.kind) {
    case NormalIntentKind::PublishTelemetry:
    case NormalIntentKind::PublishAdapterRemoved:
    case NormalIntentKind::PublishAdapterRestored:
        return input.subject_id != 0 && input.snapshot_revision != 0;
    case NormalIntentKind::RefreshRssi:
    case NormalIntentKind::PullConfig:
    case NormalIntentKind::PullCommand:
        return input.subject_id == 0 && input.snapshot_revision == 0;
    case NormalIntentKind::Invalid:
    default:
        return false;
    }
}

enum class TrustedReceiptKind : std::uint8_t {
    Invalid = 0,
    TransportEstablished = 1,
    TransportAttemptFailed = 2,
    ConfigCommitted = 3,
    OperationCompleted = 4,
    OperationFailed = 5,
    DeadlineExpired = 6,
    SnapshotSucceeded = 7,
    SnapshotFailed = 8,
    TransportDisconnected = 9,
};

struct TrustedReceipt {
    TrustedReceiptKind kind{TrustedReceiptKind::Invalid};
    RuntimeOwnerEffectKind effect_kind{RuntimeOwnerEffectKind::None};
    std::uint16_t reserved{0};
    std::uint32_t correlation_id{0};
    std::uint32_t mqtt_session_id{0};
    std::uint32_t mqtt_generation{0};
    std::uint32_t config_commit_sequence{0};
    std::uint32_t config_apply_epoch{0};
    std::uint32_t diagnostic_code{0};
};

enum class NormalCompletionKind : std::uint8_t {
    Invalid = 0,
    Succeeded = 1,
    Failed = 2,
    TimedOut = 3,
    Cancelled = 4,
};

struct NormalCompletion {
    NormalCompletionKind kind{NormalCompletionKind::Invalid};
    std::array<std::uint8_t, 3> reserved{};
    std::uint32_t dispatch_sequence{0};
    std::uint32_t enqueue_sequence{0};
    std::uint32_t diagnostic_code{0};
};

enum class AdapterDispatchKind : std::uint8_t {
    None = 0,
    CoreEffect = 1,
    NormalIntent = 2,
};

struct AdapterDispatch {
    AdapterDispatchKind kind{AdapterDispatchKind::None};
    std::array<std::uint8_t, 3> reserved{};
    std::uint32_t dispatch_sequence{0};
    std::uint32_t enqueue_sequence{0};
    RuntimeOwnerEffect effect{};
    NormalIntent normal_intent{};
};

enum class AdapterCriticalReason : std::uint8_t {
    None = 0,
    TrustedQueueOverflow = 1,
    TrustedProtocolViolation = 2,
    NormalSequenceSaturation = 3,
    TrustedSequenceSaturation = 4,
    DispatchSequenceSaturation = 5,
    PendingEffectInvariant = 6,
    CoreAdapterInvariant = 7,
};

struct AdapterCriticalLedger {
    AdapterCriticalReason first_reason{AdapterCriticalReason::None};
    AdapterCriticalReason last_reason{AdapterCriticalReason::None};
    std::uint16_t reserved{0};
    std::uint32_t reason_mask{0};
    std::uint32_t first_ingress_sequence{0};
    std::uint32_t last_ingress_sequence{0};
    std::uint32_t first_diagnostic_code{0};
    std::uint32_t last_diagnostic_code{0};
    std::uint32_t occurrence_count{0};
};

struct RuntimeOwnerAdapterView {
    RuntimeOwnerView core{};
    AdapterDispatch current_dispatch{};
    AdapterDispatch physical_inflight{};
    AdapterCriticalLedger critical{};

    std::uint32_t last_normal_enqueue_sequence{0};
    std::uint32_t last_trusted_ingress_sequence{0};
    std::uint32_t last_dispatch_sequence{0};
    std::uint32_t last_ack_dispatch_sequence{0};
    std::uint32_t last_trusted_diagnostic_ingress_sequence{0};
    std::uint32_t last_trusted_diagnostic_code{0};

    std::uint32_t normal_coalesced_count{0};
    std::uint32_t normal_rejected_full_count{0};
    std::uint32_t normal_cancelled_count{0};
    std::uint32_t trusted_rejected_full_count{0};
    std::uint32_t trusted_protocol_violation_count{0};
    std::uint32_t trusted_stale_count{0};
    std::uint32_t trusted_duplicate_count{0};
    std::uint32_t trusted_cancelled_count{0};
    std::uint32_t effect_cancelled_count{0};
    std::uint32_t dispatch_rejected_ack_count{0};
    std::uint32_t normal_completion_stale_count{0};

    std::uint8_t normal_depth{0};
    std::uint8_t normal_high_water{0};
    std::uint8_t trusted_depth{0};
    std::uint8_t trusted_high_water{0};
    std::uint8_t pending_effect_count{0};
    std::uint8_t transport_request_pending{0};
    std::uint8_t shutdown_pending{0};
    std::uint8_t shutdown_terminal_override_latched{0};
    std::uint8_t critical_pending{0};
    std::uint8_t boot_end_released{0};
    std::uint8_t core_fail_closed_latched{0};
    std::uint8_t core_adapter_fatal_latched{0};
    std::uint8_t sequence_fatal_latched{0};
    std::uint8_t dispatch_fatal_latched{0};
    std::uint8_t safety_delivery_blocked{0};
    std::uint8_t physical_inflight_cancel_pending{0};
};

class RuntimeOwnerAdapterCore;
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
struct RuntimeOwnerAdapterNormalSlotSnapshot {
    NormalIntent intent{};
    std::uint32_t enqueue_sequence{0};
};

struct RuntimeOwnerAdapterTrustedSlotSnapshot {
    std::uint8_t payload_kind{0};
    std::array<std::uint8_t, 3> reserved{};
    std::uint32_t ingress_sequence{0};
    TrustedReceipt receipt{};
    NormalCompletion normal_completion{};
};

struct RuntimeOwnerAdapterPendingEffectSlotSnapshot {
    std::uint32_t preassigned_dispatch_sequence{0};
    RuntimeOwnerEffect effect{};
};

struct RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot {
    std::uint32_t ingress_sequence{0};
    TrustedReceipt receipt{};
};

struct RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot {
    std::uint32_t ingress_sequence{0};
    NormalCompletion completion{};
};

static_assert(
    sizeof(RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot) == 32);
static_assert(
    alignof(RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot) == 4);
static_assert(
    std::is_standard_layout<
        RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot>::value);
static_assert(
    std::is_trivially_copyable<
        RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot>::value);

static_assert(
    sizeof(RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot) == 20);
static_assert(
    alignof(RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot) == 4);
static_assert(
    std::is_standard_layout<
        RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot>::value);
static_assert(
    std::is_trivially_copyable<
        RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot>::value);

static_assert(sizeof(RuntimeOwnerAdapterPendingEffectSlotSnapshot) == 28);
static_assert(alignof(RuntimeOwnerAdapterPendingEffectSlotSnapshot) == 4);
static_assert(
    std::is_standard_layout<
        RuntimeOwnerAdapterPendingEffectSlotSnapshot>::value);
static_assert(
    std::is_trivially_copyable<
        RuntimeOwnerAdapterPendingEffectSlotSnapshot>::value);

static_assert(sizeof(RuntimeOwnerAdapterTrustedSlotSnapshot) == 52);
static_assert(alignof(RuntimeOwnerAdapterTrustedSlotSnapshot) == 4);
static_assert(
    std::is_standard_layout<RuntimeOwnerAdapterTrustedSlotSnapshot>::value);
static_assert(
    std::is_trivially_copyable<RuntimeOwnerAdapterTrustedSlotSnapshot>::value);

struct RuntimeOwnerAdapterPrivateSnapshot {
    RuntimeOwnerView core{};
    AdapterDispatch current_dispatch{};
    AdapterDispatch physical_inflight{};
    std::array<RuntimeOwnerAdapterNormalSlotSnapshot, 8> normal_slots{};
    std::array<RuntimeOwnerAdapterTrustedSlotSnapshot, 8> trusted_slots{};
    std::array<RuntimeOwnerAdapterPendingEffectSlotSnapshot, 4>
        pending_effect_slots{};
    RuntimeOwnerAdapterLastTrustedReceiptSignatureSnapshot
        last_trusted_receipt_signature{};
    RuntimeOwnerAdapterLastNormalCompletionSignatureSnapshot
        last_normal_completion_signature{};
    AdapterCriticalLedger critical{};
    std::uint32_t last_normal_enqueue_sequence{0};
    std::uint32_t last_trusted_ingress_sequence{0};
    std::uint32_t last_dispatch_sequence{0};
    std::uint32_t last_ack_dispatch_sequence{0};
    std::uint32_t last_trusted_diagnostic_ingress_sequence{0};
    std::uint32_t last_trusted_diagnostic_code{0};
    std::uint32_t normal_coalesced_count{0};
    std::uint32_t normal_rejected_full_count{0};
    std::uint32_t normal_cancelled_count{0};
    std::uint32_t dispatch_rejected_ack_count{0};
    std::uint32_t trusted_rejected_full_count{0};
    std::uint32_t trusted_protocol_violation_count{0};
    std::uint32_t trusted_stale_count{0};
    std::uint32_t trusted_duplicate_count{0};
    std::uint32_t trusted_cancelled_count{0};
    std::uint32_t effect_cancelled_count{0};
    std::uint32_t normal_completion_stale_count{0};
    std::uint8_t normal_head{0};
    std::uint8_t normal_tail{0};
    std::uint8_t normal_count{0};
    std::uint8_t normal_high_water{0};
    std::uint8_t trusted_head{0};
    std::uint8_t trusted_tail{0};
    std::uint8_t trusted_count{0};
    std::uint8_t trusted_high_water{0};
    std::uint8_t pending_effect_head{0};
    std::uint8_t pending_effect_tail{0};
    std::uint8_t pending_effect_count{0};
    std::uint8_t accepted_liveness_mask{0};
    bool transport_request_pending{false};
    bool shutdown_pending{false};
    bool shutdown_terminal_override_latched{false};
    bool boot_end_released{false};
    bool critical_pending{false};
    bool core_fail_closed_latched{false};
    bool core_adapter_fatal_latched{false};
    bool sequence_fatal_latched{false};
    bool dispatch_fatal_latched{false};
    bool safety_delivery_blocked{false};
    bool physical_inflight_cancel_pending{false};
};

class RuntimeOwnerAdapterCoreTestPeer;
#endif

class RuntimeOwnerNormalPort {
public:
    RuntimeOwnerNormalPort() = delete;
    RuntimeOwnerNormalPort(const RuntimeOwnerNormalPort &) = delete;
    RuntimeOwnerNormalPort &operator=(const RuntimeOwnerNormalPort &) = delete;
    RuntimeOwnerNormalPort(RuntimeOwnerNormalPort &&) = delete;
    RuntimeOwnerNormalPort &operator=(RuntimeOwnerNormalPort &&) = delete;
    ~RuntimeOwnerNormalPort() noexcept = default;

    [[nodiscard]] NormalSubmitResult submit(NormalIntent input) noexcept;

private:
    friend class RuntimeOwnerAdapterCore;

    explicit RuntimeOwnerNormalPort(RuntimeOwnerAdapterCore *const owner) noexcept
        : owner_(owner)
    {
    }

    RuntimeOwnerAdapterCore *owner_{nullptr};
};

class RuntimeOwnerShutdownPort {
public:
    RuntimeOwnerShutdownPort() = delete;
    RuntimeOwnerShutdownPort(const RuntimeOwnerShutdownPort &) = delete;
    RuntimeOwnerShutdownPort &operator=(const RuntimeOwnerShutdownPort &) = delete;
    RuntimeOwnerShutdownPort(RuntimeOwnerShutdownPort &&) = delete;
    RuntimeOwnerShutdownPort &operator=(RuntimeOwnerShutdownPort &&) = delete;
    ~RuntimeOwnerShutdownPort() noexcept = default;

    [[nodiscard]] UrgentRequestResult request() noexcept;

private:
    friend class RuntimeOwnerAdapterCore;

    explicit RuntimeOwnerShutdownPort(
        RuntimeOwnerAdapterCore *const owner) noexcept
        : owner_(owner)
    {
    }

    RuntimeOwnerAdapterCore *owner_{nullptr};
};

class RuntimeOwnerTrustedReceiptPort {
public:
    RuntimeOwnerTrustedReceiptPort() = delete;
    RuntimeOwnerTrustedReceiptPort(
        const RuntimeOwnerTrustedReceiptPort &) = delete;
    RuntimeOwnerTrustedReceiptPort &operator=(
        const RuntimeOwnerTrustedReceiptPort &) = delete;
    RuntimeOwnerTrustedReceiptPort(
        RuntimeOwnerTrustedReceiptPort &&) = delete;
    RuntimeOwnerTrustedReceiptPort &operator=(
        RuntimeOwnerTrustedReceiptPort &&) = delete;
    ~RuntimeOwnerTrustedReceiptPort() noexcept = default;

    [[nodiscard]] TrustedIngressResult submit(
        TrustedReceipt input) noexcept;

private:
    friend class RuntimeOwnerAdapterCore;

    explicit RuntimeOwnerTrustedReceiptPort(
        RuntimeOwnerAdapterCore *const owner) noexcept
        : owner_(owner)
    {
    }

    RuntimeOwnerAdapterCore *owner_{nullptr};
};

class RuntimeOwnerNormalCompletionPort {
public:
    RuntimeOwnerNormalCompletionPort() = delete;
    RuntimeOwnerNormalCompletionPort(
        const RuntimeOwnerNormalCompletionPort &) = delete;
    RuntimeOwnerNormalCompletionPort &operator=(
        const RuntimeOwnerNormalCompletionPort &) = delete;
    RuntimeOwnerNormalCompletionPort(
        RuntimeOwnerNormalCompletionPort &&) = delete;
    RuntimeOwnerNormalCompletionPort &operator=(
        RuntimeOwnerNormalCompletionPort &&) = delete;
    ~RuntimeOwnerNormalCompletionPort() noexcept = default;

    [[nodiscard]] TrustedIngressResult submit(
        NormalCompletion input) noexcept;

private:
    friend class RuntimeOwnerAdapterCore;

    explicit RuntimeOwnerNormalCompletionPort(
        RuntimeOwnerAdapterCore *const owner) noexcept
        : owner_(owner)
    {
    }

    RuntimeOwnerAdapterCore *owner_{nullptr};
};

class RuntimeOwnerAdapterCore {
public:
    explicit RuntimeOwnerAdapterCore() noexcept = default;
    RuntimeOwnerAdapterCore(const RuntimeOwnerAdapterCore &) = delete;
    RuntimeOwnerAdapterCore &operator=(const RuntimeOwnerAdapterCore &) = delete;
    RuntimeOwnerAdapterCore(RuntimeOwnerAdapterCore &&) = delete;
    RuntimeOwnerAdapterCore &operator=(RuntimeOwnerAdapterCore &&) = delete;
    ~RuntimeOwnerAdapterCore() noexcept = default;

    [[nodiscard]] RuntimeOwnerNormalPort normal_port() & noexcept;
    [[nodiscard]] RuntimeOwnerNormalPort normal_port() && = delete;
    [[nodiscard]] RuntimeOwnerShutdownPort shutdown_port() & noexcept;
    [[nodiscard]] RuntimeOwnerShutdownPort shutdown_port() && = delete;
    [[nodiscard]] RuntimeOwnerTrustedReceiptPort
        trusted_receipt_port() & noexcept;
    [[nodiscard]] RuntimeOwnerTrustedReceiptPort
        trusted_receipt_port() && = delete;
    [[nodiscard]] RuntimeOwnerNormalCompletionPort
        normal_completion_port() & noexcept;
    [[nodiscard]] RuntimeOwnerNormalCompletionPort
        normal_completion_port() && = delete;

    [[nodiscard]] OwnerRequestResult request_transport_attempt() noexcept;
    [[nodiscard]] AdapterStepResult step() noexcept;
    [[nodiscard]] AdapterDispatch peek_dispatch() const noexcept;
    [[nodiscard]] DispatchAckResult acknowledge_dispatch(
        std::uint32_t dispatch_sequence) noexcept;
    [[nodiscard]] RuntimeOwnerAdapterView view() const noexcept;

private:
    friend class RuntimeOwnerNormalPort;
    friend class RuntimeOwnerShutdownPort;
    friend class RuntimeOwnerTrustedReceiptPort;
    friend class RuntimeOwnerNormalCompletionPort;
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
    friend class RuntimeOwnerAdapterCoreTestPeer;
#endif

    enum class TrustedIngressPayloadKind : std::uint8_t {
        None = 0,
        CoreReceipt = 1,
        NormalCompletion = 2,
    };

    enum class MalformedTransitionOrigin : std::uint8_t {
        TransportRequest = 0,
        TrustedHead = 1,
        Critical = 2,
        Shutdown = 3,
    };

    struct TrustedIngressEnvelope {
        TrustedIngressPayloadKind kind{TrustedIngressPayloadKind::None};
        std::array<std::uint8_t, 3> reserved{};
        std::uint32_t ingress_sequence{0};
        TrustedReceipt receipt{};
        NormalCompletion normal_completion{};
    };

    struct PendingEffectSlot {
        std::uint32_t preassigned_dispatch_sequence{0};
        RuntimeOwnerEffect effect{};
    };

    struct LastTrustedReceiptSignature {
        std::uint32_t ingress_sequence{0};
        TrustedReceipt receipt{};
    };

    struct LastNormalCompletionSignature {
        std::uint32_t ingress_sequence{0};
        NormalCompletion completion{};
    };

    struct NormalQueueEntry {
        NormalIntent intent{};
        std::uint32_t enqueue_sequence{0};
    };

    template <typename... Fields>
    struct InternalValueFields
        : std::integral_constant<
              bool,
              ((!std::is_pointer<Fields>::value &&
                !std::is_reference<Fields>::value &&
                std::is_trivially_copyable<Fields>::value) && ...)> {
    };

    static_assert(std::is_same<
                  typename std::underlying_type<
                      TrustedIngressPayloadKind>::type,
                  std::uint8_t>::value);
    static_assert(std::is_same<
                  typename std::underlying_type<
                      MalformedTransitionOrigin>::type,
                  std::uint8_t>::value);

    static_assert(sizeof(TrustedIngressEnvelope) == 52);
    static_assert(alignof(TrustedIngressEnvelope) == 4);
    static_assert(std::is_standard_layout<TrustedIngressEnvelope>::value);
    static_assert(std::is_trivially_copyable<TrustedIngressEnvelope>::value);
    static_assert(InternalValueFields<
                  decltype(TrustedIngressEnvelope::kind),
                  decltype(TrustedIngressEnvelope::reserved),
                  decltype(TrustedIngressEnvelope::ingress_sequence),
                  decltype(TrustedIngressEnvelope::receipt),
                  decltype(TrustedIngressEnvelope::normal_completion)>::value);

    static_assert(sizeof(LastTrustedReceiptSignature) == 32);
    static_assert(alignof(LastTrustedReceiptSignature) == 4);
    static_assert(
        std::is_standard_layout<LastTrustedReceiptSignature>::value);
    static_assert(
        std::is_trivially_copyable<LastTrustedReceiptSignature>::value);
    static_assert(InternalValueFields<
                  decltype(LastTrustedReceiptSignature::ingress_sequence),
                  decltype(LastTrustedReceiptSignature::receipt)>::value);

    static_assert(sizeof(LastNormalCompletionSignature) == 20);
    static_assert(alignof(LastNormalCompletionSignature) == 4);
    static_assert(
        std::is_standard_layout<LastNormalCompletionSignature>::value);
    static_assert(
        std::is_trivially_copyable<LastNormalCompletionSignature>::value);
    static_assert(InternalValueFields<
                  decltype(LastNormalCompletionSignature::ingress_sequence),
                  decltype(LastNormalCompletionSignature::completion)>::value);

    static_assert(sizeof(NormalQueueEntry) == 16);
    static_assert(alignof(NormalQueueEntry) == 4);
    static_assert(std::is_standard_layout<NormalQueueEntry>::value);
    static_assert(std::is_trivially_copyable<NormalQueueEntry>::value);

    static_assert(sizeof(PendingEffectSlot) == 28);
    static_assert(alignof(PendingEffectSlot) == 4);
    static_assert(std::is_standard_layout<PendingEffectSlot>::value);
    static_assert(std::is_trivially_copyable<PendingEffectSlot>::value);
    static_assert(InternalValueFields<
                  decltype(PendingEffectSlot::preassigned_dispatch_sequence),
                  decltype(PendingEffectSlot::effect)>::value);

    [[nodiscard]] static bool normal_intents_have_same_key(
        NormalIntent left,
        NormalIntent right) noexcept;
    [[nodiscard]] static bool trusted_receipt_is_canonical(
        TrustedReceipt input) noexcept;
    [[nodiscard]] static bool normal_completion_is_canonical(
        NormalCompletion input) noexcept;
    [[nodiscard]] bool normal_admission_blocked() const noexcept;
    [[nodiscard]] bool trusted_admission_blocked() const noexcept;
    [[nodiscard]] NormalSubmitResult submit_normal(
        NormalIntent input) noexcept;
    [[nodiscard]] RuntimeOwnerTransition submit_core_input(
        RuntimeOwnerInput input) noexcept;
    [[nodiscard]] RuntimeOwnerView view_after_core_submit() noexcept;
    [[nodiscard]] AdapterStepResult handle_malformed_core_transition(
        MalformedTransitionOrigin origin,
        RuntimeOwnerPhase phase_before,
        RuntimeOwnerPhase observed_phase_after,
        std::uint32_t trusted_ingress_sequence) noexcept;
    void cancel_non_safety_authorization() noexcept;
    void cancel_for_active_critical() noexcept;
    void cancel_for_shutdown() noexcept;
    void quarantine_physical_inflight() noexcept;
    [[nodiscard]] AdapterStepResult shutdown_terminal_step(
        RuntimeOwnerPhase phase) noexcept;
    [[nodiscard]] UrgentRequestResult request_shutdown() noexcept;
    void record_critical(
        AdapterCriticalReason reason,
        std::uint32_t ingress_sequence,
        std::uint32_t diagnostic_code) noexcept;
    [[nodiscard]] TrustedIngressResult enqueue_trusted_receipt(
        TrustedReceipt input) noexcept;
    [[nodiscard]] TrustedIngressResult enqueue_normal_completion(
        NormalCompletion input) noexcept;

    static constexpr std::uint8_t kNormalQueueCapacity = 8;
    static constexpr std::uint8_t kTrustedQueueCapacity = 8;
    static constexpr std::uint8_t kPendingEffectCapacity = 4;

    using TrustedQueueStorage =
        std::array<TrustedIngressEnvelope, kTrustedQueueCapacity>;
    static_assert(sizeof(TrustedQueueStorage) == 416);
    static_assert(alignof(TrustedQueueStorage) == 4);
    static_assert(std::is_standard_layout<TrustedQueueStorage>::value);
    static_assert(std::is_trivially_copyable<TrustedQueueStorage>::value);

    using PendingEffectStorage =
        std::array<PendingEffectSlot, kPendingEffectCapacity>;
    static_assert(sizeof(PendingEffectStorage) == 112);
    static_assert(alignof(PendingEffectStorage) == 4);
    static_assert(std::is_standard_layout<PendingEffectStorage>::value);
    static_assert(std::is_trivially_copyable<PendingEffectStorage>::value);

    RuntimeOwnerCore core_{};
    AdapterDispatch current_dispatch_{};
    AdapterDispatch physical_inflight_{};
    std::array<NormalQueueEntry, kNormalQueueCapacity> normal_queue_{};
    TrustedQueueStorage trusted_queue_{};
    PendingEffectStorage pending_effects_{};
    LastTrustedReceiptSignature last_trusted_receipt_signature_{};
    LastNormalCompletionSignature last_normal_completion_signature_{};
    AdapterCriticalLedger critical_{};
    std::uint32_t last_normal_enqueue_sequence_{0};
    std::uint32_t last_trusted_ingress_sequence_{0};
    std::uint32_t last_dispatch_sequence_{0};
    std::uint32_t last_ack_dispatch_sequence_{0};
    std::uint32_t last_trusted_diagnostic_ingress_sequence_{0};
    std::uint32_t last_trusted_diagnostic_code_{0};
    std::uint32_t normal_coalesced_count_{0};
    std::uint32_t normal_rejected_full_count_{0};
    std::uint32_t normal_cancelled_count_{0};
    std::uint32_t dispatch_rejected_ack_count_{0};
    std::uint32_t trusted_rejected_full_count_{0};
    std::uint32_t trusted_protocol_violation_count_{0};
    std::uint32_t trusted_stale_count_{0};
    std::uint32_t trusted_duplicate_count_{0};
    std::uint32_t trusted_cancelled_count_{0};
    std::uint32_t effect_cancelled_count_{0};
    std::uint32_t normal_completion_stale_count_{0};
    std::uint8_t normal_head_{0};
    std::uint8_t normal_tail_{0};
    std::uint8_t normal_count_{0};
    std::uint8_t normal_high_water_{0};
    std::uint8_t trusted_head_{0};
    std::uint8_t trusted_tail_{0};
    std::uint8_t trusted_count_{0};
    std::uint8_t trusted_high_water_{0};
    std::uint8_t pending_effect_head_{0};
    std::uint8_t pending_effect_tail_{0};
    std::uint8_t pending_effect_count_{0};
    std::uint8_t accepted_liveness_mask_{0};
    bool transport_request_pending_{false};
    bool shutdown_pending_{false};
    bool shutdown_terminal_override_latched_{false};
    bool boot_end_released_{false};
    bool critical_pending_{false};
    bool core_fail_closed_latched_{false};
    bool core_adapter_fatal_latched_{false};
    bool sequence_fatal_latched_{false};
    bool dispatch_fatal_latched_{false};
    bool safety_delivery_blocked_{false};
    bool physical_inflight_cancel_pending_{false};
#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
    RuntimeOwnerTransition core_transition_override_{};
    RuntimeOwnerView core_post_submit_view_override_{};
    std::uint32_t core_submit_count_{0};
    bool core_transition_override_pending_{false};
    bool core_post_submit_view_override_pending_{false};
    bool last_config_validation_bypass_used_{false};
    bool last_operation_completed_validation_bypass_used_{false};
    bool last_liveness_failure_validation_bypass_used_{false};
    bool last_snapshot_validation_bypass_used_{false};
#endif
};

inline RuntimeOwnerNormalPort RuntimeOwnerAdapterCore::normal_port() & noexcept
{
    return RuntimeOwnerNormalPort{this};
}

inline RuntimeOwnerShutdownPort RuntimeOwnerAdapterCore::shutdown_port() & noexcept
{
    return RuntimeOwnerShutdownPort{this};
}

inline RuntimeOwnerTrustedReceiptPort
RuntimeOwnerAdapterCore::trusted_receipt_port() & noexcept
{
    return RuntimeOwnerTrustedReceiptPort{this};
}

inline RuntimeOwnerNormalCompletionPort
RuntimeOwnerAdapterCore::normal_completion_port() & noexcept
{
    return RuntimeOwnerNormalCompletionPort{this};
}

#if defined(NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING)
class RuntimeOwnerAdapterCoreTestPeer {
public:
    using PendingEffectSlot = RuntimeOwnerAdapterCore::PendingEffectSlot;
    using TrustedEnqueueResult = TrustedIngressResult;
    using TrustedIngressPayloadKind =
        RuntimeOwnerAdapterCore::TrustedIngressPayloadKind;
    using TrustedIngressEnvelope =
        RuntimeOwnerAdapterCore::TrustedIngressEnvelope;
    using LastTrustedReceiptSignature =
        RuntimeOwnerAdapterCore::LastTrustedReceiptSignature;
    using LastNormalCompletionSignature =
        RuntimeOwnerAdapterCore::LastNormalCompletionSignature;

    [[nodiscard]] static bool fixture_drive_core_to_runtime_ready(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool fixture_drive_core_to_recovery_pending(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool fixture_drive_core_to_phase(
        RuntimeOwnerAdapterCore &adapter,
        RuntimeOwnerPhase phase) noexcept;
    [[nodiscard]] static bool fixture_drive_core_to_post_boot_recovery(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    static void fixture_prepare_core_awaiting_config(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t session_id,
        std::uint32_t generation,
        std::uint32_t last_config_commit_sequence) noexcept;
    static void fixture_set_core_config_counters(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t config_apply_epoch_counter,
        std::uint32_t correlation_id_counter) noexcept;
    static void fixture_set_core_boot_orchestration_ended(
        RuntimeOwnerAdapterCore &adapter,
        bool ended) noexcept;
    static void fixture_set_boot_end_released(
        RuntimeOwnerAdapterCore &adapter,
        bool released) noexcept;
    static void fixture_commit_core_shutdown(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    static void fixture_set_core_adapter_fatal(
        RuntimeOwnerAdapterCore &adapter,
        bool latched) noexcept;
    static void fixture_set_core_fail_closed(
        RuntimeOwnerAdapterCore &adapter,
        bool latched) noexcept;
    static void fixture_set_sequence_fatal(
        RuntimeOwnerAdapterCore &adapter,
        bool latched) noexcept;
    static void fixture_set_dispatch_fatal(
        RuntimeOwnerAdapterCore &adapter,
        bool latched) noexcept;
    static void fixture_set_safety_delivery_blocked(
        RuntimeOwnerAdapterCore &adapter,
        bool blocked) noexcept;
    static void fixture_set_shutdown_terminal_override(
        RuntimeOwnerAdapterCore &adapter,
        bool latched) noexcept;
    static void fixture_set_last_normal_enqueue_sequence(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t sequence) noexcept;
    static void fixture_set_normal_diagnostic_counts(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t coalesced,
        std::uint32_t rejected_full) noexcept;
    static void fixture_set_last_trusted_ingress_sequence(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t sequence) noexcept;
    static void fixture_set_last_trusted_receipt_signature(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t ingress_sequence,
        TrustedReceipt receipt) noexcept;
    static void fixture_set_last_dispatch_sequence(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t sequence) noexcept;
    static void fixture_clear_pending_effects(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    static void fixture_set_accepted_liveness_mask(
        RuntimeOwnerAdapterCore &adapter,
        std::uint8_t mask) noexcept;
    static void fixture_set_trusted_diagnostic_counts(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t rejected_full,
        std::uint32_t protocol_violation) noexcept;
    static void fixture_set_critical_occurrence_count(
        RuntimeOwnerAdapterCore &adapter,
        std::uint32_t occurrence_count) noexcept;
    static void fixture_override_next_core_transition(
        RuntimeOwnerAdapterCore &adapter,
        RuntimeOwnerTransition transition) noexcept;
    static void fixture_override_next_core_post_submit_view(
        RuntimeOwnerAdapterCore &adapter,
        RuntimeOwnerView view) noexcept;
    static void fixture_seed_begin_fallback_cleanup_state(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    static void fixture_seed_trusted_fallback_nonqueue_state(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    static void fixture_seed_authorization_pending_effect(
        RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static std::uint32_t fixture_core_submit_count(
        const RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool fixture_core_transition_override_pending(
        const RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool
        fixture_core_post_submit_view_override_pending(
            const RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool
        fixture_last_config_validation_bypass_used(
            const RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool
        fixture_last_operation_completed_validation_bypass_used(
            const RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool
        fixture_last_liveness_failure_validation_bypass_used(
            const RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static bool
        fixture_last_snapshot_validation_bypass_used(
            const RuntimeOwnerAdapterCore &adapter) noexcept;
    [[nodiscard]] static TrustedEnqueueResult enqueue_trusted_receipt(
        RuntimeOwnerAdapterCore &adapter,
        TrustedReceipt input) noexcept;
    [[nodiscard]] static TrustedEnqueueResult enqueue_normal_completion(
        RuntimeOwnerAdapterCore &adapter,
        NormalCompletion input) noexcept;
    [[nodiscard]] static bool fixture_enqueue_trusted_receipt_unchecked(
        RuntimeOwnerAdapterCore &adapter,
        TrustedReceipt input) noexcept;
    [[nodiscard]] static bool fixture_consume_normal(
        RuntimeOwnerAdapterCore &adapter,
        NormalIntent &intent,
        std::uint32_t &enqueue_sequence) noexcept;
    [[nodiscard]] static bool fixture_consume_trusted(
        RuntimeOwnerAdapterCore &adapter,
        TrustedIngressEnvelope &envelope) noexcept;
    [[nodiscard]] static RuntimeOwnerAdapterPrivateSnapshot snapshot(
        const RuntimeOwnerAdapterCore &adapter) noexcept;
};
#endif

namespace runtime_owner_adapter_detail {

template <typename Type, std::size_t ExpectedSize, std::size_t ExpectedAlign>
constexpr bool has_fixed_dto_contract =
    sizeof(Type) == ExpectedSize &&
    alignof(Type) == ExpectedAlign &&
    std::is_standard_layout<Type>::value &&
    std::is_trivially_copyable<Type>::value;

template <typename... Fields>
constexpr bool has_only_nonowning_value_fields =
    ((!std::is_pointer<Fields>::value &&
      !std::is_reference<Fields>::value &&
      std::is_trivially_copyable<Fields>::value) && ...);

template <typename Enum>
constexpr bool has_uint8_underlying_type =
    std::is_same<typename std::underlying_type<Enum>::type,
                 std::uint8_t>::value;

static_assert(has_uint8_underlying_type<NormalSubmitResult>);
static_assert(has_uint8_underlying_type<TrustedIngressResult>);
static_assert(has_uint8_underlying_type<UrgentRequestResult>);
static_assert(has_uint8_underlying_type<OwnerRequestResult>);
static_assert(has_uint8_underlying_type<DispatchAckResult>);
static_assert(has_uint8_underlying_type<AdapterStepAction>);
static_assert(has_uint8_underlying_type<NormalIntentKind>);
static_assert(has_uint8_underlying_type<TrustedReceiptKind>);
static_assert(has_uint8_underlying_type<NormalCompletionKind>);
static_assert(has_uint8_underlying_type<AdapterDispatchKind>);
static_assert(has_uint8_underlying_type<AdapterCriticalReason>);

static_assert(has_fixed_dto_contract<AdapterStepResult, 16, 4>);
static_assert(has_fixed_dto_contract<NormalIntent, 12, 4>);
static_assert(has_fixed_dto_contract<TrustedReceipt, 28, 4>);
static_assert(has_fixed_dto_contract<NormalCompletion, 16, 4>);
static_assert(has_fixed_dto_contract<AdapterDispatch, 48, 4>);
static_assert(has_fixed_dto_contract<AdapterCriticalLedger, 28, 4>);
static_assert(has_fixed_dto_contract<RuntimeOwnerAdapterView, 252, 4>);

static_assert(has_only_nonowning_value_fields<
              decltype(AdapterStepResult::action),
              decltype(AdapterStepResult::core_disposition),
              decltype(AdapterStepResult::phase_before),
              decltype(AdapterStepResult::phase_after),
              decltype(AdapterStepResult::consumed_ingress_sequence),
              decltype(AdapterStepResult::consumed_enqueue_sequence),
              decltype(AdapterStepResult::prepared_dispatch_sequence)>);

static_assert(has_only_nonowning_value_fields<
              decltype(NormalIntent::kind),
              decltype(NormalIntent::flags),
              decltype(NormalIntent::reserved),
              decltype(NormalIntent::subject_id),
              decltype(NormalIntent::snapshot_revision)>);

static_assert(has_only_nonowning_value_fields<
              decltype(TrustedReceipt::kind),
              decltype(TrustedReceipt::effect_kind),
              decltype(TrustedReceipt::reserved),
              decltype(TrustedReceipt::correlation_id),
              decltype(TrustedReceipt::mqtt_session_id),
              decltype(TrustedReceipt::mqtt_generation),
              decltype(TrustedReceipt::config_commit_sequence),
              decltype(TrustedReceipt::config_apply_epoch),
              decltype(TrustedReceipt::diagnostic_code)>);

static_assert(has_only_nonowning_value_fields<
              decltype(NormalCompletion::kind),
              decltype(NormalCompletion::reserved),
              decltype(NormalCompletion::dispatch_sequence),
              decltype(NormalCompletion::enqueue_sequence),
              decltype(NormalCompletion::diagnostic_code)>);

static_assert(has_only_nonowning_value_fields<
              decltype(AdapterDispatch::kind),
              decltype(AdapterDispatch::reserved),
              decltype(AdapterDispatch::dispatch_sequence),
              decltype(AdapterDispatch::enqueue_sequence),
              decltype(AdapterDispatch::effect),
              decltype(AdapterDispatch::normal_intent)>);

static_assert(has_only_nonowning_value_fields<
              decltype(AdapterCriticalLedger::first_reason),
              decltype(AdapterCriticalLedger::last_reason),
              decltype(AdapterCriticalLedger::reserved),
              decltype(AdapterCriticalLedger::reason_mask),
              decltype(AdapterCriticalLedger::first_ingress_sequence),
              decltype(AdapterCriticalLedger::last_ingress_sequence),
              decltype(AdapterCriticalLedger::first_diagnostic_code),
              decltype(AdapterCriticalLedger::last_diagnostic_code),
              decltype(AdapterCriticalLedger::occurrence_count)>);

static_assert(has_only_nonowning_value_fields<
              decltype(RuntimeOwnerAdapterView::core),
              decltype(RuntimeOwnerAdapterView::current_dispatch),
              decltype(RuntimeOwnerAdapterView::physical_inflight),
              decltype(RuntimeOwnerAdapterView::critical),
              decltype(RuntimeOwnerAdapterView::last_normal_enqueue_sequence),
              decltype(RuntimeOwnerAdapterView::last_trusted_ingress_sequence),
              decltype(RuntimeOwnerAdapterView::last_dispatch_sequence),
              decltype(RuntimeOwnerAdapterView::last_ack_dispatch_sequence),
              decltype(RuntimeOwnerAdapterView::last_trusted_diagnostic_ingress_sequence),
              decltype(RuntimeOwnerAdapterView::last_trusted_diagnostic_code),
              decltype(RuntimeOwnerAdapterView::normal_coalesced_count),
              decltype(RuntimeOwnerAdapterView::normal_rejected_full_count),
              decltype(RuntimeOwnerAdapterView::normal_cancelled_count),
              decltype(RuntimeOwnerAdapterView::trusted_rejected_full_count),
              decltype(RuntimeOwnerAdapterView::trusted_protocol_violation_count),
              decltype(RuntimeOwnerAdapterView::trusted_stale_count),
              decltype(RuntimeOwnerAdapterView::trusted_duplicate_count),
              decltype(RuntimeOwnerAdapterView::trusted_cancelled_count),
              decltype(RuntimeOwnerAdapterView::effect_cancelled_count),
              decltype(RuntimeOwnerAdapterView::dispatch_rejected_ack_count),
              decltype(RuntimeOwnerAdapterView::normal_completion_stale_count),
              decltype(RuntimeOwnerAdapterView::normal_depth),
              decltype(RuntimeOwnerAdapterView::normal_high_water),
              decltype(RuntimeOwnerAdapterView::trusted_depth),
              decltype(RuntimeOwnerAdapterView::trusted_high_water),
              decltype(RuntimeOwnerAdapterView::pending_effect_count),
              decltype(RuntimeOwnerAdapterView::transport_request_pending),
              decltype(RuntimeOwnerAdapterView::shutdown_pending),
              decltype(RuntimeOwnerAdapterView::shutdown_terminal_override_latched),
              decltype(RuntimeOwnerAdapterView::critical_pending),
              decltype(RuntimeOwnerAdapterView::boot_end_released),
              decltype(RuntimeOwnerAdapterView::core_fail_closed_latched),
              decltype(RuntimeOwnerAdapterView::core_adapter_fatal_latched),
              decltype(RuntimeOwnerAdapterView::sequence_fatal_latched),
              decltype(RuntimeOwnerAdapterView::dispatch_fatal_latched),
              decltype(RuntimeOwnerAdapterView::safety_delivery_blocked),
              decltype(RuntimeOwnerAdapterView::physical_inflight_cancel_pending)>);

} // namespace runtime_owner_adapter_detail

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_ADAPTER_CORE_HPP
