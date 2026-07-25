#ifndef NB_IOT_BOOT_V2_LIFECYCLE_GATE_HPP
#define NB_IOT_BOOT_V2_LIFECYCLE_GATE_HPP

#include <cstdint>

namespace boot_v2 {

struct LivenessAttemptToken {
    std::uint32_t mqtt_session_id{0};
    std::uint32_t mqtt_generation{0};
    std::uint32_t config_apply_epoch{0};

    constexpr bool valid() const
    {
        return mqtt_session_id != 0 &&
               mqtt_generation != 0 &&
               config_apply_epoch != 0;
    }
};

constexpr bool operator==(
    const LivenessAttemptToken &left,
    const LivenessAttemptToken &right)
{
    return left.mqtt_session_id == right.mqtt_session_id &&
           left.mqtt_generation == right.mqtt_generation &&
           left.config_apply_epoch == right.config_apply_epoch;
}

constexpr bool operator!=(
    const LivenessAttemptToken &left,
    const LivenessAttemptToken &right)
{
    return !(left == right);
}

enum class LivenessSignal : std::uint8_t {
    AtOk = 0,
    SameSessionPubAck = 1,
    SubscriptionAlive = 2,
    FollowupConfigReceived = 3,
};

enum class LivenessGateStatus : std::uint8_t {
    NotStarted = 0,
    Waiting = 1,
    Passed = 2,
};

class PostConfigLivenessGate {
public:
    [[nodiscard]] bool begin_after_config_apply(LivenessAttemptToken attempt);
    bool observe(LivenessSignal signal, LivenessAttemptToken source_attempt);
    bool passed_for(LivenessAttemptToken attempt) const;
    LivenessGateStatus status() const;
    LivenessAttemptToken active_attempt() const;

private:
    static bool signal_mask(LivenessSignal signal, std::uint8_t &mask);

    static constexpr std::uint8_t kRequiredSignalMask = 0x0f;
    LivenessAttemptToken active_attempt_{};
    std::uint8_t observed_signal_mask_{0};
    LivenessGateStatus status_{LivenessGateStatus::NotStarted};
};

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_LIFECYCLE_GATE_HPP
