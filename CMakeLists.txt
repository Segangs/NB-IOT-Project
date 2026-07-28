# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

cmake_minimum_required(VERSION 3.13)

# 1. 보드 및 플랫폼 설정 
set(PICO_BOARD pico2)
set(PICO_PLATFORM rp2350)      

# 💡 [여기가 핵심!] SDK 환경을 불러오기 전에 프리토스 소스 경로를 강력하게 주입합니다.
set(PICO_FREERTOS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel" CACHE PATH "FreeRTOS Kernel Path" FORCE)

# 2. SDK 경로 설정 
set(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/pico-sdk")

# 3. Pico SDK 설정을 불러옴
include(pico_sdk_import.cmake)

# 4. 프로젝트 이름 및 언어 설정
project(nb_iot_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Load only the public settings required by the firmware target.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_firmware_env.cmake")
nb_iot_load_firmware_env(
    "${CMAKE_CURRENT_SOURCE_DIR}/.env"
    NB_IOT_FIRMWARE_ENV_DEFINITIONS
)

# 5. Pico SDK 초기화
pico_sdk_init()

# ====================================================================================
# [참고 반영] FreeRTOS 경로 지정 및 전용 포트 임포트
# ====================================================================================
# 1) FreeRTOS 설정 폴더 경로 지정 및 등록
set(FREERTOS_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Config" CACHE STRING "FreeRTOS Config Path")
include_directories("${FREERTOS_CONFIG_DIR}")

# 2) FreeRTOS 커널 폴더 경로 지정
set(FREERTOS_KERNEL_PATH "${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel" CACHE PATH "FreeRTOS Kernel Path" FORCE)

# 3) RP2350 전용 FreeRTOS 임포트 스크립트 실행 (헤더 경로 자동 매핑)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)
# ====================================================================================



# 6. 실행 파일 생성
add_executable(nb_iot_project
    main.cpp
    src/boot_v2/lifecycle_gate.cpp
    src/boot_v2/liveness_service.cpp
    src/boot_v2/runtime_owner_core.cpp
    src/boot_v2/runtime_owner_adapter_core.cpp
    src/boot_v2/runtime_owner_executor_contract.cpp
    src/boot_v2/runtime_snapshot_core.cpp
    src/boot_v2/runtime_owner_task_core.cpp
    src/boot_v2/runtime_owner_rtos.cpp
    src/boot_v2/runtime_owner_cutover_core.cpp
    src/boot_v2/runtime_owner_producer_facade.cpp
    src/boot_v2/runtime_owner_device_backend.cpp
    src/boot_v2/runtime_owner_shutdown_finalizer_core.cpp
    src/boot_v2/runtime_owner_shutdown_record_core.cpp
    src/boot_v2/runtime_owner_shutdown_record_store.cpp
    src/boot_v2/power_state_core.cpp
    src/boot_v2/power_state_runtime.cpp
    src/boot_v2/sensor_quality_core.cpp
    src/boot_v2/temperature_alarm_publish_core.cpp
    src/boot_v2/mqtt_power_event_codec.cpp
    src/boot_v2/mqtt_command_codec.cpp
    src/boot_v2/command_ack_core.cpp
    src/boot_v2/command_journal_record_core.cpp
    src/boot_v2/command_journal_store_core.cpp
    src/boot_v2/command_journal_flash_store.cpp
    src/boot_v2/command_boot_effect_core.cpp
    src/boot_v2/command_runtime_coordinator.cpp
    src/boot_v2/command_status_snapshot_evaluator.cpp
    lib/LCD_I2C.cpp
    src/tasks/app_context.cpp
    src/tasks/tasks_boot.cpp
    src/tasks/tasks_buzzer.cpp
    src/tasks/tasks_debug.cpp
    src/tasks/tasks_led.cpp
    src/tasks/tasks_sensor.cpp
    src/tasks/tasks_sensor_reader.cpp
    src/tasks/tasks_lcd.cpp
    src/tasks/tasks_modem.cpp
    src/tasks/tasks_mqtt.cpp
    src/tasks/tasks_periodic_modem.cpp
    src/lib/flash_logger.cpp
    src/lib/log.cpp
    src/lib/mqtt_payload.cpp
)

# Standalone HL7811 diagnostic image.  It deliberately has no FreeRTOS,
# RuntimeOwner, sensor, display, audio, flash-log, or product-task sources.
add_executable(modem_at_console
    src/diagnostics/modem_at_console.cpp
)

target_compile_features(modem_at_console PRIVATE cxx_std_17)

target_link_libraries(modem_at_console PRIVATE
    pico_stdlib
    pico_stdio
    pico_stdio_usb
    hardware_uart
)

pico_enable_stdio_usb(modem_at_console 1)
pico_enable_stdio_uart(modem_at_console 0)
pico_add_extra_outputs(modem_at_console)

# 7. 라이브러리 연결
target_link_libraries(nb_iot_project 
    pico_stdlib
    pico_stdio
    pico_stdio_usb
    hardware_uart
    hardware_adc
    hardware_i2c
    hardware_pio
    hardware_flash
    pico_flash
    hardware_pwm
    pico_multicore      
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

target_compile_definitions(nb_iot_project PRIVATE
    ${NB_IOT_FIRMWARE_ENV_DEFINITIONS}
    NB_IOT_POST_CONFIG_HANDOFF_TRIAL=1
    NB_IOT_CONFIG_QOS0_TRIAL=1
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
)

# stdio 설정
add_compile_definitions(PICO_STDIO_USB_DEFAULT_TIMEOUT_MS=0)
pico_enable_stdio_usb(nb_iot_project 1)
pico_enable_stdio_uart(nb_iot_project 0)

pico_add_extra_outputs(nb_iot_project)

set(flash_partition_layout_header
    "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_v2/flash_partition_layout.hpp")
file(READ "${flash_partition_layout_header}" flash_partition_layout_source)
set(firmware_a_declaration_semicolon "__NB_IOT_FIRMWARE_A_DECLARATION_SEMICOLON__")
set(firmware_a_declaration_regex
    "inline[ ]+constexpr[ ]+std::uint32_t[ ]+firmware_a_size[ ]*=[ ]*(0x[0-9A-Fa-f]+)u[ ]*${firmware_a_declaration_semicolon}")
string(REPLACE ";" "${firmware_a_declaration_semicolon}"
    flash_partition_layout_source_for_match
    "${flash_partition_layout_source}")
string(REGEX MATCHALL "${firmware_a_declaration_regex}"
    firmware_slot_size_matches
    "${flash_partition_layout_source_for_match}")
list(LENGTH firmware_slot_size_matches firmware_slot_size_match_count)
if(NOT firmware_slot_size_match_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one canonical firmware_a_size declaration in "
        "${flash_partition_layout_header}; found ${firmware_slot_size_match_count}")
endif()
string(REGEX MATCH "${firmware_a_declaration_regex}"
    firmware_slot_size_match
    "${firmware_slot_size_matches}")
if(NOT firmware_slot_size_match)
    message(FATAL_ERROR
        "Unable to derive firmware_slot_size from firmware_a_size in "
        "${flash_partition_layout_header}")
endif()
math(EXPR firmware_slot_size "${CMAKE_MATCH_1}")

add_custom_command(
    TARGET nb_iot_project
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=$<TARGET_FILE_DIR:nb_iot_project>/nb_iot_project.bin
        -DMAX_BYTES=${firmware_slot_size}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_artifact_size.cmake"
    VERBATIM
)
