// =============================================================================
// battsim_store.c - LittleFS persistence for the battery simulator.
// =============================================================================

#include "battsim_store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_littlefs.h"

static const char *TAG = "bs_store";

#define BASE   "/bs"
#define LABEL  "battlog"

static bool s_ok;

static uint32_t crc_of(const void *p, size_t len)
{
    return esp_rom_crc32_le(0, (const uint8_t *)p, (uint32_t)len);
}

static void run_dir(uint16_t id, char *out, size_t cap)
{
    snprintf(out, cap, BASE "/r%05u", (unsigned)id);
}

static void run_path(uint16_t id, const char *leaf, char *out, size_t cap)
{
    snprintf(out, cap, BASE "/r%05u/%s", (unsigned)id, leaf);
}

// Whole-file replace. LittleFS commits a file atomically on close, so a power
// cut leaves either the old or the new contents.
static bool write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

static bool append_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "ab");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

static bool read_file(const char *path, void *data, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

bool bs_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = BASE,
        .partition_label = LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        // ESP_ERR_NOT_FOUND = the board still has the pre-battlog partition
        // table; battery simulation reports UNAVAILABLE until a wired flash.
        ESP_LOGW(TAG, "battlog mount failed: %s", esp_err_to_name(err));
        s_ok = false;
        return false;
    }
    s_ok = true;
    uint32_t total = 0, used = 0;
    bs_store_usage(&total, &used);
    ESP_LOGI(TAG, "battlog mounted: %u / %u KiB used",
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return true;
}

bool bs_store_available(void) { return s_ok; }

void bs_store_usage(uint32_t *total, uint32_t *used)
{
    size_t t = 0, u = 0;
    if (s_ok) esp_littlefs_info(LABEL, &t, &u);
    if (total) *total = (uint32_t)t;
    if (used)  *used  = (uint32_t)u;
}

// ---------------------------------------------------------------------------
// Profiles.
// ---------------------------------------------------------------------------
static void profile_path(uint8_t slot, char *out, size_t cap)
{
    snprintf(out, cap, BASE "/p%02u.bin", (unsigned)slot);
}

bool bs_store_profile_save(uint8_t slot, const char *name, const bs_params_t *p)
{
    if (!s_ok || slot >= BS_MAX_PROFILES) return false;
    bs_profile_file_t f = { .magic = BS_MAGIC_PROFILE, .version = BS_PROFILE_VERSION };
    if (name) strncpy(f.name, name, BS_NAME_LEN - 1);
    f.params = *p;
    f.crc = crc_of(&f, offsetof(bs_profile_file_t, crc));
    char path[32];
    profile_path(slot, path, sizeof(path));
    return write_file(path, &f, sizeof(f));
}

bool bs_store_profile_load(uint8_t slot, char *name, bs_params_t *p)
{
    if (!s_ok || slot >= BS_MAX_PROFILES) return false;
    bs_profile_file_t f;
    char path[32];
    profile_path(slot, path, sizeof(path));
    if (!read_file(path, &f, sizeof(f))) return false;
    if (f.magic != BS_MAGIC_PROFILE ||
        f.crc != crc_of(&f, offsetof(bs_profile_file_t, crc))) return false;
    if (name) { memcpy(name, f.name, BS_NAME_LEN); name[BS_NAME_LEN - 1] = '\0'; }
    if (p) *p = f.params;
    return true;
}

uint16_t bs_store_profile_mask(void)
{
    uint16_t m = 0;
    for (uint8_t i = 0; i < BS_MAX_PROFILES; i++) {
        if (bs_store_profile_load(i, NULL, NULL)) m |= (uint16_t)(1u << i);
    }
    return m;
}

bool bs_store_profile_delete(uint8_t slot)
{
    if (!s_ok || slot >= BS_MAX_PROFILES) return false;
    char path[32];
    profile_path(slot, path, sizeof(path));
    return unlink(path) == 0;
}

// ---------------------------------------------------------------------------
// Runs.
// ---------------------------------------------------------------------------
int bs_store_list_runs(uint16_t *ids, int max)
{
    if (!s_ok) return 0;
    DIR *d = opendir(BASE);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        unsigned id;
        if (e->d_name[0] != 'r' || sscanf(e->d_name + 1, "%5u", &id) != 1) continue;
        if (n < max && ids) ids[n] = (uint16_t)id;
        n++;
    }
    closedir(d);
    if (ids) {   // ascending, small n: insertion sort
        int lim = n < max ? n : max;
        for (int i = 1; i < lim; i++) {
            uint16_t v = ids[i]; int j = i - 1;
            while (j >= 0 && ids[j] > v) { ids[j + 1] = ids[j]; j--; }
            ids[j + 1] = v;
        }
    }
    return n;
}

bool bs_store_meta_write(const bs_run_meta_t *meta)
{
    if (!s_ok) return false;
    bs_run_meta_t m = *meta;
    m.magic = BS_MAGIC_META;
    // The format is fixed at creation: a v1 run stays v1 when rewritten.
    if (m.version == 0) m.version = BS_FMT_VERSION;
    m.crc = crc_of(&m, offsetof(bs_run_meta_t, crc));
    char path[40];
    run_path(m.run_id, "meta.bin", path, sizeof(path));
    return write_file(path, &m, sizeof(m));
}

bool bs_store_run_create(bs_run_meta_t *meta)
{
    if (!s_ok) return false;
    uint16_t ids[64];
    int n = bs_store_list_runs(ids, 64);
    uint16_t next = 1;
    if (n > 64) {
        // More runs than the scratch list: scan for the true maximum.
        uint16_t *all = malloc(sizeof(uint16_t) * (size_t)n);
        if (!all) return false;
        int m = bs_store_list_runs(all, n);
        next = (uint16_t)(all[m - 1] + 1);
        free(all);
    } else if (n > 0) {
        next = (uint16_t)(ids[n - 1] + 1);
    }
    if (next == 0) return false;   // id space exhausted (65535 runs)
    char dir[24];
    run_dir(next, dir, sizeof(dir));
    if (mkdir(dir, 0775) != 0) return false;
    meta->run_id = next;
    meta->version = BS_FMT_VERSION;
    return bs_store_meta_write(meta);
}

bool bs_store_meta_read(uint16_t run_id, bs_run_meta_t *meta)
{
    if (!s_ok) return false;
    char path[40];
    run_path(run_id, "meta.bin", path, sizeof(path));
    if (!read_file(path, meta, sizeof(*meta))) return false;
    return meta->magic == BS_MAGIC_META &&
           meta->crc == crc_of(meta, offsetof(bs_run_meta_t, crc));
}

// Reads a v2 checkpoint, or a 352 B v1 one (energy fields come back as 0).
static bool ckpt_load(const char *path, bs_ckpt_t *ck)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(ck, 1, sizeof(*ck), f);
    fclose(f);
    if (n == sizeof(bs_ckpt_t)) {
        return ck->magic == BS_MAGIC_CKPT &&
               ck->crc == crc_of(ck, offsetof(bs_ckpt_t, crc));
    }
    if (n != BS_CKPT_V1_SIZE) return false;
    const size_t crc_off = BS_CKPT_V1_SIZE - 8;
    uint32_t crc;
    memcpy(&crc, (const uint8_t *)ck + crc_off, sizeof(crc));
    if (ck->magic != BS_MAGIC_CKPT || crc != crc_of(ck, crc_off)) return false;
    memset((uint8_t *)ck + crc_off, 0, sizeof(*ck) - crc_off);
    return true;
}

// Newest valid checkpoint: 'a' for ck_a, 'b' for ck_b, 0 if neither.
static char ckpt_pick(uint16_t run_id, bs_ckpt_t *out)
{
    bs_ckpt_t a, b;
    char pa[40], pb[40];
    run_path(run_id, "ck_a", pa, sizeof(pa));
    run_path(run_id, "ck_b", pb, sizeof(pb));
    bool va = ckpt_load(pa, &a);
    bool vb = ckpt_load(pb, &b);
    if (!va && !vb) return 0;
    bool use_a = va && (!vb || (int32_t)(a.seq - b.seq) > 0);
    if (out) *out = use_a ? a : b;
    return use_a ? 'a' : 'b';
}

bool bs_store_ckpt_read(uint16_t run_id, bs_ckpt_t *ck)
{
    if (!s_ok) return false;
    return ckpt_pick(run_id, ck) != 0;
}

bool bs_store_ckpt_write(uint16_t run_id, bs_ckpt_t *ck)
{
    if (!s_ok) return false;
    ck->magic = BS_MAGIC_CKPT;
    ck->version = BS_CKPT_VERSION;
    ck->seq++;
    ck->crc = crc_of(ck, offsetof(bs_ckpt_t, crc));
    char path[40];
    // Odd sequence numbers go to A, even to B: the previous one always survives.
    run_path(run_id, (ck->seq & 1u) ? "ck_a" : "ck_b", path, sizeof(path));
    return write_file(path, ck, sizeof(*ck));
}

bool bs_store_event(uint16_t run_id, const bs_event_t *ev)
{
    if (!s_ok) return false;
    char path[40];
    run_path(run_id, "ev.bin", path, sizeof(path));
    return append_file(path, ev, sizeof(*ev));
}

bool bs_store_m1_append(uint16_t run_id, const bs_hist_rec_v2_t *r)
{
    if (!s_ok) return false;
    uint32_t day = r->t_s / 86400u;
    char leaf[16], path[40];
    snprintf(leaf, sizeof(leaf), "m%04u.bin", (unsigned)day);
    run_path(run_id, leaf, path, sizeof(path));
    struct stat st;
    bool new_day = stat(path, &st) != 0;
    if (!append_file(path, r, sizeof(*r))) return false;
    if (new_day && day >= BS_M1_KEEP_DAYS) {
        // Drop every day file older than the retention window.
        uint16_t days[64];
        int n = bs_store_m1_days(run_id, days, 64);
        for (int i = 0; i < n && i < 64; i++) {
            if (days[i] + BS_M1_KEEP_DAYS <= day) {
                snprintf(leaf, sizeof(leaf), "m%04u.bin", (unsigned)days[i]);
                run_path(run_id, leaf, path, sizeof(path));
                unlink(path);
            }
        }
    }
    return true;
}

// Pair-merge q15.bin in place (via a temp file) so it never exceeds
// BS_Q15_MAX_RECS records; the record interval doubles each time.
static bool q15_compact(uint16_t run_id, bs_ckpt_t *ck)
{
    char src[40], tmp[40];
    run_path(run_id, "q15.bin", src, sizeof(src));
    run_path(run_id, "q15.tmp", tmp, sizeof(tmp));
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return false; }
    bs_hist_rec_v2_t a, b;
    uint32_t written = 0;
    bool ok = true;
    while (fread(&a, sizeof(a), 1, in) == 1) {
        if (fread(&b, sizeof(b), 1, in) == 1) {
            uint64_t ta = a.dt_s, tb = b.dt_s, tt = ta + tb;
            bs_hist_rec_v2_t m = b;   // t_s, SOC and cumulative fields of b
            if (tt) m.v_avg_mv = (uint16_t)(((uint64_t)a.v_avg_mv * ta + (uint64_t)b.v_avg_mv * tb) / tt);
            m.dt_s = (uint16_t)(tt > 65535u ? 65535u : tt);
            m.flags = (uint16_t)(a.flags | b.flags);
            if (a.v_min_mv < m.v_min_mv) m.v_min_mv = a.v_min_mv;
            if (a.v_max_mv > m.v_max_mv) m.v_max_mv = a.v_max_mv;
            if (a.i_min_na < m.i_min_na) m.i_min_na = a.i_min_na;
            if (a.i_max_na > m.i_max_na) m.i_max_na = a.i_max_na;
            a = m;
        }
        if (fwrite(&a, sizeof(a), 1, out) != 1) { ok = false; break; }
        written++;
    }
    fclose(in);
    ok = (fclose(out) == 0) && ok;
    if (!ok) { unlink(tmp); return false; }
    unlink(src);
    if (rename(tmp, src) != 0) return false;
    ck->q15_count = written;
    ck->q15_interval_s *= 2u;
    return true;
}

bool bs_store_q15_append(uint16_t run_id, const bs_hist_rec_v2_t *r, bs_ckpt_t *ck)
{
    if (!s_ok) return false;
    char path[40];
    run_path(run_id, "q15.bin", path, sizeof(path));
    if (!append_file(path, r, sizeof(*r))) return false;
    ck->q15_count++;
    if (ck->q15_count >= BS_Q15_MAX_RECS) return q15_compact(run_id, ck);
    return true;
}

bool bs_store_s1_write(uint16_t run_id, const bs_hist_rec_v2_t *recs, size_t n)
{
    if (!s_ok) return false;
    char path[40];
    run_path(run_id, "s1.bin", path, sizeof(path));
    return write_file(path, recs, n * sizeof(*recs));
}

static void rm_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[64];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

bool bs_store_delete_run(uint16_t run_id)
{
    if (!s_ok) return false;
    uint16_t active;
    if (bs_store_active_get(&active) && active == run_id) return false;
    char dir[24];
    run_dir(run_id, dir, sizeof(dir));
    struct stat st;
    if (stat(dir, &st) != 0) return false;
    rm_tree(dir);
    return true;
}

bool bs_store_active_get(uint16_t *run_id)
{
    if (!s_ok) return false;
    FILE *f = fopen(BASE "/active", "r");
    if (!f) return false;
    unsigned id = 0;
    bool ok = fscanf(f, "%u", &id) == 1 && id > 0 && id <= 0xFFFFu;
    fclose(f);
    if (ok && run_id) *run_id = (uint16_t)id;
    return ok;
}

bool bs_store_active_set(uint16_t run_id)
{
    if (!s_ok) return false;
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "%u", (unsigned)run_id);
    return write_file(BASE "/active", buf, (size_t)n);
}

void bs_store_active_clear(void)
{
    if (s_ok) unlink(BASE "/active");
}

static bool file_path(uint16_t run_id, uint16_t file_id, char *out, size_t cap)
{
    char leaf[16];
    switch (file_id) {
    case BS_FILE_META:   strcpy(leaf, "meta.bin"); break;
    case BS_FILE_EVENTS: strcpy(leaf, "ev.bin");   break;
    case BS_FILE_Q15:    strcpy(leaf, "q15.bin");  break;
    case BS_FILE_S1:     strcpy(leaf, "s1.bin");   break;
    case BS_FILE_CKPT: {
        // Serve whichever checkpoint is newest and valid.
        char which = ckpt_pick(run_id, NULL);
        if (!which) return false;
        strcpy(leaf, which == 'a' ? "ck_a" : "ck_b");
        break;
    }
    default:
        if (file_id < BS_FILE_M1_BASE || file_id - BS_FILE_M1_BASE > 9999) return false;
        snprintf(leaf, sizeof(leaf), "m%04u.bin", (unsigned)(file_id - BS_FILE_M1_BASE));
        break;
    }
    run_path(run_id, leaf, out, cap);
    return true;
}

int32_t bs_store_file_size(uint16_t run_id, uint16_t file_id)
{
    if (!s_ok) return -1;
    char path[40];
    if (!file_path(run_id, file_id, path, sizeof(path))) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int32_t)st.st_size;
}

int32_t bs_store_file_read(uint16_t run_id, uint16_t file_id, uint32_t offset,
                           uint8_t *buf, uint32_t len)
{
    if (!s_ok) return -1;
    char path[40];
    if (!file_path(run_id, file_id, path, sizeof(path))) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int32_t n = -1;
    if (fseek(f, (long)offset, SEEK_SET) == 0) n = (int32_t)fread(buf, 1, len, f);
    fclose(f);
    return n;
}

int bs_store_m1_days(uint16_t run_id, uint16_t *days, int max)
{
    if (!s_ok) return 0;
    char dir[24];
    run_dir(run_id, dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        unsigned day;
        if (e->d_name[0] == 'm' && sscanf(e->d_name + 1, "%4u.bin", &day) == 1) {
            days[n++] = (uint16_t)day;
        }
    }
    closedir(d);
    for (int i = 1; i < n; i++) {
        uint16_t v = days[i]; int j = i - 1;
        while (j >= 0 && days[j] > v) { days[j + 1] = days[j]; j--; }
        days[j + 1] = v;
    }
    return n;
}

#define ST_CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); ok = false; goto out; } } while (0)

bool bs_store_selftest(void)
{
    if (!s_ok) { printf("store not mounted\n"); return false; }
    const uint16_t id = BS_SELFTEST_RUN;
    char dir[24], path[40];
    run_dir(id, dir, sizeof(dir));
    rm_tree(dir);
    if (mkdir(dir, 0775) != 0) { printf("mkdir %s failed\n", dir); return false; }
    bool ok = true;
    bs_hist_rec_v2_t *buf = NULL;
    uint16_t days[64];

    // Retention: one record per simulated day, days 0..33.
    for (uint32_t day = 0; day <= 33; day++) {
        bs_hist_rec_v2_t r = { .t_s = day * 86400u + 60u, .dt_s = 60, .q_dut_nc = day };
        ST_CHECK(bs_store_m1_append(id, &r), "m1 append day %u", (unsigned)day);
        int n = bs_store_m1_days(id, days, 64);
        uint32_t want_n = day < BS_M1_KEEP_DAYS ? day + 1 : BS_M1_KEEP_DAYS;
        uint32_t want_first = day < BS_M1_KEEP_DAYS ? 0 : day - BS_M1_KEEP_DAYS + 1;
        ST_CHECK(n == (int)want_n && days[0] == want_first && days[n - 1] == day,
                 "day %u: %d files, first %u (want %u, first %u)", (unsigned)day, n,
                 n ? days[0] : 0u, (unsigned)want_n, (unsigned)want_first);
        if (day == 31 || day == 32 || day == 33) {
            printf("  day %2u: %d m1 files kept, oldest m%04u\n", (unsigned)day, n, days[0]);
        }
    }

    // q15: 8191 records written directly, the 8192nd through the API compacts.
    run_path(id, "q15.bin", path, sizeof(path));
    buf = malloc(256 * sizeof(bs_hist_rec_v2_t));
    ST_CHECK(buf, "alloc");
    FILE *f = fopen(path, "wb");
    ST_CHECK(f, "q15 open");
    for (uint32_t i = 0; i < BS_Q15_MAX_RECS - 1; i += 256) {
        uint32_t k = 0;
        for (; k < 256 && i + k < BS_Q15_MAX_RECS - 1; k++) {
            uint32_t j = i + k;
            buf[k] = (bs_hist_rec_v2_t){ .t_s = (j + 1) * 900u, .dt_s = 900,
                                         .v_avg_mv = (uint16_t)(1000 + (j & 1) * 2),
                                         .v_min_mv = (uint16_t)(900 + j % 7),
                                         .flags = (uint16_t)((j == 3) ? BS_RF_GAP : 0),
                                         .q_dut_nc = j + 1, .e_dut_uj = 10 * (j + 1) };
        }
        if (fwrite(buf, sizeof(*buf), k, f) != k) { fclose(f); ST_CHECK(false, "q15 write"); }
    }
    fclose(f);
    bs_ckpt_t ck = { .q15_interval_s = 900, .q15_count = BS_Q15_MAX_RECS - 1 };
    bs_hist_rec_v2_t last = { .t_s = BS_Q15_MAX_RECS * 900u, .dt_s = 900, .v_avg_mv = 1002,
                              .v_min_mv = 900, .q_dut_nc = BS_Q15_MAX_RECS,
                              .e_dut_uj = 10 * BS_Q15_MAX_RECS };
    ST_CHECK(bs_store_q15_append(id, &last, &ck), "q15 append/compact");
    int32_t sz = bs_store_file_size(id, BS_FILE_Q15);
    ST_CHECK(ck.q15_count == BS_Q15_MAX_RECS / 2 && ck.q15_interval_s == 1800 &&
             sz == (int32_t)(BS_Q15_MAX_RECS / 2 * sizeof(bs_hist_rec_v2_t)),
             "compaction: count %u interval %u size %ld", (unsigned)ck.q15_count,
             (unsigned)ck.q15_interval_s, (long)sz);
    bs_hist_rec_v2_t r0, r1, rl;
    ST_CHECK(bs_store_file_read(id, BS_FILE_Q15, 0, (uint8_t *)&r0, sizeof(r0)) == sizeof(r0) &&
             bs_store_file_read(id, BS_FILE_Q15, sizeof(r0), (uint8_t *)&r1, sizeof(r1)) == sizeof(r1) &&
             bs_store_file_read(id, BS_FILE_Q15, (uint32_t)sz - sizeof(rl), (uint8_t *)&rl, sizeof(rl)) == sizeof(rl),
             "q15 read back");
    ST_CHECK(r0.t_s == 1800 && r0.dt_s == 1800 && r0.q_dut_nc == 2 && r0.e_dut_uj == 20 &&
             r0.v_avg_mv == 1001 && r0.v_min_mv == 900 && r0.flags == 0,
             "rec0 t %u dt %u q %lld e %lld vavg %u vmin %u fl %x", (unsigned)r0.t_s,
             (unsigned)r0.dt_s, (long long)r0.q_dut_nc, (long long)r0.e_dut_uj,
             r0.v_avg_mv, r0.v_min_mv, r0.flags);
    ST_CHECK(r1.t_s == 3600 && r1.flags == BS_RF_GAP, "rec1 t %u flags %x", (unsigned)r1.t_s, r1.flags);
    ST_CHECK(rl.t_s == BS_Q15_MAX_RECS * 900u && rl.q_dut_nc == BS_Q15_MAX_RECS && rl.dt_s == 1800,
             "last t %u q %lld dt %u", (unsigned)rl.t_s, (long long)rl.q_dut_nc, (unsigned)rl.dt_s);
    printf("  q15: 8192 -> %u records, interval %u s, merged dt/v_avg/min/flags/cumulative ok\n",
           (unsigned)ck.q15_count, (unsigned)ck.q15_interval_s);
out:
    free(buf);
    rm_tree(dir);
    printf("bs selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
