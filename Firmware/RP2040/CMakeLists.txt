cmake_minimum_required(VERSION 3.13)

set(CMAKE_BUILD_TYPE RelWithDebInfo)

set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
# Force Pico SDK to use our forked TinyUSB (lib/tinyusb) which adds
# tud_vendor_n_fifo_clear and tud_vendor_n_rx_reprime used by bb_la_usb.c.
# Without this, pico_sdk_init() resolves TinyUSB from ${PICO_SDK_PATH}/lib/tinyusb
# (upstream) and the link step fails with undefined references to those symbols.
# PICO_TINYUSB_PATH is honoured from either an env var or a CMake cache entry;
# FORCE ensures this value wins even if something up-tree pre-defined it.
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/tinyusb CACHE PATH "TinyUSB path" FORCE)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/debugprobe/freertos)
include(${CMAKE_CURRENT_LIST_DIR}/lib/debugprobe/FreeRTOS_Kernel_import.cmake)

project(bugbuster_hat C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

if (${PICO_SDK_VERSION_MAJOR} LESS 2)
    message(SEND_ERROR "Pico SDK 2.x required.")
endif()

set(DEBUGPROBE_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/debugprobe)

add_executable(bugbuster_hat
    # Our integrated main (replaces debugprobe's main.c — adds bb_cmd_task)
    src/bb_main_integrated.c
    # Our USB descriptors (replaces debugprobe's — adds LA vendor interface)
    src/bb_usb_descriptors.c
    # debugprobe core sources (main.c + usb_descriptors.c EXCLUDED — we provide our own)
    ${DEBUGPROBE_DIR}/src/probe_config.c
    ${DEBUGPROBE_DIR}/src/probe.c
    ${DEBUGPROBE_DIR}/src/cdc_uart.c
    ${DEBUGPROBE_DIR}/src/get_serial.c
    ${DEBUGPROBE_DIR}/src/sw_dp_pio.c
    ${DEBUGPROBE_DIR}/src/tusb_edpt_handler.c
    ${DEBUGPROBE_DIR}/src/autobaud.c
    # CMSIS-DAP
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/JTAG_DP.c
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP_vendor.c
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Source/SWO.c
    # BugBuster extensions
    src/bb_main.c
    src/bb_protocol.c
    src/bb_power.c
    src/bb_hvpak.c
    src/bb_pins.c
    src/bb_swd.c
    src/bb_la.c
    src/bb_la_rle.c
    src/bb_la_usb.c
)

target_include_directories(bugbuster_hat PRIVATE
    src/
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/DAP/Firmware/Include/
    ${DEBUGPROBE_DIR}/CMSIS_DAP/CMSIS/Core/Include/
    ${DEBUGPROBE_DIR}/include/
    ${DEBUGPROBE_DIR}/src/
)

target_compile_definitions(bugbuster_hat PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
    DEBUG_ON_PICO=1
    DEBUGPROBE_INTEGRATION=1
    BB_HAT_FIRMWARE=1
)

target_compile_options(bugbuster_hat PRIVATE -Wall)
# Our local override headers must win over debugprobe defaults.

# PIO programs — debugprobe SWD (PIO 0)
pico_generate_pio_header(bugbuster_hat ${DEBUGPROBE_DIR}/src/probe.pio)
pico_generate_pio_header(bugbuster_hat ${DEBUGPROBE_DIR}/src/probe_oen.pio)
pico_generate_pio_header(bugbuster_hat ${DEBUGPROBE_DIR}/src/autobaud.pio)

# PIO programs — BugBuster LA (PIO 1)
pico_generate_pio_header(bugbuster_hat ${CMAKE_CURRENT_LIST_DIR}/src/bb_la.pio)
pico_generate_pio_header(bugbuster_hat ${CMAKE_CURRENT_LIST_DIR}/src/bb_la_trigger.pio)

# Version — single source of truth.
# PROBE_VERSION format: "bb-hat-MAJOR.MINOR"
# CMake extracts MAJOR/MINOR and injects them as compile definitions so that
# bb_main.c (BB_HAT_FW_MAJOR/MINOR) no longer needs manual edits on release.
set(PROBE_VERSION "bb-hat-2.1")
string(REGEX MATCH "bb-hat-([0-9]+)\\.([0-9]+)" _ver_match "${PROBE_VERSION}")
if(NOT _ver_match)
    message(FATAL_ERROR "PROBE_VERSION '${PROBE_VERSION}' does not match expected format 'bb-hat-MAJOR.MINOR'")
endif()
set(BB_HAT_FW_MAJOR_VAL "${CMAKE_MATCH_1}")
set(BB_HAT_FW_MINOR_VAL "${CMAKE_MATCH_2}")

set(VERSION_DIR ${CMAKE_BINARY_DIR}/generated/probe)
file(MAKE_DIRECTORY ${VERSION_DIR})
file(WRITE ${VERSION_DIR}/version.h "#ifndef _PROBE_VERSION_H\n#define _PROBE_VERSION_H\n#define PROBE_VERSION \"${PROBE_VERSION}\"\n#endif\n")
target_include_directories(bugbuster_hat PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(bugbuster_hat PRIVATE
    BB_HAT_FW_MAJOR=${BB_HAT_FW_MAJOR_VAL}
    BB_HAT_FW_MINOR=${BB_HAT_FW_MINOR_VAL}
)

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
    hardware_uart
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap1
)

# Keep the image XIP by default; the LA already uses DMA/PIO for the fast path,
# and forcing copy_to_ram pushes the full firmware over the RP2040 SRAM budget.
pico_set_binary_type(bugbuster_hat default)

pico_add_extra_outputs(bugbuster_hat)
