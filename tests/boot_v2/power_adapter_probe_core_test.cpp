#include "power_adapter_probe_core.hpp"

#include <cstddef>
#include <cstdio>

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;

void check(const bool condition, const char *expression, const int line) noexcept
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using boot_v2::PowerAdapterProbeKind;
using boot_v2::PowerAdapterDiagnosticSnapshot;
using boot_v2::classify_power_adapter_probe;
using boot_v2::clamp_power_adapter_recovery_us;
using boot_v2::encode_power_adapter_diagnostic_flags;
using boot_v2::update_power_adapter_non_strong_latch;

void test_low_floating_input_is_low() noexcept
{
    CHECK(classify_power_adapter_probe(false, false) ==
          PowerAdapterProbeKind::Low);
    CHECK(classify_power_adapter_probe(false, true) ==
          PowerAdapterProbeKind::Low);
}

void test_pull_down_distinguishes_weak_and_strong_high() noexcept
{
    CHECK(classify_power_adapter_probe(true, false) ==
          PowerAdapterProbeKind::Weak);
    CHECK(classify_power_adapter_probe(true, true) ==
          PowerAdapterProbeKind::Strong);
}

void test_non_strong_sample_stays_latched_after_strong_power_returns() noexcept
{
    bool non_strong_seen = false;
    non_strong_seen = update_power_adapter_non_strong_latch(
        non_strong_seen, PowerAdapterProbeKind::Strong);
    CHECK(!non_strong_seen);

    non_strong_seen = update_power_adapter_non_strong_latch(
        non_strong_seen, PowerAdapterProbeKind::Weak);
    CHECK(non_strong_seen);

    non_strong_seen = update_power_adapter_non_strong_latch(
        non_strong_seen, PowerAdapterProbeKind::Strong);
    CHECK(non_strong_seen);

    CHECK(update_power_adapter_non_strong_latch(
        false, PowerAdapterProbeKind::Low));
}

void test_diagnostic_flags_preserve_observed_hardware_states() noexcept
{
    const PowerAdapterDiagnosticSnapshot snapshot{
        3,
        2,
        47,
        true,
        true,
        false,
        true,
    };

    CHECK(encode_power_adapter_diagnostic_flags(snapshot) == 0x0BU);
}

void test_recovery_time_is_clamped_to_flash_field_range() noexcept
{
    CHECK(clamp_power_adapter_recovery_us(0) == 0);
    CHECK(clamp_power_adapter_recovery_us(32767) == 32767);
    CHECK(clamp_power_adapter_recovery_us(32768) == 32767);
    CHECK(clamp_power_adapter_recovery_us(1000000) == 32767);
}

} // namespace

int main()
{
    test_low_floating_input_is_low();
    test_pull_down_distinguishes_weak_and_strong_high();
    test_non_strong_sample_stays_latched_after_strong_power_returns();
    test_diagnostic_flags_preserve_observed_hardware_states();
    test_recovery_time_is_clamped_to_flash_field_range();

    if (g_failures != 0) {
        std::fprintf(stderr,
                     "power_adapter_probe_core_test: %zu/%zu failed\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("power_adapter_probe_core_test: %zu checks passed\n",
                g_checks);
    return 0;
}
