#pragma once

// =============================================================================
// version.h — central firmware version for the ESP32-P4 DAQ HAT.
//
// Bump these on every release. The version is reported over the S3 HAT link
// (GET_VERSION) and embedded in the OTA metadata so the S3 / network / PC can
// decide whether an update is needed and detect rollbacks.
//
// Semantic versioning: MAJOR.MINOR.PATCH.
//   MAJOR — breaking wire-protocol or hardware-contract changes.
//   MINOR — backward-compatible features.
//   PATCH — fixes only.
// =============================================================================

#include <stdint.h>

#define FW_VERSION_MAJOR   2
#define FW_VERSION_MINOR   0
#define FW_VERSION_PATCH   5

// Packed 32-bit version: 0x00MMmmpp (Major/minor/patch), easy to compare.
#define FW_VERSION_U32  (((uint32_t)FW_VERSION_MAJOR << 16) | \
                         ((uint32_t)FW_VERSION_MINOR << 8)  | \
                         ((uint32_t)FW_VERSION_PATCH))

#define FW_STRINGIFY_(x) #x
#define FW_STRINGIFY(x)  FW_STRINGIFY_(x)

// Human-readable version string, e.g. "bb-daq-p4 1.0.0".
#define FW_VERSION_STRING  "bb-daq-p4 " \
    FW_STRINGIFY(FW_VERSION_MAJOR) "." \
    FW_STRINGIFY(FW_VERSION_MINOR) "." \
    FW_STRINGIFY(FW_VERSION_PATCH)

// Product identifier embedded in OTA metadata so a wrong-target image is
// rejected before it is written.
#define FW_PRODUCT_ID      "bb-daq-p4"
