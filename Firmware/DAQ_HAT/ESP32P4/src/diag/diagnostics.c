// =============================================================================
// diagnostics.c — onboard-device diagnostics snapshot for the C6 menu
// =============================================================================

#include "diagnostics.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/temperature_sensor.h"

#include "adaq7769_regs.h"

// ESP32-P4 internal die-temperature sensor (installed in diagnostics_init).
static temperature_sensor_handle_t s_tsens = NULL;

esp_err_t diagnostics_init(void)
{
    // Range must map to a single HW measurement range; (-10,110) spans two and
    // makes temperature_sensor_install() fail (blanking the P4 die-temp row).
    // (-10,80) covers the realistic ESP32-P4 die range.
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&tcfg, &s_tsens);
    if (err == ESP_OK) err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) s_tsens = NULL;
    return err;
}

esp_err_t diagnostics_p4_temp_celsius(float *out_c)
{
    if (!s_tsens) return ESP_ERR_INVALID_STATE;
    if (!out_c)   return ESP_ERR_INVALID_ARG;
    return temperature_sensor_get_celsius(s_tsens, out_c);
}

// ADAQ7769-1 internal die temperature via the diagnostic mux. This re-routes the
// converter and would corrupt the gapless capture, so it is ONLY read when the
// fast acquisition path is idle; while streaming it reports DDP_DIAG_TEMP_NA.
// Conversion (datasheet: 0.6 mV/C RTO, two-point linear):
//   25 C -> code 0x059FFF (368639),  75 C -> code 0x068FFF (430079).
static int16_t adaq_die_temp_c10(daq_board_t *b, int idx)
{
    if (idx < 0 || idx >= ADAQ_COUNT || !b->adaq_ok[idx] || b->fast_running)
        return DDP_DIAG_TEMP_NA;
    int32_t raw = 0;
    if (adaq7769_read_diagnostic(&b->adaq[idx], ADAQ_DIAGMUX_TEMP, &raw) != ESP_OK)
        return DDP_DIAG_TEMP_NA;
    float t = 25.0f + ((float)raw - 368639.0f) * (50.0f / 61440.0f);
    return (int16_t)lroundf(t * 10.0f);
}

void diagnostics_push(daq_board_t *b)
{
    ddp_diag_t d = {0};
    uint16_t valid = 0;

    // --- AD7415 board temperatures (U2, U28) ---
    d.t_board0_c10 = DDP_DIAG_TEMP_NA;
    d.t_board1_c10 = DDP_DIAG_TEMP_NA;
    for (int i = 0; i < 2; ++i) {
        float c = 0.0f;
        if (b->temp_ok[i] && ad741x_read_celsius(&b->temp[i], &c) == ESP_OK) {
            int16_t v = (int16_t)lroundf(c * 10.0f);
            b->t_board_c10[i] = v;          // cache for the USB STATUS frame
            if (i == 0) { d.t_board0_c10 = v; valid |= DDP_DIAG_V_BOARD0; }
            else        { d.t_board1_c10 = v; valid |= DDP_DIAG_V_BOARD1; }
        } else {
            b->t_board_c10[i] = DDP_DIAG_TEMP_NA;
        }
    }

    // --- ADAQ7769-1 die temperatures (only while acquisition is stopped) ---
    d.t_adaq0_c10 = adaq_die_temp_c10(b, 0);
    d.t_adaq1_c10 = adaq_die_temp_c10(b, 1);
    d.t_adaq2_c10 = adaq_die_temp_c10(b, 2);
    if (d.t_adaq0_c10 != DDP_DIAG_TEMP_NA) valid |= DDP_DIAG_V_ADAQ0;
    if (d.t_adaq1_c10 != DDP_DIAG_TEMP_NA) valid |= DDP_DIAG_V_ADAQ1;
    if (d.t_adaq2_c10 != DDP_DIAG_TEMP_NA) valid |= DDP_DIAG_V_ADAQ2;

    // --- ESP32-P4 internal temperature sensor ---
    d.t_p4_c10 = DDP_DIAG_TEMP_NA;
    if (s_tsens) {
        float tc = 0.0f;
        if (temperature_sensor_get_celsius(s_tsens, &tc) == ESP_OK) {
            d.t_p4_c10 = (int16_t)lroundf(tc * 10.0f);
            valid |= DDP_DIAG_V_P4TEMP;
        }
    }

    // --- Fused current / voltage / power (signed micro-units) ---
    d.i_ua = (int32_t)lroundf(power_dsp_last_i(&b->dsp) * 1e6f);
    d.v_uv = (int32_t)lroundf(power_dsp_last_v(&b->dsp) * 1e6f);
    d.p_uw = (int32_t)lroundf(power_dsp_last_p(&b->dsp) * 1e6f);
    valid |= DDP_DIAG_V_IVP;

    // --- SMU monitor currents (LTM8056 IINMON / IOUTMON) ---
    float iin = 0.0f, iout = 0.0f;
    bool smu_ok = false;
    if (smu_read_input_current(&b->smu, &iin) == ESP_OK) {
        d.smu_iin_ma = (int16_t)lroundf(iin * 1000.0f); smu_ok = true;
    }
    if (smu_read_output_current(&b->smu, &iout) == ESP_OK) {
        d.smu_iout_ma = (int16_t)lroundf(iout * 1000.0f); smu_ok = true;
    }
    if (smu_ok) valid |= DDP_DIAG_V_SMU;

    // --- V_DUT (programmed setpoint) ---
    d.vdut_mv = (uint16_t)lroundf(b->smu.vdut_set * 1000.0f);
    if (b->smu.enabled) valid |= DDP_DIAG_V_VDUT;

    // --- S3 mainboard telemetry relay (die temp, USB-PD, VADJ/VLOGIC rails) ---
    d.t_s3_c10 = DDP_DIAG_TEMP_NA;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (b->s3_telem_ms != 0 && (now_ms - b->s3_telem_ms) < 5000) {
        const s3link_telemetry_t *t = &b->s3_telem;
        valid |= DDP_DIAG_V_S3;
        if ((t->flags & S3LINK_TLM_F_DIE) && t->die_temp_c10 != S3LINK_TLM_NA)
            d.t_s3_c10 = t->die_temp_c10;
        if (t->flags & S3LINK_TLM_F_RAILS) {
            d.vadj1_mv  = t->vadj1_mv;
            d.vadj2_mv  = t->vadj2_mv;
            d.vlogic_mv = t->vlogic_mv;
        }
        if (t->flags & S3LINK_TLM_F_PD) {
            d.pd_mv = t->pd_mv;
            d.pd_ma = t->pd_ma;
            valid |= DDP_DIAG_V_S3PD;
        }
    }

    // --- P4 runtime stats ---
    uint32_t free_kb = esp_get_free_heap_size() / 1024u;
    d.p4_free_mem_kb  = (free_kb > 0xFFFFu) ? 0xFFFFu : (uint16_t)free_kb;
    UBaseType_t hw    = uxTaskGetStackHighWaterMark(NULL);  // ESP-IDF: bytes
    d.p4_free_stack_b = (hw > 0xFFFFu) ? 0xFFFFu : (uint16_t)hw;
    d.p4_tasks        = (uint8_t)uxTaskGetNumberOfTasks();
    d.p4_uptime_s     = (uint32_t)(esp_timer_get_time() / 1000000);

    d.valid = valid;
    ddp_master_set_diagnostics(&b->ddp, &d);
}
