# MicroPython ESP-IDF component — Phase 0 (link only, nothing executed)
# PlatformIO uses SCons, not Ninja, so add_custom_command/add_custom_target from
# mkrules.cmake never run under PIO. We run the entire qstr/moduledefs/root_pointers
# generation pipeline at CMake configure time via execute_process() instead.

cmake_minimum_required(VERSION 3.16)

# ── Paths ─────────────────────────────────────────────────────────────────────
set(MICROPY_DIR ${CMAKE_CURRENT_LIST_DIR}/micropython)
set(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32)
set(MICROPY_BOARD_DIR ${CMAKE_CURRENT_LIST_DIR})

# Generated headers directory — must exist before idf_component_register validates it.
set(MICROPY_GENHDR_DIR ${CMAKE_BINARY_DIR}/genhdr)
file(MAKE_DIRECTORY ${MICROPY_GENHDR_DIR})

# py.cmake sets MICROPY_SOURCE_PY, MICROPY_INC_CORE, MICROPY_PY_DIR.
# Only defines variables — safe to call unconditionally.
include(${MICROPY_DIR}/py/py.cmake)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Disable btree: berkeley-db submodule is not initialised.
    set(MICROPY_PY_BTREE OFF)

    # extmod.cmake sets MICROPY_SOURCE_EXTMOD, MICROPY_OOFATFS_DIR, etc.
    include(${MICROPY_DIR}/extmod/extmod.cmake)

    # usermod.cmake — creates the empty 'usermod' INTERFACE library target.
    include(${MICROPY_DIR}/py/usermod.cmake)

    # ── Shared runtime helpers ─────────────────────────────────────────────────
    list(APPEND MICROPY_SOURCE_SHARED
        ${MICROPY_DIR}/shared/runtime/interrupt_char.c
        ${MICROPY_DIR}/shared/runtime/stdout_helpers.c
        ${MICROPY_DIR}/shared/runtime/sys_stdio_mphal.c
        ${MICROPY_DIR}/shared/timeutils/timeutils.c
    )

    # ── Port sources (our minimal HAL) ────────────────────────────────────────
    list(APPEND MICROPY_SOURCE_PORT
        ${CMAKE_CURRENT_LIST_DIR}/mphalport.c
    )

    # ── BugBuster app sources that contain qstr/module-registration macros ────
    # PlatformIO auto-compiles Firmware/ESP32/src/*.cpp into the app component.
    # Keep these out of MICROPY_SOURCE_PORT to avoid duplicate object actions,
    # but include them in the qstr/moduledefs pipeline below.
    list(APPEND MICROPY_SOURCE_BUGBUSTER
        ${CMAKE_CURRENT_LIST_DIR}/../../src/mp/modbugbuster.c
        ${CMAKE_CURRENT_LIST_DIR}/../../src/mp/modbugbuster_channel.c
        ${CMAKE_CURRENT_LIST_DIR}/../../src/mp/modbugbuster_i2c.c
        ${CMAKE_CURRENT_LIST_DIR}/../../src/mp/modbugbuster_spi.c
    )

    # ── qstr source list (all sources that contain Q(...) macros) ─────────────
    list(APPEND MICROPY_SOURCE_QSTR
        ${MICROPY_SOURCE_PY}
        ${MICROPY_SOURCE_EXTMOD}
        ${MICROPY_SOURCE_SHARED}
        ${MICROPY_SOURCE_PORT}
        ${MICROPY_SOURCE_BUGBUSTER}
    )

    # Port qstr definitions
    set(MICROPY_QSTRDEFS_PY ${MICROPY_PY_DIR}/qstrdefs.h)
    set(MICROPY_QSTRDEFS_PORT ${CMAKE_CURRENT_LIST_DIR}/qstrdefsport.h)

    # ── IDF components required by MicroPython core ───────────────────────────
    list(APPEND IDF_COMPONENTS
        app_update
        bootloader_support
        driver
        esp_adc
        esp_common
        esp_event
        esp_hw_support
        esp_netif
        esp_partition
        esp_pm
        esp_psram
        esp_ringbuf
        esp_rom
        esp_system
        esp_timer
        esp_wifi
        esp_random
        freertos
        hal
        heap
        log
        lwip
        mbedtls
        newlib
        nvs_flash
        soc
        spi_flash
        vfs
    )
endif()

# ── Register the IDF component ────────────────────────────────────────────────
idf_component_register(
    SRCS
        ${MICROPY_SOURCE_PY}
        ${MICROPY_SOURCE_EXTMOD}
        ${MICROPY_SOURCE_SHARED}
        ${MICROPY_SOURCE_PORT}
    INCLUDE_DIRS
        ${MICROPY_INC_CORE}
        ${MICROPY_INC_USERMOD}
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_BINARY_DIR}
        ${MICROPY_GENHDR_DIR}
    REQUIRES
        ${IDF_COMPONENTS}
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(MICROPY_TARGET ${COMPONENT_TARGET})

    # ── Compile definitions ───────────────────────────────────────────────────
    # VFS/LFS/fatfs disabled in Phase 0 — those lib sources are not included.
    target_compile_definitions(${MICROPY_TARGET} PUBLIC
        ${MICROPY_DEF_CORE}
    )

    # Suppress upstream warnings
    target_compile_options(${MICROPY_TARGET} PUBLIC
        -Wno-clobbered
        -Wno-deprecated-declarations
        -Wno-missing-field-initializers
    )

    # usermod INTERFACE library (empty, created by usermod.cmake)
    target_link_libraries(${MICROPY_TARGET} usermod)

    # ── qstr generation at configure time (PIO uses SCons, not Ninja) ─────────
    # PlatformIO only runs CMake for code-model extraction; add_custom_command
    # chains in mkrules.cmake are never executed by SCons. We run every step
    # of the pipeline here with execute_process() so headers exist before
    # SCons starts compiling.

    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    # Build preprocessor flags from the registered component target's properties.
    get_target_property(_mp_inc  ${MICROPY_TARGET} INCLUDE_DIRECTORIES)
    get_target_property(_mp_def  ${MICROPY_TARGET} COMPILE_DEFINITIONS)

    set(MICROPY_CPP_FLAGS "")
    if(_mp_inc)
        foreach(_i ${_mp_inc})
            # Skip CMake generator expressions — they're not resolvable at configure time.
            if(NOT _i MATCHES "^\\$<")
                list(APPEND MICROPY_CPP_FLAGS "-I${_i}")
            endif()
        endforeach()
    endif()
    if(_mp_def)
        foreach(_d ${_mp_def})
            if(NOT _d MATCHES "^\\$<")
                list(APPEND MICROPY_CPP_FLAGS "-D${_d}")
            endif()
        endforeach()
    endif()

    # ── BugBuster src/ subdir include path for qstr pp ────────────────────────
    # After the 2026-04-26 reorg, headers live next to their .cpp in subdirs
    # (hal/, bus/, bbp/, cli/, web/, net/, mp/, hat/, diag/, dio/). The
    # MICROPY_TARGET property only carries the parent src/ dir at this point
    # in CMake configure, so the qstr pp pipeline can't resolve bare
    # `#include "foo.h"` for headers that moved into subdirs. Glob every dir
    # under src/ that contains a .h and add it explicitly.
    file(GLOB_RECURSE _bb_app_headers ${CMAKE_CURRENT_LIST_DIR}/../../src/*.h)
    set(_bb_inc_dirs "")
    foreach(_h ${_bb_app_headers})
        get_filename_component(_d ${_h} DIRECTORY)
        list(APPEND _bb_inc_dirs ${_d})
    endforeach()
    list(REMOVE_DUPLICATES _bb_inc_dirs)
    foreach(_d ${_bb_inc_dirs})
        list(APPEND MICROPY_CPP_FLAGS "-I${_d}")
    endforeach()

    # Add essential IDF component include paths that appear as generator expressions
    # in target properties (and thus were skipped above). These are needed for the
    # qstrdefs preprocessing step which runs the C preprocessor at configure time.
    set(_IDF_COMP $ENV{IDF_PATH}/components)
    # Also handle PlatformIO's framework-espidf path
    if(NOT EXISTS ${_IDF_COMP}/freertos)
        # PlatformIO stores IDF in a different location
        get_filename_component(_IDF_COMP
            ${CMAKE_CURRENT_LIST_DIR}/../../../../.platformio/packages/framework-espidf/components
            ABSOLUTE)
    endif()

    if(EXISTS ${_IDF_COMP}/freertos)
        # FreeRTOS
        list(APPEND MICROPY_CPP_FLAGS
            "-I${_IDF_COMP}/freertos/FreeRTOS-Kernel/include"
            "-I${_IDF_COMP}/freertos/FreeRTOS-Kernel/portable/xtensa/include"
            "-I${_IDF_COMP}/freertos/esp_additions/include"
            "-I${_IDF_COMP}/freertos/esp_additions/include/freertos"
            "-I${_IDF_COMP}/freertos/FreeRTOS-Kernel/include/freertos"
        )
        # esp_common, esp_system, esp_rom, newlib, soc, hal
        list(APPEND MICROPY_CPP_FLAGS
            "-I${_IDF_COMP}/esp_common/include"
            "-I${_IDF_COMP}/esp_system/include"
            "-I${_IDF_COMP}/esp_rom/include"
            "-I${_IDF_COMP}/esp_rom/include/esp32s3"
            "-I${_IDF_COMP}/esp_rom/esp32s3"
            "-I${_IDF_COMP}/newlib/platform_include"
            "-I${_IDF_COMP}/soc/include"
            "-I${_IDF_COMP}/soc/esp32s3/include"
            "-I${_IDF_COMP}/hal/include"
            "-I${_IDF_COMP}/hal/platform_port/include"
            "-I${_IDF_COMP}/hal/esp32s3/include"
            "-I${_IDF_COMP}/heap/include"
            "-I${_IDF_COMP}/log/include"
            "-I${_IDF_COMP}/esp_hw_support/include"
            "-I${_IDF_COMP}/esp_hw_support/include/soc"
            "-I${_IDF_COMP}/esp_hw_support/port/esp32s3/include"
            "-I${_IDF_COMP}/xtensa/include"
            "-I${_IDF_COMP}/xtensa/esp32s3/include"
            "-DESP_PLATFORM"
            "-DIDF_VER=\"v5.3.1\""
            "-DGCC_NOT_5_2_0=0"
        )
    endif()

    # ── Step 1: mpversion.h ───────────────────────────────────────────────────
    set(MICROPY_MPVERSION ${MICROPY_GENHDR_DIR}/mpversion.h)
    if(NOT EXISTS ${MICROPY_MPVERSION})
        execute_process(
            COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeversionhdr.py ${MICROPY_MPVERSION}
            WORKING_DIRECTORY ${MICROPY_DIR}
            RESULT_VARIABLE _res
        )
        if(_res)
            message(FATAL_ERROR "MicroPython: makeversionhdr.py failed (${_res})")
        endif()
    endif()

    # ── Step 2: qstr.i.last — preprocess all qstr sources ────────────────────
    set(MICROPY_QSTRDEFS_LAST ${MICROPY_GENHDR_DIR}/qstr.i.last)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py
            pp ${CMAKE_C_COMPILER} -E
            output ${MICROPY_QSTRDEFS_LAST}
            cflags   ${MICROPY_CPP_FLAGS} -DNO_QSTR
            cxxflags ${MICROPY_CPP_FLAGS} -DNO_QSTR
            sources  ${MICROPY_SOURCE_QSTR}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdefs.py pp failed (${_res})")
    endif()

    # ── Step 3: split qstr ────────────────────────────────────────────────────
    set(MICROPY_QSTRDEFS_SPLIT ${MICROPY_GENHDR_DIR}/qstr.split)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py
            split qstr ${MICROPY_QSTRDEFS_LAST} ${MICROPY_GENHDR_DIR}/qstr _
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdefs.py split qstr failed (${_res})")
    endif()
    file(TOUCH ${MICROPY_QSTRDEFS_SPLIT})

    # ── Step 4: cat qstr (collected) ─────────────────────────────────────────
    set(MICROPY_QSTRDEFS_COLLECTED ${MICROPY_GENHDR_DIR}/qstrdefs.collected.h)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py
            cat qstr _ ${MICROPY_GENHDR_DIR}/qstr ${MICROPY_QSTRDEFS_COLLECTED}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdefs.py cat qstr failed (${_res})")
    endif()

    # ── Step 5: preprocess qstrdefs (cat | sed | cpp | sed) ──────────────────
    # Add -DNO_QSTR so mpconfigport.h skips IDF includes (FreeRTOSConfig.h etc.)
    # that aren't on the path at configure time. The QCFG/Q() macros only need
    # py/mpconfig.h defines (MICROPY_QSTR_BYTES_IN_LEN etc.), not IDF headers.
    set(MICROPY_QSTRDEFS_PREPROCESSED ${MICROPY_GENHDR_DIR}/qstrdefs.preprocessed.h)
    # Flatten flags into a shell-safe string for the -c argument
    string(JOIN " " _cpp_flags_str ${MICROPY_CPP_FLAGS})
    execute_process(
        COMMAND sh -c
            "cat ${MICROPY_QSTRDEFS_PY} ${MICROPY_QSTRDEFS_PORT} ${MICROPY_QSTRDEFS_COLLECTED} \
             | sed 's/^Q(.*)/\"&\"/' \
             | ${CMAKE_C_COMPILER} -E ${_cpp_flags_str} -DNO_QSTR - \
             | sed 's/^\\\"\\(Q(.*)\\)\\\"/\\1/' \
             > ${MICROPY_QSTRDEFS_PREPROCESSED}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: qstrdefs preprocess step failed (${_res})")
    endif()

    # ── Step 6: generate qstrdefs.generated.h ────────────────────────────────
    set(MICROPY_QSTRDEFS_GENERATED ${MICROPY_GENHDR_DIR}/qstrdefs.generated.h)
    execute_process(
        COMMAND sh -c
            "${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdata.py \
             ${MICROPY_QSTRDEFS_PREPROCESSED} > ${MICROPY_QSTRDEFS_GENERATED}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdata.py failed (${_res})")
    endif()

    # ── Step 7: moduledefs.h ──────────────────────────────────────────────────
    set(MICROPY_MODULEDEFS_SPLIT    ${MICROPY_GENHDR_DIR}/moduledefs.split)
    set(MICROPY_MODULEDEFS_COLLECTED ${MICROPY_GENHDR_DIR}/moduledefs.collected)
    set(MICROPY_MODULEDEFS          ${MICROPY_GENHDR_DIR}/moduledefs.h)

    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py
            split module ${MICROPY_QSTRDEFS_LAST} ${MICROPY_GENHDR_DIR}/module _
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdefs.py split module failed (${_res})")
    endif()
    file(TOUCH ${MICROPY_MODULEDEFS_SPLIT})

    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py
            cat module _ ${MICROPY_GENHDR_DIR}/module ${MICROPY_MODULEDEFS_COLLECTED}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdefs.py cat module failed (${_res})")
    endif()

    execute_process(
        COMMAND sh -c
            "${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makemoduledefs.py \
             ${MICROPY_MODULEDEFS_COLLECTED} > ${MICROPY_MODULEDEFS}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makemoduledefs.py failed (${_res})")
    endif()

    # ── Step 8: root_pointers.h ───────────────────────────────────────────────
    set(MICROPY_ROOT_POINTERS_SPLIT    ${MICROPY_GENHDR_DIR}/root_pointers.split)
    set(MICROPY_ROOT_POINTERS_COLLECTED ${MICROPY_GENHDR_DIR}/root_pointers.collected)
    set(MICROPY_ROOT_POINTERS          ${MICROPY_GENHDR_DIR}/root_pointers.h)

    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py
            split root_pointer ${MICROPY_QSTRDEFS_LAST} ${MICROPY_GENHDR_DIR}/root_pointer _
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdefs.py split root_pointer failed (${_res})")
    endif()
    file(TOUCH ${MICROPY_ROOT_POINTERS_SPLIT})

    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py
            cat root_pointer _ ${MICROPY_GENHDR_DIR}/root_pointer ${MICROPY_ROOT_POINTERS_COLLECTED}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: makeqstrdefs.py cat root_pointer failed (${_res})")
    endif()

    execute_process(
        COMMAND sh -c
            "${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/make_root_pointers.py \
             ${MICROPY_ROOT_POINTERS_COLLECTED} > ${MICROPY_ROOT_POINTERS}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        RESULT_VARIABLE _res
    )
    if(_res)
        message(FATAL_ERROR "MicroPython: make_root_pointers.py failed (${_res})")
    endif()

    message(STATUS "MicroPython: generated headers written to ${MICROPY_GENHDR_DIR}")
endif()
