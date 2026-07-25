#include "runtime_owner_cutover_core.hpp"

#include <limits>

namespace boot_v2 {
namespace {

bool facts_equal(
    const RuntimeOwnerActivationFacts left,
    const RuntimeOwnerActivationFacts right) noexcept
{
    return left.queue_drain_ready == right.queue_drain_ready &&
           left.physical_executor_ready == right.physical_executor_ready &&
           left.producers_quiesced == right.producers_quiesced &&
           left.legacy_direct_access_disabled ==
               right.legacy_direct_access_disabled &&
           left.rollback_ready == right.rollback_ready &&
           left.reserved[0] == right.reserved[0] &&
           left.reserved[1] == right.reserved[1] &&
           left.reserved[2] == right.reserved[2];
}

bool advance_sequence(RuntimeOwnerCutoverView &view) noexcept
{
    if (view.transition_sequence ==
        std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    ++view.transition_sequence;
    return true;
}

} // namespace

RuntimeOwnerCutoverResult RuntimeOwnerCutoverCore::prepare(
    const RuntimeOwnerActivationFacts facts,
    const std::uint32_t stable_identity) noexcept
{
    const RuntimeOwnerActivationPreflightResult preflight =
        runtime_owner_activation_preflight(facts, stable_identity);
    if (preflight == RuntimeOwnerActivationPreflightResult::RejectedInvalid) {
        return RuntimeOwnerCutoverResult::RejectedInvalid;
    }
    if (preflight ==
        RuntimeOwnerActivationPreflightResult::RejectedIncomplete) {
        return RuntimeOwnerCutoverResult::RejectedIncomplete;
    }
    if (view_.state == RuntimeOwnerCutoverState::Prepared) {
        return view_.stable_identity == stable_identity &&
                       facts_equal(prepared_facts_, facts)
                   ? RuntimeOwnerCutoverResult::AcceptedDuplicate
                   : RuntimeOwnerCutoverResult::RejectedInvalid;
    }
    if (view_.state != RuntimeOwnerCutoverState::Dormant ||
        !advance_sequence(view_)) {
        return RuntimeOwnerCutoverResult::RejectedInvalid;
    }
    prepared_facts_ = facts;
    view_.stable_identity = stable_identity;
    view_.state = RuntimeOwnerCutoverState::Prepared;
    return RuntimeOwnerCutoverResult::Prepared;
}

RuntimeOwnerCutoverResult RuntimeOwnerCutoverCore::commit(
    const std::uint32_t stable_identity) noexcept
{
    if (stable_identity == 0 || stable_identity != view_.stable_identity) {
        return RuntimeOwnerCutoverResult::RejectedInvalid;
    }
    if (view_.state == RuntimeOwnerCutoverState::Committed) {
        return RuntimeOwnerCutoverResult::AcceptedDuplicate;
    }
    if (view_.state != RuntimeOwnerCutoverState::Prepared ||
        !advance_sequence(view_)) {
        return RuntimeOwnerCutoverResult::RejectedInvalid;
    }
    view_.state = RuntimeOwnerCutoverState::Committed;
    return RuntimeOwnerCutoverResult::Committed;
}

RuntimeOwnerCutoverResult RuntimeOwnerCutoverCore::fail(
    const bool ingress_enabled) noexcept
{
    if (!ingress_enabled) {
        if (view_.state == RuntimeOwnerCutoverState::AbortedPreAdmission) {
            return RuntimeOwnerCutoverResult::AcceptedDuplicate;
        }
        if (view_.state != RuntimeOwnerCutoverState::Prepared ||
            !advance_sequence(view_)) {
            return RuntimeOwnerCutoverResult::RejectedInvalid;
        }
        view_.state = RuntimeOwnerCutoverState::AbortedPreAdmission;
        return RuntimeOwnerCutoverResult::AbortedPreAdmission;
    }

    if (view_.state == RuntimeOwnerCutoverState::CleanRebootRequired) {
        return RuntimeOwnerCutoverResult::AcceptedDuplicate;
    }
    if (view_.state != RuntimeOwnerCutoverState::Committed ||
        !advance_sequence(view_)) {
        return RuntimeOwnerCutoverResult::RejectedInvalid;
    }
    view_.state = RuntimeOwnerCutoverState::CleanRebootRequired;
    return RuntimeOwnerCutoverResult::CleanRebootRequired;
}

RuntimeOwnerCutoverView RuntimeOwnerCutoverCore::view() const noexcept
{
    return view_;
}

} // namespace boot_v2
