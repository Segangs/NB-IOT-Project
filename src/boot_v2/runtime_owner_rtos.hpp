#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_RTOS_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_RTOS_HPP

#include <cstdint>
#include <type_traits>

#include "runtime_owner_task_core.hpp"

namespace boot_v2 {

enum class RuntimeOwnerAtomicCutoverResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedNotStarted = 1,
    RejectedNotQuiesced = 2,
    RejectedNotReady = 3,
    Activated = 4,
    AlreadyActive = 5,
};

struct RuntimeOwnerRtosDrainMetrics {
    std::uint32_t urgent_processed_count{0};
    std::uint32_t control_processed_count{0};
    std::uint32_t normal_processed_count{0};
    std::uint32_t dropped_invalid_count{0};
    std::uint32_t receive_fault_count{0};
    std::uint8_t receiver_ready{0};
    std::uint8_t cutover_ready{0};
    std::uint8_t reserved[2]{};
};

static_assert(sizeof(RuntimeOwnerRtosDrainMetrics) == 24);
static_assert(alignof(RuntimeOwnerRtosDrainMetrics) == 4);
static_assert(std::is_standard_layout<RuntimeOwnerRtosDrainMetrics>::value);
static_assert(
    std::is_trivially_copyable<RuntimeOwnerRtosDrainMetrics>::value);

[[nodiscard]] RuntimeOwnerStartResult runtime_owner_rtos_start() noexcept;
[[nodiscard]] RuntimeOwnerAtomicCutoverResult
runtime_owner_rtos_activate_atomic() noexcept;
[[nodiscard]] RuntimeOwnerRtosStatus runtime_owner_rtos_status() noexcept;
[[nodiscard]] RuntimeOwnerRedactedStatus
runtime_owner_redacted_status() noexcept;
[[nodiscard]] RuntimeOwnerRtosDrainMetrics
runtime_owner_rtos_drain_metrics() noexcept;

}  // namespace boot_v2

#endif
