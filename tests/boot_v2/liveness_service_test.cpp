#include "liveness_service.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace {

std::size_t g_allocation_count = 0;
std::size_t g_deallocation_count = 0;
std::size_t g_check_count = 0;
std::size_t g_failure_count = 0;

void check_impl(
    const bool condition,
    const char *expression,
    const char *file,
    const int line)
{
    ++g_check_count;
    if (!condition) {
        ++g_failure_count;
        std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
    }
}

#define CHECK(expression) check_impl((expression), #expression, __FILE__, __LINE__)

using boot_v2::LivenessAttemptToken;
using boot_v2::LivenessGateStatus;
using boot_v2::LivenessServiceCommand;
using boot_v2::LivenessServiceCommandKind;
using boot_v2::LivenessServiceCommandResult;
using boot_v2::PostConfigLivenessCommandBoundary;
using boot_v2::PostConfigLivenessService;

template <typename Type, typename = void>
struct HasPublicSubmit : std::false_type {
};

template <typename Type>
struct HasPublicSubmit<Type, std::void_t<decltype(
    std::declval<Type &>().submit(std::declval<LivenessServiceCommand>()))>>
    : std::true_type {
};

constexpr LivenessAttemptToken kAttempt{8, 4, 10};

constexpr std::array<LivenessServiceCommandKind, 4> kCompletionKinds{
    LivenessServiceCommandKind::AtOk,
    LivenessServiceCommandKind::SameSessionPubAck,
    LivenessServiceCommandKind::SubscriptionAlive,
    LivenessServiceCommandKind::FollowupConfigReceived,
};

constexpr LivenessServiceCommand command(
    const LivenessServiceCommandKind kind,
    const LivenessAttemptToken attempt = kAttempt)
{
    return {kind, attempt};
}

void expect_fresh_boundary(PostConfigLivenessCommandBoundary &boundary)
{
    CHECK(boundary.status() == LivenessGateStatus::NotStarted);
    CHECK(!boundary.active_attempt().valid());
    CHECK(!boundary.passed_for({}));
    CHECK(!boundary.passed_for(kAttempt));
}

void test_safe_defaults_and_invalid_input()
{
    PostConfigLivenessCommandBoundary boundary;
    expect_fresh_boundary(boundary);

    CHECK(boundary.submit({}) == LivenessServiceCommandResult::Rejected);
    for (const auto kind : kCompletionKinds) {
        CHECK(boundary.submit(command(kind)) ==
              LivenessServiceCommandResult::Rejected);
    }
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              {0, 4, 10})) == LivenessServiceCommandResult::Rejected);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              {8, 0, 10})) == LivenessServiceCommandResult::Rejected);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              {8, 4, 0})) == LivenessServiceCommandResult::Rejected);

    for (std::uint16_t value = 6; value <= 255; ++value) {
        CHECK(boundary.submit(command(
                  static_cast<LivenessServiceCommandKind>(value))) ==
              LivenessServiceCommandResult::Rejected);
    }
    expect_fresh_boundary(boundary);
}

void test_begin_and_all_subsets()
{
    for (std::uint8_t subset = 0; subset < 16; ++subset) {
        PostConfigLivenessCommandBoundary boundary;
        CHECK(boundary.submit(command(
                  LivenessServiceCommandKind::BeginAfterConfigApply)) ==
              LivenessServiceCommandResult::AcceptedWaiting);
        CHECK(boundary.status() == LivenessGateStatus::Waiting);
        CHECK(boundary.active_attempt() == kAttempt);

        LivenessServiceCommandResult last =
            LivenessServiceCommandResult::AcceptedWaiting;
        for (std::size_t index = 0; index < kCompletionKinds.size(); ++index) {
            if ((subset & static_cast<std::uint8_t>(1u << index)) != 0) {
                last = boundary.submit(command(kCompletionKinds[index]));
            }
        }

        if (subset == 15) {
            CHECK(last == LivenessServiceCommandResult::AcceptedPassed);
            CHECK(boundary.status() == LivenessGateStatus::Passed);
            CHECK(boundary.passed_for(kAttempt));
        } else {
            CHECK(boundary.status() == LivenessGateStatus::Waiting);
            CHECK(!boundary.passed_for(kAttempt));
        }
    }
}

void test_all_completion_orders()
{
    auto order = kCompletionKinds;
    std::size_t permutation_count = 0;
    do {
        PostConfigLivenessCommandBoundary boundary;
        CHECK(boundary.submit(command(
                  LivenessServiceCommandKind::BeginAfterConfigApply)) ==
              LivenessServiceCommandResult::AcceptedWaiting);

        for (std::size_t index = 0; index < order.size(); ++index) {
            const auto expected = index + 1 == order.size()
                                      ? LivenessServiceCommandResult::AcceptedPassed
                                      : LivenessServiceCommandResult::AcceptedWaiting;
            CHECK(boundary.submit(command(order[index])) == expected);
        }
        CHECK(boundary.passed_for(kAttempt));
        ++permutation_count;
    } while (std::next_permutation(order.begin(), order.end()));

    CHECK(permutation_count == 24);
}

void test_rejected_completion_does_not_record_hidden_evidence(
    const LivenessServiceCommand rejected)
{
    for (std::size_t omitted = 0; omitted < kCompletionKinds.size(); ++omitted) {
        PostConfigLivenessCommandBoundary boundary;
        CHECK(boundary.submit(command(
                  LivenessServiceCommandKind::BeginAfterConfigApply)) ==
              LivenessServiceCommandResult::AcceptedWaiting);

        const std::size_t prefix = (omitted + 1) % kCompletionKinds.size();
        CHECK(boundary.submit(command(kCompletionKinds[prefix])) ==
              LivenessServiceCommandResult::AcceptedWaiting);
        CHECK(boundary.submit(rejected) ==
              LivenessServiceCommandResult::Rejected);

        for (std::size_t index = 0; index < kCompletionKinds.size(); ++index) {
            if (index != omitted && index != prefix) {
                CHECK(boundary.submit(command(kCompletionKinds[index])) ==
                      LivenessServiceCommandResult::AcceptedWaiting);
            }
        }

        CHECK(boundary.status() == LivenessGateStatus::Waiting);
        CHECK(!boundary.passed_for(kAttempt));
        CHECK(boundary.submit(command(kCompletionKinds[omitted])) ==
              LivenessServiceCommandResult::AcceptedPassed);
    }
}

void test_rejected_input_is_evidence_neutral()
{
    test_rejected_completion_does_not_record_hidden_evidence(command(
        LivenessServiceCommandKind::SameSessionPubAck,
        {9, kAttempt.mqtt_generation, kAttempt.config_apply_epoch}));
    test_rejected_completion_does_not_record_hidden_evidence(command(
        LivenessServiceCommandKind::SameSessionPubAck,
        {kAttempt.mqtt_session_id, 5, kAttempt.config_apply_epoch}));
    test_rejected_completion_does_not_record_hidden_evidence(command(
        LivenessServiceCommandKind::SameSessionPubAck,
        {kAttempt.mqtt_session_id, kAttempt.mqtt_generation, 11}));
    test_rejected_completion_does_not_record_hidden_evidence(command(
        LivenessServiceCommandKind::Invalid));
    for (std::uint16_t value = 6; value <= 255; ++value) {
        test_rejected_completion_does_not_record_hidden_evidence(command(
            static_cast<LivenessServiceCommandKind>(value)));
    }
}

void test_rejected_begin_preserves_partial_evidence()
{
    PostConfigLivenessCommandBoundary boundary;
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(LivenessServiceCommandKind::AtOk)) ==
          LivenessServiceCommandResult::AcceptedWaiting);

    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              {0, 4, 11})) == LivenessServiceCommandResult::Rejected);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              {9, 5, 10})) == LivenessServiceCommandResult::Rejected);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              {9, 5, 9})) == LivenessServiceCommandResult::Rejected);

    CHECK(boundary.submit(command(LivenessServiceCommandKind::SameSessionPubAck)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(LivenessServiceCommandKind::SubscriptionAlive)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::FollowupConfigReceived)) ==
          LivenessServiceCommandResult::AcceptedPassed);
}

void test_duplicate_exact_token_and_new_attempt_results()
{
    PostConfigLivenessCommandBoundary boundary;
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(LivenessServiceCommandKind::AtOk)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(LivenessServiceCommandKind::AtOk)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(LivenessServiceCommandKind::SameSessionPubAck)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(LivenessServiceCommandKind::SubscriptionAlive)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::FollowupConfigReceived)) ==
          LivenessServiceCommandResult::AcceptedPassed);
    CHECK(boundary.submit(command(LivenessServiceCommandKind::AtOk)) ==
          LivenessServiceCommandResult::AcceptedPassed);
    CHECK(boundary.passed_for(kAttempt));
    CHECK(!boundary.passed_for({9, 4, 10}));
    CHECK(!boundary.passed_for({8, 5, 10}));
    CHECK(!boundary.passed_for({8, 4, 11}));

    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::SubscriptionAlive,
              {9, 4, 10})) == LivenessServiceCommandResult::Rejected);

    constexpr LivenessAttemptToken next{9, 5, 11};
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              next)) == LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.status() == LivenessGateStatus::Waiting);
    CHECK(!boundary.passed_for(kAttempt));
    CHECK(!boundary.passed_for(next));

    CHECK(boundary.submit(command(LivenessServiceCommandKind::AtOk, next)) ==
          LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::SameSessionPubAck,
              next)) == LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::SubscriptionAlive,
              next)) == LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.status() == LivenessGateStatus::Waiting);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::FollowupConfigReceived,
              next)) == LivenessServiceCommandResult::AcceptedPassed);
    CHECK(boundary.passed_for(next));
}

void test_epoch_saturation_is_fail_closed()
{
    PostConfigLivenessCommandBoundary boundary;
    constexpr LivenessAttemptToken near_max{1, 1, UINT32_MAX - 1};
    constexpr LivenessAttemptToken at_max{2, 2, UINT32_MAX};
    constexpr LivenessAttemptToken wrapped{3, 3, 1};

    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              near_max)) == LivenessServiceCommandResult::AcceptedWaiting);
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              at_max)) == LivenessServiceCommandResult::AcceptedWaiting);
    for (std::size_t index = 0; index < kCompletionKinds.size(); ++index) {
        const auto expected = index + 1 == kCompletionKinds.size()
                                  ? LivenessServiceCommandResult::AcceptedPassed
                                  : LivenessServiceCommandResult::AcceptedWaiting;
        CHECK(boundary.submit(command(kCompletionKinds[index], at_max)) == expected);
    }
    CHECK(boundary.passed_for(at_max));
    CHECK(boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              wrapped)) == LivenessServiceCommandResult::Rejected);
    CHECK(boundary.passed_for(at_max));

    PostConfigLivenessCommandBoundary fresh_boot_boundary;
    CHECK(fresh_boot_boundary.submit(command(
              LivenessServiceCommandKind::BeginAfterConfigApply,
              wrapped)) == LivenessServiceCommandResult::AcceptedWaiting);
}

} // namespace

void *operator new(const std::size_t size)
{
    ++g_allocation_count;
    if (void *memory = std::malloc(size)) {
        return memory;
    }
    std::abort();
}

void *operator new[](const std::size_t size)
{
    ++g_allocation_count;
    if (void *memory = std::malloc(size)) {
        return memory;
    }
    std::abort();
}

void operator delete(void *memory) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete[](void *memory) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
    ++g_deallocation_count;
    std::free(memory);
}

static_assert(std::is_standard_layout<LivenessServiceCommand>::value);
static_assert(std::is_trivially_copyable<LivenessServiceCommand>::value);
static_assert(!std::is_default_constructible<PostConfigLivenessService>::value);
static_assert(!HasPublicSubmit<PostConfigLivenessService>::value);
static_assert(!std::is_copy_constructible<PostConfigLivenessService>::value);
static_assert(!std::is_move_constructible<PostConfigLivenessService>::value);
static_assert(!std::is_copy_assignable<PostConfigLivenessService>::value);
static_assert(!std::is_move_assignable<PostConfigLivenessService>::value);
static_assert(!std::is_copy_constructible<PostConfigLivenessCommandBoundary>::value);
static_assert(!std::is_move_constructible<PostConfigLivenessCommandBoundary>::value);
static_assert(!std::is_copy_assignable<PostConfigLivenessCommandBoundary>::value);
static_assert(!std::is_move_assignable<PostConfigLivenessCommandBoundary>::value);
static_assert(!std::is_polymorphic<PostConfigLivenessService>::value);
static_assert(!std::is_polymorphic<PostConfigLivenessCommandBoundary>::value);
static_assert(std::is_nothrow_destructible<PostConfigLivenessService>::value);
static_assert(std::is_nothrow_destructible<PostConfigLivenessCommandBoundary>::value);
static_assert(noexcept(std::declval<PostConfigLivenessCommandBoundary &>().submit({})));
static_assert(noexcept(std::declval<const PostConfigLivenessCommandBoundary &>().status()));
static_assert(noexcept(
    std::declval<const PostConfigLivenessCommandBoundary &>().active_attempt()));
static_assert(noexcept(
    std::declval<const PostConfigLivenessCommandBoundary &>().passed_for({})));

int main()
{
    const auto allocation_baseline = g_allocation_count;
    const auto deallocation_baseline = g_deallocation_count;

    test_safe_defaults_and_invalid_input();
    test_begin_and_all_subsets();
    test_all_completion_orders();
    test_rejected_input_is_evidence_neutral();
    test_rejected_begin_preserves_partial_evidence();
    test_duplicate_exact_token_and_new_attempt_results();
    test_epoch_saturation_is_fail_closed();

    CHECK(g_allocation_count == allocation_baseline);
    CHECK(g_deallocation_count == deallocation_baseline);

    if (g_failure_count != 0) {
        std::fprintf(
            stderr,
            "LIVENESS_SERVICE_TEST FAIL checks=%zu failures=%zu\n",
            g_check_count,
            g_failure_count);
        return 1;
    }

    std::printf("LIVENESS_SERVICE_TEST PASS checks=%zu\n", g_check_count);
    return 0;
}
