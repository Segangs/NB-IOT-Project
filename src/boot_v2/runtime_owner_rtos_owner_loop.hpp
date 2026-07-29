#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_RTOS_OWNER_LOOP_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_RTOS_OWNER_LOOP_HPP

#include "runtime_owner_rtos_drain_core.hpp"
#include "runtime_owner_physical_executor.hpp"
#include "runtime_owner_shutdown_finalizer_core.hpp"
#include "runtime_owner_task_core.hpp"

namespace boot_v2 {

class RuntimeOwnerRtosOwnerLoop {
public:
    explicit RuntimeOwnerRtosOwnerLoop(RuntimeOwnerTaskCore &core) noexcept
        : core_(core) {}

    [[nodiscard]] RuntimeOwnerDrainConsumeResult consume_urgent(
        const RuntimeOwnerUrgentMessage input) noexcept
    {
        if (!runtime_owner_is_canonical_urgent(input)) {
            return RuntimeOwnerDrainConsumeResult::DroppedInvalid;
        }
        RuntimeOwnerShutdownRequestResult request_result =
            RuntimeOwnerShutdownRequestResult::RejectedInvalid;
        if (input.source == RuntimeOwnerUrgentSource::PowerButton) {
            auto port = core_.power_button_shutdown_port();
            request_result = port.request(
                input.producer_sequence, input.incident_correlation_id);
        } else if (
            input.source == RuntimeOwnerUrgentSource::AdapterLossCommitted) {
            auto port = core_.adapter_loss_shutdown_port();
            request_result = port.request(
                input.producer_sequence, input.incident_correlation_id);
        } else {
            auto port = core_.authenticated_command_shutdown_port();
            request_result = port.request(
                input.producer_sequence, input.incident_correlation_id);
        }
        if (request_result == RuntimeOwnerShutdownRequestResult::Accepted) {
            shutdown_context_ = input;
            has_shutdown_context_ = true;
        }
        (void)core_.process_cycle(RuntimeOwnerTaskCycleInput{});
        return RuntimeOwnerDrainConsumeResult::Processed;
    }

    [[nodiscard]] RuntimeOwnerDrainConsumeResult consume_control(
        const RuntimeOwnerControlMessage input) noexcept
    {
        if (!runtime_owner_is_canonical_control(input)) {
            return RuntimeOwnerDrainConsumeResult::DroppedInvalid;
        }
        RuntimeOwnerTaskCycleInput cycle{};
        cycle.transport_pending = 1;
        (void)core_.process_cycle(cycle);
        return RuntimeOwnerDrainConsumeResult::Processed;
    }

    [[nodiscard]] RuntimeOwnerDrainConsumeResult consume_normal(
        const NormalIntent input) noexcept
    {
        const RuntimeOwnerDrainConsumeResult classification =
            runtime_owner_normal_intent_is_canonical(input)
                ? RuntimeOwnerDrainConsumeResult::Processed
                : RuntimeOwnerDrainConsumeResult::DroppedInvalid;
        RuntimeOwnerTaskCycleInput cycle{};
        cycle.normal_pending = 1;
        cycle.normal = input;
        (void)core_.process_cycle(cycle);
        return classification;
    }

    [[nodiscard]] RuntimeOwnerTaskCycleResult advance() noexcept
    {
        return core_.process_cycle(RuntimeOwnerTaskCycleInput{});
    }

    [[nodiscard]] TemperatureAlarmDeliveryPopResult
        try_pop_alarm_delivery(
            TemperatureAlarmDeliveryEvent &event) noexcept
    {
        return core_.try_pop_alarm_delivery(event);
    }

    [[nodiscard]] bool
        take_alarm_delivery_overflow_log_pending() noexcept
    {
        return core_.take_alarm_delivery_overflow_log_pending();
    }

    template <typename Backend>
    [[nodiscard]] RuntimeOwnerPhysicalStepResult execute_one(
        Backend &backend) noexcept
    {
        auto port = core_.executor_port();
        return runtime_owner_execute_one(port, backend);
    }

    template <typename Backend>
    [[nodiscard]] RuntimeOwnerPhysicalStepResult submit_deferred_config(
        Backend &backend) noexcept
    {
        auto port = core_.executor_port();
        return runtime_owner_submit_deferred_config(port, backend);
    }

    template <typename Backend>
    [[nodiscard]] RuntimeOwnerShutdownStepResult execute_shutdown_cleanup(
        Backend &backend,
        const RuntimeOwnerShutdownDirective &directive,
        const RuntimeOwnerUrgentMessage context) noexcept
    {
        return backend.execute_shutdown_cleanup(
            directive, context);
    }

    [[nodiscard]] bool physical_inflight_idle() const noexcept
    {
        return core_.adapter_.view().physical_inflight.kind ==
               AdapterDispatchKind::None;
    }

    [[nodiscard]] bool copy_shutdown_context(
        RuntimeOwnerUrgentMessage &output) const noexcept
    {
        if (!has_shutdown_context_) {
            return false;
        }
        output = shutdown_context_;
        return true;
    }

private:
    RuntimeOwnerTaskCore &core_;
    RuntimeOwnerUrgentMessage shutdown_context_{};
    bool has_shutdown_context_{false};
};

} // namespace boot_v2

#endif
