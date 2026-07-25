#include "command_journal_store_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace boot_v2;

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

bool same_runtime_record(
    const CommandJournalRecord &left,
    const CommandJournalRecord &right) noexcept
{
    return left.schema_version == right.schema_version &&
           left.state == right.state &&
           left.opcode == right.opcode &&
           left.phase == right.phase &&
           left.result == right.result &&
           left.error == right.error &&
           left.retry_count == right.retry_count &&
           left.expected_effect == right.expected_effect &&
           left.ttl_checkpoint_clock_valid ==
               right.ttl_checkpoint_clock_valid &&
           left.dispatch_latched == right.dispatch_latched &&
           left.reserved == right.reserved &&
           left.cmd_id == right.cmd_id &&
           left.job_id == right.job_id &&
           left.ttl_seconds == right.ttl_seconds &&
           left.remaining_ttl_seconds ==
               right.remaining_ttl_seconds &&
           left.ttl_checkpoint_monotonic_seconds ==
               right.ttl_checkpoint_monotonic_seconds &&
           left.ttl_checkpoint_unix_seconds ==
               right.ttl_checkpoint_unix_seconds &&
           left.ttl_checkpoint_boot_sequence ==
               right.ttl_checkpoint_boot_sequence &&
           left.boot_sequence_before_execute ==
               right.boot_sequence_before_execute;
}

CommandJournalRecord runtime_record(
    const std::uint32_t cmd_id,
    const std::uint32_t job_id) noexcept
{
    CommandJournalRecord record{};
    record.state = CommandJournalState::AcceptedPersisted;
    record.opcode = CommandOpcode::Reboot;
    record.phase = CommandAckPhase::Accepted;
    record.result = CommandResult::Accepted;
    record.error = CommandError::None;
    record.retry_count = 2;
    record.expected_effect = CommandExpectedEffect::Reset;
    record.ttl_checkpoint_clock_valid = 1;
    record.dispatch_latched = 0;
    record.cmd_id = cmd_id;
    record.job_id = job_id;
    record.ttl_seconds = 600;
    record.remaining_ttl_seconds = 590;
    record.ttl_checkpoint_monotonic_seconds = 100;
    record.ttl_checkpoint_unix_seconds = 1720000000u;
    record.ttl_checkpoint_boot_sequence = 7;
    record.boot_sequence_before_execute = 7;
    return record;
}

CommandJournalRecordV1 encoded_record(
    const CommandJournalRecord &runtime,
    const std::uint32_t sequence) noexcept
{
    CommandJournalRecordV1 record{};
    CHECK(command_journal_record_encode(runtime, sequence, record));
    return record;
}

CommandJournalRecordV1 erased_record() noexcept
{
    CommandJournalRecordV1 record{};
    std::memset(&record, 0xFF, sizeof(record));
    return record;
}

constexpr std::size_t FAKE_SECTOR_SIZE = 4096;
constexpr std::size_t FAKE_PAGE_SIZE = 256;
using FakeSlotSector =
    std::array<std::uint8_t, FAKE_SECTOR_SIZE>;

FakeSlotSector sector_with_record(
    const CommandJournalRecordV1 &record) noexcept
{
    FakeSlotSector sector{};
    sector.fill(0xFFu);
    std::memcpy(sector.data(), &record, sizeof(record));
    return sector;
}

struct FakeSlotStorage {
    FakeSlotSector slot_a{};
    FakeSlotSector slot_b{};
    CommandJournalSlot failed_read{CommandJournalSlot::None};
    std::size_t failed_read_call{0};
    std::size_t corrupt_read_call{0};
    std::size_t corrupt_read_byte{0};
    bool repair_corrupt_read_crc{false};
    bool fail_replace_before_erase{false};
    std::size_t program_bytes{FAKE_PAGE_SIZE};
    std::size_t read_count{0};
    std::size_t read_a_count{0};
    std::size_t read_b_count{0};
    std::size_t replace_count{0};
    CommandJournalSlot last_replaced_slot{CommandJournalSlot::None};
    std::array<std::uint8_t, FAKE_PAGE_SIZE> last_page{};
    std::size_t last_page_size{0};
    std::uint32_t last_timeout_ms{0};
    bool last_page_aligned_to_256{false};

    FakeSlotStorage(
        const CommandJournalRecordV1 &initial_a,
        const CommandJournalRecordV1 &initial_b,
        const CommandJournalSlot initial_failed_read =
            CommandJournalSlot::None) noexcept
        : slot_a(sector_with_record(initial_a)),
          slot_b(sector_with_record(initial_b)),
          failed_read(initial_failed_read)
    {
    }
};

FakeSlotSector *fake_slot_sector(
    FakeSlotStorage &storage,
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

const FakeSlotSector *fake_slot_sector(
    const FakeSlotStorage &storage,
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

CommandJournalRecordV1 fake_slot_record(
    const FakeSlotStorage &storage,
    const CommandJournalSlot slot) noexcept
{
    CommandJournalRecordV1 record{};
    const FakeSlotSector *const sector =
        fake_slot_sector(storage, slot);
    if (sector != nullptr) {
        std::memcpy(&record, sector->data(), sizeof(record));
    }
    return record;
}

void fake_set_slot_record(
    FakeSlotStorage &storage,
    const CommandJournalSlot slot,
    const CommandJournalRecordV1 &record) noexcept
{
    FakeSlotSector *const sector =
        fake_slot_sector(storage, slot);
    if (sector != nullptr) {
        *sector = sector_with_record(record);
    }
}

bool fake_read_slot(
    void *const context,
    const CommandJournalSlot slot,
    CommandJournalRecordV1 &output) noexcept
{
    if (context == nullptr) {
        return false;
    }
    auto &storage = *static_cast<FakeSlotStorage *>(context);
    ++storage.read_count;
    if (slot == storage.failed_read ||
        storage.read_count == storage.failed_read_call) {
        return false;
    }
    const FakeSlotSector *const sector =
        fake_slot_sector(storage, slot);
    if (sector == nullptr) {
        return false;
    }
    if (slot == CommandJournalSlot::A) {
        ++storage.read_a_count;
    } else {
        ++storage.read_b_count;
    }
    std::memcpy(&output, sector->data(), sizeof(output));
    if (storage.read_count == storage.corrupt_read_call &&
        storage.corrupt_read_byte < sizeof(output)) {
        auto *const bytes =
            reinterpret_cast<std::uint8_t *>(&output);
        bytes[storage.corrupt_read_byte] ^= 0x01u;
        if (storage.repair_corrupt_read_crc) {
            output.crc32 =
                command_journal_record_crc32(output);
        }
    }
    return true;
}

bool fake_replace_slot(
    void *const context,
    const CommandJournalSlot slot,
    const std::uint8_t *const page,
    const std::size_t page_size,
    const std::uint32_t timeout_ms) noexcept
{
    if (context == nullptr || page == nullptr ||
        page_size < sizeof(CommandJournalRecordV1) ||
        page_size > 256 ||
        (slot != CommandJournalSlot::A &&
         slot != CommandJournalSlot::B)) {
        return false;
    }

    auto &storage = *static_cast<FakeSlotStorage *>(context);
    ++storage.replace_count;
    storage.last_replaced_slot = slot;
    storage.last_page.fill(0);
    std::memcpy(storage.last_page.data(), page, page_size);
    storage.last_page_size = page_size;
    storage.last_timeout_ms = timeout_ms;
    storage.last_page_aligned_to_256 =
        reinterpret_cast<std::uintptr_t>(page) % 256u == 0;

    if (storage.fail_replace_before_erase) {
        return false;
    }

    FakeSlotSector *const target =
        fake_slot_sector(storage, slot);
    if (target == nullptr) {
        return false;
    }
    target->fill(0xFFu);
    const std::size_t bytes_to_program =
        storage.program_bytes < page_size
            ? storage.program_bytes
            : page_size;
    for (std::size_t index = 0;
         index < bytes_to_program;
         ++index) {
        (*target)[index] &= page[index];
    }
    return bytes_to_program == page_size;
}

CommandJournalStorePort fake_port(FakeSlotStorage &storage) noexcept
{
    return {
        &storage,
        fake_read_slot,
        fake_replace_slot,
    };
}

void check_non_loaded(
    const CommandJournalStoreResult result,
    const CommandJournalStoreStatus expected_status,
    const CommandJournalRecord &output,
    const CommandJournalRecord &original_output) noexcept
{
    CHECK(result.status == expected_status);
    CHECK(result.active_slot == CommandJournalSlot::None);
    CHECK(result.sequence == 0);
    CHECK(same_runtime_record(output, original_output));
}

void check_committed_page(
    const FakeSlotStorage &storage,
    const CommandJournalStoreResult result,
    const CommandJournalSlot expected_slot,
    const std::uint32_t expected_sequence,
    const CommandJournalRecord &expected_runtime,
    const std::uint32_t expected_timeout_ms,
    const std::size_t expected_replace_count) noexcept
{
    CHECK(result.status == CommandJournalStoreStatus::Committed);
    CHECK(result.active_slot == expected_slot);
    CHECK(result.sequence == expected_sequence);
    CHECK(storage.replace_count == expected_replace_count);
    CHECK(storage.last_replaced_slot == expected_slot);
    CHECK(storage.last_page_size == 256);
    CHECK(storage.last_timeout_ms == expected_timeout_ms);
    CHECK(storage.last_page_aligned_to_256);

    CommandJournalRecordV1 persisted{};
    std::memcpy(
        &persisted,
        storage.last_page.data(),
        sizeof(persisted));
    CHECK(persisted.magic == COMMAND_JOURNAL_RECORD_MAGIC);
    CHECK(persisted.version == COMMAND_JOURNAL_RECORD_VERSION);
    CHECK(persisted.size == 64);
    CHECK(persisted.sequence == expected_sequence);
    CHECK(command_journal_record_valid(persisted));

    CommandJournalRecord decoded{};
    CHECK(command_journal_record_decode(persisted, decoded));
    CHECK(same_runtime_record(decoded, expected_runtime));

    for (std::size_t index = sizeof(persisted);
         index < storage.last_page.size();
         ++index) {
        CHECK(storage.last_page[index] == 0xFFu);
    }

    const CommandJournalRecordV1 target =
        fake_slot_record(storage, expected_slot);
    CHECK(std::memcmp(&target, &persisted, sizeof(target)) == 0);
}

void check_failed_commit(
    const CommandJournalStoreResult result,
    const CommandJournalStoreStatus expected_status,
    const FakeSlotStorage &storage,
    const std::size_t expected_replace_count) noexcept
{
    CHECK(result.status == expected_status);
    CHECK(result.active_slot == CommandJournalSlot::None);
    CHECK(result.sequence == 0);
    CHECK(storage.replace_count == expected_replace_count);
}

bool sector_matches_programmed_prefix(
    const FakeSlotSector &sector,
    const std::array<std::uint8_t, FAKE_PAGE_SIZE> &page,
    const std::size_t programmed_bytes) noexcept
{
    for (std::size_t index = 0;
         index < sector.size();
         ++index) {
        const std::uint8_t expected =
            index < programmed_bytes ? page[index] : 0xFFu;
        if (sector[index] != expected) {
            return false;
        }
    }
    return true;
}

void reset_read_injection(FakeSlotStorage &storage) noexcept
{
    storage.failed_read = CommandJournalSlot::None;
    storage.failed_read_call = 0;
    storage.corrupt_read_call = 0;
    storage.corrupt_read_byte = 0;
    storage.repair_corrupt_read_crc = false;
    storage.read_count = 0;
    storage.read_a_count = 0;
    storage.read_b_count = 0;
}

void check_loaded_record(
    FakeSlotStorage &storage,
    const CommandJournalSlot expected_slot,
    const std::uint32_t expected_sequence,
    const CommandJournalRecord &expected_record) noexcept
{
    reset_read_injection(storage);
    const std::size_t replace_count_before_load =
        storage.replace_count;
    CommandJournalRecord output = runtime_record(990, 991);

    const CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);

    CHECK(result.status == CommandJournalStoreStatus::Loaded);
    CHECK(result.active_slot == expected_slot);
    CHECK(result.sequence == expected_sequence);
    CHECK(same_runtime_record(output, expected_record));
    CHECK(storage.read_count == 2);
    CHECK(storage.replace_count == replace_count_before_load);
}

void test_both_erased_returns_blank_without_changing_output() noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
        CommandJournalSlot::None,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    const CommandJournalRecord original_output = output;

    const CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);

    check_non_loaded(
        result,
        CommandJournalStoreStatus::Blank,
        output,
        original_output);
}

void test_newest_valid_slot_is_loaded_and_decoded() noexcept
{
    const CommandJournalRecord older = runtime_record(17, 23);
    const CommandJournalRecord newer = runtime_record(18, 24);
    FakeSlotStorage storage{
        encoded_record(older, 10),
        encoded_record(newer, 11),
        CommandJournalSlot::None,
    };
    CommandJournalRecord output = runtime_record(900, 901);

    const CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);

    CHECK(result.status == CommandJournalStoreStatus::Loaded);
    CHECK(result.active_slot == CommandJournalSlot::B);
    CHECK(result.sequence == 11);
    CHECK(same_runtime_record(output, newer));
}

void test_valid_record_is_loaded_when_peer_is_corrupt() noexcept
{
    const CommandJournalRecord valid_a = runtime_record(31, 41);
    FakeSlotStorage storage{
        encoded_record(valid_a, 20),
        CommandJournalRecordV1{},
        CommandJournalSlot::None,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);
    CHECK(result.status == CommandJournalStoreStatus::Loaded);
    CHECK(result.active_slot == CommandJournalSlot::A);
    CHECK(result.sequence == 20);
    CHECK(same_runtime_record(output, valid_a));

    const CommandJournalRecord valid_b = runtime_record(32, 42);
    fake_set_slot_record(
        storage, CommandJournalSlot::A, CommandJournalRecordV1{});
    fake_set_slot_record(
        storage,
        CommandJournalSlot::B,
        encoded_record(valid_b, 21));
    output = runtime_record(900, 901);
    result = command_journal_store_load(fake_port(storage), output);
    CHECK(result.status == CommandJournalStoreStatus::Loaded);
    CHECK(result.active_slot == CommandJournalSlot::B);
    CHECK(result.sequence == 21);
    CHECK(same_runtime_record(output, valid_b));
}

void test_corrupt_only_storage_is_rejected_without_changing_output() noexcept
{
    FakeSlotStorage storage{
        CommandJournalRecordV1{},
        erased_record(),
        CommandJournalSlot::None,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    const CommandJournalRecord original_output = output;
    CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);
    check_non_loaded(
        result,
        CommandJournalStoreStatus::Rejected,
        output,
        original_output);

    fake_set_slot_record(
        storage, CommandJournalSlot::A, erased_record());
    fake_set_slot_record(
        storage, CommandJournalSlot::B, CommandJournalRecordV1{});
    output = original_output;
    result = command_journal_store_load(fake_port(storage), output);
    check_non_loaded(
        result,
        CommandJournalStoreStatus::Rejected,
        output,
        original_output);

    fake_set_slot_record(
        storage, CommandJournalSlot::A, CommandJournalRecordV1{});
    fake_set_slot_record(
        storage, CommandJournalSlot::B, CommandJournalRecordV1{});
    output = original_output;
    result = command_journal_store_load(fake_port(storage), output);
    check_non_loaded(
        result,
        CommandJournalStoreStatus::Rejected,
        output,
        original_output);
}

void test_divergent_same_sequence_is_rejected_without_changing_output()
    noexcept
{
    FakeSlotStorage storage{
        encoded_record(runtime_record(51, 61), 30),
        encoded_record(runtime_record(52, 62), 30),
        CommandJournalSlot::None,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    const CommandJournalRecord original_output = output;

    const CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);

    check_non_loaded(
        result,
        CommandJournalStoreStatus::Rejected,
        output,
        original_output);
}

void test_half_range_sequence_is_rejected_without_changing_output()
    noexcept
{
    FakeSlotStorage storage{
        encoded_record(runtime_record(71, 81), 1),
        encoded_record(runtime_record(72, 82), 0x80000001u),
        CommandJournalSlot::None,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    const CommandJournalRecord original_output = output;

    const CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);

    check_non_loaded(
        result,
        CommandJournalStoreStatus::Rejected,
        output,
        original_output);
}

void test_null_read_callback_is_invalid_without_changing_output() noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
        CommandJournalSlot::None,
    };
    const CommandJournalStorePort port{
        &storage,
        nullptr,
        fake_replace_slot,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    const CommandJournalRecord original_output = output;

    const CommandJournalStoreResult result =
        command_journal_store_load(port, output);

    check_non_loaded(
        result,
        CommandJournalStoreStatus::InvalidInput,
        output,
        original_output);
}

void test_null_replace_callback_is_invalid_without_changing_output()
    noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
        CommandJournalSlot::None,
    };
    const CommandJournalStorePort port{
        &storage,
        fake_read_slot,
        nullptr,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    const CommandJournalRecord original_output = output;

    const CommandJournalStoreResult result =
        command_journal_store_load(port, output);

    check_non_loaded(
        result,
        CommandJournalStoreStatus::InvalidInput,
        output,
        original_output);
}

void test_slot_read_failure_keeps_output_unchanged() noexcept
{
    FakeSlotStorage storage{
        encoded_record(runtime_record(91, 101), 40),
        encoded_record(runtime_record(92, 102), 41),
        CommandJournalSlot::A,
    };
    CommandJournalRecord output = runtime_record(900, 901);
    const CommandJournalRecord original_output = output;
    CommandJournalStoreResult result =
        command_journal_store_load(fake_port(storage), output);
    check_non_loaded(
        result,
        CommandJournalStoreStatus::ReadFailed,
        output,
        original_output);

    storage.failed_read = CommandJournalSlot::B;
    output = original_output;
    result = command_journal_store_load(fake_port(storage), output);
    check_non_loaded(
        result,
        CommandJournalStoreStatus::ReadFailed,
        output,
        original_output);
}

void test_blank_commit_starts_at_a_then_alternates_b_a_b() noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
        CommandJournalSlot::None,
    };
    constexpr std::uint32_t timeout_ms = 4321;

    const CommandJournalRecord first = runtime_record(101, 201);
    CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(storage), first, timeout_ms);
    check_committed_page(
        storage,
        result,
        CommandJournalSlot::A,
        1,
        first,
        timeout_ms,
        1);
    CHECK(storage.read_a_count == 2);
    CHECK(storage.read_b_count == 1);

    const CommandJournalRecord second = runtime_record(102, 202);
    result = command_journal_store_commit(
        fake_port(storage), second, timeout_ms);
    check_committed_page(
        storage,
        result,
        CommandJournalSlot::B,
        2,
        second,
        timeout_ms,
        2);
    CHECK(storage.read_a_count == 3);
    CHECK(storage.read_b_count == 3);

    const CommandJournalRecord third = runtime_record(103, 203);
    result = command_journal_store_commit(
        fake_port(storage), third, timeout_ms);
    check_committed_page(
        storage,
        result,
        CommandJournalSlot::A,
        3,
        third,
        timeout_ms,
        3);
    CHECK(storage.read_a_count == 5);
    CHECK(storage.read_b_count == 4);

    const CommandJournalRecord fourth = runtime_record(104, 204);
    result = command_journal_store_commit(
        fake_port(storage), fourth, timeout_ms);
    check_committed_page(
        storage,
        result,
        CommandJournalSlot::B,
        4,
        fourth,
        timeout_ms,
        4);
    CHECK(storage.read_a_count == 6);
    CHECK(storage.read_b_count == 6);
}

void test_commit_wraps_max_sequence_to_one_in_next_slot() noexcept
{
    FakeSlotStorage storage{
        encoded_record(
            runtime_record(301, 401),
            0xFFFFFFFFu),
        erased_record(),
        CommandJournalSlot::None,
    };
    const CommandJournalRecord next = runtime_record(302, 402);
    constexpr std::uint32_t timeout_ms = 9876;

    const CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(storage), next, timeout_ms);

    check_committed_page(
        storage,
        result,
        CommandJournalSlot::B,
        1,
        next,
        timeout_ms,
        1);
    CHECK(storage.read_a_count == 1);
    CHECK(storage.read_b_count == 2);
}

void test_empty_tombstone_commits_as_canonical_record() noexcept
{
    FakeSlotStorage storage{
        encoded_record(runtime_record(501, 601), 8),
        erased_record(),
        CommandJournalSlot::None,
    };
    const CommandJournalRecord tombstone{};
    constexpr std::uint32_t timeout_ms = 2468;

    const CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(storage), tombstone, timeout_ms);

    check_committed_page(
        storage,
        result,
        CommandJournalSlot::B,
        9,
        tombstone,
        timeout_ms,
        1);
    CHECK(
        fake_slot_record(storage, CommandJournalSlot::B).state ==
        CommandJournalState::Empty);
}

void test_commit_rejects_invalid_inputs_without_storage_access() noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
        CommandJournalSlot::None,
    };
    const CommandJournalRecord valid = runtime_record(701, 801);
    constexpr std::uint32_t timeout_ms = 1000;

    CommandJournalStoreResult result =
        command_journal_store_commit(
            {&storage, nullptr, fake_replace_slot},
            valid,
            timeout_ms);
    CHECK(result.status == CommandJournalStoreStatus::InvalidInput);
    CHECK(result.active_slot == CommandJournalSlot::None);
    CHECK(result.sequence == 0);

    result = command_journal_store_commit(
        {&storage, fake_read_slot, nullptr},
        valid,
        timeout_ms);
    CHECK(result.status == CommandJournalStoreStatus::InvalidInput);
    CHECK(result.active_slot == CommandJournalSlot::None);
    CHECK(result.sequence == 0);

    result = command_journal_store_commit(
        fake_port(storage), valid, 0);
    CHECK(result.status == CommandJournalStoreStatus::InvalidInput);
    CHECK(result.active_slot == CommandJournalSlot::None);
    CHECK(result.sequence == 0);

    CommandJournalRecord non_canonical = valid;
    non_canonical.reserved = 1;
    result = command_journal_store_commit(
        fake_port(storage), non_canonical, timeout_ms);
    CHECK(result.status == CommandJournalStoreStatus::InvalidInput);
    CHECK(result.active_slot == CommandJournalSlot::None);
    CHECK(result.sequence == 0);

    CHECK(storage.read_a_count == 0);
    CHECK(storage.read_b_count == 0);
    CHECK(storage.replace_count == 0);
}

void test_commit_initial_read_failures_are_read_failed_without_write()
    noexcept
{
    const CommandJournalRecord next = runtime_record(801, 901);
    constexpr std::uint32_t timeout_ms = 1000;

    FakeSlotStorage fail_a{
        erased_record(),
        erased_record(),
    };
    fail_a.failed_read_call = 1;
    CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(fail_a), next, timeout_ms);
    check_failed_commit(
        result,
        CommandJournalStoreStatus::ReadFailed,
        fail_a,
        0);
    CHECK(fail_a.read_count == 1);

    FakeSlotStorage fail_b{
        erased_record(),
        erased_record(),
    };
    fail_b.failed_read_call = 2;
    result = command_journal_store_commit(
        fake_port(fail_b), next, timeout_ms);
    check_failed_commit(
        result,
        CommandJournalStoreStatus::ReadFailed,
        fail_b,
        0);
    CHECK(fail_b.read_count == 2);
}

void test_commit_replace_failure_is_write_failed() noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
    };
    const FakeSlotSector original_a = storage.slot_a;
    const FakeSlotSector original_b = storage.slot_b;
    storage.fail_replace_before_erase = true;

    const CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(storage), runtime_record(802, 902), 1000);

    check_failed_commit(
        result,
        CommandJournalStoreStatus::WriteFailed,
        storage,
        1);
    CHECK(storage.last_replaced_slot == CommandJournalSlot::A);
    CHECK(storage.slot_a == original_a);
    CHECK(storage.slot_b == original_b);
}

void test_commit_post_write_read_failure_is_verify_failed() noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
    };
    storage.failed_read_call = 3;

    const CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(storage), runtime_record(803, 903), 1000);

    check_failed_commit(
        result,
        CommandJournalStoreStatus::VerifyFailed,
        storage,
        1);
    CHECK(storage.read_count == 3);
    CHECK(
        command_journal_record_valid(
            fake_slot_record(storage, CommandJournalSlot::A)));
}

void test_commit_readback_mismatches_are_verify_failed() noexcept
{
    const CommandJournalRecord next = runtime_record(804, 904);

    FakeSlotStorage byte_mismatch{
        erased_record(),
        erased_record(),
    };
    byte_mismatch.corrupt_read_call = 3;
    byte_mismatch.corrupt_read_byte =
        offsetof(CommandJournalRecordV1, cmd_id);
    byte_mismatch.repair_corrupt_read_crc = true;
    CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(byte_mismatch), next, 1000);
    check_failed_commit(
        result,
        CommandJournalStoreStatus::VerifyFailed,
        byte_mismatch,
        1);

    FakeSlotStorage crc_mismatch{
        erased_record(),
        erased_record(),
    };
    crc_mismatch.corrupt_read_call = 3;
    crc_mismatch.corrupt_read_byte =
        offsetof(CommandJournalRecordV1, crc32);
    result = command_journal_store_commit(
        fake_port(crc_mismatch), next, 1000);
    check_failed_commit(
        result,
        CommandJournalStoreStatus::VerifyFailed,
        crc_mismatch,
        1);
}

void test_commit_rejected_storage_states_do_not_write() noexcept
{
    const CommandJournalRecord next = runtime_record(805, 905);
    constexpr std::uint32_t timeout_ms = 1000;

    FakeSlotStorage corrupt_only{
        CommandJournalRecordV1{},
        erased_record(),
    };
    CommandJournalStoreResult result =
        command_journal_store_commit(
            fake_port(corrupt_only), next, timeout_ms);
    check_failed_commit(
        result,
        CommandJournalStoreStatus::Rejected,
        corrupt_only,
        0);

    FakeSlotStorage split_brain{
        encoded_record(runtime_record(806, 906), 50),
        encoded_record(runtime_record(807, 907), 50),
    };
    result = command_journal_store_commit(
        fake_port(split_brain), next, timeout_ms);
    check_failed_commit(
        result,
        CommandJournalStoreStatus::Rejected,
        split_brain,
        0);

    FakeSlotStorage half_range{
        encoded_record(runtime_record(808, 908), 1),
        encoded_record(runtime_record(809, 909), 0x80000001u),
    };
    result = command_journal_store_commit(
        fake_port(half_range), next, timeout_ms);
    check_failed_commit(
        result,
        CommandJournalStoreStatus::Rejected,
        half_range,
        0);
}

void test_power_cut_before_complete_record_recovers_active_a() noexcept
{
    const CommandJournalRecord active = runtime_record(901, 1001);
    const CommandJournalRecord older = runtime_record(900, 1000);
    const CommandJournalRecord next = runtime_record(902, 1002);

    for (std::size_t programmed_bytes = 0;
         programmed_bytes < sizeof(CommandJournalRecordV1);
         ++programmed_bytes) {
        FakeSlotStorage storage{
            encoded_record(active, 40),
            encoded_record(older, 39),
        };
        const FakeSlotSector active_before = storage.slot_a;
        storage.program_bytes = programmed_bytes;

        const CommandJournalStoreResult commit =
            command_journal_store_commit(
                fake_port(storage), next, 1000);

        check_failed_commit(
            commit,
            CommandJournalStoreStatus::WriteFailed,
            storage,
            1);
        CHECK(storage.read_count == 2);
        CHECK(
            storage.last_replaced_slot ==
            CommandJournalSlot::B);
        CHECK(storage.slot_a == active_before);
        CHECK(storage.last_page_size == FAKE_PAGE_SIZE);
        CHECK(
            sector_matches_programmed_prefix(
                storage.slot_b,
                storage.last_page,
                programmed_bytes));
        CHECK(
            command_journal_record_classify(
                fake_slot_record(storage, CommandJournalSlot::B)) ==
            (programmed_bytes == 0
                 ? CommandJournalSlotState::Blank
                 : CommandJournalSlotState::Corrupt));

        check_loaded_record(
            storage, CommandJournalSlot::A, 40, active);
        CHECK(storage.slot_a == active_before);
    }
}

void test_power_cut_after_complete_record_recovers_new_b() noexcept
{
    const CommandJournalRecord active = runtime_record(911, 1011);
    const CommandJournalRecord older = runtime_record(910, 1010);
    const CommandJournalRecord next = runtime_record(912, 1012);
    constexpr std::array<std::size_t, 7> PROGRAM_POINTS{
        64,
        65,
        96,
        128,
        192,
        255,
        256,
    };

    for (const std::size_t programmed_bytes : PROGRAM_POINTS) {
        FakeSlotStorage storage{
            encoded_record(active, 50),
            encoded_record(older, 49),
        };
        const FakeSlotSector active_before = storage.slot_a;
        storage.program_bytes = programmed_bytes;

        const CommandJournalStoreResult commit =
            command_journal_store_commit(
                fake_port(storage), next, 1000);

        if (programmed_bytes < FAKE_PAGE_SIZE) {
            check_failed_commit(
                commit,
                CommandJournalStoreStatus::WriteFailed,
                storage,
                1);
            CHECK(storage.read_count == 2);
        } else {
            CHECK(
                commit.status ==
                CommandJournalStoreStatus::Committed);
            CHECK(
                commit.active_slot ==
                CommandJournalSlot::B);
            CHECK(commit.sequence == 51);
            CHECK(storage.replace_count == 1);
            CHECK(storage.read_count == 3);
        }
        CHECK(
            storage.last_replaced_slot ==
            CommandJournalSlot::B);
        CHECK(storage.slot_a == active_before);
        CHECK(storage.last_page_size == FAKE_PAGE_SIZE);
        CHECK(
            sector_matches_programmed_prefix(
                storage.slot_b,
                storage.last_page,
                programmed_bytes));
        const CommandJournalRecordV1 persisted =
            fake_slot_record(storage, CommandJournalSlot::B);
        CHECK(command_journal_record_valid(persisted));
        CHECK(persisted.sequence == 51);

        check_loaded_record(
            storage, CommandJournalSlot::B, 51, next);
        CHECK(storage.slot_a == active_before);
    }
}

void test_first_commit_partial_record_is_rejected_on_recovery() noexcept
{
    const CommandJournalRecord first = runtime_record(921, 1021);

    for (std::size_t programmed_bytes = 1;
         programmed_bytes < sizeof(CommandJournalRecordV1);
         ++programmed_bytes) {
        FakeSlotStorage storage{
            erased_record(),
            erased_record(),
        };
        storage.program_bytes = programmed_bytes;

        const CommandJournalStoreResult commit =
            command_journal_store_commit(
                fake_port(storage), first, 1000);

        check_failed_commit(
            commit,
            CommandJournalStoreStatus::WriteFailed,
            storage,
            1);
        CHECK(storage.read_count == 2);
        CHECK(
            storage.last_replaced_slot ==
            CommandJournalSlot::A);
        CHECK(
            sector_matches_programmed_prefix(
                storage.slot_a,
                storage.last_page,
                programmed_bytes));
        CHECK(
            command_journal_record_classify(
                fake_slot_record(storage, CommandJournalSlot::A)) ==
            CommandJournalSlotState::Corrupt);
        CHECK(
            command_journal_record_classify(
                fake_slot_record(storage, CommandJournalSlot::B)) ==
            CommandJournalSlotState::Blank);

        reset_read_injection(storage);
        CommandJournalRecord output = runtime_record(992, 993);
        const CommandJournalRecord original_output = output;
        const CommandJournalStoreResult recovered =
            command_journal_store_load(fake_port(storage), output);
        check_non_loaded(
            recovered,
            CommandJournalStoreStatus::Rejected,
            output,
            original_output);
        CHECK(storage.read_count == 2);
        CHECK(storage.replace_count == 1);
    }
}

void test_first_commit_cut_after_erase_remains_blank() noexcept
{
    FakeSlotStorage storage{
        erased_record(),
        erased_record(),
    };
    storage.program_bytes = 0;
    const CommandJournalStoreResult commit =
        command_journal_store_commit(
            fake_port(storage), runtime_record(922, 1022), 1000);
    check_failed_commit(
        commit,
        CommandJournalStoreStatus::WriteFailed,
        storage,
        1);
    CHECK(storage.last_replaced_slot == CommandJournalSlot::A);
    CHECK(
        sector_matches_programmed_prefix(
            storage.slot_a, storage.last_page, 0));

    reset_read_injection(storage);
    CommandJournalRecord output = runtime_record(994, 995);
    const CommandJournalRecord original_output = output;
    const CommandJournalStoreResult recovered =
        command_journal_store_load(fake_port(storage), output);
    check_non_loaded(
        recovered,
        CommandJournalStoreStatus::Blank,
        output,
        original_output);
    CHECK(storage.replace_count == 1);
}

} // namespace

int main()
{
    test_both_erased_returns_blank_without_changing_output();
    test_newest_valid_slot_is_loaded_and_decoded();
    test_valid_record_is_loaded_when_peer_is_corrupt();
    test_corrupt_only_storage_is_rejected_without_changing_output();
    test_divergent_same_sequence_is_rejected_without_changing_output();
    test_half_range_sequence_is_rejected_without_changing_output();
    test_null_read_callback_is_invalid_without_changing_output();
    test_null_replace_callback_is_invalid_without_changing_output();
    test_slot_read_failure_keeps_output_unchanged();
    test_blank_commit_starts_at_a_then_alternates_b_a_b();
    test_commit_wraps_max_sequence_to_one_in_next_slot();
    test_empty_tombstone_commits_as_canonical_record();
    test_commit_rejects_invalid_inputs_without_storage_access();
    test_commit_initial_read_failures_are_read_failed_without_write();
    test_commit_replace_failure_is_write_failed();
    test_commit_post_write_read_failure_is_verify_failed();
    test_commit_readback_mismatches_are_verify_failed();
    test_commit_rejected_storage_states_do_not_write();
    test_power_cut_before_complete_record_recovers_active_a();
    test_power_cut_after_complete_record_recovers_new_b();
    test_first_commit_partial_record_is_rejected_on_recovery();
    test_first_commit_cut_after_erase_remains_blank();

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "command_journal_store_core_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "command_journal_store_core_test: %zu checks passed\n",
        g_checks);
    return 0;
}
