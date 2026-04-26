// =============================================================================
// cli_complete.cpp - Tab completion over the command table.
// =============================================================================

#include "cli_complete.h"
#include "cli_cmdtab.h"

#include <string.h>

int cli_complete_first_word(const char* prefix,
                            const char** out_names, int out_cap)
{
    if (!prefix || !out_names || out_cap <= 0) return 0;
    size_t plen = strlen(prefix);
    int n = 0;

    for (size_t i = 0; i < g_cliCommandCount && n < out_cap; i++) {
        const CliCommand* c = &g_cliCommands[i];
        if (!c->name) continue;
        if (c->category == CAT_HIDDEN) continue;
        if (strncmp(c->name, prefix, plen) == 0) {
            out_names[n++] = c->name;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Generic "match static list" helper used by the per-command completers.
// ---------------------------------------------------------------------------
static int match_static_list(const char* prefix, const char* const* pool,
                             int pool_count, const char** out, int out_cap)
{
    if (!prefix) prefix = "";
    size_t plen = strlen(prefix);
    int n = 0;
    for (int i = 0; i < pool_count && n < out_cap; i++) {
        if (strncmp(pool[i], prefix, plen) == 0) out[n++] = pool[i];
    }
    return n;
}

// Help completer — all primary command names.
int cli_complete_help(const char* prefix, int arg_idx,
                      const char** out, int out_cap)
{
    (void)arg_idx;
    return cli_complete_first_word(prefix, out, out_cap);
}

// Channel 0..3 (for func, adc, dac, ilimit, vrange, avdd, do).
int cli_complete_channel(const char* prefix, int arg_idx,
                         const char** out, int out_cap)
{
    (void)arg_idx;
    static const char* pool[] = { "0", "1", "2", "3" };
    return match_static_list(prefix, pool, 4, out, out_cap);
}

// GPIO pin names (A..F and 0..5).
int cli_complete_gpio_pin(const char* prefix, int arg_idx,
                          const char** out, int out_cap)
{
    (void)arg_idx;
    static const char* pool[] = {
        "A", "B", "C", "D", "E", "F",
        "0", "1", "2", "3", "4", "5",
    };
    return match_static_list(prefix, pool, 12, out, out_cap);
}

// PCA9535 control names (first arg); sub-arg (second arg) is 0/1.
int cli_complete_pca_name(const char* prefix, int arg_idx,
                          const char** out, int out_cap)
{
    if (arg_idx == 0) {
        static const char* pool[] = {
            "vadj1", "vadj2", "15v", "mux", "usb",
            "efuse1", "efuse2", "efuse3", "efuse4",
            "status",
        };
        return match_static_list(prefix, pool, 10, out, out_cap);
    }
    if (arg_idx == 1) {
        static const char* pool[] = { "0", "1" };
        return match_static_list(prefix, pool, 2, out, out_cap);
    }
    return 0;
}

int cli_complete_usbpd_sub(const char* prefix, int arg_idx,
                           const char** out, int out_cap)
{
    if (arg_idx == 0) {
        static const char* pool[] = { "status", "caps", "select" };
        return match_static_list(prefix, pool, 3, out, out_cap);
    }
    // After `select`, offer common PD voltages.
    static const char* pool[] = { "5", "9", "12", "15", "18", "20" };
    return match_static_list(prefix, pool, 6, out, out_cap);
}

int cli_complete_on_off(const char* prefix, int arg_idx,
                        const char** out, int out_cap)
{
    (void)arg_idx;
    static const char* pool[] = { "on", "off" };
    return match_static_list(prefix, pool, 2, out, out_cap);
}

int cli_complete_wifi_sub(const char* prefix, int arg_idx,
                          const char** out, int out_cap)
{
    (void)arg_idx;
    static const char* pool[] = { "scan" };
    return match_static_list(prefix, pool, 1, out, out_cap);
}

int cli_complete_common_prefix(const char* const* names, int count,
                               char* out, int out_cap)
{
    if (!out || out_cap <= 0) return 0;
    out[0] = '\0';
    if (!names || count <= 0) return 0;
    if (count == 1) {
        // The "common prefix" of one string is the whole string.
        strncpy(out, names[0], (size_t)out_cap - 1);
        out[out_cap - 1] = '\0';
        return (int)strlen(out);
    }

    // Walk character positions until any name diverges or we hit out_cap.
    int pos = 0;
    while (pos < out_cap - 1) {
        char ch = names[0][pos];
        if (ch == '\0') break;
        bool match = true;
        for (int i = 1; i < count; i++) {
            if (names[i][pos] != ch) { match = false; break; }
        }
        if (!match) break;
        out[pos++] = ch;
    }
    out[pos] = '\0';
    return pos;
}
