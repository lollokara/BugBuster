// =============================================================================
// daq_board.c — BugBuster DAQ HAT (ESP32-P4) board integration
// =============================================================================

#include "daq_board.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb_proto.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "version.h"
#include "ota.h"
#include "usb_backend.h"
#include "buttons_p4.h"
#include "c6_flasher.h"
#include "relay_stage.h"
#include "wifi_ap.h"
#include "tcp_backend.h"
#include "daq_wifi_ident.h"
#include "captive_dns.h"

static const char *TAG = "daq_board";

// Largest single SPI DMA transfer we expect (40-bit sample = 5 bytes); keep a
// comfortable margin for future block transfers.
#define DAQ_SPI_MAX_XFER   64

// -----------------------------------------------------------------------------
// Analog power-rail bring-up. The 3x ADAQ7769-1 are powered from the analog 3V3
// rail (TPS74601, EN=PWR_3V3_EN_PIN, PG=PWR_3V3_PG_PIN); the front-end amps need
// the +/-26V and +/-24V rails. All are OFF at boot (board pull-downs). They MUST
// be enabled here BEFORE the shared ADAQ reset/probe — otherwise every SPI read
// returns 0x00 and all three ADAQs identify as absent ("ADAQ 0/3").
// Sequence per config.h: EN 3V3 -> wait PG -> EN +/-26V -> EN +/-24V -> settle.
static void power_rails_up(void)
{
    gpio_config_t en = {
        .pin_bit_mask = (1ULL << PWR_3V3_EN_PIN) |
                        (1ULL << PWR_26V_EN_PIN) |
                        (1ULL << PWR_24V_EN_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&en);

    gpio_config_t pg = {
        .pin_bit_mask = 1ULL << PWR_3V3_PG_PIN,
        .mode         = GPIO_MODE_INPUT,
    };
    gpio_config(&pg);

    // 1) Analog 3V3 LDO — powers the ADAQs.
    gpio_set_level(PWR_3V3_EN_PIN, 1);

    // 2) Wait for power-good (up to 50 ms). Proceed on timeout so a board whose
    //    PG strap is absent/unrouted still boots (the settle delay covers it).
    bool pg_ok = false;
    for (int i = 0; i < 50; ++i) {
        if (gpio_get_level(PWR_3V3_PG_PIN)) { pg_ok = true; break; }
        esp_rom_delay_us(1000);
    }
    if (!pg_ok) {
        ESP_LOGW(TAG, "3V3 analog rail PG not asserted after 50ms (continuing)");
    }

    // 3) Front-end +/-26V then +/-24V supplies.
    gpio_set_level(PWR_26V_EN_PIN, 1);
    esp_rom_delay_us(2000);
    gpio_set_level(PWR_24V_EN_PIN, 1);

    // 4) Settle before the shared ADAQ reset / first SPI access.
    esp_rom_delay_us(5000);
    ESP_LOGI(TAG, "analog rails up (3V3 EN + PG=%d, +/-26V, +/-24V)", (int)pg_ok);
}

// All three ADAQ *RST are tied to one GPIO. Pulse it once for a clean POR; the
// per-device begin() then uses a SPI soft reset (reset_pin == GPIO_NUM_NC).
static void shared_adaq_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ADAQ_SHARED_RESET_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(ADAQ_SHARED_RESET_PIN, 0);
    esp_rom_delay_us(10);
    gpio_set_level(ADAQ_SHARED_RESET_PIN, 1);
    esp_rom_delay_us(300);   // datasheet: >=200us from RESET to first SPI write
}

static esp_err_t init_spi_buses(void)
{
    esp_err_t err = adaq_ll_bus_init(ADAQ_BUSA_HOST, ADAQ_BUSA_SCLK_PIN,
                                     ADAQ_BUSA_MOSI_PIN, ADAQ_BUSA_MISO_PIN,
                                     DAQ_SPI_MAX_XFER);
    if (err != ESP_OK) return err;

    err = adaq_ll_bus_init(ADAQ_BUSB_HOST, ADAQ_BUSB_SCLK_PIN,
                           ADAQ_BUSB_MOSI_PIN, ADAQ_BUSB_MISO_PIN,
                           DAQ_SPI_MAX_XFER);
    return err;
}

static esp_err_t init_i2c_bus(daq_board_t *b)
{
    i2c_master_bus_config_t buscfg = {
        .i2c_port          = DAQ_I2C_PORT,
        .sda_io_num        = DAQ_I2C_SDA_PIN,
        .scl_io_num        = DAQ_I2C_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags             = { .enable_internal_pullup = true },
    };
    return i2c_new_master_bus(&buscfg, &b->i2c_bus);
}

esp_err_t daq_board_init(daq_board_t *b)
{
    memset(b, 0, sizeof(*b));

    // OTA bookkeeping: detect a pending-verify boot (post-update) early so the
    // caller can run a self-test and confirm (or let the bootloader roll back).
    ota_init();
    relay_stage_init();

    // Bring up the analog power rails BEFORE any ADAQ access — the ADAQs are
    // unpowered (and read 0x00) until the 3V3 analog LDO is enabled.
    power_rails_up();

    esp_err_t err = init_spi_buses();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // --- ADAQ #0 : ADAQ1/U1, dedicated bus A, FINE current, sync master ---
    // Reads the 51/2 ohm CSA output (single-ended) via mux U24. Both CSA paths
    // are gain 1 -> wired to IN1_AAF (+/-4.096 V).
    adaq7769_params_t p0 = {
        .host           = ADAQ_BUSA_HOST,
        .cs_pin         = ADAQ0_CS_PIN,
        .reset_pin      = ADAQ0_RESET_PIN,
        .drdy_pin       = ADAQ0_DRDY_PIN,
        .aaf_input      = ADAQ_AAF_IN1,
        .is_sync_master = (ADAQ_SYNC_MASTER_INDEX == 0),
    };
    // --- ADAQ #1 : ADAQ2/U22, bus B, COARSE current (50 mohm) ---
    // COARSE CSA output is gain 1 -> wired to IN1_AAF (+/-4.096 V).
    adaq7769_params_t p1 = {
        .host           = ADAQ_BUSB_HOST,
        .cs_pin         = ADAQ1_CS_PIN,
        .reset_pin      = ADAQ1_RESET_PIN,
        .drdy_pin       = ADAQ1_DRDY_PIN,
        .aaf_input      = ADAQ_AAF_IN1,
        .is_sync_master = (ADAQ_SYNC_MASTER_INDEX == 1),
    };
    // --- ADAQ #2 : ADAQ3/U23, bus B, VOLTAGE (V_DUT via mux U25) ---
    // V_DUT spans up to ~20 V -> IN3_AAF (only input wide enough). Confirm.
    adaq7769_params_t p2 = {
        .host           = ADAQ_BUSB_HOST,
        .cs_pin         = ADAQ2_CS_PIN,
        .reset_pin      = ADAQ2_RESET_PIN,
        .drdy_pin       = ADAQ2_DRDY_PIN,
        .aaf_input      = ADAQ_AAF_IN3,
        .is_sync_master = (ADAQ_SYNC_MASTER_INDEX == 2),
    };
    const adaq7769_params_t *params[ADAQ_COUNT] = { &p0, &p1, &p2 };

    // One shared hardware reset for all three ADAQ before per-device config.
    shared_adaq_reset();

    for (int i = 0; i < ADAQ_COUNT; ++i) {
        err = adaq7769_attach(&b->adaq[i], params[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADAQ #%d attach failed: %s", i, esp_err_to_name(err));
            continue;
        }
        err = adaq7769_begin(&b->adaq[i]);
        b->adaq_ok[i] = (err == ESP_OK);
        if (!b->adaq_ok[i]) {
            ESP_LOGW(TAG, "ADAQ #%d begin failed: %s", i, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "ADAQ #%d ready (ODR=%.0f SPS)",
                     i, adaq7769_output_data_rate(&b->adaq[i]));
        }
    }

    // Slow the VOLTAGE channel relative to the current channels. U22/U23 share
    // bus B and one SYNC line (cannot be phase-staggered); a lower VOLTAGE ODR
    // de-collides its DRDY from COARSE so the shared bus stays COARSE-dominated.
    if (b->adaq_ok[ADAQ_ROLE_VOLTAGE]) {
        float achieved = 0.0f;
        if (adaq7769_set_output_data_rate(&b->adaq[ADAQ_ROLE_VOLTAGE],
                                          VOLTAGE_ODR_TARGET_SPS,
                                          &achieved) == ESP_OK) {
            ESP_LOGI(TAG, "VOLTAGE ODR set to %.0f SPS (target %.0f)",
                     (double)achieved, (double)VOLTAGE_ODR_TARGET_SPS);
        }
    }

    // U25 voltage mux address pins (GPIO47/48) must be driven as outputs and
    // pre-set BEFORE range_manager_init() asserts MUX_EN (GPIO49 HIGH), which
    // is shared between U24 and U25. Without this, U25 is live with floating
    // A0/A1 lines from the moment MUX_EN goes high.
    // Pre-load the output latch first so the pin never glitches LOW when the
    // output driver is enabled.
    gpio_set_level(VOLT_MUX_A0_PIN, VOLT_MUX_ADDR_VDUT & 1);
    gpio_set_level(VOLT_MUX_A1_PIN, (VOLT_MUX_ADDR_VDUT >> 1) & 1);
    {
        gpio_config_t volt_mux_cfg = {
            .pin_bit_mask = (1ULL << VOLT_MUX_A0_PIN) | (1ULL << VOLT_MUX_A1_PIN),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&volt_mux_cfg);
    }

    // --- Current-range manager (observes the analog autorange loop) ---
    range_manager_init(&b->range);

    // --- I2C devices ---
    err = init_i2c_bus(b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return ESP_OK;   // SPI front-end is still usable
    }

    b->temp_ok[0] = (ad741x_attach(&b->temp[0], b->i2c_bus, AD741X_0_ADDR,
                                   DAQ_I2C_FREQ_HZ, /*has_alert=*/false) == ESP_OK);
    b->temp_ok[1] = (ad741x_attach(&b->temp[1], b->i2c_bus, AD741X_1_ADDR,
                                   DAQ_I2C_FREQ_HZ, /*has_alert=*/false) == ESP_OK);
    b->idac_ok    = (ds4424_attach(&b->idac, b->i2c_bus, DS4424_I2C_ADDR,
                                   DAQ_I2C_FREQ_HZ) == ESP_OK);

    // --- Source/SMU (LTM8056 programmed via DS4424) ---
    smu_init(&b->smu, &b->idac);

    // SMU factory calibration: bring up NVS, load any stored cal tables, install
    // them into the SMU, and spawn the calibration worker task.
    smu_cal_init(&b->cal, b);

    // --- Fusion + power DSP (driven by the FINE current ODR) ---
    float current_odr = b->adaq_ok[ADAQ_ROLE_FINE]
                            ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE])
                            : 256000.0f;
    // Lean on COARSE for ~1 filter settle after a range switch, then short blend.
    // 64-sample blackout / 16-sample cross-fade are conservative defaults; tune
    // against the ADAQ settle tables for the active filter/ODR.
    // settle=8: the ADAQ7769-1 filter settles in 3-7 output samples
    // (SINC3/SINC5/wideband, datasheet Table 5). The old value of 64 caused
    // COARSE floods during HI<->MID autoranging at low ODR (64 samples at
    // 4kSPS = 16ms per transition). The hardware FILT_NOT_SETTLED status bit
    // already drives fine_valid=false independently; the software settle is a
    // backup guard, so 8 samples is ample margin at any configured rate.
    current_fusion_init(&b->fusion, &b->range, /*settle=*/8, /*blend=*/4);

    // Load per-board range threshold calibration from NVS. If present, apply
    // the calibrated trust windows to current_fusion so FINE is used up to
    // the exact hardware SET threshold rather than a conservative estimate.
    {
        range_threshold_cal_t thr = {0};
        if (range_cal_load(&thr) == ESP_OK && thr.magic == RANGE_CAL_MAGIC) {
            current_fusion_set_trust(&b->fusion, thr.i_trust_hi, thr.i_trust_mid);
        }
    }
    power_dsp_init(&b->dsp, current_odr);
    // Multi-resolution zoom tiers (x100 each) and a continuous 1024-pt Hann FFT.
    multires_init(&b->multires, /*tiers=*/4, /*factor=*/100, NULL, NULL);
    spectrum_init(&b->spectrum, 1024, SPEC_WIN_HANN);
    spectrum_set_enabled(&b->spectrum, true);
    b->fft_source = 0;   // current
    usb_stream_init(&b->usb);

    ESP_LOGI(TAG, "board init: ADAQ %d/%d, temp %d/2, idac %d, ODR %.0f SPS",
             (b->adaq_ok[0] + b->adaq_ok[1] + b->adaq_ok[2]), ADAQ_COUNT,
             (b->temp_ok[0] + b->temp_ok[1]), b->idac_ok, current_odr);
    return ESP_OK;
}

esp_err_t daq_board_read_current(daq_board_t *b, float *amps,
                                 current_range_t *range_out)
{
    current_range_t range = range_manager_poll(&b->range);

    // LO (and any mid-transition) is read by the always-valid COARSE ADAQ;
    // HI/MID by the FINE ADAQ (its mux already points at the right CSA).
    uint8_t idx = range_uses_coarse(range) ? ADAQ_ROLE_COARSE : ADAQ_ROLE_FINE;
    if (range == RANGE_UNKNOWN) {
        idx = ADAQ_ROLE_COARSE;
        range = RANGE_LO;   // use the 50 mohm scale during a transition
    }
    if (!b->adaq_ok[idx]) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t raw = 0;
    esp_err_t err = adaq7769_read_sample(&b->adaq[idx], &raw);
    if (err != ESP_OK) {
        return err;
    }
    float v_adc = adaq7769_code_to_volts(&b->adaq[idx], raw);
    *amps = range_manager_volts_to_amps(&b->range, range, v_adc);
    if (range_out) {
        *range_out = range;
    }
    return ESP_OK;
}

esp_err_t daq_board_process_step(daq_board_t *b, fusion_output_t *out)
{
    // 1) Voltage (slow channel) -> hold in the DSP.
    if (b->adaq_ok[ADAQ_ROLE_VOLTAGE]) {
        int32_t vr = 0;
        if (adaq7769_read_sample(&b->adaq[ADAQ_ROLE_VOLTAGE], &vr) == ESP_OK) {
            float v = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_VOLTAGE], vr);
            // TODO: apply the V_DUT sense divider scale once characterised.
            power_dsp_set_voltage(&b->dsp, v);
        }
    }

    // 2) Observe range and read FINE + COARSE (both SYNC-aligned).
    current_range_t range = range_manager_poll(&b->range);

    fusion_input_t in = {
        .range        = range,
        .fine_valid   = false,
        .coarse_valid = false,
        .fine_v       = 0.0f,
        .coarse_v     = 0.0f,
    };

    if (b->adaq_ok[ADAQ_ROLE_FINE]) {
        int32_t fr = 0;
        if (adaq7769_read_sample(&b->adaq[ADAQ_ROLE_FINE], &fr) == ESP_OK) {
            in.fine_v     = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_FINE], fr);
            in.fine_valid = true;
        }
    }
    if (b->adaq_ok[ADAQ_ROLE_COARSE]) {
        int32_t cr = 0;
        if (adaq7769_read_sample(&b->adaq[ADAQ_ROLE_COARSE], &cr) == ESP_OK) {
            in.coarse_v     = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_COARSE], cr);
            in.coarse_valid = true;
        }
    }

    // 3) Fuse -> seamless current; 4) feed the power DSP.
    fusion_output_t fo;
    current_fusion_step(&b->fusion, &in, &fo);
    power_dsp_push_current(&b->dsp, fo.amps);

    if (out) {
        *out = fo;
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// USB streaming integration
// -----------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Deferred control task — processes heavy USB commands off the TinyUSB stack.
// CMD_SET_RATE calls daq_board_stop_fast (which has a vTaskDelay) + 3× SPI
// writes + daq_board_run_fast; running that chain inline on the 4096-byte
// TinyUSB task overflows its stack (Stack protection fault, MCAUSE=0x1b).
// ---------------------------------------------------------------------------
typedef enum { CTRL_MSG_SET_RATE, CTRL_MSG_SET_SOURCE,
               CTRL_MSG_RANGE_CAL_START } ctrl_msg_type_t;

typedef struct {
    ctrl_msg_type_t type;
    union {
        usb_cmd_rate_t      rate;
        usb_cmd_source_t    source;
        usb_cmd_range_cal_t range_cal;
    };
} ctrl_msg_t;

static void daq_ctrl_task(void *arg)
{
    daq_board_t *b = (daq_board_t *)arg;
    ctrl_msg_t msg;
    while (1) {
        if (xQueueReceive(b->ctrl_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        switch (msg.type) {

            case CTRL_MSG_SET_RATE: {
                const usb_cmd_rate_t *c = &msg.rate;
                // ADAQ hardware ODR (current_sps, voltage_sps) is applied
                // exclusively by the BBP settings path:
                //   desktop daq_cfg_set_enum → S3 link → apply_sample_rate
                //                           → daq_board_stop_fast/run_fast
                // Re-applying the same stop/run here races with that path and
                // double-frees the PSRAM ring buffers, causing "one batch then
                // nothing" after the first CMD_START.
                // Only the USB stream decimation is safe to update here; it is
                // not covered by BBP and requires no SPI bus access. Map the
                // requested SPS onto stream decimation relative to the cached
                // hardware ODRs, so hosts without a BBP control plane (the
                // iOS WiFi client) get a real effective-rate change without
                // touching the ADAQ config.
                uint32_t decim = (c->decimation >= 1) ? c->decimation : 1;
                if (c->current_sps > 0 && b->fine_rate_hz > 0) {
                    uint32_t d = (b->fine_rate_hz + c->current_sps / 2)
                                 / c->current_sps;
                    if (d < 1) d = 1;
                    if (d > 255) d = 255;
                    if (d > decim) decim = d;
                }
                b->wave_decim = (uint8_t)decim;
                b->wave_count = 0;

                uint32_t vdecim = 1;
                if (c->voltage_sps > 0 && b->volt_rate_hz > 0) {
                    vdecim = (b->volt_rate_hz + c->voltage_sps / 2)
                             / c->voltage_sps;
                    if (vdecim < 1) vdecim = 1;
                    if (vdecim > 255) vdecim = 255;
                }
                b->volt_decim = (uint8_t)vdecim;
                b->volt_count = 0;
                ESP_LOGI(TAG, "SET_RATE: wave_decim=%u volt_decim=%u "
                         "(req i=%lu v=%lu, odr i=%lu v=%lu)",
                         (unsigned)b->wave_decim, (unsigned)b->volt_decim,
                         (unsigned long)c->current_sps,
                         (unsigned long)c->voltage_sps,
                         (unsigned long)b->fine_rate_hz,
                         (unsigned long)b->volt_rate_hz);
                break;
            }

            case CTRL_MSG_SET_SOURCE: {
                const usb_cmd_source_t *c = &msg.source;
                daq_board_set_source(b, c->vdut, c->ilimit, c->enable != 0);
                break;
            }

            case CTRL_MSG_RANGE_CAL_START: {
                const usb_cmd_range_cal_t *rc = &msg.range_cal;
                b->range_cal.r_cal_a_ohm = (rc->r_cal_a_ohm > 0.0f)
                                           ? rc->r_cal_a_ohm : 5600.0f;
                b->range_cal.r_cal_b_ohm = (rc->r_cal_b_ohm > 0.0f)
                                           ? rc->r_cal_b_ohm : 56.0f;
                if (range_cal_start(&b->range_cal, b) != ESP_OK) {
                    ESP_LOGE(TAG, "range_cal_start failed");
                }
                break;
            }
        }
    }
}

// Control commands from the PC. Bound to b->usb via usb_stream_set_cmd_cb.
static void usb_cmd_handler(usb_rec_type_t cmd, const uint8_t *payload,
                            uint16_t len, void *user)
{
    daq_board_t *b = (daq_board_t *)user;
    switch (cmd) {
        case USB_CMD_START:
            // Session reset before streaming: with the fast path running the
            // reset must be deferred to the producer task (single-writer —
            // see usb_stream_reset_session); with no producer it is safe to
            // apply immediately so the reset is not left pending forever.
            if (b->fast_running) {
                usb_stream_reset_session(&b->usb);
            } else {
                usb_stream_reset_apply(&b->usb);
            }
            usb_stream_set_streaming(&b->usb, true);
            ESP_LOGI(TAG, "CMD_START: streaming on (mounted=%d, fast=%d)",
                     usb_backend_mounted(), b->fast_running);
            break;
        case USB_CMD_STOP:
            usb_stream_set_streaming(&b->usb, false);
            ESP_LOGI(TAG, "CMD_STOP: streaming off");
            usb_stream_flush_wave_i(&b->usb);
            usb_stream_flush_wave_v(&b->usb);
            break;
        case USB_CMD_RESET_ENERGY:
            power_dsp_reset_energy(&b->dsp);
            break;
        case USB_CMD_RESET_STATS:
            power_dsp_reset_stats(&b->dsp);
            spectrum_reset(&b->spectrum);
            break;
        case USB_CMD_FFT_CONFIG:
            if (len >= sizeof(usb_cmd_fft_t)) {
                const usb_cmd_fft_t *c = (const usb_cmd_fft_t *)payload;
                uint16_t n = (uint16_t)(c->nbins * 2);
                if (spectrum_configure(&b->spectrum, n,
                                       (spectrum_window_t)c->window) == ESP_OK) {
                    b->fft_source = c->source ? 1 : 0;
                    spectrum_set_enabled(&b->spectrum, c->enabled != 0);
                }
            }
            break;
        case USB_CMD_RANGE_LOCK:
            if (len >= 1) {
                uint8_t r = payload[0];
                range_manager_force(&b->range,
                                    (r >= RANGE_COUNT) ? RANGE_UNKNOWN
                                                       : (current_range_t)r);
            }
            break;
        case USB_CMD_SET_SOURCE:
            // Deferred to ctrl task — SMU I2C writes must not run on the
            // 4096-byte TinyUSB stack.
            if (len >= sizeof(usb_cmd_source_t) && b->ctrl_queue) {
                ctrl_msg_t msg = { .type = CTRL_MSG_SET_SOURCE };
                memcpy(&msg.source, payload, sizeof(usb_cmd_source_t));
                xQueueSend(b->ctrl_queue, &msg, 0); // non-blocking; drop if queue full
            }
            break;
        case USB_CMD_ARM:
            if (len >= sizeof(usb_cmd_arm_t)) {
                const usb_cmd_arm_t *c = (const usb_cmd_arm_t *)payload;
                usb_stream_set_arm(&b->usb, c->armed != 0, c->pre_samples);
            }
            break;
        case USB_CMD_RANGE_CAL_START:
            // Deferred to ctrl task: range_cal_start spawns a task which calls
            // daq_board_stop_fast — must not run on the TinyUSB stack.
            if (b->ctrl_queue) {
                ctrl_msg_t msg = { .type = CTRL_MSG_RANGE_CAL_START };
                if (len >= sizeof(usb_cmd_range_cal_t))
                    memcpy(&msg.range_cal, payload, sizeof(usb_cmd_range_cal_t));
                xQueueSend(b->ctrl_queue, &msg, 0);
            }
            break;
        case USB_CMD_RANGE_CAL_ACK:
            range_cal_ack(&b->range_cal);
            break;
        case USB_CMD_RANGE_CAL_ABORT:
            range_cal_abort(&b->range_cal);
            break;
        case USB_CMD_OTA_BEGIN: {
            // Direct desktop->P4 staging ingest (bypasses S3/WiFi entirely).
            // Trailing byte selects the eventual relay destination
            // (RELAY_TARGET_C6 / _S3); default to C6 if the host omits it,
            // reject an unrecognized value (matches the S3-link OTA_BEGIN path).
            if (len < sizeof(ota_meta_t)) {
                ESP_LOGE(TAG, "USB_CMD_OTA_BEGIN: payload too short");
                break;
            }
            ota_meta_t meta;
            memcpy(&meta, payload, sizeof(meta));
            relay_target_t target = RELAY_TARGET_C6;
            if (len > sizeof(meta)) {
                uint8_t raw_target = payload[sizeof(meta)];
                if (raw_target != RELAY_TARGET_C6 && raw_target != RELAY_TARGET_S3) {
                    ESP_LOGE(TAG, "USB_CMD_OTA_BEGIN: unrecognized relay target %u", raw_target);
                    break;
                }
                target = (relay_target_t)raw_target;
            }
            if (relay_stage_begin(target, &meta) != ESP_OK) {
                ESP_LOGE(TAG, "USB_CMD_OTA_BEGIN: relay_stage_begin failed");
            }
            break;
        }
        case USB_CMD_OTA_DATA: {
            if (len < 4) {
                ESP_LOGE(TAG, "USB_CMD_OTA_DATA: payload too short");
                break;
            }
            uint32_t offset;
            memcpy(&offset, payload, sizeof(offset));
            if (relay_stage_write(offset, payload + 4, len - 4) != ESP_OK) {
                ESP_LOGE(TAG, "USB_CMD_OTA_DATA: relay_stage_write failed");
            }
            break;
        }
        case USB_CMD_OTA_END:
            if (relay_stage_end() != ESP_OK) {
                ESP_LOGE(TAG, "USB_CMD_OTA_END: relay_stage_end/verify failed");
            }
            break;
        case USB_CMD_OTA_ABORT:
            relay_stage_reset(RELAY_FAILED);
            break;
        case USB_CMD_SET_RATE:
            // Deferred to ctrl task: stop_fast has vTaskDelay + SPI writes +
            // run_fast, which overflows the 4096-byte TinyUSB task stack
            // (Stack protection fault, MCAUSE=0x1b, Core 0 panic).
            if (len >= sizeof(usb_cmd_rate_t) && b->ctrl_queue) {
                ctrl_msg_t msg = { .type = CTRL_MSG_SET_RATE };
                memcpy(&msg.rate, payload, sizeof(usb_cmd_rate_t));
                xQueueSend(b->ctrl_queue, &msg, 0); // non-blocking; drop if queue full
            }
            break;
        default:
            break;
    }
}

esp_err_t daq_board_usb_start(daq_board_t *b)
{
    // Create the deferred control queue and task BEFORE starting TinyUSB so
    // the queue exists as soon as the first USB command could arrive.
    if (b->ctrl_queue == NULL) {
        b->ctrl_queue = xQueueCreate(4, sizeof(ctrl_msg_t));
        if (b->ctrl_queue == NULL) {
            ESP_LOGE(TAG, "ctrl_queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (b->ctrl_task == NULL) {
        // Must outrank daq_fast_task (prio 12): at full ODR the fast-path
        // consumer is CPU-bound on core 0 with progress==true almost every
        // iteration (see daq_fast_task), so it never voluntarily yields.
        // A ctrl_task priority below that (previously 8) starves forever —
        // xQueueSend() posts SET_RATE/SET_SOURCE/RANGE_CAL_START but the
        // handler never runs, so the device looks unresponsive to commands.
        // Same class of bug already fixed once for the TinyUSB task, see the
        // priority=13 comment in usb_backend.c.
        BaseType_t ok = xTaskCreatePinnedToCore(
            daq_ctrl_task, "daq_ctrl", 4096, b,
            /*prio=*/14, &b->ctrl_task, /*core=*/0);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "daq_ctrl_task create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    usb_stream_set_cmd_cb(&b->usb, usb_cmd_handler, b);
    esp_err_t err = usb_backend_start(&b->usb);
    b->usb_ok = (err == ESP_OK);
    if (b->usb_ok) {
        // Do NOT enable streaming here: at boot there is no host reading yet,
        // so WAVE frames would flow into the TX FIFO with nobody draining it
        // (bench: 1989 FIFO frame drops before the host even connected).
        // Streaming is gated on USB_CMD_START / HATP_CMD_DAQ_START instead.
        ESP_LOGI(TAG, "USB measurement stream ready (streaming gated on CMD_START)");
    }
    return err;
}

esp_err_t daq_board_stream_step(daq_board_t *b, fusion_output_t *out)
{
    fusion_output_t fo;
    esp_err_t err = daq_board_process_step(b, &fo);
    if (err != ESP_OK) {
        return err;
    }
    float p = power_dsp_last_p(&b->dsp);

    // Feed multi-resolution reduction + continuous spectrum.
    multires_push(&b->multires, fo.amps);
    spectrum_push(&b->spectrum, (b->fft_source == 1) ? p : fo.amps);

    uint32_t rate = (uint32_t)adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE]);
    usb_stream_push_sample(&b->usb, &fo, rate, /*decimation=*/1,
                           range_manager_settling(&b->range));
    // Non-fast path has no independent voltage sample cadence here (the VOLTAGE
    // ADC is only drained on the fast path's bus-B loop) — the current sample
    // is pushed above; no WAVE_V push is emitted from this path.
    if (out) {
        *out = fo;
    }
    return ESP_OK;
}

esp_err_t daq_board_stream_summary(daq_board_t *b)
{
    // Summary frames (Stats/Energy/Status/FFT) go through the same vendor bulk-IN
    // TX FIFO as waveform frames (8192 bytes). When not streaming, no host is
    // reading the FIFO, so summary frames accumulate unread. By the time the user
    // starts a capture the FIFO is full (~8 s of summary at 10 Hz) and the first
    // waveform frames (4118 bytes each) are dropped immediately because only 16
    // bytes are available. Gate all summary output on streaming to keep the FIFO
    // empty when idle and ready for waveform bursts when streaming starts.
    if (!b->usb.streaming) {
        return ESP_OK;
    }
    usb_stream_flush_wave_i(&b->usb);
    usb_stream_flush_wave_v(&b->usb);
    usb_stream_send_stats(&b->usb, &b->dsp);
    usb_stream_send_energy(&b->usb, &b->dsp);

    // Continuous spectrum: send the latest averaged magnitude bins.
    static float mags[SPECTRUM_MAX_BINS];
    uint16_t nb = spectrum_get_magnitude(&b->spectrum, mags, SPECTRUM_MAX_BINS);
    if (nb > 0) {
        // Spectrum is fed the DECIMATED tap, so its sample rate (and thus the
        // FFT frequency axis) is the fused ODR / dsp_decim.
        uint8_t dd = b->dsp_decim ? b->dsp_decim : 1;
        uint32_t rate =
            (uint32_t)adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE]) / dd;
        usb_stream_send_fft(&b->usb, mags, nb, rate, b->fft_source,
                            (uint8_t)SPEC_WIN_HANN);
    }

    // Device status (range, streaming, SMU set-points, FINE ADC health).
    //
    // fine_err_pct: ratio of FINE ADAQ status-header reads that carried an
    // error bit (ADC/DIG/CLK/SAT/UNSETTLED/SPI) since boot/reset, expressed
    // as 0-100. A persistent 100 means every FINE sample is invalid and the
    // fused stream is running on COARSE only.
    const adaq7769_t *fine_dev = &b->adaq[ADAQ_ROLE_FINE];
    uint8_t fine_err_pct = 0;
    if (b->adaq_ok[ADAQ_ROLE_FINE] && fine_dev->diag_status_reads > 0) {
        uint64_t pct = (uint64_t)fine_dev->diag_err_count * 100u
                     / fine_dev->diag_status_reads;
        fine_err_pct = (uint8_t)(pct > 100 ? 100 : pct);
    } else if (!b->adaq_ok[ADAQ_ROLE_FINE]) {
        fine_err_pct = 100;   // not present = effectively 100% bad
    }
    uint8_t adaq_ok_bits = (b->adaq_ok[0] ? 1u : 0u)
                         | (b->adaq_ok[1] ? 2u : 0u)
                         | (b->adaq_ok[2] ? 4u : 0u);

    // Direct-USB relay/staging ingest progress (USB_CMD_OTA_*). Exposed here
    // rather than as a per-command ack since usb_cmd_cb_t has no response
    // channel; the desktop polls this heartbeat to learn staging state.
    relay_status_t relay_st;
    relay_stage_get_status(&relay_st);

    usb_status_payload_t st = {
        .sample_rate    = (uint32_t)adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE]),
        .overflow_count = 0,
        .range          = (uint8_t)range_manager_current(&b->range),
        .streaming      = 1,
        .range_locked   = b->range.override_active ? 1 : 0,
        .source_enabled = b->smu.enabled ? 1 : 0,
        .vdut_set       = b->smu.vdut_set,
        .ilimit_set     = b->smu.ilimit_set,
        .in_voltage     = 0.0f,
        .in_current     = 0.0f,
        .adaq_ok_bits     = adaq_ok_bits,
        .fine_err_pct     = fine_err_pct,
        .drop_fine        = (uint16_t)(b->drop_fine  > 0xFFFFu ? 0xFFFFu : b->drop_fine),
        .drop_coarse      = (uint16_t)(b->drop_coarse > 0xFFFFu ? 0xFFFFu : b->drop_coarse),
        .fine_diag_sticky = b->adaq_ok[ADAQ_ROLE_FINE]
                                ? fine_dev->diag_sticky : 0xFFu,
        .frames_tx        = b->usb.tx_frames,
        .bytes_per_sec    = b->usb.bytes_per_sec,
        .fifo_drop_frames = b->usb.dropped_frames,
        .ring_high_water  = 0, // filled if adaq_stream exposes it; else 0 (documented)
        .wave_i_index_lo  = (uint32_t)b->usb.sample_seq,
        .relay_target       = (uint8_t)relay_st.target,
        .relay_state        = (uint8_t)relay_st.state,
        .relay_image_size   = relay_st.image_size,
        .relay_staged_bytes = relay_st.staged_bytes,
        .relay_pushed_bytes = relay_st.pushed_bytes,
    };
    usb_stream_get_type_counters(&b->usb, &st.wave_i_frames, &st.wave_v_frames,
                                 &st.wave_i_drops, &st.wave_v_drops);
    usb_stream_send_status(&b->usb, &st);

    // usb_stream_perf_tick() computes the TX throughput EMA and emits a 1 Hz
    // perf log; this summary path runs at 10 Hz, so tick every 10th call.
    if (++b->perf_div >= 10) {
        b->perf_div = 0;
        usb_stream_perf_tick(&b->usb);
    }
    return ESP_OK;
}

esp_err_t daq_board_set_source(daq_board_t *b, float vdut, float ilimit,
                               bool enable)
{
    if (ilimit > 0.0f) {
        smu_set_current_limit(&b->smu, ilimit);
    }
    if (vdut > 0.0f) {
        smu_set_voltage(&b->smu, vdut);
    }
    esp_err_t err = smu_enable(&b->smu, enable);
    return err;
}

// -----------------------------------------------------------------------------
// ESP32-S3 mainboard link (HAT-protocol slave)
// -----------------------------------------------------------------------------

// Payload layout for HATP_CMD_DAQ_SET_SOURCE (matches usb_cmd_source_t order).
typedef struct __attribute__((packed)) {
    float   vdut;
    float   ilimit;
    uint8_t enable;
} s3_set_source_t;

// Active OTA target (set on OTA_BEGIN): HATP_OTA_TARGET_P4 / _C6 / _STAGE.
static uint8_t  s_ota_target = HATP_OTA_TARGET_P4;
static uint32_t s_c6_ota_size = 0;

// When s_ota_target == HATP_OTA_TARGET_STAGE, a second trailing byte on
// OTA_BEGIN (payload[sizeof(ota_meta_t) + 1]) selects which relay target the
// staged image is ultimately destined for (RELAY_TARGET_C6 / _S3), since the
// wire protocol only reserves one generic "stage" target value.
static relay_target_t s_relay_target = RELAY_TARGET_C6;

// Bring the C6 display link (DDP master) back up after a C6 flash hands UART2
// back. ddp_master_deinit() released the driver; init+start re-acquire it.
static void c6_link_restart(daq_board_t *b)
{
    ddp_master_init(&b->ddp);
    b->ddp.cal = &b->cal;   // rebind after init memset (CAL_CTRL handling)
    ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
}

// True from the moment a bring-up task is created until it exits, on every
// path. xTaskCreate() returning pdPASS does NOT mean the task has run yet, so
// the STARTING-state check alone left a narrow window where a second START
// could spawn a second bring-up task racing the first over the same softAP.
static volatile bool s_bringup_alive;

// Generation counter identifying the *current* bring-up attempt. Every START
// and every RECYCLE bumps this. Each wifi_stream_bringup_task() instance
// captures the generation value it was spawned with (passed explicitly via
// its task argument -- NOT read from this global at task-entry time, since a
// second START could already have bumped the global before the first task's
// first instruction runs) and, at every checkpoint below, treats
// "my generation != s_bringup_gen" as "I have been cancelled/superseded".
//
// A plain boolean cancel flag is NOT enough here: an earlier version of this
// fix used one, and RECYCLE's bounded wait can legitimately time out while
// the old task is stuck deep inside a single wifi_ap_start() call. RECYCLE
// then force-clears s_bringup_alive (so the next START, which the iOS
// recovery ladder issues immediately after recycling, isn't blocked) --
// but that new START also resets a shared boolean cancel flag, which
// un-cancels the still-running orphan right before its next checkpoint. The
// orphan then wrongly concludes it was never cancelled and can go on to
// stamp READY, racing the brand-new task over the same softAP/DNS/TCP
// resources. A per-attempt generation number has no such ambiguity: the
// orphan's captured generation can never again match s_bringup_gen once
// either RECYCLE or a new START has bumped it, so it stays permanently
// cancelled regardless of what happens afterward.
static volatile uint32_t s_bringup_gen;

// The generation that currently OWNS the AP/DNS/TCP resources (i.e. is the
// most recent task to have started touching them). Set by a bring-up task
// the moment it begins its AP stage, right before it first calls
// wifi_ap_start(). wifi_stream_bringup_cancel_unwind() only tears down those
// resources when its caller's generation still matches this -- otherwise a
// stale, cancelled task that is only now unwinding (because RECYCLE's bounded
// wait timed out while it was blocked deep in a single wifi_ap_start() call)
// would destroy a SUCCESSOR generation's live softAP/DNS/TCP instead of its
// own, and report IDLE over a link that is actually READY. Single-writer:
// only ever assigned by whichever bring-up task is currently in the AP stage.
static volatile uint32_t s_owner_gen;

// Small heap-allocated (not static/shared) argument block for
// wifi_stream_bringup_task(), so each spawned instance gets its OWN
// board pointer + generation pair -- freed by the task itself right after
// it copies both into locals at entry. Must not be a shared static: if a new
// task were spawned while an orphan from a prior generation had not yet read
// a shared instance, the two would alias and the orphan could observe the
// new task's generation instead of its own.
typedef struct {
    daq_board_t *board;
    uint32_t gen;
} bringup_task_arg_t;

// How long DAQ_WIFI_STREAM_FAILED persists before decaying to IDLE.
#define WIFI_STREAM_FAILED_DECAY_MS 5000u

// Bound on how long RECYCLE waits for an in-flight bring-up task to observe
// its generation has been superseded and exit before RECYCLE proceeds with
// its own teardown.
// The S3 side's hat_command() timeout for this HAT link command is 200ms
// (hat.cpp); this bound intentionally exceeds that, since a TIMEOUT/UNKNOWN
// reply on the S3 is explicitly acceptable (it still clears its own mirror
// and logs) and correctness here -- never letting a stale bring-up task
// resurrect the softAP after RECYCLE reports done -- matters more than
// fitting inside the S3's poll window. 300ms comfortably covers the task's
// per-stage work between cancel checks (the AP retry loop's own inter-
// attempt delay is 500ms, so this bound will not always catch a task
// blocked deep in a single wifi_ap_start() attempt -- see the comment at the
// wait loop below for what happens then) while staying well short of
// anything a human or the phone's UI would perceive as a hang.
#define BRINGUP_CANCEL_WAIT_MS 300u

// Tears down anything THIS generation may have brought up so far and exits
// without publishing READY/FAILED, leaving b->wifi_stream_info at IDLE.
// Called from every cancel checkpoint below -- including the very first one,
// at task entry, before this task has brought up anything at all.
//
// Ownership-gated: a cancelled/superseded task can be sitting on a VERY
// stale checkpoint result (RECYCLE's bounded wait can time out while this
// task is blocked deep in a single wifi_ap_start() call, whose own retry
// delay of 500ms comfortably outlasts that bound). By the time such an
// orphan reaches its next checkpoint, a fresh generation may have already
// started -- or even finished -- its own bring-up and be sitting on a live
// softAP/DNS/TCP stack. Tearing that down unconditionally would destroy a
// SUCCESSOR's resources and falsely report IDLE over a link that is READY.
// So the 4 teardown calls (and the state write to IDLE) only run
// `if (s_owner_gen == my_gen)` -- i.e. only when no later generation has
// claimed ownership of the resources since. When ownership has moved on,
// this task has nothing of its own left to clean up: it simply deletes
// itself, touching neither the resources nor b->wifi_stream_info nor
// s_bringup_alive, leaving all three exactly as the owning generation left
// them. s_bringup_gen itself is untouched either way -- it must keep
// advancing forward and is never owned by an individual task instance.
static void wifi_stream_bringup_cancel_unwind(daq_board_t *b, uint32_t my_gen)
{
    ESP_LOGW(TAG, "wifi stream bring-up: superseded/cancelled, unwinding");
    if (s_owner_gen == my_gen) {
        tcp_backend_stop();
        usb_backend_start(&b->usb);   // re-register the USB transport, mirroring wifi_stream_teardown()
        captive_dns_stop();
        wifi_ap_stop();
        ddp_master_set_wifi_stream_mode(&b->ddp, false);
        memset(&b->wifi_stream_info, 0, sizeof(b->wifi_stream_info));
        b->wifi_stream_info.state = DAQ_WIFI_STREAM_IDLE;
        s_bringup_alive = false;
    }
    vTaskDelete(NULL);
}

// Runs the actual DAQ WiFi streaming bring-up (softAP over ESP-Hosted, fast-
// fail DNS, TCP backend) in its own task so HATP_CMD_DAQ_WIFI_STREAM_START
// can return to the S3-link dispatcher immediately -- see the long comment
// at that command's case for why inline bring-up there breaks the S3's
// 200ms response wait. Self-deletes on completion; final state (READY or
// FAILED) is visible to the S3 via HATP_CMD_DAQ_WIFI_STREAM_INFO polling.
//
// Checks its own captured generation against s_bringup_gen at every stage
// boundary (before the AP retry loop, between AP/DNS/TCP stages, and
// immediately before publishing READY) so a concurrent RECYCLE -- or a
// concurrent new START, which permanently supersedes this instance too --
// can never be silently undone by this task resurrecting the softAP or
// stamping READY after being cancelled/superseded. See
// wifi_stream_bringup_cancel_unwind() above and the s_bringup_gen comment.
static void wifi_stream_bringup_task(void *arg)
{
    bringup_task_arg_t *targ = (bringup_task_arg_t *)arg;
    daq_board_t *b = targ->board;
    const uint32_t my_gen = targ->gen;   // captured ONCE, at entry -- never re-read from the arg struct again
    free(targ);

    // Generated ONCE per boot and cached, not regenerated on every START:
    // the SSID is MAC-derived (constant per device regardless), but the
    // password used to be freshly randomized on every single Play press.
    // iOS keys its saved WiFi credentials by SSID, and repeatedly re-joining
    // the same SSID with a different password each time is exactly the
    // pattern that trips its anti-flap protection -- it starts silently
    // refusing to even attempt association ("Unable to Join Network", no
    // station-connect event ever reaches the AP side) until the user
    // manually "Forget This Network"s it. Stable credentials for the life of
    // the boot avoid that entirely; a real reboot (new random password) is
    // still fine since that's infrequent, not a per-Play-press event.
    static char s_ssid[33];
    static char s_password[65];
    static bool s_ident_generated;
    if (!s_ident_generated) {
        daq_wifi_ident_generate(s_ssid, sizeof(s_ssid), s_password, sizeof(s_password));
        s_ident_generated = true;
    }
    const char *ssid = s_ssid;
    const char *password = s_password;

    // Cancel check: RECYCLE could have raced xTaskCreate() itself, landing
    // between the create call and this task's first scheduled instruction.
    if (my_gen != s_bringup_gen) wifi_stream_bringup_cancel_unwind(b, my_gen);

    ddp_master_set_wifi_stream_mode(&b->ddp, true);

    // The DDP command above is fire-and-forget: the C6 only starts bringing
    // up its ESP-Hosted SDIO slave stack (wifi_hosted_start(), called from
    // its ~100ms main loop once it observes the mode flag) AFTER it receives
    // and processes this, which is not instantaneous. With
    // CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY (sdkconfig.defaults)
    // the P4's own esp_wifi_init() below will NOT hard-reset the C6 as long
    // as this SDIO handshake succeeds -- but it still needs the C6 to have
    // already started listening, so this retry window covers the gap
    // between sending the DDP command and the C6 actually getting to
    // wifi_hosted_start(). (With the old unconditional-reset default,
    // hammering the SDIO bus immediately also reset the C6 back to square
    // one every time, wiping the very flag this command just set -- see
    // .mex/patterns/daq-hat-ios-wifi-streaming.md for that whole saga.)
    b->wifi_stream_info.stage = DAQ_WIFI_STAGE_AP;
    // Claim ownership of the AP/DNS/TCP resources now, before the first call
    // that actually touches them (wifi_ap_start() below). From this point on,
    // wifi_stream_bringup_cancel_unwind() will tear down on our behalf only
    // as long as no later generation has since claimed ownership out from
    // under us.
    s_owner_gen = my_gen;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 6; attempt++) {
        // Cancel check: bail out of the retry loop between attempts rather
        // than burning through the remaining ones once a recycle is pending.
        if (my_gen != s_bringup_gen) wifi_stream_bringup_cancel_unwind(b, my_gen);
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(500));
        err = wifi_ap_start(ssid, password);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "wifi_ap_start attempt %d failed: %s, retrying...",
                 attempt + 1, esp_err_to_name(err));
    }
    // Cancel check: AP came up (or the retries were exhausted) -- do not
    // proceed into DNS if a recycle landed while we were retrying/joining.
    if (my_gen != s_bringup_gen) wifi_stream_bringup_cancel_unwind(b, my_gen);
    if (err == ESP_OK) {
        b->wifi_stream_info.stage = DAQ_WIFI_STAGE_DNS;
        err = captive_dns_start();
    }
    if (my_gen != s_bringup_gen) wifi_stream_bringup_cancel_unwind(b, my_gen);
    if (err == ESP_OK) {
        b->wifi_stream_info.stage = DAQ_WIFI_STAGE_TCP;
        err = tcp_backend_start(&b->usb, DAQ_WIFI_STREAM_TCP_PORT);
    }
    // Cancel check: this is the critical one -- must run before the READY
    // state is ever published below, or a recycle that arrived just as the
    // TCP backend came up would be silently undone.
    if (my_gen != s_bringup_gen) wifi_stream_bringup_cancel_unwind(b, my_gen);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi stream start failed: %s", esp_err_to_name(err));
        tcp_backend_stop();
        captive_dns_stop();
        wifi_ap_stop();
        ddp_master_set_wifi_stream_mode(&b->ddp, false);
        b->wifi_stream_info.state = DAQ_WIFI_STREAM_FAILED;
        b->wifi_stream_info.failed_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_bringup_alive = false;
        vTaskDelete(NULL);
        return;
    }
    strlcpy(b->wifi_stream_info.ssid, ssid, sizeof(b->wifi_stream_info.ssid));
    strlcpy(b->wifi_stream_info.password, password, sizeof(b->wifi_stream_info.password));
    b->wifi_stream_info.port = DAQ_WIFI_STREAM_TCP_PORT;
    b->wifi_stream_info.host[0] = 192;
    b->wifi_stream_info.host[1] = 168;
    b->wifi_stream_info.host[2] = 4;
    b->wifi_stream_info.host[3] = 1;
    b->wifi_stream_info.state = DAQ_WIFI_STREAM_READY;
    s_bringup_alive = false;
    vTaskDelete(NULL);
}

// Shared by the explicit HATP_CMD_DAQ_WIFI_STREAM_STOP handler and the idle-
// timeout auto-teardown in daq_ui_task below -- same sequence either way.
static void wifi_stream_teardown(daq_board_t *b)
{
    tcp_backend_stop();
    usb_backend_start(&b->usb);   // re-register the USB transport
    captive_dns_stop();
    wifi_ap_stop();
    ddp_master_set_wifi_stream_mode(&b->ddp, false);
    memset(&b->wifi_stream_info, 0, sizeof(b->wifi_stream_info));
    b->wifi_stream_info.state = DAQ_WIFI_STREAM_IDLE;
}

static int s3_cmd_handler(uint8_t cmd, const uint8_t *payload, uint8_t len,
                          uint8_t *resp, void *user)
{
    daq_board_t *b = (daq_board_t *)user;
    switch (cmd) {
        case HATP_CMD_DAQ_START:
            // Same single-writer handoff as USB_CMD_START: defer to the
            // producer when it is running, apply directly otherwise.
            if (b->fast_running) {
                usb_stream_reset_session(&b->usb);
            } else {
                usb_stream_reset_apply(&b->usb);
            }
            usb_stream_set_streaming(&b->usb, true);
            return 0;

        case HATP_CMD_DAQ_STOP:
            usb_stream_set_streaming(&b->usb, false);
            usb_stream_flush_wave_i(&b->usb);
            usb_stream_flush_wave_v(&b->usb);
            return 0;

        // ---- DAQ WiFi streaming bring-up (BLE-driven; see daq_wifi_ident.h,
        // captive_dns.h, tcp_backend.h). Lifted from cmd_wifiap's "on"/"off"
        // branches (cli.c) minus the CLI printfs, with generated credentials
        // instead of argv-supplied ones.
        //
        // Response convention: like HATP_CMD_DAQ_START/_STOP, this is a
        // fire-and-forget command -- the "1-byte accept/reject" / "1-byte
        // ack" the S3 sees is really just which frame comes back
        // (HATP_RSP_OK vs HATP_RSP_ERROR via s3_link.c's generic dispatch),
        // not a payload byte (there is no per-cmd response code path for
        // these two in s3_link.c, same as DAQ_START/STOP today).
        //
        // MUST return immediately: this handler runs synchronously inside
        // the S3-link command dispatch, and the S3 side (hat.cpp
        // hat_daq_wifi_stream_start()) only waits 200ms for a response. The
        // actual bring-up (softAP/DNS/TCP, with a multi-attempt retry loop
        // for the C6's ESP-Hosted stack to come up) can take seconds -- doing
        // it inline here always times out the S3, which then retries WHILE
        // the first attempt is still finishing or has just succeeded,
        // colliding with it (tcp_backend_start() sees "already running" and
        // the retry's failure-cleanup tears down the just-established softAP
        // out from under the first, actually-successful attempt). So this
        // only kicks off wifi_stream_bringup_task() and returns; the S3
        // learns real status via the existing HATP_CMD_DAQ_WIFI_STREAM_INFO
        // poll / b->wifi_stream_info.state, which is exactly what that
        // state machine was already built for. ----
        case HATP_CMD_DAQ_WIFI_STREAM_START: {
            // Idempotent: a repeat START while one is already in flight or
            // already up must NOT reset state or spawn a second task. The
            // liveness flag covers the window between xTaskCreate() returning
            // and the task actually running.
            if (s_bringup_alive ||
                b->wifi_stream_info.state == DAQ_WIFI_STREAM_STARTING ||
                b->wifi_stream_info.state == DAQ_WIFI_STREAM_READY) {
                return 0;
            }
            memset(&b->wifi_stream_info, 0, sizeof(b->wifi_stream_info));
            b->wifi_stream_info.state = DAQ_WIFI_STREAM_STARTING;
            // Bump the generation so this attempt has an identity distinct
            // from anything before it -- an orphaned prior task (e.g. one
            // RECYCLE gave up waiting on) can never again match s_bringup_gen
            // once this fires, so it stays permanently cancelled no matter
            // what it does after this point. See the s_bringup_gen comment.
            // Non-atomic RMW on a volatile: safe only because the S3-link
            // dispatcher task is the single writer of s_bringup_gen (here and
            // at the RECYCLE handler below); it is never bumped concurrently
            // from another task.
            uint32_t gen = ++s_bringup_gen;
            s_bringup_alive = true;
            bringup_task_arg_t *targ = (bringup_task_arg_t *)malloc(sizeof(*targ));
            BaseType_t ok = pdFALSE;
            if (targ) {
                targ->board = b;
                targ->gen = gen;
                ok = xTaskCreate(wifi_stream_bringup_task, "wifi_bringup",
                                 4096, targ, 5, NULL);
            }
            if (ok != pdPASS) {
                free(targ);
                s_bringup_alive = false;
                b->wifi_stream_info.state = DAQ_WIFI_STREAM_FAILED;
                b->wifi_stream_info.failed_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
                return -1;
            }
            return 0;   // accepted -> HATP_RSP_OK (bring-up continues in background)
        }

        case HATP_CMD_DAQ_WIFI_STREAM_STOP:
            wifi_stream_teardown(b);
            return 0;   // ack -> HATP_RSP_OK

        case HATP_CMD_DAQ_WIFI_STREAM_RECYCLE: {
            // Escape hatch: the FULL teardown sequence, unconditionally,
            // ignoring every cached state flag. STOP is cooperative and can
            // itself be skipped or half-completed if state is inconsistent;
            // this exists precisely for when state is already wrong, and is
            // what lets the phone clear a wedge without a power-cycle.
            // Safe to run from any state because every call below is
            // idempotent (see wifi_ap.c's per-step guards and unconditional
            // s_up clear).
            //
            // A bring-up task can be mid-flight (inside its AP retry loop,
            // or about to publish READY) when RECYCLE lands. Blindly clearing
            // s_bringup_alive here (as an earlier version of this handler
            // did) does not stop that task -- it can go on to resurrect the
            // softAP and stamp READY right after our teardown below runs,
            // silently undoing the recycle. So: bump the generation first
            // (permanently orphaning whatever task is in flight -- see the
            // s_bringup_gen comment above), then wait -- BOUNDED, never
            // unbounded -- for it to actually exit before proceeding. The
            // wait is capped at BRINGUP_CANCEL_WAIT_MS (300ms), deliberately
            // more than the S3's 200ms hat_command() timeout for this
            // command: a TIMEOUT/UNKNOWN reply on the S3 side is explicitly
            // fine (it still clears its own mirror and logs), and never
            // letting a stale task resurrect the softAP after we report done
            // matters more than fitting the S3's poll window.
            //
            // If the task still hasn't exited after the bound (e.g. it is
            // blocked deep inside a single wifi_ap_start() call), we log and
            // proceed anyway. Unlike a boolean cancel flag, this is safe even
            // though the very next command is typically a fresh START (the
            // iOS recovery ladder recycles then immediately re-provisions):
            // that START bumps the generation AGAIN when it spawns its own
            // task, so the orphan's captured generation can never again
            // match s_bringup_gen -- it stays permanently cancelled and will
            // unwind to IDLE without publishing READY whenever it does next
            // reach a checkpoint, no matter how many further STARTs/RECYCLEs
            // happen in the meantime. The teardown below and the orphan's
            // eventual self-unwind race safely (both are idempotent and both
            // converge on IDLE).
            ESP_LOGW(TAG, "wifi stream: force recycle requested");
            // Non-atomic RMW on a volatile: safe only because the S3-link
            // dispatcher task is the single writer of s_bringup_gen (here and
            // at the START handler above); it is never bumped concurrently
            // from another task.
            ++s_bringup_gen;
            uint32_t waited_ms = 0;
            while (s_bringup_alive && waited_ms < BRINGUP_CANCEL_WAIT_MS) {
                vTaskDelay(pdMS_TO_TICKS(10));
                waited_ms += 10;
            }
            if (s_bringup_alive) {
                ESP_LOGW(TAG, "wifi stream recycle: bring-up task still alive after %ums wait, "
                              "proceeding with teardown anyway (task is permanently orphaned by "
                              "generation and will self-unwind on its own next checkpoint)",
                         BRINGUP_CANCEL_WAIT_MS);
            }
            s_bringup_alive = false;   // a stuck flag must not block the next START
            wifi_stream_teardown(b);
            b->wifi_stream_info.failed_at_ms = 0;
            return 0;   // ack -> HATP_RSP_OK
        }

        case HATP_CMD_DAQ_WIFI_STREAM_INFO: {
            // Chunk b->wifi_stream_info into the wire's [status][seq][flags]
            // + up to ~29 data bytes framing. While in a terminal state
            // (READY/FAILED), always re-chunk from the beginning on every
            // poll -- simpler than tracking cross-poll cursor state, and
            // re-answering old data repeatedly is harmless.
            uint8_t status;
            switch (b->wifi_stream_info.state) {
                case DAQ_WIFI_STREAM_READY:   status = HATP_WIFI_INFO_ST_READY;   break;
                case DAQ_WIFI_STREAM_FAILED:  status = HATP_WIFI_INFO_ST_FAILED;  break;
                default:                      status = HATP_WIFI_INFO_ST_STARTING; break;
            }

            if (status != HATP_WIFI_INFO_ST_READY) {
                resp[0] = status;
                resp[1] = 0;
                // Only FAILED is actually a terminal/final chunk. STARTING is
                // still in progress -- setting LAST here too was a real bug:
                // the S3 side (hat.cpp) treats LAST as "sequence complete",
                // so a STARTING poll landing with LAST set got misread as a
                // finished-but-not-READY sequence and immediately aborted the
                // whole bring-up as FAILED after just one poll, intermittently
                // (whenever a poll happened to land during STARTING rather
                // than after bring-up had already reached READY).
                resp[2] = (status == HATP_WIFI_INFO_ST_FAILED) ? HATP_WIFI_INFO_LAST : 0;
                // Extra byte (beyond the shared 3-byte header): coarse
                // bring-up substage, only meaningful while STARTING -- see
                // daq_wifi_stage_t.
                resp[3] = (uint8_t)b->wifi_stream_info.stage;
                return HATP_WIFI_INFO_HDR + 1;
            }

            s3link_wifi_stream_info_t blob;
            memset(&blob, 0, sizeof(blob));
            strlcpy(blob.ssid, b->wifi_stream_info.ssid, sizeof(blob.ssid));
            strlcpy(blob.password, b->wifi_stream_info.password, sizeof(blob.password));
            blob.port = b->wifi_stream_info.port;   // little-endian on this target
            memcpy(blob.host, b->wifi_stream_info.host, sizeof(blob.host));

            // Static cursor: reset to 0 whenever the caller starts a new
            // reassembly poll (seq==0 implicitly, since we always start a
            // fresh generation here -- there is only ever one link/one
            // connection using this command, so static state is safe).
            static uint8_t s_seq;
            static size_t  s_off;
            // Since we always re-chunk from the start for a terminal state,
            // detect "new poll cycle" simply by resetting whenever this is
            // called with s_off already having reached the end last time.
            if (s_off >= sizeof(blob)) { s_off = 0; s_seq = 0; }

            // Unlike HATP_CMD_STAGE_READ (where the S3 requests a length it
            // knows fits its own 32-byte HAT_FRAME_MAX_LEN), this response is
            // sized unilaterally by the P4 -- it must not chunk against the
            // P4-local HATP_MAX_PAYLOAD budget (240 B), or the frame overflows
            // the S3's real wire limit and hat_recv_frame() drops it outright.
            const size_t max_data = (size_t)HAT_WIRE_FRAME_MAX_LEN - HATP_WIFI_INFO_HDR;
            size_t remaining = sizeof(blob) - s_off;
            size_t chunk_len = (remaining < max_data) ? remaining : max_data;
            bool last = (s_off + chunk_len) >= sizeof(blob);

            resp[0] = HATP_WIFI_INFO_ST_READY;
            resp[1] = s_seq;
            resp[2] = last ? HATP_WIFI_INFO_LAST : 0;
            memcpy(resp + HATP_WIFI_INFO_HDR, (const uint8_t *)&blob + s_off, chunk_len);

            s_off += chunk_len;
            s_seq++;
            if (last) { s_off = 0; s_seq = 0; }   // rewind so the next poll re-sends from the start

            return (int)(HATP_WIFI_INFO_HDR + chunk_len);
        }

        case HATP_CMD_DAQ_SET_SOURCE:
            if (len >= sizeof(s3_set_source_t)) {
                const s3_set_source_t *c = (const s3_set_source_t *)payload;
                daq_board_set_source(b, c->vdut, c->ilimit, c->enable != 0);
                return 0;
            }
            return -1;

        case HATP_CMD_DAQ_SYNC:
            // Establish a shared timebase epoch with the S3 (pre/post run). The
            // S3 timestamps its digital markers against this sample index.
            b->sync_epoch = b->usb.sample_seq;
            return 0;

        case HATP_CMD_DAQ_ARM:
            // Arm/disarm the trigger latch + record the pre-roll depth. The S3
            // evaluates the OR/AND IO logic; the P4 only annotates semantics
            // (the PC keeps the actual pre-trigger window from the live stream).
            if (len >= sizeof(s3link_daq_arm_t)) {
                const s3link_daq_arm_t *a = (const s3link_daq_arm_t *)payload;
                usb_stream_set_arm(&b->usb, a->armed != 0, a->pre_samples);
                return 0;
            }
            return -1;

        case HATP_CMD_DAQ_MARK:
            // A digital event fired on an S3 IO. Emit a MARKER aligned to the
            // live sample index (sub-sample HW timestamping refines this via the
            // shared IRQ line; here we use the UART-arrival sample index).
            if (len >= sizeof(s3link_daq_mark_t)) {
                const s3link_daq_mark_t *m = (const s3link_daq_mark_t *)payload;
                usb_stream_send_marker(&b->usb, m->channel, m->edge, m->kind,
                                       UINT64_MAX);
                return 0;
            }
            return -1;

        case HATP_CMD_SET_CH_LEDS:
            // 4 channel-status colour codes from the S3 -> relay to the C6
            // neopixels (rendered in pairs, DAQ_NPX_CHANNEL mode).
            if (len < 4) return -1;
            if (b->ddp.running) ddp_master_set_ch_leds(&b->ddp, payload);
            return 0;

        case HATP_CMD_DAQ_TELEMETRY:
            // S3 mainboard telemetry snapshot (die temp, USB-PD, VADJ/VLOGIC
            // rails) -> cache + timestamp for relay to the C6 diagnostics menu.
            if (len < sizeof(s3link_telemetry_t)) return -1;
            memcpy(&b->s3_telem, payload, sizeof(b->s3_telem));
            b->s3_telem_ms = (uint32_t)(esp_timer_get_time() / 1000);
            return 0;

        case HATP_CMD_MB_POLL:
            // The S3 is polling for a pending C6 "Main Board Settings" request.
            // While streaming to the PC, defer only the heavier *script*
            // requests (list/run/stop) so they never contend with acquisition;
            // the tiny power reads/writes (rail + e-fuse status/toggle) stay
            // live so the Power menu keeps working during a capture.
            if (b->fast_running) {
                uint8_t t = ddp_master_peek_mb_type(&b->ddp);
                if (t == DDP_MB_SCRIPTS || t == DDP_MB_SCRIPT_RUN ||
                    t == DDP_MB_SCRIPT_STOP)
                    return 0;
            }
            return (int)ddp_master_take_mb_request(&b->ddp, resp, HATP_MAX_PAYLOAD);

        case HATP_CMD_MB_RESULT: {
            // Reassemble the chunked execution result from the S3:
            // [type][status][seq][flags][data]. Concatenate the data across
            // chunks; on the final chunk relay [type][status][all-data] to the
            // C6 as DDP_CMD_MB_RESPONSE. Single-link, so static state is safe.
            static uint8_t  asm_buf[248];
            static uint16_t asm_len = 0;
            static uint8_t  asm_seq = 0;
            static bool     asm_active = false;
            if (len < HATP_MB_RSLT_HDR) return -1;
            uint8_t seq = payload[2];
            uint8_t flags = payload[3];
            const uint8_t *chunk = &payload[HATP_MB_RSLT_HDR];
            int chunk_len = (int)len - HATP_MB_RSLT_HDR;
            if (seq == 0) {
                asm_buf[0] = payload[0];   // req_type
                asm_buf[1] = payload[1];   // status
                asm_len = 2;
                asm_seq = 0;
                asm_active = true;
            }
            if (!asm_active || seq != asm_seq) { asm_active = false; return -1; }
            if (chunk_len > 0 &&
                asm_len + chunk_len <= (int)sizeof(asm_buf)) {
                memcpy(&asm_buf[asm_len], chunk, chunk_len);
                asm_len += (uint16_t)chunk_len;
            }
            asm_seq++;
            if (flags & HATP_MB_RSLT_LAST) {
                ddp_master_send(&b->ddp, DDP_CMD_MB_RESPONSE, asm_buf, asm_len);
                asm_active = false;
            }
            return 0;
        }

        // ---- SMU factory calibration ----
        case HATP_CMD_DAQ_CAL_START: {
            if (len < 1) return -1;
            smu_cal_mode_t mode = (payload[0] == SMU_CAL_MODE_CURRENT)
                                      ? SMU_CAL_MODE_CURRENT
                                      : SMU_CAL_MODE_VOLTAGE;
            return (smu_cal_start(&b->cal, mode) == ESP_OK) ? 0 : -1;
        }
        case HATP_CMD_DAQ_CAL_ACK:
            smu_cal_ack(&b->cal);
            return 0;
        case HATP_CMD_DAQ_CAL_ABORT:
            smu_cal_abort(&b->cal);
            return 0;
        case HATP_CMD_DAQ_CAL_STATUS: {
            smu_cal_status_t st;
            smu_cal_get_status(&b->cal, &st);
            memcpy(resp, &st, sizeof(st));
            return (int)sizeof(st);
        }

        case HATP_CMD_DAQ_GET_STATUS: {
            s3link_daq_status_t st = {
                .range          = (uint8_t)range_manager_current(&b->range),
                .streaming      = b->usb.streaming ? 1 : 0,
                .source_enabled = b->smu.enabled ? 1 : 0,
                ._pad           = 0,
                .last_i         = power_dsp_last_i(&b->dsp),
                .last_v         = power_dsp_last_v(&b->dsp),
                .last_p         = power_dsp_last_p(&b->dsp),
                .energy_mwh     = (float)power_dsp_energy_mwh(&b->dsp),
            };
            memcpy(resp, &st, sizeof(st));
            return (int)sizeof(st);
        }

        // ---- VDUT (programmable DUT power supply, smu.{c,h}) ----
        case HATP_CMD_DAQ_VDUT_STATUS: {
            bool present = (b->smu.idac && b->smu.idac->present);
            float meas_i;
            esp_err_t ierr = smu_read_output_current(&b->smu, &meas_i);
            s3link_vdut_status_t st = {
                .present      = present ? 1 : 0,
                .enabled      = b->smu.enabled ? 1 : 0,
                .fault        = (!present) ? 1 : 0,
                ._pad         = 0,
                .vdut_set_v   = b->smu.vdut_set,
                .ilimit_set_a = b->smu.ilimit_set,
                .meas_v       = power_dsp_last_v(&b->dsp),
                .meas_i       = (ierr == ESP_OK) ? meas_i : power_dsp_last_i(&b->dsp),
            };
            memcpy(resp, &st, sizeof(st));
            return (int)sizeof(st);
        }

        case HATP_CMD_DAQ_VDUT_ENABLE: {
            if (len < 1) return -1;
            bool present = (b->smu.idac && b->smu.idac->present);
            if (!present) return -1;
            return (smu_enable(&b->smu, payload[0] != 0) == ESP_OK) ? 0 : -1;
        }

        case HATP_CMD_DAQ_VDUT_SETPOINT: {
            if (len < sizeof(s3link_vdut_setpoint_t)) return -1;
            const s3link_vdut_setpoint_t *sp = (const s3link_vdut_setpoint_t *)payload;
            // Reject out-of-range requests rather than silently clamping (the
            // S3-side API also bounds-checks before ever sending this, but the
            // P4 re-validates since it's the source of truth for the hardware
            // limits).
            if (sp->vdut_v < SMU_VDUT_MIN || sp->vdut_v > SMU_VDUT_MAX ||
                sp->ilimit_a < SMU_ILIMIT_MIN_A || sp->ilimit_a > SMU_ILIMIT_FULLSCALE_A) {
                return -1;
            }
            bool present = (b->smu.idac && b->smu.idac->present);
            if (!present) return -1;
            esp_err_t e1 = smu_set_current_limit(&b->smu, sp->ilimit_a);
            esp_err_t e2 = smu_set_voltage(&b->smu, sp->vdut_v);
            return (e1 == ESP_OK && e2 == ESP_OK) ? 0 : -1;
        }

        // ---- Firmware version ----
        case HATP_CMD_GET_VERSION: {
            // Response: u32 packed version + NUL-terminated version string.
            uint32_t v = FW_VERSION_U32;
            memcpy(resp, &v, sizeof(v));
            size_t slen = strlen(FW_VERSION_STRING);
            if (slen > HATP_MAX_PAYLOAD - 5) slen = HATP_MAX_PAYLOAD - 5;
            memcpy(resp + 4, FW_VERSION_STRING, slen);
            resp[4 + slen] = 0;
            return (int)(5 + slen);
        }

        // ---- OTA (split/streaming: chunks written straight to flash) ----
        // OTA_BEGIN carries an optional trailing target byte: P4 self (default)
        // or C6 (flash the on-module display co-processor over UART2).
        case HATP_CMD_OTA_BEGIN: {
            if (len < sizeof(ota_meta_t)) return -1;
            ota_meta_t meta;
            memcpy(&meta, payload, sizeof(meta));
            s_ota_target = (len > sizeof(ota_meta_t)) ? payload[sizeof(ota_meta_t)]
                                                      : HATP_OTA_TARGET_P4;
            if (s_ota_target == HATP_OTA_TARGET_C6) {
                ddp_master_deinit(&b->ddp);          // hand UART2 to the flasher
                if (c6_flasher_begin(meta.image_size, 0) != ESP_OK) {
                    c6_link_restart(b);
                    return -1;
                }
                s_c6_ota_size = meta.image_size;
                return 0;
            }
            if (s_ota_target == HATP_OTA_TARGET_STAGE) {
                // Second trailing byte selects the eventual relay destination
                // (RELAY_TARGET_C6 / _S3); default to C6 if the host omits it.
                if (len > sizeof(ota_meta_t) + 1) {
                    uint8_t raw_relay_target = payload[sizeof(ota_meta_t) + 1];
                    if (raw_relay_target != RELAY_TARGET_C6 && raw_relay_target != RELAY_TARGET_S3) {
                        return -1;
                    }
                    s_relay_target = (relay_target_t)raw_relay_target;
                } else {
                    s_relay_target = RELAY_TARGET_C6;
                }
                return (relay_stage_begin(s_relay_target, &meta) == ESP_OK) ? 0 : -1;
            }
            return (ota_begin(&meta) == ESP_OK) ? 0 : -1;
        }
        case HATP_CMD_OTA_DATA: {
            if (len < sizeof(s3link_ota_data_hdr_t)) return -1;
            uint32_t offset;
            memcpy(&offset, payload, sizeof(offset));
            const uint8_t *fw = payload + sizeof(s3link_ota_data_hdr_t);
            uint8_t fw_len = (uint8_t)(len - sizeof(s3link_ota_data_hdr_t));
            if (s_ota_target == HATP_OTA_TARGET_C6) {
                return (c6_flasher_write(fw, fw_len) == ESP_OK) ? 0 : -1;
            }
            if (s_ota_target == HATP_OTA_TARGET_STAGE) {
                return (relay_stage_write(offset, fw, fw_len) == ESP_OK) ? 0 : -1;
            }
            return (ota_write(offset, fw, fw_len) == ESP_OK) ? 0 : -1;
        }
        case HATP_CMD_OTA_END:
            if (s_ota_target == HATP_OTA_TARGET_C6) {
                esp_err_t rc = c6_flasher_finish();
                c6_link_restart(b);                  // C6 now runs the new image
                return (rc == ESP_OK) ? 0 : -1;
            }
            if (s_ota_target == HATP_OTA_TARGET_STAGE) {
                esp_err_t rc = relay_stage_end();
                s_ota_target = HATP_OTA_TARGET_P4;
                return (rc == ESP_OK) ? 0 : -1;
            }
            return (ota_end() == ESP_OK) ? 0 : -1;
        case HATP_CMD_OTA_ABORT:
            if (s_ota_target == HATP_OTA_TARGET_C6) {
                c6_flasher_abort();
                c6_link_restart(b);
                s_ota_target = HATP_OTA_TARGET_P4;
                return 0;
            }
            if (s_ota_target == HATP_OTA_TARGET_STAGE) {
                relay_stage_reset(RELAY_FAILED);
                s_ota_target = HATP_OTA_TARGET_P4;
                return 0;
            }
            ota_abort();
            return 0;
        case HATP_CMD_OTA_STATUS: {
            if (s_ota_target == HATP_OTA_TARGET_C6) {
                uint32_t recv = c6_flasher_received();
                resp[0] = OTA_RECEIVING;            // C6 path has no PENDING_VERIFY
                resp[1] = 0;
                memcpy(resp + 2, &recv, 4);
                memcpy(resp + 6, &s_c6_ota_size, 4);
                return 10;
            }
            ota_status_t st;
            ota_get_status(&st);
            // Compact wire status: state, pending_verify, received, image_size.
            resp[0] = (uint8_t)st.state;
            resp[1] = st.pending_verify ? 1 : 0;
            memcpy(resp + 2, &st.received, 4);
            memcpy(resp + 6, &st.image_size, 4);
            return 10;
        }
        case HATP_CMD_OTA_CONFIRM:
            // The C6 ROM-flash path has no A/B rollback; confirm is a no-op for it.
            if (s_ota_target == HATP_OTA_TARGET_C6) return 0;
            return (ota_confirm() == ESP_OK) ? 0 : -1;
        case HATP_CMD_OTA_ROLLBACK:
            if (s_ota_target == HATP_OTA_TARGET_C6) return -1;   // unsupported for C6
            ota_rollback();   // reboots on success
            return -1;        // if it returns, it failed

        case HATP_CMD_STAGE_READ: {
            if (len < sizeof(s3link_stage_read_req_t)) return -1;
            s3link_stage_read_req_t req;
            memcpy(&req, payload, sizeof(req));
            if (req.len > HATP_OTA_CHUNK_MAX) req.len = HATP_OTA_CHUNK_MAX;
            int n = relay_stage_read(req.offset, resp, req.len);
            if (n < 0) return -1;
            return n;   // 0 = EOF, caller (s3_link.c) sends it back as HATP_RSP_STAGE_DATA
        }

        default:
            return -1;
    }
}

esp_err_t daq_board_s3_start(daq_board_t *b)
{
    esp_err_t err = s3_link_init(&b->s3, s3_cmd_handler, b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "S3 link init failed: %s", esp_err_to_name(err));
        return err;
    }
    // Must outrank daq_fast_task (prio 12), same starvation class already
    // fixed for daq_ctrl_task (see the priority=14 comment in
    // daq_board_usb_start): at high ODR daq_fast_task is CPU-bound on core 0
    // and rarely yields, so a lower-priority service_task can't drain the S3
    // UART link in time. That makes CMD_DAQ_CONFIG (0xB6, e.g. daq_cfg_set /
    // apply_sample_rate) time out on the S3 side (BBP_ERR_TIMEOUT, 0x11)
    // even though the P4 is alive — it just never got scheduled.
    return s3_link_start(&b->s3, /*core=*/0, /*prio=*/14);
}

// Client disconnect from the WiFi stream TCP port -> auto-teardown after
// this long with nobody (re)connected, so the DAQ HAT returns to normal
// (non-WiFi, USB-backend) operation without a manual stop. Also covers the
// case where the stream was started but a client never joined at all.
#define WIFI_STREAM_IDLE_TIMEOUT_MS 60000u

// UI task: relay front-panel buttons to the C6 and push the latest measurement
// for the on-screen readout. Low priority, ~20 ms cadence. Also owns the WiFi
// stream idle-timeout check above (same cadence is plenty for a 60s timer).
static void daq_ui_task(void *arg)
{
    daq_board_t *b = (daq_board_t *)arg;
    uint32_t last_meas = 0;
    uint32_t last_hello = 0;
    uint32_t wifi_disconnect_since_ms = 0;   // 0 == "not counting down"
    for (;;) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);

        if (b->wifi_stream_info.state == DAQ_WIFI_STREAM_READY) {
            if (tcp_backend_connected()) {
                wifi_disconnect_since_ms = 0;
            } else if (wifi_disconnect_since_ms == 0) {
                wifi_disconnect_since_ms = t;
            } else if ((t - wifi_disconnect_since_ms) >= WIFI_STREAM_IDLE_TIMEOUT_MS) {
                ESP_LOGI(TAG, "wifi stream: no client for %u ms, tearing down",
                         WIFI_STREAM_IDLE_TIMEOUT_MS);
                wifi_stream_teardown(b);
                wifi_disconnect_since_ms = 0;
            }
        } else {
            wifi_disconnect_since_ms = 0;
            // FAILED is not terminal: decay it back to IDLE so a transient
            // bring-up failure stops being reported as a permanent fault and
            // the next START starts from a clean state.
            if (b->wifi_stream_info.state == DAQ_WIFI_STREAM_FAILED &&
                !s_bringup_alive &&
                (t - b->wifi_stream_info.failed_at_ms) >= WIFI_STREAM_FAILED_DECAY_MS) {
                ESP_LOGI(TAG, "wifi stream: clearing FAILED state after %u ms",
                         WIFI_STREAM_FAILED_DECAY_MS);
                memset(&b->wifi_stream_info, 0, sizeof(b->wifi_stream_info));
                b->wifi_stream_info.state = DAQ_WIFI_STREAM_IDLE;
            }
        }

        // The C6 link (UART2) is handed to the flasher during a C6 firmware
        // update; skip all DDP traffic while it is down.
        if (b->ddp.running) {
            uint8_t ev = buttons_p4_poll(t);
            if (ev) ddp_master_button_event(&b->ddp, ev);

            if ((t - last_meas) >= 100) {
                last_meas = t;
                uint8_t mflags = DDP_FLAG_V_VALID | DDP_FLAG_I_VALID;
                if (b->smu.enabled) mflags |= DDP_FLAG_SRC_ON;
                // Pack the live current range (for the C6 home-screen badge).
                current_range_t rng = range_manager_current(&b->range);
                uint8_t rc = (rng == RANGE_HI)  ? DDP_RANGE_HI  :
                             (rng == RANGE_MID) ? DDP_RANGE_MID :
                             (rng == RANGE_LO)  ? DDP_RANGE_LO  : DDP_RANGE_UNKNOWN;
                mflags |= (uint8_t)(rc << DDP_FLAG_RANGE_SHIFT);
                ddp_master_set_measurement(&b->ddp,
                                           power_dsp_last_v(&b->dsp),
                                           power_dsp_last_i(&b->dsp),
                                           mflags);
            }

            // Periodic presence probe: prompt the C6 to reply with RSP_INFO so
            // the two discover each other regardless of boot order / a transient
            // link drop (the C6 also announces itself at 1 Hz). Cheap keepalive.
            if ((t - last_hello) >= 1000) {
                last_hello = t;
                ddp_master_send(&b->ddp, DDP_CMD_GET_INFO, NULL, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool daq_board_pd_ok(const daq_board_t *b, uint16_t min_mv, uint16_t min_ma)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (b->s3_telem_ms == 0 || (now - b->s3_telem_ms) > 5000) return false;
    if (!(b->s3_telem.flags & S3LINK_TLM_F_PD)) return false;
    return b->s3_telem.pd_mv >= min_mv && b->s3_telem.pd_ma >= min_ma;
}

esp_err_t daq_board_c6_start(daq_board_t *b)
{
    esp_err_t err = ddp_master_init(&b->ddp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DDP master init failed: %s", esp_err_to_name(err));
        return err;
    }
    b->ddp.cal = &b->cal;   // bind the cal engine for DDP_CMD_CAL_CTRL handling
    err = ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
    if (err != ESP_OK) return err;

    buttons_p4_init();

    BaseType_t ok = xTaskCreatePinnedToCore(daq_ui_task, "daq_ui", 4096, b,
                                             /*prio=*/5, NULL, /*core=*/0);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t daq_board_start_streaming(daq_board_t *b, size_t ring_capacity)
{

    // Per-sample CRC DISABLED (reverted 2026-07-24). It was enabled for one
    // session to reject corrupted conversions at the source, but the CRC-8
    // init value used by the verification code (0x03) was copied from the
    // single-bus capture_read() path, which is DEAD CODE — never exercised
    // by this board (daq_board.c only ever calls adaq_stream_comb_start) and
    // therefore never bench-verified against real hardware. Two real framing
    // bugs in the verification code were found and fixed on the bench
    // (frame length ignoring the CRC byte; CRC offset using the per-sample
    // status THROTTLE instead of the persistent status config — see
    // .mex/patterns/daq-scope-refactor-brief.md rounds 11-12), but even
    // after both fixes current still read as dense noise, unresponsive to a
    // forced range. That means the init constant itself is unverified and
    // possibly wrong for continuous-read frames (register-access CRC uses a
    // DIFFERENT init, 0x00, per adaq_ll_write_reg — transaction types are
    // not guaranteed to share one). Do not re-enable without confirming the
    // continuous-read CRC init against the ADAQ7769 datasheet; the
    // one-sample isolated-outlier despike in fast_emit() (glitch_isolated)
    // is the verified glitch defense in the meantime.
    // Bus A group: ADAQ #0 alone.
    adaq7769_t *grp_a[1] = { &b->adaq[0] };
    esp_err_t err = adaq_stream_init(&b->stream_a, grp_a, 1, ring_capacity,
                                     /*status=*/true, /*crc=*/false);
    if (err != ESP_OK) return err;

    // Bus B group: ADAQ #1 + #2.
    adaq7769_t *grp_b[2] = { &b->adaq[1], &b->adaq[2] };
    err = adaq_stream_init(&b->stream_b, grp_b, 2, ring_capacity,
                           /*status=*/true, /*crc=*/false);
    if (err != ESP_OK) {
        adaq_stream_deinit(&b->stream_a);
        return err;
    }

    // ONE capture task services BOTH buses (all 3 devices). Two per-bus tasks
    // pinned to the same core time-slice and, at high ODR, drop samples on the
    // off-slice (DRDY notifications coalesce). A single task reads the two
    // independent SPI hosts back-to-back with no time-slicing. Each stream
    // keeps its own ring, so the fusion consumer is unchanged. Pinned to core 1
    // (dedicated acquisition core); core 0 runs USB/DSP/links/UI.
    adaq_stream_t *streams[2] = { &b->stream_a, &b->stream_b };
    err = adaq_stream_comb_start(&b->capture, streams, 2, /*core=*/1, /*prio=*/20);
    return err;
}

esp_err_t daq_board_stop_streaming(daq_board_t *b)
{
    adaq_stream_comb_stop(&b->capture);
    adaq_stream_deinit(&b->stream_a);
    adaq_stream_deinit(&b->stream_b);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// DRDY-gated fast path: drain the per-bus rings, pair FINE+COARSE, run the
// fusion -> power DSP -> multires -> spectrum -> USB pipeline at full ODR.
// -----------------------------------------------------------------------------

// VOLTAGE is the 2nd device in the bus-B capture group {COARSE, VOLTAGE}.
#define FASTB_COARSE_LOCAL   0
#define FASTB_VOLTAGE_LOCAL  1

static inline bool fast_sample_good(const adaq_sample_t *s)
{
    return (s->flags & (ADAQ_SAMPLE_FLAG_STATUS_ERR |
                        ADAQ_SAMPLE_FLAG_CRC_ERR)) == 0;
}

// Open-circuit baseline (raw ADC_DATA) for a current range at the live V_DUT
// code, subtracted in software from each sample. Returns 0 when no baseline is
// stored for that range/code. HI and MID use the FINE ADC but have distinct
// baselines, so the subtraction is keyed on the live (autoranged) range.
static inline int32_t base_offset_adc(daq_board_t *b, uint8_t range)
{
    int32_t off = 0;
    smu_base_offset(&b->cal, range, b->smu.v_code, &off);
    return off;
}

// Fuse one (optional) FINE + (optional) COARSE pair and push it downstream.
// ---------------------------------------------------------------------------
// Isolated-outlier despike (glitch eradication). The capture engine misses a
// small fraction of edges under SCLK overlap (faststat "missed" counter) and
// the adjacent conversions read as corrupted codes of ARBITRARY value with
// clean flags — visible as random full-scale single-sample spikes on the
// host, polluting the stream, DSP, and stats alike. A corrupted conversion is
// identifiable without thresholds tuned per signal: a sample far from BOTH
// neighbors while the neighbors agree with each other cannot be signal at
// this ODR. Real steps/edges keep neighbors disagreeing and pass untouched.
// Costs a one-sample delay on each stream.
// ---------------------------------------------------------------------------
static inline bool glitch_isolated(float prev, float x, float next, float eps)
{
    float dp = fabsf(x - prev);
    float dn = fabsf(x - next);
    float pn = fabsf(prev - next);
    return dp > 4.0f * (pn + eps) && dn > 4.0f * (pn + eps);
}

typedef struct {
    float           prev;      // x[n-2], post-correction
    fusion_output_t hold;      // x[n-1], pending emission
    bool            hold_settling;
    uint8_t         n;
} i_despike_t;

typedef struct {
    float   prev;              // x[n-2], post-correction
    float   hold;              // x[n-1], pending emission
    uint8_t n;
} v_despike_t;

static i_despike_t s_i_glitch;
static v_despike_t s_v_glitch;

static void glitch_filter_reset(void)
{
    s_i_glitch.n = 0;
    s_v_glitch.n = 0;
}

static void fast_emit(daq_board_t *b, const adaq_sample_t *fine,
                      const adaq_sample_t *coarse)
{
    // range_manager_step() processes any pending ISR flags from the FF GPIOs
    // (range-up immediate, range-down deferred with lock + confirmation) and
    // drives the bypass GPIOs when warranted.  It must be called every sample
    // so the lock/confirm counters advance correctly even when one of the two
    // ADC streams is absent.
    fusion_input_t in = {
        .range        = range_manager_step(&b->range),
        .fine_valid   = false,
        .coarse_valid = false,
        .fine_v       = 0.0f,
        .coarse_v     = 0.0f,
    };
    if (fine && b->adaq_ok[ADAQ_ROLE_FINE]) {
        int32_t fv = fine->value;
        if (in.range == RANGE_HI || in.range == RANGE_MID)
            fv -= base_offset_adc(b, (uint8_t)in.range);
        in.fine_v     = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_FINE], fv);
        in.fine_valid = fast_sample_good(fine);
    }
    if (coarse && b->adaq_ok[ADAQ_ROLE_COARSE]) {
        int32_t cv = coarse->value - base_offset_adc(b, (uint8_t)RANGE_LO);
        in.coarse_v     = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_COARSE], cv);
        in.coarse_valid = fast_sample_good(coarse);
    }

    fusion_output_t fo;
    current_fusion_step(&b->fusion, &in, &fo);

    // One-sample-delayed despike (see glitch_isolated above): every consumer
    // below — power, DSP tail, and the wire push — sees the corrected stream.
    bool settling_now = range_manager_settling(&b->range);
    fusion_output_t emit_fo;
    bool emit_settling;
    if (s_i_glitch.n == 0) {
        s_i_glitch.hold = fo;
        s_i_glitch.hold_settling = settling_now;
        s_i_glitch.n = 1;
        return;
    }
    emit_fo = s_i_glitch.hold;
    emit_settling = s_i_glitch.hold_settling;
    if (s_i_glitch.n >= 2 &&
        glitch_isolated(s_i_glitch.prev, emit_fo.amps, fo.amps, 1e-6f)) {
        emit_fo.amps = 0.5f * (s_i_glitch.prev + fo.amps);
    }
    s_i_glitch.prev = emit_fo.amps;
    s_i_glitch.hold = fo;
    s_i_glitch.hold_settling = settling_now;
    if (s_i_glitch.n < 2) s_i_glitch.n = 2;

    // Instantaneous power for the full-rate PC stream: p = v_held * i. Computed
    // inline (cheap) so the heavy power DSP need not run every sample. v is the
    // held voltage from the slower VOLTAGE ADC.
    float v = power_dsp_voltage(&b->dsp);
    float p = v * emit_fo.amps;

    // DSP TAIL (energy/charge/stats + multires + spectrum) runs DECIMATED: it
    // doesn't need the full per-channel rate, and decimating frees core-0 budget
    // so the full-rate fused stream to the PC keeps flowing. power_dsp's dt is
    // set to the decimated period in daq_board_run_fast so energy/charge stay
    // correct. The PC still gets every fused sample below.
    if (++b->dsp_count >= b->dsp_decim) {
        b->dsp_count = 0;
        power_dsp_push_current(&b->dsp, emit_fo.amps);
        multires_push(&b->multires, emit_fo.amps);
        spectrum_push(&b->spectrum,
                      (b->fft_source == 1) ? power_dsp_last_p(&b->dsp) : emit_fo.amps);
    }

    // Full-rate fused waveform to the PC (decimated only by wave_decim, def 1).
    // fine_rate_hz is cached once in daq_board_run_fast (constant while the
    // fast path runs) to avoid a per-sample adaq7769_output_data_rate() call
    // at up to 256 ksps.
    if (++b->wave_count >= b->wave_decim) {
        b->wave_count = 0;
        usb_stream_push_sample(&b->usb, &emit_fo, b->fine_rate_hz, b->wave_decim,
                               emit_settling);
    }
}

static void daq_fast_task(void *arg)
{
    daq_board_t *b = (daq_board_t *)arg;
    const bool fine_ok   = b->adaq_ok[ADAQ_ROLE_FINE];
    const bool coarse_ok = b->adaq_ok[ADAQ_ROLE_COARSE];

    // 10 Hz STATS/ENERGY/FFT/STATUS summary, driven off the emit count so all
    // USB writes stay on this single task (no cross-task contention on b->usb).
    float rate0 = fine_ok ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE])
                          : 256000.0f;
    uint32_t summary_interval = (uint32_t)(rate0 / 10.0f);
    if (summary_interval == 0) summary_interval = 1;
    uint32_t summary_count = 0;

    adaq_sample_t fine = {0}, coarse = {0}, sb = {0};
    bool have_fine = false, have_coarse = false, have_offset = false;
    int64_t seq_offset = 0;   // (coarse.seq - fine.seq) learned at first pairing

    uint32_t yield_ctr = 0;
    while (b->fast_running) {
        bool progress = false;

        // Refill a FINE sample (bus A).
        if (fine_ok && !have_fine) {
            have_fine = (adaq_stream_read(&b->stream_a, &fine, 1) == 1);
            if (have_fine) progress = true;
        }

        // Drain bus B: route VOLTAGE straight to the DSP, hold the next COARSE.
        while (!have_coarse && adaq_stream_read(&b->stream_b, &sb, 1) == 1) {
            progress = true;
            if (sb.device_id == FASTB_VOLTAGE_LOCAL) {
                // sb.value != 0: cheap prefilter for one common corruption
                // pattern (all-zero conversion with clean flags); the
                // one-sample despike below catches arbitrary-value glitches.
                if (b->adaq_ok[ADAQ_ROLE_VOLTAGE] && fast_sample_good(&sb) &&
                    sb.value != 0) {
                    float vv = adaq7769_code_to_volts(
                                   &b->adaq[ADAQ_ROLE_VOLTAGE], sb.value)
                               * V_DUT_SENSE_SCALE;
                    // One-sample-delayed isolated-outlier despike (see
                    // glitch_isolated) -- corrupted conversions of arbitrary
                    // value never reach the DSP or the wire.
                    float emit_v = 0.0f;
                    bool  have_v = false;
                    if (s_v_glitch.n == 0) {
                        s_v_glitch.hold = vv;
                        s_v_glitch.n = 1;
                    } else {
                        emit_v = s_v_glitch.hold;
                        if (s_v_glitch.n >= 2 &&
                            glitch_isolated(s_v_glitch.prev, emit_v, vv, 0.01f)) {
                            emit_v = 0.5f * (s_v_glitch.prev + vv);
                        }
                        s_v_glitch.prev = emit_v;
                        s_v_glitch.hold = vv;
                        if (s_v_glitch.n < 2) s_v_glitch.n = 2;
                        have_v = true;
                    }
                    if (have_v) {
                        power_dsp_set_voltage(&b->dsp, emit_v);
                        // SET_RATE stream decimation. The wave header carries
                        // the *effective* rate so hosts that ignore the
                        // header's decimation byte still get a correct
                        // timebase.
                        if (++b->volt_count >= b->volt_decim) {
                            b->volt_count = 0;
                            uint32_t vdec = b->volt_decim ? b->volt_decim : 1;
                            usb_stream_push_voltage(&b->usb, emit_v,
                                                    b->volt_rate_hz / vdec);
                        }
                    }
                }
            } else {
                coarse = sb;
                have_coarse = true;
            }
        }

        bool emitted = false;
        if (fine_ok && coarse_ok) {
            if (have_fine && have_coarse) {
                if (!have_offset) {
                    seq_offset  = (int64_t)coarse.seq - (int64_t)fine.seq;
                    have_offset = true;
                }
                int64_t fadj = (int64_t)fine.seq + seq_offset;
                int64_t cadj = (int64_t)coarse.seq;
                if (fadj == cadj) {
                    fast_emit(b, &fine, &coarse);
                    have_fine = have_coarse = false;
                    emitted = true;
                } else if (fadj < cadj) {
                    // FINE seq fell behind COARSE.  If the gap is large it
                    // means a FINE ring overflow caused a seq jump — resync
                    // immediately instead of draining thousands of COARSE
                    // samples (which would starve the FINE ring further).
                    if ((cadj - fadj) > 128) {
                        have_offset = false;
                        seq_offset  = 0;
                    }
                    have_fine = false;
                    b->drop_fine++;
                } else {
                    // COARSE seq fell behind FINE — same overflow guard.
                    if ((fadj - cadj) > 128) {
                        have_offset = false;
                        seq_offset  = 0;
                    }
                    have_coarse = false;
                    b->drop_coarse++;
                }
                progress = true;
            }
        } else if (fine_ok && have_fine) {
            fast_emit(b, &fine, NULL);   // COARSE absent -> FINE only
            have_fine = false;
            emitted = progress = true;
        } else if (coarse_ok && have_coarse) {
            fast_emit(b, NULL, &coarse); // FINE absent -> COARSE only
            have_coarse = false;
            emitted = progress = true;
        }

        if (emitted && ++summary_count >= summary_interval) {
            summary_count = 0;
            daq_board_stream_summary(b);
        }

        if (!progress) {
            vTaskDelay(1);   // rings drained; yield ~1 tick
        } else if ((++yield_ctr & 0x3FFu) == 0) {
            // At sustained high ODR, progress is true on almost every
            // iteration, so the branch above rarely fires. This task (prio
            // 12) then never blocks, so core 0's IDLE0 task never runs to
            // reset its own watchdog entry -> TWDT reset ("IDLE0 did not
            // reset in time"), and daq_ctrl_task/TinyUSB also get starved
            // (looks like the device is unresponsive to commands). Force a
            // short real block periodically so core 0 always has scheduling
            // gaps regardless of how busy the rings are.
            vTaskDelay(1);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t daq_board_run_fast(daq_board_t *b, size_t ring_capacity)
{
    if (b->fast_running) return ESP_OK;

    // If no ADAQ front-end responded (e.g. analog rails not yet enabled, or the
    // ADCs are unpopulated/held in reset), do NOT start the acquisition path.
    // The prio-20 DRDY capture tasks would otherwise spin on floating DRDY lines
    // (spurious edge-ISR flood) and starve IDLE0 -> task watchdog reset every
    // ~5 s, which also tears down the USB-HS stream before it can enumerate.
    // Stay alive in link-only mode: USB (PID 0x4001), S3 and C6 links keep
    // running so the board is reachable for bring-up and rail control.
    int adaq_ok_count = b->adaq_ok[0] + b->adaq_ok[1] + b->adaq_ok[2];
    if (adaq_ok_count == 0) {
        ESP_LOGW(TAG, "no ADAQ front-end detected (0/%d) — acquisition idle; "
                      "USB/S3/C6 links stay up. Enable analog rails and reset "
                      "to bring up the ADCs.", ADAQ_COUNT);
        return ESP_OK;
    }

    // Set fast_running BEFORE bringing up the capture task. The capture task
    // holds the SPI bus with an explicit spi_device_acquire_bus() for the whole
    // session; other tasks (main housekeeping diagnostics_push, TUI status poll)
    // do ADAQ register access guarded on !fast_running. If the flag were set
    // only AFTER start_streaming, a concurrent register poll could fire while
    // the capture task already owns the bus -> spi_device_polling_end assert
    // (handle != acquiring_dev). Setting it first locks those pollers out.
    b->fast_running = true;

    esp_err_t err = daq_board_start_streaming(b, ring_capacity);
    if (err != ESP_OK) {
        b->fast_running = false;
        return err;
    }

    if (b->wave_decim == 0) b->wave_decim = WAVE_STREAM_DECIM_DEFAULT;
    b->wave_count   = 0;
    if (b->volt_decim == 0) b->volt_decim = 1;
    b->volt_count   = 0;
    glitch_filter_reset();
    b->volt_rate_hz = (uint32_t)(b->adaq_ok[ADAQ_ROLE_VOLTAGE]
                          ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_VOLTAGE])
                          : 0);
    if (b->dsp_decim == 0) b->dsp_decim = DAQ_DSP_DECIM_DEFAULT;
    b->dsp_count    = 0;
    // Cache the FINE ODR once for fast_emit's per-sample wire push — it's
    // constant while the fast path runs (measurable saving at 256 ksps).
    b->fine_rate_hz = (uint32_t)(b->adaq_ok[ADAQ_ROLE_FINE]
                          ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE])
                          : 256000.0f);
    // The DSP tail runs every dsp_decim-th fused sample, so set the power-DSP
    // timebase to the decimated rate — otherwise energy/charge integrate with
    // the wrong dt (off by dsp_decim).
    {
        float odr = b->adaq_ok[ADAQ_ROLE_FINE]
                        ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE])
                        : 256000.0f;
        power_dsp_set_rate(&b->dsp, odr / (float)b->dsp_decim);
    }
    b->drop_fine    = 0;
    b->drop_coarse  = 0;

    // Pin the processor to core 0 (alongside FINE capture). It runs below the
    // prio-20 capture tasks, so DRDY reads always pre-empt the pipeline.
    BaseType_t ok = xTaskCreatePinnedToCore(daq_fast_task, "daq_fast", 8192, b,
                                            /*prio=*/12, &b->fast_task,
                                            /*core=*/0);
    if (ok != pdPASS) {
        b->fast_running = false;
        daq_board_stop_streaming(b);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "fast path running (ring=%u, wave_decim=%u)",
             (unsigned)ring_capacity, b->wave_decim);
    return ESP_OK;
}

esp_err_t daq_board_stop_fast(daq_board_t *b)
{
    if (!b->fast_running) {
        // Not running: just make sure any partial capture state is torn down.
        return daq_board_stop_streaming(b);
    }

    // Order matters to avoid an SPI bus-ownership assert. The capture task holds
    // the bus via spi_device_acquire_bus(); main/TUI register pollers guard on
    // !fast_running.
    //   1. Stop the capture task and release the bus WHILE fast_running is still
    //      set, so those pollers stay locked out during teardown.
    adaq_stream_comb_stop(&b->capture);
    //   2. The bus is now free — clear the flag (register access is safe again)
    //      and let the processor task, which only touches the sample rings (not
    //      SPI), observe it and self-delete.
    b->fast_running = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    b->fast_task = NULL;
    //   3. Free the rings only AFTER the processor has stopped reading them
    //      (adaq_stream_read is not NULL-safe against a freed ring).
    adaq_stream_deinit(&b->stream_a);
    adaq_stream_deinit(&b->stream_b);
    return ESP_OK;
}

