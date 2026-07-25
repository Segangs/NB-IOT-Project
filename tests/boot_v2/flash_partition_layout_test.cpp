#include "flash_partition_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace boot_v2::flash_partition;

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

struct Region {
    std::uint32_t offset;
    std::uint32_t size;
};

std::string read_source(const char *const relative_path)
{
    const std::string path =
        std::string(NB_IOT_SOURCE_ROOT) + "/" + relative_path;
    std::ifstream source(path);
    return {
        std::istreambuf_iterator<char>(source),
        std::istreambuf_iterator<char>()};
}

void test_consumers_use_central_partition_layout()
{
    const std::string flash_logger_header =
        read_source("src/lib/flash_logger.hpp");
    const std::string shutdown_store =
        read_source("src/boot_v2/runtime_owner_shutdown_record_store.cpp");
    const std::string flash_logger =
        read_source("src/lib/flash_logger.cpp");

    CHECK(flash_logger_header.find("#define FLASH_LOG_OFFSET") ==
          std::string::npos);
    CHECK(flash_logger_header.find("#define SHUTDOWN_RECORD_SLOT_A_OFFSET") ==
          std::string::npos);
    CHECK(shutdown_store.find("flash_partition::shutdown_record_a_offset") !=
          std::string::npos);
    CHECK(flash_logger.find("flash_partition::sensor_log_offset") !=
          std::string::npos);
}

void test_exact_partition_table() noexcept
{
    CHECK(version == 1u);
    CHECK(total_size == 0x400000u);
    CHECK(sector_size == 0x001000u);
    CHECK(page_size == 0x000100u);

    CHECK(bootloader_offset == 0x000000u);
    CHECK(bootloader_size == 0x020000u);
    CHECK(firmware_a_offset == 0x020000u);
    CHECK(firmware_a_size == 0x140000u);
    CHECK(firmware_b_offset == 0x160000u);
    CHECK(firmware_b_size == 0x140000u);
    CHECK(model_a_offset == 0x2A0000u);
    CHECK(model_a_size == 0x040000u);
    CHECK(model_b_offset == 0x2E0000u);
    CHECK(model_b_size == 0x040000u);
    CHECK(audio_offset == 0x320000u);
    CHECK(audio_size == 0x0C8000u);
    CHECK(service_offset == 0x3E8000u);
    CHECK(service_size == 0x018000u);

    CHECK(audio_slot_count == 5u);
    CHECK(audio_slot_size == 0x028000u);
    CHECK(audio_slot_0_offset == 0x320000u);
    CHECK(audio_slot_1_offset == 0x348000u);
    CHECK(audio_slot_2_offset == 0x370000u);
    CHECK(audio_slot_3_offset == 0x398000u);
    CHECK(audio_slot_4_offset == 0x3C0000u);

    CHECK(model_metadata_a_offset == 0x3E8000u);
    CHECK(model_metadata_b_offset == 0x3E9000u);
    CHECK(model_metadata_slot_size == 0x001000u);
    CHECK(boot_metadata_a_offset == 0x3EA000u);
    CHECK(boot_metadata_b_offset == 0x3EB000u);
    CHECK(boot_metadata_slot_size == 0x001000u);
    CHECK(command_journal_a_offset == 0x3EC000u);
    CHECK(command_journal_b_offset == 0x3ED000u);
    CHECK(command_journal_slot_size == 0x001000u);
    CHECK(metadata_scratch_offset == 0x3EE000u);
    CHECK(metadata_scratch_size == 0x002000u);
    CHECK(sensor_log_offset == 0x3F0000u);
    CHECK(sensor_log_size == 0x00E000u);
    CHECK(shutdown_record_a_offset == 0x3FE000u);
    CHECK(shutdown_record_b_offset == 0x3FF000u);
    CHECK(shutdown_record_slot_size == 0x001000u);
}

void test_top_level_regions_are_exactly_adjacent() noexcept
{
    CHECK(end(bootloader_offset, bootloader_size) == firmware_a_offset);
    CHECK(end(firmware_a_offset, firmware_a_size) == firmware_b_offset);
    CHECK(end(firmware_b_offset, firmware_b_size) == model_a_offset);
    CHECK(end(model_a_offset, model_a_size) == model_b_offset);
    CHECK(end(model_b_offset, model_b_size) == audio_offset);
    CHECK(end(audio_offset, audio_size) == service_offset);
    CHECK(end(service_offset, service_size) == total_size);
}

void test_audio_slots_are_five_exact_subdivisions() noexcept
{
    CHECK(audio_slot_0_offset == audio_offset);
    CHECK(end(audio_slot_0_offset, audio_slot_size) == audio_slot_1_offset);
    CHECK(end(audio_slot_1_offset, audio_slot_size) == audio_slot_2_offset);
    CHECK(end(audio_slot_2_offset, audio_slot_size) == audio_slot_3_offset);
    CHECK(end(audio_slot_3_offset, audio_slot_size) == audio_slot_4_offset);
    CHECK(end(audio_slot_4_offset, audio_slot_size) ==
          end(audio_offset, audio_size));
}

void test_service_regions_are_exactly_adjacent() noexcept
{
    CHECK(service_offset == model_metadata_a_offset);
    CHECK(end(model_metadata_a_offset, model_metadata_slot_size) ==
          model_metadata_b_offset);
    CHECK(end(model_metadata_b_offset, model_metadata_slot_size) ==
          boot_metadata_a_offset);
    CHECK(end(boot_metadata_a_offset, boot_metadata_slot_size) ==
          boot_metadata_b_offset);
    CHECK(end(boot_metadata_b_offset, boot_metadata_slot_size) ==
          command_journal_a_offset);
    CHECK(end(command_journal_a_offset, command_journal_slot_size) ==
          command_journal_b_offset);
    CHECK(end(command_journal_b_offset, command_journal_slot_size) ==
          metadata_scratch_offset);
    CHECK(end(metadata_scratch_offset, metadata_scratch_size) ==
          sensor_log_offset);
    CHECK(end(sensor_log_offset, sensor_log_size) ==
          shutdown_record_a_offset);
    CHECK(end(shutdown_record_a_offset, shutdown_record_slot_size) ==
          shutdown_record_b_offset);
    CHECK(end(shutdown_record_b_offset, shutdown_record_slot_size) ==
          total_size);
}

void test_every_region_is_sector_aligned() noexcept
{
    const std::array<Region, 22> regions{{
        {bootloader_offset, bootloader_size},
        {firmware_a_offset, firmware_a_size},
        {firmware_b_offset, firmware_b_size},
        {model_a_offset, model_a_size},
        {model_b_offset, model_b_size},
        {audio_offset, audio_size},
        {service_offset, service_size},
        {audio_slot_0_offset, audio_slot_size},
        {audio_slot_1_offset, audio_slot_size},
        {audio_slot_2_offset, audio_slot_size},
        {audio_slot_3_offset, audio_slot_size},
        {audio_slot_4_offset, audio_slot_size},
        {model_metadata_a_offset, model_metadata_slot_size},
        {model_metadata_b_offset, model_metadata_slot_size},
        {boot_metadata_a_offset, boot_metadata_slot_size},
        {boot_metadata_b_offset, boot_metadata_slot_size},
        {command_journal_a_offset, command_journal_slot_size},
        {command_journal_b_offset, command_journal_slot_size},
        {metadata_scratch_offset, metadata_scratch_size},
        {sensor_log_offset, sensor_log_size},
        {shutdown_record_a_offset, shutdown_record_slot_size},
        {shutdown_record_b_offset, shutdown_record_slot_size},
    }};

    for (const Region region : regions) {
        CHECK(region.offset % sector_size == 0u);
        CHECK(region.size % sector_size == 0u);
    }
    CHECK(total_size % sector_size == 0u);
    CHECK(sector_size % page_size == 0u);
}

} // namespace

int main()
{
    test_consumers_use_central_partition_layout();
    test_exact_partition_table();
    test_top_level_regions_are_exactly_adjacent();
    test_audio_slots_are_five_exact_subdivisions();
    test_service_regions_are_exactly_adjacent();
    test_every_region_is_sector_aligned();

    if (g_failures != 0u) {
        std::fprintf(
            stderr,
            "flash_partition_layout_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "flash_partition_layout_test: %zu checks passed\n",
        g_checks);
    return 0;
}
