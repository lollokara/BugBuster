#pragma once

#include <stdint.h>

#define BB_STANDBY_SCHEMA 1u
#define BB_HAT_CMD_STANDBY 0x7Cu
#define BB_HAT_RSP_STANDBY 0x9Cu
#define BB_DDP_CMD_STANDBY 0x1Fu
#define BB_MB_STANDBY_POLICY 0x0Au

#define BB_ST_OP_POLL 0u
#define BB_ST_OP_SLEEP 1u
#define BB_ST_OP_WAKE 2u
#define BB_ST_OP_PROGRESS 3u

#define BB_ST_ACTIVE 0u
#define BB_ST_PREPARING 1u
#define BB_ST_ASLEEP 2u
#define BB_ST_WAKING 3u
#define BB_ST_FAULT_SAFE 4u
#define BB_ST_BOOT_STAGE 0x80u

#define BB_ST_BOOT_IO (1u << 0)
#define BB_ST_BOOT_WIFI (1u << 1)
#define BB_ST_BOOT_P4 (1u << 2)
#define BB_ST_BOOT_DISPLAY (1u << 3)
#define BB_ST_BOOT_SETTINGS (1u << 4)
#define BB_ST_BOOT_C6_WIFI (1u << 5)

#define BB_ST_INH_SCRIPT (1u << 0)
#define BB_ST_INH_STREAM (1u << 1)
#define BB_ST_INH_TRIGGER (1u << 2)
#define BB_ST_INH_WAVEFORM (1u << 3)
#define BB_ST_INH_UART (1u << 4)
#define BB_ST_INH_BUS (1u << 5)
#define BB_ST_INH_OTA (1u << 6)
#define BB_ST_INH_CALIBRATION (1u << 7)
#define BB_ST_INH_SELFTEST (1u << 8)
#define BB_ST_INH_WORK (1u << 9)
#define BB_ST_INH_HOST (1u << 10)
#define BB_ST_INH_BATTSIM (1u << 11)
#define BB_ST_INH_SWD (1u << 12)
#define BB_ST_INH_HAT_UNKNOWN (1u << 13)
#define BB_ST_INH_CLOCK (1u << 14)

typedef struct __attribute__((packed)) {
    uint8_t schema;
    uint8_t op;
    uint8_t state;
    uint8_t stage;
    uint32_t generation;
    uint16_t timeout_seconds;
    uint16_t completed;
    uint16_t failed;
    uint16_t skipped;
} bb_standby_request_t;

typedef struct __attribute__((packed)) {
    uint8_t schema;
    uint8_t state;
    uint8_t ready;
    uint8_t failure;
    uint32_t generation;
    uint32_t inhibitors;
    uint32_t activity;
} bb_standby_reply_t;

#if defined(__cplusplus)
static_assert(sizeof(bb_standby_request_t) == 16, "standby request wire size");
static_assert(sizeof(bb_standby_reply_t) == 16, "standby reply wire size");
#else
_Static_assert(sizeof(bb_standby_request_t) == 16, "standby request wire size");
_Static_assert(sizeof(bb_standby_reply_t) == 16, "standby reply wire size");
#endif