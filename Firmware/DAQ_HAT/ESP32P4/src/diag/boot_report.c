#include "boot_report.h"
#include <stdio.h>
#include <string.h>

static const char *const k_names[] = {
    "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT", "WDT", "DEEPSLEEP",
    "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE", "PWR_GLITCH", "CPU_LOCKUP",
};
#define N_NAMES (sizeof k_names / sizeof k_names[0])

const char *br_reset_name(int reason)
{
    return (reason >= 0 && (size_t)reason < N_NAMES) ? k_names[reason] : "UNKNOWN";
}

int br_reset_abnormal(int reason)
{
    switch (reason) {
    case 4: case 5: case 6: case 7: case 9: case 14: case 15: return 1;
    default: return 0;
    }
}

static int crumb_ok(const br_crumb_t *c) { return c->magic == (BR_CRUMB_MAGIC ^ c->boot); }

int br_crumb_boot(br_crumb_t *c, br_crumb_t *prev_out)
{
    int valid = crumb_ok(c);
    if (prev_out) *prev_out = *c;
    uint32_t boot = valid ? c->boot + 1u : 1u;
    memset(c, 0, sizeof *c);
    c->boot = boot;
    c->magic = BR_CRUMB_MAGIC ^ boot;
    return valid;
}

void br_crumb_begin(br_crumb_t *c, uint8_t act, uint16_t run, uint32_t up_ms)
{
    c->last_act = act;
    c->run = run;
    c->up_ms = up_ms;
    c->inflight = 1;
}

void br_crumb_end(br_crumb_t *c, uint32_t up_ms)
{
    c->up_ms = up_ms;
    c->inflight = 0;
}

static size_t fin(int n, size_t cap) { return (n < 0) ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n); }

size_t br_fmt_reset(char *out, size_t cap, int reason, uint32_t boot)
{
    return fin(snprintf(out, cap, "reset=%s code=%d abnormal=%d boot=%u", br_reset_name(reason), reason,
                        br_reset_abnormal(reason), (unsigned)boot), cap);
}

size_t br_fmt_crumb(char *out, size_t cap, const br_crumb_t *p)
{
    return fin(snprintf(out, cap, "last_act=%u run=%u inflight=%u up_ms=%u", (unsigned)p->last_act,
                        (unsigned)p->run, (unsigned)p->inflight, (unsigned)p->up_ms), cap);
}

size_t br_fmt_crash(char *out, size_t cap, const char *task, uint32_t pc, uint32_t ra)
{
    return fin(snprintf(out, cap, "task=%.15s pc=0x%08x ra=0x%08x", task ? task : "?",
                        (unsigned)pc, (unsigned)ra), cap);
}

size_t br_fmt_fault(char *out, size_t cap, uint32_t sp, uint32_t cause, uint32_t tval)
{
    return fin(snprintf(out, cap, "sp=0x%08x cause=0x%x tval=0x%08x", (unsigned)sp, (unsigned)cause,
                        (unsigned)tval), cap);
}

size_t br_fmt_elf(char *out, size_t cap, const char *sha_hex, uint32_t depth)
{
    return fin(snprintf(out, cap, "elf=%.9s depth=%u", sha_hex ? sha_hex : "?", (unsigned)depth), cap);
}

size_t br_fmt_why(char *out, size_t cap, const char *reason)
{
    return fin(snprintf(out, cap, "why=%.70s", reason ? reason : "?"), cap);
}

size_t br_scan_stack(const uint8_t *dump, size_t size, uint32_t *out, size_t max)
{
    size_t n = 0;
    for (size_t o = 0; o + 4 <= size && n < max; o += 4) {
        uint32_t w = (uint32_t)dump[o] | (uint32_t)dump[o + 1] << 8 | (uint32_t)dump[o + 2] << 16 |
                     (uint32_t)dump[o + 3] << 24;
        if (w < 0x40000000u || w > 0x4fffffffu) continue;
        int dup = 0;
        for (size_t k = 0; k < n; k++) if (out[k] == w) { dup = 1; break; }
        if (!dup) out[n++] = w;
    }
    return n;
}

size_t br_fmt_bt(char *out, size_t cap, const uint32_t *bt, uint32_t depth, uint32_t *idx, uint32_t line)
{
    if (*idx >= depth) return 0;
    size_t n = (size_t)snprintf(out, cap, "bt%u=", (unsigned)line);
    for (int k = 0; k < 5 && *idx < depth; k++, (*idx)++) {
        int w = snprintf(out + n, cap - n, "%s0x%08x", k ? "," : "", (unsigned)bt[*idx]);
        if (w < 0 || n + (size_t)w >= cap) break;
        n += (size_t)w;
    }
    return n;
}
