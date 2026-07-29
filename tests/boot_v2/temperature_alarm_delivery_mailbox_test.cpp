#include "temperature_alarm_delivery_mailbox.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <type_traits>

namespace {

std::size_t g_checks = 0;
std::size_t g_failures = 0;
std::atomic<std::size_t> g_allocations{0};
std::atomic<std::size_t> g_deallocations{0};

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

using namespace boot_v2;

constexpr TemperatureAlarmDeliveryEvent event(
    const std::uint32_t sensor_id,
    const std::uint32_t revision,
    const TemperatureAlarmTerminalResult result =
        TemperatureAlarmTerminalResult::Succeeded,
    const TemperatureAlarmEdge edge = TemperatureAlarmEdge::High) noexcept
{
    return {sensor_id, revision, result, edge, 0};
}

constexpr bool events_equal(
    const TemperatureAlarmDeliveryEvent left,
    const TemperatureAlarmDeliveryEvent right) noexcept
{
    return left.sensor_id == right.sensor_id &&
           left.snapshot_revision == right.snapshot_revision &&
           left.result == right.result && left.edge == right.edge &&
           left.reserved == right.reserved;
}

void test_event_abi_and_canonical_shape() noexcept
{
    CHECK(sizeof(TemperatureAlarmDeliveryEvent) == 12);
    CHECK(alignof(TemperatureAlarmDeliveryEvent) == 4);
    CHECK(std::is_standard_layout<TemperatureAlarmDeliveryEvent>::value);
    CHECK(std::is_trivially_copyable<TemperatureAlarmDeliveryEvent>::value);
    CHECK(offsetof(TemperatureAlarmDeliveryEvent, sensor_id) == 0);
    CHECK(offsetof(TemperatureAlarmDeliveryEvent, snapshot_revision) == 4);
    CHECK(offsetof(TemperatureAlarmDeliveryEvent, result) == 8);
    CHECK(offsetof(TemperatureAlarmDeliveryEvent, edge) == 9);
    CHECK(offsetof(TemperatureAlarmDeliveryEvent, reserved) == 10);

    CHECK(temperature_alarm_delivery_event_is_canonical(event(1, 7)));
    CHECK(temperature_alarm_delivery_event_is_canonical(event(
        2,
        8,
        TemperatureAlarmTerminalResult::Failed,
        TemperatureAlarmEdge::Clear)));
    CHECK(temperature_alarm_delivery_event_is_canonical(event(
        1,
        9,
        TemperatureAlarmTerminalResult::TimedOut,
        TemperatureAlarmEdge::High)));
    CHECK(temperature_alarm_delivery_event_is_canonical(event(
        2,
        10,
        TemperatureAlarmTerminalResult::Cancelled,
        TemperatureAlarmEdge::Clear)));

    auto malformed = event(1, 7);
    malformed.sensor_id = 0;
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
    malformed = event(1, 7);
    malformed.sensor_id = 3;
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
    malformed = event(1, 7);
    malformed.snapshot_revision = 0;
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
    malformed = event(1, 7);
    malformed.result = TemperatureAlarmTerminalResult::Invalid;
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
    malformed = event(1, 7);
    malformed.result =
        static_cast<TemperatureAlarmTerminalResult>(0xff);
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
    malformed = event(1, 7);
    malformed.edge = TemperatureAlarmEdge::Invalid;
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
    malformed = event(1, 7);
    malformed.edge = static_cast<TemperatureAlarmEdge>(0xff);
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
    malformed = event(1, 7);
    malformed.reserved = 1;
    CHECK(!temperature_alarm_delivery_event_is_canonical(malformed));
}

void test_fifo_wrap_full_duplicate_and_overflow_latch() noexcept
{
    TemperatureAlarmDeliveryMailbox mailbox{};
    CHECK(TemperatureAlarmDeliveryMailbox::capacity() == 16);
    CHECK(mailbox.view().depth == 0);

    for (std::uint32_t revision = 1; revision <= 16; ++revision) {
        CHECK(mailbox.try_push(event(1, revision)) ==
              TemperatureAlarmDeliveryPushResult::Accepted);
    }
    CHECK(mailbox.view().depth == 16);

    const TemperatureAlarmDeliveryEvent rejected = event(2, 17);
    CHECK(mailbox.try_push(rejected) ==
          TemperatureAlarmDeliveryPushResult::RejectedFull);
    auto full_view = mailbox.view();
    CHECK(full_view.depth == 16);
    CHECK(full_view.overflow_count == 1);
    CHECK(full_view.overflow_latched == 1);
    CHECK(full_view.overflow_log_pending == 1);
    CHECK(mailbox.take_overflow_log_pending());
    CHECK(!mailbox.take_overflow_log_pending());

    TemperatureAlarmDeliveryEvent output{};
    for (std::uint32_t revision = 1; revision <= 8; ++revision) {
        CHECK(mailbox.try_pop(output) ==
              TemperatureAlarmDeliveryPopResult::Popped);
        CHECK(events_equal(output, event(1, revision)));
    }
    for (std::uint32_t revision = 17; revision <= 24; ++revision) {
        CHECK(mailbox.try_push(event(2, revision)) ==
              TemperatureAlarmDeliveryPushResult::Accepted);
    }

    for (std::uint32_t revision = 9; revision <= 16; ++revision) {
        CHECK(mailbox.try_pop(output) ==
              TemperatureAlarmDeliveryPopResult::Popped);
        CHECK(events_equal(output, event(1, revision)));
    }
    for (std::uint32_t revision = 17; revision <= 24; ++revision) {
        CHECK(mailbox.try_pop(output) ==
              TemperatureAlarmDeliveryPopResult::Popped);
        CHECK(events_equal(output, event(2, revision)));
    }
    CHECK(mailbox.try_pop(output) ==
          TemperatureAlarmDeliveryPopResult::Empty);
    CHECK(mailbox.view().depth == 0);

    const auto duplicate = event(
        1,
        31,
        TemperatureAlarmTerminalResult::Failed,
        TemperatureAlarmEdge::Clear);
    CHECK(mailbox.try_push(duplicate) ==
          TemperatureAlarmDeliveryPushResult::Accepted);
    CHECK(mailbox.try_push(duplicate) ==
          TemperatureAlarmDeliveryPushResult::AcceptedDuplicate);
    CHECK(mailbox.view().depth == 1);

    CHECK(mailbox.try_push(event(
              2,
              31,
              TemperatureAlarmTerminalResult::Failed,
              TemperatureAlarmEdge::Clear)) ==
          TemperatureAlarmDeliveryPushResult::Accepted);
    CHECK(mailbox.try_push(event(
              1,
              32,
              TemperatureAlarmTerminalResult::Failed,
              TemperatureAlarmEdge::Clear)) ==
          TemperatureAlarmDeliveryPushResult::Accepted);
    CHECK(mailbox.try_push(event(
              1,
              31,
              TemperatureAlarmTerminalResult::TimedOut,
              TemperatureAlarmEdge::Clear)) ==
          TemperatureAlarmDeliveryPushResult::Accepted);
    CHECK(mailbox.try_push(event(
              1,
              31,
              TemperatureAlarmTerminalResult::Failed,
              TemperatureAlarmEdge::High)) ==
          TemperatureAlarmDeliveryPushResult::Accepted);
    CHECK(mailbox.view().depth == 5);

    while (mailbox.try_pop(output) ==
           TemperatureAlarmDeliveryPopResult::Popped) {
    }
    for (std::uint32_t revision = 100; revision < 116; ++revision) {
        CHECK(mailbox.try_push(event(1, revision)) ==
              TemperatureAlarmDeliveryPushResult::Accepted);
    }
    CHECK(mailbox.try_push(event(2, 200)) ==
          TemperatureAlarmDeliveryPushResult::RejectedFull);
    full_view = mailbox.view();
    CHECK(full_view.overflow_count == 2);
    CHECK(full_view.overflow_latched == 1);
    CHECK(full_view.overflow_log_pending == 0);
    CHECK(!mailbox.take_overflow_log_pending());
}

void test_rejected_invalid_never_mutates_mailbox() noexcept
{
    TemperatureAlarmDeliveryMailbox mailbox{};
    auto malformed = event(1, 7);
    malformed.reserved = 1;
    CHECK(mailbox.try_push(malformed) ==
          TemperatureAlarmDeliveryPushResult::RejectedInvalid);
    const auto view = mailbox.view();
    CHECK(view.depth == 0);
    CHECK(view.overflow_count == 0);
    CHECK(view.overflow_latched == 0);
    CHECK(view.overflow_log_pending == 0);
}

void test_duplicate_candidate_is_revalidated_against_latest_read() noexcept
{
    using temperature_alarm_delivery_detail::
        logical_index_is_pending;
    using temperature_alarm_delivery_detail::
        saturating_increment;

    CHECK(logical_index_is_pending(11, 11, 12));
    CHECK(!logical_index_is_pending(11, 12, 12));
    CHECK(logical_index_is_pending(
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        0));
    CHECK(!logical_index_is_pending(
        std::numeric_limits<std::uint32_t>::max(),
        0,
        0));
    CHECK(saturating_increment(
              std::numeric_limits<std::uint32_t>::max() - 1) ==
          std::numeric_limits<std::uint32_t>::max());
    CHECK(saturating_increment(
              std::numeric_limits<std::uint32_t>::max()) ==
          std::numeric_limits<std::uint32_t>::max());
}

void test_cross_thread_spsc_stress_preserves_exact_fifo() noexcept
{
    constexpr std::uint32_t kEventCount = 200000;
    TemperatureAlarmDeliveryMailbox mailbox{};
    std::atomic<bool> producer_failed{false};
    std::atomic<bool> consumer_failed{false};

    std::thread producer([&]() {
        for (std::uint32_t revision = 1; revision <= kEventCount;) {
            const auto pushed = mailbox.try_push(event(
                revision % 2 == 0 ? 2u : 1u,
                revision,
                revision % 4 == 0
                    ? TemperatureAlarmTerminalResult::Failed
                    : TemperatureAlarmTerminalResult::Succeeded,
                revision % 3 == 0 ? TemperatureAlarmEdge::Clear
                                  : TemperatureAlarmEdge::High));
            if (pushed == TemperatureAlarmDeliveryPushResult::Accepted) {
                ++revision;
            } else if (
                pushed !=
                TemperatureAlarmDeliveryPushResult::RejectedFull) {
                producer_failed.store(true, std::memory_order_relaxed);
                return;
            } else {
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&]() {
        TemperatureAlarmDeliveryEvent output{};
        for (std::uint32_t revision = 1; revision <= kEventCount;) {
            const auto popped = mailbox.try_pop(output);
            if (popped == TemperatureAlarmDeliveryPopResult::Empty) {
                std::this_thread::yield();
                continue;
            }
            const auto expected = event(
                revision % 2 == 0 ? 2u : 1u,
                revision,
                revision % 4 == 0
                    ? TemperatureAlarmTerminalResult::Failed
                    : TemperatureAlarmTerminalResult::Succeeded,
                revision % 3 == 0 ? TemperatureAlarmEdge::Clear
                                  : TemperatureAlarmEdge::High);
            if (popped != TemperatureAlarmDeliveryPopResult::Popped ||
                !events_equal(output, expected)) {
                consumer_failed.store(true, std::memory_order_relaxed);
                return;
            }
            ++revision;
        }
    });

    producer.join();
    consumer.join();
    CHECK(!producer_failed.load(std::memory_order_relaxed));
    CHECK(!consumer_failed.load(std::memory_order_relaxed));
    CHECK(mailbox.view().depth == 0);
}

void test_single_thread_operations_are_allocation_free() noexcept
{
    TemperatureAlarmDeliveryMailbox mailbox{};
    TemperatureAlarmDeliveryEvent output{};
    const std::size_t allocations_before =
        g_allocations.load(std::memory_order_relaxed);
    const std::size_t deallocations_before =
        g_deallocations.load(std::memory_order_relaxed);

    for (std::uint32_t revision = 1; revision <= 16; ++revision) {
        CHECK(mailbox.try_push(event(1, revision)) ==
              TemperatureAlarmDeliveryPushResult::Accepted);
    }
    CHECK(mailbox.try_push(event(2, 17)) ==
          TemperatureAlarmDeliveryPushResult::RejectedFull);
    for (std::uint32_t revision = 1; revision <= 16; ++revision) {
        CHECK(mailbox.try_pop(output) ==
              TemperatureAlarmDeliveryPopResult::Popped);
    }

    CHECK(g_allocations.load(std::memory_order_relaxed) ==
          allocations_before);
    CHECK(g_deallocations.load(std::memory_order_relaxed) ==
          deallocations_before);
}

void test_sensor_consumer_applies_success_retry_and_opposite_edge() noexcept
{
    TemperatureAlarmPublishCore channel0{};
    TemperatureAlarmPublishCore channel1{};
    TemperatureAlarmDeliveryConsumerCore consumer{};

    const auto high0 = channel0.observe(true, true, 500);
    const auto high1 = channel1.observe(true, true, 510);
    CHECK(channel0.mark_enqueued(high0.snapshot_revision, high0.edge));
    CHECK(channel1.mark_enqueued(high1.snapshot_revision, high1.edge));

    CHECK(consumer.apply(
        {1,
         high0.snapshot_revision,
         TemperatureAlarmTerminalResult::Succeeded,
         TemperatureAlarmEdge::High,
         0},
        channel0,
        channel1));
    CHECK(consumer.apply(
        {2,
         high1.snapshot_revision,
         TemperatureAlarmTerminalResult::Failed,
         TemperatureAlarmEdge::High,
         0},
        channel0,
        channel1));

    const auto clear0 = channel0.observe(true, false, 200);
    CHECK(clear0.publish_required == 1);
    CHECK(clear0.edge == TemperatureAlarmEdge::Clear);
    CHECK(clear0.value_deci_celsius == 200);
    CHECK(clear0.snapshot_revision != high0.snapshot_revision);

    const auto retry1 = channel1.observe(true, false, 210);
    CHECK(retry1.publish_required == 1);
    CHECK(retry1.edge == TemperatureAlarmEdge::High);
    CHECK(retry1.value_deci_celsius == 510);
    CHECK(retry1.snapshot_revision != high1.snapshot_revision);
    CHECK(channel1.mark_enqueued(
        retry1.snapshot_revision, retry1.edge));

    CHECK(!consumer.apply(
        {2,
         retry1.snapshot_revision,
         TemperatureAlarmTerminalResult::Succeeded,
         TemperatureAlarmEdge::Clear,
         0},
        channel0,
        channel1));
    CHECK(!consumer.apply(
        {1,
         high0.snapshot_revision,
         TemperatureAlarmTerminalResult::Succeeded,
         TemperatureAlarmEdge::High,
         0},
        channel0,
        channel1));
    CHECK(consumer.view().rejected_completion_count == 2);
}

void test_sensor_source_drains_before_sampling_and_observe() noexcept
{
    std::ifstream stream(
        NB_IOT_SOURCE_ROOT "/src/tasks/tasks_sensor_reader.cpp");
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    const std::size_t loop = source.find("while (true)");
    const std::size_t drain = source.find(
        "runtime_owner_try_receive_temperature_alarm_delivery", loop);
    const std::size_t sample = source.find("read_vsys_voltage", loop);
    const std::size_t observe =
        source.find("alert_publish_ch0.observe", loop);
    CHECK(stream.is_open());
    CHECK(loop != std::string::npos);
    CHECK(drain != std::string::npos);
    CHECK(sample != std::string::npos);
    CHECK(observe != std::string::npos);
    CHECK(loop < drain);
    CHECK(drain < sample);
    CHECK(drain < observe);
    CHECK(source.find("ALARM_COMPLETION_REJECT") !=
          std::string::npos);
}

} // namespace

void *operator new(const std::size_t size)
{
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void *const allocation = std::malloc(size)) {
        return allocation;
    }
    std::abort();
}

void operator delete(void *const pointer) noexcept
{
    g_deallocations.fetch_add(1, std::memory_order_relaxed);
    std::free(pointer);
}

void operator delete(void *const pointer, std::size_t) noexcept
{
    g_deallocations.fetch_add(1, std::memory_order_relaxed);
    std::free(pointer);
}

int main()
{
    test_event_abi_and_canonical_shape();
    test_fifo_wrap_full_duplicate_and_overflow_latch();
    test_rejected_invalid_never_mutates_mailbox();
    test_duplicate_candidate_is_revalidated_against_latest_read();
    test_cross_thread_spsc_stress_preserves_exact_fifo();
    test_single_thread_operations_are_allocation_free();
    test_sensor_consumer_applies_success_retry_and_opposite_edge();
    test_sensor_source_drains_before_sampling_and_observe();
    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "temperature_alarm_delivery_mailbox_test: %zu/%zu failed\n",
            g_failures,
            g_checks);
        return 1;
    }
    std::printf(
        "temperature_alarm_delivery_mailbox_test: %zu checks passed\n",
        g_checks);
    return 0;
}
