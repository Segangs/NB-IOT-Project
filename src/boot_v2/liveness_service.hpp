#ifndef NB_IOT_BOOT_V2_LIVENESS_SERVICE_HPP
#define NB_IOT_BOOT_V2_LIVENESS_SERVICE_HPP

#include <cstdint>

#include "lifecycle_gate.hpp"

namespace boot_v2 {

enum class LivenessServiceCommandKind : std::uint8_t {
    Invalid = 0,
    BeginAfterConfigApply = 1,
    AtOk = 2,
    SameSessionPubAck = 3,
    SubscriptionAlive = 4,
    FollowupConfigReceived = 5,
};

struct LivenessServiceCommand {
    LivenessServiceCommandKind kind{LivenessServiceCommandKind::Invalid};
    LivenessAttemptToken attempt{};
};

enum class LivenessServiceCommandResult : std::uint8_t {
    Rejected = 0,
    AcceptedWaiting = 1,
    AcceptedPassed = 2,
};

class PostConfigLivenessCommandBoundary;

class PostConfigLivenessService {
public:
    PostConfigLivenessService(const PostConfigLivenessService &) = delete;
    PostConfigLivenessService &operator=(const PostConfigLivenessService &) = delete;
    PostConfigLivenessService(PostConfigLivenessService &&) = delete;
    PostConfigLivenessService &operator=(PostConfigLivenessService &&) = delete;
    ~PostConfigLivenessService() noexcept = default;

private:
    friend class PostConfigLivenessCommandBoundary;

    PostConfigLivenessService() noexcept = default;

    [[nodiscard]] LivenessServiceCommandResult submit(
        LivenessServiceCommand command) noexcept;
    [[nodiscard]] LivenessGateStatus status() const noexcept;
    [[nodiscard]] LivenessAttemptToken active_attempt() const noexcept;
    [[nodiscard]] bool passed_for(LivenessAttemptToken attempt) const noexcept;

    PostConfigLivenessGate gate_{};
};

// Compile-only synchronous mutation port. A future runtime owner must marshal all
// calls through one task. This type is not thread-safe and not ISR-safe, and it
// does not authenticate the producer provenance of any completion signal.
class PostConfigLivenessCommandBoundary {
public:
    PostConfigLivenessCommandBoundary() noexcept = default;
    PostConfigLivenessCommandBoundary(
        const PostConfigLivenessCommandBoundary &) = delete;
    PostConfigLivenessCommandBoundary &operator=(
        const PostConfigLivenessCommandBoundary &) = delete;
    PostConfigLivenessCommandBoundary(
        PostConfigLivenessCommandBoundary &&) = delete;
    PostConfigLivenessCommandBoundary &operator=(
        PostConfigLivenessCommandBoundary &&) = delete;
    ~PostConfigLivenessCommandBoundary() noexcept = default;

    // BeginAfterConfigApply is valid only after the future request coordinator
    // verifies every CONFIG page and checksum and completes the atomic apply.
    [[nodiscard]] LivenessServiceCommandResult submit(
        LivenessServiceCommand command) noexcept;
    [[nodiscard]] LivenessGateStatus status() const noexcept;
    [[nodiscard]] LivenessAttemptToken active_attempt() const noexcept;
    [[nodiscard]] bool passed_for(LivenessAttemptToken attempt) const noexcept;

private:
    PostConfigLivenessService service_{};
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_LIVENESS_SERVICE_HPP
