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

# 1. 보드 및 플랫폼 설정 (반드시 pico_sdk_import.cmake 이전에 설정)
set(PICO_BOARD pico2_w)        # Pico 2 W 모델 명시
set(PICO_PLATFORM rp2350)      # RP2350 칩셋 플랫폼 명시

# 2. SDK 경로 설정 (프로젝트 폴더 바로 아래의 pico-sdk)
set(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/pico-sdk")

# 3. Pico SDK 설정을 불러옴
include(pico_sdk_import.cmake)

# 4. 프로젝트 이름 및 언어 설정
project(nb_iot_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 5. Pico SDK 초기화
pico_sdk_init()

# 6. 실행 파일 생성
add_executable(nb_iot_project
    main.c
)

# 필요한 라이브러리 연결
target_link_libraries(nb_iot_project pico_stdlib hardware_uart)

# USB 시리얼(stdio) 활성화 여부 선택 (printf 등을 USB로 보려면 1)
pico_enable_stdio_usb(nb_iot_project 1)
pico_enable_stdio_uart(nb_iot_project 0)

# uf2, bin, elf, hex 파일 생성을 위한 추가 출력 설정
pico_add_extra_outputs(nb_iot_project)