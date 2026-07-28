#include "power_state_runtime.hpp"

#include <atomic>

namespace boot_v2 {
namespace {

std::atomic<bool> g_battery_grace_active{false};

} // namespace

void power_state_set_battery_grace(const bool active) noexcept
{
    g_battery_grace_active.store(active, std::memory_order_release);
}

bool power_state_battery_grace_active() noexcept
{
    return g_battery_grace_active.load(std::memory_order_acquire);
}

} // namespace boot_v2
