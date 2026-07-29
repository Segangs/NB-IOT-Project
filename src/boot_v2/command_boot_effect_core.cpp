#include "command_boot_effect_core.hpp"

namespace boot_v2 {
namespace {

bool watchdog_evidence_matches(
    const CommandJournalRecord &journal,
    const CommandBootEffectEvidence &evidence) noexcept
{
    return evidence.shutdown_record.planned_action ==
               RuntimeOwnerShutdownPlannedAction::WatchdogReboot &&
           evidence.watchdog_marker_present == 1 &&
           evidence.watchdog_cmd_id == journal.cmd_id;
}

} // namespace

bool command_boot_effect_matches(
    const CommandJournalRecord &journal,
    const std::uint32_t current_boot_sequence,
    const CommandBootEffectEvidence &evidence) noexcept
{
    const bool common =
        journal.state == CommandJournalState::ExecuteMarked &&
        journal.dispatch_latched == 1 &&
        journal.cmd_id != 0 &&
        current_boot_sequence != 0 &&
        static_cast<std::int32_t>(
            current_boot_sequence -
            journal.boot_sequence_before_execute) > 0 &&
        evidence.shutdown_record_present == 1 &&
        evidence.watchdog_marker_present <= 1 &&
        evidence.reserved[0] == 0 &&
        evidence.reserved[1] == 0 &&
        runtime_owner_shutdown_record_valid(evidence.shutdown_record) &&
        evidence.shutdown_record.reason == 3 &&
        evidence.shutdown_record.producer_sequence == journal.cmd_id &&
        evidence.shutdown_record.incident_correlation_id == journal.cmd_id;
    if (!common) {
        return false;
    }

    if (journal.opcode == CommandOpcode::Reboot) {
        return journal.expected_effect == CommandExpectedEffect::Reset &&
               watchdog_evidence_matches(journal, evidence);
    }
    if (journal.opcode != CommandOpcode::PowerOff ||
        journal.expected_effect != CommandExpectedEffect::PowerOff) {
        return false;
    }
    const bool backend_canonical_record =
        (evidence.shutdown_record.initial_usb_present == 0 &&
         evidence.shutdown_record.planned_action ==
             RuntimeOwnerShutdownPlannedAction::Gp15Kill) ||
        (evidence.shutdown_record.initial_usb_present == 1 &&
         evidence.shutdown_record.planned_action ==
             RuntimeOwnerShutdownPlannedAction::WatchdogReboot);
    if (!backend_canonical_record) {
        return false;
    }
    if (evidence.watchdog_marker_present == 1) {
        return evidence.watchdog_cmd_id == journal.cmd_id;
    }
    return true;
}

} // namespace boot_v2
