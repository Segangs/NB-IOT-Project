#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "flash_test_platform.hpp"
#include "../../src/boot_v2/flash_partition_layout.hpp"
#include "../../src/lib/flash_operation_service.hpp"

namespace {

using boot_v2::FlashOperationResult;
using boot_v2::FlashOperationTransaction;
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

enum class TransactionAction {
    Read,
    Erase,
    Program,
    Replace,
    ReplaceThenReadback,
    ReturnPlatformFailure,
};

struct TransactionRequest {
    TransactionAction action{TransactionAction::Erase};
    std::uint32_t offset{partition::sensor_log_offset};
    std::uint32_t page_offset{partition::sensor_log_offset};
    void *read_output{nullptr};
    const std::uint8_t *page{nullptr};
    std::size_t size{partition::sector_size};
    bool callback_called{false};
    FlashOperationResult readback_result{};
};

FlashOperationResult run_transaction(
    FlashOperationTransaction &transaction,
    void *const opaque) noexcept
{
    auto *const request =
        static_cast<TransactionRequest *>(opaque);
    if (request == nullptr) {
        return boot_v2::FlashOperationCode::InvalidArgument;
    }
    request->callback_called = true;
    switch (request->action) {
    case TransactionAction::Read:
        return transaction.read(
            request->offset,
            request->read_output,
            request->size);
    case TransactionAction::Erase:
        return transaction.erase_range(
            request->offset, request->size);
    case TransactionAction::Program:
        return transaction.program_page(
            request->offset,
            request->page,
            request->size);
    case TransactionAction::Replace:
        return transaction.replace_sector(
            request->offset,
            request->page_offset,
            request->page,
            request->size);
    case TransactionAction::ReplaceThenReadback: {
        const FlashOperationResult result =
            transaction.replace_sector(
                request->offset,
                request->page_offset,
                request->page,
                request->size);
        std::array<std::uint8_t, FLASH_PAGE_SIZE> readback{};
        request->readback_result = transaction.read(
            request->page_offset,
            readback.data(),
            readback.size());
        return result;
    }
    case TransactionAction::ReturnPlatformFailure:
        return boot_v2::FlashOperationCode::PlatformFailure;
    }
    return boot_v2::FlashOperationCode::PlatformFailure;
}

FlashOperationResult execute(
    TransactionRequest &request,
    const std::uint32_t timeout_ms = 10u) noexcept
{
    return boot_v2::flash_operation_execute(
        run_transaction, &request, timeout_ms);
}

void check_outcome(
    const FlashOperationResult result,
    const boot_v2::FlashOperationCode code,
    const boot_v2::FlashMutationDisposition mutation,
    const bool deadline_exceeded,
    const int line) noexcept
{
    check(result.code == code, "result.code == code", line);
    check(
        result.mutation == mutation,
        "result.mutation == mutation",
        line);
    check(
        result.deadline_exceeded == deadline_exceeded,
        "result.deadline_exceeded == deadline_exceeded",
        line);
}

#define CHECK_OUTCOME(result, code, mutation, deadline_exceeded) \
    check_outcome(                                                \
        (result), (code), (mutation), (deadline_exceeded),        \
        __LINE__)

void test_invalid_service_inputs_fail_closed()
{
    reset_platform();
    TransactionRequest request{};
    CHECK(
        boot_v2::flash_operation_execute(
            nullptr, &request, 10u) ==
        boot_v2::FlashOperationCode::InvalidArgument);
    CHECK(
        boot_v2::flash_operation_execute(
            run_transaction, &request, 0u) ==
        boot_v2::FlashOperationCode::InvalidArgument);
    CHECK(
        boot_v2::flash_operation_execute(
            run_transaction, &request, UINT32_MAX) ==
        boot_v2::FlashOperationCode::InvalidArgument);
    CHECK(
        boot_v2::flash_operation_execute(
            run_transaction, &request, 60001u) ==
        boot_v2::FlashOperationCode::InvalidArgument);
    CHECK(!request.callback_called);
    CHECK(g_platform.erase_calls == 0u);
}

void test_pre_scheduler_core0_uses_irq_only_backend()
{
    reset_platform();
    g_platform.scheduler_state = taskSCHEDULER_NOT_STARTED;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::Succeeded,
        boot_v2::FlashMutationDisposition::Applied,
        false);
    CHECK(request.callback_called);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.disable_irq_calls == 1u);
    CHECK(g_platform.restore_irq_calls == 1u);
    CHECK(g_platform.flash_safe_execute_calls == 0u);
    CHECK(g_platform.enter_calls == 0u);
    CHECK(g_platform.exit_calls == 0u);
    CHECK(!g_platform.raw_without_safety);
}

void test_pre_scheduler_nonzero_core_fails_closed()
{
    reset_platform();
    g_platform.scheduler_state = taskSCHEDULER_NOT_STARTED;
    g_platform.core_num = 1u;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::LockUnavailable,
        boot_v2::FlashMutationDisposition::NotAttempted,
        false);
    CHECK(!request.callback_called);
    CHECK(g_platform.erase_calls == 0u);
    CHECK(g_platform.flash_safe_execute_calls == 0u);
    CHECK(g_platform.disable_irq_calls == 0u);
}

void test_post_scheduler_success_uses_helper_and_mutex()
{
    reset_platform();
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::Succeeded,
        boot_v2::FlashMutationDisposition::Applied,
        false);
    CHECK(request.callback_called);
    CHECK(g_platform.mutex_take_calls == 1u);
    CHECK(g_platform.mutex_give_calls == 1u);
    CHECK(g_platform.enter_calls == 1u);
    CHECK(g_platform.exit_calls == 1u);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.flash_safe_execute_calls == 1u);
    CHECK(!g_platform.raw_without_safety);
}

void test_mutex_contention_exhausts_budget_without_callback()
{
    reset_platform();
    g_platform.mutex_take_elapsed_us = 10000u;
    g_platform.mutex_take_result = pdFALSE;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::TimedOut,
        boot_v2::FlashMutationDisposition::NotAttempted,
        true);
    CHECK(!request.callback_called);
    CHECK(g_platform.mutex_take_calls == 1u);
    CHECK(g_platform.mutex_give_calls == 0u);
    CHECK(g_platform.enter_calls == 0u);
    CHECK(g_platform.erase_calls == 0u);
}

void test_enter_and_exit_share_one_remaining_budget()
{
    reset_platform();
    g_platform.mutex_take_elapsed_us = 2000u;
    g_platform.enter_elapsed_us = 3000u;
    g_platform.mutation_elapsed_us = 1000u;
    g_platform.exit_elapsed_us = 1000u;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::Succeeded,
        boot_v2::FlashMutationDisposition::Applied,
        false);
    CHECK(g_platform.enter_timeout_ms == 4u);
    CHECK(g_platform.exit_timeout_ms == 4u);
    CHECK(g_platform.now_us == 7000u);
}

void test_enter_timeout_is_definite_no_mutation()
{
    reset_platform();
    g_platform.enter_result = PICO_ERROR_TIMEOUT;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::TimedOut,
        boot_v2::FlashMutationDisposition::NotAttempted,
        true);
    CHECK(request.callback_called);
    CHECK(g_platform.enter_calls == 1u);
    CHECK(g_platform.exit_calls == 0u);
    CHECK(g_platform.erase_calls == 0u);
}

void test_enter_platform_failure_is_definite_no_mutation()
{
    reset_platform();
    g_platform.enter_result = PICO_ERROR_NOT_PERMITTED;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::PlatformFailure,
        boot_v2::FlashMutationDisposition::NotAttempted,
        false);
    CHECK(g_platform.exit_calls == 0u);
    CHECK(g_platform.erase_calls == 0u);
}

void test_exit_timeout_reports_completed_mutation_uncertainty()
{
    reset_platform();
    g_platform.exit_result = PICO_ERROR_TIMEOUT;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::TimedOut,
        boot_v2::FlashMutationDisposition::Applied,
        true);
    CHECK(request.callback_called);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.exit_calls == 1u);
}

void test_exit_platform_failure_reports_completed_mutation_uncertainty()
{
    reset_platform();
    g_platform.exit_result = PICO_ERROR_NOT_PERMITTED;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::PlatformFailure,
        boot_v2::FlashMutationDisposition::Applied,
        false);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.exit_calls == 1u);
}

void test_interrupted_raw_callback_reports_unknown_mutation()
{
    reset_platform();
    g_platform.interrupt_after_erase = true;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::TimedOut,
        boot_v2::FlashMutationDisposition::Unknown,
        true);
    CHECK(request.callback_called);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.exit_calls == 0u);
}

void test_completed_callback_is_not_reclassified_by_deadline()
{
    reset_platform();
    g_platform.mutation_elapsed_us = 12000u;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::Succeeded,
        boot_v2::FlashMutationDisposition::Applied,
        true);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.exit_calls == 1u);
    CHECK(g_platform.exit_timeout_ms == 5u);
}

void test_completed_mutation_allows_serialized_deadline_readback()
{
    reset_platform();
    g_platform.mutation_elapsed_us = 6000u;
    alignas(FLASH_PAGE_SIZE)
        std::array<std::uint8_t, FLASH_PAGE_SIZE> page{};
    TransactionRequest request{};
    request.action = TransactionAction::ReplaceThenReadback;
    request.page = page.data();
    request.size = page.size();
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::Succeeded,
        boot_v2::FlashMutationDisposition::Applied,
        true);
    CHECK_OUTCOME(
        request.readback_result,
        boot_v2::FlashOperationCode::Succeeded,
        boot_v2::FlashMutationDisposition::NotAttempted,
        true);
    CHECK(g_platform.xip_base_while_mutex_calls == 1u);
}

void test_mutex_release_failure_preserves_mutation_completion()
{
    reset_platform();
    g_platform.mutex_give_result = pdFALSE;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::LockUnavailable,
        boot_v2::FlashMutationDisposition::Applied,
        false);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.mutex_give_calls == 1u);
}

void test_callback_failure_and_suspended_scheduler_propagate()
{
    reset_platform();
    TransactionRequest callback_failure{};
    callback_failure.action =
        TransactionAction::ReturnPlatformFailure;
    CHECK(
        execute(callback_failure) ==
        boot_v2::FlashOperationCode::PlatformFailure);
    CHECK(g_platform.mutex_give_calls == 1u);

    reset_platform();
    g_platform.scheduler_state = taskSCHEDULER_SUSPENDED;
    TransactionRequest suspended{};
    CHECK(
        execute(suspended) ==
        boot_v2::FlashOperationCode::LockUnavailable);
    CHECK(!suspended.callback_called);
}

void test_read_range_and_mutation_alignment_validation()
{
    alignas(FLASH_PAGE_SIZE)
        std::array<std::uint8_t, FLASH_PAGE_SIZE> page{};
    std::uint32_t output = 0;

    reset_platform();
    TransactionRequest bad_read{};
    bad_read.action = TransactionAction::Read;
    bad_read.offset = partition::firmware_a_offset;
    bad_read.read_output = &output;
    bad_read.size = sizeof(output);
    CHECK(
        execute(bad_read) ==
        boot_v2::FlashOperationCode::InvalidRange);

    reset_platform();
    TransactionRequest bad_erase{};
    bad_erase.offset = partition::sensor_log_offset + 1u;
    CHECK(
        execute(bad_erase) ==
        boot_v2::FlashOperationCode::InvalidAlignment);
    CHECK(g_platform.erase_calls == 0u);

    reset_platform();
    TransactionRequest zero_erase{};
    zero_erase.size = 0u;
    CHECK(
        execute(zero_erase) ==
        boot_v2::FlashOperationCode::InvalidArgument);

    reset_platform();
    TransactionRequest bad_program{};
    bad_program.action = TransactionAction::Program;
    bad_program.page = page.data();
    bad_program.size = page.size() - 1u;
    CHECK(
        execute(bad_program) ==
        boot_v2::FlashOperationCode::InvalidAlignment);
    CHECK(g_platform.program_calls == 0u);

    reset_platform();
    TransactionRequest outside{};
    outside.action = TransactionAction::Program;
    outside.offset = partition::metadata_scratch_offset;
    outside.page = page.data();
    outside.size = page.size();
    CHECK(
        execute(outside) ==
        boot_v2::FlashOperationCode::InvalidRange);

    reset_platform();
    TransactionRequest replace{};
    replace.action = TransactionAction::Replace;
    replace.offset = partition::command_journal_a_offset;
    replace.page_offset = partition::command_journal_a_offset;
    replace.page = page.data();
    replace.size = page.size();
    CHECK(
        execute(replace) ==
        boot_v2::FlashOperationCode::Succeeded);
    CHECK(g_platform.erase_calls == 1u);
    CHECK(g_platform.program_calls == 1u);
}

void test_interrupt_context_is_rejected_before_any_lock()
{
    reset_platform();
    g_platform.scheduler_state = taskSCHEDULER_NOT_STARTED;
    g_platform.inside_interrupt = pdTRUE;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::LockUnavailable,
        boot_v2::FlashMutationDisposition::NotAttempted,
        false);
    CHECK(!request.callback_called);
    CHECK(g_platform.mutex_take_calls == 0u);
    CHECK(g_platform.disable_irq_calls == 0u);
    CHECK(g_platform.erase_calls == 0u);
}

void test_missing_post_scheduler_helper_fails_before_mutation()
{
    reset_platform();
    g_platform.helper_available = false;
    TransactionRequest request{};
    const FlashOperationResult result = execute(request);

    CHECK_OUTCOME(
        result,
        boot_v2::FlashOperationCode::PlatformFailure,
        boot_v2::FlashMutationDisposition::NotAttempted,
        false);
    CHECK(request.callback_called);
    CHECK(g_platform.erase_calls == 0u);
}

} // namespace

int main()
{
    test_invalid_service_inputs_fail_closed();
    test_pre_scheduler_core0_uses_irq_only_backend();
    test_pre_scheduler_nonzero_core_fails_closed();
    test_post_scheduler_success_uses_helper_and_mutex();
    test_mutex_contention_exhausts_budget_without_callback();
    test_enter_and_exit_share_one_remaining_budget();
    test_enter_timeout_is_definite_no_mutation();
    test_enter_platform_failure_is_definite_no_mutation();
    test_exit_timeout_reports_completed_mutation_uncertainty();
    test_exit_platform_failure_reports_completed_mutation_uncertainty();
    test_interrupted_raw_callback_reports_unknown_mutation();
    test_completed_callback_is_not_reclassified_by_deadline();
    test_completed_mutation_allows_serialized_deadline_readback();
    test_mutex_release_failure_preserves_mutation_completion();
    test_callback_failure_and_suspended_scheduler_propagate();
    test_read_range_and_mutation_alignment_validation();
    test_interrupt_context_is_rejected_before_any_lock();
    test_missing_post_scheduler_helper_fails_before_mutation();

    if (g_failures != 0u) {
        std::fprintf(
            stderr,
            "flash_operation_service_behavior_test: "
            "%zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "flash_operation_service_behavior_test: "
        "%zu checks passed\n",
        g_checks);
    return 0;
}
