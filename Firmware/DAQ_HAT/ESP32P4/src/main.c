#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"

#include "daq_board.h"
#include "daq_settings_glue.h"
#include "diagnostics.h"
#include "cli.h"
#include "usb_backend.h"

static const char *TAG = "daq_hat_p4";

static daq_board_t s_board;

void app_main(void)
{
    // LDO channel 4 (VO4) supplies GPIO47/48 via VDD_IO_5. Must be set to 3.3V
    // before any GPIO on that domain is driven; default after reset is ~1.2V.
    static esp_ldo_channel_handle_t s_ldo4_handle;
    esp_ldo_channel_config_t ldo4_cfg = {
        .chan_id    = 4,
        .voltage_mv = 3300,
    };
    if (esp_ldo_acquire_channel(&ldo4_cfg, &s_ldo4_handle) != ESP_OK) {
        ESP_LOGE(TAG, "LDO_VO4 init failed — GPIO47/48 domain may be at wrong voltage");
    }

    ESP_LOGI(TAG, "DAQ HAT P4 firmware starting...");

    esp_err_t err = daq_board_init(&s_board);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    // Bind the authoritative settings store to the subsystems and apply every
    // persisted/default value (range, SMU, FFT, decimation). Must run after
    // daq_board_init() (subsystems exist) and before the fast path starts.
    daq_board_bind_settings(&s_board);

    // Bring up the inter-processor links (S3 mainboard + C6 display) BEFORE the
    // USB-HS stream. These carry the control plane and the DDP diagnostics the
    // C6 uses to leave "simulation mode", and they must never be gated by the
    // USB-HS bring-up: on this board the USB-HS PHY (dedicated USB_DM/USB_DP,
    // J5) can be actively driven/enumerated by the host while VBUS state is not
    // monitored (see daq_board_usb_start), and if that bring-up stalls it must
    // not prevent the S3/C6 links — and therefore live telemetry — from coming
    // up. Ordering: S3 -> C6 -> (diagnostics) -> USB. See PowerAnalyzer §1.

    // Bring up the ESP32-S3 mainboard link (HAT-protocol slave). The S3 detects
    // the HAT, reads HAT_TYPE_DAQ_POWER via GET_INFO, and loads DAQ resources.
    if (daq_board_s3_start(&s_board) == ESP_OK) {
        ESP_LOGI(TAG, "S3 link ready");
    }

    // Bring up the ESP32-C6 display link (DDP master) + front-panel buttons:
    // relays button presses to the C6 menu and pushes live measurements.
    if (daq_board_c6_start(&s_board) == ESP_OK) {
        ESP_LOGI(TAG, "C6 display link ready");
    }

    // Internal die-temperature sensor for the Diagnostics menu. Optional — a
    // failure here only blanks the "ESP32-P4 Temp" row.
    if (diagnostics_init() != ESP_OK) {
        ESP_LOGW(TAG, "internal temperature sensor unavailable");
    }

    // Bring up the USB-HS measurement stream to the PC. Done AFTER the S3/C6
    // links so a USB-HS enumeration issue can never keep the P4 from pushing
    // DDP telemetry (which would leave the C6 stuck showing "simulation").
    // The acquisition path below tolerates the stream not being mounted yet
    // (frames are dropped until a host attaches — see usb_stream emit_frame).
    if (daq_board_usb_start(&s_board) == ESP_OK) {
        ESP_LOGI(TAG, "USB stream ready");
    }

    // DRDY-gated fast acquisition path (per-bus capture + fusion/DSP/USB stream).
    // Auto-starts at the default 8 kSPS/channel ODR, which is the measured
    // end-to-end lossless ceiling (raise at runtime with `odr`; toggle with
    // `fast on|off`). Higher ODRs are available but currently overflow the
    // FINE/COARSE fusion consumer — see the capture/fusion fast-path work.
    esp_err_t ferr = daq_board_run_fast(&s_board, 8192);
    if (ferr != ESP_OK) {
        ESP_LOGE(TAG, "fast path start failed: %s", esp_err_to_name(ferr));
    } else {
        ESP_LOGI(TAG, "fast acquisition running (8 kSPS/ch, use 'fast'/'odr' to change)");
    }

    // Interactive bring-up console on the USB-Serial-JTAG debug port (J1).
    // Type 'help' for commands (status/read/adaq/temp/rail/vdut/ilimit).
    if (daq_cli_start(&s_board) != ESP_OK) {
        ESP_LOGW(TAG, "bring-up console unavailable");
    }

    // Low-rate housekeeping only: the fast task owns the acquisition + USB
    // pipeline. Here we log a heartbeat and push the full onboard-device
    // diagnostics snapshot to the C6 Diagnostics menu (~1 Hz).
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGD(TAG, "I=%.6g A  V=%.4f V  P=%.6g W  E=%.4f mWh  Q=%.4f mAh  "
                      "drop F/C=%u/%u",
                 power_dsp_last_i(&s_board.dsp),
                 power_dsp_last_v(&s_board.dsp),
                 power_dsp_last_p(&s_board.dsp),
                 power_dsp_energy_mwh(&s_board.dsp),
                 power_dsp_charge_mah(&s_board.dsp),
                 (unsigned)s_board.drop_fine,
                 (unsigned)s_board.drop_coarse);
        diagnostics_push(&s_board);
    }
}
