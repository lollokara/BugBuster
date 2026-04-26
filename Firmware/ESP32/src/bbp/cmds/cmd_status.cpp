// =============================================================================
// cmd_status.cpp — Registry handlers for status/info/GPIO/UART/alert/diag commands
//
//   BBP_CMD_GET_STATUS          (0x01)
//   BBP_CMD_GET_DEVICE_INFO     (0x02)
//   BBP_CMD_GET_FAULTS          (0x03)
//   BBP_CMD_GET_DIAGNOSTICS     (0x04)
//   BBP_CMD_GET_GPIO_STATUS     (0x40)
//   BBP_CMD_SET_GPIO_CONFIG     (0x41)
//   BBP_CMD_SET_GPIO_VALUE      (0x42)
//   BBP_CMD_ADC_LEDS_SET_MODE   (0x47)
//   BBP_CMD_GET_UART_CONFIG     (0x50)
//   BBP_CMD_SET_UART_CONFIG     (0x51)
//   BBP_CMD_GET_UART_PINS       (0x52)
//   BBP_CMD_CLEAR_ALL_ALERTS    (0x20)
//   BBP_CMD_CLEAR_CH_ALERT      (0x21)
//   BBP_CMD_SET_ALERT_MASK      (0x22)
//   BBP_CMD_SET_CH_ALERT_MASK   (0x23)
//   BBP_CMD_SET_DIAG_CONFIG     (0x30)
//   BBP_CMD_SET_DO_CONFIG       (0x16)
//   BBP_CMD_SET_DO_STATE        (0x17)
//   BBP_CMD_SET_ILIMIT          (0x19)
//   BBP_CMD_SET_AVDD_SEL        (0x1A)
//   BBP_CMD_MUX_SET_ALL         (0x90)
//   BBP_CMD_MUX_GET_ALL         (0x91)
//   BBP_CMD_MUX_SET_SWITCH      (0x92)
//   BBP_CMD_SET_LSHIFT_OE       (0xE0)
//   BBP_CMD_GET_ADMIN_TOKEN     (0x74)
//   BBP_CMD_PING                (0xFE)
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "tasks.h"
#include "state_lock.h"
#include "uart_bridge.h"
#include "adc_leds.h"
#include "adgs2414d.h"
#include "auth.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"

// pin_write is declared in config.h / hal, forward-declared if needed
extern void pin_write(int pin, int value);

// tasks_apply_gpio_config / tasks_apply_gpio_output (declared in tasks.h)
extern bool tasks_apply_gpio_config(uint8_t gpio, GpioSelect mode, bool pulldown);
extern bool tasks_apply_gpio_output(uint8_t gpio, bool value);

static const char *TAG = "cmd_status";

// millis_now() lives in bbp.cpp (static), replicate via esp_timer here
static inline uint32_t status_millis_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ---------------------------------------------------------------------------
// GET_STATUS  payload: (none)
// Wire format matches legacy handleGetStatus (bbp.cpp:326-386).
// Reads g_deviceState under g_stateMutex (50ms).
// ---------------------------------------------------------------------------
static int handler_get_status(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    ScopedStateLock lock;
    if (!lock.held()) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_bool(resp, &pos, g_deviceState.spiOk);
    bbp_put_f32(resp, &pos, g_deviceState.dieTemperature);
    bbp_put_u16(resp, &pos, g_deviceState.alertStatus);
    bbp_put_u16(resp, &pos, g_deviceState.alertMask);
    bbp_put_u16(resp, &pos, g_deviceState.supplyAlertStatus);
    bbp_put_u16(resp, &pos, g_deviceState.supplyAlertMask);
    bbp_put_u16(resp, &pos, g_deviceState.liveStatus);

    for (uint8_t ch = 0; ch < 4; ch++) {
        const ChannelState &cs = g_deviceState.channels[ch];
        bbp_put_u8(resp, &pos, ch);
        bbp_put_u8(resp, &pos, (uint8_t)cs.function);
        bbp_put_u24(resp, &pos, cs.adcRawCode);
        bbp_put_f32(resp, &pos, cs.adcValue);
        bbp_put_u8(resp, &pos, (uint8_t)cs.adcRange);
        bbp_put_u8(resp, &pos, (uint8_t)cs.adcRate);
        bbp_put_u8(resp, &pos, (uint8_t)cs.adcMux);
        bbp_put_u16(resp, &pos, cs.dacCode);
        bbp_put_f32(resp, &pos, cs.dacValue);
        bbp_put_bool(resp, &pos, cs.dinState);
        bbp_put_u32(resp, &pos, cs.dinCounter);
        bbp_put_bool(resp, &pos, cs.doState);
        bbp_put_u16(resp, &pos, cs.channelAlertStatus);
        bbp_put_u16(resp, &pos, cs.channelAlertMask);
        bbp_put_u16(resp, &pos, cs.rtdExcitationUa);
    }

    for (uint8_t d = 0; d < 4; d++) {
        bbp_put_u8(resp, &pos, g_deviceState.diag[d].source);
        bbp_put_u16(resp, &pos, g_deviceState.diag[d].rawCode);
        bbp_put_f32(resp, &pos, g_deviceState.diag[d].value);
    }

    uint8_t muxStates[ADGS_API_MAIN_DEVICES] = {};
    adgs_get_api_states(muxStates);
    for (uint8_t m = 0; m < ADGS_API_MAIN_DEVICES; m++) {
        bbp_put_u8(resp, &pos, muxStates[m]);
    }

    for (uint8_t g = 0; g < 12; g++) {
        bbp_put_u8(resp, &pos, g_deviceState.dio[g].mode);
        bbp_put_bool(resp, &pos, g_deviceState.dio[g].outputVal);
        bbp_put_bool(resp, &pos, g_deviceState.dio[g].inputVal);
        bbp_put_bool(resp, &pos, g_deviceState.dio[g].pulldown);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// GET_DEVICE_INFO  payload: (none)
// resp: bool spiOk, u8 siliconRev, u16 id0, u16 id1
// Wire format matches legacy handleGetDeviceInfo (bbp.cpp:388-408).
// Live SPI reads — uses bbp_spi_read_register() thin wrapper added in bbp.cpp.
// ---------------------------------------------------------------------------
static int handler_get_device_info(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    uint16_t siliconRev = 0, id0 = 0, id1 = 0;
    bool spiOk = true;
    spiOk &= bbp_spi_read_reg(0x46, &siliconRev);
    spiOk &= bbp_spi_read_reg(0x47, &id0);
    spiOk &= bbp_spi_read_reg(0x48, &id1);
    if (!spiOk) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_bool(resp, &pos, g_deviceState.spiOk);
    bbp_put_u8(resp, &pos, (uint8_t)(siliconRev & 0xFF));
    bbp_put_u16(resp, &pos, id0);
    bbp_put_u16(resp, &pos, id1);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// GET_FAULTS  payload: (none)
// resp: u16 alertStatus, u16 alertMask, u16 supplyAlertStatus, u16 supplyAlertMask,
//       then 4x (u8 ch, u16 channelAlertStatus, u16 channelAlertMask)
// Wire format matches legacy handleGetFaults (bbp.cpp:410-431).
// ---------------------------------------------------------------------------
static int handler_get_faults(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    ScopedStateLock lock;
    if (!lock.held()) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u16(resp, &pos, g_deviceState.alertStatus);
    bbp_put_u16(resp, &pos, g_deviceState.alertMask);
    bbp_put_u16(resp, &pos, g_deviceState.supplyAlertStatus);
    bbp_put_u16(resp, &pos, g_deviceState.supplyAlertMask);

    for (uint8_t ch = 0; ch < 4; ch++) {
        bbp_put_u8(resp, &pos, ch);
        bbp_put_u16(resp, &pos, g_deviceState.channels[ch].channelAlertStatus);
        bbp_put_u16(resp, &pos, g_deviceState.channels[ch].channelAlertMask);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// GET_DIAGNOSTICS  payload: (none)
// resp: 4x (u8 slot, u8 source, u16 rawCode, f32 value)
// Wire format matches legacy handleGetDiagnostics (bbp.cpp:433-450).
// ---------------------------------------------------------------------------
static int handler_get_diagnostics(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    ScopedStateLock lock;
    if (!lock.held()) return -CMD_ERR_BUSY;

    size_t pos = 0;
    for (uint8_t d = 0; d < 4; d++) {
        bbp_put_u8(resp, &pos, d);
        bbp_put_u8(resp, &pos, g_deviceState.diag[d].source);
        bbp_put_u16(resp, &pos, g_deviceState.diag[d].rawCode);
        bbp_put_f32(resp, &pos, g_deviceState.diag[d].value);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// GET_GPIO_STATUS  payload: (none)
// resp: 12x (u8 gpio, u8 mode, bool outputVal, bool inputVal, bool pulldown)
// Wire format matches legacy handleGetGpioStatus (bbp.cpp:500-518).
// ---------------------------------------------------------------------------
static int handler_get_gpio_status(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    ScopedStateLock lock;
    if (!lock.held()) return -CMD_ERR_BUSY;

    size_t pos = 0;
    for (uint8_t g = 0; g < 12; g++) {
        bbp_put_u8(resp, &pos, g);
        bbp_put_u8(resp, &pos, g_deviceState.dio[g].mode);
        bbp_put_bool(resp, &pos, g_deviceState.dio[g].outputVal);
        bbp_put_bool(resp, &pos, g_deviceState.dio[g].inputVal);
        bbp_put_bool(resp, &pos, g_deviceState.dio[g].pulldown);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SET_GPIO_CONFIG  payload: u8 gpio, u8 mode, bool pulldown  → resp: same 3 bytes
// Wire format matches legacy handleSetGpioConfig (bbp.cpp:911-927).
// memcpy echo (bbp.cpp:925).
// ---------------------------------------------------------------------------
static int handler_set_gpio_config(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t gpio = bbp_get_u8(payload, &rpos);
    uint8_t mode = bbp_get_u8(payload, &rpos);
    bool pulldown = bbp_get_bool(payload, &rpos);
    if (gpio >= 12) return -CMD_ERR_OUT_OF_RANGE;

    if (!tasks_apply_gpio_config(gpio, (GpioSelect)mode, pulldown))
        return -CMD_ERR_OUT_OF_RANGE;

    // memcpy echo matching legacy bbp.cpp:925
    memcpy(resp, payload, 3);
    *resp_len = 3;
    return 3;
}

// ---------------------------------------------------------------------------
// SET_GPIO_VALUE  payload: u8 gpio, bool value  → resp: same 2 bytes
// Wire format matches legacy handleSetGpioValue (bbp.cpp:929-947).
// memcpy echo (bbp.cpp:946). Also suspends LED auto-updates.
// ---------------------------------------------------------------------------
static int handler_set_gpio_value(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t gpio = bbp_get_u8(payload, &rpos);
    bool value   = bbp_get_bool(payload, &rpos);
    if (gpio >= 12) return -CMD_ERR_OUT_OF_RANGE;

    if (!tasks_apply_gpio_output(gpio, value))
        return -CMD_ERR_OUT_OF_RANGE;

    // Suspend auto LED updates (legacy bbp.cpp:944)
    adc_leds_set_manual(true);

    // memcpy echo matching legacy bbp.cpp:946
    memcpy(resp, payload, 2);
    *resp_len = 2;
    return 2;
}

// ---------------------------------------------------------------------------
// ADC_LEDS_SET_MODE  payload: u8 mode(0=auto, 1=manual)  → resp: u8 mode
// Wire format matches legacy handleAdcLedsSetMode (bbp.cpp:950-957).
// ---------------------------------------------------------------------------
static int handler_adc_leds_set_mode(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    bool manual = (payload[0] != 0);
    adc_leds_set_manual(manual);
    resp[0] = payload[0];
    *resp_len = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// GET_UART_CONFIG  payload: (none)
// resp: u8 count, then for each bridge: u8 id, u8 uart_num, u8 tx_pin, u8 rx_pin,
//       u32 baudrate, u8 data_bits, u8 parity, u8 stop_bits, bool enabled
// Wire format matches legacy handleGetUartConfig (bbp.cpp:520-538).
// ---------------------------------------------------------------------------
static int handler_get_uart_config(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, CDC_BRIDGE_COUNT);

    for (int i = 0; i < CDC_BRIDGE_COUNT; i++) {
        UartBridgeConfig cfg;
        uart_bridge_get_config(i, &cfg);
        bbp_put_u8(resp, &pos, (uint8_t)i);
        bbp_put_u8(resp, &pos, cfg.uart_num);
        bbp_put_u8(resp, &pos, (uint8_t)cfg.tx_pin);
        bbp_put_u8(resp, &pos, (uint8_t)cfg.rx_pin);
        bbp_put_u32(resp, &pos, cfg.baudrate);
        bbp_put_u8(resp, &pos, cfg.data_bits);
        bbp_put_u8(resp, &pos, cfg.parity);
        bbp_put_u8(resp, &pos, cfg.stop_bits);
        bbp_put_bool(resp, &pos, cfg.enabled);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SET_UART_CONFIG  payload: u8 bridgeId, u8 uart_num, u8 tx_pin, u8 rx_pin,
//                           u32 baudrate, u8 data_bits, u8 parity, u8 stop_bits,
//                           bool enabled  (12 bytes total)
//   resp: same 12 bytes (memcpy echo — bbp.cpp:1189)
// Wire format matches legacy handleSetUartConfig (bbp.cpp:1169-1191).
// ---------------------------------------------------------------------------
static int handler_set_uart_config(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 12) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t bridgeId = bbp_get_u8(payload, &rpos);
    if (bridgeId >= CDC_BRIDGE_COUNT) return -CMD_ERR_OUT_OF_RANGE;

    UartBridgeConfig cfg;
    cfg.uart_num  = bbp_get_u8(payload, &rpos);
    cfg.tx_pin    = (int)bbp_get_u8(payload, &rpos);
    cfg.rx_pin    = (int)bbp_get_u8(payload, &rpos);
    cfg.baudrate  = bbp_get_u32(payload, &rpos);
    cfg.data_bits = bbp_get_u8(payload, &rpos);
    cfg.parity    = bbp_get_u8(payload, &rpos);
    cfg.stop_bits = bbp_get_u8(payload, &rpos);
    cfg.enabled   = bbp_get_bool(payload, &rpos);

    uart_bridge_set_config(bridgeId, &cfg);

    // memcpy echo matching legacy bbp.cpp:1189
    memcpy(resp, payload, 12);
    *resp_len = 12;
    return 12;
}

// ---------------------------------------------------------------------------
// GET_UART_PINS  payload: (none)
// resp: u8 count, then N u8 pin numbers
// Wire format matches legacy handleGetUartPins (bbp.cpp:541-550).
// ---------------------------------------------------------------------------
static int handler_get_uart_pins(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    int pins[40];
    int count = uart_bridge_get_available_pins(pins, 40);
    size_t pos = 0;
    bbp_put_u8(resp, &pos, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        bbp_put_u8(resp, &pos, (uint8_t)pins[i]);
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// CLEAR_ALL_ALERTS  payload: (none)  resp: (none)
// Wire format matches legacy handleClearAllAlerts (bbp.cpp:826-831).
// ---------------------------------------------------------------------------
static int handler_clear_all_alerts(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    Command cmd = {};
    cmd.type = CMD_CLEAR_ALERTS;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// CLEAR_CH_ALERT  payload: u8 ch  → resp: u8 ch
// Wire format matches legacy handleClearChAlert (bbp.cpp:833-848).
// ---------------------------------------------------------------------------
static int handler_clear_ch_alert(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch = bbp_get_u8(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type = CMD_CLEAR_CHANNEL_ALERT;
    cmd.channel = ch;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    resp[0] = ch;
    *resp_len = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// SET_ALERT_MASK  payload: u16 alertMask, u16 supplyMask  → resp: same 4 bytes
// Wire format matches legacy handleSetAlertMask (bbp.cpp:851-870).
// memcpy echo (bbp.cpp:869).
// ---------------------------------------------------------------------------
static int handler_set_alert_mask(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 4) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint16_t alertMask  = bbp_get_u16(payload, &rpos);
    uint16_t supplyMask = bbp_get_u16(payload, &rpos);

    Command cmd = {};
    cmd.type = CMD_SET_ALERT_MASK;
    cmd.maskVal = alertMask;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    Command cmd2 = {};
    cmd2.type = CMD_SET_SUPPLY_ALERT_MASK;
    cmd2.maskVal = supplyMask;
    if (!sendCommand(cmd2)) return -CMD_ERR_BUSY;

    // memcpy echo matching legacy bbp.cpp:869
    memcpy(resp, payload, 4);
    *resp_len = 4;
    return 4;
}

// ---------------------------------------------------------------------------
// SET_CH_ALERT_MASK  payload: u8 ch, u16 mask  → resp: same 3 bytes
// Wire format matches legacy handleSetChAlertMask (bbp.cpp:873-889).
// memcpy echo (bbp.cpp:888).
// ---------------------------------------------------------------------------
static int handler_set_ch_alert_mask(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch   = bbp_get_u8(payload, &rpos);
    uint16_t mask = bbp_get_u16(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type = CMD_SET_CH_ALERT_MASK;
    cmd.channel = ch;
    cmd.maskVal = mask;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // memcpy echo matching legacy bbp.cpp:888
    memcpy(resp, payload, 3);
    *resp_len = 3;
    return 3;
}

// ---------------------------------------------------------------------------
// SET_DIAG_CONFIG  payload: u8 slot, u8 source  → resp: same 2 bytes
// Wire format matches legacy handleSetDiagConfig (bbp.cpp:892-908).
// memcpy echo (bbp.cpp:907).
// ---------------------------------------------------------------------------
static int handler_set_diag_config(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t slot   = bbp_get_u8(payload, &rpos);
    uint8_t source = bbp_get_u8(payload, &rpos);
    if (slot >= 4 || source > 13) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type = CMD_DIAG_CONFIG;
    cmd.diagCfg.slot = slot;
    cmd.diagCfg.source = source;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // memcpy echo matching legacy bbp.cpp:907
    memcpy(resp, payload, 2);
    *resp_len = 2;
    return 2;
}

// ---------------------------------------------------------------------------
// SET_DO_CONFIG  payload: u8 ch, u8 mode, bool srcSelGpio, u8 t1, u8 t2
//   resp: same 5 bytes (memcpy echo — bbp.cpp:746)
// Wire format matches legacy handleSetDoConfig (bbp.cpp:725-748).
// ---------------------------------------------------------------------------
static int handler_set_do_config(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 5) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch          = bbp_get_u8(payload, &rpos);
    uint8_t mode        = bbp_get_u8(payload, &rpos);
    bool    srcSelGpio  = bbp_get_bool(payload, &rpos);
    uint8_t t1          = bbp_get_u8(payload, &rpos);
    uint8_t t2          = bbp_get_u8(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type = CMD_DO_CONFIG;
    cmd.channel = ch;
    cmd.doCfg.mode = mode;
    cmd.doCfg.srcSelGpio = srcSelGpio;
    cmd.doCfg.t1 = t1;
    cmd.doCfg.t2 = t2;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // memcpy echo matching legacy bbp.cpp:746
    memcpy(resp, payload, 5);
    *resp_len = 5;
    return 5;
}

// ---------------------------------------------------------------------------
// SET_DO_STATE  payload: u8 ch, bool on  → resp: same 2 bytes
// Wire format matches legacy handleSetDoState (bbp.cpp:750-767).
// memcpy echo (bbp.cpp:765).
// ---------------------------------------------------------------------------
static int handler_set_do_state(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch = bbp_get_u8(payload, &rpos);
    bool on    = bbp_get_bool(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type = CMD_DO_SET;
    cmd.channel = ch;
    cmd.boolVal = on;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // memcpy echo matching legacy bbp.cpp:765
    memcpy(resp, payload, 2);
    *resp_len = 2;
    return 2;
}

// ---------------------------------------------------------------------------
// SET_ILIMIT  payload: u8 ch, bool limit  → resp: same 2 bytes
// Wire format matches legacy handleSetIlimit (bbp.cpp:788-805).
// memcpy echo (bbp.cpp:804).
// ---------------------------------------------------------------------------
static int handler_set_ilimit(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch  = bbp_get_u8(payload, &rpos);
    bool limit  = bbp_get_bool(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type = CMD_SET_CURRENT_LIMIT;
    cmd.channel = ch;
    cmd.boolVal = limit;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // memcpy echo matching legacy bbp.cpp:804
    memcpy(resp, payload, 2);
    *resp_len = 2;
    return 2;
}

// ---------------------------------------------------------------------------
// SET_AVDD_SEL  payload: u8 ch, u8 sel  → resp: same 2 bytes
// Wire format matches legacy handleSetAvddSel (bbp.cpp:807-824).
// memcpy echo (bbp.cpp:823).
// ---------------------------------------------------------------------------
static int handler_set_avdd_sel(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch  = bbp_get_u8(payload, &rpos);
    uint8_t sel = bbp_get_u8(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type = CMD_SET_AVDD_SELECT;
    cmd.channel = ch;
    cmd.avddSel = sel;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // memcpy echo matching legacy bbp.cpp:823
    memcpy(resp, payload, 2);
    *resp_len = 2;
    return 2;
}

// ---------------------------------------------------------------------------
// MUX_SET_ALL  payload: ADGS_API_MAIN_DEVICES bytes of states
//   resp: ADGS_API_MAIN_DEVICES bytes (actual states after set)
// Wire format matches legacy handleMuxSetAll (bbp.cpp:2040-2055).
// ---------------------------------------------------------------------------
static int handler_mux_set_all(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < ADGS_API_MAIN_DEVICES) return -CMD_ERR_BAD_ARG;

    uint8_t states[ADGS_API_MAIN_DEVICES] = {};
    memcpy(states, payload, ADGS_API_MAIN_DEVICES);
    adgs_set_api_all_safe(states);
    adgs_get_api_states(states);
    memcpy(resp, states, ADGS_API_MAIN_DEVICES);

    // Update cached state (legacy bbp.cpp:2050-2053)
    ScopedStateLock lock;
    if (lock.held()) {
        adgs_get_all_states(g_deviceState.muxState);
    }

    *resp_len = ADGS_API_MAIN_DEVICES;
    return (int)ADGS_API_MAIN_DEVICES;
}

// ---------------------------------------------------------------------------
// MUX_GET_ALL  payload: (none)  resp: ADGS_API_MAIN_DEVICES bytes
// Wire format matches legacy handleMuxGetAll (bbp.cpp:2057-2063).
// ---------------------------------------------------------------------------
static int handler_mux_get_all(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    uint8_t states[ADGS_API_MAIN_DEVICES] = {};
    adgs_get_api_states(states);
    memcpy(resp, states, ADGS_API_MAIN_DEVICES);
    *resp_len = ADGS_API_MAIN_DEVICES;
    return (int)ADGS_API_MAIN_DEVICES;
}

// ---------------------------------------------------------------------------
// MUX_SET_SWITCH  payload: u8 device, u8 sw, bool closed  → resp: same 3 bytes
// Wire format matches legacy handleMuxSetSwitch (bbp.cpp:2065-2088).
// ---------------------------------------------------------------------------
static int handler_mux_set_switch(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;
    uint8_t device = payload[0];
    uint8_t sw     = payload[1];
    bool closed    = payload[2] != 0;

    if (device >= ADGS_API_MAIN_DEVICES || sw >= ADGS_NUM_SWITCHES)
        return -CMD_ERR_OUT_OF_RANGE;

    if (!adgs_set_api_switch_safe(device, sw, closed))
        return -CMD_ERR_OUT_OF_RANGE;

    // Update cached state (legacy bbp.cpp:2081-2084)
    ScopedStateLock lock;
    if (lock.held()) {
        adgs_get_all_states(g_deviceState.muxState);
    }

    resp[0] = device;
    resp[1] = sw;
    resp[2] = closed ? 1 : 0;
    *resp_len = 3;
    return 3;
}

// ---------------------------------------------------------------------------
// SET_LSHIFT_OE  payload: u8 on(0=off, 1=on)  → resp: u8 on
// Wire format matches legacy handleSetLshiftOe (bbp.cpp:2091-2099).
// ---------------------------------------------------------------------------
static int handler_set_lshift_oe(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    bool on = payload[0] != 0;
    pin_write(PIN_LSHIFT_OE, on ? 1 : 0);
    ESP_LOGI(TAG, "Level shifter OE = %s", on ? "ON" : "OFF");
    resp[0] = on ? 1 : 0;
    *resp_len = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// GET_ADMIN_TOKEN  payload: (none)
// resp: u8 token_len, then token bytes
// Wire format matches legacy handleGetAdminToken (bbp.cpp:2379-2392).
// ---------------------------------------------------------------------------
static int handler_get_admin_token(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;
    const char *token = auth_get_admin_token();
    size_t tlen = strlen(token);
    size_t pos = 0;
    bbp_put_u8(resp, &pos, (uint8_t)tlen);
    memcpy(resp + pos, token, tlen);
    pos += tlen;
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// PING  payload: optional u32 token  → resp: u32 token, u32 timestamp_ms
// Wire format matches legacy handlePing (bbp.cpp:2365-2376).
// ---------------------------------------------------------------------------
static int handler_ping(const uint8_t *payload, size_t len,
                        uint8_t *resp, size_t *resp_len)
{
    uint32_t token = 0;
    if (len >= 4) {
        size_t rpos = 0;
        token = bbp_get_u32(payload, &rpos);
    }
    size_t pos = 0;
    bbp_put_u32(resp, &pos, token);
    bbp_put_u32(resp, &pos, status_millis_now());
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// Commands with variable/complex responses use rsp=NULL (CLI prints raw hex len).
// get_status, get_gpio_status, get_uart_config, get_uart_pins,
// mux_set_all, mux_get_all, get_admin_token have variable/large responses.

static const ArgSpec s_get_device_info_rsp[] = {
    { "spiOk",      ARG_BOOL, true, 0, 0 },
    { "siliconRev", ARG_U8,   true, 0, 0 },
    { "id0",        ARG_U16,  true, 0, 0 },
    { "id1",        ARG_U16,  true, 0, 0 },
};

static const ArgSpec s_set_gpio_config_args[] = {
    { "gpio",     ARG_U8,   true, 0, 11 },
    { "mode",     ARG_U8,   true, 0, 255 },
    { "pulldown", ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_set_gpio_config_rsp[] = {
    { "gpio",     ARG_U8,   true, 0, 0 },
    { "mode",     ARG_U8,   true, 0, 0 },
    { "pulldown", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_set_gpio_value_args[] = {
    { "gpio",  ARG_U8,   true, 0, 11 },
    { "value", ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_set_gpio_value_rsp[] = {
    { "gpio",  ARG_U8,   true, 0, 0 },
    { "value", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_adc_leds_set_mode_args[] = {
    { "mode", ARG_U8, true, 0, 1 },
};
static const ArgSpec s_adc_leds_set_mode_rsp[] = {
    { "mode", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_set_uart_config_args[] = {
    { "bridgeId",  ARG_U8,  true, 0, 255 },
    { "uart_num",  ARG_U8,  true, 0, 255 },
    { "tx_pin",    ARG_U8,  true, 0, 255 },
    { "rx_pin",    ARG_U8,  true, 0, 255 },
    { "baudrate",  ARG_U32, true, 0, 0 },
    { "data_bits", ARG_U8,  true, 0, 255 },
    { "parity",    ARG_U8,  true, 0, 255 },
    { "stop_bits", ARG_U8,  true, 0, 255 },
    { "enabled",   ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_clear_ch_alert_args[] = {
    { "channel", ARG_U8, true, 0, 3 },
};
static const ArgSpec s_clear_ch_alert_rsp[] = {
    { "channel", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_set_alert_mask_args[] = {
    { "alertMask",  ARG_U16, true, 0, 0 },
    { "supplyMask", ARG_U16, true, 0, 0 },
};
static const ArgSpec s_set_alert_mask_rsp[] = {
    { "alertMask",  ARG_U16, true, 0, 0 },
    { "supplyMask", ARG_U16, true, 0, 0 },
};

static const ArgSpec s_set_ch_alert_mask_args[] = {
    { "channel", ARG_U8,  true, 0, 3 },
    { "mask",    ARG_U16, true, 0, 0 },
};
static const ArgSpec s_set_ch_alert_mask_rsp[] = {
    { "channel", ARG_U8,  true, 0, 0 },
    { "mask",    ARG_U16, true, 0, 0 },
};

static const ArgSpec s_set_diag_config_args[] = {
    { "slot",   ARG_U8, true, 0, 3 },
    { "source", ARG_U8, true, 0, 13 },
};
static const ArgSpec s_set_diag_config_rsp[] = {
    { "slot",   ARG_U8, true, 0, 0 },
    { "source", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_set_do_config_args[] = {
    { "channel",    ARG_U8,   true, 0, 3 },
    { "mode",       ARG_U8,   true, 0, 255 },
    { "srcSelGpio", ARG_BOOL, true, 0, 0 },
    { "t1",         ARG_U8,   true, 0, 255 },
    { "t2",         ARG_U8,   true, 0, 255 },
};
static const ArgSpec s_set_do_config_rsp[] = {
    { "channel",    ARG_U8,   true, 0, 0 },
    { "mode",       ARG_U8,   true, 0, 0 },
    { "srcSelGpio", ARG_BOOL, true, 0, 0 },
    { "t1",         ARG_U8,   true, 0, 0 },
    { "t2",         ARG_U8,   true, 0, 0 },
};

static const ArgSpec s_set_do_state_args[] = {
    { "channel", ARG_U8,   true, 0, 3 },
    { "on",      ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_set_do_state_rsp[] = {
    { "channel", ARG_U8,   true, 0, 0 },
    { "on",      ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_set_ilimit_args[] = {
    { "channel", ARG_U8,   true, 0, 3 },
    { "limit",   ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_set_ilimit_rsp[] = {
    { "channel", ARG_U8,   true, 0, 0 },
    { "limit",   ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_set_avdd_sel_args[] = {
    { "channel", ARG_U8, true, 0, 3 },
    { "sel",     ARG_U8, true, 0, 3 },
};
static const ArgSpec s_set_avdd_sel_rsp[] = {
    { "channel", ARG_U8, true, 0, 0 },
    { "sel",     ARG_U8, true, 0, 0 },
};

static const ArgSpec s_mux_set_switch_args[] = {
    { "device", ARG_U8,   true, 0, 255 },
    { "sw",     ARG_U8,   true, 0, 255 },
    { "closed", ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_mux_set_switch_rsp[] = {
    { "device", ARG_U8,   true, 0, 0 },
    { "sw",     ARG_U8,   true, 0, 0 },
    { "closed", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_set_lshift_oe_args[] = {
    { "on", ARG_U8, true, 0, 1 },
};
static const ArgSpec s_set_lshift_oe_rsp[] = {
    { "on", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_ping_args[] = {
    { "token", ARG_U32, false, 0, 0 },
};
static const ArgSpec s_ping_rsp[] = {
    { "token",        ARG_U32, true, 0, 0 },
    { "timestamp_ms", ARG_U32, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_status_cmds[] = {
    { BBP_CMD_GET_STATUS,        "get_status",
      NULL,                    0, NULL,                    0, handler_get_status,        CMD_FLAG_READS_STATE },
    { BBP_CMD_GET_DEVICE_INFO,   "get_device_info",
      NULL,                    0, s_get_device_info_rsp,   4, handler_get_device_info,   CMD_FLAG_READS_STATE },
    { BBP_CMD_GET_FAULTS,        "get_faults",
      NULL,                    0, NULL,                    0, handler_get_faults,        CMD_FLAG_READS_STATE },
    { BBP_CMD_GET_DIAGNOSTICS,   "get_diagnostics",
      NULL,                    0, NULL,                    0, handler_get_diagnostics,   CMD_FLAG_READS_STATE },
    { BBP_CMD_GET_GPIO_STATUS,   "get_gpio_status",
      NULL,                    0, NULL,                    0, handler_get_gpio_status,   CMD_FLAG_READS_STATE },
    { BBP_CMD_SET_GPIO_CONFIG,   "set_gpio_config",
      s_set_gpio_config_args,  3, s_set_gpio_config_rsp,  3, handler_set_gpio_config,   0                   },
    { BBP_CMD_SET_GPIO_VALUE,    "set_gpio_value",
      s_set_gpio_value_args,   2, s_set_gpio_value_rsp,   2, handler_set_gpio_value,    0                   },
    { BBP_CMD_ADC_LEDS_SET_MODE, "adc_leds_set_mode",
      s_adc_leds_set_mode_args, 1, s_adc_leds_set_mode_rsp, 1, handler_adc_leds_set_mode, 0                },
    { BBP_CMD_GET_UART_CONFIG,   "get_uart_config",
      NULL,                    0, NULL,                    0, handler_get_uart_config,   CMD_FLAG_READS_STATE },
    { BBP_CMD_SET_UART_CONFIG,   "set_uart_config",
      s_set_uart_config_args,  9, NULL,                    0, handler_set_uart_config,   0                   },
    { BBP_CMD_GET_UART_PINS,     "get_uart_pins",
      NULL,                    0, NULL,                    0, handler_get_uart_pins,     CMD_FLAG_READS_STATE },
    { BBP_CMD_CLEAR_ALL_ALERTS,  "clear_all_alerts",
      NULL,                    0, NULL,                    0, handler_clear_all_alerts,  0                   },
    { BBP_CMD_CLEAR_CH_ALERT,    "clear_ch_alert",
      s_clear_ch_alert_args,   1, s_clear_ch_alert_rsp,   1, handler_clear_ch_alert,    0                   },
    { BBP_CMD_SET_ALERT_MASK,    "set_alert_mask",
      s_set_alert_mask_args,   2, s_set_alert_mask_rsp,   2, handler_set_alert_mask,    0                   },
    { BBP_CMD_SET_CH_ALERT_MASK, "set_ch_alert_mask",
      s_set_ch_alert_mask_args, 2, s_set_ch_alert_mask_rsp, 2, handler_set_ch_alert_mask, 0                },
    { BBP_CMD_SET_DIAG_CONFIG,   "set_diag_config",
      s_set_diag_config_args,  2, s_set_diag_config_rsp,  2, handler_set_diag_config,   0                   },
    { BBP_CMD_SET_DO_CONFIG,     "set_do_config",
      s_set_do_config_args,    5, s_set_do_config_rsp,    5, handler_set_do_config,     0                   },
    { BBP_CMD_SET_DO_STATE,      "set_do_state",
      s_set_do_state_args,     2, s_set_do_state_rsp,     2, handler_set_do_state,      0                   },
    { BBP_CMD_SET_ILIMIT,        "set_ilimit",
      s_set_ilimit_args,       2, s_set_ilimit_rsp,       2, handler_set_ilimit,        0                   },
    { BBP_CMD_SET_AVDD_SEL,      "set_avdd_sel",
      s_set_avdd_sel_args,     2, s_set_avdd_sel_rsp,     2, handler_set_avdd_sel,      0                   },
    { BBP_CMD_MUX_SET_ALL,       "mux_set_all",
      NULL,                    0, NULL,                    0, handler_mux_set_all,       0                   },
    { BBP_CMD_MUX_GET_ALL,       "mux_get_all",
      NULL,                    0, NULL,                    0, handler_mux_get_all,       CMD_FLAG_READS_STATE },
    { BBP_CMD_MUX_SET_SWITCH,    "mux_set_switch",
      s_mux_set_switch_args,   3, s_mux_set_switch_rsp,   3, handler_mux_set_switch,    0                   },
    { BBP_CMD_SET_LSHIFT_OE,     "set_lshift_oe",
      s_set_lshift_oe_args,    1, s_set_lshift_oe_rsp,    1, handler_set_lshift_oe,     0                   },
    { BBP_CMD_GET_ADMIN_TOKEN,   "get_admin_token",
      NULL,                    0, NULL,                    0, handler_get_admin_token,   CMD_FLAG_READS_STATE },
    { BBP_CMD_PING,              "ping",
      s_ping_args,             1, s_ping_rsp,             2, handler_ping,              CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_status(void)
{
    cmd_registry_register_block(s_status_cmds,
        sizeof(s_status_cmds) / sizeof(s_status_cmds[0]));
}
