#pragma once

// =============================================================================
// battsim_host.h - host read access to the battery simulator over the S3 link
// (HATP_CMD_BS). Requests are served by a low-priority worker so LittleFS work
// never runs on the s3_link task (prio 14, 4 KB stack).
//
// Request: [op u8][args]. Reply (HATP_RSP_BS_DATA): op-specific, <= 240 B.
//   BS_HOP_STATUS    -                                  -> battsim_status_t
//   BS_HOP_LIST_RUNS u16 start                          -> u16 total, u16 active (0 = none),
//                                                          u16 ids[] (<= BS_HOST_IDS_MAX)
//   BS_HOP_RUN_DIR   u16 run, u8 start                  -> u16 run, u8 total, u8 start,
//                                                          {u16 file_id, u32 size}[] (<= 38)
//   BS_HOP_READ      u16 run, u16 file, u32 off, u8 len -> raw bytes (0 = EOF)
//   BS_HOP_PROFILE   u8 slot                            -> char name[24], bs_params_t
//   BS_HOP_SET_EPOCH u32 unix_s                         -> empty
//   BS_HOP_S1_SINCE  u16 run, u32 since_t_s, u8 max      -> u16 run, u8 n, u8 more,
//                                                          bs_s1_sample_t[n] (n <= 14, loaded run only)
// Any failure (bad args, missing run/file, busy) -> RSP_ERROR.
// =============================================================================

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BS_HOP_STATUS    = 0,
    BS_HOP_LIST_RUNS = 1,
    BS_HOP_RUN_DIR   = 2,
    BS_HOP_READ      = 3,
    BS_HOP_PROFILE   = 4,
    BS_HOP_SET_EPOCH = 5,
    BS_HOP_S1_SINCE  = 6,
};

#define BS_HOST_IDS_MAX   100u
#define BS_HOST_DIR_MAX   38u
#define BS_HOST_READ_MAX  236u

/** Serve one host request. Returns the reply length, or -1 for RSP_ERROR. */
int battsim_host_handle(const uint8_t *req, uint8_t len, uint8_t *resp, size_t cap);

#ifdef __cplusplus
}
#endif
