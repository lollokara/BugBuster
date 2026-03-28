// =============================================================================
// i2c_bus.cpp - Shared I2C bus driver for BugBuster (ESP-IDF new I2C driver)
// =============================================================================

#include "i2c_bus.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus_handle = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;

bool i2c_bus_init(void)
{
    if (s_initialized) return true;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create I2C mutex");
        return false;
    }

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_PORT_NUM;
    bus_config.sda_io_num = PIN_I2C_SDA;
    bus_config.scl_io_num = PIN_I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d SCL=%d freq=%dHz)",
             PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    return true;
}

bool i2c_bus_ready(void)
{
    return s_initialized;
}

// Helper: get or create a device handle for a given address
// The new I2C driver requires adding devices to the bus
static i2c_master_dev_handle_t get_dev_handle(uint8_t addr)
{
    // We maintain a small cache of device handles
    // Max 8 devices on one bus is plenty
    static struct { uint8_t addr; i2c_master_dev_handle_t handle; } s_devs[8];
    static int s_dev_count = 0;

    for (int i = 0; i < s_dev_count; i++) {
        if (s_devs[i].addr == addr) return s_devs[i].handle;
    }

    if (s_dev_count >= 8) {
        ESP_LOGE(TAG, "Too many I2C devices");
        return NULL;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = I2C_FREQ_HZ;

    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device 0x%02X: %s", addr, esp_err_to_name(err));
        return NULL;
    }

    s_devs[s_dev_count].addr = addr;
    s_devs[s_dev_count].handle = dev_handle;
    s_dev_count++;

    return dev_handle;
}

bool i2c_bus_write(uint8_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    i2c_master_dev_handle_t dev = get_dev_handle(addr);
    bool ok = false;
    if (dev) {
        esp_err_t err = i2c_master_transmit(dev, data, len, timeout_ms);
        ok = (err == ESP_OK);
        if (!ok) {
            ESP_LOGD(TAG, "I2C write 0x%02X failed: %s", addr, esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_mutex);
    return ok;
}

bool i2c_bus_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                         uint8_t *rd_data, size_t rd_len, uint32_t timeout_ms)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    i2c_master_dev_handle_t dev = get_dev_handle(addr);
    bool ok = false;
    if (dev) {
        esp_err_t err = i2c_master_transmit_receive(dev, wr_data, wr_len, rd_data, rd_len, timeout_ms);
        ok = (err == ESP_OK);
        if (!ok) {
            ESP_LOGD(TAG, "I2C write_read 0x%02X failed: %s", addr, esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_mutex);
    return ok;
}

bool i2c_bus_read(uint8_t addr, uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    i2c_master_dev_handle_t dev = get_dev_handle(addr);
    bool ok = false;
    if (dev) {
        esp_err_t err = i2c_master_receive(dev, data, len, timeout_ms);
        ok = (err == ESP_OK);
        if (!ok) {
            ESP_LOGD(TAG, "I2C read 0x%02X failed: %s", addr, esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_mutex);
    return ok;
}

bool i2c_bus_probe(uint8_t addr)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    esp_err_t err = i2c_master_probe(s_bus_handle, addr, 50);
    bool found = (err == ESP_OK);

    xSemaphoreGive(s_mutex);
    return found;
}
