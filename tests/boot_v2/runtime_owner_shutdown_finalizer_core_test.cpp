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

void test_latest_usb_state_selects_automatic_final_action()
{
    struct Case {
        bool initial_present;
        bool latest_present;
        RuntimeOwnerShutdownUsbResult usb_result;
        RuntimeOwnerShutdownFinalizeAction action;
    };
    constexpr std::array<Case, 2> cases{{
        {true,
         false,
         RuntimeOwnerShutdownUsbResult::Gp15Allowed,
         RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill},
        {false,
         true,
         RuntimeOwnerShutdownUsbResult::WatchdogAllowed,
         RuntimeOwnerShutdownFinalizeAction::CommitWatchdog},
    }};

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const Case &test_case = cases[index];
        const std::uint32_t started_at =
            static_cast<std::uint32_t>(1000 + index * 1000);
        RuntimeOwnerShutdownFinalizerCore core{};
        CHECK(core.start(
                  power_button(
                      static_cast<std::uint32_t>(20 + index),
                      static_cast<std::uint32_t>(120 + index)),
                  usb(test_case.initial_present, 10, started_at),
                  started_at,
                  90000) == RuntimeOwnerShutdownStartResult::Started);
        for (const auto step : kSteps) {
            CHECK(core.complete(
                      step,
                      RuntimeOwnerShutdownStepResult::Succeeded,
                      started_at + 100) ==
                  RuntimeOwnerShutdownCompletionResult::Accepted);
        }
        CHECK(core.submit_usb_recheck(
                  usb(test_case.latest_present, 11, started_at + 200)) ==
              test_case.usb_result);
        CHECK(core.next(started_at + 201).action == test_case.action);
    }
}

void test_malformed_and_stale_usb_are_rejected_before_fresh_sample()
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
    UsbPowerObservation malformed = usb(true, 21, 6001);
    malformed.present = 2;
    CHECK(core.submit_usb_recheck(malformed) ==
          RuntimeOwnerShutdownUsbResult::RejectedInvalid);
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

void test_poweroff_outcome_is_best_effort_and_masks_are_preserved()
{
    struct Case {
        RuntimeOwnerShutdownStepResult poweroff_result;
        bool latest_present;
        RuntimeOwnerShutdownUsbResult usb_result;
        RuntimeOwnerShutdownFinalizeAction action;
    };
    constexpr std::array<Case, 3> cases{{
        {RuntimeOwnerShutdownStepResult::Failed,
         false,
         RuntimeOwnerShutdownUsbResult::Gp15Allowed,
         RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill},
        {RuntimeOwnerShutdownStepResult::TimedOut,
         true,
         RuntimeOwnerShutdownUsbResult::WatchdogAllowed,
         RuntimeOwnerShutdownFinalizeAction::CommitWatchdog},
        {RuntimeOwnerShutdownStepResult::Skipped,
         false,
         RuntimeOwnerShutdownUsbResult::Gp15Allowed,
         RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill},
    }};
    const std::uint8_t poweroff_mask =
        runtime_owner_shutdown_step_mask(
            RuntimeOwnerShutdownCleanupStep::PowerOffModem);

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const Case &test_case = cases[index];
        const std::uint32_t started_at =
            static_cast<std::uint32_t>(7000 + index * 1000);
        RuntimeOwnerShutdownFinalizerCore core{};
        CHECK(core.start(
                  power_button(
                      static_cast<std::uint32_t>(30 + index),
                      static_cast<std::uint32_t>(130 + index)),
                  usb(!test_case.latest_present, 30, started_at),
                  started_at,
                  90000) == RuntimeOwnerShutdownStartResult::Started);
        for (const auto step : kSteps) {
            const RuntimeOwnerShutdownStepResult result =
                step == RuntimeOwnerShutdownCleanupStep::PowerOffModem
                    ? test_case.poweroff_result
                    : RuntimeOwnerShutdownStepResult::Succeeded;
            CHECK(core.complete(step, result, started_at + 100) ==
                  RuntimeOwnerShutdownCompletionResult::Accepted);
        }

        const RuntimeOwnerShutdownDirective recheck =
            core.next(started_at + 200);
        CHECK(recheck.action ==
              RuntimeOwnerShutdownFinalizeAction::RecheckUsb);
        CHECK(recheck.cleanup_failed_mask ==
              (test_case.poweroff_result ==
                       RuntimeOwnerShutdownStepResult::Failed
                   ? poweroff_mask
                   : 0));
        CHECK(recheck.cleanup_timed_out_mask ==
              (test_case.poweroff_result ==
                       RuntimeOwnerShutdownStepResult::TimedOut
                   ? poweroff_mask
                   : 0));
        CHECK(recheck.cleanup_skipped_mask ==
              (test_case.poweroff_result ==
                       RuntimeOwnerShutdownStepResult::Skipped
                   ? poweroff_mask
                   : 0));

        UsbPowerObservation latest =
            usb(test_case.latest_present, 31, started_at + 201);
        latest.cleanup_timed_out_mask =
            recheck.cleanup_timed_out_mask;
        latest.cleanup_skipped_mask =
            recheck.cleanup_skipped_mask;
        CHECK(core.submit_usb_recheck(latest) == test_case.usb_result);
        const RuntimeOwnerShutdownDirective final =
            core.next(started_at + 202);
        CHECK(final.action == test_case.action);
        CHECK(final.cleanup_failed_mask ==
              recheck.cleanup_failed_mask);
        CHECK(final.cleanup_timed_out_mask ==
              recheck.cleanup_timed_out_mask);
        CHECK(final.cleanup_skipped_mask ==
              recheck.cleanup_skipped_mask);
    }
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
            const RuntimeOwnerShutdownStepResult result =
                !initial_present &&
                        step ==
                            RuntimeOwnerShutdownCleanupStep::PowerOffModem
                    ? RuntimeOwnerShutdownStepResult::Failed
                    : RuntimeOwnerShutdownStepResult::Succeeded;
            CHECK(core.complete(
                      step,
                      result,
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

void test_deadline_is_inclusive_and_uses_fresh_usb()
{
    struct Case {
        bool latest_present;
        RuntimeOwnerShutdownUsbResult usb_result;
        RuntimeOwnerShutdownFinalizeAction action;
    };
    constexpr std::array<Case, 2> cases{{
        {false,
         RuntimeOwnerShutdownUsbResult::Gp15Allowed,
         RuntimeOwnerShutdownFinalizeAction::CommitGp15Kill},
        {true,
         RuntimeOwnerShutdownUsbResult::WatchdogAllowed,
         RuntimeOwnerShutdownFinalizeAction::CommitWatchdog},
    }};

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const Case &test_case = cases[index];
        const std::uint32_t started_at =
            static_cast<std::uint32_t>(100 + index * 2000);
        RuntimeOwnerShutdownFinalizerCore core{};
        CHECK(core.start(
                  power_button(
                      static_cast<std::uint32_t>(40 + index),
                      static_cast<std::uint32_t>(140 + index)),
                  usb(!test_case.latest_present, 30, started_at),
                  started_at,
                  1000) == RuntimeOwnerShutdownStartResult::Started);
        CHECK(core.next(started_at + 999).action ==
              RuntimeOwnerShutdownFinalizeAction::RunCleanupStep);
        const RuntimeOwnerShutdownDirective deadline =
            core.next(started_at + 1000);
        CHECK(deadline.action ==
              RuntimeOwnerShutdownFinalizeAction::RecheckUsb);
        CHECK(deadline.hard_deadline == 1);
        CHECK(deadline.remaining_ms == 0);
        CHECK(core.complete(
                  RuntimeOwnerShutdownCleanupStep::StopOutputs,
                  RuntimeOwnerShutdownStepResult::Succeeded,
                  started_at + 1000) ==
              RuntimeOwnerShutdownCompletionResult::RejectedTerminal);

        UsbPowerObservation final_usb =
            usb(test_case.latest_present, 31, started_at + 1001);
        final_usb.hard_deadline_reached = 1;
        CHECK(core.submit_usb_recheck(final_usb) ==
              test_case.usb_result);
        const RuntimeOwnerShutdownDirective final =
            core.next(started_at + 1002);
        CHECK(final.action == test_case.action);
        CHECK(final.hard_deadline == 1);
    }

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
    test_latest_usb_state_selects_automatic_final_action();
    test_malformed_and_stale_usb_are_rejected_before_fresh_sample();
    test_poweroff_outcome_is_best_effort_and_masks_are_preserved();
    test_authenticated_reboot_always_commits_watchdog();
    test_authenticated_power_off_preserves_usb_policy();
    test_deadline_is_inclusive_and_uses_fresh_usb();
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
