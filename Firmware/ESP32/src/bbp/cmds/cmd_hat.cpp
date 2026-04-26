// =============================================================================
// cmd_hat.cpp — Registry handlers for HAT expansion board commands
//
// Command-style HAT opcodes only. Streaming data path (LA bulk transfer) stays
// in bbp.cpp for slice 5. The following are migrated here:
//
//   BBP_CMD_HAT_GET_STATUS      (0xC5)
//   BBP_CMD_HAT_SET_PIN         (0xC6)
//   BBP_CMD_HAT_SET_ALL_PINS    (0xC7)
//   BBP_CMD_HAT_RESET           (0xC8)
//   BBP_CMD_HAT_DETECT          (0xC9)
//   BBP_CMD_HAT_SET_POWER       (0xCA)
//   BBP_CMD_HAT_GET_POWER       (0xCB)
//   BBP_CMD_HAT_SET_IO_VOLTAGE  (0xCC)
//   BBP_CMD_HAT_SETUP_SWD       (0xCD)
//   BBP_CMD_HAT_GET_HVPAK_INFO  (0xCE) — passthrough
//   BBP_CMD_HAT_LA_CONFIG       (0xCF)
//   BBP_CMD_HAT_LA_ARM          (0xD5)
//   BBP_CMD_HAT_LA_FORCE        (0xD6)
//   BBP_CMD_HAT_LA_STATUS       (0xD7)
//   BBP_CMD_HAT_LA_READ         (0xD8)
//   BBP_CMD_HAT_LA_STOP         (0xD9)
//   BBP_CMD_HAT_LA_TRIGGER      (0xDA)
//   BBP_CMD_HAT_GET_HVPAK_CAPS  (0xDB) — passthrough
//   BBP_CMD_HAT_GET_HVPAK_LUT   (0xDC) — passthrough
//   BBP_CMD_HAT_SET_HVPAK_LUT   (0xDD) — passthrough
//   BBP_CMD_HAT_GET_HVPAK_BRIDGE(0xDE) — passthrough
//   BBP_CMD_HAT_SET_HVPAK_BRIDGE(0xDF) — passthrough
//   BBP_CMD_HAT_GET_HVPAK_ANALOG(0xE5) — passthrough
//   BBP_CMD_HAT_SET_HVPAK_ANALOG(0xE6) — passthrough
//   BBP_CMD_HAT_GET_HVPAK_PWM   (0xE7) — passthrough
//   BBP_CMD_HAT_SET_HVPAK_PWM   (0xE8) — passthrough
//   BBP_CMD_HAT_HVPAK_REG_READ  (0xE9) — passthrough
//   BBP_CMD_HAT_HVPAK_REG_WRITE_MASKED (0xEA) — passthrough
//   BBP_CMD_HAT_LA_LOG_ENABLE   (0xEB)
//   BBP_CMD_HAT_LA_USB_RESET    (0xED)
//   BBP_CMD_HAT_LA_STREAM_START (0xEE)
//
// NOT migrated (slice 5 streaming territory):
//   Any bulk LA data path that streams continuously.
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "hat.h"

// ---------------------------------------------------------------------------
// Helper: translate HAT error code to CMD error code.
// Mirrors the switch in legacy handleHatHvpakPassthrough (bbp.cpp:1766-1793).
// ---------------------------------------------------------------------------
static int hat_err_to_cmd_err(uint8_t hat_err)
{
    switch (hat_err) {
        case HAT_ERR_HVPAK_NO_DEVICE:        return -CMD_ERR_HARDWARE;
        case HAT_ERR_HVPAK_TIMEOUT:          return -CMD_ERR_TIMEOUT;
        case HAT_ERR_HVPAK_UNKNOWN_IDENTITY: return -CMD_ERR_INVALID_STATE;
        case HAT_ERR_HVPAK_UNSUPPORTED_CAP:  return -CMD_ERR_INVALID_STATE;
        case HAT_ERR_HVPAK_INVALID_INDEX:    return -CMD_ERR_OUT_OF_RANGE;
        case HAT_ERR_HVPAK_UNSAFE_REG:       return -CMD_ERR_OUT_OF_RANGE;
        case HAT_ERR_HVPAK_INVALID_ARG:      return -CMD_ERR_BAD_ARG;
        case HAT_ERR_BUSY:                   return -CMD_ERR_BUSY;
        default:                             return -CMD_ERR_BAD_ARG;
    }
}

// ---------------------------------------------------------------------------
// Helper: generic HVPAK passthrough.
// Mirrors legacy handleHatHvpakPassthrough (bbp.cpp:1756-1798).
// Response is memcpy of rsp[rsp_len] — preserving raw HAT bytes.
// ---------------------------------------------------------------------------
static int hvpak_passthrough(uint8_t hat_cmd,
                             const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len,
                             uint32_t timeout_ms)
{
    uint8_t rsp[32] = {};
    uint8_t rsp_len = 0;

    if (!hat_hvpak_request(hat_cmd, payload, (uint8_t)len, rsp, &rsp_len, timeout_ms, sizeof(rsp))) {
        return hat_err_to_cmd_err(hat_get_last_error());
    }

    // memcpy echo — raw HAT response bytes (bbp.cpp:1797)
    memcpy(resp, rsp, rsp_len);
    *resp_len = (size_t)rsp_len;
    return (int)rsp_len;
}

// ---------------------------------------------------------------------------
// HAT_GET_STATUS  payload: (none)
// resp: bool detected, bool connected, u8 type, f32 detect_voltage,
//       u8 fw_major, u8 fw_minor, bool config_confirmed,
//       HAT_NUM_EXT_PINS u8 pin_config[],
//       2x (bool enabled, f32 current_ma, bool fault),
//       u16 io_voltage_mv, u8 hvpak_part, bool hvpak_ready, u8 hvpak_last_error,
//       bool dap_connected, bool target_detected, u32 target_dpidr
// Wire format matches legacy handleHatGetStatus (bbp.cpp:1578-1613).
// Calls hat_get_dap_status() if connected, matching legacy.
// ---------------------------------------------------------------------------
static int handler_hat_get_status(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    // Refresh DAP status if connected (matching legacy bbp.cpp:1581-1583)
    if (hat_get_state()->connected) {
        hat_get_dap_status();
    }

    const HatState *hs = hat_get_state();
    size_t pos = 0;
    bbp_put_bool(resp, &pos, hs->detected);
    bbp_put_bool(resp, &pos, hs->connected);
    bbp_put_u8(resp, &pos, (uint8_t)hs->type);
    bbp_put_f32(resp, &pos, hs->detect_voltage);
    bbp_put_u8(resp, &pos, hs->fw_version_major);
    bbp_put_u8(resp, &pos, hs->fw_version_minor);
    bbp_put_bool(resp, &pos, hs->config_confirmed);
    for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
        bbp_put_u8(resp, &pos, (uint8_t)hs->pin_config[i]);
    }
    for (int i = 0; i < 2; i++) {
        bbp_put_bool(resp, &pos, hs->connector[i].enabled);
        bbp_put_f32(resp, &pos, hs->connector[i].current_ma);
        bbp_put_bool(resp, &pos, hs->connector[i].fault);
    }
    bbp_put_u16(resp, &pos, hs->io_voltage_mv);
    bbp_put_u8(resp, &pos, hs->hvpak_part);
    bbp_put_bool(resp, &pos, hs->hvpak_ready);
    bbp_put_u8(resp, &pos, hs->hvpak_last_error);
    bbp_put_bool(resp, &pos, hs->dap_connected);
    bbp_put_bool(resp, &pos, hs->target_detected);
    bbp_put_u32(resp, &pos, hs->target_dpidr);

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_SET_PIN  payload: u8 pin, u8 func  → resp: u8 pin, u8 func, bool confirmed
// Wire format matches legacy handleHatSetPin (bbp.cpp:1616-1639).
// ---------------------------------------------------------------------------
static int handler_hat_set_pin(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t pin  = bbp_get_u8(payload, &rpos);
    uint8_t func = bbp_get_u8(payload, &rpos);

    if (pin >= HAT_NUM_EXT_PINS || func >= HAT_FUNC_COUNT) return -CMD_ERR_OUT_OF_RANGE;

    if (!hat_set_pin(pin, (HatPinFunction)func)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, pin);
    bbp_put_u8(resp, &pos, func);
    bbp_put_bool(resp, &pos, true);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_SET_ALL_PINS  payload: HAT_NUM_EXT_PINS u8 funcs
// resp: HAT_NUM_EXT_PINS u8 funcs, bool confirmed
// Wire format matches legacy handleHatSetAllPins (bbp.cpp:1642-1668).
// ---------------------------------------------------------------------------
static int handler_hat_set_all_pins(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    if (len < HAT_NUM_EXT_PINS) return -CMD_ERR_BAD_ARG;

    HatPinFunction config[HAT_NUM_EXT_PINS];
    size_t rpos = 0;
    for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
        config[i] = (HatPinFunction)bbp_get_u8(payload, &rpos);
        if (config[i] >= HAT_FUNC_COUNT) return -CMD_ERR_OUT_OF_RANGE;
    }

    if (!hat_set_all_pins(config)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
        bbp_put_u8(resp, &pos, (uint8_t)config[i]);
    }
    bbp_put_bool(resp, &pos, true);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_RESET  payload: (none)  resp: (none)
// Wire format matches legacy handleHatReset (bbp.cpp:1671-1678).
// ---------------------------------------------------------------------------
static int handler_hat_reset(const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    if (!hat_reset()) return -CMD_ERR_BUSY;
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HAT_DETECT  payload: (none)
// resp: bool detected, u8 type, f32 detect_voltage, bool connected
// Wire format matches legacy handleHatDetect (bbp.cpp:1681-1696).
// ---------------------------------------------------------------------------
static int handler_hat_detect(const uint8_t *payload, size_t len,
                              uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    hat_detect();
    const HatState *hs = hat_get_state();

    // If newly detected, try to connect (matching legacy bbp.cpp:1686-1689)
    if (hs->detected && !hs->connected) {
        hat_connect();
    }

    size_t pos = 0;
    bbp_put_bool(resp, &pos, hs->detected);
    bbp_put_u8(resp, &pos, (uint8_t)hs->type);
    bbp_put_f32(resp, &pos, hs->detect_voltage);
    bbp_put_bool(resp, &pos, hs->connected);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_SET_POWER  payload: u8 conn(0-1), u8 on  → resp: u8 conn, bool on
// Wire format matches legacy handleHatSetPower (bbp.cpp:1701-1716).
// ---------------------------------------------------------------------------
static int handler_hat_set_power(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t conn = bbp_get_u8(payload, &rpos);
    uint8_t on   = bbp_get_u8(payload, &rpos);
    if (conn > 1) return -CMD_ERR_OUT_OF_RANGE;

    if (!hat_set_power((HatConnector)conn, on != 0)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, conn);
    bbp_put_bool(resp, &pos, on != 0);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_GET_POWER  payload: (none)
// resp: 2x (bool enabled, f32 current_ma, bool fault),
//       u16 io_voltage_mv, u8 hvpak_part, bool hvpak_ready, u8 hvpak_last_error
// Wire format matches legacy handleHatGetPower (bbp.cpp:1719-1734).
// Calls hat_get_power_status() to refresh, matching legacy.
// ---------------------------------------------------------------------------
static int handler_hat_get_power(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    hat_get_power_status();  // Live read (matching legacy bbp.cpp:1721)
    const HatState *hs = hat_get_state();

    size_t pos = 0;
    for (int i = 0; i < 2; i++) {
        bbp_put_bool(resp, &pos, hs->connector[i].enabled);
        bbp_put_f32(resp, &pos, hs->connector[i].current_ma);
        bbp_put_bool(resp, &pos, hs->connector[i].fault);
    }
    bbp_put_u16(resp, &pos, hs->io_voltage_mv);
    bbp_put_u8(resp, &pos, hs->hvpak_part);
    bbp_put_bool(resp, &pos, hs->hvpak_ready);
    bbp_put_u8(resp, &pos, hs->hvpak_last_error);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_SET_IO_VOLTAGE  payload: u16 mv
// resp: u16 mv_requested, u16 io_voltage_mv, u8 hvpak_part, bool hvpak_ready,
//       u8 hvpak_last_error
// Wire format matches legacy handleHatSetIoVoltage (bbp.cpp:1737-1753).
// ---------------------------------------------------------------------------
static int handler_hat_set_io_voltage(const uint8_t *payload, size_t len,
                                      uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint16_t mv = bbp_get_u16(payload, &rpos);

    if (!hat_set_io_voltage(mv)) return -CMD_ERR_BAD_ARG;

    size_t pos = 0;
    bbp_put_u16(resp, &pos, mv);
    bbp_put_u16(resp, &pos, hat_get_state()->io_voltage_mv);
    bbp_put_u8(resp, &pos, hat_get_state()->hvpak_part);
    bbp_put_bool(resp, &pos, hat_get_state()->hvpak_ready);
    bbp_put_u8(resp, &pos, hat_get_state()->hvpak_last_error);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_SETUP_SWD  payload: u16 voltage_mv, u8 conn(0-1)
// resp: bool ok, u16 voltage_mv, u8 conn
// Wire format matches legacy handleHatSetupSwd (bbp.cpp:1801-1817).
// ---------------------------------------------------------------------------
static int handler_hat_setup_swd(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint16_t voltage_mv = bbp_get_u16(payload, &rpos);
    uint8_t  conn       = bbp_get_u8(payload, &rpos);
    if (conn > 1) return -CMD_ERR_BAD_ARG;

    if (!hat_setup_swd(voltage_mv, (HatConnector)conn)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_bool(resp, &pos, true);
    bbp_put_u16(resp, &pos, voltage_mv);
    bbp_put_u8(resp, &pos, conn);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_LA_CONFIG  payload: u8 channels, u32 rate_hz, u32 depth
// resp: u8 channels, u32 rate_hz, u32 depth
// Wire format matches legacy handleHatLaConfig (bbp.cpp:1822-1843).
// ---------------------------------------------------------------------------
static int handler_hat_la_config(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 9) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  channels = bbp_get_u8(payload, &rpos);
    uint32_t rate_hz  = bbp_get_u32(payload, &rpos);
    uint32_t depth    = bbp_get_u32(payload, &rpos);

    if (!hat_la_configure(channels, rate_hz, depth)) {
        uint8_t hat_err = hat_get_last_error();
        return (hat_err == HAT_ERR_BUSY) ? -CMD_ERR_BUSY : -CMD_ERR_TIMEOUT;
    }

    size_t pos = 0;
    bbp_put_u8(resp, &pos, channels);
    bbp_put_u32(resp, &pos, rate_hz);
    bbp_put_u32(resp, &pos, depth);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_LA_TRIGGER  payload: u8 mode, u8 value  → resp: (none)
// Wire format matches legacy handleHatLaTrigger (bbp.cpp:1846-1858).
// ---------------------------------------------------------------------------
static int handler_hat_la_trigger(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    (void)resp;
    if (!hat_la_set_trigger(payload[0], payload[1])) {
        uint8_t hat_err = hat_get_last_error();
        return (hat_err == HAT_ERR_BUSY) ? -CMD_ERR_BUSY : -CMD_ERR_TIMEOUT;
    }
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HAT_LA_ARM  payload: (none)  resp: (none)
// Wire format matches legacy handleHatLaArm (bbp.cpp:1861-1872).
// ---------------------------------------------------------------------------
static int handler_hat_la_arm(const uint8_t *payload, size_t len,
                              uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    if (!hat_la_arm()) {
        uint8_t hat_err = hat_get_last_error();
        return (hat_err == HAT_ERR_BUSY) ? -CMD_ERR_BUSY : -CMD_ERR_TIMEOUT;
    }
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HAT_LA_FORCE  payload: (none)  resp: (none)
// Wire format matches legacy handleHatLaForce (bbp.cpp:1874-1884).
// ---------------------------------------------------------------------------
static int handler_hat_la_force(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    if (!hat_la_force()) {
        uint8_t hat_err = hat_get_last_error();
        return (hat_err == HAT_ERR_BUSY) ? -CMD_ERR_BUSY : -CMD_ERR_TIMEOUT;
    }
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HAT_LA_STOP  payload: (none)  resp: (none)
// Wire format matches legacy handleHatLaStop (bbp.cpp:1887-1893).
// ---------------------------------------------------------------------------
static int handler_hat_la_stop(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    if (!hat_la_stop()) return -CMD_ERR_TIMEOUT;
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HAT_LA_STATUS  payload: (none)
// resp: u8 state, u8 channels, u32 samples_captured, u32 total_samples,
//       u32 actual_rate_hz, u8 usb_connected, u8 usb_mounted,
//       u8 stream_stop_reason, u32 stream_overrun_count,
//       u32 stream_short_write_count, u8 usb_rearm_pending,
//       u8 usb_rearm_request_count, u8 usb_rearm_complete_count
// Wire format matches legacy handleHatLaStatus (bbp.cpp:1925-1946).
// ---------------------------------------------------------------------------
static int handler_hat_la_status(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    HatLaStatus st = {};
    if (!hat_la_get_status(&st)) return -CMD_ERR_TIMEOUT;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, st.state);
    bbp_put_u8(resp, &pos, st.channels);
    bbp_put_u32(resp, &pos, st.samples_captured);
    bbp_put_u32(resp, &pos, st.total_samples);
    bbp_put_u32(resp, &pos, st.actual_rate_hz);
    bbp_put_u8(resp, &pos, st.usb_connected);
    bbp_put_u8(resp, &pos, st.usb_mounted);
    bbp_put_u8(resp, &pos, st.stream_stop_reason);
    bbp_put_u32(resp, &pos, st.stream_overrun_count);
    bbp_put_u32(resp, &pos, st.stream_short_write_count);
    bbp_put_u8(resp, &pos, st.usb_rearm_pending);
    bbp_put_u8(resp, &pos, st.usb_rearm_request_count);
    bbp_put_u8(resp, &pos, st.usb_rearm_complete_count);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_LA_READ  payload: u32 offset, u16 read_len
// resp: u32 offset, u8 actual_len, then actual_len bytes
// Wire format matches legacy handleHatLaRead (bbp.cpp:1949-1972).
// read_len clamped to 28 bytes per chunk (matching legacy bbp.cpp:1959).
// ---------------------------------------------------------------------------
static int handler_hat_la_read(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    if (len < 6) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint32_t offset   = bbp_get_u32(payload, &rpos);
    uint16_t read_len = bbp_get_u16(payload, &rpos);
    if (read_len > 256) read_len = 256;

    uint8_t chunk[256];
    // Clamp to 28 bytes per chunk matching legacy bbp.cpp:1959
    uint8_t actual = hat_la_read_data(offset, chunk, (uint8_t)(read_len > 28 ? 28 : read_len));

    if (read_len > 0 && actual == 0) return -CMD_ERR_TIMEOUT;

    size_t pos = 0;
    bbp_put_u32(resp, &pos, offset);
    bbp_put_u8(resp, &pos, actual);
    memcpy(&resp[pos], chunk, actual);
    pos += actual;
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// HAT_LA_LOG_ENABLE  payload: u8 enable  → resp: (none)
// Wire format matches legacy handleHatLaLogEnable (bbp.cpp:1914-1922).
// ---------------------------------------------------------------------------
static int handler_hat_la_log_enable(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    (void)resp;
    if (!hat_la_log_enable(payload[0] != 0)) return -CMD_ERR_TIMEOUT;
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HAT_LA_USB_RESET  payload: (none)  resp: (none)
// Wire format matches legacy handleHatLaUsbReset (bbp.cpp:1896-1902).
// ---------------------------------------------------------------------------
static int handler_hat_la_usb_reset(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    if (!hat_la_usb_reset()) return -CMD_ERR_TIMEOUT;
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HAT_LA_STREAM_START  payload: (none)  resp: (none)
// Wire format matches legacy handleHatLaStreamStart (bbp.cpp:1905-1911).
// ---------------------------------------------------------------------------
static int handler_hat_la_stream_start(const uint8_t *payload, size_t len,
                                       uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    if (!hat_la_stream_start()) return -CMD_ERR_TIMEOUT;
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// HVPAK passthrough handlers (all delegate to hvpak_passthrough helper).
// Wire format: raw bytes from HAT UART (memcpy echo from rsp buffer).
// Legacy: handleHatHvpakPassthrough (bbp.cpp:1756-1798).
// ---------------------------------------------------------------------------
static int handler_hat_get_hvpak_info(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_GET_HVPAK_INFO, p, l, r, rl, 200); }

static int handler_hat_get_hvpak_caps(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_GET_HVPAK_CAPS, p, l, r, rl, 200); }

static int handler_hat_get_hvpak_lut(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_GET_HVPAK_LUT, p, l, r, rl, 200); }

static int handler_hat_set_hvpak_lut(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_SET_HVPAK_LUT, p, l, r, rl, 200); }

static int handler_hat_get_hvpak_bridge(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_GET_HVPAK_BRIDGE, p, l, r, rl, 200); }

static int handler_hat_set_hvpak_bridge(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_SET_HVPAK_BRIDGE, p, l, r, rl, 200); }

static int handler_hat_get_hvpak_analog(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_GET_HVPAK_ANALOG, p, l, r, rl, 200); }

static int handler_hat_set_hvpak_analog(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_SET_HVPAK_ANALOG, p, l, r, rl, 200); }

static int handler_hat_get_hvpak_pwm(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_GET_HVPAK_PWM, p, l, r, rl, 200); }

static int handler_hat_set_hvpak_pwm(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_SET_HVPAK_PWM, p, l, r, rl, 200); }

static int handler_hat_hvpak_reg_read(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_HVPAK_REG_READ, p, l, r, rl, 200); }

static int handler_hat_hvpak_reg_write_masked(const uint8_t *p, size_t l, uint8_t *r, size_t *rl)
{ return hvpak_passthrough(HAT_CMD_HVPAK_REG_WRITE_MASKED, p, l, r, rl, 200); }

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// hat_get_status, hat_get_power, hat_la_status, hat_la_read, hvpak passthroughs:
// complex/variable/opaque responses — rsp=NULL.
// hat_set_all_pins: variable-length (HAT_NUM_EXT_PINS bytes) — args=NULL.

static const ArgSpec s_hat_set_pin_args[] = {
    { "pin",  ARG_U8, true, 0, 255 },
    { "func", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_hat_set_pin_rsp[] = {
    { "pin",       ARG_U8,   true, 0, 0 },
    { "func",      ARG_U8,   true, 0, 0 },
    { "confirmed", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_hat_detect_rsp[] = {
    { "detected",       ARG_BOOL, true, 0, 0 },
    { "type",           ARG_U8,   true, 0, 0 },
    { "detect_voltage", ARG_F32,  true, 0, 0 },
    { "connected",      ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_hat_set_power_args[] = {
    { "conn", ARG_U8, true, 0, 1 },
    { "on",   ARG_U8, true, 0, 1 },
};
static const ArgSpec s_hat_set_power_rsp[] = {
    { "conn", ARG_U8,   true, 0, 0 },
    { "on",   ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_hat_set_io_voltage_args[] = {
    { "mv", ARG_U16, true, 0, 5000 },
};

static const ArgSpec s_hat_setup_swd_args[] = {
    { "voltage_mv", ARG_U16, true, 0, 5000 },
    { "conn",       ARG_U8,  true, 0, 1 },
};
static const ArgSpec s_hat_setup_swd_rsp[] = {
    { "ok",         ARG_BOOL, true, 0, 0 },
    { "voltage_mv", ARG_U16,  true, 0, 0 },
    { "conn",       ARG_U8,   true, 0, 0 },
};

static const ArgSpec s_hat_la_config_args[] = {
    { "channels", ARG_U8,  true, 0, 255 },
    { "rate_hz",  ARG_U32, true, 0, 0 },
    { "depth",    ARG_U32, true, 0, 0 },
};
static const ArgSpec s_hat_la_config_rsp[] = {
    { "channels", ARG_U8,  true, 0, 0 },
    { "rate_hz",  ARG_U32, true, 0, 0 },
    { "depth",    ARG_U32, true, 0, 0 },
};

static const ArgSpec s_hat_la_trigger_args[] = {
    { "mode",  ARG_U8, true, 0, 255 },
    { "value", ARG_U8, true, 0, 255 },
};

static const ArgSpec s_hat_la_read_args[] = {
    { "offset",   ARG_U32, true, 0, 0 },
    { "read_len", ARG_U16, true, 0, 256 },
};

static const ArgSpec s_hat_la_log_enable_args[] = {
    { "enable", ARG_U8, true, 0, 1 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_hat_cmds[] = {
    { BBP_CMD_HAT_GET_STATUS,            "hat_get_status",
      NULL,                    0, NULL,                   0, handler_hat_get_status,            CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_SET_PIN,               "hat_set_pin",
      s_hat_set_pin_args,      2, s_hat_set_pin_rsp,     3, handler_hat_set_pin,               0                   },
    { BBP_CMD_HAT_SET_ALL_PINS,          "hat_set_all_pins",
      NULL,                    0, NULL,                   0, handler_hat_set_all_pins,          0                   },
    { BBP_CMD_HAT_RESET,                 "hat_reset",
      NULL,                    0, NULL,                   0, handler_hat_reset,                 0                   },
    { BBP_CMD_HAT_DETECT,                "hat_detect",
      NULL,                    0, s_hat_detect_rsp,      4, handler_hat_detect,                0                   },
    { BBP_CMD_HAT_SET_POWER,             "hat_set_power",
      s_hat_set_power_args,    2, s_hat_set_power_rsp,   2, handler_hat_set_power,             0                   },
    { BBP_CMD_HAT_GET_POWER,             "hat_get_power",
      NULL,                    0, NULL,                   0, handler_hat_get_power,             CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_SET_IO_VOLTAGE,        "hat_set_io_voltage",
      s_hat_set_io_voltage_args, 1, NULL,                0, handler_hat_set_io_voltage,        0                   },
    { BBP_CMD_HAT_SETUP_SWD,             "hat_setup_swd",
      s_hat_setup_swd_args,    2, s_hat_setup_swd_rsp,   3, handler_hat_setup_swd,             0                   },
    { BBP_CMD_HAT_GET_HVPAK_INFO,        "hat_get_hvpak_info",
      NULL,                    0, NULL,                   0, handler_hat_get_hvpak_info,        CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_LA_CONFIG,             "hat_la_config",
      s_hat_la_config_args,    3, s_hat_la_config_rsp,   3, handler_hat_la_config,             0                   },
    { BBP_CMD_HAT_LA_ARM,                "hat_la_arm",
      NULL,                    0, NULL,                   0, handler_hat_la_arm,                0                   },
    { BBP_CMD_HAT_LA_FORCE,              "hat_la_force",
      NULL,                    0, NULL,                   0, handler_hat_la_force,              0                   },
    { BBP_CMD_HAT_LA_STATUS,             "hat_la_status",
      NULL,                    0, NULL,                   0, handler_hat_la_status,             CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_LA_READ,               "hat_la_read",
      s_hat_la_read_args,      2, NULL,                   0, handler_hat_la_read,               CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_LA_STOP,               "hat_la_stop",
      NULL,                    0, NULL,                   0, handler_hat_la_stop,               0                   },
    { BBP_CMD_HAT_LA_TRIGGER,            "hat_la_trigger",
      s_hat_la_trigger_args,   2, NULL,                   0, handler_hat_la_trigger,            0                   },
    { BBP_CMD_HAT_GET_HVPAK_CAPS,        "hat_get_hvpak_caps",
      NULL,                    0, NULL,                   0, handler_hat_get_hvpak_caps,        CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_GET_HVPAK_LUT,         "hat_get_hvpak_lut",
      NULL,                    0, NULL,                   0, handler_hat_get_hvpak_lut,         CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_SET_HVPAK_LUT,         "hat_set_hvpak_lut",
      NULL,                    0, NULL,                   0, handler_hat_set_hvpak_lut,         0                   },
    { BBP_CMD_HAT_GET_HVPAK_BRIDGE,      "hat_get_hvpak_bridge",
      NULL,                    0, NULL,                   0, handler_hat_get_hvpak_bridge,      CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_SET_HVPAK_BRIDGE,      "hat_set_hvpak_bridge",
      NULL,                    0, NULL,                   0, handler_hat_set_hvpak_bridge,      0                   },
    { BBP_CMD_HAT_GET_HVPAK_ANALOG,      "hat_get_hvpak_analog",
      NULL,                    0, NULL,                   0, handler_hat_get_hvpak_analog,      CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_SET_HVPAK_ANALOG,      "hat_set_hvpak_analog",
      NULL,                    0, NULL,                   0, handler_hat_set_hvpak_analog,      0                   },
    { BBP_CMD_HAT_GET_HVPAK_PWM,         "hat_get_hvpak_pwm",
      NULL,                    0, NULL,                   0, handler_hat_get_hvpak_pwm,         CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_SET_HVPAK_PWM,         "hat_set_hvpak_pwm",
      NULL,                    0, NULL,                   0, handler_hat_set_hvpak_pwm,         0                   },
    { BBP_CMD_HAT_HVPAK_REG_READ,        "hat_hvpak_reg_read",
      NULL,                    0, NULL,                   0, handler_hat_hvpak_reg_read,        CMD_FLAG_READS_STATE },
    { BBP_CMD_HAT_HVPAK_REG_WRITE_MASKED,"hat_hvpak_reg_write_masked",
      NULL,                    0, NULL,                   0, handler_hat_hvpak_reg_write_masked, 0                  },
    { BBP_CMD_HAT_LA_LOG_ENABLE,         "hat_la_log_enable",
      s_hat_la_log_enable_args, 1, NULL,                  0, handler_hat_la_log_enable,         0                   },
    { BBP_CMD_HAT_LA_USB_RESET,          "hat_la_usb_reset",
      NULL,                    0, NULL,                   0, handler_hat_la_usb_reset,          0                   },
    { BBP_CMD_HAT_LA_STREAM_START,       "hat_la_stream_start",
      NULL,                    0, NULL,                   0, handler_hat_la_stream_start,       CMD_FLAG_STREAMING  },
};

extern "C" void register_cmds_hat(void)
{
    cmd_registry_register_block(s_hat_cmds,
        sizeof(s_hat_cmds) / sizeof(s_hat_cmds[0]));
}
