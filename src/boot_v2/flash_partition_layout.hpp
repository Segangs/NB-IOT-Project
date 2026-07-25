#pragma once

#include <cstdint>

namespace boot_v2::flash_partition {

inline constexpr std::uint32_t version = 1u;
inline constexpr std::uint32_t total_size = 0x400000u;
inline constexpr std::uint32_t sector_size = 0x001000u;
inline constexpr std::uint32_t page_size = 0x000100u;

inline constexpr std::uint32_t bootloader_offset = 0x000000u;
inline constexpr std::uint32_t bootloader_size = 0x020000u;
inline constexpr std::uint32_t firmware_a_offset = 0x020000u;
inline constexpr std::uint32_t firmware_a_size = 0x140000u;
inline constexpr std::uint32_t firmware_b_offset = 0x160000u;
inline constexpr std::uint32_t firmware_b_size = 0x140000u;
inline constexpr std::uint32_t model_a_offset = 0x2A0000u;
inline constexpr std::uint32_t model_a_size = 0x040000u;
inline constexpr std::uint32_t model_b_offset = 0x2E0000u;
inline constexpr std::uint32_t model_b_size = 0x040000u;
inline constexpr std::uint32_t audio_offset = 0x320000u;
inline constexpr std::uint32_t audio_size = 0x0C8000u;
inline constexpr std::uint32_t service_offset = 0x3E8000u;
inline constexpr std::uint32_t service_size = 0x018000u;

inline constexpr std::uint32_t audio_slot_count = 5u;
inline constexpr std::uint32_t audio_slot_size = 0x028000u;
inline constexpr std::uint32_t audio_slot_0_offset = 0x320000u;
inline constexpr std::uint32_t audio_slot_1_offset = 0x348000u;
inline constexpr std::uint32_t audio_slot_2_offset = 0x370000u;
inline constexpr std::uint32_t audio_slot_3_offset = 0x398000u;
inline constexpr std::uint32_t audio_slot_4_offset = 0x3C0000u;

inline constexpr std::uint32_t model_metadata_a_offset = 0x3E8000u;
inline constexpr std::uint32_t model_metadata_b_offset = 0x3E9000u;
inline constexpr std::uint32_t model_metadata_slot_size = 0x001000u;
inline constexpr std::uint32_t boot_metadata_a_offset = 0x3EA000u;
inline constexpr std::uint32_t boot_metadata_b_offset = 0x3EB000u;
inline constexpr std::uint32_t boot_metadata_slot_size = 0x001000u;
inline constexpr std::uint32_t command_journal_a_offset = 0x3EC000u;
inline constexpr std::uint32_t command_journal_b_offset = 0x3ED000u;
inline constexpr std::uint32_t command_journal_slot_size = 0x001000u;
inline constexpr std::uint32_t metadata_scratch_offset = 0x3EE000u;
inline constexpr std::uint32_t metadata_scratch_size = 0x002000u;
inline constexpr std::uint32_t sensor_log_offset = 0x3F0000u;
inline constexpr std::uint32_t sensor_log_size = 0x00E000u;
inline constexpr std::uint32_t shutdown_record_a_offset = 0x3FE000u;
inline constexpr std::uint32_t shutdown_record_b_offset = 0x3FF000u;
inline constexpr std::uint32_t shutdown_record_slot_size = 0x001000u;

constexpr std::uint32_t end(
    const std::uint32_t offset,
    const std::uint32_t size) noexcept
{
    return offset + size;
}

static_assert(firmware_a_size == firmware_b_size);
static_assert(end(bootloader_offset, bootloader_size) == firmware_a_offset);
static_assert(end(firmware_a_offset, firmware_a_size) == firmware_b_offset);
static_assert(end(firmware_b_offset, firmware_b_size) == model_a_offset);
static_assert(end(model_a_offset, model_a_size) == model_b_offset);
static_assert(end(model_b_offset, model_b_size) == audio_offset);
static_assert(end(audio_offset, audio_size) == service_offset);
static_assert(end(service_offset, service_size) == total_size);

static_assert(audio_slot_0_offset == audio_offset);
static_assert(end(audio_slot_0_offset, audio_slot_size) ==
              audio_slot_1_offset);
static_assert(end(audio_slot_1_offset, audio_slot_size) ==
              audio_slot_2_offset);
static_assert(end(audio_slot_2_offset, audio_slot_size) ==
              audio_slot_3_offset);
static_assert(end(audio_slot_3_offset, audio_slot_size) ==
              audio_slot_4_offset);
static_assert(end(audio_slot_4_offset, audio_slot_size) ==
              end(audio_offset, audio_size));
static_assert(audio_slot_count * audio_slot_size == audio_size);

static_assert(service_offset == model_metadata_a_offset);
static_assert(end(model_metadata_a_offset, model_metadata_slot_size) ==
              model_metadata_b_offset);
static_assert(end(model_metadata_b_offset, model_metadata_slot_size) ==
              boot_metadata_a_offset);
static_assert(end(boot_metadata_a_offset, boot_metadata_slot_size) ==
              boot_metadata_b_offset);
static_assert(end(boot_metadata_b_offset, boot_metadata_slot_size) ==
              command_journal_a_offset);
static_assert(end(command_journal_a_offset, command_journal_slot_size) ==
              command_journal_b_offset);
static_assert(end(command_journal_b_offset, command_journal_slot_size) ==
              metadata_scratch_offset);
static_assert(end(metadata_scratch_offset, metadata_scratch_size) ==
              sensor_log_offset);
static_assert(end(sensor_log_offset, sensor_log_size) ==
              shutdown_record_a_offset);
static_assert(end(shutdown_record_a_offset, shutdown_record_slot_size) ==
              shutdown_record_b_offset);
static_assert(end(shutdown_record_b_offset, shutdown_record_slot_size) ==
              total_size);

static_assert(bootloader_offset % sector_size == 0u);
static_assert(bootloader_size % sector_size == 0u);
static_assert(firmware_a_offset % sector_size == 0u);
static_assert(firmware_a_size % sector_size == 0u);
static_assert(firmware_b_offset % sector_size == 0u);
static_assert(firmware_b_size % sector_size == 0u);
static_assert(model_a_offset % sector_size == 0u);
static_assert(model_a_size % sector_size == 0u);
static_assert(model_b_offset % sector_size == 0u);
static_assert(model_b_size % sector_size == 0u);
static_assert(audio_offset % sector_size == 0u);
static_assert(audio_size % sector_size == 0u);
static_assert(service_offset % sector_size == 0u);
static_assert(service_size % sector_size == 0u);
static_assert(audio_slot_0_offset % sector_size == 0u);
static_assert(audio_slot_1_offset % sector_size == 0u);
static_assert(audio_slot_2_offset % sector_size == 0u);
static_assert(audio_slot_3_offset % sector_size == 0u);
static_assert(audio_slot_4_offset % sector_size == 0u);
static_assert(audio_slot_size % sector_size == 0u);
static_assert(model_metadata_a_offset % sector_size == 0u);
static_assert(model_metadata_b_offset % sector_size == 0u);
static_assert(model_metadata_slot_size % sector_size == 0u);
static_assert(boot_metadata_a_offset % sector_size == 0u);
static_assert(boot_metadata_b_offset % sector_size == 0u);
static_assert(boot_metadata_slot_size % sector_size == 0u);
static_assert(command_journal_a_offset % sector_size == 0u);
static_assert(command_journal_b_offset % sector_size == 0u);
static_assert(command_journal_slot_size % sector_size == 0u);
static_assert(metadata_scratch_offset % sector_size == 0u);
static_assert(metadata_scratch_size % sector_size == 0u);
static_assert(sensor_log_offset % sector_size == 0u);
static_assert(sensor_log_size % sector_size == 0u);
static_assert(shutdown_record_a_offset % sector_size == 0u);
static_assert(shutdown_record_b_offset % sector_size == 0u);
static_assert(shutdown_record_slot_size % sector_size == 0u);
static_assert(total_size % sector_size == 0u);
static_assert(sector_size % page_size == 0u);

static_assert(sensor_log_offset == 0x3F0000u);
static_assert(shutdown_record_a_offset == 0x3FE000u);
static_assert(shutdown_record_b_offset == 0x3FF000u);

} // namespace boot_v2::flash_partition
