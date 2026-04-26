# Auto-generated component build for the BugBuster ESP32-S3 firmware.
#
# Source layout (2026-04-26 reorg): files are grouped into cohesive subdirs
# (hal/, bus/, bbp/, cli/, web/, net/, mp/, hat/, diag/, dio/). Build still
# uses GLOB_RECURSE so adding a new file just works without touching this
# file. Headers live next to their .cpp in the same subdir; bare
# `#include "foo.h"` keeps working because every directory containing an .h
# is added to INCLUDE_DIRS below.

file(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

file(GLOB_RECURSE app_headers ${CMAKE_SOURCE_DIR}/src/*.h)

# Derive INCLUDE_DIRS from every directory that contains a header, so that
# bare `#include "foo.h"` continues to resolve after the reorg without
# rewriting any source file.
set(include_dirs "")
foreach(h ${app_headers})
    get_filename_component(d ${h} DIRECTORY)
    list(APPEND include_dirs ${d})
endforeach()
list(REMOVE_DUPLICATES include_dirs)
# Guarantee the component root is always present (for headers added later).
list(APPEND include_dirs ${CMAKE_CURRENT_LIST_DIR})
list(REMOVE_DUPLICATES include_dirs)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES micropython esp_http_client mqtt mbedtls
)
