#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_SHUTDOWN_RECORD_CORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_SHUTDOWN_RECORD_CORE_HPP

#include <cstdint>
#include <type_traits>

namespace boot_v2 {

constexpr std::uint32_t RUNTIME_OWNER_SHUTDOWN_RECORD_MAGIC = 0x53484431u;
constexpr std::uint16_t RUNTIME_OWNER_SHUTDOWN_RECORD_VERSION = 1u;

enum class RuntimeOwnerShutdownPlannedAction : std::uint8_t {
    Invalid = 0,
    WatchdogReboot = 1,
    Gp15Kill = 2,
};

struct RuntimeOwnerShutdownRecordInput {
    std::uint32_t producer_sequence{0};
    std::uint32_t incident_correlation_id{0};
    std::uint32_t elapsed_ms{0};
    std::uint8_t reason{0};
    std::uint8_t initial_usb_present{0};
    RuntimeOwnerShutdownPlannedAction planned_action{
        RuntimeOwnerShutdownPlannedAction::Invalid};
    std::uint8_t hard_deadline_reached{0};
    std::uint8_t cleanup_succeeded_mask{0};
    std::uint8_t cleanup_failed_mask{0};
    std::uint8_t cleanup_timed_out_mask{0};
    std::uint8_t cleanup_skipped_mask{0};
    std::uint8_t hardware_reset_count{0};
};

struct alignas(32) RuntimeOwnerShutdownRecordV1 {
    std::uint32_t magic{0};
    std::uint16_t version{0};
    std::uint16_t size{0};
    std::uint32_t sequence{0};
    std::uint32_t producer_sequence{0};
    std::uint32_t incident_correlation_id{0};
    std::uint32_t elapsed_ms{0};
    std::uint8_t reason{0};
    std::uint8_t initial_usb_present{0};
    RuntimeOwnerShutdownPlannedAction planned_action{
        RuntimeOwnerShutdownPlannedAction::Invalid};
    std::uint8_t hard_deadline_reached{0};
    std::uint8_t cleanup_succeeded_mask{0};
    std::uint8_t cleanup_failed_mask{0};
    std::uint8_t cleanup_timed_out_mask{0};
    std::uint8_t cleanup_skipped_mask{0};
    std::uint8_t hardware_reset_count{0};
    std::uint8_t reserved[27]{};
    std::uint32_t crc32{0};
};

static_assert(sizeof(RuntimeOwnerShutdownRecordV1) == 64);
static_assert(alignof(RuntimeOwnerShutdownRecordV1) == 32);
static_assert(
    std::is_standard_layout<RuntimeOwnerShutdownRecordV1>::value);
static_assert(
    std::is_trivially_copyable<RuntimeOwnerShutdownRecordV1>::value);

[[nodiscard]] std::uint32_t runtime_owner_shutdown_record_crc(
    const RuntimeOwnerShutdownRecordV1 &record) noexcept;

[[nodiscard]] RuntimeOwnerShutdownRecordV1
runtime_owner_shutdown_record_make(
    RuntimeOwnerShutdownRecordInput input,
    std::uint32_t sequence) noexcept;

[[nodiscard]] bool runtime_owner_shutdown_record_valid(
    const RuntimeOwnerShutdownRecordV1 &record) noexcept;

[[nodiscard]] const RuntimeOwnerShutdownRecordV1 *
runtime_owner_shutdown_record_select_latest(
    const RuntimeOwnerShutdownRecordV1 &slot_a,
    const RuntimeOwnerShutdownRecordV1 &slot_b) noexcept;

} // namespace boot_v2

#endif
