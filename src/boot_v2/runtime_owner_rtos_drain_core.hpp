#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_RTOS_DRAIN_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_RTOS_DRAIN_CORE_HPP

#include <cstdint>
#include <limits>
#include <type_traits>

#include "runtime_owner_rtos.hpp"

namespace boot_v2 {

enum class RuntimeOwnerRtosLane : std::uint8_t {
    None = 0,
    Urgent = 1,
    Control = 2,
    Normal = 3,
};

enum class RuntimeOwnerQueueReadResult : std::uint8_t {
    Empty = 0,
    Received = 1,
    Fault = 2,
};

enum class RuntimeOwnerDrainResult : std::uint8_t {
    Empty = 0,
    Processed = 1,
    DroppedInvalid = 2,
    Fault = 3,
};

enum class RuntimeOwnerDrainConsumeResult : std::uint8_t {
    Processed = 0,
    DroppedInvalid = 1,
};

enum class RuntimeOwnerUrgentSource : std::uint8_t {
    Invalid = 0,
    PowerButton = 1,
    AdapterLossCommitted = 2,
    AuthenticatedRemoteCommand = 3,
};

enum class RuntimeOwnerActivationPreflightResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedIncomplete = 1,
    Ready = 2,
};

enum class RuntimeOwnerStopPreflightResult : std::uint8_t {
    RejectedInvalid = 0,
    SafePreAdmissionAbort = 1,
    RequiresCleanReboot = 2,
};

struct RuntimeOwnerUrgentMessage {
    RuntimeOwnerUrgentSource source{RuntimeOwnerUrgentSource::Invalid};
    std::uint8_t reserved[3]{};
    std::uint32_t producer_sequence{0};
    std::uint32_t incident_correlation_id{0};
};

struct RuntimeOwnerDrainStep {
    RuntimeOwnerDrainResult result{RuntimeOwnerDrainResult::Empty};
    RuntimeOwnerRtosLane lane{RuntimeOwnerRtosLane::None};
    std::uint8_t reserved[2]{};
};

struct RuntimeOwnerActivationFacts {
    std::uint8_t queue_drain_ready{0};
    std::uint8_t physical_executor_ready{0};
    std::uint8_t producers_quiesced{0};
    std::uint8_t legacy_direct_access_disabled{0};
    std::uint8_t rollback_ready{0};
    std::uint8_t reserved[3]{};
};

struct RuntimeOwnerStopFacts {
    std::uint8_t ingress_enabled{0};
    std::uint8_t work_started{0};
    std::uint8_t queues_empty{0};
    std::uint8_t physical_inflight_idle{0};
    std::uint8_t reserved[4]{};
};

static_assert(sizeof(RuntimeOwnerUrgentMessage) == 12);
static_assert(alignof(RuntimeOwnerUrgentMessage) == 4);
static_assert(sizeof(RuntimeOwnerDrainStep) == 4);
static_assert(alignof(RuntimeOwnerDrainStep) == 1);
static_assert(sizeof(RuntimeOwnerActivationFacts) == 8);
static_assert(alignof(RuntimeOwnerActivationFacts) == 1);
static_assert(sizeof(RuntimeOwnerStopFacts) == 8);
static_assert(alignof(RuntimeOwnerStopFacts) == 1);
static_assert(std::is_standard_layout<RuntimeOwnerUrgentMessage>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerUrgentMessage>::value);
static_assert(std::is_standard_layout<RuntimeOwnerDrainStep>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerDrainStep>::value);
static_assert(std::is_standard_layout<RuntimeOwnerActivationFacts>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerActivationFacts>::value);
static_assert(std::is_standard_layout<RuntimeOwnerStopFacts>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerStopFacts>::value);

[[nodiscard]] constexpr bool runtime_owner_is_canonical_bit(
    const std::uint8_t value) noexcept
{
    return value <= 1;
}

[[nodiscard]] constexpr bool runtime_owner_is_canonical_urgent(
    const RuntimeOwnerUrgentMessage input) noexcept
{
    const bool source_valid =
        input.source == RuntimeOwnerUrgentSource::PowerButton ||
        input.source == RuntimeOwnerUrgentSource::AdapterLossCommitted ||
        input.source == RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand;
    return source_valid && input.reserved[0] == 0 &&
           input.reserved[1] == 0 && input.reserved[2] == 0 &&
           input.producer_sequence != 0 &&
           input.incident_correlation_id != 0;
}

[[nodiscard]] constexpr bool runtime_owner_is_canonical_control(
    const RuntimeOwnerControlMessage input) noexcept
{
    return input.kind == RuntimeOwnerControlKind::RequestTransportAttempt &&
           input.reserved[0] == 0 && input.reserved[1] == 0 &&
           input.reserved[2] == 0;
}

[[nodiscard]] constexpr RuntimeOwnerActivationPreflightResult
runtime_owner_activation_preflight(
    const RuntimeOwnerActivationFacts facts,
    const std::uint32_t stable_identity) noexcept
{
    const bool canonical =
        runtime_owner_is_canonical_bit(facts.queue_drain_ready) &&
        runtime_owner_is_canonical_bit(facts.physical_executor_ready) &&
        runtime_owner_is_canonical_bit(facts.producers_quiesced) &&
        runtime_owner_is_canonical_bit(
            facts.legacy_direct_access_disabled) &&
        runtime_owner_is_canonical_bit(facts.rollback_ready) &&
        facts.reserved[0] == 0 && facts.reserved[1] == 0 &&
        facts.reserved[2] == 0;
    if (!canonical || stable_identity == 0) {
        return RuntimeOwnerActivationPreflightResult::RejectedInvalid;
    }
    const bool ready = facts.queue_drain_ready != 0 &&
                       facts.physical_executor_ready != 0 &&
                       facts.producers_quiesced != 0 &&
                       facts.legacy_direct_access_disabled != 0 &&
                       facts.rollback_ready != 0;
    return ready ? RuntimeOwnerActivationPreflightResult::Ready
                 : RuntimeOwnerActivationPreflightResult::RejectedIncomplete;
}

[[nodiscard]] constexpr RuntimeOwnerStopPreflightResult
runtime_owner_stop_preflight(const RuntimeOwnerStopFacts facts) noexcept
{
    const bool canonical =
        runtime_owner_is_canonical_bit(facts.ingress_enabled) &&
        runtime_owner_is_canonical_bit(facts.work_started) &&
        runtime_owner_is_canonical_bit(facts.queues_empty) &&
        runtime_owner_is_canonical_bit(facts.physical_inflight_idle) &&
        facts.reserved[0] == 0 && facts.reserved[1] == 0 &&
        facts.reserved[2] == 0 && facts.reserved[3] == 0;
    if (!canonical) {
        return RuntimeOwnerStopPreflightResult::RejectedInvalid;
    }
    const bool safe = facts.ingress_enabled == 0 &&
                      facts.work_started == 0 && facts.queues_empty != 0 &&
                      facts.physical_inflight_idle != 0;
    return safe ? RuntimeOwnerStopPreflightResult::SafePreAdmissionAbort
                : RuntimeOwnerStopPreflightResult::RequiresCleanReboot;
}

constexpr void runtime_owner_increment_saturating(
    std::uint32_t &counter) noexcept
{
    if (counter != std::numeric_limits<std::uint32_t>::max()) {
        ++counter;
    }
}

constexpr void runtime_owner_record_drain_step(
    RuntimeOwnerRtosDrainMetrics &metrics,
    const RuntimeOwnerDrainStep step) noexcept
{
    if (step.result == RuntimeOwnerDrainResult::Processed) {
        if (step.lane == RuntimeOwnerRtosLane::Urgent) {
            runtime_owner_increment_saturating(
                metrics.urgent_processed_count);
        } else if (step.lane == RuntimeOwnerRtosLane::Control) {
            runtime_owner_increment_saturating(
                metrics.control_processed_count);
        } else if (step.lane == RuntimeOwnerRtosLane::Normal) {
            runtime_owner_increment_saturating(
                metrics.normal_processed_count);
        }
    } else if (step.result == RuntimeOwnerDrainResult::DroppedInvalid) {
        runtime_owner_increment_saturating(metrics.dropped_invalid_count);
    } else if (step.result == RuntimeOwnerDrainResult::Fault) {
        runtime_owner_increment_saturating(metrics.receive_fault_count);
    }
}

[[nodiscard]] constexpr RuntimeOwnerDrainResult runtime_owner_map_consume(
    const RuntimeOwnerDrainConsumeResult result) noexcept
{
    return result == RuntimeOwnerDrainConsumeResult::Processed
               ? RuntimeOwnerDrainResult::Processed
               : RuntimeOwnerDrainResult::DroppedInvalid;
}

template <typename Backend, typename Sink>
[[nodiscard]] RuntimeOwnerDrainStep runtime_owner_drain_once(
    Backend &backend,
    Sink &sink) noexcept
{
    RuntimeOwnerUrgentMessage urgent{};
    const RuntimeOwnerQueueReadResult urgent_read =
        backend.receive_urgent(urgent);
    if (urgent_read == RuntimeOwnerQueueReadResult::Fault) {
        return {RuntimeOwnerDrainResult::Fault,
                RuntimeOwnerRtosLane::Urgent,
                {}};
    }
    if (urgent_read == RuntimeOwnerQueueReadResult::Received) {
        return {runtime_owner_map_consume(sink.consume_urgent(urgent)),
                RuntimeOwnerRtosLane::Urgent,
                {}};
    }

    RuntimeOwnerControlMessage control{};
    const RuntimeOwnerQueueReadResult control_read =
        backend.receive_control(control);
    if (control_read == RuntimeOwnerQueueReadResult::Fault) {
        return {RuntimeOwnerDrainResult::Fault,
                RuntimeOwnerRtosLane::Control,
                {}};
    }
    if (control_read == RuntimeOwnerQueueReadResult::Received) {
        return {runtime_owner_map_consume(sink.consume_control(control)),
                RuntimeOwnerRtosLane::Control,
                {}};
    }

    NormalIntent normal{};
    const RuntimeOwnerQueueReadResult normal_read =
        backend.receive_normal(normal);
    if (normal_read == RuntimeOwnerQueueReadResult::Fault) {
        return {RuntimeOwnerDrainResult::Fault,
                RuntimeOwnerRtosLane::Normal,
                {}};
    }
    if (normal_read == RuntimeOwnerQueueReadResult::Received) {
        return {runtime_owner_map_consume(sink.consume_normal(normal)),
                RuntimeOwnerRtosLane::Normal,
                {}};
    }

    return {};
}

template <typename Backend, typename Sink, typename Recorder>
[[nodiscard]] RuntimeOwnerDrainStep runtime_owner_drain_until_idle(
    Backend &backend,
    Sink &sink,
    Recorder &&recorder) noexcept
{
    for (;;) {
        const RuntimeOwnerDrainStep step =
            runtime_owner_drain_once(backend, sink);
        recorder(step);
        if (step.result == RuntimeOwnerDrainResult::Empty ||
            step.result == RuntimeOwnerDrainResult::Fault) {
            return step;
        }
    }
}

}  // namespace boot_v2

#endif
