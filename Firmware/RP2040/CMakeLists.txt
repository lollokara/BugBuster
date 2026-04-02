cmake_minimum_required(VERSION 3.13)

# =============================================================================
# BugBuster HAT — RP2040 Firmware
# Based on raspberrypi/debugprobe with BugBuster extensions
# =============================================================================

set(CMAKE_BUILD_TYPE RelWithDebInfo)

# Pico SDK (cloned under lib/)
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# FreeRTOS (from debugprobe submodule)
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/debugprobe/freertos)
include(${CMAKE_CURRENT_LIST_DIR}/lib/debugprobe/FreeRTOS_Kernel_import.cmake)

project(bugbuster_hat C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Require Pico SDK 2.x
if (${PICO_SDK_VERSION_MAJOR} LESS 2)
    message(SEND_ERROR "Pico SDK 2.x required. Found: ${PICO_SDK_VERSION_STRING}")
endif()

# =============================================================================
# Main executable: debugprobe core + BugBuster extensions
# =============================================================================

set(DEBUGPROBE_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/debugprobe)

add_executable(bugbuster_hat
    # debugprobe core sources
    ${DEBUGPROBE_DIR}/src/probe_config.c
    ${DEBUGPROBE_DIR}/src/main.c
    ${DEBUGPROBE_DIR}/src/usb_descriptors.c
    ${DEBUGPROBE_DIR}/src/probe.c
    ${DEBUGPROBE_DIR}/src/cdc_uart.c
    ${DEBUGPROBE_DIR}/src/get_serial.c
    ${DEBUGPROBE_DIR}/src/sw_dp_pio.c
    ${DEBUGPROBE_DIR}/src/tusb_edpt_handler.c
    ${DEBUGPROBE_DIR}/src/autobaud.c
    # CMSIS-DAP sources
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/JTAG_DP.c
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP_vendor.c
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/SWO.c
    # BugBuster extension sources
    src/bb_protocol.c
    src/bb_power.c
    src/bb_hvpak.c
    src/bb_pins.c
    src/bb_swd.c
    src/bb_la.c
)

# Include directories
target_include_directories(bugbuster_hat PRIVATE
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Include/
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/Core/Include/
    ${DEBUGPROBE_DIR}/include/
    ${DEBUGPROBE_DIR}/src/
    src/
)

# Compile definitions
target_compile_definitions(bugbuster_hat PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
    DEBUG_ON_PICO=1                   # Use Pico-style pin mapping (not Debug Probe board)
    DEBUGPROBE_INTEGRATION=1          # Enable debugprobe hooks in our code
    BB_HAT_FIRMWARE=1
)

target_compile_options(bugbuster_hat PRIVATE -Wall)

# PIO programs — debugprobe SWD (PIO 0)
pico_generate_pio_header(bugbuster_hat ${DEBUGPROBE_DIR}/src/probe.pio)
pico_generate_pio_header(bugbuster_hat ${DEBUGPROBE_DIR}/src/probe_oen.pio)
pico_generate_pio_header(bugbuster_hat ${DEBUGPROBE_DIR}/src/autobaud.pio)

# PIO programs — BugBuster LA (PIO 1)
pico_generate_pio_header(bugbuster_hat ${CMAKE_CURRENT_LIST_DIR}/src/bb_la.pio)

# Version header — debugprobe expects probe/version.h
set(VERSION_DIR ${CMAKE_BINARY_DIR}/generated/probe)
file(MAKE_DIRECTORY ${VERSION_DIR})
file(WRITE ${VERSION_DIR}/version.h "#ifndef _PROBE_VERSION_H\n#define _PROBE_VERSION_H\n#define PROBE_VERSION \"bb-hat-1.0\"\n#endif\n")
target_include_directories(bugbuster_hat PRIVATE ${CMAKE_BINARY_DIR}/generated)

# Libraries
target_link_libraries(bugbuster_hat PRIVATE
    pico_multicore
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_clocks
    hardware_adc
    hardware_i2c
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap1
)

pico_set_binary_type(bugbuster_hat copy_to_ram)
pico_add_extra_outputs(bugbuster_hat)
