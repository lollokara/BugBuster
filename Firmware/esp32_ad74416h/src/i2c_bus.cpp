// =============================================================================
// i2c_bus.cpp - Shared I2C bus driver for BugBuster (legacy I2C driver)
// =============================================================================

#include "i2c_bus.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";

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

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = PIN_I2C_SDA;
    conf.scl_io_num = PIN_I2C_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_DISABLE;  // External pull-ups
    conf.scl_pullup_en = GPIO_PULLUP_DISABLE;
    conf.master.clk_speed = I2C_FREQ_HZ;

    esp_err_t err = i2c_param_config(I2C_PORT_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_driver_install(I2C_PORT_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
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

bool i2c_bus_write(uint8_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(timeout_ms));
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "I2C write 0x%02X failed: %s", addr, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool i2c_bus_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                         uint8_t *rd_data, size_t rd_len, uint32_t timeout_ms)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    // Write phase
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, wr_data, wr_len, true);
    // Repeated start + read phase
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    if (rd_len > 1) {
        i2c_master_read(cmd, rd_data, rd_len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, rd_data + rd_len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(timeout_ms));
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "I2C write_read 0x%02X failed: %s", addr, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool i2c_bus_read(uint8_t addr, uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(timeout_ms));
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "I2C read 0x%02X failed: %s", addr, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool i2c_bus_probe(uint8_t addr)
{
    if (!s_initialized) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(s_mutex);
    return err == ESP_OK;
}
