#include "lifecycle_gate.hpp"

namespace boot_v2 {

bool PostConfigLivenessGate::begin_after_config_apply(
    const LivenessAttemptToken attempt)
{
    if (!attempt.valid()) {
        return false;
    }
    if (status_ != LivenessGateStatus::NotStarted &&
        attempt.config_apply_epoch <= active_attempt_.config_apply_epoch) {
        return false;
    }

    active_attempt_ = attempt;
    observed_signal_mask_ = 0;
    status_ = LivenessGateStatus::Waiting;
    return true;
}

bool PostConfigLivenessGate::observe(
    const LivenessSignal signal,
    const LivenessAttemptToken source_attempt)
{
    if (status_ == LivenessGateStatus::NotStarted ||
        source_attempt != active_attempt_) {
        return false;
    }

    std::uint8_t mask = 0;
    if (!signal_mask(signal, mask)) {
        return false;
    }

    observed_signal_mask_ =
        static_cast<std::uint8_t>(observed_signal_mask_ | mask);
    if (observed_signal_mask_ == kRequiredSignalMask) {
        status_ = LivenessGateStatus::Passed;
    }
    return true;
}

bool PostConfigLivenessGate::passed_for(
    const LivenessAttemptToken attempt) const
{
    return status_ == LivenessGateStatus::Passed &&
           attempt == active_attempt_;
}

LivenessGateStatus PostConfigLivenessGate::status() const
{
    return status_;
}

LivenessAttemptToken PostConfigLivenessGate::active_attempt() const
{
    return active_attempt_;
}

bool PostConfigLivenessGate::signal_mask(
    const LivenessSignal signal,
    std::uint8_t &mask)
{
    switch (signal) {
    case LivenessSignal::AtOk:
        mask = 0x01;
        return true;
    case LivenessSignal::SameSessionPubAck:
        mask = 0x02;
        return true;
    case LivenessSignal::SubscriptionAlive:
        mask = 0x04;
        return true;
    case LivenessSignal::FollowupConfigReceived:
        mask = 0x08;
        return true;
    }
    mask = 0;
    return false;
}

} // namespace boot_v2
