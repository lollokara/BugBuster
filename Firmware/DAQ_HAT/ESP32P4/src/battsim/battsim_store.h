#pragma once

// =============================================================================
// battsim_store.h - battery simulator persistence on the `battlog` LittleFS.
//
// Layout (mounted at /bs):
//   /bs/p00.bin .. p15.bin   battery profiles (bs_profile_file_t)
//   /bs/active               id of the loaded run (text), absent if none
//   /bs/rNNNNN/meta.bin      bs_run_meta_t (rewritten when live params change)
//   /bs/rNNNNN/ck_a, ck_b    bs_ckpt_t ping-pong, newest valid seq wins
//   /bs/rNNNNN/ev.bin        bs_event_t, append-only
//   /bs/rNNNNN/s1.bin        last hour at 1 s (dumped from PSRAM on pause/stop)
//   /bs/rNNNNN/mDDDD.bin     1-min records for simulated day DDDD, last 7 kept
//   /bs/rNNNNN/q15.bin       whole-run records, 15 min doubling on compaction
//
// All structs are little-endian, packed and versioned; hosts decode them from
// the field lists here (python/bugbuster/battsim.py mirrors them).
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "battsim_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BS_MAX_PROFILES   16
#define BS_NAME_LEN       24
#define BS_M1_KEEP_DAYS   7
#define BS_Q15_MAX_RECS   8192u
#define BS_FMT_VERSION    1u

#define BS_MAGIC_PROFILE  0x46505342u   // "BSPF"
#define BS_MAGIC_META     0x4E525342u   // "BSRN"
#define BS_MAGIC_CKPT     0x4B435342u   // "BSCK"

typedef struct __attribute__((packed)) {
    uint32_t    magic;
    uint16_t    version;
    uint16_t    rsv;
    char        name[BS_NAME_LEN];
    bs_params_t params;
    uint32_t    crc;           // CRC32 over everything before it
} bs_profile_file_t;

// One history record (32 B). t_s is the END of the interval, simulated time.
typedef struct __attribute__((packed)) {
    uint32_t t_s;
    uint16_t v_avg_mv, v_min_mv, v_max_mv;
    uint16_t soc_x100;         // 0..10000 = 0..100.00 %
    int32_t  i_avg_na, i_min_na, i_max_na;   // DUT current (measured)
    int64_t  q_used_nc;        // cumulative charge removed (all sources)
} bs_hist_rec_t;
_Static_assert(sizeof(bs_hist_rec_t) == 32, "bs_hist_rec_t wire size");

typedef enum {
    BS_EV_CREATED = 1,  BS_EV_START = 2,    BS_EV_PAUSE = 3,   BS_EV_STOP = 4,
    BS_EV_DEPLETED = 5, BS_EV_REBOOT = 6,   BS_EV_PARAM = 7,   BS_EV_STALL = 8,
    BS_EV_OUTPUT_OFF = 9, BS_EV_PD_LOST = 10, BS_EV_STORE_ERR = 11,
} bs_event_code_t;

typedef struct __attribute__((packed)) {
    uint32_t t_s;
    uint16_t code;             // bs_event_code_t
    uint16_t rsv;
    int32_t  a, b;             // code-specific (BS_EV_PARAM: key, new value)
} bs_event_t;
_Static_assert(sizeof(bs_event_t) == 16, "bs_event_t wire size");

// Not packed: every field is naturally aligned (explicit rsv fields, static
// size asserts), so pointers into it are safe and the layout is still fixed.
typedef struct {
    uint32_t    magic;
    uint16_t    version;
    uint16_t    run_id;
    uint32_t    created_epoch;  // host wall clock at creation, 0 = unknown
    char        name[BS_NAME_LEN];
    bs_params_t params;         // live copy (live-editable fields may change)
    uint32_t    crc;
} bs_run_meta_t;
_Static_assert(sizeof(bs_run_meta_t) == 68, "bs_run_meta_t wire size");

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  state;            // battsim_state_t
    uint8_t  rsv;
    uint32_t seq;
    uint32_t rsv2;
    int64_t  q_dut_pc;         // integrated DUT charge
    int64_t  ticks;            // simulated time, MCLK ticks
    int64_t  q_ext_pc;         // virtual external load
    int64_t  q_sd_pc;          // self-discharge
    int64_t  q_peuk_pc;        // Peukert surcharge
    int64_t  q_init_pc;        // charge already missing at start (1 - SOC0)
    int64_t  ext_rem;          // ext-load remainder (nA*ticks, < 16384)
    double   sd_frac;          // self-discharge fractional pC carry
    double   peuk_frac;        // Peukert fractional pC carry
    int64_t  win_q_pc[30];     // 1-min buckets of total charge (remaining time)
    uint32_t win_head, win_count;
    uint32_t q15_interval_s;   // grows by doubling on compaction
    uint32_t q15_count;
    uint32_t crc;
    uint32_t rsv3;
} bs_ckpt_t;
_Static_assert(sizeof(bs_ckpt_t) == 352, "bs_ckpt_t wire size");

typedef enum {
    BS_FILE_META = 0, BS_FILE_CKPT = 1, BS_FILE_EVENTS = 2, BS_FILE_Q15 = 3,
    BS_FILE_S1 = 4,
    BS_FILE_M1_BASE = 0x100,   // + simulated day number
} bs_file_id_t;

bool bs_store_init(void);
bool bs_store_available(void);
void bs_store_usage(uint32_t *total, uint32_t *used);

bool bs_store_profile_save(uint8_t slot, const char *name, const bs_params_t *p);
bool bs_store_profile_load(uint8_t slot, char *name, bs_params_t *p);
uint16_t bs_store_profile_mask(void);   // bit n = slot n populated
bool bs_store_profile_delete(uint8_t slot);

bool bs_store_run_create(bs_run_meta_t *meta);   // assigns meta->run_id
bool bs_store_meta_write(const bs_run_meta_t *meta);
bool bs_store_meta_read(uint16_t run_id, bs_run_meta_t *meta);
bool bs_store_ckpt_write(uint16_t run_id, bs_ckpt_t *ck);   // sets seq + crc
bool bs_store_ckpt_read(uint16_t run_id, bs_ckpt_t *ck);
bool bs_store_event(uint16_t run_id, const bs_event_t *ev);
bool bs_store_m1_append(uint16_t run_id, const bs_hist_rec_t *r);
bool bs_store_q15_append(uint16_t run_id, const bs_hist_rec_t *r, bs_ckpt_t *ck);
bool bs_store_s1_write(uint16_t run_id, const bs_hist_rec_t *recs, size_t n);

int  bs_store_list_runs(uint16_t *ids, int max);
bool bs_store_delete_run(uint16_t run_id);
bool bs_store_active_get(uint16_t *run_id);
bool bs_store_active_set(uint16_t run_id);
void bs_store_active_clear(void);

// Host access: size of / chunk read from one run file. -1 if absent.
int32_t bs_store_file_size(uint16_t run_id, uint16_t file_id);
int32_t bs_store_file_read(uint16_t run_id, uint16_t file_id, uint32_t offset,
                           uint8_t *buf, uint32_t len);
// 1-min day files present for a run, ascending (returns count).
int  bs_store_m1_days(uint16_t run_id, uint16_t *days, int max);

#ifdef __cplusplus
}
#endif
