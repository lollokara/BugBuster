#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "daq_board.h"
#include "daq_settings_glue.h"
#include "diagnostics.h"

static const char *TAG = "daq_hat_p4";

static daq_board_t s_board;

void app_main(void)
{
    ESP_LOGI(TAG, "DAQ HAT P4 firmware starting...");

    esp_err_t err = daq_board_init(&s_board);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    // Bind the authoritative settings store to the subsystems and apply every
    // persisted/default value (range, SMU, FFT, decimation). Must run after
    // daq_board_init() (subsystems exist) and before the fast path starts.
    daq_board_bind_settings(&s_board);

    // Bring up the USB-HS measurement stream to the PC.
    if (daq_board_usb_start(&s_board) == ESP_OK) {
        ESP_LOGI(TAG, "USB stream ready");
    }

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

    // Start the DRDY-gated fast acquisition path: per-bus capture tasks plus the
    // pairing/fusion/DSP/spectrum/stream processor. The PSRAM ring gives tens of
    // ms of slack so FFT spikes and USB back-pressure never drop samples.
    esp_err_t ferr = daq_board_run_fast(&s_board, 8192);
    if (ferr != ESP_OK) {
        ESP_LOGE(TAG, "fast path start failed: %s", esp_err_to_name(ferr));
    } else {
        ESP_LOGI(TAG, "fast acquisition running");
    }

    // Low-rate housekeeping only: the fast task owns the acquisition + USB
    // pipeline. Here we log a heartbeat and push the full onboard-device
    // diagnostics snapshot to the C6 Diagnostics menu (~1 Hz).
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "I=%.6g A  V=%.4f V  P=%.6g W  E=%.4f mWh  Q=%.4f mAh  "
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
