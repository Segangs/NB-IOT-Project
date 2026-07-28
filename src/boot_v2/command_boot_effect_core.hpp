#ifndef NB_IOT_BOOT_V2_COMMAND_BOOT_EFFECT_CORE_HPP
#define NB_IOT_BOOT_V2_COMMAND_BOOT_EFFECT_CORE_HPP

#include <cstdint>

#include "command_ack_core.hpp"
#include "runtime_owner_shutdown_record_core.hpp"

namespace boot_v2 {

constexpr std::uint32_t COMMAND_WATCHDOG_SCRATCH_MAGIC = 0x12345678u;

struct CommandBootEffectEvidence {
    std::uint8_t shutdown_record_present{0};
    std::uint8_t watchdog_marker_present{0};
    std::uint8_t reserved[2]{};
    std::uint32_t watchdog_cmd_id{0};
    RuntimeOwnerShutdownRecordV1 shutdown_record{};
};

[[nodiscard]] bool command_boot_effect_matches(
    const CommandJournalRecord &journal,
    std::uint32_t current_boot_sequence,
    const CommandBootEffectEvidence &evidence) noexcept;

} // namespace boot_v2

#endif // NB_IOT_BOOT_V2_COMMAND_BOOT_EFFECT_CORE_HPP
