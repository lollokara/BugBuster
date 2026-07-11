// =============================================================================
// c6_flasher.c — ESP32-C6 firmware flashing from the P4 (esp-serial-flasher).
// =============================================================================

#include "c6_flasher.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_loader.h"
#include "esp32_port.h"
#include "esp32_sdio_port.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "rom/gpio.h"

static const char *TAG = "c6_flash";

// Bootloader data block. Incoming S3 chunks (<=236 B) are coalesced into these.
#define C6_FLASH_BLOCK 1024u

static esp_loader_t           s_loader;
static esp32_port_t           s_port;
static esp32_sdio_port_t      s_sdio_port;
static esp_loader_flash_cfg_t s_cfg;

static uint8_t  s_buf[C6_FLASH_BLOCK];
static size_t   s_fill;
static uint32_t s_received;
static bool     s_active;

// No-op enter_bootloader used when we manually control RST+BOOT timing.
static void noop_enter_bootloader(esp_loader_port_t *p) { (void)p; }

// Helper: configure RST and BOOT as push-pull GPIO outputs via the GPIO matrix.
// esp32_port_init / esp32_sdio_port_init call gpio_reset_pin which sets the IOMUX
// to the pad's default peripheral (UART0 TX/RX on P4 GPIO43/44). On P4 these pads
// are "fast" IOMUX pads — the GPIO matrix is BYPASSED unless the IOMUX is explicitly
// switched to GPIO-matrix mode first. gpio_config() calls gpio_hal_iomux_func_sel
// but the pad-specific ROM function esp_rom_gpio_pad_select_gpio is the unconditional
// way to force GPIO-matrix mode before any further configuration.
void c6_gpio_init_output(void)
{
    // Force IOMUX to GPIO-matrix function BEFORE gpio_config so that subsequent
    // gpio_set_level calls actually drive the physical pads.
    esp_rom_gpio_pad_select_gpio(C6_RST_PIN);
    esp_rom_gpio_pad_select_gpio(C6_BOOT_PIN);
    esp_rom_gpio_pad_select_gpio(C6_BOOT_EN_PIN);

    gpio_config_t pin_cfg = {
        .pin_bit_mask = BIT64(C6_RST_PIN) | BIT64(C6_BOOT_PIN) | BIT64(C6_BOOT_EN_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pin_cfg);
    gpio_set_drive_capability((gpio_num_t)C6_RST_PIN,     GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)C6_BOOT_PIN,    GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)C6_BOOT_EN_PIN, GPIO_DRIVE_CAP_3);
    ESP_LOGI(TAG, "C6 RST/BOOT pins configured as GPIO OUTPUT (RST=GPIO%d, BOOT=GPIO%d, BOOT_EN=GPIO%d)",
             (int)C6_RST_PIN, (int)C6_BOOT_PIN, (int)C6_BOOT_EN_PIN);
}

static void port_deinit(void)
{
    if (s_port.port.ops && s_port.port.ops->deinit) {
        s_port.port.ops->deinit(&s_port.port);
    }
    if (s_sdio_port.port.ops && s_sdio_port.port.ops->deinit) {
        s_sdio_port.port.ops->deinit(&s_sdio_port.port);
        s_sdio_port.port.ops = NULL;
    }
    s_active = false;
}

esp_err_t c6_flasher_begin(uint32_t image_size, uint32_t flash_offset)
{
    if (s_active) port_deinit();
    s_fill = 0;
    s_received = 0;

    s_port = (esp32_port_t){
        .port.ops    = &esp32_uart_ops,
        .baud_rate   = 115200,
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

    // Re-apply GPIO matrix routing after esp32_port_init's gpio_reset_pin.
    // Keep BOOT_EN=HIGH (GPIO43=1) for UART/USB download mode (boot:0x5);
    // LOW would give SDIO-only download which doesn't work with UART flasher.
    c6_gpio_init_output();
    gpio_set_level((gpio_num_t)C6_BOOT_PIN,    0);  // GPIO9=0 → download mode
    gpio_set_level((gpio_num_t)C6_BOOT_EN_PIN, 1);  // GPIO8=1 → UART/USB mode

    esp_loader_connect_args_t ca = ESP_LOADER_CONNECT_DEFAULT();
    if (esp_loader_connect_with_stub(&s_loader, &ca) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "C6 bootloader connect failed");
        port_deinit();
        return ESP_FAIL;
    }

    s_cfg = (esp_loader_flash_cfg_t){
        .offset      = flash_offset,
        .image_size  = image_size,
        .block_size  = C6_FLASH_BLOCK,
        .skip_verify = false,
    };
    if (esp_loader_flash_start(&s_loader, &s_cfg) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "flash_start failed (offset=0x%x size=%u)",
                 (unsigned)flash_offset, (unsigned)image_size);
        port_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "C6 UART flashing started (%u bytes @ 0x%x)",
             (unsigned)image_size, (unsigned)flash_offset);
    return ESP_OK;
}

esp_err_t c6_flasher_begin_sdio(uint32_t image_size, uint32_t flash_offset)
{
    if (s_active) port_deinit();
    s_fill = 0;
    s_received = 0;

    // Force IOMUX to GPIO-matrix mode for ALL SDIO pins before any SDMMC init.
    // gpio_reset_pin() (called inside sdmmc_host_init_slot) sets pads back to
    // their IOMUX default peripheral, which on P4 can block the GPIO matrix
    // DATA0 routing used by CMD53. Calling esp_rom_gpio_pad_select_gpio first
    // ensures clean GPIO-matrix ownership for the SDMMC peripheral to claim.
    esp_rom_gpio_pad_select_gpio(C6_SDIO_CLK_PIN);
    esp_rom_gpio_pad_select_gpio(C6_SDIO_CMD_PIN);
    esp_rom_gpio_pad_select_gpio(C6_SDIO_DAT0_PIN);
    esp_rom_gpio_pad_select_gpio(C6_SDIO_DAT1_PIN);
    esp_rom_gpio_pad_select_gpio(C6_SDIO_DAT2_PIN);
    esp_rom_gpio_pad_select_gpio(C6_SDIO_DAT3_PIN);

    // Assert BOOT=0 (GPIO43 → C6 GPIO9 LOW = download mode) immediately and
    // keep it LOW for the entire operation.
    // BOOT_EN (GPIO44 → C6 GPIO8) must stay HIGH for SDIO download mode.
    // Configure both as outputs via GPIO matrix (see c6_gpio_init_output).
    c6_gpio_init_output();
    gpio_set_level((gpio_num_t)C6_BOOT_PIN,    0);  // GPIO44: GPIO9=0 → download mode
    gpio_set_level((gpio_num_t)C6_BOOT_EN_PIN, 1);  // GPIO43: GPIO8=1 → SDIO sub-mode
    ESP_LOGI(TAG, "C6_BOOT(GPIO%d=IO9)=LOW  C6_BOOT_EN(GPIO%d=IO8)=HIGH  → SDIO download",
             (int)C6_BOOT_PIN, (int)C6_BOOT_EN_PIN);

    memset(&s_sdio_port, 0, sizeof(s_sdio_port));
    s_sdio_port = (esp32_sdio_port_t){
        .port.ops    = &esp32_sdio_ops,
        .slot        = SDMMC_HOST_SLOT_1,
        .max_freq_khz = SDMMC_FREQ_PROBING,
        .bus_width   = SDIO_1BIT,
        .sdio_clk_pin = C6_SDIO_CLK_PIN,
        .sdio_cmd_pin = C6_SDIO_CMD_PIN,
        .sdio_d0_pin  = C6_SDIO_DAT0_PIN,
        .sdio_d1_pin  = C6_SDIO_DAT1_PIN,
        .sdio_d2_pin  = C6_SDIO_DAT2_PIN,
        .sdio_d3_pin  = C6_SDIO_DAT3_PIN,
        .reset_pin   = C6_RST_PIN,
        .boot_pin    = C6_BOOT_PIN,
    };

    if (esp_loader_init_sdio(&s_loader, &s_sdio_port.port) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "SDIO init failed");
        return ESP_FAIL;
    }
    s_active = true;

    // Re-apply GPIO matrix routing AFTER init (esp32_sdio_port_init calls
    // gpio_reset_pin which sets IOMUX back to UART0 default on P4 GPIO43/44,
    // and may clear DATA0 routing on GPIO14).
    c6_gpio_init_output();
    esp_rom_gpio_pad_select_gpio(C6_SDIO_DAT0_PIN);   // ensure DATA0 is GPIO-matrix
    gpio_set_level((gpio_num_t)C6_BOOT_PIN, 0);       // re-assert after init

    // Retry loop: RST → wait → connect.
    // BOOT stays LOW throughout (re-asserted after each failed attempt).
    // Each retry issues a clean RST via enter_bootloader so the C6 restarts
    // in SDIO download mode fresh.
    const int MAX_TRIES = 5;
    esp_loader_connect_args_t ca = {
        .sync_timeout = 500,
        .trials       = 10,
    };

    // Use a modified ops table that replaces enter_bootloader with a no-op.
    // We do our own RST+wait below before each connect attempt so the C6 has
    // 1500 ms to initialise its SDIO slave — the library's enter_bootloader
    // would do an additional RST and only wait BOOT_HOLD_TIME_MS (800 ms) which
    // is not enough for CMD53 data transfers to start working.
    esp_loader_port_ops_t sdio_ops_noebl = esp32_sdio_ops;
    sdio_ops_noebl.enter_bootloader = noop_enter_bootloader;
    s_sdio_port.port.ops = &sdio_ops_noebl;

    esp_loader_error_t conn_err = ESP_LOADER_ERROR_FAIL;
    for (int attempt = 0; attempt < MAX_TRIES; attempt++) {
        // Manual RST: BOOT=0 (already asserted) → RST LOW → 100ms → RST HIGH
        // → wait 1500ms for C6 SDIO ROM slave to fully initialise.
        ESP_LOGI(TAG, "SDIO attempt %d/%d: RST pulse + 1500 ms wait",
                 attempt + 1, MAX_TRIES);
        gpio_set_level((gpio_num_t)C6_BOOT_PIN,    0);  // GPIO9=0 → download mode
        gpio_set_level((gpio_num_t)C6_BOOT_EN_PIN, 1);  // GPIO8=1 → SDIO sub-mode
        gpio_set_level((gpio_num_t)C6_RST_PIN,  0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level((gpio_num_t)C6_RST_PIN,  1);
        vTaskDelay(pdMS_TO_TICKS(1500));

        conn_err = esp_loader_connect(&s_loader, &ca);
        if (conn_err == ESP_LOADER_SUCCESS) break;
        ESP_LOGW(TAG, "SDIO attempt %d failed", attempt + 1);
    }

    // Restore the real ops in case other code uses the loader handle later.
    s_sdio_port.port.ops = &esp32_sdio_ops;

    if (conn_err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "C6 SDIO bootloader connect failed after %d attempts", MAX_TRIES);
        port_deinit();
        return ESP_FAIL;
    }

    // Log connected chip type.
    {
        static const char *const chip_names[] = {
            "ESP8266","ESP32","ESP32-S2","ESP32-C3","ESP32-S3",
            "ESP32-C2","ESP32-C5","ESP32-H2","ESP32-C6","ESP32-P4","ESP32-C61",
        };
        target_chip_t tgt = esp_loader_get_target(&s_loader);
        const char *name = (tgt < (int)(sizeof(chip_names)/sizeof(*chip_names)))
                           ? chip_names[tgt] : "UNKNOWN";
        ESP_LOGI(TAG, "Connected — chip: %s (id=%d)", name, (int)tgt);
    }

    s_cfg = (esp_loader_flash_cfg_t){
        .offset      = flash_offset,
        .image_size  = image_size,
        .block_size  = C6_FLASH_BLOCK,
        .skip_verify = false,
    };
    if (esp_loader_flash_start(&s_loader, &s_cfg) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "flash_start (SDIO) failed (offset=0x%x size=%u)",
                 (unsigned)flash_offset, (unsigned)image_size);
        port_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "C6 SDIO flashing started (%u bytes @ 0x%x)",
             (unsigned)image_size, (unsigned)flash_offset);
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
