#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "daq_board.h"

static const char *TAG = "daq_hat_p4";

static daq_board_t s_board;

void app_main(void)
{
    ESP_LOGI(TAG, "DAQ HAT P4 firmware starting...");

    esp_err_t err = daq_board_init(&s_board);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    // Bring up the USB-HS measurement stream to the PC.
    if (daq_board_usb_start(&s_board) == ESP_OK) {
        ESP_LOGI(TAG, "USB stream ready");
    }

    // Bring up the ESP32-S3 mainboard link (HAT-protocol slave). The S3 detects
    // the HAT, reads HAT_TYPE_DAQ_POWER via GET_INFO, and loads DAQ resources.
    if (daq_board_s3_start(&s_board) == ESP_OK) {
        ESP_LOGI(TAG, "S3 link ready");
    }

    // Demo processing loop: acquire -> fuse -> DSP -> stream, with a periodic
    // STATS/ENERGY summary and a console heartbeat.
    uint32_t tick = 0;
    while (1) {
        fusion_output_t fo;
        if (daq_board_stream_step(&s_board, &fo) == ESP_OK) {
            // streaming handled inside stream_step
        }

        if (++tick >= 1000) {
            tick = 0;
            daq_board_stream_summary(&s_board);
            ESP_LOGI(TAG, "I=%.6g A  V=%.4f V  P=%.6g W  E=%.4f mWh  Q=%.4f mAh",
                     power_dsp_last_i(&s_board.dsp),
                     power_dsp_last_v(&s_board.dsp),
                     power_dsp_last_p(&s_board.dsp),
                     power_dsp_energy_mwh(&s_board.dsp),
                     power_dsp_charge_mah(&s_board.dsp));
            for (int i = 0; i < 2; ++i) {
                if (s_board.temp_ok[i]) {
                    float c = 0.0f;
                    if (ad741x_read_celsius(&s_board.temp[i], &c) == ESP_OK) {
                        ESP_LOGI(TAG, "temp[%d] = %.2f C", i, c);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
