#include "runtime_owner_rtos.hpp"
#include "runtime_owner_rtos_drain_core.hpp"
#include "runtime_owner_rtos_owner_loop.hpp"
#include "runtime_owner_device_backend.hpp"
#include "runtime_owner_cutover_core.hpp"
#include "runtime_owner_shutdown_finalizer_core.hpp"

#include <atomic>
#include <cstdint>
#include <limits>

#include "FreeRTOS.h"
#include "hardware/watchdog.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "queue.h"
#include "task.h"

#include "../config.h"
#include "../lib/log.hpp"

namespace boot_v2 {

class RuntimeOwnerCutoverCoordinator {
public:
    [[nodiscard]] RuntimeOwnerTaskActivationResult activate(
        RuntimeOwnerTaskCore &core,
        const std::uint32_t stable_identity) noexcept
    {
        const RuntimeOwnerCutoverPermit permit{stable_identity};
        return core.activate(permit);
    }
};

namespace {

constexpr UBaseType_t kRuntimeOwnerCoreMask = 1u << 0;
constexpr UBaseType_t kRuntimeOwnerPriority = 2;
constexpr configSTACK_DEPTH_TYPE kRuntimeOwnerStackWords = 1024;
constexpr UBaseType_t kUrgentDepth = 1;
constexpr UBaseType_t kControlDepth = 2;
constexpr UBaseType_t kNormalDepth = 8;

StaticTask_t g_task_buffer{};
StackType_t g_task_stack[kRuntimeOwnerStackWords]{};
TaskHandle_t g_task_handle{nullptr};
StaticQueue_t g_urgent_queue_buffer{};
StaticQueue_t g_control_queue_buffer{};
StaticQueue_t g_normal_queue_buffer{};
alignas(RuntimeOwnerUrgentMessage)
std::uint8_t g_urgent_storage[
    kUrgentDepth * sizeof(RuntimeOwnerUrgentMessage)]{};
alignas(RuntimeOwnerControlMessage)
std::uint8_t g_control_storage[
    kControlDepth * sizeof(RuntimeOwnerControlMessage)]{};
alignas(NormalIntent)
std::uint8_t g_normal_storage[kNormalDepth * sizeof(NormalIntent)]{};
QueueHandle_t g_urgent_queue{nullptr};
QueueHandle_t g_control_queue{nullptr};
QueueHandle_t g_normal_queue{nullptr};
RuntimeOwnerTaskCore g_task_core{};
RuntimeOwnerRedactedStatus g_redacted_status_cache{};
RuntimeOwnerRtosStatus g_status{};
RuntimeOwnerRtosDrainMetrics g_drain_metrics{};

class RuntimeOwnerRtosQueueBackend {
public:
    RuntimeOwnerQueueReadResult receive_urgent(
        RuntimeOwnerUrgentMessage &output) noexcept
    {
        return receive(g_urgent_queue, output);
    }

    RuntimeOwnerQueueReadResult receive_control(
        RuntimeOwnerControlMessage &output) noexcept
    {
        return receive(g_control_queue, output);
    }

    RuntimeOwnerQueueReadResult receive_normal(NormalIntent &output) noexcept
    {
        return receive(g_normal_queue, output);
    }

private:
    template <typename Value>
    static RuntimeOwnerQueueReadResult receive(
        const QueueHandle_t queue,
        Value &output) noexcept
    {
        const BaseType_t result = xQueueReceive(queue, &output, 0);
        if (result == pdPASS) {
            return RuntimeOwnerQueueReadResult::Received;
        }
        if (result == errQUEUE_EMPTY) {
            return RuntimeOwnerQueueReadResult::Empty;
        }
        return RuntimeOwnerQueueReadResult::Fault;
    }
};

RuntimeOwnerRtosQueueBackend g_queue_backend{};
RuntimeOwnerRtosOwnerLoop g_owner_loop{g_task_core};
RuntimeOwnerDeviceBackend g_device_backend{};
RuntimeOwnerCutoverCore g_cutover_core{};
RuntimeOwnerCutoverCoordinator g_cutover_coordinator{};
RuntimeOwnerShutdownFinalizerCore g_shutdown_finalizer{};
std::uint32_t g_usb_sample_sequence{0};

constexpr std::uint32_t kRuntimeOwnerStableCutoverIdentity = 0x47324132u;

void publish_redacted_status_cache() noexcept
{
    const RuntimeOwnerRedactedStatus snapshot =
        g_task_core.redacted_status();
    taskENTER_CRITICAL();
    g_redacted_status_cache = snapshot;
    taskEXIT_CRITICAL();
}

void increment_saturating(std::uint32_t &counter) noexcept
{
    if (counter != std::numeric_limits<std::uint32_t>::max()) {
        ++counter;
    }
}

void increment_nonzero(std::uint32_t &counter) noexcept
{
    if (counter == std::numeric_limits<std::uint32_t>::max()) {
        counter = 1;
    } else {
        ++counter;
        if (counter == 0) {
            counter = 1;
        }
    }
}

UsbPowerObservation sample_usb_power(
    const std::uint8_t cleanup_skipped_mask,
    const std::uint8_t cleanup_timed_out_mask,
    const std::uint8_t hard_deadline_reached) noexcept
{
    increment_nonzero(g_usb_sample_sequence);
    UsbPowerObservation observation{};
    observation.present = runtime_owner_usb_power_present() ? 1 : 0;
    observation.cleanup_skipped_mask = cleanup_skipped_mask;
    observation.cleanup_timed_out_mask = cleanup_timed_out_mask;
    observation.hard_deadline_reached = hard_deadline_reached;
    observation.sample_sequence = g_usb_sample_sequence;
    observation.sampled_at_monotonic_ms =
        to_ms_since_boot(get_absolute_time());
    return observation;
}

[[noreturn]] void commit_shutdown_watchdog(
    const RuntimeOwnerUrgentMessage context) noexcept
{
    watchdog_hw->scratch[2] = COMMAND_WATCHDOG_SCRATCH_MAGIC;
    watchdog_hw->scratch[3] = context.incident_correlation_id;
    LOG("SHUTDOWN_WATCHDOG_COMMIT\n");
    watchdog_reboot(0, 0, 100);
    for (;;) {
        tight_loop_contents();
    }
}

[[noreturn]] void commit_shutdown_gp15_kill() noexcept
{
    gpio_init(POWER_KILL_PIN);
    gpio_put(POWER_KILL_PIN, POWER_KILL_INACTIVE_LEVEL);
    gpio_set_dir(POWER_KILL_PIN, GPIO_OUT);
    LOG("SHUTDOWN_GP15_COMMIT\n");
    gpio_put(POWER_KILL_PIN, POWER_KILL_ACTIVE_LEVEL);
    for (;;) {
        tight_loop_contents();
    }
}

[[noreturn]] void commit_shutdown_from_fresh_usb(
    const RuntimeOwnerUrgentMessage context) noexcept
{
    const bool latest_usb_present =
        runtime_owner_usb_power_present();
    if (context.intent == RuntimeOwnerShutdownIntent::Reboot ||
        latest_usb_present) {
        commit_shutdown_watchdog(context);
    }
    commit_shutdown_gp15_kill();
}

void drive_owner_until_idle() noexcept
{
    for (std::uint32_t cycle = 0; cycle < 64; ++cycle) {
        const RuntimeOwnerTaskCycleResult advanced =
            g_owner_loop.advance();
        if (g_owner_loop.take_alarm_delivery_overflow_log_pending()) {
            LOG("ALARM_DELIVERY_OVERFLOW\n");
        }
        const RuntimeOwnerPhysicalStepResult deferred =
            g_owner_loop.submit_deferred_config(g_device_backend);
        if (deferred == RuntimeOwnerPhysicalStepResult::Completed) {
            continue;
        }
        if (deferred == RuntimeOwnerPhysicalStepResult::Rejected ||
            deferred == RuntimeOwnerPhysicalStepResult::Terminal) {
            break;
        }
        const RuntimeOwnerPhysicalStepResult physical =
            g_owner_loop.execute_one(g_device_backend);
        if (physical == RuntimeOwnerPhysicalStepResult::Terminal) {
            break;
        }
        if (physical == RuntimeOwnerPhysicalStepResult::Rejected) {
            taskENTER_CRITICAL();
            increment_saturating(g_drain_metrics.receive_fault_count);
            taskEXIT_CRITICAL();
            break;
        }
        if (physical == RuntimeOwnerPhysicalStepResult::NoCommand &&
            (advanced.step_result.action == AdapterStepAction::Idle ||
             advanced.step_result.action ==
                 AdapterStepAction::AwaitingTrustedReceipt ||
             advanced.step_result.action == AdapterStepAction::Terminal)) {
            break;
        }
    }
}

[[noreturn]] void run_shutdown_finalizer(
    const RuntimeOwnerUrgentMessage context) noexcept
{
    const UsbPowerObservation initial_usb = sample_usb_power(0, 0, 0);
    const std::uint32_t started_at =
        to_ms_since_boot(get_absolute_time());
    const RuntimeOwnerShutdownStartResult started =
        g_shutdown_finalizer.start(
            context,
            initial_usb,
            started_at,
            SHUTDOWN_HARD_DEADLINE_MS);
    if (started != RuntimeOwnerShutdownStartResult::Started &&
        started != RuntimeOwnerShutdownStartResult::AcceptedDuplicate) {
        LOG("SHUTDOWN_USB_INITIAL_UNSAFE\n");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    for (;;) {
        const std::uint32_t now =
            to_ms_since_boot(get_absolute_time());
        const RuntimeOwnerShutdownDirective directive =
            g_shutdown_finalizer.next(now);
        switch (directive.action) {
        case RuntimeOwnerShutdownFinalizeAction::RunCleanupStep: {
            const RuntimeOwnerShutdownStepResult result =
                g_owner_loop.execute_shutdown_cleanup(
                    g_device_backend,
                    directive,
                    context);
            (void)g_shutdown_finalizer.complete(
                directive.step,
                result,
                to_ms_since_boot(get_absolute_time()));
            break;
        }
        case RuntimeOwnerShutdownFinalizeAction::RecheckUsb: {
            const UsbPowerObservation final_usb = sample_usb_power(
                directive.cleanup_skipped_mask,
                directive.cleanup_timed_out_mask,
                directive.hard_deadline);
            (void)g_shutdown_finalizer.submit_usb_recheck(final_usb);
            break;
        }
        case RuntimeOwnerShutdownFinalizeAction::CommitWatchdog:
            commit_shutdown_watchdog(context);
        case RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill:
            commit_shutdown_gp15_kill();
        case RuntimeOwnerShutdownFinalizeAction::AbortUsbChanged:
            LOG("SHUTDOWN_USB_CHANGED_RECHECK\n");
            commit_shutdown_from_fresh_usb(context);
        case RuntimeOwnerShutdownFinalizeAction::AbortEvidenceMissing:
            LOG("SHUTDOWN_EVIDENCE_MISSING_RECHECK\n");
            commit_shutdown_from_fresh_usb(context);
        case RuntimeOwnerShutdownFinalizeAction::Idle:
        case RuntimeOwnerShutdownFinalizeAction::Terminal:
            LOG("SHUTDOWN_FINALIZER_TERMINAL_ABORT\n");
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void runtime_owner_task_entry(void *) noexcept
{
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL();
        increment_saturating(g_status.wake_count);
        taskEXIT_CRITICAL();

        RuntimeOwnerUrgentMessage shutdown_context{};
        bool shutdown_received = false;
        for (;;) {
            const RuntimeOwnerDrainStep step = runtime_owner_drain_once(
                g_queue_backend, g_owner_loop);
            if (g_owner_loop.take_alarm_delivery_overflow_log_pending()) {
                LOG("ALARM_DELIVERY_OVERFLOW\n");
            }
            publish_redacted_status_cache();
            taskENTER_CRITICAL();
            runtime_owner_record_drain_step(g_drain_metrics, step);
            taskEXIT_CRITICAL();

            if (g_owner_loop.copy_shutdown_context(shutdown_context)) {
                taskENTER_CRITICAL();
                g_status.ingress_enabled = 0;
                taskEXIT_CRITICAL();
                LOG("SHUTDOWN_INGRESS_CLOSED\n");
                shutdown_received = true;
                break;
            }
            if (step.result == RuntimeOwnerDrainResult::Empty ||
                step.result == RuntimeOwnerDrainResult::Fault) {
                break;
            }
        }

        drive_owner_until_idle();
        publish_redacted_status_cache();
        if (shutdown_received) {
            publish_redacted_status_cache();
            run_shutdown_finalizer(shutdown_context);
        }
    }
}

template <typename Value>
RuntimeOwnerIngressResult try_submit(
    const QueueHandle_t queue,
    const Value &value) noexcept
{
    std::uint8_t started{0};
    std::uint8_t ingress_enabled{0};
    TaskHandle_t task_handle{nullptr};

    taskENTER_CRITICAL();
    started = g_status.started;
    ingress_enabled = g_status.ingress_enabled;
    task_handle = g_task_handle;
    taskEXIT_CRITICAL();

    const RuntimeOwnerIngressGateResult gate = runtime_owner_ingress_gate(
        started != 0,
        ingress_enabled != 0);
    if (gate == RuntimeOwnerIngressGateResult::RejectedNotStarted) {
        return RuntimeOwnerIngressResult::RejectedNotStarted;
    }
    if (gate == RuntimeOwnerIngressGateResult::RejectedInactive) {
        taskENTER_CRITICAL();
        increment_saturating(g_status.rejected_inactive_count);
        taskEXIT_CRITICAL();
        return RuntimeOwnerIngressResult::RejectedInactive;
    }

    const BaseType_t send_result = xQueueSend(queue, &value, 0);
    if (send_result != pdPASS) {
        if (send_result == errQUEUE_FULL) {
            taskENTER_CRITICAL();
            increment_saturating(g_status.rejected_full_count);
            taskEXIT_CRITICAL();
        }
        return RuntimeOwnerIngressResult::RejectedFull;
    }

    (void)xTaskNotifyGive(task_handle);
    return RuntimeOwnerIngressResult::AcceptedForDelivery;
}

} // namespace

TemperatureAlarmDeliveryPopResult
runtime_owner_try_receive_temperature_alarm_delivery(
    TemperatureAlarmDeliveryEvent &event) noexcept
{
    const TemperatureAlarmDeliveryPopResult result =
        g_owner_loop.try_pop_alarm_delivery(event);
    if (result == TemperatureAlarmDeliveryPopResult::Popped) {
        TaskHandle_t task_handle{nullptr};
        taskENTER_CRITICAL();
        task_handle = g_task_handle;
        taskEXIT_CRITICAL();
        if (task_handle != nullptr) {
            (void)xTaskNotifyGive(task_handle);
        }
    }
    return result;
}

bool runtime_owner_usb_power_present() noexcept
{
    return stdio_usb_connected();
}

RuntimeOwnerStartResult runtime_owner_rtos_start() noexcept
{
    std::uint8_t started{0};
    std::uint8_t start_failed{0};

    taskENTER_CRITICAL();
    started = g_status.started;
    start_failed = g_status.start_failed;
    taskEXIT_CRITICAL();

    if (started != 0) {
        return RuntimeOwnerStartResult::AlreadyStarted;
    }
    if (start_failed != 0) {
        return RuntimeOwnerStartResult::Failed;
    }

    g_urgent_queue = xQueueCreateStatic(
        kUrgentDepth,
        sizeof(RuntimeOwnerUrgentMessage),
        g_urgent_storage,
        &g_urgent_queue_buffer);
    g_control_queue = xQueueCreateStatic(
        kControlDepth,
        sizeof(RuntimeOwnerControlMessage),
        g_control_storage,
        &g_control_queue_buffer);
    g_normal_queue = xQueueCreateStatic(
        kNormalDepth,
        sizeof(NormalIntent),
        g_normal_storage,
        &g_normal_queue_buffer);

    if (g_urgent_queue == nullptr || g_control_queue == nullptr ||
        g_normal_queue == nullptr) {
        taskENTER_CRITICAL();
        g_status.start_failed = 1;
        taskEXIT_CRITICAL();
        return RuntimeOwnerStartResult::Failed;
    }

    g_task_handle = xTaskCreateStaticAffinitySet(
        runtime_owner_task_entry,
        "RuntimeOwner",
        kRuntimeOwnerStackWords,
        nullptr,
        kRuntimeOwnerPriority,
        g_task_stack,
        &g_task_buffer,
        kRuntimeOwnerCoreMask);
    if (g_task_handle == nullptr) {
        taskENTER_CRITICAL();
        g_status.start_failed = 1;
        taskEXIT_CRITICAL();
        return RuntimeOwnerStartResult::Failed;
    }

    taskENTER_CRITICAL();
    g_status.started = 1;
    g_status.ingress_enabled = 0;
    g_drain_metrics.receiver_ready = 1;
    g_drain_metrics.cutover_ready = 0;
    taskEXIT_CRITICAL();
    return RuntimeOwnerStartResult::Started;
}

RuntimeOwnerAtomicCutoverResult runtime_owner_rtos_activate_atomic() noexcept
{
    if (!g_device_backend.prepare()) {
        return RuntimeOwnerAtomicCutoverResult::RejectedNotReady;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        return RuntimeOwnerAtomicCutoverResult::RejectedNotQuiesced;
    }

    std::uint8_t started{0};
    std::uint8_t ingress_enabled{0};
    std::uint8_t cutover_ready{0};
    taskENTER_CRITICAL();
    started = g_status.started;
    ingress_enabled = g_status.ingress_enabled;
    cutover_ready = g_drain_metrics.cutover_ready;
    taskEXIT_CRITICAL();

    if (ingress_enabled != 0 && cutover_ready != 0) {
        return RuntimeOwnerAtomicCutoverResult::AlreadyActive;
    }
    if (started == 0 || g_urgent_queue == nullptr ||
        g_control_queue == nullptr || g_normal_queue == nullptr) {
        return RuntimeOwnerAtomicCutoverResult::RejectedNotStarted;
    }
    if (ingress_enabled != 0 || cutover_ready != 0) {
        return RuntimeOwnerAtomicCutoverResult::RejectedInvalid;
    }

    const bool queues_empty =
        uxQueueMessagesWaiting(g_urgent_queue) == 0 &&
        uxQueueMessagesWaiting(g_control_queue) == 0 &&
        uxQueueMessagesWaiting(g_normal_queue) == 0;
    if (!queues_empty || !g_owner_loop.physical_inflight_idle()) {
        return RuntimeOwnerAtomicCutoverResult::RejectedNotQuiesced;
    }

    const RuntimeOwnerActivationFacts facts{1, 1, 1, 1, 1, {}};
    if (runtime_owner_activation_preflight(
            facts, kRuntimeOwnerStableCutoverIdentity) !=
        RuntimeOwnerActivationPreflightResult::Ready) {
        return RuntimeOwnerAtomicCutoverResult::RejectedNotReady;
    }
    if (g_cutover_core.prepare(
            facts, kRuntimeOwnerStableCutoverIdentity) !=
        RuntimeOwnerCutoverResult::Prepared) {
        return RuntimeOwnerAtomicCutoverResult::RejectedNotReady;
    }

    const RuntimeOwnerTaskActivationResult activation =
        g_cutover_coordinator.activate(
            g_task_core, kRuntimeOwnerStableCutoverIdentity);
    if (activation != RuntimeOwnerTaskActivationResult::Activated) {
        (void)g_cutover_core.fail(false);
        return RuntimeOwnerAtomicCutoverResult::RejectedNotReady;
    }
    publish_redacted_status_cache();
    if (g_cutover_core.commit(kRuntimeOwnerStableCutoverIdentity) !=
        RuntimeOwnerCutoverResult::Committed) {
        (void)g_cutover_core.fail(false);
        return RuntimeOwnerAtomicCutoverResult::RejectedNotReady;
    }

    taskENTER_CRITICAL();
    g_drain_metrics.cutover_ready = 1;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    g_status.ingress_enabled = 1;
    taskEXIT_CRITICAL();
    return RuntimeOwnerAtomicCutoverResult::Activated;
}

namespace runtime_owner_rtos_detail {

RuntimeOwnerIngressResult submit_control(
    const RuntimeOwnerControlMessage value) noexcept
{
    return try_submit(g_control_queue, value);
}

RuntimeOwnerIngressResult submit_normal(const NormalIntent value) noexcept
{
    return try_submit(g_normal_queue, value);
}

RuntimeOwnerIngressResult submit_urgent(
    const RuntimeOwnerUrgentMessage value) noexcept
{
    return try_submit(g_urgent_queue, value);
}

} // namespace runtime_owner_rtos_detail

RuntimeOwnerRtosStatus runtime_owner_rtos_status() noexcept
{
    RuntimeOwnerRtosStatus status{};
    taskENTER_CRITICAL();
    status = g_status;
    taskEXIT_CRITICAL();
    return status;
}

RuntimeOwnerRedactedStatus runtime_owner_redacted_status() noexcept
{
    RuntimeOwnerRedactedStatus status{};
    taskENTER_CRITICAL();
    status = g_redacted_status_cache;
    taskEXIT_CRITICAL();
    return status;
}

RuntimeOwnerRtosDrainMetrics runtime_owner_rtos_drain_metrics() noexcept
{
    RuntimeOwnerRtosDrainMetrics metrics{};
    taskENTER_CRITICAL();
    metrics = g_drain_metrics;
    taskEXIT_CRITICAL();
    return metrics;
}

} // namespace boot_v2
