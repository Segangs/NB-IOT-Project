cmake_minimum_required(VERSION 3.16)
project(nb_iot_boot_v2_host_tests LANGUAGES CXX)

enable_testing()

# The dormant RTOS surface must not expose an unscoped shutdown authority.
# Shutdown remains available only through the three provenance-carrying
# TaskCore source ports defined by the cutover contract.
file(READ
    "${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2/runtime_owner_rtos.hpp"
    RUNTIME_OWNER_RTOS_PUBLIC_SURFACE)
file(READ
    "${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2/runtime_owner_rtos.cpp"
    RUNTIME_OWNER_RTOS_IMPLEMENTATION_SURFACE)
file(READ
    "${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2/runtime_owner_task_core.hpp"
    RUNTIME_OWNER_TASK_CORE_PUBLIC_SURFACE)
foreach(FORBIDDEN_RUNTIME_OWNER_SURFACE
        "runtime_owner_try_request_shutdown"
        "RequestShutdown"
        "shutdown_result")
    if(RUNTIME_OWNER_RTOS_PUBLIC_SURFACE MATCHES
            "${FORBIDDEN_RUNTIME_OWNER_SURFACE}" OR
       RUNTIME_OWNER_RTOS_IMPLEMENTATION_SURFACE MATCHES
            "${FORBIDDEN_RUNTIME_OWNER_SURFACE}" OR
       RUNTIME_OWNER_TASK_CORE_PUBLIC_SURFACE MATCHES
            "${FORBIDDEN_RUNTIME_OWNER_SURFACE}")
        message(FATAL_ERROR
            "Forbidden generic shutdown surface: ${FORBIDDEN_RUNTIME_OWNER_SURFACE}")
    endif()
endforeach()

set(PROTECTED_LEGACY_PATHS
    "${CMAKE_CURRENT_LIST_DIR}/../../src/tasks/tasks_boot.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/tasks/tasks_periodic_modem.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/tasks/tasks_debug.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/tasks/tasks_modem.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/tasks/tasks_mqtt.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/tasks/app_context.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../main.cpp"
)
set(PROTECTED_LEGACY_SHA256
    "89a17edafde9a79f73ae43477ba73ef4f1d0abf73a66ccf02b36a6559c3b87d2"
    "c365dfb515076ae7648195488a612871e97edab1f35f617ba49c5d688a785d1d"
    "2d673bde2a73cb752d24a921c8655dcc3f6d9a954283695f99d266d2a23f8ef4"
    "b7ab5b3269e111b3cabe4147f1a5148c7c0e570db853c1dd4ce3f609b531968f"
    "e6796a1f6188b79bcf5b283394b01eff748a90202acde1185af6a6a7193eead2"
    "6244f9979958cdac7aa0fb11987c09e596ac4e89ff4091887e6a68b9f675e407"
    "079971b5dd00460744fafaac9d86a0f1796f6a46d07b29e08ae5a40eb74fb580"
)
list(LENGTH PROTECTED_LEGACY_PATHS PROTECTED_LEGACY_PATH_COUNT)
list(LENGTH PROTECTED_LEGACY_SHA256 PROTECTED_LEGACY_SHA_COUNT)
if(NOT PROTECTED_LEGACY_PATH_COUNT EQUAL PROTECTED_LEGACY_SHA_COUNT)
    message(FATAL_ERROR "Protected legacy SHA table length mismatch")
endif()
math(EXPR PROTECTED_LEGACY_LAST_INDEX
    "${PROTECTED_LEGACY_PATH_COUNT} - 1")
foreach(PROTECTED_LEGACY_INDEX RANGE 0 ${PROTECTED_LEGACY_LAST_INDEX})
    list(GET PROTECTED_LEGACY_PATHS
        ${PROTECTED_LEGACY_INDEX} PROTECTED_LEGACY_PATH)
    list(GET PROTECTED_LEGACY_SHA256
        ${PROTECTED_LEGACY_INDEX} PROTECTED_LEGACY_EXPECTED_SHA)
    file(SHA256 "${PROTECTED_LEGACY_PATH}" PROTECTED_LEGACY_ACTUAL_SHA)
    if(NOT PROTECTED_LEGACY_ACTUAL_SHA STREQUAL
            PROTECTED_LEGACY_EXPECTED_SHA)
        message(FATAL_ERROR
            "Protected legacy changed: ${PROTECTED_LEGACY_PATH}")
    endif()
endforeach()

add_executable(lifecycle_gate_test
    lifecycle_gate_test.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)

target_include_directories(lifecycle_gate_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)

target_compile_features(lifecycle_gate_test PRIVATE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lifecycle_gate_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
    )
endif()

add_test(NAME lifecycle_gate_test COMMAND lifecycle_gate_test)

add_executable(liveness_service_test
    liveness_service_test.cpp
    ../../src/boot_v2/liveness_service.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)

target_include_directories(liveness_service_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)

target_compile_features(liveness_service_test PRIVATE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(liveness_service_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(NAME liveness_service_test COMMAND liveness_service_test)

add_executable(runtime_owner_core_test
    runtime_owner_core_test.cpp
    ../../src/boot_v2/runtime_owner_core.cpp
    ../../src/boot_v2/liveness_service.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)

target_include_directories(runtime_owner_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)

target_compile_features(runtime_owner_core_test PRIVATE cxx_std_17)

target_compile_definitions(runtime_owner_core_test PRIVATE
    NB_IOT_RUNTIME_OWNER_TESTING=1
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(NAME runtime_owner_core_test COMMAND runtime_owner_core_test)

add_executable(runtime_owner_post_config_handoff_test
    runtime_owner_post_config_handoff_test.cpp
    ../../src/boot_v2/runtime_owner_task_core.cpp
    ../../src/boot_v2/runtime_owner_adapter_core.cpp
    ../../src/boot_v2/runtime_owner_executor_contract.cpp
    ../../src/boot_v2/runtime_owner_core.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
    ../../src/boot_v2/liveness_service.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)
target_include_directories(runtime_owner_post_config_handoff_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(runtime_owner_post_config_handoff_test PRIVATE cxx_std_17)
target_compile_definitions(runtime_owner_post_config_handoff_test PRIVATE
    NB_IOT_POST_CONFIG_HANDOFF_TRIAL=1
    NB_IOT_RUNTIME_OWNER_TASK_TESTING=1
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_post_config_handoff_test PRIVATE
        -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_post_config_handoff_test
    COMMAND runtime_owner_post_config_handoff_test
)

add_executable(runtime_owner_adapter_core_test
    runtime_owner_adapter_core_test.cpp
    ../../src/boot_v2/runtime_owner_adapter_core.cpp
    ../../src/boot_v2/runtime_owner_core.cpp
    ../../src/boot_v2/liveness_service.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)

target_include_directories(runtime_owner_adapter_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)

target_compile_features(runtime_owner_adapter_core_test PRIVATE cxx_std_17)

set_target_properties(runtime_owner_adapter_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)

target_compile_definitions(runtime_owner_adapter_core_test PRIVATE
    NB_IOT_RUNTIME_OWNER_ADAPTER_TESTING=1
    NB_IOT_RUNTIME_OWNER_TESTING=1
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_adapter_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(
    NAME runtime_owner_adapter_core_test
    COMMAND runtime_owner_adapter_core_test
)

add_executable(runtime_owner_task_core_test
    runtime_owner_task_core_test.cpp
    ../../src/boot_v2/runtime_owner_task_core.cpp
    ../../src/boot_v2/runtime_owner_adapter_core.cpp
    ../../src/boot_v2/runtime_owner_executor_contract.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
    ../../src/boot_v2/runtime_owner_core.cpp
    ../../src/boot_v2/liveness_service.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)

target_include_directories(runtime_owner_task_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(runtime_owner_task_core_test PRIVATE cxx_std_17)
set_target_properties(runtime_owner_task_core_test PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(runtime_owner_task_core_test PRIVATE
    NB_IOT_RUNTIME_OWNER_TASK_TESTING=1
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_task_core_test PRIVATE
        -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
    )
endif()
add_test(NAME runtime_owner_task_core_test COMMAND runtime_owner_task_core_test)

add_executable(runtime_owner_executor_contract_test
    runtime_owner_executor_contract_test.cpp
    ../../src/boot_v2/runtime_owner_executor_contract.cpp
)
target_include_directories(runtime_owner_executor_contract_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(runtime_owner_executor_contract_test PRIVATE cxx_std_17)
set_target_properties(runtime_owner_executor_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_executor_contract_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_executor_contract_test
    COMMAND runtime_owner_executor_contract_test
)

find_package(Threads REQUIRED)
add_executable(runtime_snapshot_core_test
    runtime_snapshot_core_test.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
)
target_include_directories(runtime_snapshot_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(runtime_snapshot_core_test PRIVATE cxx_std_17)
set_target_properties(runtime_snapshot_core_test PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(runtime_snapshot_core_test PRIVATE
    NB_IOT_RUNTIME_SNAPSHOT_TESTING=1
)
target_link_libraries(runtime_snapshot_core_test PRIVATE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_snapshot_core_test PRIVATE
        -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
    )
endif()
add_test(NAME runtime_snapshot_core_test COMMAND runtime_snapshot_core_test)

add_executable(runtime_owner_cutover_integration_test
    runtime_owner_cutover_integration_test.cpp
    ../../src/boot_v2/runtime_owner_task_core.cpp
    ../../src/boot_v2/runtime_owner_adapter_core.cpp
    ../../src/boot_v2/runtime_owner_executor_contract.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
    ../../src/boot_v2/runtime_owner_core.cpp
    ../../src/boot_v2/liveness_service.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)
target_include_directories(runtime_owner_cutover_integration_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_cutover_integration_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_cutover_integration_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_cutover_integration_test PRIVATE
    NB_IOT_RUNTIME_OWNER_TASK_TESTING=1
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_cutover_integration_test PRIVATE
        -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_cutover_integration_test
    COMMAND runtime_owner_cutover_integration_test
)

add_executable(runtime_owner_rtos_drain_core_test
    runtime_owner_rtos_drain_core_test.cpp
)
target_include_directories(runtime_owner_rtos_drain_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_rtos_drain_core_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_rtos_drain_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_rtos_drain_core_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_rtos_drain_core_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_rtos_drain_core_test
    COMMAND runtime_owner_rtos_drain_core_test
)

add_executable(runtime_owner_rtos_owner_loop_test
    runtime_owner_rtos_owner_loop_test.cpp
    ../../src/boot_v2/runtime_owner_task_core.cpp
    ../../src/boot_v2/runtime_owner_adapter_core.cpp
    ../../src/boot_v2/runtime_owner_executor_contract.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
    ../../src/boot_v2/runtime_owner_core.cpp
    ../../src/boot_v2/liveness_service.cpp
    ../../src/boot_v2/lifecycle_gate.cpp
)
target_include_directories(runtime_owner_rtos_owner_loop_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(runtime_owner_rtos_owner_loop_test PRIVATE cxx_std_17)
set_target_properties(runtime_owner_rtos_owner_loop_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_rtos_owner_loop_test PRIVATE
    NB_IOT_RUNTIME_OWNER_TASK_TESTING=1
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_rtos_owner_loop_test PRIVATE
        -Wall -Wextra -Werror -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_rtos_owner_loop_test
    COMMAND runtime_owner_rtos_owner_loop_test
)

add_executable(runtime_owner_producer_contract_test
    runtime_owner_producer_contract_test.cpp
)
target_include_directories(runtime_owner_producer_contract_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_producer_contract_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_producer_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_producer_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_producer_contract_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_producer_contract_test
    COMMAND runtime_owner_producer_contract_test
)

add_executable(temperature_alarm_publish_core_test
    temperature_alarm_publish_core_test.cpp
    ../../src/boot_v2/temperature_alarm_publish_core.cpp
)
target_include_directories(temperature_alarm_publish_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    temperature_alarm_publish_core_test PRIVATE cxx_std_17
)
set_target_properties(temperature_alarm_publish_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(temperature_alarm_publish_core_test PRIVATE
        -Wall -Wextra -Werror -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME temperature_alarm_publish_core_test
    COMMAND temperature_alarm_publish_core_test
)

add_executable(power_state_core_test
    power_state_core_test.cpp
    ../../src/boot_v2/power_state_core.cpp
)
target_include_directories(power_state_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(power_state_core_test PRIVATE cxx_std_17)
set_target_properties(power_state_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(power_state_core_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME power_state_core_test
    COMMAND power_state_core_test
)

add_executable(power_adapter_probe_core_test
    power_adapter_probe_core_test.cpp
)
target_include_directories(power_adapter_probe_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(power_adapter_probe_core_test PRIVATE cxx_std_17)
set_target_properties(power_adapter_probe_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(power_adapter_probe_core_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME power_adapter_probe_core_test
    COMMAND power_adapter_probe_core_test
)

add_executable(status_led_policy_test
    status_led_policy_test.cpp
)
target_include_directories(status_led_policy_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(status_led_policy_test PRIVATE cxx_std_17)
set_target_properties(status_led_policy_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(status_led_policy_test PRIVATE
        -Wall -Wextra -Werror -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME status_led_policy_test
    COMMAND status_led_policy_test
)

add_executable(lcd_status_policy_test
    lcd_status_policy_test.cpp
)
target_include_directories(lcd_status_policy_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(lcd_status_policy_test PRIVATE cxx_std_17)
set_target_properties(lcd_status_policy_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(lcd_status_policy_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lcd_status_policy_test PRIVATE
        -Wall -Wextra -Werror -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME lcd_status_policy_test
    COMMAND lcd_status_policy_test
)

add_executable(runtime_owner_transmit_indicator_test
    runtime_owner_transmit_indicator_test.cpp
)
target_include_directories(runtime_owner_transmit_indicator_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_transmit_indicator_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_transmit_indicator_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        runtime_owner_transmit_indicator_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_transmit_indicator_test
    COMMAND runtime_owner_transmit_indicator_test
)

add_executable(mqtt_power_event_codec_test
    mqtt_power_event_codec_test.cpp
    ../../src/boot_v2/mqtt_power_event_codec.cpp
)
target_include_directories(mqtt_power_event_codec_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(mqtt_power_event_codec_test PRIVATE cxx_std_17)
set_target_properties(mqtt_power_event_codec_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(mqtt_power_event_codec_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME mqtt_power_event_codec_test
    COMMAND mqtt_power_event_codec_test
)

add_executable(runtime_owner_physical_executor_test
    runtime_owner_physical_executor_test.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
)
target_include_directories(runtime_owner_physical_executor_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_physical_executor_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_physical_executor_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_physical_executor_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_physical_executor_test
    COMMAND runtime_owner_physical_executor_test
)

add_executable(runtime_owner_cutover_core_test
    runtime_owner_cutover_core_test.cpp
    ../../src/boot_v2/runtime_owner_cutover_core.cpp
)
target_include_directories(runtime_owner_cutover_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(runtime_owner_cutover_core_test PRIVATE cxx_std_17)
set_target_properties(runtime_owner_cutover_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_cutover_core_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(NAME runtime_owner_cutover_core_test COMMAND runtime_owner_cutover_core_test)

add_executable(runtime_owner_producer_facade_contract_test
    runtime_owner_producer_facade_contract_test.cpp
    ../../src/boot_v2/runtime_owner_producer_facade.cpp
    ../../src/boot_v2/power_state_runtime.cpp
)
target_include_directories(runtime_owner_producer_facade_contract_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_producer_facade_contract_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_producer_facade_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_producer_facade_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_producer_facade_contract_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_producer_facade_contract_test
    COMMAND runtime_owner_producer_facade_contract_test
)

add_executable(runtime_owner_device_backend_contract_test
    runtime_owner_device_backend_contract_test.cpp
)
target_include_directories(runtime_owner_device_backend_contract_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_device_backend_contract_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_device_backend_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_device_backend_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_device_backend_contract_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_device_backend_contract_test
    COMMAND runtime_owner_device_backend_contract_test
)

add_executable(runtime_owner_legacy_cutover_contract_test
    runtime_owner_legacy_cutover_contract_test.cpp
)
target_compile_features(
    runtime_owner_legacy_cutover_contract_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_legacy_cutover_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_legacy_cutover_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_legacy_cutover_contract_test PRIVATE
        -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_legacy_cutover_contract_test
    COMMAND runtime_owner_legacy_cutover_contract_test
)

add_executable(runtime_owner_atomic_cutover_contract_test
    runtime_owner_atomic_cutover_contract_test.cpp
)
target_include_directories(runtime_owner_atomic_cutover_contract_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_atomic_cutover_contract_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_atomic_cutover_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(runtime_owner_atomic_cutover_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_atomic_cutover_contract_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_atomic_cutover_contract_test
    COMMAND runtime_owner_atomic_cutover_contract_test
)

add_executable(lcd_runtime_contract_test
    lcd_runtime_contract_test.cpp
)
target_compile_features(lcd_runtime_contract_test PRIVATE cxx_std_17)
set_target_properties(lcd_runtime_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(lcd_runtime_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lcd_runtime_contract_test PRIVATE
        -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME lcd_runtime_contract_test
    COMMAND lcd_runtime_contract_test
)

add_executable(pcb_pinmap_contract_test
    pcb_pinmap_contract_test.cpp
)
target_compile_features(pcb_pinmap_contract_test PRIVATE cxx_std_17)
set_target_properties(pcb_pinmap_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_definitions(pcb_pinmap_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pcb_pinmap_contract_test PRIVATE
        -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME pcb_pinmap_contract_test
    COMMAND pcb_pinmap_contract_test
)

add_executable(runtime_owner_shutdown_finalizer_core_test
    runtime_owner_shutdown_finalizer_core_test.cpp
    ../../src/boot_v2/runtime_owner_shutdown_finalizer_core.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
)
target_include_directories(runtime_owner_shutdown_finalizer_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_shutdown_finalizer_core_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_shutdown_finalizer_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_shutdown_finalizer_core_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_shutdown_finalizer_core_test
    COMMAND runtime_owner_shutdown_finalizer_core_test
)

add_executable(runtime_owner_shutdown_record_core_test
    runtime_owner_shutdown_record_core_test.cpp
    ../../src/boot_v2/runtime_owner_shutdown_record_core.cpp
)
target_include_directories(runtime_owner_shutdown_record_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(
    runtime_owner_shutdown_record_core_test PRIVATE cxx_std_17
)
set_target_properties(runtime_owner_shutdown_record_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(runtime_owner_shutdown_record_core_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME runtime_owner_shutdown_record_core_test
    COMMAND runtime_owner_shutdown_record_core_test
)

add_executable(command_boot_effect_core_test
    command_boot_effect_core_test.cpp
    ../../src/boot_v2/command_boot_effect_core.cpp
    ../../src/boot_v2/runtime_owner_shutdown_record_core.cpp
)
target_include_directories(command_boot_effect_core_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/boot_v2
)
target_compile_features(command_boot_effect_core_test PRIVATE cxx_std_17)
set_target_properties(command_boot_effect_core_test PROPERTIES
    CXX_EXTENSIONS OFF
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(command_boot_effect_core_test PRIVATE
        -Wall -Wextra -Werror -Wswitch-enum -pedantic
        -fno-exceptions -fno-rtti
    )
endif()
add_test(
    NAME command_boot_effect_core_test
    COMMAND command_boot_effect_core_test
)

add_executable(sensor_quality_core_test
    sensor_quality_core_test.cpp
    ../../src/boot_v2/sensor_quality_core.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
)

target_include_directories(sensor_quality_core_test PRIVATE
    ../..
)

target_compile_features(sensor_quality_core_test PRIVATE cxx_std_17)
set_target_properties(sensor_quality_core_test PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(sensor_quality_core_test PRIVATE
    NB_IOT_RUNTIME_SNAPSHOT_TESTING=1
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sensor_quality_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
    )
endif()

add_test(NAME sensor_quality_core_test COMMAND sensor_quality_core_test)

add_executable(ds18b20_sample_core_test
    ds18b20_sample_core_test.cpp
)

target_include_directories(ds18b20_sample_core_test PRIVATE
    ../..
)

target_compile_features(ds18b20_sample_core_test PRIVATE cxx_std_17)
set_target_properties(ds18b20_sample_core_test PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ds18b20_sample_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(NAME ds18b20_sample_core_test COMMAND ds18b20_sample_core_test)

add_executable(command_ack_core_test
    command_ack_core_test.cpp
    ../../src/boot_v2/command_ack_core.cpp
    ../../src/boot_v2/mqtt_command_codec.cpp
)

target_include_directories(command_ack_core_test PRIVATE
    ../..
)

target_compile_features(command_ack_core_test PRIVATE cxx_std_17)
set_target_properties(command_ack_core_test PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(command_ack_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
    )
endif()

add_test(NAME command_ack_core_test COMMAND command_ack_core_test)

add_executable(command_journal_record_core_test
    command_journal_record_core_test.cpp
    ../../src/boot_v2/command_journal_record_core.cpp
    ../../src/boot_v2/command_ack_core.cpp
    ../../src/boot_v2/mqtt_command_codec.cpp
)

target_include_directories(command_journal_record_core_test PRIVATE
    ../../src/boot_v2
)

target_compile_features(
    command_journal_record_core_test PRIVATE cxx_std_17
)
set_target_properties(
    command_journal_record_core_test PROPERTIES CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(command_journal_record_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
    )
endif()

add_test(
    NAME command_journal_record_core_test
    COMMAND command_journal_record_core_test
)

add_executable(command_journal_store_core_test
    command_journal_store_core_test.cpp
    ../../src/boot_v2/command_journal_store_core.cpp
    ../../src/boot_v2/command_journal_record_core.cpp
    ../../src/boot_v2/command_ack_core.cpp
    ../../src/boot_v2/mqtt_command_codec.cpp
)

target_include_directories(command_journal_store_core_test PRIVATE
    ../../src/boot_v2
)

target_compile_features(
    command_journal_store_core_test PRIVATE cxx_std_17
)
set_target_properties(
    command_journal_store_core_test PROPERTIES CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(command_journal_store_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(
    NAME command_journal_store_core_test
    COMMAND command_journal_store_core_test
)

add_executable(command_runtime_coordinator_test
    command_runtime_coordinator_test.cpp
    ../../src/boot_v2/command_boot_effect_core.cpp
    ../../src/boot_v2/command_journal_store_core.cpp
    ../../src/boot_v2/command_journal_record_core.cpp
    ../../src/boot_v2/command_ack_core.cpp
    ../../src/boot_v2/mqtt_command_codec.cpp
    ../../src/boot_v2/command_runtime_coordinator.cpp
    ../../src/boot_v2/runtime_owner_shutdown_record_core.cpp
)

target_include_directories(command_runtime_coordinator_test PRIVATE
    ../../src/boot_v2
)

target_compile_features(
    command_runtime_coordinator_test PRIVATE cxx_std_17
)
set_target_properties(
    command_runtime_coordinator_test PROPERTIES CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(command_runtime_coordinator_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(
    NAME command_runtime_coordinator_test
    COMMAND command_runtime_coordinator_test
)

add_executable(command_status_snapshot_evaluator_test
    command_status_snapshot_evaluator_test.cpp
    ../../src/boot_v2/command_status_snapshot_evaluator.cpp
    ../../src/boot_v2/runtime_snapshot_core.cpp
)

target_include_directories(command_status_snapshot_evaluator_test PRIVATE
    ../../src/boot_v2
)

target_compile_features(
    command_status_snapshot_evaluator_test PRIVATE cxx_std_17
)
set_target_properties(
    command_status_snapshot_evaluator_test PROPERTIES CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(command_status_snapshot_evaluator_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wswitch-enum
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(
    NAME command_status_snapshot_evaluator_test
    COMMAND command_status_snapshot_evaluator_test
)

add_executable(command_periodic_schedule_core_test
    command_periodic_schedule_core_test.cpp
)

target_include_directories(command_periodic_schedule_core_test PRIVATE
    ../../src/boot_v2
)

target_compile_features(
    command_periodic_schedule_core_test PRIVATE cxx_std_17
)
set_target_properties(
    command_periodic_schedule_core_test PROPERTIES CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(command_periodic_schedule_core_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(
    NAME command_periodic_schedule_core_test
    COMMAND command_periodic_schedule_core_test
)

add_executable(flash_partition_layout_test
    flash_partition_layout_test.cpp
)

target_include_directories(flash_partition_layout_test PRIVATE
    ../../src/boot_v2
)

target_compile_features(
    flash_partition_layout_test PRIVATE cxx_std_17
)
target_compile_definitions(flash_partition_layout_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
set_target_properties(
    flash_partition_layout_test PROPERTIES CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(flash_partition_layout_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(
    NAME flash_partition_layout_test
    COMMAND flash_partition_layout_test
)

add_test(
    NAME check_artifact_size_test
    COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_LIST_DIR}/../cmake/check_artifact_size_test.cmake
)

add_executable(command_journal_flash_store_contract_test
    command_journal_flash_store_contract_test.cpp
)

target_compile_features(
    command_journal_flash_store_contract_test PRIVATE cxx_std_17
)
target_compile_definitions(
    command_journal_flash_store_contract_test PRIVATE
    NB_IOT_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)
set_target_properties(
    command_journal_flash_store_contract_test PROPERTIES
    CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        command_journal_flash_store_contract_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -fno-exceptions
        -fno-rtti
    )
endif()

add_test(
    NAME command_journal_flash_store_contract_test
    COMMAND command_journal_flash_store_contract_test
)
