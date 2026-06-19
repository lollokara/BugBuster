// =============================================================================
// daq_board.c — BugBuster DAQ HAT (ESP32-P4) board integration
// =============================================================================

#include "daq_board.h"
#include <string.h>
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "config.h"
#include "version.h"
#include "ota.h"
#include "usb_backend.h"

static const char *TAG = "daq_board";

// Largest single SPI DMA transfer we expect (40-bit sample = 5 bytes); keep a
// comfortable margin for future block transfers.
#define DAQ_SPI_MAX_XFER   64

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
    // Wide-swing 50 mohm path -> IN3_AAF (gain 0.143, +/-28.7 V span).
    adaq7769_params_t p1 = {
        .host           = ADAQ_BUSB_HOST,
        .cs_pin         = ADAQ1_CS_PIN,
        .reset_pin      = ADAQ1_RESET_PIN,
        .drdy_pin       = ADAQ1_DRDY_PIN,
        .aaf_input      = ADAQ_AAF_IN3,
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

    // --- Fusion + power DSP (driven by the FINE current ODR) ---
    float current_odr = b->adaq_ok[ADAQ_ROLE_FINE]
                            ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE])
                            : 256000.0f;
    // Lean on COARSE for ~1 filter settle after a range switch, then short blend.
    // 64-sample blackout / 16-sample cross-fade are conservative defaults; tune
    // against the ADAQ settle tables for the active filter/ODR.
    current_fusion_init(&b->fusion, &b->range, /*settle=*/64, /*blend=*/16);
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

// Control commands from the PC. Bound to b->usb via usb_stream_set_cmd_cb.
static void usb_cmd_handler(usb_rec_type_t cmd, const uint8_t *payload,
                            uint16_t len, void *user)
{
    daq_board_t *b = (daq_board_t *)user;
    switch (cmd) {
        case USB_CMD_START:
            usb_stream_set_streaming(&b->usb, true);
            break;
        case USB_CMD_STOP:
            usb_stream_set_streaming(&b->usb, false);
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
            if (len >= sizeof(usb_cmd_source_t)) {
                const usb_cmd_source_t *c = (const usb_cmd_source_t *)payload;
                daq_board_set_source(b, c->vdut, c->ilimit, c->enable != 0);
            }
            break;
        default:
            // SET_RATE / FFT_CONFIG / SET_SOURCE handled in later phases.
            break;
    }
}

esp_err_t daq_board_usb_start(daq_board_t *b)
{
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
                           rate, /*decimation=*/1);
    if (out) {
        *out = fo;
    }
    return ESP_OK;
}

esp_err_t daq_board_stream_summary(daq_board_t *b)
{
    usb_stream_flush_waveform(&b->usb);
    usb_stream_send_stats(&b->usb, &b->dsp);
    usb_stream_send_energy(&b->usb, &b->dsp);

    // Continuous spectrum: send the latest averaged magnitude bins.
    static float mags[SPECTRUM_MAX_BINS];
    uint16_t nb = spectrum_get_magnitude(&b->spectrum, mags, SPECTRUM_MAX_BINS);
    if (nb > 0) {
        uint32_t rate = (uint32_t)adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE]);
        usb_stream_send_fft(&b->usb, mags, nb, rate, b->fft_source,
                            (uint8_t)SPEC_WIN_HANN);
    }

    // Device status (range, streaming, SMU set-points).
    usb_status_payload_t st = {
        .sample_rate    = (uint32_t)adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE]),
        .overflow_count = 0,
        .range          = (uint8_t)range_manager_current(&b->range),
        .streaming      = 1,
        .range_locked   = b->range.override_active ? 1 : 0,
        .source_enabled = b->smu.enabled ? 1 : 0,
        .vdut_set       = b->smu.vdut_set,
        .ilimit_set     = b->smu.ilimit_set,
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
    return smu_enable(&b->smu, enable);
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
        case HATP_CMD_OTA_BEGIN: {
            if (len < sizeof(ota_meta_t)) return -1;
            ota_meta_t meta;
            memcpy(&meta, payload, sizeof(meta));
            return (ota_begin(&meta) == ESP_OK) ? 0 : -1;
        }
        case HATP_CMD_OTA_DATA: {
            if (len < sizeof(s3link_ota_data_hdr_t)) return -1;
            uint32_t offset;
            memcpy(&offset, payload, sizeof(offset));
            const uint8_t *fw = payload + sizeof(s3link_ota_data_hdr_t);
            uint8_t fw_len = (uint8_t)(len - sizeof(s3link_ota_data_hdr_t));
            return (ota_write(offset, fw, fw_len) == ESP_OK) ? 0 : -1;
        }
        case HATP_CMD_OTA_END:
            return (ota_end() == ESP_OK) ? 0 : -1;
        case HATP_CMD_OTA_ABORT:
            ota_abort();
            return 0;
        case HATP_CMD_OTA_STATUS: {
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
            return (ota_confirm() == ESP_OK) ? 0 : -1;
        case HATP_CMD_OTA_ROLLBACK:
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
    return s3_link_start(&b->s3, /*core=*/0, /*prio=*/10);
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

    // Pin the two capture tasks to different cores to spread the load.
    err = adaq_stream_start(&b->stream_a, /*core=*/0, /*prio=*/20);
    if (err != ESP_OK) return err;
    err = adaq_stream_start(&b->stream_b, /*core=*/1, /*prio=*/20);
    return err;
}

esp_err_t daq_board_stop_streaming(daq_board_t *b)
{
    adaq_stream_stop(&b->stream_a);
    adaq_stream_stop(&b->stream_b);
    adaq_stream_deinit(&b->stream_a);
    adaq_stream_deinit(&b->stream_b);
    return ESP_OK;
}
