#include "runtime_owner_producer_facade.hpp"

#include "runtime_owner_producer_contract.hpp"

namespace boot_v2 {
namespace runtime_owner_rtos_detail {

[[nodiscard]] RuntimeOwnerIngressResult submit_control(
    RuntimeOwnerControlMessage value) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult submit_normal(
    NormalIntent value) noexcept;
[[nodiscard]] RuntimeOwnerIngressResult submit_urgent(
    RuntimeOwnerUrgentMessage value) noexcept;

} // namespace runtime_owner_rtos_detail
namespace {

bool exact_control_authorized(
    const RuntimeOwnerProducerKind producer) noexcept
{
    const RuntimeOwnerProducerDecision decision =
        runtime_owner_authorize_producer_request(
            producer,
            RuntimeOwnerProducerRequestKind::RequestTransportAttempt);
    return decision.result ==
               RuntimeOwnerProducerAuthorizationResult::Authorized &&
           decision.lane == RuntimeOwnerRtosLane::Control &&
           decision.control_kind ==
               RuntimeOwnerControlKind::RequestTransportAttempt &&
           decision.urgent_source == RuntimeOwnerUrgentSource::Invalid &&
           decision.normal_kind == NormalIntentKind::Invalid;
}

RuntimeOwnerIngressResult submit_transport(
    const RuntimeOwnerProducerKind producer) noexcept
{
    if (!exact_control_authorized(producer)) {
        return RuntimeOwnerIngressResult::RejectedInvalid;
    }
    return runtime_owner_rtos_detail::submit_control({
        RuntimeOwnerControlKind::RequestTransportAttempt, {}});
}

RuntimeOwnerIngressResult submit_periodic_normal(
    const RuntimeOwnerProducerRequestKind request,
    const NormalIntent intent) noexcept
{
    const RuntimeOwnerProducerDecision decision =
        runtime_owner_authorize_producer_request(
            RuntimeOwnerProducerKind::Periodic, request);
    if (decision.result !=
            RuntimeOwnerProducerAuthorizationResult::Authorized ||
        decision.lane != RuntimeOwnerRtosLane::Normal ||
        decision.normal_kind != intent.kind ||
        decision.urgent_source != RuntimeOwnerUrgentSource::Invalid ||
        decision.control_kind != RuntimeOwnerControlKind::Invalid ||
        !runtime_owner_normal_intent_is_canonical(intent)) {
        return RuntimeOwnerIngressResult::RejectedInvalid;
    }
    return runtime_owner_rtos_detail::submit_normal(intent);
}

RuntimeOwnerIngressResult submit_shutdown(
    const RuntimeOwnerProducerKind producer,
    const RuntimeOwnerUrgentSource expected_source,
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    if (producer_sequence == 0 || incident_correlation_id == 0) {
        return RuntimeOwnerIngressResult::RejectedInvalid;
    }
    const RuntimeOwnerProducerDecision decision =
        runtime_owner_authorize_producer_request(
            producer, RuntimeOwnerProducerRequestKind::RequestShutdown);
    if (decision.result !=
            RuntimeOwnerProducerAuthorizationResult::Authorized ||
        decision.lane != RuntimeOwnerRtosLane::Urgent ||
        decision.urgent_source != expected_source ||
        decision.control_kind != RuntimeOwnerControlKind::Invalid ||
        decision.normal_kind != NormalIntentKind::Invalid) {
        return RuntimeOwnerIngressResult::RejectedInvalid;
    }
    return runtime_owner_rtos_detail::submit_urgent({
        expected_source, {}, producer_sequence, incident_correlation_id});
}

} // namespace

RuntimeOwnerIngressResult runtime_owner_boot_request_transport() noexcept
{
    return submit_transport(RuntimeOwnerProducerKind::Boot);
}

RuntimeOwnerIngressResult runtime_owner_periodic_request_transport() noexcept
{
    return submit_transport(RuntimeOwnerProducerKind::Periodic);
}

RuntimeOwnerIngressResult runtime_owner_periodic_publish_telemetry(
    const std::uint32_t sensor_id,
    const std::uint32_t snapshot_revision) noexcept
{
    return submit_periodic_normal(
        RuntimeOwnerProducerRequestKind::PublishTelemetry,
        {NormalIntentKind::PublishTelemetry, 0, 0, sensor_id,
         snapshot_revision});
}

RuntimeOwnerIngressResult runtime_owner_periodic_refresh_rssi() noexcept
{
    return submit_periodic_normal(
        RuntimeOwnerProducerRequestKind::RefreshRssi,
        {NormalIntentKind::RefreshRssi, 0, 0, 0, 0});
}

RuntimeOwnerIngressResult runtime_owner_periodic_pull_config() noexcept
{
    return submit_periodic_normal(
        RuntimeOwnerProducerRequestKind::PullConfig,
        {NormalIntentKind::PullConfig, 0, 0, 0, 0});
}

RuntimeOwnerIngressResult runtime_owner_periodic_pull_command() noexcept
{
    return submit_periodic_normal(
        RuntimeOwnerProducerRequestKind::PullCommand,
        {NormalIntentKind::PullCommand, 0, 0, 0, 0});
}

RuntimeOwnerIngressResult runtime_owner_power_button_request_shutdown(
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    return submit_shutdown(
        RuntimeOwnerProducerKind::PowerButton,
        RuntimeOwnerUrgentSource::PowerButton,
        producer_sequence,
        incident_correlation_id);
}

RuntimeOwnerIngressResult runtime_owner_adapter_loss_request_shutdown(
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    return submit_shutdown(
        RuntimeOwnerProducerKind::AdapterMonitor,
        RuntimeOwnerUrgentSource::AdapterLossCommitted,
        producer_sequence,
        incident_correlation_id);
}

RuntimeOwnerIngressResult runtime_owner_authenticated_request_shutdown(
    const std::uint32_t producer_sequence,
    const std::uint32_t incident_correlation_id) noexcept
{
    return submit_shutdown(
        RuntimeOwnerProducerKind::AuthenticatedCommand,
        RuntimeOwnerUrgentSource::AuthenticatedRemoteCommand,
        producer_sequence,
        incident_correlation_id);
}

} // namespace boot_v2
