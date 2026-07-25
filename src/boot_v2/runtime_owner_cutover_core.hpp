#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_CUTOVER_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_CUTOVER_CORE_HPP

#include <cstdint>
#include <type_traits>

#include "runtime_owner_rtos_drain_core.hpp"

namespace boot_v2 {

enum class RuntimeOwnerCutoverState : std::uint8_t {
    Dormant = 0,
    Prepared = 1,
    Committed = 2,
    AbortedPreAdmission = 3,
    CleanRebootRequired = 4,
};

enum class RuntimeOwnerCutoverResult : std::uint8_t {
    RejectedInvalid = 0,
    RejectedIncomplete = 1,
    Prepared = 2,
    Committed = 3,
    AcceptedDuplicate = 4,
    AbortedPreAdmission = 5,
    CleanRebootRequired = 6,
};

struct RuntimeOwnerCutoverView {
    RuntimeOwnerCutoverState state{RuntimeOwnerCutoverState::Dormant};
    std::uint8_t reserved[3]{};
    std::uint32_t stable_identity{0};
    std::uint32_t transition_sequence{0};
};

class RuntimeOwnerCutoverCore {
public:
    RuntimeOwnerCutoverCore() noexcept = default;
    RuntimeOwnerCutoverCore(const RuntimeOwnerCutoverCore &) = delete;
    RuntimeOwnerCutoverCore &operator=(const RuntimeOwnerCutoverCore &) = delete;
    RuntimeOwnerCutoverCore(RuntimeOwnerCutoverCore &&) = delete;
    RuntimeOwnerCutoverCore &operator=(RuntimeOwnerCutoverCore &&) = delete;
    ~RuntimeOwnerCutoverCore() noexcept = default;

    [[nodiscard]] RuntimeOwnerCutoverResult prepare(
        RuntimeOwnerActivationFacts facts,
        std::uint32_t stable_identity) noexcept;
    [[nodiscard]] RuntimeOwnerCutoverResult commit(
        std::uint32_t stable_identity) noexcept;
    [[nodiscard]] RuntimeOwnerCutoverResult fail(
        bool ingress_enabled) noexcept;
    [[nodiscard]] RuntimeOwnerCutoverView view() const noexcept;

private:
    RuntimeOwnerCutoverView view_{};
    RuntimeOwnerActivationFacts prepared_facts_{};
};

namespace runtime_owner_cutover_contract_detail {

static_assert(std::is_same<
              typename std::underlying_type<RuntimeOwnerCutoverState>::type,
              std::uint8_t>::value);
static_assert(std::is_same<
              typename std::underlying_type<RuntimeOwnerCutoverResult>::type,
              std::uint8_t>::value);
static_assert(sizeof(RuntimeOwnerCutoverView) == 12);
static_assert(alignof(RuntimeOwnerCutoverView) == 4);
static_assert(std::is_standard_layout<RuntimeOwnerCutoverView>::value);
static_assert(std::is_trivially_copyable<RuntimeOwnerCutoverView>::value);

} // namespace runtime_owner_cutover_contract_detail

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_RUNTIME_OWNER_CUTOVER_CORE_HPP
