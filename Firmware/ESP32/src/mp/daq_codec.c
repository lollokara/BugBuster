// =============================================================================
// daq_codec.c — see daq_codec.h. Pure C, little-endian wire, host-tested.
// =============================================================================

#include "daq_codec.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return (int64_t)v;
}
static float rdf(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint8_t daqc_key_type(uint16_t key)
{
    switch (key) {
    case DAQC_K_BS_CHEM:         return DAQC_T_ENUM;
    case DAQC_K_BS_CELLS:        return DAQC_T_U8;
    case DAQC_K_BS_CAPACITY_MAH: return DAQC_T_U32;
    case DAQC_K_BS_START_SOC:    return DAQC_T_U16;
    case DAQC_K_BS_CUTOFF_MV:    return DAQC_T_U16;
    case DAQC_K_BS_RINT_UOHM:    return DAQC_T_U32;
    case DAQC_K_BS_PEUKERT:      return DAQC_T_U16;
    case DAQC_K_BS_SD_ENABLE:    return DAQC_T_BOOL;
    case DAQC_K_BS_SD_PCT:       return DAQC_T_U16;
    case DAQC_K_BS_EXT_ENABLE:   return DAQC_T_BOOL;
    case DAQC_K_BS_EXT_UA:       return DAQC_T_U32;
    case DAQC_K_BS_DITHER:       return DAQC_T_BOOL;
    case DAQC_K_BS_PROFILE_SLOT: return DAQC_T_U8;
    case DAQC_K_BS_RUN_SELECT:   return DAQC_T_U16;
    case DAQC_K_BS_NAME:         return DAQC_T_STR;
    default:                     return 0;
    }
}

size_t daqc_tlv_int(uint8_t *out, size_t cap, uint16_t key, int64_t value)
{
    uint8_t t = daqc_key_type(key);
    size_t vlen;
    uint64_t max;
    switch (t) {
    case DAQC_T_BOOL: vlen = 1; max = 1; value = value ? 1 : 0; break;
    case DAQC_T_U8:
    case DAQC_T_ENUM: vlen = 1; max = 0xFFu; break;
    case DAQC_T_U16:  vlen = 2; max = 0xFFFFu; break;
    case DAQC_T_U32:  vlen = 4; max = 0xFFFFFFFFull; break;
    default:          return 0;
    }
    if (!out || cap < 4 + vlen || value < 0 || (uint64_t)value > max) return 0;
    wr16(out, key);
    out[2] = t;
    out[3] = (uint8_t)vlen;
    for (size_t i = 0; i < vlen; i++) out[4 + i] = (uint8_t)((uint64_t)value >> (8 * i));
    return 4 + vlen;
}

size_t daqc_tlv_str(uint8_t *out, size_t cap, uint16_t key, const char *s)
{
    if (daqc_key_type(key) != DAQC_T_STR || !out || !s) return 0;
    size_t n = strlen(s);
    if (n > DAQC_NAME_MAX || cap < 4 + n) return 0;
    wr16(out, key);
    out[2] = DAQC_T_STR;
    out[3] = (uint8_t)n;
    memcpy(out + 4, s, n);
    return 4 + n;
}

bool daqc_bs_status_decode(const uint8_t *raw, size_t n, daqc_bs_status_t *o)
{
    if (!raw || !o || n < DAQC_STATUS_SIZE) return false;
    memset(o, 0, sizeof(*o));
    o->version      = raw[0];
    o->state        = raw[1];
    o->flags        = raw[2];
    o->last_error   = raw[3];
    o->run_id       = rd16(raw + 4);
    o->chem         = raw[6];
    o->cells        = raw[7];
    o->capacity_mah = rd32(raw + 8);
    o->soc_pct      = (float)rd16(raw + 12) / 100.0f;
    o->profile_mask = rd16(raw + 14);
    o->elapsed_s    = rd32(raw + 16);
    o->remaining_s  = (o->flags & DAQC_FLAG_REMAIN_OK) ? (int64_t)rd32(raw + 20) : -1;
    o->v_target     = rdf(raw + 24);
    o->v_meas       = rdf(raw + 28);
    o->i_meas       = rdf(raw + 32);
    o->i_avg        = rdf(raw + 36);
    o->q_used_c     = (double)rd64(raw + 40) * 1e-9;
    o->q_dut_c      = (double)rd64(raw + 48) * 1e-9;
    o->fs_total     = rd32(raw + 80);
    o->fs_used      = rd32(raw + 84);
    o->has_energy   = n >= 96;
    o->e_dut_j      = o->has_energy ? (double)rd64(raw + 88) * 1e-6 : 0.0;
    return true;
}

bool daqc_bs_meta_decode(const uint8_t *raw, size_t n, daqc_bs_meta_t *o)
{
    if (!raw || !o || n < DAQC_META_SIZE || rd32(raw) != DAQC_META_MAGIC) return false;
    memset(o, 0, sizeof(*o));
    o->version       = rd16(raw + 4);
    o->run_id        = rd16(raw + 6);
    o->created_epoch = rd32(raw + 8);
    memcpy(o->name, raw + 12, 24);
    o->name[DAQC_NAME_MAX + 1] = '\0';
    const uint8_t *p = raw + 36;
    o->params.chem           = p[0];
    o->params.cells          = p[1];
    o->params.sd_enable      = p[2] != 0;
    o->params.ext_enable     = p[3] != 0;
    o->params.dither         = p[4] != 0;
    o->params.capacity_mah   = rd32(p + 8);
    o->params.start_soc_pct  = (float)rd16(p + 12) / 10.0f;
    o->params.cutoff_mv_cell = rd16(p + 14);
    o->params.rint_uohm_cell = rd32(p + 16);
    o->params.peukert        = (float)rd16(p + 20) / 1000.0f;
    o->params.sd_pct_month   = (float)rd16(p + 22) / 100.0f;
    o->params.ext_load_ua    = rd32(p + 24);
    return true;
}

size_t daqc_list_request(uint8_t *out, uint16_t start)
{
    out[0] = DAQC_BS_OP_LIST_RUNS;
    wr16(out + 1, start);
    return 3;
}

bool daqc_list_decode(const uint8_t *raw, size_t n, uint16_t *total, uint16_t *active,
                      uint16_t *ids, int max, int *count)
{
    if (!raw || n < 4) return false;
    if (total) *total = rd16(raw);
    if (active) *active = rd16(raw + 2);
    int c = 0;
    for (size_t off = 4; off + 2 <= n && c < max; off += 2) ids[c++] = rd16(raw + off);
    if (count) *count = c;
    return true;
}

size_t daqc_read_request(uint8_t *out, uint16_t run, uint16_t file, uint32_t off, uint8_t len)
{
    out[0] = DAQC_BS_OP_READ;
    wr16(out + 1, run);
    wr16(out + 3, file);
    wr32(out + 5, off);
    out[9] = len;
    return 10;
}

size_t daqc_s1_request(uint8_t *out, uint16_t run, uint32_t since_t_s, uint8_t max)
{
    out[0] = DAQC_BS_OP_S1_SINCE;
    wr16(out + 1, run);
    wr32(out + 3, since_t_s);
    out[7] = max;
    return 8;
}

int daqc_s1_decode(const uint8_t *raw, size_t n, uint16_t *run_id, daqc_s1_t *out, int max,
                   bool *more)
{
    if (!raw || n < 4) return -1;
    uint8_t cnt = raw[2];
    if (n < 4 + (size_t)cnt * DAQC_S1_SIZE) return -1;
    if (run_id) *run_id = rd16(raw);
    if (more) *more = raw[3] != 0;
    int k = 0;
    for (; k < (int)cnt && k < max; k++) {
        const uint8_t *p = raw + 4 + (size_t)k * DAQC_S1_SIZE;
        out[k].t_s     = rd32(p);
        out[k].v       = (float)rd16(p + 4) / 1000.0f;
        out[k].soc_pct = (float)rd16(p + 6) / 100.0f;
        out[k].i       = (float)(int32_t)rd32(p + 8) * 1e-6f;
        out[k].flags   = rd16(p + 12);
    }
    return k;
}

static const char *const k_chem[] = { "lipo", "lifepo4", "nimh", "lead" };
static const char *const k_state[] = { "none", "paused", "active", "depleted", "stopped" };
static const char *const k_err[] = { "none", "no store", "no run", "busy", "invalid", "state",
                                     "no PD contract", "acquisition not running", "I/O error",
                                     "not found" };

int daqc_chem_from_name(const char *s)
{
    if (!s) return -1;
    for (int i = 0; i < 4; i++) {
        const char *a = s, *b = k_chem[i];
        while (*a && *b) {
            char c = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
            if (c != *b) break;
            a++; b++;
        }
        if (*a == '\0' && *b == '\0') return i;
    }
    return -1;
}

const char *daqc_chem_name(uint8_t chem)  { return chem < 4 ? k_chem[chem] : "unknown"; }
const char *daqc_state_name(uint8_t st)   { return st < 5 ? k_state[st] : "unknown"; }
const char *daqc_error_name(uint8_t err)  { return err < 10 ? k_err[err] : "unknown"; }
