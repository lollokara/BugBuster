// =============================================================================
// battsim_s1.c - see battsim_s1.h.
// =============================================================================

#include "battsim_s1.h"

// k-th oldest record of the ring.
static const bs_hist_rec_v2_t *at(const bs_hist_rec_v2_t *ring, uint32_t head, uint32_t count,
                                  uint32_t cap, uint32_t k)
{
    return &ring[(head + cap - count + k) % cap];
}

int bs_s1_since(const bs_hist_rec_v2_t *ring, uint32_t head, uint32_t count, uint32_t cap,
                uint32_t since_t_s, bs_s1_sample_t *out, int max, bool *more)
{
    if (more) *more = false;
    if (!ring || !out || max <= 0 || cap == 0 || count == 0) return 0;
    if (count > cap) count = cap;

    uint32_t k = 0;
    while (k < count && at(ring, head, count, cap, k)->t_s <= since_t_s) k++;

    int n = 0;
    for (; k < count && n < max; k++, n++) {
        const bs_hist_rec_v2_t *r = at(ring, head, count, cap, k);
        int64_t i_na;
        if (k > 0 && r->dt_s > 0) {
            const bs_hist_rec_v2_t *p = at(ring, head, count, cap, k - 1);
            i_na = (r->q_dut_nc - p->q_dut_nc) / (int64_t)r->dt_s;
        } else {
            i_na = ((int64_t)r->i_min_na + (int64_t)r->i_max_na) / 2;
        }
        out[n].t_s      = r->t_s;
        out[n].v_mv     = r->v_avg_mv;
        out[n].soc_x100 = r->soc_x100;
        out[n].i_ua     = (int32_t)(i_na / 1000);
        out[n].flags    = r->flags;
        out[n].dt_s     = r->dt_s;
    }
    if (more) *more = k < count;
    return n;
}
