// =============================================================================
// cli_cmdtab.cpp - CLI command table + auto-help renderer.
//
// Single source of truth for every CLI command. The dispatcher in cli.cpp
// walks this table, and the `help` command auto-renders from it so new
// commands appear automatically. Phase 1 of the CLI rebuild.
// =============================================================================

#include "cli_cmdtab.h"
#include "cli_cmds_dev.h"
#include "cli_cmds_sys.h"
#include "cli_cmd_adapter.h"
#include "cli_term.h"
#include "cli_tui.h"
#include "cli_history.h"
#include "cli_complete.h"
#include "serial_io.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Forward: the `help` handler lives in this file because it reads the table.
// ---------------------------------------------------------------------------
extern "C" void cli_cmd_help(const char* args);

// ---------------------------------------------------------------------------
// The command table. Order within a category is preserved in the help output.
//
// Entries that omit the trailing `complete` field rely on C++ aggregate
// zero-initialization to set it to NULL. We suppress the associated warning
// here rather than writing ", NULL" on every row, because most entries do
// not have a completer and the noise is not useful.
// ---------------------------------------------------------------------------
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

const CliCommand g_cliCommands[] = {
    // ---- General ----
    { "help",     "?,h",    cli_cmd_help,          CAT_GENERAL,
      "help [cmd]",         "Show this help (or detail for one command)", NULL,
      cli_complete_help },
    { "color",    NULL,     cli_cmd_color,         CAT_GENERAL,
      "color [on|off]",     "Enable / disable ANSI color output", NULL,
      cli_complete_on_off },
    { "history",  NULL,     cli_cmd_history,       CAT_GENERAL,
      "history",            "Show the command history ring", NULL },
    { "tui",      NULL,     cli_cmd_tui,           CAT_GENERAL,
      "tui",                "Open full-screen dashboard (q or Ctrl-C to exit)", NULL },
    { "menu",     "m",      cli_cmd_menu,          CAT_GENERAL,
      "menu",               "Show main menu (numeric shortcuts)", NULL },
    { "status",   "s",      cli_cmd_status,        CAT_GENERAL,
      "status",             "Device overview (all channels)", NULL },
    { "temp",     "t",      cli_cmd_temp,          CAT_GENERAL,
      "temp",               "Read die temperature", NULL },
    { "scratch",  NULL,     cli_cmd_scratch,       CAT_GENERAL,
      "scratch",            "SPI comms test (SCRATCH register)", NULL },
    { "reset",    NULL,     cli_cmd_reset,         CAT_GENERAL,
      "reset",              "Hardware reset (pulse RESET pin)", NULL },
    { "rstinfo",  NULL,     cli_cmd_rstinfo,       CAT_GENERAL,
      "rstinfo",            "Show last ESP reset reason code", NULL },
    { "coredump", "cdump",   cli_cmd_coredump,      CAT_GENERAL,
      "coredump [info|dump|clear]", "Show, dump, or clear saved ESP panic coredump", NULL },
    { "silicon",  NULL,     cli_cmd_silicon,       CAT_GENERAL,
      "silicon",            "Read silicon revision and ID", NULL },
    { "regs",     NULL,     cli_cmd_regs,          CAT_GENERAL,
      "regs",               "Quick register dump (key registers)", NULL },

    // ---- Register Access ----
    { "rreg",     NULL,     cli_cmd_read_reg,      CAT_REGS,
      "rreg <addr>",        "Read register (hex addr, e.g. rreg 76)", NULL },
    { "wreg",     NULL,     cli_cmd_write_reg,     CAT_REGS,
      "wreg <addr> <val>",  "Write SCRATCH register only (0x76-0x79)", NULL },

    // ---- Channel Config ----
    { "func",     NULL,     cli_cmd_func,          CAT_CHANNEL,
      "func <ch> <code>",
      "Set channel function (codes: 0=HIGH_IMP 1=VOUT 2=IOUT 3=VIN", NULL,
      cli_complete_channel },
    { "ilimit",   NULL,     cli_cmd_ilimit,        CAT_CHANNEL,
      "ilimit <ch> <0|1>",  "Set current limit (1=enabled)", NULL,
      cli_complete_channel },
    { "vrange",   NULL,     cli_cmd_vrange,        CAT_CHANNEL,
      "vrange <ch> <0|1>",  "Set VOUT range (0=unipolar 1=bipolar)", NULL,
      cli_complete_channel },
    { "avdd",     NULL,     cli_cmd_avdd,          CAT_CHANNEL,
      "avdd <ch> <0-3>",    "Set AVDD source selection", NULL,
      cli_complete_channel },

    // ---- ADC ----
    { "adc",      NULL,     cli_cmd_adc,           CAT_ADC,
      "adc [ch]",           "Read ADC (all channels or specific ch 0-3)", NULL,
      cli_complete_channel },
    { "adccont",  NULL,     cli_cmd_adc_cont,      CAT_ADC,
      "adccont <sec>",      "Continuous ADC print for N seconds", NULL },
    { "diag",     NULL,     cli_cmd_adc_diag,      CAT_ADC,
      "diag",               "Read all ADC diagnostic channels", NULL },
    { "diagcfg",  NULL,     cli_cmd_diag_cfg,      CAT_ADC,
      "diagcfg <slot> <src>",
      "Configure diag slot (0-3) source (0=AGND 1=Temp 2=DVCC ...)", NULL },
    { "diagread", NULL,     cli_cmd_diag_read,     CAT_ADC,
      "diagread",           "Read all 4 diagnostic slots (cached)", NULL },

    // ---- DAC ----
    { "dac",      NULL,     cli_cmd_dac,           CAT_DAC,
      "dac <ch> <code|v|i>",
      "Set DAC code (0-65535), voltage (v <V>), or current (i <mA>)", NULL,
      cli_complete_channel },
    { "sweep",    NULL,     cli_cmd_sweep,         CAT_DAC,
      "sweep <ch> <ms>",    "Sawtooth sweep DAC ch (period in ms)", NULL,
      cli_complete_channel },

    // ---- Digital I/O ----
    { "din",      NULL,     cli_cmd_din,           CAT_DIO,
      "din",                "Read all digital input states + counters", NULL },
    { "do",       NULL,     cli_cmd_do_set,        CAT_DIO,
      "do <ch> <0|1>",      "Set digital output on/off", NULL,
      cli_complete_channel },

    // ---- GPIO ----
    { "gpio",     NULL,     cli_cmd_gpio,          CAT_GPIO,
      "gpio [pin [mode|set|read] ...]",
      "Read/configure GPIO A-F (type 'gpio' for full usage)", NULL,
      cli_complete_gpio_pin },

    // ---- Faults ----
    { "faults",   "f",      cli_cmd_faults,        CAT_FAULTS,
      "faults",             "Read all fault/alert registers", NULL },
    { "clear",    NULL,     cli_cmd_clear_faults,  CAT_FAULTS,
      "clear",              "Clear all faults", NULL },

    // ---- MUX (ADGS2414D) ----
    { "mux",      NULL,     cli_cmd_mux,           CAT_MUX,
      "mux [<dev> <sw> [0|1]]",
      "Show MUX state / set / toggle switch", NULL },
    { "muxreset", NULL,     cli_cmd_mux_reset,     CAT_MUX,
      "muxreset",           "Soft reset ADGS (address mode) / open all", NULL },
    { "cstest",   NULL,     cli_cmd_cstest,        CAT_MUX,
      "cstest",             "Toggle MUX CS pin 5x (GPIO sanity test)", NULL },
    { "muxtest",  NULL,     cli_cmd_muxtest,       CAT_MUX,
      "muxtest [hex]",      "Test ADGS2414D address mode (single device)", NULL },

    // ---- I2C Devices ----
    { "i2cscan",  NULL,     cli_cmd_i2c_scan,      CAT_I2C_BUS,
      "i2cscan",            "Scan I2C bus for devices", NULL },
    { "idac",     NULL,     cli_cmd_idac,          CAT_I2C_BUS,
      "idac [<ch> [code|v|cal] ...]",
      "Show/set DS4424 IDAC (ch: 0=LvlShft 1=VADJ1 2=VADJ2)", NULL },
    { "idac_cal", NULL,     cli_cmd_idac_cal,      CAT_I2C_BUS,
      "idac_cal <ch>",
      "Auto-calibrate IDAC channel via selftest U23 measurement path", NULL },
    { "usbpd",    "pd",     cli_cmd_usbpd,         CAT_I2C_BUS,
      "usbpd [status|caps|select <V>]",
      "HUSB238 USB-PD status / capability request / PDO select", NULL,
      cli_complete_usbpd_sub },
    { "pca",      "ioexp",  cli_cmd_pca,           CAT_I2C_BUS,
      "pca [<name> <0|1>]",
      "PCA9535 GPIO expander (vadj1/vadj2/15v/mux/usb/efuse1-4)", NULL,
      cli_complete_pca_name },
    { "supplies", NULL,     cli_cmd_supplies,      CAT_I2C_BUS,
      "supplies",
      "Show cached supply rail voltages (VADJ1, VADJ2, VLOGIC)", NULL },
    { "selftest", NULL,     cli_cmd_selftest,      CAT_I2C_BUS,
      "selftest worker [on|off]",
      "Enable/disable supply monitor (opt-in, default off)", NULL },

    // ---- Network ----
    { "wifi",     NULL,     cli_cmd_wifi,          CAT_NETWORK,
      "wifi [scan | <ssid> <password>]",
      "Show WiFi status / scan networks / connect STA", NULL,
      cli_complete_wifi_sub },
    { "token",    NULL,     cli_cmd_token,         CAT_NETWORK,
      "token",              "Show admin token for HTTP pairing", NULL, NULL },

    // ---- Diagnostic / low-level ----
    { "clkout",   NULL,     cli_cmd_clkout,        CAT_DIAG,
      "clkout <io> <src> <hz>|off|status",
      "Bench clock on IO 1-12 (src=ledc|mcpwm). LEDC<=40MHz, MCPWM<=80MHz; level shifter ~25MHz limit.", NULL },
    { "spiclock", NULL,     cli_cmd_spiclock,      CAT_DIAG,
      "spiclock [<Hz>]",
      "Get / set SPI clock frequency (100kHz .. 20MHz)", NULL },

    // ---- Registry adapter ----
    { "cmd",  NULL,     cli_cmd_generic,       CAT_DIAG,
      "cmd <name> [key=value ...]",
      "Dispatch any registered command by name (see 'cmds')", NULL,
      cli_cmd_adapter_complete },
    { "cmds", NULL,     cli_cmd_list,          CAT_DIAG,
      "cmds [filter]",
      "List all registered commands with arg specs", NULL },
};

#pragma GCC diagnostic pop

const size_t g_cliCommandCount = sizeof(g_cliCommands) / sizeof(g_cliCommands[0]);

// ---------------------------------------------------------------------------
// Category labels (indexed by CliCategory).
// ---------------------------------------------------------------------------
static const char* kCategoryLabel[CAT__COUNT] = {
    "General",              // CAT_GENERAL
    "Register Access",      // CAT_REGS
    "Channel Config",       // CAT_CHANNEL
    "ADC",                  // CAT_ADC
    "DAC",                  // CAT_DAC
    "Digital I/O",          // CAT_DIO
    "GPIO",                 // CAT_GPIO
    "Faults",               // CAT_FAULTS
    "MUX (ADGS2414D)",      // CAT_MUX
    "I2C Devices",          // CAT_I2C_BUS
    "Network",              // CAT_NETWORK
    "Diagnostic / Low-level", // CAT_DIAG
    NULL                    // CAT_HIDDEN — not rendered
};

// ---------------------------------------------------------------------------
// Alias matcher: returns true if `needle` equals any comma-separated token in
// `aliases_csv` (case-sensitive, whitespace-tolerant around tokens).
// ---------------------------------------------------------------------------
static bool alias_matches(const char* aliases_csv, const char* needle)
{
    if (!aliases_csv || !needle) return false;
    const char* p = aliases_csv;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char* tok_start = p;
        while (*p && *p != ',') p++;
        size_t tok_len = (size_t)(p - tok_start);
        // Trim trailing spaces
        while (tok_len > 0 && tok_start[tok_len - 1] == ' ') tok_len--;
        size_t need_len = strlen(needle);
        if (tok_len == need_len && strncmp(tok_start, needle, tok_len) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public: find by name or alias.
// ---------------------------------------------------------------------------
const CliCommand* cli_cmdtab_find(const char* name_or_alias)
{
    if (!name_or_alias || !*name_or_alias) return NULL;
    for (size_t i = 0; i < g_cliCommandCount; i++) {
        const CliCommand* c = &g_cliCommands[i];
        if (strcmp(c->name, name_or_alias) == 0) return c;
        if (alias_matches(c->aliases, name_or_alias)) return c;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Public: print full help in a compact multi-column grid.
// Column width is fixed at 11 chars (widest primary names are 8 chars).
// Column count adapts to the terminal width reported by cli_term.
// ---------------------------------------------------------------------------
void cli_cmdtab_print_help(void)
{
    const int COL_WIDTH = 11;
    const int LEFT_PAD  = 2;
    int cols_avail = term_cols() - LEFT_PAD;
    if (cols_avail < COL_WIDTH) cols_avail = COL_WIDTH;
    int ncols = cols_avail / COL_WIDTH;
    if (ncols < 1) ncols = 1;
    if (ncols > 10) ncols = 10;

    term_println("");
    term_bold_cprint(TERM_FG_B_CYAN, "BugBuster CLI");
    term_print("  ");
    term_cprintf(TERM_FG_B_BLACK,
        "— type 'help <cmd>' for detail, 'tui' for dashboard\r\n");

    for (int cat = 0; cat < CAT__COUNT; cat++) {
        if (cat == CAT_HIDDEN) continue;
        const char* label = kCategoryLabel[cat];
        if (!label) continue;

        // Collect names for this category.
        const char* names[40];
        int ncount = 0;
        for (size_t i = 0; i < g_cliCommandCount && ncount < 40; i++) {
            if ((int)g_cliCommands[i].category == cat) {
                names[ncount++] = g_cliCommands[i].name;
            }
        }
        if (ncount == 0) continue;

        term_println("");
        term_cprintf(TERM_FG_B_YELLOW, "%s", label);
        term_println("");

        for (int i = 0; i < ncount; i++) {
            if ((i % ncols) == 0) term_print("  ");
            term_cprintf(TERM_FG_B_CYAN, "%-*s", COL_WIDTH, names[i]);
            if ((i % ncols) == (ncols - 1) || i == ncount - 1) term_println("");
        }
    }

    term_println("");
}

// ---------------------------------------------------------------------------
// Public: detailed help for one command.
// ---------------------------------------------------------------------------
void cli_cmdtab_print_cmd_help(const CliCommand* c)
{
    if (!c) { term_println("(unknown command)"); return; }

    term_println("");
    term_bold_cprint(TERM_FG_B_CYAN, c->name);
    if (c->aliases && *c->aliases) {
        term_cprintf(TERM_FG_B_BLACK, "  (aliases: %s)", c->aliases);
    }
    term_println("");

    if (c->usage && *c->usage) {
        term_print("  Usage: ");
        term_cprintf(TERM_FG_B_GREEN, "%s", c->usage);
        term_println("");
    }
    if (c->short_help && *c->short_help) {
        term_printf("  %s\r\n", c->short_help);
    }
    if (c->long_help && *c->long_help) {
        term_println("");
        term_println(c->long_help);
    }
}

// ---------------------------------------------------------------------------
// `help` handler. With an argument, prints detail for that command.
// ---------------------------------------------------------------------------
extern "C" void cli_cmd_help(const char* args)
{
    while (args && *args == ' ') args++;
    if (args && *args) {
        const CliCommand* c = cli_cmdtab_find(args);
        if (c) {
            cli_cmdtab_print_cmd_help(c);
        } else {
            term_cprintf(TERM_FG_B_RED,
                "Unknown command: '%s'. Type 'help' for the list.\r\n", args);
        }
    } else {
        cli_cmdtab_print_help();
    }
}
