#include "runtime_owner_shutdown_record_core.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "flash_partition_layout.hpp"

namespace {

using namespace boot_v2;

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(const bool condition, const char *expression, const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(
            stderr, "CHECK failed: %s:%d: %s\n", __FILE__, line, expression);
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

RuntimeOwnerShutdownRecordInput record_input(
    const RuntimeOwnerShutdownPlannedAction action =
        RuntimeOwnerShutdownPlannedAction::WatchdogReboot) noexcept
{
    RuntimeOwnerShutdownRecordInput input{};
    input.producer_sequence = 41;
    input.incident_correlation_id = 99;
    input.elapsed_ms = 88000;
    input.reason = 1;
    input.initial_usb_present = action ==
                                        RuntimeOwnerShutdownPlannedAction::
                                            WatchdogReboot
                                    ? 1
                                    : 0;
    input.planned_action = action;
    input.cleanup_succeeded_mask = 0x7F;
    input.hardware_reset_count = 2;
    return input;
}

void test_binary_contract_and_flash_layout() noexcept
{
    CHECK(sizeof(RuntimeOwnerShutdownRecordV1) == 64);
    CHECK(alignof(RuntimeOwnerShutdownRecordV1) == 32);
    CHECK(std::is_standard_layout<RuntimeOwnerShutdownRecordV1>::value);
    CHECK(std::is_trivially_copyable<RuntimeOwnerShutdownRecordV1>::value);
    CHECK(flash_partition::sensor_log_offset == 0x3F0000u);
    CHECK(flash_partition::sensor_log_size == 0x00E000u);
    CHECK(flash_partition::shutdown_record_a_offset == 0x3FE000u);
    CHECK(flash_partition::shutdown_record_b_offset == 0x3FF000u);
    CHECK(
        flash_partition::shutdown_record_slot_size ==
        flash_partition::sector_size);
    CHECK(
        flash_partition::shutdown_record_b_offset +
            flash_partition::shutdown_record_slot_size ==
        flash_partition::total_size);
    CHECK(flash_partition::total_size == 0x400000u);
}

void test_make_crc_and_corruption_detection() noexcept
{
    const RuntimeOwnerShutdownRecordV1 record =
        runtime_owner_shutdown_record_make(record_input(), 7);
    CHECK(record.magic == RUNTIME_OWNER_SHUTDOWN_RECORD_MAGIC);
    CHECK(record.version == RUNTIME_OWNER_SHUTDOWN_RECORD_VERSION);
    CHECK(record.size == sizeof(RuntimeOwnerShutdownRecordV1));
    CHECK(record.sequence == 7);
    CHECK(record.producer_sequence == 41);
    CHECK(record.incident_correlation_id == 99);
    CHECK(record.elapsed_ms == 88000);
    CHECK(record.planned_action ==
          RuntimeOwnerShutdownPlannedAction::WatchdogReboot);
    CHECK(runtime_owner_shutdown_record_valid(record));
    CHECK(
        record.crc32 == runtime_owner_shutdown_record_crc(record));

    RuntimeOwnerShutdownRecordV1 corrupted = record;
    corrupted.elapsed_ms ^= 1u;
    CHECK(!runtime_owner_shutdown_record_valid(corrupted));

    corrupted = record;
    corrupted.crc32 ^= 1u;
    CHECK(!runtime_owner_shutdown_record_valid(corrupted));

    corrupted = record;
    corrupted.initial_usb_present = 2;
    corrupted.crc32 = runtime_owner_shutdown_record_crc(corrupted);
    CHECK(!runtime_owner_shutdown_record_valid(corrupted));
}

void test_latest_valid_slot_and_sequence_wrap() noexcept
{
    RuntimeOwnerShutdownRecordV1 older =
        runtime_owner_shutdown_record_make(record_input(), 12);
    RuntimeOwnerShutdownRecordV1 newer =
        runtime_owner_shutdown_record_make(
            record_input(RuntimeOwnerShutdownPlannedAction::Gp15Kill), 13);
    CHECK(runtime_owner_shutdown_record_select_latest(older, newer) == &newer);
    CHECK(runtime_owner_shutdown_record_select_latest(newer, older) == &newer);

    newer.crc32 ^= 1u;
    CHECK(runtime_owner_shutdown_record_select_latest(older, newer) == &older);

    older.crc32 ^= 1u;
    CHECK(runtime_owner_shutdown_record_select_latest(older, newer) == nullptr);

    RuntimeOwnerShutdownRecordV1 before_wrap =
        runtime_owner_shutdown_record_make(record_input(), 0xFFFFFFFFu);
    RuntimeOwnerShutdownRecordV1 after_wrap =
        runtime_owner_shutdown_record_make(record_input(), 1u);
    CHECK(
        runtime_owner_shutdown_record_select_latest(
            before_wrap, after_wrap) == &after_wrap);
}

} // namespace

int main()
{
    test_binary_contract_and_flash_layout();
    test_make_crc_and_corruption_detection();
    test_latest_valid_slot_and_sequence_wrap();

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "runtime_owner_shutdown_record_core_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "runtime_owner_shutdown_record_core_test: %zu checks passed\n",
        g_checks);
    return 0;
}
