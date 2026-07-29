#include "command_boot_effect_core.hpp"

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

constexpr CommandExpectedEffect effect_for(
    const CommandOpcode opcode) noexcept
{
    return opcode == CommandOpcode::Reboot
               ? CommandExpectedEffect::Reset
               : opcode == CommandOpcode::PowerOff
                     ? CommandExpectedEffect::PowerOff
                     : CommandExpectedEffect::None;
}

CommandJournalRecord execute_marked(
    const CommandOpcode opcode,
    const std::uint32_t cmd_id,
    const std::uint32_t boot_sequence) noexcept
{
    CommandJournalRecord record{};
    record.state = CommandJournalState::ExecuteMarked;
    record.opcode = opcode;
    record.phase = CommandAckPhase::Accepted;
    record.result = CommandResult::Accepted;
    record.expected_effect = effect_for(opcode);
    record.dispatch_latched = 1;
    record.cmd_id = cmd_id;
    record.ttl_seconds = 120;
    record.remaining_ttl_seconds = 120;
    record.ttl_checkpoint_boot_sequence = boot_sequence;
    record.boot_sequence_before_execute = boot_sequence;
    return record;
}

RuntimeOwnerShutdownRecordV1 shutdown_record(
    const std::uint32_t cmd_id,
    const bool initial_usb_present,
    const RuntimeOwnerShutdownPlannedAction action,
    const std::uint8_t reason = 3) noexcept
{
    RuntimeOwnerShutdownRecordInput input{};
    input.producer_sequence = cmd_id;
    input.incident_correlation_id = cmd_id;
    input.elapsed_ms = 1200;
    input.reason = reason;
    input.initial_usb_present = initial_usb_present ? 1 : 0;
    input.planned_action = action;
    input.cleanup_succeeded_mask = 0x7F;
    return runtime_owner_shutdown_record_make(input, 17);
}

CommandBootEffectEvidence watchdog_evidence(
    const std::uint32_t cmd_id,
    const bool initial_usb_present) noexcept
{
    CommandBootEffectEvidence evidence{};
    evidence.shutdown_record_present = 1;
    evidence.watchdog_marker_present = 1;
    evidence.watchdog_cmd_id = cmd_id;
    evidence.shutdown_record = shutdown_record(
        cmd_id,
        initial_usb_present,
        RuntimeOwnerShutdownPlannedAction::WatchdogReboot);
    return evidence;
}

CommandBootEffectEvidence gp15_evidence(
    const std::uint32_t cmd_id) noexcept
{
    CommandBootEffectEvidence evidence{};
    evidence.shutdown_record_present = 1;
    evidence.shutdown_record = shutdown_record(
        cmd_id,
        false,
        RuntimeOwnerShutdownPlannedAction::Gp15Kill);
    return evidence;
}

void test_exact_positive_evidence() noexcept
{
    CHECK(command_boot_effect_matches(
        execute_marked(CommandOpcode::Reboot, 41, 7),
        8,
        watchdog_evidence(41, false)));
    CHECK(command_boot_effect_matches(
        execute_marked(CommandOpcode::Reboot, 42, 7),
        8,
        watchdog_evidence(42, true)));
    CHECK(command_boot_effect_matches(
        execute_marked(CommandOpcode::PowerOff, 43, 7),
        8,
        watchdog_evidence(43, true)));
    CHECK(command_boot_effect_matches(
        execute_marked(CommandOpcode::PowerOff, 44, 7),
        8,
        gp15_evidence(44)));
}

void test_poweroff_initial_usb_present_accepts_actual_gp15_evidence() noexcept
{
    const CommandJournalRecord power_off =
        execute_marked(CommandOpcode::PowerOff, 45, 7);
    CommandBootEffectEvidence actual_gp15 =
        watchdog_evidence(45, true);
    actual_gp15.watchdog_marker_present = 0;
    actual_gp15.watchdog_cmd_id = 0;

    CHECK(command_boot_effect_matches(power_off, 8, actual_gp15));
}

void test_poweroff_initial_usb_absent_requires_exact_actual_watchdog_marker()
    noexcept
{
    const CommandJournalRecord power_off =
        execute_marked(CommandOpcode::PowerOff, 46, 7);
    CommandBootEffectEvidence actual_watchdog =
        gp15_evidence(46);
    actual_watchdog.watchdog_marker_present = 1;
    actual_watchdog.watchdog_cmd_id = 46;
    CHECK(command_boot_effect_matches(power_off, 8, actual_watchdog));

    actual_watchdog.watchdog_cmd_id = 99;
    CHECK(!command_boot_effect_matches(power_off, 8, actual_watchdog));
}

void test_poweroff_rejects_usb_absent_watchdog_record_pair() noexcept
{
    const CommandJournalRecord power_off =
        execute_marked(CommandOpcode::PowerOff, 47, 7);
    const CommandBootEffectEvidence marker_present =
        watchdog_evidence(47, false);
    CHECK(!command_boot_effect_matches(power_off, 8, marker_present));

    CommandBootEffectEvidence marker_absent = marker_present;
    marker_absent.watchdog_marker_present = 0;
    marker_absent.watchdog_cmd_id = 0;
    CHECK(!command_boot_effect_matches(power_off, 8, marker_absent));
}

void test_poweroff_rejects_usb_present_gp15_record_pair() noexcept
{
    const CommandJournalRecord power_off =
        execute_marked(CommandOpcode::PowerOff, 48, 7);
    CommandBootEffectEvidence marker_absent{};
    marker_absent.shutdown_record_present = 1;
    marker_absent.shutdown_record = shutdown_record(
        48,
        true,
        RuntimeOwnerShutdownPlannedAction::Gp15Kill);
    CHECK(!command_boot_effect_matches(power_off, 8, marker_absent));

    CommandBootEffectEvidence marker_present = marker_absent;
    marker_present.watchdog_marker_present = 1;
    marker_present.watchdog_cmd_id = 48;
    CHECK(!command_boot_effect_matches(power_off, 8, marker_present));
}

void test_identity_reason_action_and_marker_must_match() noexcept
{
    const CommandJournalRecord reboot =
        execute_marked(CommandOpcode::Reboot, 51, 9);

    CHECK(!command_boot_effect_matches(
        reboot, 10, watchdog_evidence(52, true)));

    CommandBootEffectEvidence wrong_record_identity =
        watchdog_evidence(51, true);
    wrong_record_identity.shutdown_record = shutdown_record(
        52,
        true,
        RuntimeOwnerShutdownPlannedAction::WatchdogReboot);
    CHECK(!command_boot_effect_matches(
        reboot, 10, wrong_record_identity));

    CommandBootEffectEvidence wrong_reason = watchdog_evidence(51, true);
    wrong_reason.shutdown_record = shutdown_record(
        51,
        true,
        RuntimeOwnerShutdownPlannedAction::WatchdogReboot,
        2);
    CHECK(!command_boot_effect_matches(reboot, 10, wrong_reason));

    CommandBootEffectEvidence wrong_action = watchdog_evidence(51, true);
    wrong_action.shutdown_record = shutdown_record(
        51,
        true,
        RuntimeOwnerShutdownPlannedAction::Gp15Kill);
    CHECK(!command_boot_effect_matches(reboot, 10, wrong_action));

    CommandBootEffectEvidence no_marker = watchdog_evidence(51, true);
    no_marker.watchdog_marker_present = 0;
    CHECK(!command_boot_effect_matches(reboot, 10, no_marker));

    CommandBootEffectEvidence wrong_scratch = watchdog_evidence(51, true);
    wrong_scratch.watchdog_cmd_id = 99;
    CHECK(!command_boot_effect_matches(reboot, 10, wrong_scratch));
}

void test_boot_journal_crc_and_shape_fail_closed() noexcept
{
    const CommandJournalRecord reboot =
        execute_marked(CommandOpcode::Reboot, 61, 11);
    const CommandBootEffectEvidence evidence =
        watchdog_evidence(61, true);
    CHECK(!command_boot_effect_matches(reboot, 11, evidence));

    CommandBootEffectEvidence corrupt = evidence;
    corrupt.shutdown_record.crc32 ^= 1u;
    CHECK(!command_boot_effect_matches(reboot, 12, corrupt));

    CommandJournalRecord status =
        execute_marked(CommandOpcode::RequestStatus, 61, 11);
    CHECK(!command_boot_effect_matches(status, 12, evidence));
    CommandJournalRecord fota =
        execute_marked(CommandOpcode::FotaPrepare, 61, 11);
    CHECK(!command_boot_effect_matches(fota, 12, evidence));

    CommandJournalRecord not_marked = reboot;
    not_marked.state = CommandJournalState::AcceptedReceipted;
    not_marked.dispatch_latched = 0;
    CHECK(!command_boot_effect_matches(not_marked, 12, evidence));

    CommandBootEffectEvidence dirty = evidence;
    dirty.reserved[1] = 1;
    CHECK(!command_boot_effect_matches(reboot, 12, dirty));

    CommandBootEffectEvidence invalid_bit = evidence;
    invalid_bit.shutdown_record_present = 2;
    CHECK(!command_boot_effect_matches(reboot, 12, invalid_bit));
}

} // namespace

int main()
{
    test_exact_positive_evidence();
    test_poweroff_initial_usb_present_accepts_actual_gp15_evidence();
    test_poweroff_initial_usb_absent_requires_exact_actual_watchdog_marker();
    test_poweroff_rejects_usb_absent_watchdog_record_pair();
    test_poweroff_rejects_usb_present_gp15_record_pair();
    test_identity_reason_action_and_marker_must_match();
    test_boot_journal_crc_and_shape_fail_closed();

    if (failures != 0) {
        std::printf(
            "COMMAND_BOOT_EFFECT_CORE_TEST FAIL checks=%zu failures=%zu\n",
            checks,
            failures);
        return 1;
    }
    std::printf(
        "COMMAND_BOOT_EFFECT_CORE_TEST PASS checks=%zu\n",
        checks);
    return 0;
}
