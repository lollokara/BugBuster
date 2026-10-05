// =============================================================================
// standby_hw.cpp - binds the standby coordinator (standby_system.c) to the S3
// drivers. Every hardware step runs on the main-loop task from
// standby_runtime_tick(); other tasks only see the barrier and the gates.
//
// Never used here, by design: PCA_CTRL_MUX_EN (that bit is LOGIC_EN), raw PCA
// port masks, the USB hub / LOGIC_EN / core rails, MCU sleep or reboot.
// =============================================================================

#include "standby_hw.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "adc_leds.h"
#include "adgs2414d.h"
#include "ble_service.h"
#include "bbp.h"
#include "clkgen.h"
#include "config.h"
#include "daq_trigger.h"
#include "dio.h"
#include "efuse_imon.h"
#include "ext_bus.h"
#include "hat.h"
#include "pca9535.h"
#include "selftest.h"
#include "status_led.h"
#include "tasks.h"
#include "uart_bridge.h"
#include "update_manager.h"
#include "ws_stream.h"
#include "standby_api.h"
#include "cmd_errors.h"

static const char *TAG = "standby";

// Cross-module owner status that has no header of its own.
extern "C" bool cmd_ota_session_active(void);   // bbp/cmds/cmd_ota.cpp
// Provided by the scripting owner: true while a script is running or queued
// (an idle persistent VM is NOT work). Until it is linked in, scripts are
// assumed to be running so standby stays off rather than risk a live script.
extern "C" bool scripting_has_work(void) __attribute__((weak));

#define STANDBY_UART_WINDOW_MS 30000u
#define STANDBY_NVS_NS         "standby"
#define STANDBY_NVS_KEY        "timeout_s"
#define STANDBY_DEFAULT_S      300u

static StandbySystem     s_sys;
static SemaphoreHandle_t s_lock = nullptr;
static SemaphoreHandle_t s_write_lock = nullptr;   // one policy write at a time (see hw_write_lock)
static TaskHandle_t      s_coord = nullptr;       // the main-loop task: runs every step
static volatile bool     s_ready = false;
static volatile bool     s_power_transition = false;
static volatile bool     s_led_standby = false;
static bool              s_bus_held = false;       // coordinator owns g_spi_bus_mutex
static uint32_t          s_led_step_ms = 0;

extern SemaphoreHandle_t g_spi_bus_mutex;

// ---------------------------------------------------------------------------
// ops: lock / clock
// ---------------------------------------------------------------------------
static void hw_lock(void *) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void hw_unlock(void *) { xSemaphoreGive(s_lock); }
static uint32_t hw_now(void *) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// ops: inhibitors. Called both outside the lock (sampling) and inside it (the
// atomic re-check), so everything here is lock-free or a short try-lock.
// ---------------------------------------------------------------------------
static bool update_in_progress(void)
{
    update_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    update_manager_fill_installed(&snap);
    return snap.state != UPDATE_STATE_IDLE && snap.state != UPDATE_STATE_CHECKING &&
           snap.state != UPDATE_STATE_FAILED;
}

static uint32_t hw_inhibitors(void *)
{
    uint32_t m = 0;

    if (scripting_has_work) {
        if (scripting_has_work()) m |= BB_ST_INH_SCRIPT;
    } else {
        m |= BB_ST_INH_SCRIPT;   // owner not linked yet: fail closed
    }

    if (bbpAdcStreamMask() != 0 || bbpScopeStreamActive() || bbpAdcDspActive() ||
        tasks_scope_mode_active() || ws_stream_has_subscriber(WS_STREAM_SCOPE) ||
        ws_stream_has_subscriber(WS_STREAM_ADC) || ws_stream_has_subscriber(WS_STREAM_ADC_DSP) ||
        ws_stream_has_subscriber(WS_STREAM_LA_META))
        m |= BB_ST_INH_STREAM;

    if (daq_trigger_is_armed()) m |= BB_ST_INH_TRIGGER;
    if (bbpWavegenActive()) m |= BB_ST_INH_WAVEFORM;
    if (uart_bridge_recent_activity(STANDBY_UART_WINDOW_MS)) m |= BB_ST_INH_UART;
    if (ext_bus_jobs_pending()) m |= BB_ST_INH_BUS;

    if (cmd_ota_session_active() || update_in_progress() || update_manager_reboot_pending())
        m |= BB_ST_INH_OTA;

    const SelftestCalResult *cal = selftest_get_cal_result();
    if (cal && cal->status == CAL_STATUS_RUNNING) m |= BB_ST_INH_CALIBRATION;
    // U23 closed = a measurement or the e-fuse IMON monitor owns the route.
    if (selftest_is_busy() || efuse_imon_active()) m |= BB_ST_INH_SELFTEST;

    bool clk = false; uint8_t clk_io = 0; int clk_gpio = 0; ClkSrc clk_src = CLKSRC_LEDC;
    uint32_t clk_req = 0, clk_act = 0;
    if (clkgen_status(&clk, &clk_io, &clk_gpio, &clk_src, &clk_req, &clk_act) && clk)
        m |= BB_ST_INH_CLOCK;

    if (tasks_cmd_busy()) m |= BB_ST_INH_WORK;

    // A logical session that never used the presence API - a handshaked USB host, see
    // standby_system_usb_session() - or an authenticated BLE central. A cable or an enumerated
    // CDC port alone is neither. A USB host that registered presence is represented by its
    // bounded client lease instead (counted by the policy), so releasing it allows sleep.
    if (standby_system_usb_legacy_host(&s_sys) ||
        (ble_service_is_connected() && ble_service_is_authenticated()))
        m |= BB_ST_INH_HOST;

    return m;
}

// ---------------------------------------------------------------------------
// ops: HAT
// ---------------------------------------------------------------------------
static StandbyHatLink hw_hat_link(void *)
{
    if (!hat_detected()) return STANDBY_HAT_NONE;
    const HatState *hs = hat_get_state();
    return (hs && hs->connected) ? STANDBY_HAT_LINKED : STANDBY_HAT_DETECTED;
}

static StandbyXchg hw_hat_exchange(void *, const bb_standby_request_t *rq, bb_standby_reply_t *rp)
{
    uint8_t rsp[32] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_request(BB_HAT_CMD_STANDBY, (const uint8_t *)rq, sizeof(*rq), rsp, &rsp_len,
                              250, sizeof(rsp));
    if (cmd == BB_HAT_RSP_STANDBY && rsp_len == sizeof(*rp)) {
        memcpy(rp, rsp, sizeof(*rp));
        return STANDBY_XCHG_OK;
    }
    // Any other answer is a HAT that does not speak standby (HAT_RSP_ERROR from
    // firmware that predates it). Silence returns 0.
    return cmd == 0 ? STANDBY_XCHG_TIMEOUT : STANDBY_XCHG_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// ops: persistence
// ---------------------------------------------------------------------------
// Serialises policy writes: validate -> persist -> apply happen in one order for every task,
// so what flash holds and what runs cannot diverge. Bounded wait: a stuck writer is BUSY.
static bool hw_write_lock(void *)
{
    return s_write_lock && xSemaphoreTake(s_write_lock, pdMS_TO_TICKS(3000)) == pdTRUE;
}

static void hw_write_unlock(void *)
{
    if (s_write_lock) xSemaphoreGive(s_write_lock);
}

// true only when the value is committed. After a failed set/commit the previous value is
// put back, so what flash holds is still what is running.
static bool hw_persist(void *, uint32_t seconds)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(STANDBY_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timeout not persisted: nvs_open %s", esp_err_to_name(err));
        return false;
    }
    uint16_t prev = 0;
    const bool had_prev = nvs_get_u16(h, STANDBY_NVS_KEY, &prev) == ESP_OK;
    err = nvs_set_u16(h, STANDBY_NVS_KEY, (uint16_t)seconds);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timeout not persisted: %s", esp_err_to_name(err));
        if (had_prev) nvs_set_u16(h, STANDBY_NVS_KEY, prev);
        else nvs_erase_key(h, STANDBY_NVS_KEY);
        nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static uint32_t load_timeout(void)
{
    uint16_t v = STANDBY_DEFAULT_S;
    nvs_handle_t h;
    if (nvs_open(STANDBY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, STANDBY_NVS_KEY, &v) != ESP_OK) v = STANDBY_DEFAULT_S;
        nvs_close(h);
    }
    return v;   // standby_init() maps anything invalid to the default
}

// ---------------------------------------------------------------------------
// ops: local hardware steps
// ---------------------------------------------------------------------------
static struct {
    bool        begun;
    StandbyStep step;
    uint32_t    generation;
    uint32_t    t0;
} s_ls;

static bool step_first(StandbyStep step, uint32_t generation, uint32_t now)
{
    if (s_ls.begun && s_ls.step == step && s_ls.generation == generation) return false;
    s_ls.begun = true;
    s_ls.step = step;
    s_ls.generation = generation;
    s_ls.t0 = now;
    return true;
}

static void shadow_muxes_zero(void)
{
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t i = 0; i < ADGS_NUM_DEVICES; i++) g_deviceState.muxState[i] = 0;
        xSemaphoreGive(g_stateMutex);
    }
}

static bool all_zero(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (b[i] != 0) return false;
    return true;
}

static bool route_open_verified(void)
{
    uint8_t rb[ADGS_NUM_DEVICES] = {};
    if (!adgs_readback_verify(rb) || !all_zero(rb, ADGS_NUM_DEVICES)) return false;
    uint8_t api[ADGS_API_MAIN_DEVICES] = {};
    adgs_get_api_states(api);
    if (!all_zero(api, ADGS_API_MAIN_DEVICES)) return false;
#if ADGS_HAS_SELFTEST
    if (adgs_get_selftest() != 0) return false;
#endif
    return true;
}

static bool supplies_and_efuses_off(void)
{
    pca9535_update();
    const PCA9535State *st = pca9535_get_state();
    if (!st || !st->present) return false;
    return !st->vadj1_en && !st->vadj2_en && !st->efuse_en[0] && !st->efuse_en[1] &&
           !st->efuse_en[2] && !st->efuse_en[3];
}

static StandbyStepResult step_quiesce(uint32_t now, bool first)
{
    if (first) {
        s_power_transition = true;
        ESP_LOGI(TAG, "quiesce: pausing analog workers");
    }
    if (standby_system_analog_depth(&s_sys) != 0 || tasks_cmd_busy()) {
        if (tasks_cmd_busy() && !tasks_drain_command_queue(500)) {
            ESP_LOGE(TAG, "quiesce: command queue did not drain");
            return STANDBY_STEP_FAILED;
        }
        if (standby_system_analog_depth(&s_sys) != 0 || tasks_cmd_busy())
            return (now - s_ls.t0) < 1500u ? STANDBY_STEP_PENDING : STANDBY_STEP_FAILED;
    }
    adc_leds_standby_off();
    g_deviceState.analogUnavailable = true;
    return STANDBY_STEP_DONE;
}

static StandbyStepResult step_mux_off(void)
{
    // An attached e-fuse monitor or a running self-test is an inhibitor; refuse
    // rather than pull a route out from under it.
    if (efuse_imon_active()) return STANDBY_STEP_FAILED;

    uint8_t zero[ADGS_API_MAIN_DEVICES] = {};
    if (!adgs_set_api_all_safe(zero)) return STANDBY_STEP_FAILED;
#if ADGS_HAS_SELFTEST
    if (!adgs_set_selftest(0x00)) return STANDBY_STEP_FAILED;
#endif
    shadow_muxes_zero();
    if (!route_open_verified()) {
        ESP_LOGE(TAG, "mux-off: read-back is not all open");
        return STANDBY_STEP_FAILED;
    }
    return STANDBY_STEP_DONE;
}

// A channel function back to high impedance, confirmed by reading the converter (the apply
// call reports nothing). The DAC setpoint shadows survive for a later explicit enable;
// nothing is re-applied on wake.
static bool channel_is_high_imp(uint8_t ch)
{
    ChannelFunction f = CH_FUNC_HIGH_IMP;
    return tasks_read_channel_function(ch, &f) && f == CH_FUNC_HIGH_IMP;
}

static bool force_channel_high_imp(uint8_t ch)
{
    uint16_t code = 0; float val = 0.0f; bool bip = false;
    bool have_shadow = false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        code = g_deviceState.channels[ch].dacCode;
        val = g_deviceState.channels[ch].dacValue;
        bip = g_deviceState.channels[ch].dacBipolar;
        have_shadow = true;
        xSemaphoreGive(g_stateMutex);
    }
    bool ok = channel_is_high_imp(ch);
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        tasks_apply_channel_function(ch, CH_FUNC_HIGH_IMP);
        ok = channel_is_high_imp(ch);
    }
    if (have_shadow && xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[ch].dacCode = code;
        g_deviceState.channels[ch].dacValue = val;
        g_deviceState.channels[ch].dacBipolar = bip;
        xSemaphoreGive(g_stateMutex);
    }
    if (!ok) ESP_LOGE(TAG, "channel %u is not confirmed high-impedance", (unsigned)ch);
    return ok;
}

// E-fuses and VADJ off, each on its own: one refusal never leaves the others on.
static bool efuses_and_vadj_off(void)
{
    pca9535_update();
    const PCA9535State *st = pca9535_get_state();
    if (!st || !st->present) return false;
    bool ok = true;
    for (uint8_t i = 0; i < 4; i++) {
        if (st->efuse_en[i] && !pca9535_user_arm_efuse(i, false)) ok = false;
    }
    if (st->vadj1_en && !pca9535_set_control(PCA_CTRL_VADJ1_EN, false)) ok = false;
    if (st->vadj2_en && !pca9535_set_control(PCA_CTRL_VADJ2_EN, false)) ok = false;
    return ok;
}

// Digital IOs high impedance, confirmed from the driver state.
static bool dios_off(void)
{
    bool ok = true;
    for (uint8_t io = DIO_FIRST_IO; io <= DIO_LAST_IO; io++) {
        DioState d;
        if (!dio_get_state(io, &d)) { ok = false; continue; }
        if (d.mode == DIO_MODE_DISABLED) continue;
        dio_configure(io, DIO_MODE_DISABLED);
        if (!dio_get_state(io, &d) || d.mode != DIO_MODE_DISABLED) ok = false;
    }
    return ok;
}

static StandbyStepResult step_outputs_off(void)
{
    // Every target is attempted: the first failure must not leave later outputs live.
    bool ok = true;
    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) ok = force_channel_high_imp(ch) && ok;
    ok = efuses_and_vadj_off() && ok;
    ok = dios_off() && ok;

    // A UART bridge keeps its pins (and the level shifter) if the host left one enabled.
    bool bridge_enabled = false;
    for (int b = 0; b < 2; b++) {
        UartBridgeConfig cfg;
        if (uart_bridge_get_config(b, &cfg) && cfg.enabled) bridge_enabled = true;
    }
    if (!bridge_enabled) gpio_set_level(PIN_LSHIFT_OE, 0);

    if (!supplies_and_efuses_off()) {
        ESP_LOGE(TAG, "outputs-off: a supply or e-fuse is still enabled");
        ok = false;
    }
    return ok ? STANDBY_STEP_DONE : STANDBY_STEP_FAILED;
}

static StandbyStepResult step_analog_off(void)
{
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            g_deviceState.channels[ch].adcRawCode = 0;
            g_deviceState.channels[ch].adcValue = 0.0f;
        }
        xSemaphoreGive(g_stateMutex);
    }
    g_deviceState.analogUnavailable = true;

    // Own the bus before the rail goes: any straggler is shut out, and an
    // in-flight transfer finishes first.
    if (!s_bus_held) {
        if (g_spi_bus_mutex == nullptr ||
            xSemaphoreTakeRecursive(g_spi_bus_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "analog-off: SPI bus busy");
            return STANDBY_STEP_FAILED;
        }
        s_bus_held = true;
    }
    // The analog supply is only cut under a switch matrix that is proven open.
    if (!route_open_verified()) {
        ESP_LOGE(TAG, "analog-off: switch matrix not verified open, supply left on");
        return STANDBY_STEP_FAILED;
    }
    if (!pca9535_set_control(PCA_CTRL_15V_EN, false)) return STANDBY_STEP_FAILED;
    pca9535_update();
    const PCA9535State *st = pca9535_get_state();
    if (!st || st->en_15v) return STANDBY_STEP_FAILED;
    return STANDBY_STEP_DONE;
}

static StandbyStepResult step_indicators_off(void)
{
    s_led_standby = true;
    status_led_standby(true);
    return STANDBY_STEP_DONE;
}

static StandbyStepResult step_wake_safe(void)
{
    // Nothing may be live while the system is not ACTIVE; force it if it is.
    pca9535_update();
    const PCA9535State *st = pca9535_get_state();
    if (st && st->present) {
        for (uint8_t i = 0; i < 4; i++) if (st->efuse_en[i]) pca9535_user_arm_efuse(i, false);
        if (st->vadj1_en) pca9535_set_control(PCA_CTRL_VADJ1_EN, false);
        if (st->vadj2_en) pca9535_set_control(PCA_CTRL_VADJ2_EN, false);
    }
    s_power_transition = true;
    return supplies_and_efuses_off() ? STANDBY_STEP_DONE : STANDBY_STEP_FAILED;
}

static StandbyStepResult step_analog_on(uint32_t now, bool first)
{
    if (first && !pca9535_set_control(PCA_CTRL_15V_EN, true)) return STANDBY_STEP_FAILED;
    return (now - s_ls.t0) < 500u ? STANDBY_STEP_PENDING : STANDBY_STEP_DONE;   // boot settles 500 ms
}

static StandbyStepResult step_reinitialize(void)
{
    AD74416H *dev = tasks_get_device();
    if (!dev) return STANDBY_STEP_FAILED;

    // The coordinator passes the bus gate; no other task can run SPI yet. Nothing below is
    // assumed to have worked: each configuration step reports back, and the converter's ADC
    // and the switch matrix are read back before the system may call itself ready.
    bool ok = dev->reinitialize();
    if (!ok) ESP_LOGE(TAG, "reinitialize: AD74416H SCRATCH verify failed");
    if (ok && !dev->setupDiagnostics()) {
        ESP_LOGE(TAG, "reinitialize: diagnostic slots did not read back");
        ok = false;
    }
    if (ok && !dev->startAdcConversion(true, 0x00, 0x0F)) {
        ESP_LOGE(TAG, "reinitialize: ADC conversion start not written");
        ok = false;
    }
    if (ok && !dev->verifyAdcConversion(0x00, 0x0F)) {
        ESP_LOGE(TAG, "reinitialize: ADC conversion enables did not read back");
        ok = false;
    }
    if (!ok) {
        g_deviceState.spiOk = false;
        return STANDBY_STEP_FAILED;
    }

    // Measurement shadows restart from zero (they are flagged unavailable until READY). Fault
    // latches, alert masks and the fault history are NOT cleared here: the fault monitor
    // refreshes them from the converter, and an earlier genuine fault stays visible until then.
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            ChannelState &c = g_deviceState.channels[ch];
            c.function = CH_FUNC_HIGH_IMP;
            c.adcRange = ADC_RNG_0_12V;
            c.adcRate = ADC_RATE_20SPS;
            c.adcMux = ADC_MUX_LF_TO_AGND;
            c.adcRawCode = 0;
            c.adcValue = 0.0f;
            c.rtdExcitationUa = 0;
        }
        const uint8_t diag_src[4] = {1, 5, 2, 3};
        for (uint8_t d = 0; d < 4; d++) {
            g_deviceState.diag[d].source = diag_src[d];
            g_deviceState.diag[d].rawCode = 0;
            g_deviceState.diag[d].value = 0.0f;
            g_deviceState.diag[d].skipReads = 2;   // first reads follow the reset
        }
        xSemaphoreGive(g_stateMutex);
    }

    if (!adgs_reinit_after_power() || !route_open_verified()) {
        ESP_LOGE(TAG, "reinitialize: switch matrix is not verified open");
        g_deviceState.spiOk = false;
        return STANDBY_STEP_FAILED;
    }
    shadow_muxes_zero();
    gpio_set_level(PIN_LSHIFT_OE, 0);   // level shifters stay off until a host enables them

    g_deviceState.spiOk = true;
    if (s_bus_held) {
        xSemaphoreGiveRecursive(g_spi_bus_mutex);
        s_bus_held = false;
    }
    return STANDBY_STEP_DONE;
}

static StandbyStepResult step_indicators_on(void)
{
    adc_leds_standby_restore();
    s_led_standby = false;
    status_led_standby(false);
    g_deviceState.analogUnavailable = false;
    s_power_transition = false;
    return STANDBY_STEP_DONE;
}

static StandbyStepResult hw_local_step(void *, StandbyStep step, uint32_t generation)
{
    uint32_t now = hw_now(nullptr);
    bool first = step_first(step, generation, now);
    switch (step) {
    case STANDBY_QUIESCE:         return step_quiesce(now, first);
    case STANDBY_HAT_SLEEP:       return STANDBY_STEP_DONE;   // HAT-only step
    case STANDBY_MUX_OFF:         return step_mux_off();
    case STANDBY_OUTPUTS_OFF:     return step_outputs_off();
    case STANDBY_ANALOG_OFF:      return step_analog_off();
    case STANDBY_INDICATORS_OFF:  return step_indicators_off();
    case STANDBY_WAKE_SAFE:       return step_wake_safe();
    case STANDBY_ANALOG_ON:       return step_analog_on(now, first);
    case STANDBY_REINITIALIZE:    return step_reinitialize();
    case STANDBY_HAT_WAKE:        return STANDBY_STEP_DONE;   // HAT-only step
    case STANDBY_INDICATORS_ON:   return step_indicators_on();
    default:                      return STANDBY_STEP_FAILED;
    }
}

// ---------------------------------------------------------------------------
// FAULT_SAFE: whatever a failed sequence left powered is forced off, each target on its own
// so one failure never skips the rest. true only when all of it was verified. The analog
// supply is never cut here: that needs a verified-open switch matrix, which a fault may not
// allow. Digital outputs / the level shifter go off unconditionally in a fault.
// ---------------------------------------------------------------------------
static bool hw_safe_outputs_off(void *)
{
    bool ok = true;
    pca9535_update();
    const PCA9535State *st = pca9535_get_state();
    const bool analog_powered = st && st->present && st->en_15v;

    if (analog_powered) {
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) ok = force_channel_high_imp(ch) && ok;
        uint8_t zero[ADGS_API_MAIN_DEVICES] = {};
        bool mux_open = adgs_set_api_all_safe(zero);
#if ADGS_HAS_SELFTEST
        mux_open = adgs_set_selftest(0x00) && mux_open;
#endif
        if (mux_open) shadow_muxes_zero();
        mux_open = mux_open && route_open_verified();
        if (!mux_open) ESP_LOGE(TAG, "safe-state: switch matrix not verified open");
        ok = mux_open && ok;
    }
    ok = efuses_and_vadj_off() && ok;
    ok = dios_off() && ok;
    gpio_set_level(PIN_LSHIFT_OE, 0);
    if (!supplies_and_efuses_off()) ok = false;
    if (!ok) ESP_LOGE(TAG, "safe-state: outputs not verified off, retrying");
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void standby_runtime_init(void)
{
    if (s_ready) return;
    s_lock = xSemaphoreCreateMutex();
    s_write_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_write_lock) {
        ESP_LOGE(TAG, "lock creation failed: standby disabled");
        return;
    }
    StandbyOps ops = {};
    ops.ctx = nullptr;
    ops.lock = hw_lock;
    ops.unlock = hw_unlock;
    ops.now_ms = hw_now;
    ops.inhibitors = hw_inhibitors;
    ops.hat_link = hw_hat_link;
    ops.hat_exchange = hw_hat_exchange;
    ops.local_step = hw_local_step;
    ops.persist_timeout = hw_persist;
    ops.write_lock = hw_write_lock;
    ops.write_unlock = hw_write_unlock;
    ops.safe_outputs_off = hw_safe_outputs_off;
    standby_system_init(&s_sys, &ops, load_timeout());
    // A host that handshook before the coordinator existed is already a legacy session.
    if (bbpIsActive()) standby_system_usb_session(&s_sys, true);
    s_ready = true;
    ESP_LOGI(TAG, "coordinator ready, timeout %us", (unsigned)(s_sys.policy.timeout_ms / 1000u));
}

StandbySystem *standby_runtime(void) { return s_ready ? &s_sys : nullptr; }

void standby_runtime_tick(void)
{
    if (!s_ready) return;
    if (!s_coord) s_coord = xTaskGetCurrentTaskHandle();
    standby_system_tick(&s_sys);

    if (s_led_standby) {
        uint32_t now = hw_now(nullptr);
        if ((uint32_t)(now - s_led_step_ms) >= 20u) {
            s_led_step_ms = now;
            status_led_standby_step();
        }
    }
}

StandbyAdmit standby_hw_admit(void)
{
    return s_ready ? standby_system_work_begin(&s_sys) : STANDBY_ADMIT_OK;
}

void standby_hw_leave(void)
{
    if (s_ready) standby_system_work_end(&s_sys);
}

bool standby_hw_analog_enter(void)
{
    return s_ready ? standby_system_analog_enter(&s_sys) : true;
}

void standby_hw_analog_leave(void)
{
    if (s_ready) standby_system_analog_leave(&s_sys);
}

bool standby_hw_bus_gate_open(void)
{
    if (!s_ready || standby_system_analog_allowed(&s_sys)) return true;
    return s_coord != nullptr && xTaskGetCurrentTaskHandle() == s_coord;
}

bool standby_hw_power_transition(void) { return s_power_transition; }

void standby_hw_activity(void)
{
    if (s_ready) standby_system_activity(&s_sys);
}

void standby_hw_boot_milestone(uint16_t completed, uint16_t failed, uint16_t skipped)
{
    if (s_ready) standby_system_boot_progress(&s_sys, completed, failed, skipped);
}

int standby_hw_mailbox_policy(uint32_t requested_seconds, uint8_t *state_out)
{
    StandbyStatus st;
    standby_hw_status(&st);
    if (state_out) *state_out = st.state;
    if (!s_ready || requested_seconds == 0xFFFFu) return st.timeout_seconds;
    if (standby_system_set_timeout(&s_sys, requested_seconds) != STANDBY_RC_OK) return -1;
    standby_hw_status(&st);
    if (state_out) *state_out = st.state;
    return st.timeout_seconds;
}

void standby_hw_status(StandbyStatus *out)
{
    if (s_ready) {
        standby_system_get(&s_sys, out);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = BB_ST_ACTIVE;
    out->ready = 1;
}

void standby_hw_usb_session(bool open)
{
    if (s_ready) standby_system_usb_session(&s_sys, open);
}

// Hardware the coordinator depends on, judged by what actually came up: a failed check is
// reported as not ready with the stage that failed, never as ACTIVE with ready=true. A later
// wake retries every stage; USB, Wi-Fi and the cached status stay usable meanwhile.
void standby_hw_boot_check(void)
{
    if (!s_ready) return;
    uint16_t failed = 0;
    const PCA9535State *st = pca9535_get_state();
    if (!st || !st->present) failed |= STANDBY_STEP_BIT(STANDBY_ANALOG_ON);       // rails cannot be controlled or verified
    if (!g_deviceState.spiOk || !g_deviceState.muxOk) failed |= STANDBY_STEP_BIT(STANDBY_REINITIALIZE);
    if (failed != 0u) {
        ESP_LOGE(TAG, "boot check failed (stage mask 0x%04x): not ready", (unsigned)failed);
        standby_system_boot_fault(&s_sys, failed);
        // A converter whose first verify failed at a cold power-on is repaired by the wake chain
        // (REINITIALIZE). Run it now instead of leaving the board "not ready" until a client asks.
        // Anything else (no PCA9535, rails not controllable) cannot be repaired by a wake.
        if ((failed & ~STANDBY_STEP_BIT(STANDBY_REINITIALIZE)) == 0u) standby_system_wake(&s_sys);
    }
}

int standby_hw_bbp(StandbySource src, const uint8_t *payload, size_t len, uint8_t *resp,
                   size_t *resp_len)
{
    StandbySystem *s = standby_runtime();
    if (!s) return -CMD_ERR_INVALID_STATE;
    return standby_api_bbp(s, payload, len, resp, resp_len, src);
}
