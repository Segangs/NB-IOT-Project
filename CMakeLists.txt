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
set(PICO_BOARD pico2_w)        
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

# Read .env file if it exists and define compiler macros
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.env")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.env" ENV_LINES)
    foreach(line ${ENV_LINES})
        # Ignore comments and empty lines
        if(NOT line MATCHES "^#" AND line MATCHES "=")
            string(REPLACE "=" ";" line_split ${line})
            list(GET line_split 0 key)
            list(GET line_split 1 val)
            # Remove any wrapping quotes from val
            string(REGEX REPLACE "^\"|\"$" "" val "${val}")
            # Register the definition as a compiler preprocessor macro
            add_compile_definitions(${key}="${val}")
        endif()
    endforeach()
endif()

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

# stdio 설정
add_compile_definitions(PICO_STDIO_USB_DEFAULT_TIMEOUT_MS=0)
pico_enable_stdio_usb(nb_iot_project 1)
pico_enable_stdio_uart(nb_iot_project 0)

pico_add_extra_outputs(nb_iot_project)
