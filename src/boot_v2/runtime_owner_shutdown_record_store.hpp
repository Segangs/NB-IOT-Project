#ifndef NB_IOT_BOOT_V2_RUNTIME_OWNER_SHUTDOWN_RECORD_STORE_HPP
#define NB_IOT_BOOT_V2_RUNTIME_OWNER_SHUTDOWN_RECORD_STORE_HPP

#include <cstdint>

#include "runtime_owner_shutdown_record_core.hpp"

namespace boot_v2 {

[[nodiscard]] const RuntimeOwnerShutdownRecordV1 *
runtime_owner_shutdown_record_current() noexcept;

[[nodiscard]] bool runtime_owner_shutdown_record_commit(
    RuntimeOwnerShutdownRecordInput input,
    std::uint32_t timeout_ms) noexcept;

void runtime_owner_shutdown_record_log_current() noexcept;

} // namespace boot_v2

#endif
