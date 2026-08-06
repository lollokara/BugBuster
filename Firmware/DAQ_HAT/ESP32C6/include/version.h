#pragma once

// =============================================================================
// version.h — central firmware version for the ESP32-C6 DAQ HAT display MCU.
//
// Mirrors Firmware/DAQ_HAT/ESP32P4/include/version.h. Bump these on every
// release; the release pipeline reads them with
// `python Firmware/tools/firmware_version.py c6` to name the published asset
// and to fill the `c6` entry of bugbuster-update-manifest.json.
//
// UNLIKE the P4, this version is NOT reported over the wire. DDP carries no C6
// build ID, so `hat_daq_c6_version_t::c6_build_id` is always empty and the S3
// cannot verify a published buildId against the chip that is actually running.
// Closing that gap means bumping DDP_PROTO_VERSION and the C6 firmware
// together; until then this header is the single source of truth for CI only,
// and the boot log below is the sole way to read it off a live board.
//
// Semantic versioning: MAJOR.MINOR.PATCH.
//   MAJOR — breaking wire-protocol or hardware-contract changes.
//   MINOR — backward-compatible features.
//   PATCH — fixes only.
// =============================================================================

#include <stdint.h>

#define FW_VERSION_MAJOR   2
#define FW_VERSION_MINOR   1
#define FW_VERSION_PATCH   0

// Packed 32-bit version: 0x00MMmmpp (major/minor/patch), easy to compare.
#define FW_VERSION_U32  (((uint32_t)FW_VERSION_MAJOR << 16) | \
                         ((uint32_t)FW_VERSION_MINOR << 8)  | \
                         ((uint32_t)FW_VERSION_PATCH))

#define FW_STRINGIFY_(x) #x
#define FW_STRINGIFY(x)  FW_STRINGIFY_(x)

// Human-readable version string, e.g. "bb-daq-c6 1.0.0".
#define FW_VERSION_STRING  "bb-daq-c6 " \
    FW_STRINGIFY(FW_VERSION_MAJOR) "." \
    FW_STRINGIFY(FW_VERSION_MINOR) "." \
    FW_STRINGIFY(FW_VERSION_PATCH)

// Product identifier. The C6 is flashed as a full merged image by the ESP-ROM
// loader (not an OTA slot), so nothing on-chip checks this today; it exists so
// the two DAQ HAT headers stay symmetric.
#define FW_PRODUCT_ID      "bb-daq-c6"
