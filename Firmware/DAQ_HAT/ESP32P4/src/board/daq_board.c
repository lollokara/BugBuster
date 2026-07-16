// =============================================================================
// daq_board.c — BugBuster DAQ HAT (ESP32-P4) board integration
// =============================================================================

#include "daq_board.h"
#include <string.h>
#include "freertos/queue.h"
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
                // not covered by BBP and requires no SPI bus access.
                if (c->decimation >= 1) {
                    b->wave_decim = c->decimation;
                    b->wave_count = 0;
                }
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
            usb_stream_set_streaming(&b->usb, true);
            ESP_LOGI(TAG, "CMD_START: streaming on (mounted=%d, fast=%d)",
                     usb_backend_mounted(), b->fast_running);
            break;
        case USB_CMD_STOP:
            usb_stream_set_streaming(&b->usb, false);
            ESP_LOGI(TAG, "CMD_STOP: streaming off");
            usb_stream_flush_waveform(&b->usb);
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
        usb_stream_set_streaming(&b->usb, true);
        ESP_LOGI(TAG, "USB measurement stream started");
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
    usb_stream_push_sample(&b->usb, &fo,
                           power_dsp_last_v(&b->dsp), p,
                           rate, /*decimation=*/1,
                           range_manager_settling(&b->range));
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
    usb_stream_flush_waveform(&b->usb);
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
    };
    usb_stream_send_status(&b->usb, &st);
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

// Active OTA target (set on OTA_BEGIN): HATP_OTA_TARGET_P4 / _C6.
static uint8_t  s_ota_target = HATP_OTA_TARGET_P4;
static uint32_t s_c6_ota_size = 0;

// Bring the C6 display link (DDP master) back up after a C6 flash hands UART2
// back. ddp_master_deinit() released the driver; init+start re-acquire it.
static void c6_link_restart(daq_board_t *b)
{
    ddp_master_init(&b->ddp);
    b->ddp.cal = &b->cal;   // rebind after init memset (CAL_CTRL handling)
    ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
}

static int s3_cmd_handler(uint8_t cmd, const uint8_t *payload, uint8_t len,
                          uint8_t *resp, void *user)
{
    daq_board_t *b = (daq_board_t *)user;
    switch (cmd) {
        case HATP_CMD_DAQ_START:
            usb_stream_set_streaming(&b->usb, true);
            return 0;

        case HATP_CMD_DAQ_STOP:
            usb_stream_set_streaming(&b->usb, false);
            usb_stream_flush_waveform(&b->usb);
            return 0;

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
                                       0xFFFFFFFFu);
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
            return (ota_write(offset, fw, fw_len) == ESP_OK) ? 0 : -1;
        }
        case HATP_CMD_OTA_END:
            if (s_ota_target == HATP_OTA_TARGET_C6) {
                esp_err_t rc = c6_flasher_finish();
                c6_link_restart(b);                  // C6 now runs the new image
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

// UI task: relay front-panel buttons to the C6 and push the latest measurement
// for the on-screen readout. Low priority, ~20 ms cadence.
static void daq_ui_task(void *arg)
{
    daq_board_t *b = (daq_board_t *)arg;
    uint32_t last_meas = 0;
    uint32_t last_hello = 0;
    for (;;) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);

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

    // Instantaneous power for the full-rate PC stream: p = v_held * i. Computed
    // inline (cheap) so the heavy power DSP need not run every sample. v is the
    // held voltage from the slower VOLTAGE ADC.
    float v = power_dsp_voltage(&b->dsp);
    float p = v * fo.amps;

    // DSP TAIL (energy/charge/stats + multires + spectrum) runs DECIMATED: it
    // doesn't need the full per-channel rate, and decimating frees core-0 budget
    // so the full-rate fused stream to the PC keeps flowing. power_dsp's dt is
    // set to the decimated period in daq_board_run_fast so energy/charge stay
    // correct. The PC still gets every fused sample below.
    if (++b->dsp_count >= b->dsp_decim) {
        b->dsp_count = 0;
        power_dsp_push_current(&b->dsp, fo.amps);
        multires_push(&b->multires, fo.amps);
        spectrum_push(&b->spectrum,
                      (b->fft_source == 1) ? power_dsp_last_p(&b->dsp) : fo.amps);
    }

    // Full-rate fused waveform to the PC (decimated only by wave_decim, def 1).
    if (++b->wave_count >= b->wave_decim) {
        b->wave_count = 0;
        uint32_t rate = (uint32_t)adaq7769_output_data_rate(
                            &b->adaq[ADAQ_ROLE_FINE]);
        usb_stream_push_sample(&b->usb, &fo, v, p, rate, b->wave_decim,
                               range_manager_settling(&b->range));
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
                if (b->adaq_ok[ADAQ_ROLE_VOLTAGE] && fast_sample_good(&sb)) {
                    float vv = adaq7769_code_to_volts(
                                   &b->adaq[ADAQ_ROLE_VOLTAGE], sb.value);
                    power_dsp_set_voltage(&b->dsp, vv * V_DUT_SENSE_SCALE);
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
    if (b->dsp_decim == 0) b->dsp_decim = DAQ_DSP_DECIM_DEFAULT;
    b->dsp_count    = 0;
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

