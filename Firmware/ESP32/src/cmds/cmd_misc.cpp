// =============================================================================
// cmd_misc.cpp — Registry handlers for miscellaneous system commands
//
//   BBP_CMD_REG_READ     (0x71)
//   BBP_CMD_REG_WRITE    (0x72)
//   BBP_CMD_SET_WATCHDOG (0x73)
//   BBP_CMD_DEVICE_RESET (0x70)
//   BBP_CMD_SET_SPI_CLOCK(0xE3)
// =============================================================================
#include "../cmd_registry.h"
#include "../cmd_errors.h"
#include "../bbp_codec.h"
#include "../bbp.h"
#include "../tasks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cmd_misc";

// g_spi_bus_mutex is defined in tasks.cpp / bbp.cpp — forward declare
extern SemaphoreHandle_t g_spi_bus_mutex;

// ---------------------------------------------------------------------------
// REG_READ  payload: u8 addr  → resp: u8 addr, u16 value
// Wire format matches legacy handleRegRead (bbp.cpp:1193-1206).
// Live SPI read via bbp_spi_read_reg() (thin wrapper added to bbp.cpp).
// ---------------------------------------------------------------------------
static int handler_reg_read(const uint8_t *payload, size_t len,
                            uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t addr = payload[0];

    uint16_t value = 0;
    if (!bbp_spi_read_reg(addr, &value)) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, addr);
    bbp_put_u16(resp, &pos, value);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// REG_WRITE  payload: u8 addr, u16 value  → resp: same 3 bytes (memcpy echo)
// Wire format matches legacy handleRegWrite (bbp.cpp:1217-1242).
// Safety: only SCRATCH registers allowed (matching legacy isSafeRawRegisterWrite).
// memcpy echo (legacy bbp.cpp:1241).
// ---------------------------------------------------------------------------
static int handler_reg_write(const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  addr  = bbp_get_u8(payload, &rpos);
    uint16_t value = bbp_get_u16(payload, &rpos);

    // Safety gate: only SCRATCH registers (matching legacy isSafeRawRegisterWrite bbp.cpp:1213)
    if (!(addr >= REG_SCRATCH && addr <= (REG_SCRATCH + 3))) return -CMD_ERR_BAD_ARG;

    if (!bbp_spi_write_reg(addr, value)) return -CMD_ERR_HARDWARE;

    // Verify readback (matching legacy bbp.cpp:1235-1238)
    uint16_t readback = 0;
    if (!bbp_spi_read_reg(addr, &readback) || readback != value) return -CMD_ERR_HARDWARE;

    // memcpy echo matching legacy bbp.cpp:1241
    memcpy(resp, payload, 3);
    *resp_len = 3;
    return 3;
}

// ---------------------------------------------------------------------------
// SET_WATCHDOG  payload: u8 enable, u8 timeout_code  → resp: u8 enable, u8 timeout_code
// Wire format matches legacy handleSetWatchdog (bbp.cpp:1261-1287).
// Clamps timeout_code per datasheet + firmware safety policy (matching legacy).
// Uses bbp_spi_write_reg() thin wrapper added to bbp.cpp.
// ---------------------------------------------------------------------------
static int handler_set_watchdog(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t enable       = bbp_get_u8(payload, &rpos);
    uint8_t timeout_code = bbp_get_u8(payload, &rpos);

    // Clamp per datasheet (matching legacy bbp.cpp:1269)
    if (timeout_code > 0x0A) timeout_code = 0x09;
    // Firmware safety: clamp to >=500ms when enabled (matching legacy bbp.cpp:1275)
    if (enable && timeout_code < 0x07) timeout_code = 0x07;

    uint16_t wdt_val = ((uint16_t)(enable ? 1 : 0) << 4) | (timeout_code & 0x0F);
    if (!bbp_spi_write_reg(REG_WDT_CONFIG, wdt_val)) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, enable);
    bbp_put_u8(resp, &pos, timeout_code);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// DEVICE_RESET  payload: (none)  resp: (none)
// Wire format matches legacy handleDeviceReset (bbp.cpp:1245-1258).
// Sets all 4 channels to HIGH_IMP and clears alerts.
// ---------------------------------------------------------------------------
static int handler_device_reset(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;

    for (uint8_t ch = 0; ch < 4; ch++) {
        Command cmd = {};
        cmd.type = CMD_SET_CHANNEL_FUNC;
        cmd.channel = ch;
        cmd.func = CH_FUNC_HIGH_IMP;
        sendCommand(cmd);
    }
    Command clr = {};
    clr.type = CMD_CLEAR_ALERTS;
    sendCommand(clr);

    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// SET_SPI_CLOCK  payload: u32 hz  → resp: u32 hz, bool match
// Wire format matches legacy handleSetSpiClock (bbp.cpp:2102-2135).
// Acquires g_spi_bus_mutex (200ms) then calls bbp_spi_set_clock().
// Verifies via scratch register write/read (matching legacy).
// ---------------------------------------------------------------------------
static int handler_set_spi_clock(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 4) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint32_t hz = bbp_get_u32(payload, &rpos);

    // Pause ADC task during SPI device reconfiguration (matching legacy bbp.cpp:2110-2115)
    if (g_spi_bus_mutex == NULL ||
        xSemaphoreTakeRecursive(g_spi_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -CMD_ERR_HARDWARE;
    }

    bool ok = bbp_spi_set_clock(hz);
    xSemaphoreGiveRecursive(g_spi_bus_mutex);

    if (!ok) return -CMD_ERR_HARDWARE;

    // Verify SPI still works (matching legacy bbp.cpp:2123-2128)
    uint16_t scratch = 0;
    bbp_spi_write_reg(0x76, 0xA5C3);
    bool crc_ok = bbp_spi_read_reg(0x76, &scratch);
    bool match = (scratch == 0xA5C3);
    bbp_spi_write_reg(0x76, 0x0000);

    ESP_LOGI(TAG, "SPI clock set to %lu Hz — verify: %s", (unsigned long)hz, match ? "OK" : "FAIL");

    size_t pos = 0;
    bbp_put_u32(resp, &pos, hz);
    bbp_put_bool(resp, &pos, match && crc_ok);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
static const ArgSpec s_reg_read_args[] = {
    { "addr", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_reg_read_rsp[] = {
    { "addr",  ARG_U8,  true, 0, 0 },
    { "value", ARG_U16, true, 0, 0 },
};

static const ArgSpec s_reg_write_args[] = {
    { "addr",  ARG_U8,  true, 0, 255 },
    { "value", ARG_U16, true, 0, 0 },
};
static const ArgSpec s_reg_write_rsp[] = {
    { "addr",  ARG_U8,  true, 0, 0 },
    { "value", ARG_U16, true, 0, 0 },
};

static const ArgSpec s_set_watchdog_args[] = {
    { "enable",       ARG_U8, true, 0, 1 },
    { "timeout_code", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_set_watchdog_rsp[] = {
    { "enable",       ARG_U8, true, 0, 0 },
    { "timeout_code", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_set_spi_clock_args[] = {
    { "hz", ARG_U32, true, 100000, 20000000 },
};
static const ArgSpec s_set_spi_clock_rsp[] = {
    { "hz",    ARG_U32,  true, 0, 0 },
    { "match", ARG_BOOL, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_misc_cmds[] = {
    { BBP_CMD_REG_READ,      "reg_read",
      s_reg_read_args,     1, s_reg_read_rsp,     2, handler_reg_read,      CMD_FLAG_READS_STATE },
    { BBP_CMD_REG_WRITE,     "reg_write",
      s_reg_write_args,    2, s_reg_write_rsp,    2, handler_reg_write,     0                   },
    { BBP_CMD_SET_WATCHDOG,  "set_watchdog",
      s_set_watchdog_args, 2, s_set_watchdog_rsp, 2, handler_set_watchdog,  0                   },
    { BBP_CMD_DEVICE_RESET,  "device_reset",
      NULL,                0, NULL,               0, handler_device_reset,  0                   },
    { BBP_CMD_SET_SPI_CLOCK, "set_spi_clock",
      s_set_spi_clock_args, 1, s_set_spi_clock_rsp, 2, handler_set_spi_clock, 0                },
};

extern "C" void register_cmds_misc(void)
{
    cmd_registry_register_block(s_misc_cmds,
        sizeof(s_misc_cmds) / sizeof(s_misc_cmds[0]));
}
