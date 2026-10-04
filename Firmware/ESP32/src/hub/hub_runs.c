#include "hub_runs.h"
#include <math.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t  rdi32(const uint8_t *p) { return (int32_t)rd32(p); }
static int64_t  rdi64(const uint8_t *p) { return (int64_t)((uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32); }

bool hub_meta_parse(const uint8_t *d, size_t n, hub_meta_t *m)
{
    if (n < HUB_META_BYTES || rd32(d) != 0x4E525342u) return false;
    memset(m, 0, sizeof(*m));
    m->version = rd16(d + 4);
    m->run_id = rd16(d + 6);
    m->created_epoch = rd32(d + 8);
    memcpy(m->name, d + 12, 24);
    m->name[24] = '\0';
    const uint8_t *p = d + 36;
    m->chem = p[0]; m->cells = p[1];
    m->sd_enable = p[2] != 0; m->ext_enable = p[3] != 0; m->dither = p[4] != 0;
    m->capacity_mah = rd32(p + 8);
    m->start_soc_pct = rd16(p + 12) / 10.0f;
    m->cutoff_mv_cell = rd16(p + 14);
    m->rint_uohm = rd32(p + 16);
    m->peukert = rd16(p + 20) / 1000.0f;
    m->sd_pct_month = rd16(p + 22) / 100.0f;
    m->ext_ua = rd32(p + 24);
    return true;
}

bool hub_meta_json(hub_jw_t *w, const hub_meta_t *m, const char *state, int64_t ended_at)
{
    static const char *const CHEM[] = { "LiPo", "LiFePO4", "NiMH", "Lead-acid" };
    size_t mark = w->len;
    bool was_over = w->over;
    hub_jw_raw(w, "{\"name\":");
    hub_jw_str(w, m->name, strlen(m->name));
    hub_jw_fmt(w, ",\"state\":\"%s\",\"ended_at\":", state);
    if (ended_at > 0) hub_jw_fmt(w, "%lld", (long long)ended_at); else hub_jw_raw(w, "null");
    hub_jw_fmt(w, ",\"params\":{\"chem\":\"%s\",\"cells\":%u,\"capacity_mah\":%u,\"start_soc_pct\":%.1f,"
                  "\"cutoff_mv_cell\":%u,\"rint_uohm_cell\":%u,\"peukert\":%.3f,\"sd_enable\":%s,"
                  "\"sd_pct_month\":%.2f,\"ext_enable\":%s,\"ext_load_ua\":%u,\"dither\":%s}}",
               m->chem < 4 ? CHEM[m->chem] : "unknown", m->cells, (unsigned)m->capacity_mah,
               (double)m->start_soc_pct, m->cutoff_mv_cell, (unsigned)m->rint_uohm, (double)m->peukert,
               m->sd_enable ? "true" : "false", (double)m->sd_pct_month, m->ext_enable ? "true" : "false",
               (unsigned)m->ext_ua, m->dither ? "true" : "false");
    if (w->over && !was_over) { w->len = mark; w->over = false; w->p[mark] = '\0'; return false; }
    return !w->over;
}

const char *hub_events_final_state(const uint8_t *ev, size_t n, uint32_t *t_s)
{
    for (size_t off = (n / 16) * 16; off >= 16; off -= 16) {
        const uint8_t *e = ev + off - 16;
        uint16_t code = rd16(e + 4);
        const char *s = NULL;
        switch (code) {
        case 2: s = "active"; break;                /* BS_EV_START */
        case 3: case 6: s = "paused"; break;        /* PAUSE, REBOOT */
        case 4: s = "stopped"; break;               /* STOP */
        case 5: s = "depleted"; break;              /* DEPLETED */
        default: break;
        }
        if (s) { if (t_s) *t_s = rd32(e); return s; }
    }
    return NULL;
}

uint16_t hub_q15_res(const uint8_t *ckpt, size_t n)
{
    if (n < 352 || rd32(ckpt) != 0x4B435342u) return 900;
    uint32_t v = rd32(ckpt + 336);
    if (v == 0) return 900;
    return v > 3600 ? 3600 : (uint16_t)v;
}

bool hub_tier_of(uint16_t file_id, uint16_t q15_res, uint16_t *res, int *rank)
{
    if (file_id == 4) { *res = 1; *rank = 0; return true; }
    if (file_id >= 0x100) { *res = 60; *rank = 1; return true; }
    if (file_id == 3) { *res = q15_res; *rank = 2; return true; }
    return false;
}

size_t hub_rec_size(uint16_t version) { return version == 1 ? 32 : (version == 2 ? 48 : 0); }

size_t hub_rec_count(uint16_t version, size_t file_bytes)
{
    size_t sz = hub_rec_size(version);
    return sz ? file_bytes / sz : 0;
}

bool hub_rec_decode(uint16_t version, const uint8_t *p, hub_rec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->t = rd32(p);
    r->v = rd16(p + 4) / 1000.0f;
    r->vmin = rd16(p + 6) / 1000.0f;
    r->vmax = rd16(p + 8) / 1000.0f;
    r->soc = rd16(p + 10) / 100.0f;
    if (version == 1) {
        r->iavg_v1 = (float)(rdi32(p + 12) * 1e-9);
        r->imin = (float)(rdi32(p + 16) * 1e-9);
        r->imax = (float)(rdi32(p + 20) * 1e-9);
        return true;
    }
    if (version == 2) {
        r->imin = (float)(rdi32(p + 12) * 1e-9);
        r->imax = (float)(rdi32(p + 16) * 1e-9);
        r->dt = rd16(p + 22);
        r->q_dut_nc = rdi64(p + 24);
        r->has_q = true;
        return true;
    }
    return false;
}

void hub_rec_to_sample(const hub_rec_t *r, const hub_rec_t *prev, uint16_t version, uint32_t ts_unix,
                       uint16_t res, hub_sample_t *s)
{
    memset(s, 0, sizeof(*s));
    s->ts = ts_unix;
    s->v = r->v; s->soc = r->soc;
    s->state = -1;
    if (version == 1) {
        s->i = r->iavg_v1;
    } else if (r->has_q && r->dt > 0) {
        uint32_t start = r->t > r->dt ? r->t - r->dt : 0;
        if (prev && prev->has_q && (start > prev->t ? start - prev->t : prev->t - start) <= 1)
            s->i = (float)((r->q_dut_nc - prev->q_dut_nc) * 1e-9 / r->dt);
        else if (start == 0)
            s->i = (float)(r->q_dut_nc * 1e-9 / r->dt);
        else
            s->i = (r->imin + r->imax) / 2;
    } else {
        s->i = (r->imin + r->imax) / 2;
    }
    if (res > 1) {
        s->ext = true;
        s->vmin = r->vmin; s->vmax = r->vmax; s->imin = r->imin; s->imax = r->imax;
    }
}

bool hub_covered(const hub_range_t *ranges, size_t n, uint32_t ts, uint16_t res)
{
    for (size_t k = 0; k < n; k++) {
        if (ranges[k].res <= res && ranges[k].from < (double)ts && (double)ts <= ranges[k].to) return true;
    }
    return false;
}

bool hub_covered_src(const hub_range_t *ranges, size_t n, uint32_t ts, uint16_t res, hub_clk_src_t src)
{
    for (size_t k = 0; k < n; k++) {
        if (ranges[k].res <= res && ranges[k].clk_src <= src && ranges[k].from < (double)ts && (double)ts <= ranges[k].to) return true;
    }
    return false;
}

void hub_wallmap_init(hub_wallmap_t *m, uint32_t created_epoch)
{
    m->n = 0;
    m->created = created_epoch;
    if (created_epoch > 0) {
        m->created_src = HUB_CLK_P4_EPOCH;
        m->created_unc_ms = 200u;
    } else {
        m->created_src = HUB_CLK_EST;
        m->created_unc_ms = 3600000u;
    }
}

void hub_wallmap_set_created_clock(hub_wallmap_t *m, hub_clk_src_t src, uint32_t unc_ms)
{
    m->created_src = src;
    m->created_unc_ms = unc_ms;
}

void hub_wallmap_add_events(hub_wallmap_t *m, const uint8_t *ev, size_t n)
{
    for (size_t off = 0; off + 16 <= n && m->n < HUB_WALL_SEGS; off += 16) {
        uint32_t wall = rd32(ev + off + 12);
        if (rd16(ev + off + 4) == 2 && wall != 0) {            /* BS_EV_START with a known wall clock */
            m->seg[m->n].t_s = rd32(ev + off);
            m->seg[m->n].wall = wall;
            m->seg[m->n].src = HUB_CLK_P4_EPOCH;
            m->seg[m->n].unc_ms = 200u + (uint32_t)(((uint64_t)rd32(ev + off) * 50u) / 1000u);
            m->n++;
        }
    }
}

uint32_t hub_wallmap_unix(const hub_wallmap_t *m, uint32_t t_s)
{
    int pick = -1;
    for (uint16_t k = 0; k < m->n; k++) {
        if (m->seg[k].t_s < t_s) pick = k;
    }
    if (pick < 0) return m->created + t_s;                  /* before any START with a wall clock: the run's own epoch */
    return (uint32_t)((int64_t)m->seg[pick].wall + ((int64_t)t_s - (int64_t)m->seg[pick].t_s));
}

hub_clk_src_t hub_wallmap_src(const hub_wallmap_t *m, uint32_t t_s, uint32_t *unc_ms)
{
    int pick = -1;
    for (uint16_t k = 0; k < m->n; k++) {
        if (m->seg[k].t_s < t_s) pick = k;
    }
    if (pick < 0) {
        if (unc_ms) *unc_ms = m->created_unc_ms;
        return m->created_src;
    }
    if (unc_ms) *unc_ms = m->seg[pick].unc_ms;
    return m->seg[pick].src;
}

uint32_t hub_wallmap_run_time(const hub_wallmap_t *m, uint32_t unix_s)
{
    int pick = -1;
    for (uint16_t k = 0; k < m->n; k++) {
        if (m->seg[k].wall <= unix_s) pick = k;
    }
    if (pick < 0) return unix_s > m->created ? unix_s - m->created : 0;
    return m->seg[pick].t_s + (unix_s - m->seg[pick].wall);
}
