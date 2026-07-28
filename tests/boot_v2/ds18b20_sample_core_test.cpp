#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "../../src/boot_v2/ds18b20_sample_core.hpp"

namespace {

std::uint64_t checks = 0;

#define CHECK(expression)                                                       \
    do {                                                                        \
        ++checks;                                                               \
        if (!(expression)) {                                                    \
            std::cerr << "CHECK failed: " #expression << " at " << __FILE__   \
                      << ':' << __LINE__ << '\n';                                \
            std::exit(1);                                                       \
        }                                                                       \
    } while (false)

using boot_v2::Ds18b20RawSampleStatus;

void check_power_on_sentinel_is_rejected()
{
    const auto decision = boot_v2::decode_ds18b20_raw_sample(0x0550);
    CHECK(decision.status == Ds18b20RawSampleStatus::PowerOnSentinel);
    CHECK(decision.temperature_celsius == 0.0f);
}

void check_neighbouring_real_values_are_preserved()
{
    const auto lower = boot_v2::decode_ds18b20_raw_sample(0x054f);
    CHECK(lower.status == Ds18b20RawSampleStatus::Valid);
    CHECK(lower.temperature_celsius == 84.9375f);

    const auto upper = boot_v2::decode_ds18b20_raw_sample(0x0551);
    CHECK(upper.status == Ds18b20RawSampleStatus::Valid);
    CHECK(upper.temperature_celsius == 85.0625f);
}

void check_datasheet_range_boundaries()
{
    const auto minimum = boot_v2::decode_ds18b20_raw_sample(-880);
    CHECK(minimum.status == Ds18b20RawSampleStatus::Valid);
    CHECK(minimum.temperature_celsius == -55.0f);

    const auto below = boot_v2::decode_ds18b20_raw_sample(-881);
    CHECK(below.status == Ds18b20RawSampleStatus::OutOfRange);

    const auto maximum = boot_v2::decode_ds18b20_raw_sample(2000);
    CHECK(maximum.status == Ds18b20RawSampleStatus::Valid);
    CHECK(maximum.temperature_celsius == 125.0f);

    const auto above = boot_v2::decode_ds18b20_raw_sample(2001);
    CHECK(above.status == Ds18b20RawSampleStatus::OutOfRange);
}

} // namespace

int main()
{
    check_power_on_sentinel_is_rejected();
    check_neighbouring_real_values_are_preserved();
    check_datasheet_range_boundaries();
    std::cout << "ds18b20_sample_core_test checks=" << checks << '\n';
    return 0;
}
