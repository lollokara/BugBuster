// =============================================================================
// c6_flasher.c — ESP32-C6 firmware flashing from the P4 (esp-serial-flasher).
// =============================================================================

#include "c6_flasher.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_loader.h"
#include "esp32_port.h"

static const char *TAG = "c6_flash";

// Bootloader data block. Incoming S3 chunks (<=236 B) are coalesced into these.
#define C6_FLASH_BLOCK 1024u

static esp_loader_t          s_loader;
static esp32_port_t          s_port;
static esp_loader_flash_cfg_t s_cfg;

static uint8_t  s_buf[C6_FLASH_BLOCK];
static size_t   s_fill;
static uint32_t s_received;
static bool     s_active;

static void port_deinit(void)
{
    // Release UART2 (esp32_uart_ops.deinit calls uart_driver_delete) so the
    // ddp_master can reclaim it.
    if (s_port.port.ops && s_port.port.ops->deinit) {
        s_port.port.ops->deinit(&s_port.port);
    }
    s_active = false;
}

esp_err_t c6_flasher_begin(uint32_t image_size)
{
    if (s_active) port_deinit();
    s_fill = 0;
    s_received = 0;

    s_port = (esp32_port_t){
        .port.ops    = &esp32_uart_ops,
        .baud_rate   = 115200,            // universally-safe ROM baud
        .uart_port   = DAQ_UART_PORT,
        .uart_rx_pin = (gpio_num_t)DAQ_UART_RX_PIN,
        .uart_tx_pin = (gpio_num_t)DAQ_UART_TX_PIN,
        .reset_pin   = C6_RST_PIN,
        .boot_pin    = C6_BOOT_PIN,
    };

    if (esp_loader_init_serial(&s_loader, &s_port.port) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "serial init failed");
        return ESP_FAIL;
    }
    s_active = true;

    esp_loader_connect_args_t ca = ESP_LOADER_CONNECT_DEFAULT();
    if (esp_loader_connect_with_stub(&s_loader, &ca) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "C6 bootloader connect failed");
        port_deinit();
        return ESP_FAIL;
    }

    s_cfg = (esp_loader_flash_cfg_t){
        .offset      = 0,
        .image_size  = image_size,
        .block_size  = C6_FLASH_BLOCK,
        .skip_verify = false,
    };
    if (esp_loader_flash_start(&s_loader, &s_cfg) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "flash_start failed (size=%u)", (unsigned)image_size);
        port_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "C6 flashing started (%u bytes)", (unsigned)image_size);
    return ESP_OK;
}

esp_err_t c6_flasher_write(const uint8_t *data, size_t len)
{
    if (!s_active) return ESP_ERR_INVALID_STATE;
    while (len) {
        size_t take = C6_FLASH_BLOCK - s_fill;
        if (take > len) take = len;
        memcpy(s_buf + s_fill, data, take);
        s_fill += take;
        data   += take;
        len    -= take;
        if (s_fill == C6_FLASH_BLOCK) {
            if (esp_loader_flash_write(&s_loader, &s_cfg, s_buf, C6_FLASH_BLOCK) != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "flash_write failed at %u", (unsigned)s_received);
                return ESP_FAIL;
            }
            s_received += C6_FLASH_BLOCK;
            s_fill = 0;
        }
    }
    return ESP_OK;
}

esp_err_t c6_flasher_finish(void)
{
    if (!s_active) return ESP_ERR_INVALID_STATE;
    esp_err_t rc = ESP_OK;

    if (s_fill) {
        if (esp_loader_flash_write(&s_loader, &s_cfg, s_buf, (uint32_t)s_fill) != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "final flash_write failed");
            rc = ESP_FAIL;
        } else {
            s_received += s_fill;
        }
        s_fill = 0;
    }

    if (rc == ESP_OK && esp_loader_flash_finish(&s_loader, &s_cfg) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "flash_finish/verify failed");
        rc = ESP_FAIL;
    }

    esp_loader_reset_target(&s_loader);   // boot the freshly-flashed image
    port_deinit();
    ESP_LOGI(TAG, "C6 flashing %s (%u bytes)", rc == ESP_OK ? "done" : "FAILED",
             (unsigned)s_received);
    return rc;
}

void c6_flasher_abort(void)
{
    if (!s_active) return;
    esp_loader_reset_target(&s_loader);   // return the C6 to normal boot
    port_deinit();
    s_fill = 0;
}

uint32_t c6_flasher_received(void)
{
    return s_received;
}
