#include "runtime_owner_shutdown_finalizer_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

using namespace boot_v2;

std::size_t checks = 0;
std::size_t failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                         \
                         __FILE__, __LINE__, #condition);                      \
        }                                                                      \
    } while (false)

constexpr RuntimeOwnerUrgentMessage power_button(
    const std::uint32_t sequence = 11,
    const std::uint32_t correlation = 101) noexcept
{
    return {
        RuntimeOwnerUrgentSource::PowerButton,
        RuntimeOwnerShutdownIntent::AutomaticByUsb,
        {},
        sequence,
        correlation};
}

constexpr RuntimeOwnerUrgentMessage authenticated(
    const RuntimeOwnerShutdownIntent intent,
    const std::uint32_t sequence = 12,
    const std::uint32_t correlation = 102) noexcept
{
    return {
        RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand,
        intent,
        {},
        sequence,
        correlation};
}

constexpr UsbPowerObservation usb(
    const bool present,
    const std::uint32_t sequence,
    const std::uint32_t sampled_at) noexcept
{
    UsbPowerObservation result{};
    result.present = present ? 1 : 0;
    result.sample_sequence = sequence;
    result.sampled_at_monotonic_ms = sampled_at;
    return result;
}

constexpr std::array<RuntimeOwnerShutdownCleanupStep, 7> kSteps{{
    RuntimeOwnerShutdownCleanupStep::StopOutputs,
    RuntimeOwnerShutdownCleanupStep::PublishDying,
    RuntimeOwnerShutdownCleanupStep::CloseDeleteSessions,
    RuntimeOwnerShutdownCleanupStep::ScanSessions,
    RuntimeOwnerShutdownCleanupStep::DisconnectPdp,
    RuntimeOwnerShutdownCleanupStep::SetCfun0,
    RuntimeOwnerShutdownCleanupStep::PowerOffModem,
}};

void test_start_validation_and_exact_context()
{
    RuntimeOwnerShutdownFinalizerCore core{};
    CHECK(core.next(1000).action == RuntimeOwnerShutdownFinalizeAction::Idle);
    CHECK(core.start({}, usb(true, 1, 1000), 1000, 90000) ==
          RuntimeOwnerShutdownStartResult::RejectedInvalid);
    CHECK(core.start(power_button(), usb(true, 1, 1000), 1000, 0) ==
          RuntimeOwnerShutdownStartResult::RejectedInvalid);

    CHECK(core.start(power_button(), usb(false, 1, 1000), 1000, 90000) ==
          RuntimeOwnerShutdownStartResult::Started);
    CHECK(core.start(power_button(), usb(false, 1, 1000), 1000, 90000) ==
          RuntimeOwnerShutdownStartResult::AcceptedDuplicate);
    CHECK(core.start(power_button(12, 102), usb(false, 1, 1000),
                     1000, 90000) ==
          RuntimeOwnerShutdownStartResult::RejectedUnsafe);

    RuntimeOwnerUrgentMessage copied{};
    CHECK(core.shutdown_context(copied));
    CHECK(copied.source == RuntimeOwnerUrgentSource::PowerButton);
    CHECK(copied.intent == RuntimeOwnerShutdownIntent::AutomaticByUsb);
    CHECK(copied.producer_sequence == 11);
    CHECK(copied.incident_correlation_id == 101);
}

void test_order_failure_progress_and_masks()
{
    RuntimeOwnerShutdownFinalizerCore core{};
    CHECK(core.start(power_button(), usb(true, 3, 500), 500, 90000) ==
          RuntimeOwnerShutdownStartResult::Started);

    for (std::size_t index = 0; index < kSteps.size(); ++index) {
        const RuntimeOwnerShutdownDirective directive =
            core.next(static_cast<std::uint32_t>(600 + index));
        CHECK(directive.action ==
              RuntimeOwnerShutdownFinalizeAction::RunCleanupStep);
        CHECK(directive.step == kSteps[index]);
        CHECK(directive.hard_deadline == 0);
        CHECK(directive.remaining_ms > 0);

        if (index == 0) {
            CHECK(core.complete(
                      RuntimeOwnerShutdownCleanupStep::PublishDying,
                      RuntimeOwnerShutdownStepResult::Succeeded,
                      600) ==
                  RuntimeOwnerShutdownCompletionResult::RejectedWrongStep);
        }

        RuntimeOwnerShutdownStepResult result =
            RuntimeOwnerShutdownStepResult::Succeeded;
        if (index == 1) {
            result = RuntimeOwnerShutdownStepResult::Failed;
        } else if (index == 2) {
            result = RuntimeOwnerShutdownStepResult::TimedOut;
        } else if (index == 3) {
            result = RuntimeOwnerShutdownStepResult::Skipped;
        }
        CHECK(core.complete(kSteps[index], result,
                            static_cast<std::uint32_t>(700 + index)) ==
              RuntimeOwnerShutdownCompletionResult::Accepted);
        const auto duplicate =
            core.complete(kSteps[index], result,
                          static_cast<std::uint32_t>(700 + index));
        CHECK(
            duplicate ==
            (index + 1 == kSteps.size()
                 ? RuntimeOwnerShutdownCompletionResult::RejectedTerminal
                 : RuntimeOwnerShutdownCompletionResult::RejectedWrongStep));
    }

    const RuntimeOwnerShutdownDirective recheck = core.next(800);
    CHECK(recheck.action == RuntimeOwnerShutdownFinalizeAction::RecheckUsb);
    CHECK(recheck.cleanup_timed_out_mask ==
          runtime_owner_shutdown_step_mask(
              RuntimeOwnerShutdownCleanupStep::CloseDeleteSessions));
    CHECK(recheck.cleanup_skipped_mask ==
          runtime_owner_shutdown_step_mask(
              RuntimeOwnerShutdownCleanupStep::ScanSessions));
    CHECK(recheck.cleanup_failed_mask ==
          runtime_owner_shutdown_step_mask(
              RuntimeOwnerShutdownCleanupStep::PublishDying));
    CHECK(recheck.initial_usb_present == 1);
}

void test_usb_change_fails_closed()
{
    RuntimeOwnerShutdownFinalizerCore core{};
    CHECK(core.start(power_button(), usb(true, 10, 1000),
                     1000, 90000) ==
          RuntimeOwnerShutdownStartResult::Started);
    for (const auto step : kSteps) {
        CHECK(core.complete(step, RuntimeOwnerShutdownStepResult::Succeeded,
                            2000) ==
              RuntimeOwnerShutdownCompletionResult::Accepted);
    }
    CHECK(core.submit_usb_recheck(usb(false, 11, 3000)) ==
          RuntimeOwnerShutdownUsbResult::UsbChanged);
    CHECK(core.next(3001).action ==
          RuntimeOwnerShutdownFinalizeAction::AbortUsbChanged);
    CHECK(core.submit_usb_recheck(usb(true, 12, 3002)) ==
          RuntimeOwnerShutdownUsbResult::RejectedTerminal);
}

void test_usb_present_allows_watchdog_only_after_fresh_sample()
{
    RuntimeOwnerShutdownFinalizerCore core{};
    CHECK(core.start(power_button(), usb(true, 20, 5000),
                     5000, 90000) ==
          RuntimeOwnerShutdownStartResult::Started);
    for (const auto step : kSteps) {
        CHECK(core.complete(step, RuntimeOwnerShutdownStepResult::Succeeded,
                            6000) ==
              RuntimeOwnerShutdownCompletionResult::Accepted);
    }
    CHECK(core.submit_usb_recheck(usb(true, 20, 6001)) ==
          RuntimeOwnerShutdownUsbResult::RejectedInvalid);
    CHECK(core.submit_usb_recheck(usb(true, 21, 4999)) ==
          RuntimeOwnerShutdownUsbResult::RejectedInvalid);
    CHECK(core.submit_usb_recheck(usb(true, 21, 6001)) ==
          RuntimeOwnerShutdownUsbResult::WatchdogAllowed);
    const RuntimeOwnerShutdownDirective directive = core.next(6002);
    CHECK(directive.action ==
          RuntimeOwnerShutdownFinalizeAction::CommitWatchdog);
    CHECK(directive.hard_deadline == 0);
}

void test_usb_absent_allows_gp15_only_after_poweroff_evidence()
{
    RuntimeOwnerShutdownFinalizerCore core{};
    CHECK(core.start(power_button(), usb(false, 30, 7000),
                     7000, 90000) ==
          RuntimeOwnerShutdownStartResult::Started);
    for (const auto step : kSteps) {
        CHECK(core.complete(step, RuntimeOwnerShutdownStepResult::Succeeded,
                            8000) ==
              RuntimeOwnerShutdownCompletionResult::Accepted);
    }
    CHECK(core.submit_usb_recheck(usb(false, 31, 8001)) ==
          RuntimeOwnerShutdownUsbResult::Gp15Allowed);
    CHECK(core.next(8002).action ==
          RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill);

    RuntimeOwnerShutdownFinalizerCore missing_evidence{};
    CHECK(missing_evidence.start(
              power_button(), usb(false, 40, 9000), 9000, 90000) ==
          RuntimeOwnerShutdownStartResult::Started);
    for (const auto step : kSteps) {
        const RuntimeOwnerShutdownStepResult result =
            step == RuntimeOwnerShutdownCleanupStep::PowerOffModem
                ? RuntimeOwnerShutdownStepResult::Failed
                : RuntimeOwnerShutdownStepResult::Succeeded;
        CHECK(missing_evidence.complete(step, result, 10000) ==
              RuntimeOwnerShutdownCompletionResult::Accepted);
    }
    CHECK(missing_evidence.submit_usb_recheck(usb(false, 41, 10001)) ==
          RuntimeOwnerShutdownUsbResult::EvidenceMissing);
    const RuntimeOwnerShutdownDirective abort =
        missing_evidence.next(10002);
    CHECK(abort.action ==
          RuntimeOwnerShutdownFinalizeAction::AbortEvidenceMissing);
    CHECK(abort.cleanup_failed_mask ==
          runtime_owner_shutdown_step_mask(
              RuntimeOwnerShutdownCleanupStep::PowerOffModem));
}

void test_authenticated_reboot_always_commits_watchdog()
{
    for (const bool initial_present : {false, true}) {
        RuntimeOwnerShutdownFinalizerCore core{};
        CHECK(core.start(
                  authenticated(RuntimeOwnerShutdownIntent::Reboot),
                  usb(initial_present, 50, 11000),
                  11000,
                  90000) == RuntimeOwnerShutdownStartResult::Started);
        for (const auto step : kSteps) {
            CHECK(core.complete(
                      step,
                      RuntimeOwnerShutdownStepResult::Succeeded,
                      12000) ==
                  RuntimeOwnerShutdownCompletionResult::Accepted);
        }
        CHECK(core.submit_usb_recheck(
                  usb(initial_present, 51, 12001)) ==
              RuntimeOwnerShutdownUsbResult::WatchdogAllowed);
        CHECK(core.next(12002).action ==
              RuntimeOwnerShutdownFinalizeAction::CommitWatchdog);
    }
}

void test_authenticated_power_off_preserves_usb_policy()
{
    RuntimeOwnerShutdownFinalizerCore absent{};
    CHECK(absent.start(
              authenticated(RuntimeOwnerShutdownIntent::PowerOff),
              usb(false, 60, 13000),
              13000,
              90000) == RuntimeOwnerShutdownStartResult::Started);
    for (const auto step : kSteps) {
        CHECK(absent.complete(
                  step,
                  RuntimeOwnerShutdownStepResult::Succeeded,
                  14000) ==
              RuntimeOwnerShutdownCompletionResult::Accepted);
    }
    CHECK(absent.submit_usb_recheck(usb(false, 61, 14001)) ==
          RuntimeOwnerShutdownUsbResult::Gp15Allowed);

    RuntimeOwnerShutdownFinalizerCore present{};
    CHECK(present.start(
              authenticated(RuntimeOwnerShutdownIntent::PowerOff),
              usb(true, 70, 15000),
              15000,
              90000) == RuntimeOwnerShutdownStartResult::Started);
    for (const auto step : kSteps) {
        CHECK(present.complete(
                  step,
                  RuntimeOwnerShutdownStepResult::Succeeded,
                  16000) ==
              RuntimeOwnerShutdownCompletionResult::Accepted);
    }
    CHECK(present.submit_usb_recheck(usb(true, 71, 16001)) ==
          RuntimeOwnerShutdownUsbResult::WatchdogAllowed);
}

void test_deadline_is_inclusive_and_wrap_safe()
{
    RuntimeOwnerShutdownFinalizerCore core{};
    CHECK(core.start(power_button(), usb(true, 30, 100),
                     100, 1000) ==
          RuntimeOwnerShutdownStartResult::Started);
    CHECK(core.next(1099).action ==
          RuntimeOwnerShutdownFinalizeAction::RunCleanupStep);
    const RuntimeOwnerShutdownDirective deadline = core.next(1100);
    CHECK(deadline.action ==
          RuntimeOwnerShutdownFinalizeAction::RecheckUsb);
    CHECK(deadline.hard_deadline == 1);
    CHECK(deadline.remaining_ms == 0);
    CHECK(core.complete(
              RuntimeOwnerShutdownCleanupStep::StopOutputs,
              RuntimeOwnerShutdownStepResult::Succeeded,
              1100) ==
          RuntimeOwnerShutdownCompletionResult::RejectedTerminal);
    UsbPowerObservation final_usb = usb(true, 31, 1101);
    final_usb.hard_deadline_reached = 1;
    CHECK(core.submit_usb_recheck(final_usb) ==
          RuntimeOwnerShutdownUsbResult::EvidenceMissing);
    CHECK(core.next(1102).action ==
          RuntimeOwnerShutdownFinalizeAction::AbortEvidenceMissing);
    CHECK(core.next(1102).hard_deadline == 1);

    RuntimeOwnerShutdownFinalizerCore wrapping{};
    constexpr std::uint32_t started = 0xFFFFFFF0u;
    CHECK(wrapping.start(power_button(), usb(true, 40, started),
                         started, 32) ==
          RuntimeOwnerShutdownStartResult::Started);
    CHECK(wrapping.next(15).action ==
          RuntimeOwnerShutdownFinalizeAction::RunCleanupStep);
    CHECK(wrapping.next(16).action ==
          RuntimeOwnerShutdownFinalizeAction::RecheckUsb);
}

} // namespace

int main()
{
    test_start_validation_and_exact_context();
    test_order_failure_progress_and_masks();
    test_usb_change_fails_closed();
    test_usb_present_allows_watchdog_only_after_fresh_sample();
    test_usb_absent_allows_gp15_only_after_poweroff_evidence();
    test_authenticated_reboot_always_commits_watchdog();
    test_authenticated_power_off_preserves_usb_policy();
    test_deadline_is_inclusive_and_wrap_safe();
    if (failures != 0) {
        std::printf(
            "RUNTIME_OWNER_SHUTDOWN_FINALIZER_CORE_TEST FAIL "
            "checks=%zu failures=%zu\n",
            checks,
            failures);
        return 1;
    }
    std::printf(
        "RUNTIME_OWNER_SHUTDOWN_FINALIZER_CORE_TEST OK checks=%zu\n",
        checks);
    return 0;
}
