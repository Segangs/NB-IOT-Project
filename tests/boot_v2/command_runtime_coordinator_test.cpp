#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "command_runtime_coordinator.hpp"

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(
    const bool condition,
    const char *const expression,
    const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(
            stderr,
            "CHECK failed: %s:%d: %s\n",
            __FILE__,
            line,
            expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

using namespace boot_v2;

constexpr std::size_t kSectorSize = 4096;
constexpr std::size_t kPageSize = 256;

enum class Event : std::uint8_t {
    Commit,
    Publish,
    Snapshot,
    Dispatch,
};

struct FakeStorage {
    std::array<std::uint8_t, kSectorSize> slot_a{};
    std::array<std::uint8_t, kSectorSize> slot_b{};
    std::array<Event, 64> events{};
    std::size_t event_count{0};
    std::size_t read_count{0};
    std::size_t replace_count{0};
    std::size_t snapshot_count{0};
    std::size_t fail_read_call{0};
    bool snapshot_succeeds{true};
    bool fail_replace_before_write{false};
    bool fail_replace_after_write{false};
    bool fail_verify_read_after_write{false};
    bool fail_result_commit_before_write{false};
    bool corrupt_both_after_replace{false};

    FakeStorage() noexcept
    {
        slot_a.fill(0xFFu);
        slot_b.fill(0xFFu);
    }

    void event(const Event value) noexcept
    {
        if (event_count < events.size()) {
            events[event_count++] = value;
        }
    }
};

std::array<std::uint8_t, kSectorSize> *slot_bytes(
    FakeStorage &storage,
    const CommandJournalSlot slot) noexcept
{
    if (slot == CommandJournalSlot::A) {
        return &storage.slot_a;
    }
    if (slot == CommandJournalSlot::B) {
        return &storage.slot_b;
    }
    return nullptr;
}

bool fake_read(
    void *const context,
    const CommandJournalSlot slot,
    CommandJournalRecordV1 &output) noexcept
{
    if (context == nullptr) {
        return false;
    }
    auto &storage = *static_cast<FakeStorage *>(context);
    ++storage.read_count;
    if (storage.read_count == storage.fail_read_call) {
        return false;
    }
    const auto *const bytes = slot_bytes(storage, slot);
    if (bytes == nullptr) {
        return false;
    }
    std::memcpy(&output, bytes->data(), sizeof(output));
    return true;
}

bool fake_replace(
    void *const context,
    const CommandJournalSlot slot,
    const std::uint8_t *const page,
    const std::size_t page_size,
    const std::uint32_t timeout_ms) noexcept
{
    if (context == nullptr || page == nullptr ||
        page_size != kPageSize || timeout_ms == 0) {
        return false;
    }
    auto &storage = *static_cast<FakeStorage *>(context);
    ++storage.replace_count;
    storage.event(Event::Commit);
    if (storage.fail_replace_before_write) {
        storage.fail_replace_before_write = false;
        return false;
    }

    auto *const bytes = slot_bytes(storage, slot);
    if (bytes == nullptr) {
        return false;
    }
    bytes->fill(0xFFu);
    std::memcpy(bytes->data(), page, page_size);

    if (storage.corrupt_both_after_replace) {
        storage.corrupt_both_after_replace = false;
        storage.slot_a.fill(0);
        storage.slot_b.fill(0);
        return false;
    }
    if (storage.fail_verify_read_after_write) {
        storage.fail_verify_read_after_write = false;
        storage.fail_read_call = storage.read_count + 1u;
    }
    if (storage.fail_replace_after_write) {
        storage.fail_replace_after_write = false;
        return false;
    }
    return true;
}

CommandJournalStorePort port(FakeStorage &storage) noexcept
{
    return {&storage, fake_read, fake_replace};
}

CommandJournalRecord persistent_record(
    const CommandJournalState state,
    const CommandOpcode opcode = CommandOpcode::RequestStatus) noexcept
{
    CommandJournalRecord record{};
    record.state = state;
    record.opcode = opcode;
    record.phase =
        state >= CommandJournalState::Executed
            ? CommandAckPhase::Final
            : CommandAckPhase::Accepted;
    record.result =
        state >= CommandJournalState::Executed
            ? CommandResult::Executed
            : CommandResult::Accepted;
    record.error = CommandError::None;
    record.expected_effect =
        opcode == CommandOpcode::Reboot
            ? CommandExpectedEffect::Reset
            : opcode == CommandOpcode::PowerOff
                  ? CommandExpectedEffect::PowerOff
                  : CommandExpectedEffect::None;
    record.dispatch_latched =
        state == CommandJournalState::ExecuteMarked ? 1 : 0;
    record.cmd_id = 41;
    record.job_id = 91;
    record.ttl_seconds = 600;
    record.remaining_ttl_seconds = 590;
    record.ttl_checkpoint_monotonic_seconds = 10;
    record.ttl_checkpoint_boot_sequence = 7;
    record.boot_sequence_before_execute = 7;
    return record;
}

void write_record(
    FakeStorage &storage,
    const CommandJournalSlot slot,
    const CommandJournalRecord runtime,
    const std::uint32_t sequence) noexcept
{
    CommandJournalRecordV1 record{};
    CHECK(command_journal_record_encode(runtime, sequence, record));
    auto *const bytes = slot_bytes(storage, slot);
    CHECK(bytes != nullptr);
    if (bytes != nullptr) {
        bytes->fill(0xFFu);
        std::memcpy(bytes->data(), &record, sizeof(record));
    }
}

bool fresh_snapshot(void *const context) noexcept
{
    if (context == nullptr) {
        return false;
    }
    auto &storage = *static_cast<FakeStorage *>(context);
    ++storage.snapshot_count;
    storage.event(Event::Snapshot);
    if (storage.fail_result_commit_before_write) {
        storage.fail_result_commit_before_write = false;
        storage.fail_replace_before_write = true;
    }
    return storage.snapshot_succeeds;
}

struct FakeDispatch {
    FakeStorage *storage{nullptr};
    CommandRuntimeCoordinator *runtime{nullptr};
    std::size_t calls{0};
    CommandOpcode opcode{CommandOpcode::None};
    std::uint32_t cmd_id{0};
    CommandJournalState state_seen{CommandJournalState::Empty};
    bool accepts{true};
};

CommandShutdownDispatchResult dispatch_shutdown(
    void *const context,
    const CommandOpcode opcode,
    const std::uint32_t cmd_id) noexcept
{
    if (context == nullptr) {
        return CommandShutdownDispatchResult::Rejected;
    }
    auto &dispatch = *static_cast<FakeDispatch *>(context);
    ++dispatch.calls;
    dispatch.opcode = opcode;
    dispatch.cmd_id = cmd_id;
    if (dispatch.runtime != nullptr) {
        dispatch.state_seen = dispatch.runtime->state();
    }
    if (dispatch.storage != nullptr) {
        dispatch.storage->event(Event::Dispatch);
    }
    return dispatch.accepts
               ? CommandShutdownDispatchResult::Accepted
               : CommandShutdownDispatchResult::Rejected;
}

CommandBootEffectEvidence reboot_evidence(
    const std::uint32_t cmd_id) noexcept
{
    RuntimeOwnerShutdownRecordInput input{};
    input.producer_sequence = cmd_id;
    input.incident_correlation_id = cmd_id;
    input.elapsed_ms = 1200;
    input.reason = 3;
    input.initial_usb_present = 1;
    input.planned_action =
        RuntimeOwnerShutdownPlannedAction::WatchdogReboot;
    input.cleanup_succeeded_mask = 0x7F;

    CommandBootEffectEvidence evidence{};
    evidence.shutdown_record_present = 1;
    evidence.watchdog_marker_present = 1;
    evidence.watchdog_cmd_id = cmd_id;
    evidence.shutdown_record =
        runtime_owner_shutdown_record_make(input, 21);
    return evidence;
}

void advance_to_accepted_receipted(
    CommandRuntimeCoordinator &runtime,
    const CommandOpcode opcode,
    const std::uint32_t request_id,
    const std::uint32_t cmd_id) noexcept
{
    CHECK(runtime.begin_poll(request_id, 0));
    CHECK(runtime.accept_response(
              {request_id, cmd_id, opcode, 0, 600},
              100) == CommandAcceptResult::Accepted);
    CommandAckMessage ack{};
    CHECK(runtime.prepare_ack(ack) ==
          CommandTransitionResult::Accepted);
    CHECK(runtime.record_puback(CommandAckPhase::Accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(runtime.record_receipt(
              {cmd_id,
               CommandAckPhase::Accepted,
               CommandResult::Accepted,
               CommandAckReceiptCode::Ingested,
               0}) == CommandTransitionResult::Accepted);
}

void test_blank_and_loaded_boot_are_ready_without_spurious_commit() noexcept
{
    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(!runtime.ready());
        CHECK(!runtime.begin_poll(1, 0));
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        CHECK(runtime.ready());
        CHECK(runtime.state() == CommandJournalState::Empty);
        CHECK(storage.replace_count == 0);
        CHECK(runtime.current_boot_sequence() == 1);
    }

    {
        FakeStorage storage{};
        const CommandJournalRecord loaded =
            persistent_record(CommandJournalState::FinalPuback);
        write_record(storage, CommandJournalSlot::A, loaded, 3);
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyLoaded);
        CHECK(runtime.ready());
        CHECK(runtime.state() == CommandJournalState::FinalPuback);
        CHECK(runtime.record().cmd_id == loaded.cmd_id);
        CHECK(storage.replace_count == 0);
        CHECK(runtime.current_boot_sequence() == 8);
    }
}

void test_boot_recovery_is_durable_before_intake() noexcept
{
    {
        FakeStorage storage{};
        write_record(
            storage,
            CommandJournalSlot::A,
            persistent_record(CommandJournalState::ExecuteMarked),
            9);
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() ==
              CommandRuntimePrepareResult::ReadyRecovered);
        CHECK(runtime.ready());
        CHECK(runtime.state() == CommandJournalState::Executed);
        CHECK(runtime.record().result == CommandResult::Failed);
        CHECK(runtime.record().error == CommandError::Journal);
        CHECK(runtime.last_terminal_command().cmd_id == 41);
        CHECK(
            runtime.last_terminal_command().result ==
            CommandResult::Failed);
        CHECK(
            runtime.last_terminal_command().error ==
            CommandError::Journal);
        CHECK(storage.replace_count == 1);
    }

    {
        FakeStorage storage{};
        write_record(
            storage,
            CommandJournalSlot::A,
            persistent_record(CommandJournalState::AcceptedPersisted),
            12);
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() ==
              CommandRuntimePrepareResult::ReadyRecovered);
        CHECK(runtime.ready());
        CHECK(runtime.state() == CommandJournalState::Executed);
        CHECK(runtime.record().result == CommandResult::Expired);
        CHECK(runtime.record().error == CommandError::Expired);
        CHECK(runtime.last_terminal_command().cmd_id == 41);
        CHECK(
            runtime.last_terminal_command().result ==
            CommandResult::Expired);
        CHECK(
            runtime.last_terminal_command().error ==
            CommandError::Expired);
        CHECK(storage.replace_count == 1);
    }
}

void test_matching_boot_effect_recovers_success_without_dispatch() noexcept
{
    FakeStorage storage{};
    write_record(
        storage,
        CommandJournalSlot::A,
        persistent_record(
            CommandJournalState::ExecuteMarked,
            CommandOpcode::Reboot),
        10);
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare(reboot_evidence(41)) ==
          CommandRuntimePrepareResult::ReadyRecovered);
    CHECK(runtime.ready());
    CHECK(runtime.state() == CommandJournalState::Executed);
    CHECK(runtime.record().result == CommandResult::Executed);
    CHECK(runtime.record().error == CommandError::None);
    CHECK(storage.replace_count == 1);
}

void test_shutdown_dispatch_happens_once_after_execute_marker_commit() noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    advance_to_accepted_receipted(
        runtime, CommandOpcode::Reboot, 91, 191);

    FakeDispatch dispatch{&storage, &runtime};
    const std::size_t before = storage.event_count;
    CHECK(runtime.execute_pending(
              101,
              {},
              {&dispatch, dispatch_shutdown}) ==
          CommandRuntimeExecutionResult::ShutdownDispatched);
    CHECK(dispatch.calls == 1);
    CHECK(dispatch.opcode == CommandOpcode::Reboot);
    CHECK(dispatch.cmd_id == 191);
    CHECK(dispatch.state_seen == CommandJournalState::ExecuteMarked);
    CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
    CHECK(storage.events[before] == Event::Commit);
    CHECK(storage.events[before + 1] == Event::Dispatch);

    CHECK(runtime.execute_pending(
              102,
              {},
              {&dispatch, dispatch_shutdown}) ==
          CommandRuntimeExecutionResult::AwaitingBootEffect);
    CHECK(dispatch.calls == 1);
    CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
}

void test_shutdown_dispatch_failures_are_fail_closed() noexcept
{
    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        advance_to_accepted_receipted(
            runtime, CommandOpcode::PowerOff, 92, 192);
        FakeDispatch dispatch{&storage, &runtime};
        storage.fail_replace_before_write = true;
        CHECK(runtime.execute_pending(
                  101,
                  {},
                  {&dispatch, dispatch_shutdown}) ==
              CommandRuntimeExecutionResult::Deferred);
        CHECK(dispatch.calls == 0);
        CHECK(runtime.state() == CommandJournalState::AcceptedReceipted);
    }

    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        advance_to_accepted_receipted(
            runtime, CommandOpcode::PowerOff, 93, 193);
        FakeDispatch dispatch{&storage, &runtime};
        storage.fail_replace_after_write = true;
        CHECK(runtime.execute_pending(
                  101,
                  {},
                  {&dispatch, dispatch_shutdown}) ==
              CommandRuntimeExecutionResult::Deferred);
        CHECK(dispatch.calls == 0);
        CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
        CHECK(runtime.execute_pending(
                  102,
                  {},
                  {&dispatch, dispatch_shutdown}) ==
              CommandRuntimeExecutionResult::AwaitingBootEffect);
        CHECK(dispatch.calls == 0);
    }

    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        advance_to_accepted_receipted(
            runtime, CommandOpcode::PowerOff, 94, 194);
        FakeDispatch dispatch{&storage, &runtime};
        dispatch.accepts = false;
        CHECK(runtime.execute_pending(
                  101,
                  {},
                  {&dispatch, dispatch_shutdown}) ==
              CommandRuntimeExecutionResult::TerminalFailed);
        CHECK(dispatch.calls == 1);
        CHECK(runtime.state() == CommandJournalState::Executed);
        CHECK(runtime.record().result == CommandResult::Failed);
        CHECK(runtime.record().error == CommandError::Execution);
    }
}

void test_every_visible_transition_commits_before_publish_or_execute() noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    CHECK(runtime.begin_poll(11, 0));

    const CommandResponse response{
        11, 42, CommandOpcode::RequestStatus, 92, 600};
    CHECK(runtime.accept_response(response, 100) ==
          CommandAcceptResult::Accepted);
    CHECK(storage.replace_count == 1);
    CHECK(runtime.state() == CommandJournalState::AcceptedPersisted);

    CommandAckMessage accepted{};
    CHECK(runtime.prepare_ack(accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 2);
    CHECK(runtime.state() ==
          CommandJournalState::AcceptedPublishPending);
    storage.event(Event::Publish);
    CHECK(storage.events[storage.event_count - 2] == Event::Commit);
    CHECK(storage.events[storage.event_count - 1] == Event::Publish);

    CHECK(runtime.record_puback(CommandAckPhase::Accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 3);
    const CommandAckReceipt accepted_receipt{
        42,
        CommandAckPhase::Accepted,
        CommandResult::Accepted,
        CommandAckReceiptCode::Ingested,
        0};
    CHECK(runtime.record_receipt(accepted_receipt) ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 4);

    const std::size_t before_execute = storage.event_count;
    CHECK(runtime.execute_pending(101, {&storage, fresh_snapshot}) ==
          CommandRuntimeExecutionResult::StatusSucceeded);
    CHECK(storage.replace_count == 6);
    CHECK(storage.event_count == before_execute + 3);
    CHECK(storage.events[before_execute] == Event::Commit);
    CHECK(storage.events[before_execute + 1] == Event::Snapshot);
    CHECK(storage.events[before_execute + 2] == Event::Commit);
    CHECK(runtime.state() == CommandJournalState::Executed);

    CommandAckMessage final{};
    CHECK(runtime.prepare_ack(final) ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 7);
    storage.event(Event::Publish);
    CHECK(storage.events[storage.event_count - 2] == Event::Commit);
    CHECK(storage.events[storage.event_count - 1] == Event::Publish);
    CHECK(runtime.record_puback(CommandAckPhase::Final) ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 8);
    const CommandAckReceipt final_receipt{
        42,
        CommandAckPhase::Final,
        CommandResult::Executed,
        CommandAckReceiptCode::Ingested,
        0};
    CHECK(runtime.record_receipt(final_receipt) ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 9);
    CHECK(runtime.clear_final_receipted() ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 10);
    CHECK(runtime.state() == CommandJournalState::Empty);
    CHECK(runtime.last_completed_cmd_id() == 42);
    CHECK(runtime.last_terminal_command().cmd_id == 42);
    CHECK(
        runtime.last_terminal_command().result ==
        CommandResult::Executed);
    CHECK(
        runtime.last_terminal_command().error ==
        CommandError::None);
}

void test_duplicate_final_generation_is_durable_before_publish() noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    CHECK(runtime.begin_poll(12, 100));
    CHECK(runtime.accept_response(
              {12, 99, CommandOpcode::Reboot, 0, 600},
              100) == CommandAcceptResult::RejectedDuplicate);
    CHECK(storage.replace_count == 1);
    CHECK(runtime.state() == CommandJournalState::Executed);
    CHECK(runtime.record().result == CommandResult::Failed);
    CHECK(runtime.record().error == CommandError::Duplicate);
    CHECK(runtime.last_terminal_command().cmd_id == 99);
    CHECK(
        runtime.last_terminal_command().result ==
        CommandResult::Failed);
    CHECK(
        runtime.last_terminal_command().error ==
        CommandError::Duplicate);

    CommandAckMessage final{};
    CHECK(runtime.prepare_ack(final) ==
          CommandTransitionResult::Accepted);
    CHECK(storage.replace_count == 2);
    CHECK(final.phase == CommandAckPhase::Final);
    CHECK(final.result == CommandResult::Failed);
    CHECK(final.error == CommandError::Duplicate);
}

void test_failed_or_indeterminate_commit_reloads_actual_ab_state() noexcept
{
    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        CHECK(runtime.begin_poll(1, 0));
        storage.fail_replace_before_write = true;
        CHECK(runtime.accept_response(
                  {1, 51, CommandOpcode::RequestStatus, 0, 60},
                  10) == CommandAcceptResult::RejectedInvalid);
        CHECK(runtime.ready());
        CHECK(runtime.state() == CommandJournalState::Empty);
    }

    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        CHECK(runtime.begin_poll(2, 0));
        storage.fail_replace_after_write = true;
        CHECK(runtime.accept_response(
                  {2, 52, CommandOpcode::RequestStatus, 0, 60},
                  10) == CommandAcceptResult::RejectedInvalid);
        CHECK(runtime.ready());
        CHECK(runtime.state() == CommandJournalState::AcceptedPersisted);
        CHECK(runtime.record().cmd_id == 52);
    }

    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        CHECK(runtime.begin_poll(3, 0));
        storage.fail_verify_read_after_write = true;
        CHECK(runtime.accept_response(
                  {3, 53, CommandOpcode::RequestStatus, 0, 60},
                  10) == CommandAcceptResult::RejectedInvalid);
        CHECK(runtime.ready());
        CHECK(runtime.state() == CommandJournalState::AcceptedPersisted);
        CHECK(runtime.record().cmd_id == 53);
    }
}

void test_read_failure_and_corrupt_only_fail_closed() noexcept
{
    {
        FakeStorage storage{};
        storage.fail_read_call = 1;
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() ==
              CommandRuntimePrepareResult::FailedClosed);
        CHECK(!runtime.ready());
        CHECK(!runtime.begin_poll(1, 0));
    }

    {
        FakeStorage storage{};
        storage.slot_a.fill(0);
        storage.slot_b.fill(0);
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() ==
              CommandRuntimePrepareResult::FailedClosed);
        CHECK(!runtime.ready());
    }

    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        CHECK(runtime.begin_poll(4, 0));
        storage.corrupt_both_after_replace = true;
        CHECK(runtime.accept_response(
                  {4, 54, CommandOpcode::RequestStatus, 0, 60},
                  10) == CommandAcceptResult::RejectedInvalid);
        CHECK(!runtime.ready());
        CHECK(!runtime.begin_poll(5, 0));
    }
}

void test_pending_journal_resumes_without_opening_a_new_poll() noexcept
{
    FakeStorage storage{};
    write_record(
        storage,
        CommandJournalSlot::A,
        persistent_record(CommandJournalState::FinalPuback),
        20);
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyLoaded);
    CHECK(runtime.pending());
    CHECK(!runtime.begin_poll(99, 0));
    CHECK(runtime.state() == CommandJournalState::FinalPuback);
}

void test_status_failure_blocks_success() noexcept
{
    {
        FakeStorage storage{};
        storage.snapshot_succeeds = false;
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        advance_to_accepted_receipted(
            runtime, CommandOpcode::RequestStatus, 1, 61);
        CommandAckMessage premature{};
        CHECK(runtime.prepare_ack(premature) ==
              CommandTransitionResult::RejectedState);
        CHECK(runtime.execute_pending(
                  101, {&storage, fresh_snapshot}) ==
              CommandRuntimeExecutionResult::TerminalFailed);
        CHECK(storage.snapshot_count == 1);
        CHECK(runtime.record().result == CommandResult::Failed);
        CHECK(runtime.record().error == CommandError::Execution);
        CommandAckMessage failed_final{};
        CHECK(runtime.prepare_ack(failed_final) ==
              CommandTransitionResult::Accepted);
        CHECK(failed_final.phase == CommandAckPhase::Final);
        CHECK(failed_final.result == CommandResult::Failed);
        CHECK(failed_final.error == CommandError::Execution);
    }
}

void test_ttl_expiry_persists_terminal_without_execution() noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    advance_to_accepted_receipted(
        runtime, CommandOpcode::RequestStatus, 70, 170);

    CHECK(runtime.execute_pending(
              700,
              {&storage, fresh_snapshot}) ==
          CommandRuntimeExecutionResult::Expired);
    CHECK(storage.snapshot_count == 0);
    CHECK(runtime.state() == CommandJournalState::Executed);
    CHECK(runtime.record().result == CommandResult::Expired);
    CHECK(runtime.record().error == CommandError::Expired);
}

void test_receipt_loss_or_mismatch_retains_journal_and_blocks_execution()
    noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    CHECK(runtime.begin_poll(71, 0));
    CHECK(runtime.accept_response(
              {71, 171, CommandOpcode::RequestStatus, 0, 600},
              100) == CommandAcceptResult::Accepted);
    CommandAckMessage accepted{};
    CHECK(runtime.prepare_ack(accepted) ==
          CommandTransitionResult::Accepted);
    CHECK(runtime.record_puback(CommandAckPhase::Accepted) ==
          CommandTransitionResult::Accepted);

    CHECK(runtime.execute_pending(
              101,
              {&storage, fresh_snapshot}) ==
          CommandRuntimeExecutionResult::Rejected);
    CHECK(runtime.state() == CommandJournalState::AcceptedPuback);
    CHECK(storage.snapshot_count == 0);

    CHECK(runtime.record_receipt(
              {171,
               CommandAckPhase::Final,
               CommandResult::Accepted,
               CommandAckReceiptCode::Ingested,
               0}) == CommandTransitionResult::RejectedMismatch);
    CHECK(runtime.state() == CommandJournalState::AcceptedPuback);
    CHECK(storage.snapshot_count == 0);
}

void test_fota_prepare_is_terminal_fail_closed() noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    advance_to_accepted_receipted(
        runtime, CommandOpcode::FotaPrepare, 76, 176);

    CHECK(runtime.execute_pending(
              101,
              {&storage, fresh_snapshot}) ==
          CommandRuntimeExecutionResult::TerminalFailed);
    CHECK(storage.snapshot_count == 0);
    CHECK(runtime.state() == CommandJournalState::Executed);
    CHECK(runtime.record().result == CommandResult::Failed);
    CHECK(runtime.record().error == CommandError::InvalidOpcode);
}

void test_execute_mark_write_before_failure_defers_without_snapshot() noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    advance_to_accepted_receipted(
        runtime, CommandOpcode::RequestStatus, 81, 181);
    const CommandJournalRecord prior = runtime.record();

    storage.fail_replace_before_write = true;
    CHECK(runtime.execute_pending(
              101, {&storage, fresh_snapshot}) ==
          CommandRuntimeExecutionResult::Deferred);
    CHECK(storage.snapshot_count == 0);
    CHECK(runtime.state() == CommandJournalState::AcceptedReceipted);
    CHECK(runtime.record().cmd_id == prior.cmd_id);
    CHECK(runtime.record().job_id == prior.job_id);
    CHECK(runtime.record().dispatch_latched == prior.dispatch_latched);
}

void test_indeterminate_execute_mark_never_reexecutes_on_next_cycle() noexcept
{
    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        advance_to_accepted_receipted(
            runtime, CommandOpcode::RequestStatus, 82, 182);

        storage.fail_replace_after_write = true;
        CHECK(runtime.execute_pending(
                  101, {&storage, fresh_snapshot}) ==
              CommandRuntimeExecutionResult::Deferred);
        CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
        CHECK(storage.snapshot_count == 0);

        CHECK(runtime.execute_pending(
                  101, {&storage, fresh_snapshot}) ==
              CommandRuntimeExecutionResult::AwaitingBootEffect);
        CHECK(storage.snapshot_count == 0);
        CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
    }

    {
        FakeStorage storage{};
        CommandRuntimeCoordinator runtime{port(storage)};
        CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
        advance_to_accepted_receipted(
            runtime, CommandOpcode::RequestStatus, 83, 183);

        storage.fail_verify_read_after_write = true;
        CHECK(runtime.execute_pending(
                  101, {&storage, fresh_snapshot}) ==
              CommandRuntimeExecutionResult::Deferred);
        CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
        CHECK(storage.snapshot_count == 0);

        CHECK(runtime.execute_pending(
                  101, {&storage, fresh_snapshot}) ==
              CommandRuntimeExecutionResult::AwaitingBootEffect);
        CHECK(storage.snapshot_count == 0);
        CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
    }
}

void test_execution_result_commit_failure_keeps_final_ack_unavailable()
    noexcept
{
    FakeStorage storage{};
    CommandRuntimeCoordinator runtime{port(storage)};
    CHECK(runtime.prepare() == CommandRuntimePrepareResult::ReadyBlank);
    advance_to_accepted_receipted(
        runtime, CommandOpcode::RequestStatus, 84, 184);

    storage.fail_result_commit_before_write = true;
    CHECK(runtime.execute_pending(
              101, {&storage, fresh_snapshot}) ==
          CommandRuntimeExecutionResult::Deferred);
    CHECK(storage.snapshot_count == 1);
    CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
    CHECK(runtime.record().dispatch_latched == 1);

    CommandAckMessage unavailable{};
    CHECK(runtime.prepare_ack(unavailable) ==
          CommandTransitionResult::RejectedState);

    CHECK(runtime.execute_pending(
              101, {&storage, fresh_snapshot}) ==
          CommandRuntimeExecutionResult::AwaitingBootEffect);
    CHECK(storage.snapshot_count == 1);
    CHECK(runtime.state() == CommandJournalState::ExecuteMarked);
    CommandAckMessage final{};
    CHECK(runtime.prepare_ack(final) ==
          CommandTransitionResult::RejectedState);
}

} // namespace

int main()
{
    test_blank_and_loaded_boot_are_ready_without_spurious_commit();
    test_boot_recovery_is_durable_before_intake();
    test_matching_boot_effect_recovers_success_without_dispatch();
    test_shutdown_dispatch_happens_once_after_execute_marker_commit();
    test_shutdown_dispatch_failures_are_fail_closed();
    test_every_visible_transition_commits_before_publish_or_execute();
    test_duplicate_final_generation_is_durable_before_publish();
    test_failed_or_indeterminate_commit_reloads_actual_ab_state();
    test_read_failure_and_corrupt_only_fail_closed();
    test_pending_journal_resumes_without_opening_a_new_poll();
    test_status_failure_blocks_success();
    test_ttl_expiry_persists_terminal_without_execution();
    test_receipt_loss_or_mismatch_retains_journal_and_blocks_execution();
    test_fota_prepare_is_terminal_fail_closed();
    test_execute_mark_write_before_failure_defers_without_snapshot();
    test_indeterminate_execute_mark_never_reexecutes_on_next_cycle();
    test_execution_result_commit_failure_keeps_final_ack_unavailable();

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "command_runtime_coordinator_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "command_runtime_coordinator_test: %zu checks passed\n",
        g_checks);
    return 0;
}
