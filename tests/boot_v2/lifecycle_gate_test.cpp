#include "lifecycle_gate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

using boot_v2::LivenessAttemptToken;
using boot_v2::LivenessGateStatus;
using boot_v2::LivenessSignal;
using boot_v2::PostConfigLivenessGate;

namespace {

std::size_t check_count = 0;
std::size_t failure_count = 0;

void record_check(
    const bool condition,
    const char *const expression,
    const char *const file,
    const int line)
{
    ++check_count;
    if (!condition) {
        ++failure_count;
        std::cerr << file << ':' << line << ": CHECK(" << expression
                  << ") failed\n";
    }
}

#define CHECK(...)                                                          \
    do {                                                                    \
        record_check(                                                       \
            static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__,       \
            __LINE__);                                                      \
    } while (false)

constexpr std::array<LivenessSignal, 4> kSignals{
    LivenessSignal::AtOk,
    LivenessSignal::SameSessionPubAck,
    LivenessSignal::SubscriptionAlive,
    LivenessSignal::FollowupConfigReceived,
};

void observe_mask(
    PostConfigLivenessGate &gate,
    const LivenessAttemptToken attempt,
    const std::uint8_t mask)
{
    for (std::size_t index = 0; index < kSignals.size(); ++index) {
        if ((mask & static_cast<std::uint8_t>(1u << index)) != 0) {
            CHECK(gate.observe(kSignals[index], attempt));
        }
    }
}

} // namespace

int main()
{
    {
        PostConfigLivenessGate zero_token_gate;
        for (const auto signal : kSignals) {
            CHECK(!zero_token_gate.observe(signal, LivenessAttemptToken{}));
        }
        CHECK(zero_token_gate.status() == LivenessGateStatus::NotStarted);
        CHECK(zero_token_gate.active_attempt() == LivenessAttemptToken{});
        CHECK(!zero_token_gate.passed_for(LivenessAttemptToken{}));
    }

    {
        PostConfigLivenessGate gate;
        const LivenessAttemptToken before_start{7, 3, 1};

        CHECK(gate.status() == LivenessGateStatus::NotStarted);
        CHECK(!gate.passed_for(before_start));
        CHECK(gate.active_attempt() == LivenessAttemptToken{});
        for (const auto signal : kSignals) {
            CHECK(!gate.observe(signal, before_start));
        }
        CHECK(gate.status() == LivenessGateStatus::NotStarted);
        CHECK(gate.active_attempt() == LivenessAttemptToken{});
        CHECK(!gate.passed_for(before_start));

        CHECK(!gate.begin_after_config_apply({0, 1, 1}));
        CHECK(!gate.begin_after_config_apply({1, 0, 1}));
        CHECK(!gate.begin_after_config_apply({1, 1, 0}));

        const LivenessAttemptToken first{7, 3, 10};
        CHECK(gate.begin_after_config_apply(first));
        CHECK(!gate.begin_after_config_apply({0, 4, 11}));
        CHECK(!gate.begin_after_config_apply({8, 0, 11}));
        CHECK(!gate.begin_after_config_apply({8, 4, 0}));
        CHECK(!gate.begin_after_config_apply({8, 4, 10}));
        CHECK(!gate.begin_after_config_apply({8, 4, 9}));
        CHECK(gate.active_attempt() == first);
        CHECK(gate.status() == LivenessGateStatus::Waiting);

        const LivenessAttemptToken next{8, 4, 11};
        CHECK(gate.begin_after_config_apply(next));
        CHECK(gate.status() == LivenessGateStatus::Waiting);
        CHECK(!gate.passed_for(next));
        CHECK(gate.active_attempt() == next);
    }

    for (std::uint8_t mask = 0; mask < 16; ++mask) {
        PostConfigLivenessGate gate;
        const LivenessAttemptToken attempt{7, 3, 1};
        CHECK(gate.begin_after_config_apply(attempt));
        observe_mask(gate, attempt, mask);
        CHECK(gate.passed_for(attempt) == (mask == 15));
        CHECK(gate.status() ==
              (mask == 15 ? LivenessGateStatus::Passed
                          : LivenessGateStatus::Waiting));
    }

    {
        auto order = kSignals;
        do {
            PostConfigLivenessGate gate;
            const LivenessAttemptToken attempt{7, 3, 2};
            CHECK(gate.begin_after_config_apply(attempt));
            for (std::size_t index = 0; index < order.size(); ++index) {
                CHECK(gate.observe(order[index], attempt));
                CHECK(gate.passed_for(attempt) ==
                      (index + 1 == order.size()));
            }
        } while (std::next_permutation(order.begin(), order.end()));
    }

    {
        PostConfigLivenessGate gate;
        const LivenessAttemptToken active{7, 3, 20};
        CHECK(gate.begin_after_config_apply(active));
        CHECK(gate.observe(LivenessSignal::AtOk, active));
        CHECK(gate.observe(LivenessSignal::AtOk, active));
        CHECK(gate.observe(LivenessSignal::AtOk, active));
        CHECK(!gate.observe(static_cast<LivenessSignal>(255), active));
        CHECK(gate.status() == LivenessGateStatus::Waiting);
        CHECK(!gate.passed_for(active));

        CHECK(gate.observe(LivenessSignal::SameSessionPubAck, active));
        CHECK(gate.observe(LivenessSignal::SubscriptionAlive, active));
        CHECK(gate.status() == LivenessGateStatus::Waiting);
        CHECK(!gate.passed_for(active));

        CHECK(!gate.observe(LivenessSignal::FollowupConfigReceived,
                            {8, 3, 20}));
        CHECK(!gate.observe(LivenessSignal::FollowupConfigReceived,
                            {7, 2, 20}));
        CHECK(!gate.observe(LivenessSignal::FollowupConfigReceived,
                            {7, 3, 19}));
        CHECK(!gate.passed_for(active));

        CHECK(gate.observe(LivenessSignal::FollowupConfigReceived, active));
        CHECK(gate.passed_for(active));
        CHECK(!gate.passed_for({8, 3, 20}));
        CHECK(!gate.passed_for({7, 2, 20}));
        CHECK(!gate.passed_for({7, 3, 19}));
        CHECK(gate.observe(LivenessSignal::SameSessionPubAck, active));
        CHECK(gate.passed_for(active));
        CHECK(!gate.observe(static_cast<LivenessSignal>(255), active));
        CHECK(gate.passed_for(active));
    }

    {
        PostConfigLivenessGate gate;
        const LivenessAttemptToken old_attempt{7, 3, 30};
        const LivenessAttemptToken new_attempt{7, 3, 31};
        CHECK(gate.begin_after_config_apply(old_attempt));
        observe_mask(gate, old_attempt, 7);
        CHECK(!gate.passed_for(old_attempt));

        CHECK(gate.begin_after_config_apply(new_attempt));
        CHECK(!gate.passed_for(new_attempt));
        for (const auto signal : kSignals) {
            CHECK(!gate.observe(signal, old_attempt));
        }
        CHECK(!gate.passed_for(old_attempt));
        CHECK(!gate.passed_for(new_attempt));

        CHECK(gate.observe(LivenessSignal::FollowupConfigReceived,
                           new_attempt));
        CHECK(gate.status() == LivenessGateStatus::Waiting);
        CHECK(!gate.passed_for(new_attempt));

        observe_mask(gate, new_attempt, 7);
        CHECK(gate.passed_for(new_attempt));

        const LivenessAttemptToken next_attempt{7, 4, 32};
        CHECK(gate.begin_after_config_apply(next_attempt));
        CHECK(!gate.passed_for(new_attempt));
        CHECK(!gate.passed_for(next_attempt));
        CHECK(gate.status() == LivenessGateStatus::Waiting);
    }

    {
        PostConfigLivenessGate gate;
        const LivenessAttemptToken passed_attempt{7, 3, 40};
        CHECK(gate.begin_after_config_apply(passed_attempt));
        observe_mask(gate, passed_attempt, 15);
        CHECK(gate.status() == LivenessGateStatus::Passed);
        CHECK(gate.passed_for(passed_attempt));

        const LivenessAttemptToken invalid_attempt{0, 4, 41};
        CHECK(!gate.begin_after_config_apply(invalid_attempt));
        CHECK(gate.status() == LivenessGateStatus::Passed);
        CHECK(gate.active_attempt() == passed_attempt);
        CHECK(gate.passed_for(passed_attempt));
        CHECK(!gate.passed_for(invalid_attempt));

        CHECK(!gate.begin_after_config_apply(passed_attempt));
        CHECK(gate.status() == LivenessGateStatus::Passed);
        CHECK(gate.active_attempt() == passed_attempt);
        CHECK(gate.passed_for(passed_attempt));

        const LivenessAttemptToken equal_epoch{8, 4, 40};
        CHECK(!gate.begin_after_config_apply(equal_epoch));
        CHECK(gate.status() == LivenessGateStatus::Passed);
        CHECK(gate.active_attempt() == passed_attempt);
        CHECK(gate.passed_for(passed_attempt));
        CHECK(!gate.passed_for(equal_epoch));

        const LivenessAttemptToken older_epoch{8, 4, 39};
        CHECK(!gate.begin_after_config_apply(older_epoch));
        CHECK(gate.status() == LivenessGateStatus::Passed);
        CHECK(gate.active_attempt() == passed_attempt);
        CHECK(gate.passed_for(passed_attempt));
        CHECK(!gate.passed_for(older_epoch));

        const LivenessAttemptToken newer_attempt{8, 4, 41};
        CHECK(gate.begin_after_config_apply(newer_attempt));
        CHECK(gate.status() == LivenessGateStatus::Waiting);
        CHECK(gate.active_attempt() == newer_attempt);
        CHECK(!gate.passed_for(passed_attempt));
        CHECK(!gate.passed_for(newer_attempt));
    }

    {
        constexpr std::uint32_t max_epoch =
            std::numeric_limits<std::uint32_t>::max();
        PostConfigLivenessGate saturated_gate;
        const LivenessAttemptToken near_max{7, 3, max_epoch - 1};
        const LivenessAttemptToken at_max{8, 4, max_epoch};
        const LivenessAttemptToken wrapped{9, 5, 1};

        CHECK(saturated_gate.begin_after_config_apply(near_max));
        CHECK(saturated_gate.begin_after_config_apply(at_max));
        observe_mask(saturated_gate, at_max, 15);
        CHECK(saturated_gate.status() == LivenessGateStatus::Passed);
        CHECK(saturated_gate.passed_for(at_max));

        CHECK(!saturated_gate.begin_after_config_apply(wrapped));
        CHECK(saturated_gate.status() == LivenessGateStatus::Passed);
        CHECK(saturated_gate.active_attempt() == at_max);
        CHECK(saturated_gate.passed_for(at_max));
        CHECK(!saturated_gate.passed_for(wrapped));

        PostConfigLivenessGate fresh_boot_gate;
        CHECK(fresh_boot_gate.begin_after_config_apply(wrapped));
        CHECK(fresh_boot_gate.status() == LivenessGateStatus::Waiting);
        CHECK(!fresh_boot_gate.passed_for(wrapped));
        observe_mask(fresh_boot_gate, wrapped, 15);
        CHECK(fresh_boot_gate.passed_for(wrapped));
    }

    if (failure_count != 0) {
        std::cerr << "LIFECYCLE_GATE_TEST FAIL checks=" << check_count
                  << " failures=" << failure_count << '\n';
        return 1;
    }

    std::cout << "LIFECYCLE_GATE_TEST PASS checks=" << check_count << '\n';
    return 0;
}
