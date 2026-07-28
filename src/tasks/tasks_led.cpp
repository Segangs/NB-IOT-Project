#include "tasks_led.hpp"

#include <atomic>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "../boot_v2/power_adapter_probe_core.hpp"
#include "../boot_v2/power_state_core.hpp"
#include "../boot_v2/power_state_runtime.hpp"
#include "../boot_v2/runtime_owner_producer_facade.hpp"
#include "../boot_v2/runtime_owner_rtos.hpp"
#include "../boot_v2/status_led_policy.hpp"
#include "../config.h"
#include "../lib/flash_logger.hpp"
#include "../lib/log.hpp"
#include "app_context.hpp"

namespace {

constexpr std::uint32_t kPowerAdapterProbePullDownUs = 200;
constexpr std::uint32_t kPowerAdapterProbeDischargeUs = 2000;
constexpr std::uint32_t kPowerAdapterProbeRecoveryLimitUs = 2000;
constexpr std::uint32_t kPowerAdapterDiagnosticPersistMs = 30000;
constexpr std::uint8_t kPowerAdapterDiagnosticMaxRecords = 16;
constexpr std::uint32_t kModemTxVisibleHoldMs = 100;
constexpr std::uint32_t kPowerAdapterEdgeMask =
    GPIO_IRQ_EDGE_FALL;

std::atomic<std::uint32_t> g_power_adapter_falling_edges{0};
std::atomic<std::uint32_t> g_power_adapter_rising_edges{0};
std::atomic<std::uint32_t> g_modem_tx_falling_edges{0};
std::atomic<bool> g_power_adapter_probe_active{false};

void status_input_gpio_irq(
    const uint gpio,
    const std::uint32_t events) noexcept
{
    if (gpio == MODEM_TXON_INPUT_PIN) {
        if ((events & GPIO_IRQ_EDGE_FALL) != 0) {
            g_modem_tx_falling_edges.fetch_add(
                1, std::memory_order_relaxed);
        }
        return;
    }
    if (gpio != POWER_ADAPTER_DETECT_PIN ||
        g_power_adapter_probe_active.load(std::memory_order_relaxed)) {
        return;
    }
    if ((events & GPIO_IRQ_EDGE_FALL) != 0) {
        g_power_adapter_falling_edges.fetch_add(
            1, std::memory_order_relaxed);
    }
    if ((events & GPIO_IRQ_EDGE_RISE) != 0) {
        g_power_adapter_rising_edges.fetch_add(
            1, std::memory_order_relaxed);
    }
}

struct PowerAdapterProbeSample {
    bool floating_high{false};
    bool loaded_high{false};
    bool recovered_high{false};
    std::uint32_t recovery_us{0};
    boot_v2::PowerAdapterProbeKind kind{
        boot_v2::PowerAdapterProbeKind::Low};
};

PowerAdapterProbeSample probe_power_adapter_input() noexcept
{
    gpio_disable_pulls(POWER_ADAPTER_DETECT_PIN);
    const bool floating_high =
        gpio_get(POWER_ADAPTER_DETECT_PIN) != 0;

    gpio_pull_down(POWER_ADAPTER_DETECT_PIN);
    busy_wait_us_32(kPowerAdapterProbePullDownUs);
    const bool loaded_high =
        gpio_get(POWER_ADAPTER_DETECT_PIN) != 0;
    gpio_disable_pulls(POWER_ADAPTER_DETECT_PIN);

    g_power_adapter_probe_active.store(true, std::memory_order_release);
    gpio_set_irq_enabled(
        POWER_ADAPTER_DETECT_PIN, kPowerAdapterEdgeMask, false);
    gpio_set_dir(POWER_ADAPTER_DETECT_PIN, GPIO_OUT);
    gpio_put(POWER_ADAPTER_DETECT_PIN, 0);
    busy_wait_us_32(kPowerAdapterProbeDischargeUs);
    gpio_set_dir(POWER_ADAPTER_DETECT_PIN, GPIO_IN);

    const std::uint32_t recovery_started_us = time_us_32();
    bool recovered_high =
        gpio_get(POWER_ADAPTER_DETECT_PIN) != 0;
    while (!recovered_high &&
           static_cast<std::uint32_t>(
               time_us_32() - recovery_started_us) <
               kPowerAdapterProbeRecoveryLimitUs) {
        recovered_high =
            gpio_get(POWER_ADAPTER_DETECT_PIN) != 0;
    }
    const std::uint32_t recovery_us = recovered_high
        ? static_cast<std::uint32_t>(
              time_us_32() - recovery_started_us)
        : kPowerAdapterProbeRecoveryLimitUs;
    gpio_disable_pulls(POWER_ADAPTER_DETECT_PIN);
    gpio_acknowledge_irq(
        POWER_ADAPTER_DETECT_PIN, kPowerAdapterEdgeMask);
    gpio_set_irq_enabled(
        POWER_ADAPTER_DETECT_PIN, kPowerAdapterEdgeMask, true);
    g_power_adapter_probe_active.store(false, std::memory_order_release);

    return {
        floating_high,
        loaded_high,
        recovered_high,
        recovery_us,
        boot_v2::classify_power_adapter_probe(
            floating_high, loaded_high),
    };
}

boot_v2::RuntimeOwnerIngressResult submit_power_state_action(
    const boot_v2::PowerStateDecision decision) noexcept
{
    switch (decision.action) {
    case boot_v2::PowerStateAction::PublishAdapterRemoved:
        return boot_v2::runtime_owner_power_publish_adapter_removed(
            decision.incident_id, decision.sequence);
    case boot_v2::PowerStateAction::PublishAdapterRestored:
        return boot_v2::runtime_owner_power_publish_adapter_restored(
            decision.incident_id, decision.sequence);
    case boot_v2::PowerStateAction::CommitShutdown:
        return boot_v2::runtime_owner_adapter_loss_request_shutdown(
            decision.sequence, decision.incident_id);
    case boot_v2::PowerStateAction::None:
    default:
        return boot_v2::RuntimeOwnerIngressResult::RejectedInvalid;
    }
}

} // namespace

// ====================================================================================
// Core 0 Task: PCB Status LED Controller
// ====================================================================================
void vStatusLedTask(void *pvParameters)
{
    const uint rj45_led_pins[] = {
        RJ45_PORT1_TEMP_LED_PIN,
        RJ45_PORT1_MIC_LED_PIN,
        RJ45_PORT2_TEMP_LED_PIN,
        RJ45_PORT2_MIC_LED_PIN
    };

    gpio_init(STATUS_LED_RED_PIN);
    gpio_set_dir(STATUS_LED_RED_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_RED_PIN, 0);

    gpio_init(STATUS_LED_GREEN_PIN);
    gpio_set_dir(STATUS_LED_GREEN_PIN, GPIO_OUT);
    gpio_put(STATUS_LED_GREEN_PIN, 0);

    gpio_init(TXON_LED_PIN);
    gpio_set_dir(TXON_LED_PIN, GPIO_OUT);
    gpio_put(TXON_LED_PIN, 1);

    for (uint pin : rj45_led_pins)
    {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }

    gpio_init(MODEM_TXON_INPUT_PIN);
    gpio_set_dir(MODEM_TXON_INPUT_PIN, GPIO_IN);
    gpio_pull_up(MODEM_TXON_INPUT_PIN);

    gpio_init(POWER_ADAPTER_DETECT_PIN);
    gpio_set_dir(POWER_ADAPTER_DETECT_PIN, GPIO_IN);
    gpio_disable_pulls(POWER_ADAPTER_DETECT_PIN);
    gpio_set_irq_enabled_with_callback(
        POWER_ADAPTER_DETECT_PIN,
        kPowerAdapterEdgeMask,
        true,
        &status_input_gpio_irq);
    gpio_set_irq_enabled(
        MODEM_TXON_INPUT_PIN,
        GPIO_IRQ_EDGE_FALL,
        true);
    flash_log_dump_power_adapter_probes();

    gpio_init(POWER_INT_PIN);
    gpio_set_dir(POWER_INT_PIN, GPIO_IN);
    gpio_pull_up(POWER_INT_PIN);

    const TickType_t sensor_blink_time = pdMS_TO_TICKS(120);
    uint32_t last_ch0_seq = g_temp_ch0_sample_seq;
    uint32_t last_ch1_seq = g_temp_ch1_sample_seq;
    TickType_t ch0_blink_until = 0;
    TickType_t ch1_blink_until = 0;
    TickType_t modem_tx_hold_until = 0;
    std::uint32_t last_modem_tx_falling_edges =
        g_modem_tx_falling_edges.load(std::memory_order_acquire);
    const TickType_t adapter_probe_interval = pdMS_TO_TICKS(1000);
    TickType_t last_adapter_probe = xTaskGetTickCount();
    TickType_t last_adapter_diagnostic_persist = last_adapter_probe;
    PowerAdapterProbeSample last_adapter_probe_sample{};
    bool adapter_probe_sample_valid = false;
    bool adapter_non_strong_seen = false;
    bool adapter_diagnostic_baseline_written = false;
    std::uint8_t adapter_diagnostic_records_written = 0;
    std::uint32_t persisted_falling_edges = 0;
    std::uint32_t persisted_rising_edges = 0;
    bool power_int_low_tracking = false;
    bool power_shutdown_latched = false;
    TickType_t power_int_low_since = 0;
    uint32_t power_producer_sequence = 0;
    uint32_t power_incident_correlation_id = 0;
    boot_v2::PowerStateCore adapter_power_state({
        POWER_ADAPTER_DEBOUNCE_MS,
        POWER_ADAPTER_SHUTDOWN_COMMIT_MS,
        POWER_ADAPTER_ABSOLUTE_OFF_MS,
    });
    boot_v2::PowerStateKind last_adapter_power_state =
        boot_v2::PowerStateKind::ExternalPower;

    while (true)
    {
        TickType_t now = xTaskGetTickCount();
        const std::uint32_t now_ms =
            static_cast<std::uint32_t>(now * portTICK_PERIOD_MS);
        bool adapter_present = gpio_get(POWER_ADAPTER_DETECT_PIN) == POWER_ADAPTER_PRESENT_LEVEL;
        if (now - last_adapter_probe >= adapter_probe_interval) {
            const PowerAdapterProbeSample probe =
                probe_power_adapter_input();
            adapter_present = probe.floating_high;
            adapter_non_strong_seen =
                boot_v2::update_power_adapter_non_strong_latch(
                    adapter_non_strong_seen, probe.kind);
            last_adapter_probe_sample = probe;
            adapter_probe_sample_valid = true;
            last_adapter_probe = now;
        }
        const bool runtime_ready =
            boot_v2::runtime_owner_redacted_status().runtime_ready != 0;
        const bool external_power_present = adapter_present || boot_v2::runtime_owner_usb_power_present();
        const bool power_state_present =
            !runtime_ready || external_power_present;
        const boot_v2::PowerStateDecision adapter_decision =
            adapter_power_state.observe(
                power_state_present,
                now_ms);
        boot_v2::power_state_set_battery_grace(
            adapter_decision.battery_grace_active != 0);
        lcd_params.is_battery_mode =
            adapter_decision.battery_grace_active != 0;

        if (adapter_decision.state != last_adapter_power_state) {
            LOG("POWER_ADAPTER_STATE %u PRESENT=%u INCIDENT=%lu\n",
                static_cast<unsigned int>(adapter_decision.state),
                power_state_present ? 1U : 0U,
                static_cast<unsigned long>(adapter_decision.incident_id));
            last_adapter_power_state = adapter_decision.state;
        }

        const std::uint32_t falling_edges =
            g_power_adapter_falling_edges.load(std::memory_order_acquire);
        const std::uint32_t rising_edges =
            g_power_adapter_rising_edges.load(std::memory_order_acquire);
        const bool edge_changed =
            falling_edges != persisted_falling_edges ||
            rising_edges != persisted_rising_edges;
        const bool periodic_persist_due =
            now - last_adapter_diagnostic_persist >=
                pdMS_TO_TICKS(kPowerAdapterDiagnosticPersistMs);
        if (runtime_ready && adapter_probe_sample_valid &&
            adapter_diagnostic_records_written <
                kPowerAdapterDiagnosticMaxRecords &&
            (!adapter_diagnostic_baseline_written ||
             edge_changed || periodic_persist_due)) {
            const boot_v2::PowerAdapterDiagnosticSnapshot snapshot{
                falling_edges,
                rising_edges,
                last_adapter_probe_sample.recovery_us,
                gpio_get(POWER_ADAPTER_DETECT_PIN) != 0,
                last_adapter_probe_sample.floating_high,
                last_adapter_probe_sample.loaded_high,
                last_adapter_probe_sample.recovered_high,
            };
            const std::uint8_t flags =
                boot_v2::encode_power_adapter_diagnostic_flags(snapshot);
            flash_log_write_power_adapter_probe(
                falling_edges,
                rising_edges,
                flags,
                boot_v2::clamp_power_adapter_recovery_us(
                    snapshot.recovery_us));
            LOG(
                "POWER_PROBE_FLASH_WRITE FALL=%lu RISE=%lu "
                "FLAGS=0x%02X RECOVERY_US=%lu\n",
                static_cast<unsigned long>(falling_edges),
                static_cast<unsigned long>(rising_edges),
                flags,
                static_cast<unsigned long>(snapshot.recovery_us));
            persisted_falling_edges = falling_edges;
            persisted_rising_edges = rising_edges;
            adapter_diagnostic_baseline_written = true;
            adapter_diagnostic_records_written++;
            last_adapter_diagnostic_persist = now;
        }

        if (boot_v2::power_state_action_submit_allowed(
                adapter_decision.action, runtime_ready)) {
            const boot_v2::RuntimeOwnerIngressResult result =
                submit_power_state_action(adapter_decision);
            if (result ==
                boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery) {
                (void)adapter_power_state.acknowledge(
                    adapter_decision.action);
                LOG("POWER_ADAPTER_ACTION %u INCIDENT=%lu SEQ=%lu\n",
                    static_cast<unsigned int>(adapter_decision.action),
                    static_cast<unsigned long>(adapter_decision.incident_id),
                    static_cast<unsigned long>(adapter_decision.sequence));
            }
        }

        if (!power_shutdown_latched) {
            const bool power_int_low = gpio_get(POWER_INT_PIN) == 0;
            if (!power_int_low) {
                power_int_low_tracking = false;
                power_producer_sequence = 0;
                power_incident_correlation_id = 0;
            } else if (!power_int_low_tracking) {
                power_int_low_tracking = true;
                power_int_low_since = now;
            } else if (
                now - power_int_low_since >=
                    pdMS_TO_TICKS(POWER_INT_DEBOUNCE_MS) &&
                boot_v2::runtime_owner_redacted_status().runtime_ready != 0) {
                if (power_producer_sequence == 0) {
                    power_producer_sequence = 1;
                    power_incident_correlation_id =
                        to_ms_since_boot(get_absolute_time());
                    if (power_incident_correlation_id == 0) {
                        power_incident_correlation_id = 1;
                    }
                }
                const boot_v2::RuntimeOwnerIngressResult result =
                    boot_v2::runtime_owner_power_button_request_shutdown(
                        power_producer_sequence,
                        power_incident_correlation_id);
                if (result ==
                    boot_v2::RuntimeOwnerIngressResult::AcceptedForDelivery) {
                    power_shutdown_latched = true;
                    LOG("POWER_BUTTON_SHUTDOWN_ACCEPTED %lu\n",
                        static_cast<unsigned long>(
                            power_incident_correlation_id));
                }
            }
        }

        const std::uint32_t modem_tx_falling_edges =
            g_modem_tx_falling_edges.load(std::memory_order_acquire);
        const bool modem_tx_level_active =
            gpio_get(MODEM_TXON_INPUT_PIN) == 0;
        if (modem_tx_level_active ||
            modem_tx_falling_edges != last_modem_tx_falling_edges) {
            last_modem_tx_falling_edges = modem_tx_falling_edges;
            modem_tx_hold_until =
                now + pdMS_TO_TICKS(kModemTxVisibleHoldMs);
        }
        const bool modem_tx_active =
            modem_tx_level_active ||
            static_cast<std::int32_t>(modem_tx_hold_until - now) > 0;

        const boot_v2::StatusLedOutputs led_outputs =
            boot_v2::status_led_outputs({
                now_ms,
                lcd_params.is_booting,
                power_shutdown_latched,
                adapter_decision.battery_grace_active != 0,
                modem_tx_active,
            });
        gpio_put(STATUS_LED_RED_PIN, led_outputs.red);
        gpio_put(STATUS_LED_GREEN_PIN, led_outputs.green);
        gpio_put(TXON_LED_PIN, led_outputs.modem_tx);

        bool port1_temp_ok = (g_temp_ch0_sample_seq > 0) && (lcd_params.status_ch0 == 0);
        bool port2_temp_ok = (g_temp_ch1_sample_seq > 0) && (g_sensor_count >= 2) && (lcd_params.status_ch1 == 0);

        if (g_temp_ch0_sample_seq != last_ch0_seq)
        {
            last_ch0_seq = g_temp_ch0_sample_seq;
            ch0_blink_until = now + sensor_blink_time;
        }
        if (g_temp_ch1_sample_seq != last_ch1_seq)
        {
            last_ch1_seq = g_temp_ch1_sample_seq;
            ch1_blink_until = now + sensor_blink_time;
        }

        gpio_put(RJ45_PORT1_TEMP_LED_PIN, (port1_temp_ok && now >= ch0_blink_until) ? 1 : 0);
        gpio_put(RJ45_PORT2_TEMP_LED_PIN, (port2_temp_ok && now >= ch1_blink_until) ? 1 : 0);
        gpio_put(RJ45_PORT1_MIC_LED_PIN, g_mic1_stream_active ? 1 : 0);
        gpio_put(RJ45_PORT2_MIC_LED_PIN, g_mic2_stream_active ? 1 : 0);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
