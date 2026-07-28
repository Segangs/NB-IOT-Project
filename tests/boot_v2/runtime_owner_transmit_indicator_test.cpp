#include "runtime_owner_transmit_indicator.hpp"

#include <cstdio>

namespace {

int checks = 0;
int failures = 0;

void check(const bool condition, const char *expression, const int line)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using boot_v2::RuntimeOwnerDeviceOperationKind;
using boot_v2::RuntimeOwnerTransmitIndicatorScope;
using boot_v2::runtime_owner_operation_uses_transmit_indicator;

void test_mqtt_operations_use_transmit_indicator()
{
    CHECK(!runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::OpenTransport));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::PublishProbe));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::VerifySubscription));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::PullFollowupConfig));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::EnterRecovery));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::PublishTelemetry));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::PullConfig));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::PullCommand));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::PublishAdapterRemoved));
    CHECK(runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::PublishAdapterRestored));
}

void test_non_mqtt_operations_leave_transmit_indicator_off()
{
    CHECK(!runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::Invalid));
    CHECK(!runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::ProbeAt));
    CHECK(!runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::FreezeBootSnapshot));
    CHECK(!runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::EndBootOrchestration));
    CHECK(!runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::RecordFault));
    CHECK(!runtime_owner_operation_uses_transmit_indicator(
        RuntimeOwnerDeviceOperationKind::RefreshRssi));
}

bool observe_active_then_return(volatile bool &indicator)
{
    RuntimeOwnerTransmitIndicatorScope scope(indicator, true);
    return indicator;
}

void test_scope_turns_indicator_on_and_restores_it_after_return()
{
    volatile bool indicator = false;
    CHECK(observe_active_then_return(indicator));
    CHECK(!indicator);
}

void test_disabled_scope_preserves_indicator()
{
    volatile bool indicator = false;
    {
        RuntimeOwnerTransmitIndicatorScope scope(indicator, false);
        CHECK(!indicator);
    }
    CHECK(!indicator);
}

void test_active_scope_restores_preexisting_true_indicator()
{
    volatile bool indicator = true;
    {
        RuntimeOwnerTransmitIndicatorScope scope(indicator, true);
        CHECK(indicator);
    }
    CHECK(indicator);
}

} // namespace

int main()
{
    test_mqtt_operations_use_transmit_indicator();
    test_non_mqtt_operations_leave_transmit_indicator_off();
    test_scope_turns_indicator_on_and_restores_it_after_return();
    test_disabled_scope_preserves_indicator();
    test_active_scope_restores_preexisting_true_indicator();
    if (failures != 0) {
        std::printf(
            "runtime_owner_transmit_indicator_test: %d/%d failed\n",
            failures,
            checks);
        return 1;
    }
    std::printf(
        "runtime_owner_transmit_indicator_test: %d checks passed\n",
        checks);
    return 0;
}
