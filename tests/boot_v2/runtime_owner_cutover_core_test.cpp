#include "runtime_owner_cutover_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(const bool condition, const char *expression, const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, line,
                     expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

using namespace boot_v2;

constexpr RuntimeOwnerActivationFacts ready_facts() noexcept
{
    return {1, 1, 1, 1, 1, {}};
}

void test_numeric_and_layout_contracts() noexcept
{
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerCutoverState::Dormant) == 0);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerCutoverState::Prepared) == 1);
    CHECK(static_cast<std::uint8_t>(RuntimeOwnerCutoverState::Committed) == 2);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerCutoverState::AbortedPreAdmission) == 3);
    CHECK(static_cast<std::uint8_t>(
              RuntimeOwnerCutoverState::CleanRebootRequired) == 4);
    CHECK(std::is_standard_layout<RuntimeOwnerCutoverView>::value);
    CHECK(std::is_trivially_copyable<RuntimeOwnerCutoverView>::value);
    CHECK(sizeof(RuntimeOwnerCutoverView) == 12);
    CHECK(alignof(RuntimeOwnerCutoverView) == 4);
}

void test_prepare_requires_exact_readiness() noexcept
{
    for (std::uint8_t index = 0; index < 5; ++index) {
        RuntimeOwnerActivationFacts facts = ready_facts();
        reinterpret_cast<std::uint8_t *>(&facts)[index] = 0;
        RuntimeOwnerCutoverCore core{};
        CHECK(core.prepare(facts, 17) ==
              RuntimeOwnerCutoverResult::RejectedIncomplete);
        CHECK(core.view().state == RuntimeOwnerCutoverState::Dormant);
    }

    RuntimeOwnerCutoverCore zero_identity{};
    CHECK(zero_identity.prepare(ready_facts(), 0) ==
          RuntimeOwnerCutoverResult::RejectedInvalid);

    RuntimeOwnerActivationFacts invalid_bit = ready_facts();
    invalid_bit.physical_executor_ready = 2;
    RuntimeOwnerCutoverCore invalid_bit_core{};
    CHECK(invalid_bit_core.prepare(invalid_bit, 17) ==
          RuntimeOwnerCutoverResult::RejectedInvalid);

    RuntimeOwnerActivationFacts dirty = ready_facts();
    dirty.reserved[1] = 1;
    RuntimeOwnerCutoverCore dirty_core{};
    CHECK(dirty_core.prepare(dirty, 17) ==
          RuntimeOwnerCutoverResult::RejectedInvalid);
}

void test_prepare_commit_and_duplicates() noexcept
{
    RuntimeOwnerCutoverCore core{};
    CHECK(core.prepare(ready_facts(), 41) ==
          RuntimeOwnerCutoverResult::Prepared);
    CHECK(core.view().state == RuntimeOwnerCutoverState::Prepared);
    CHECK(core.view().stable_identity == 41);
    CHECK(core.view().transition_sequence == 1);
    CHECK(core.prepare(ready_facts(), 41) ==
          RuntimeOwnerCutoverResult::AcceptedDuplicate);
    CHECK(core.prepare(ready_facts(), 42) ==
          RuntimeOwnerCutoverResult::RejectedInvalid);
    CHECK(core.commit(42) == RuntimeOwnerCutoverResult::RejectedInvalid);
    CHECK(core.commit(41) == RuntimeOwnerCutoverResult::Committed);
    CHECK(core.view().state == RuntimeOwnerCutoverState::Committed);
    CHECK(core.view().transition_sequence == 2);
    CHECK(core.commit(41) == RuntimeOwnerCutoverResult::AcceptedDuplicate);
    CHECK(core.commit(42) == RuntimeOwnerCutoverResult::RejectedInvalid);
}

void test_failure_boundary_is_sticky() noexcept
{
    RuntimeOwnerCutoverCore pre{};
    CHECK(pre.prepare(ready_facts(), 51) ==
          RuntimeOwnerCutoverResult::Prepared);
    CHECK(pre.fail(false) ==
          RuntimeOwnerCutoverResult::AbortedPreAdmission);
    CHECK(pre.view().state ==
          RuntimeOwnerCutoverState::AbortedPreAdmission);
    CHECK(pre.fail(false) ==
          RuntimeOwnerCutoverResult::AcceptedDuplicate);
    CHECK(pre.fail(true) == RuntimeOwnerCutoverResult::RejectedInvalid);
    CHECK(pre.commit(51) == RuntimeOwnerCutoverResult::RejectedInvalid);

    RuntimeOwnerCutoverCore post{};
    CHECK(post.prepare(ready_facts(), 61) ==
          RuntimeOwnerCutoverResult::Prepared);
    CHECK(post.commit(61) == RuntimeOwnerCutoverResult::Committed);
    CHECK(post.fail(true) ==
          RuntimeOwnerCutoverResult::CleanRebootRequired);
    CHECK(post.view().state ==
          RuntimeOwnerCutoverState::CleanRebootRequired);
    CHECK(post.fail(true) ==
          RuntimeOwnerCutoverResult::AcceptedDuplicate);
    CHECK(post.fail(false) == RuntimeOwnerCutoverResult::RejectedInvalid);
}

} // namespace

int main()
{
    test_numeric_and_layout_contracts();
    test_prepare_requires_exact_readiness();
    test_prepare_commit_and_duplicates();
    test_failure_boundary_is_sticky();
    if (g_failures != 0) {
        std::fprintf(stderr, "runtime_owner_cutover_core_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("runtime_owner_cutover_core_test: %zu checks passed\n",
                g_checks);
    return 0;
}
