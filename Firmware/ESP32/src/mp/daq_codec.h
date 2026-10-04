#pragma once
// =============================================================================
// daq_codec.h — pure-C wire codecs for the MicroPython `daq` module
// (spec 2026-10-03 §3): DAQ settings TLVs (HAT cmds 0x70..0x74) and the
// battery-simulator HAT_CMD_BS replies. Layouts mirror the reference host
// decoder python/bugbuster/battsim.py; tests/firmware_host/test_daq_codec.py
// cross-checks both and derives the constants from the P4 sources.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// TLV value types (daq_config_registry.h daq_type_t).
enum { DAQC_T_BOOL = 1, DAQC_T_U8 = 2, DAQC_T_U16 = 4, DAQC_T_U32 = 6, DAQC_T_ENUM = 9, DAQC_T_STR = 10 };

// Battery-simulator keys (DAQ_KEY(DAQ_GRP_BATT, idx)).
enum {
    DAQC_K_BS_CHEM         = 0x0801,
    DAQC_K_BS_CELLS        = 0x0802,
    DAQC_K_BS_CAPACITY_MAH = 0x0803,
    DAQC_K_BS_START_SOC    = 0x0804,
    DAQC_K_BS_CUTOFF_MV    = 0x0805,
    DAQC_K_BS_RINT_UOHM    = 0x0806,
    DAQC_K_BS_PEUKERT      = 0x0807,
    DAQC_K_BS_SD_ENABLE    = 0x0808,
    DAQC_K_BS_SD_PCT       = 0x0809,
    DAQC_K_BS_EXT_ENABLE   = 0x080A,
    DAQC_K_BS_EXT_UA       = 0x080B,
    DAQC_K_BS_DITHER       = 0x080C,
    DAQC_K_BS_PROFILE_SLOT = 0x080D,
    DAQC_K_BS_RUN_SELECT   = 0x080E,
    DAQC_K_BS_NAME         = 0x080F,
};

// CONFIG_ACTION ids.
enum {
    DAQC_ACT_BS_RUN_NEW    = 7,
    DAQC_ACT_BS_RUN_START  = 8,
    DAQC_ACT_BS_RUN_PAUSE  = 9,
    DAQC_ACT_BS_RUN_STOP   = 10,
    DAQC_ACT_BS_RUN_UNLOAD = 11,
    DAQC_ACT_BS_RUN_LOAD   = 12,
    DAQC_ACT_BS_RUN_DELETE = 13,
    DAQC_ACT_BS_RUN_REOPEN = 15,   // 14 = BS_DEFAULTS (not used by the S3 codec)
};

#define DAQC_CFG_CMD_SET    0x71u   // HAT cmd 0x70 + SET
#define DAQC_CFG_CMD_ACTION 0x74u   // HAT cmd 0x70 + ACTION

// HAT_CMD_BS ops (battsim_host.h).
enum {
    DAQC_BS_OP_STATUS    = 0,
    DAQC_BS_OP_LIST_RUNS = 1,
    DAQC_BS_OP_RUN_DIR   = 2,
    DAQC_BS_OP_READ      = 3,
    DAQC_BS_OP_PROFILE   = 4,
    DAQC_BS_OP_SET_EPOCH = 5,
    DAQC_BS_OP_S1_SINCE  = 6,
};

#define DAQC_BS_FILE_META    0
#define DAQC_NAME_MAX        23
#define DAQC_STATUS_SIZE     88
#define DAQC_META_SIZE       68
#define DAQC_META_MAGIC      0x4E525342u
#define DAQC_FLAG_REMAIN_OK  0x40u
#define DAQC_S1_SIZE         16
#define DAQC_S1_MAX          14

typedef struct {
    uint8_t  version, state, flags, last_error;
    uint16_t run_id;
    uint8_t  chem, cells;
    uint32_t capacity_mah;
    float    soc_pct;
    uint16_t profile_mask;
    uint32_t elapsed_s;
    int64_t  remaining_s;        // -1 when unknown
    float    v_target, v_meas, i_meas, i_avg;
    double   q_used_c, q_dut_c;
    uint32_t fs_total, fs_used;
    bool     has_energy;
    double   e_dut_j;
} daqc_bs_status_t;

typedef struct {
    uint8_t  chem, cells;
    bool     sd_enable, ext_enable, dither;
    uint32_t capacity_mah;
    float    start_soc_pct;
    uint16_t cutoff_mv_cell;
    uint32_t rint_uohm_cell;
    float    peukert;
    float    sd_pct_month;
    uint32_t ext_load_ua;
} daqc_bs_params_t;

typedef struct {
    uint16_t         run_id, version;
    uint32_t         created_epoch;
    char             name[DAQC_NAME_MAX + 2];
    daqc_bs_params_t params;
} daqc_bs_meta_t;

typedef struct {
    uint32_t t_s;
    float    v;        // V
    float    i;        // A
    float    soc_pct;
    uint16_t flags;    // BS_RF_*
} daqc_s1_t;

uint8_t daqc_key_type(uint16_t key);                                              // 0 = unknown
size_t  daqc_tlv_int(uint8_t *out, size_t cap, uint16_t key, int64_t value);      // 0 = refused
size_t  daqc_tlv_str(uint8_t *out, size_t cap, uint16_t key, const char *s);      // 0 = refused

bool daqc_bs_status_decode(const uint8_t *raw, size_t n, daqc_bs_status_t *out);
bool daqc_bs_meta_decode(const uint8_t *raw, size_t n, daqc_bs_meta_t *out);

size_t daqc_list_request(uint8_t *out, uint16_t start);                            // 3 B
bool   daqc_list_decode(const uint8_t *raw, size_t n, uint16_t *total, uint16_t *active,
                        uint16_t *ids, int max, int *count);
size_t daqc_read_request(uint8_t *out, uint16_t run, uint16_t file, uint32_t off, uint8_t len); // 10 B
size_t daqc_s1_request(uint8_t *out, uint16_t run, uint32_t since_t_s, uint8_t max);           // 8 B
int    daqc_s1_decode(const uint8_t *raw, size_t n, uint16_t *run_id, daqc_s1_t *out, int max,
                      bool *more);                                                 // -1 = malformed

int         daqc_chem_from_name(const char *s);  // lipo/lifepo4/nimh/lead (any case), -1 unknown
const char *daqc_chem_name(uint8_t chem);
const char *daqc_state_name(uint8_t state);       // none/paused/active/depleted/stopped
const char *daqc_error_name(uint8_t err);         // battsim_err_t text

#ifdef __cplusplus
}
#endif
