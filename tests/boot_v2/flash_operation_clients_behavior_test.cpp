#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flash_test_platform.hpp"
#include "../../src/boot_v2/command_journal_flash_store.hpp"
#include "../../src/boot_v2/flash_partition_layout.hpp"
#include "../../src/boot_v2/runtime_owner_shutdown_record_store.hpp"
#include "../../src/lib/flash_logger.hpp"

namespace {

using namespace boot_v2;
namespace partition = boot_v2::flash_partition;
using flash_test::g_flash;
using flash_test::g_platform;
using flash_test::reset_platform;

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

const FlashLogEntry &log_entry(const std::size_t index) noexcept
{
    const auto *const entries =
        reinterpret_cast<const FlashLogEntry *>(
            g_flash.data() + partition::sensor_log_offset);
    return entries[index];
}

bool erased_log_entry(const std::size_t index) noexcept
{
    const auto *const bytes =
        reinterpret_cast<const std::uint8_t *>(&log_entry(index));
    for (std::size_t byte = 0; byte < sizeof(FlashLogEntry);
         ++byte) {
        if (bytes[byte] != 0xFFu) {
            return false;
        }
    }
    return true;
}

void write_log(const float temperature) noexcept
{
    flash_log_write(
        temperature,
        4.8f,
        1u,
        0u,
        0,
        0);
}

CommandJournalRecord journal_record(
    const std::uint32_t command_id) noexcept
{
    CommandJournalRecord record{};
    record.state = CommandJournalState::AcceptedPersisted;
    record.opcode = CommandOpcode::Reboot;
    record.phase = CommandAckPhase::Accepted;
    record.result = CommandResult::Accepted;
    record.error = CommandError::None;
    record.retry_count = 0;
    record.expected_effect = CommandExpectedEffect::Reset;
    record.ttl_checkpoint_clock_valid = 1;
    record.dispatch_latched = 0;
    record.cmd_id = command_id;
    record.job_id = command_id + 100u;
    record.ttl_seconds = 600;
    record.remaining_ttl_seconds = 590;
    record.ttl_checkpoint_monotonic_seconds = 10;
    record.ttl_checkpoint_unix_seconds = 1720000000u;
    record.ttl_checkpoint_boot_sequence = 7;
    record.boot_sequence_before_execute = 7;
    return record;
}

RuntimeOwnerShutdownRecordInput shutdown_input() noexcept
{
    RuntimeOwnerShutdownRecordInput input{};
    input.producer_sequence = 41;
    input.incident_correlation_id = 99;
    input.elapsed_ms = 88000;
    input.reason = 1;
    input.initial_usb_present = 1;
    input.planned_action =
        RuntimeOwnerShutdownPlannedAction::WatchdogReboot;
    input.cleanup_succeeded_mask = 0x7Fu;
    input.hardware_reset_count = 2;
    return input;
}

void test_logger_commits_applied_exit_timeout_mutation()
{
    reset_platform();

    write_log(1.0f);
    CHECK(!erased_log_entry(0u));
    CHECK(log_entry(0u).temperature == 1.0f);

    g_platform.exit_result = PICO_ERROR_TIMEOUT;
    write_log(2.0f);
    CHECK(!erased_log_entry(1u));
    CHECK(log_entry(1u).temperature == 2.0f);

    g_platform.exit_result = PICO_OK;
    write_log(3.0f);
    CHECK(!erased_log_entry(2u));
    CHECK(log_entry(2u).temperature == 3.0f);
}

void test_logger_does_not_advance_on_not_attempted_failure()
{
    g_platform.enter_result = PICO_ERROR_TIMEOUT;
    write_log(4.0f);
    CHECK(erased_log_entry(3u));

    g_platform.enter_result = PICO_OK;
    write_log(5.0f);
    CHECK(!erased_log_entry(3u));
    CHECK(log_entry(3u).temperature == 5.0f);
    CHECK(erased_log_entry(4u));
}

void test_logger_clear_commits_applied_erase()
{
    g_platform.exit_result = PICO_ERROR_TIMEOUT;
    flash_log_clear();
    CHECK(erased_log_entry(0u));
    CHECK(erased_log_entry(1u));
    CHECK(erased_log_entry(2u));
    CHECK(erased_log_entry(3u));

    g_platform.exit_result = PICO_OK;
    write_log(6.0f);
    CHECK(!erased_log_entry(0u));
    CHECK(log_entry(0u).temperature == 6.0f);
    CHECK(erased_log_entry(1u));
}

void test_logger_rescans_unknown_completed_program()
{
    g_platform.interrupt_after_program = true;
    write_log(7.0f);
    CHECK(!erased_log_entry(1u));
    CHECK(log_entry(1u).temperature == 7.0f);

    g_platform.interrupt_after_program = false;
    write_log(8.0f);
    CHECK(!erased_log_entry(2u));
    CHECK(log_entry(2u).temperature == 8.0f);
}

void test_journal_reconciles_applied_exit_failures()
{
    reset_platform();
    g_platform.exit_result = PICO_ERROR_TIMEOUT;
    const CommandJournalStoreResult timeout_result =
        command_journal_flash_commit(journal_record(11u), 20u);
    CHECK(
        timeout_result.status ==
        CommandJournalStoreStatus::Committed);
    CHECK(timeout_result.active_slot == CommandJournalSlot::A);
    CHECK(timeout_result.sequence == 1u);

    reset_platform();
    g_platform.exit_result = PICO_ERROR_NOT_PERMITTED;
    const CommandJournalStoreResult platform_result =
        command_journal_flash_commit(journal_record(12u), 20u);
    CHECK(
        platform_result.status ==
        CommandJournalStoreStatus::Committed);
    CHECK(platform_result.active_slot == CommandJournalSlot::A);
}

void test_journal_propagates_not_attempted_failure()
{
    reset_platform();
    g_platform.enter_result = PICO_ERROR_TIMEOUT;
    const CommandJournalStoreResult result =
        command_journal_flash_commit(journal_record(13u), 20u);
    CHECK(
        result.status ==
        CommandJournalStoreStatus::WriteFailed);
    CHECK(g_platform.erase_calls == 0u);
    CHECK(g_platform.program_calls == 0u);
}

void test_journal_readback_reconciles_unknown_program()
{
    reset_platform();
    g_platform.interrupt_after_program = true;
    const CommandJournalStoreResult result =
        command_journal_flash_commit(journal_record(14u), 20u);
    CHECK(
        result.status ==
        CommandJournalStoreStatus::Committed);
    CHECK(result.active_slot == CommandJournalSlot::A);
}

void test_journal_serializes_readback_after_deadline_consumed()
{
    reset_platform();
    g_platform.mutation_elapsed_us = 11000u;
    const CommandJournalStoreResult result =
        command_journal_flash_commit(journal_record(15u), 20u);
    CHECK(
        result.status ==
        CommandJournalStoreStatus::Committed);
    CHECK(g_platform.xip_base_while_mutex_calls == 1u);
}

void test_shutdown_reconciles_applied_exit_failures()
{
    reset_platform();
    g_platform.exit_result = PICO_ERROR_TIMEOUT;
    CHECK(
        runtime_owner_shutdown_record_commit(
            shutdown_input(), 20u));
    CHECK(runtime_owner_shutdown_record_current() != nullptr);

    reset_platform();
    g_platform.exit_result = PICO_ERROR_NOT_PERMITTED;
    CHECK(
        runtime_owner_shutdown_record_commit(
            shutdown_input(), 20u));
    CHECK(runtime_owner_shutdown_record_current() != nullptr);
}

void test_shutdown_propagates_not_attempted_failure()
{
    reset_platform();
    g_platform.enter_result = PICO_ERROR_TIMEOUT;
    CHECK(
        !runtime_owner_shutdown_record_commit(
            shutdown_input(), 20u));
    CHECK(runtime_owner_shutdown_record_current() == nullptr);
    CHECK(g_platform.erase_calls == 0u);
    CHECK(g_platform.program_calls == 0u);
}

void test_shutdown_readback_reconciles_unknown_program()
{
    reset_platform();
    g_platform.interrupt_after_program = true;
    CHECK(
        runtime_owner_shutdown_record_commit(
            shutdown_input(), 20u));
    CHECK(runtime_owner_shutdown_record_current() != nullptr);
}

void test_shutdown_serializes_readback_after_deadline_consumed()
{
    reset_platform();
    g_platform.mutation_elapsed_us = 11000u;
    CHECK(
        runtime_owner_shutdown_record_commit(
            shutdown_input(), 20u));
    CHECK(g_platform.xip_base_while_mutex_calls == 1u);
}

} // namespace

int g_boot_reason_code = 0;

void app_log_init()
{
}

void app_log_set_enabled(bool)
{
}

void app_log_printf(const char *, ...)
{
}

void app_log_vprintf(const char *, va_list)
{
}

void vLogTask(void *)
{
}

int main()
{
    test_logger_commits_applied_exit_timeout_mutation();
    test_logger_does_not_advance_on_not_attempted_failure();
    test_logger_clear_commits_applied_erase();
    test_logger_rescans_unknown_completed_program();
    test_journal_reconciles_applied_exit_failures();
    test_journal_propagates_not_attempted_failure();
    test_journal_readback_reconciles_unknown_program();
    test_journal_serializes_readback_after_deadline_consumed();
    test_shutdown_reconciles_applied_exit_failures();
    test_shutdown_propagates_not_attempted_failure();
    test_shutdown_readback_reconciles_unknown_program();
    test_shutdown_serializes_readback_after_deadline_consumed();

    if (g_failures != 0u) {
        std::fprintf(
            stderr,
            "flash_operation_clients_behavior_test: "
            "%zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "flash_operation_clients_behavior_test: "
        "%zu checks passed\n",
        g_checks);
    return 0;
}
