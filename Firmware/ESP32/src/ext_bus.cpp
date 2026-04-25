// =============================================================================
// ext_bus.cpp - External target bus engine for routed IO pins
// =============================================================================

#include "ext_bus.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "ext_bus";

static constexpr i2c_port_t EXT_I2C_PORT = I2C_NUM_1;
static constexpr uint32_t EXT_I2C_MIN_HZ = 10'000;
static constexpr uint32_t EXT_I2C_MAX_HZ = 1'000'000;
static constexpr spi_host_device_t EXT_SPI_HOST = SPI3_HOST;
static constexpr uint32_t EXT_SPI_MIN_HZ = 10'000;
static constexpr uint32_t EXT_SPI_MAX_HZ = 20'000'000;
static constexpr size_t EXT_SPI_MAX_TRANSFER = 512;

static SemaphoreHandle_t s_i2c_mutex = nullptr;
static bool s_i2c_ready = false;
static uint8_t s_sda_gpio = 0xFF;
static uint8_t s_scl_gpio = 0xFF;
static uint32_t s_frequency_hz = 0;
static bool s_i2c_internal_pullups = false;

static SemaphoreHandle_t s_spi_mutex = nullptr;
static bool s_spi_bus_ready = false;
static spi_device_handle_t s_spi_dev = nullptr;
static uint8_t s_spi_tx[EXT_SPI_MAX_TRANSFER] = {};
static uint8_t s_spi_rx[EXT_SPI_MAX_TRANSFER] = {};
static uint8_t s_spi_sck_gpio = 0xFF;
static uint8_t s_spi_mosi_gpio = 0xFF;
static uint8_t s_spi_miso_gpio = 0xFF;
static uint8_t s_spi_cs_gpio = 0xFF;
static uint32_t s_spi_frequency_hz = 0;
static uint8_t s_spi_mode = 0;

static constexpr size_t EXT_JOB_CAPACITY = 16;
static constexpr size_t EXT_JOB_MAX_BYTES = 512;

struct ExtBusJob {
    uint32_t id;
    uint8_t kind;
    uint8_t status;
    uint8_t addr;
    uint16_t timeout_ms;
    size_t tx_len;
    size_t rx_len;
    uint8_t *tx;
    uint8_t *rx;
};

static SemaphoreHandle_t s_job_mutex = nullptr;
static TaskHandle_t s_job_task = nullptr;
static ExtBusJob s_jobs[EXT_JOB_CAPACITY] = {};
static uint32_t s_next_job_id = 1;

static bool valid_gpio(uint8_t gpio)
{
    return GPIO_IS_VALID_GPIO((gpio_num_t)gpio);
}

static bool reserved_i2c_addr(uint8_t addr)
{
    return addr <= 0x07 || addr >= 0x78;
}

static TickType_t ticks(uint16_t timeout_ms)
{
    return pdMS_TO_TICKS(timeout_ms ? timeout_ms : 100);
}

bool ext_i2c_setup(uint8_t sda_gpio, uint8_t scl_gpio, uint32_t frequency_hz, bool internal_pullups)
{
    if (!valid_gpio(sda_gpio) || !valid_gpio(scl_gpio) || sda_gpio == scl_gpio) {
        ESP_LOGE(TAG, "Invalid external I2C pins SDA=%u SCL=%u", sda_gpio, scl_gpio);
        return false;
    }
    if (frequency_hz < EXT_I2C_MIN_HZ || frequency_hz > EXT_I2C_MAX_HZ) {
        ESP_LOGE(TAG, "Invalid external I2C frequency %lu", (unsigned long)frequency_hz);
        return false;
    }

    if (s_i2c_mutex == nullptr) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (s_i2c_mutex == nullptr) {
            ESP_LOGE(TAG, "Failed to create external I2C mutex");
            return false;
        }
    }

    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }

    if (s_i2c_ready) {
        i2c_driver_delete(EXT_I2C_PORT);
        s_i2c_ready = false;
    }

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = (gpio_num_t)sda_gpio;
    conf.scl_io_num = (gpio_num_t)scl_gpio;
    conf.sda_pullup_en = internal_pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    conf.scl_pullup_en = internal_pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    conf.master.clk_speed = frequency_hz;

    esp_err_t err = i2c_param_config(EXT_I2C_PORT, &conf);
    if (err == ESP_OK) {
        err = i2c_driver_install(EXT_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    }

    if (err == ESP_OK) {
        s_i2c_ready = true;
        s_sda_gpio = sda_gpio;
        s_scl_gpio = scl_gpio;
        s_frequency_hz = frequency_hz;
        s_i2c_internal_pullups = internal_pullups;
        ESP_LOGI(TAG, "External I2C ready SDA=GPIO%u SCL=GPIO%u freq=%lu pullups=%s",
                 sda_gpio, scl_gpio, (unsigned long)frequency_hz,
                 internal_pullups ? "internal" : "external/off");
    } else {
        ESP_LOGE(TAG, "External I2C setup failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_i2c_mutex);
    return err == ESP_OK;
}

bool ext_i2c_ready(void)
{
    return s_i2c_ready;
}

void ext_i2c_get_status(bool *ready, uint8_t *sda_gpio, uint8_t *scl_gpio,
                        uint32_t *frequency_hz, bool *internal_pullups)
{
    if (ready) *ready = s_i2c_ready;
    if (sda_gpio) *sda_gpio = s_sda_gpio;
    if (scl_gpio) *scl_gpio = s_scl_gpio;
    if (frequency_hz) *frequency_hz = s_frequency_hz;
    if (internal_pullups) *internal_pullups = s_i2c_internal_pullups;
}

static bool ext_i2c_probe_locked(uint8_t addr, uint16_t timeout_ms)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(EXT_I2C_PORT, cmd, ticks(timeout_ms));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

bool ext_i2c_scan(uint8_t start_addr, uint8_t stop_addr, bool skip_reserved,
                  uint8_t *out_addrs, size_t max_addrs, size_t *out_count,
                  uint16_t timeout_ms)
{
    if (!s_i2c_ready || out_addrs == nullptr || out_count == nullptr || start_addr > stop_addr || stop_addr > 0x7F) {
        return false;
    }
    if (xSemaphoreTake(s_i2c_mutex, ticks(timeout_ms)) != pdTRUE) {
        return false;
    }

    size_t count = 0;
    for (uint8_t addr = start_addr; addr <= stop_addr; addr++) {
        if (skip_reserved && reserved_i2c_addr(addr)) {
            continue;
        }
        if (ext_i2c_probe_locked(addr, timeout_ms) && count < max_addrs) {
            out_addrs[count++] = addr;
        }
        if (addr == 0x7F) {
            break;
        }
    }

    *out_count = count;
    xSemaphoreGive(s_i2c_mutex);
    return true;
}

bool ext_i2c_write(uint8_t addr, const uint8_t *data, size_t len, uint16_t timeout_ms)
{
    if (!s_i2c_ready || (len > 0 && data == nullptr) || len > 255) {
        return false;
    }
    if (xSemaphoreTake(s_i2c_mutex, ticks(timeout_ms)) != pdTRUE) {
        return false;
    }
    esp_err_t err = i2c_master_write_to_device(EXT_I2C_PORT, addr, data, len, ticks(timeout_ms));
    xSemaphoreGive(s_i2c_mutex);
    return err == ESP_OK;
}

bool ext_i2c_read(uint8_t addr, uint8_t *data, size_t len, uint16_t timeout_ms)
{
    if (!s_i2c_ready || data == nullptr || len == 0 || len > 255) {
        return false;
    }
    if (xSemaphoreTake(s_i2c_mutex, ticks(timeout_ms)) != pdTRUE) {
        return false;
    }
    esp_err_t err = i2c_master_read_from_device(EXT_I2C_PORT, addr, data, len, ticks(timeout_ms));
    xSemaphoreGive(s_i2c_mutex);
    return err == ESP_OK;
}

bool ext_i2c_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                        uint8_t *rd_data, size_t rd_len, uint16_t timeout_ms)
{
    if (!s_i2c_ready || wr_data == nullptr || rd_data == nullptr ||
        wr_len == 0 || rd_len == 0 || wr_len > 255 || rd_len > 255) {
        return false;
    }
    if (xSemaphoreTake(s_i2c_mutex, ticks(timeout_ms)) != pdTRUE) {
        return false;
    }
    esp_err_t err = i2c_master_write_read_device(
        EXT_I2C_PORT, addr, wr_data, wr_len, rd_data, rd_len, ticks(timeout_ms));
    xSemaphoreGive(s_i2c_mutex);
    return err == ESP_OK;
}

static bool valid_optional_gpio(uint8_t gpio)
{
    return gpio == 0xFF || valid_gpio(gpio);
}

bool ext_spi_setup(uint8_t sck_gpio, uint8_t mosi_gpio, uint8_t miso_gpio, uint8_t cs_gpio,
                   uint32_t frequency_hz, uint8_t mode)
{
    if (!valid_gpio(sck_gpio) || !valid_optional_gpio(mosi_gpio) ||
        !valid_optional_gpio(miso_gpio) || !valid_optional_gpio(cs_gpio) ||
        mode > 3 || frequency_hz < EXT_SPI_MIN_HZ || frequency_hz > EXT_SPI_MAX_HZ) {
        ESP_LOGE(TAG, "Invalid external SPI setup sck=%u mosi=%u miso=%u cs=%u freq=%lu mode=%u",
                 sck_gpio, mosi_gpio, miso_gpio, cs_gpio, (unsigned long)frequency_hz, mode);
        return false;
    }

    if (s_spi_mutex == nullptr) {
        s_spi_mutex = xSemaphoreCreateMutex();
        if (s_spi_mutex == nullptr) {
            ESP_LOGE(TAG, "Failed to create external SPI mutex");
            return false;
        }
    }
    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }

    if (s_spi_dev != nullptr) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = nullptr;
    }
    if (s_spi_bus_ready) {
        spi_bus_free(EXT_SPI_HOST);
        s_spi_bus_ready = false;
    }

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = (mosi_gpio == 0xFF) ? -1 : (int)mosi_gpio;
    bus_cfg.miso_io_num = (miso_gpio == 0xFF) ? -1 : (int)miso_gpio;
    bus_cfg.sclk_io_num = (int)sck_gpio;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = EXT_SPI_MAX_TRANSFER;

    esp_err_t err = spi_bus_initialize(EXT_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (err == ESP_OK) {
        s_spi_bus_ready = true;
        spi_device_interface_config_t dev_cfg = {};
        dev_cfg.clock_speed_hz = (int)frequency_hz;
        dev_cfg.mode = mode;
        dev_cfg.spics_io_num = (cs_gpio == 0xFF) ? -1 : (int)cs_gpio;
        dev_cfg.queue_size = 1;
        err = spi_bus_add_device(EXT_SPI_HOST, &dev_cfg, &s_spi_dev);
    }

    if (err == ESP_OK) {
        s_spi_sck_gpio = sck_gpio;
        s_spi_mosi_gpio = mosi_gpio;
        s_spi_miso_gpio = miso_gpio;
        s_spi_cs_gpio = cs_gpio;
        s_spi_frequency_hz = frequency_hz;
        s_spi_mode = mode;
        ESP_LOGI(TAG, "External SPI ready SCK=GPIO%u MOSI=%d MISO=%d CS=%d freq=%lu mode=%u",
                 sck_gpio,
                 mosi_gpio == 0xFF ? -1 : (int)mosi_gpio,
                 miso_gpio == 0xFF ? -1 : (int)miso_gpio,
                 cs_gpio == 0xFF ? -1 : (int)cs_gpio,
                 (unsigned long)frequency_hz,
                 mode);
    } else {
        if (s_spi_bus_ready) {
            spi_bus_free(EXT_SPI_HOST);
            s_spi_bus_ready = false;
        }
        ESP_LOGE(TAG, "External SPI setup failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_spi_mutex);
    return err == ESP_OK;
}

bool ext_spi_ready(void)
{
    return s_spi_dev != nullptr;
}

void ext_spi_get_status(bool *ready, uint8_t *sck_gpio, uint8_t *mosi_gpio,
                        uint8_t *miso_gpio, uint8_t *cs_gpio,
                        uint32_t *frequency_hz, uint8_t *mode)
{
    if (ready) *ready = s_spi_dev != nullptr;
    if (sck_gpio) *sck_gpio = s_spi_sck_gpio;
    if (mosi_gpio) *mosi_gpio = s_spi_mosi_gpio;
    if (miso_gpio) *miso_gpio = s_spi_miso_gpio;
    if (cs_gpio) *cs_gpio = s_spi_cs_gpio;
    if (frequency_hz) *frequency_hz = s_spi_frequency_hz;
    if (mode) *mode = s_spi_mode;
}

bool ext_spi_transfer(const uint8_t *tx_data, size_t tx_len,
                      uint8_t *rx_data, size_t *inout_rx_len,
                      uint16_t timeout_ms)
{
    if (s_spi_dev == nullptr || inout_rx_len == nullptr || rx_data == nullptr ||
        tx_len > EXT_SPI_MAX_TRANSFER || *inout_rx_len > EXT_SPI_MAX_TRANSFER) {
        return false;
    }
    size_t transfer_len = tx_len > *inout_rx_len ? tx_len : *inout_rx_len;
    if (transfer_len == 0 || transfer_len > EXT_SPI_MAX_TRANSFER) {
        return false;
    }
    if (tx_len > 0 && tx_data == nullptr) {
        return false;
    }
    if (xSemaphoreTake(s_spi_mutex, ticks(timeout_ms)) != pdTRUE) {
        return false;
    }

    memset(s_spi_tx, 0, transfer_len);
    memset(s_spi_rx, 0, transfer_len);
    if (tx_len > 0) {
        memcpy(s_spi_tx, tx_data, tx_len);
    }

    spi_transaction_t txn = {};
    txn.length = transfer_len * 8;
    txn.tx_buffer = s_spi_tx;
    txn.rx_buffer = s_spi_rx;
    esp_err_t err = spi_device_polling_transmit(s_spi_dev, &txn);
    if (err == ESP_OK) {
        memcpy(rx_data, s_spi_rx, transfer_len);
        *inout_rx_len = transfer_len;
    }

    xSemaphoreGive(s_spi_mutex);
    return err == ESP_OK;
}

static uint8_t *job_alloc(size_t len)
{
    if (len == 0) return nullptr;
    uint8_t *ptr = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = (uint8_t *)malloc(len);
    return ptr;
}

static void job_free_buffers(ExtBusJob &job)
{
    if (job.tx) {
        free(job.tx);
        job.tx = nullptr;
    }
    if (job.rx) {
        free(job.rx);
        job.rx = nullptr;
    }
    job.tx_len = 0;
    job.rx_len = 0;
}

static void ext_job_worker(void *)
{
    while (true) {
        int idx = -1;
        if (xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
                if (s_jobs[i].status == EXT_BUS_JOB_QUEUED) {
                    s_jobs[i].status = EXT_BUS_JOB_RUNNING;
                    idx = (int)i;
                    break;
                }
            }
            xSemaphoreGive(s_job_mutex);
        }

        if (idx < 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        ExtBusJob *job = &s_jobs[idx];
        bool ok = false;
        if (job->kind == EXT_BUS_JOB_I2C_READ) {
            ok = ext_i2c_read(job->addr, job->rx, job->rx_len, job->timeout_ms);
        } else if (job->kind == EXT_BUS_JOB_I2C_WRITE_READ) {
            ok = ext_i2c_write_read(job->addr, job->tx, job->tx_len, job->rx, job->rx_len, job->timeout_ms);
        } else if (job->kind == EXT_BUS_JOB_SPI_TRANSFER) {
            size_t rx_len = job->rx_len;
            ok = ext_spi_transfer(job->tx, job->tx_len, job->rx, &rx_len, job->timeout_ms);
            job->rx_len = rx_len;
        }

        if (xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            job->status = ok ? EXT_BUS_JOB_DONE : EXT_BUS_JOB_ERROR;
            xSemaphoreGive(s_job_mutex);
        }
    }
}

static bool ensure_job_runtime(void)
{
    if (s_job_mutex == nullptr) {
        s_job_mutex = xSemaphoreCreateMutex();
        if (s_job_mutex == nullptr) return false;
    }
    if (s_job_task == nullptr) {
        BaseType_t ok = xTaskCreatePinnedToCore(ext_job_worker, "extJob", 4096, nullptr, 5, &s_job_task, 1);
        if (ok != pdPASS) {
            s_job_task = nullptr;
            return false;
        }
    }
    return true;
}

static bool submit_job(uint8_t kind, uint8_t addr, const uint8_t *tx, size_t tx_len,
                       size_t rx_len, uint16_t timeout_ms, uint32_t *job_id)
{
    if (!job_id || rx_len > EXT_JOB_MAX_BYTES || tx_len > EXT_JOB_MAX_BYTES ||
        (tx_len > 0 && tx == nullptr) || !ensure_job_runtime()) {
        return false;
    }

    uint8_t *tx_copy = nullptr;
    uint8_t *rx_buf = nullptr;
    if (tx_len > 0) {
        tx_copy = job_alloc(tx_len);
        if (!tx_copy) return false;
        memcpy(tx_copy, tx, tx_len);
    }
    if (rx_len > 0) {
        rx_buf = job_alloc(rx_len);
        if (!rx_buf) {
            if (tx_copy) free(tx_copy);
            return false;
        }
        memset(rx_buf, 0, rx_len);
    }

    if (xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (tx_copy) free(tx_copy);
        if (rx_buf) free(rx_buf);
        return false;
    }

    int slot = -1;
    for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
        if (s_jobs[i].status == EXT_BUS_JOB_EMPTY ||
            s_jobs[i].status == EXT_BUS_JOB_DONE ||
            s_jobs[i].status == EXT_BUS_JOB_ERROR) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        xSemaphoreGive(s_job_mutex);
        if (tx_copy) free(tx_copy);
        if (rx_buf) free(rx_buf);
        return false;
    }

    ExtBusJob &job = s_jobs[slot];
    job_free_buffers(job);
    job.id = s_next_job_id++;
    if (s_next_job_id == 0) s_next_job_id = 1;
    job.kind = kind;
    job.status = EXT_BUS_JOB_QUEUED;
    job.addr = addr;
    job.timeout_ms = timeout_ms;
    job.tx = tx_copy;
    job.rx = rx_buf;
    job.tx_len = tx_len;
    job.rx_len = rx_len;
    *job_id = job.id;

    xSemaphoreGive(s_job_mutex);
    return true;
}

bool ext_job_submit_i2c_read(uint8_t addr, uint8_t read_len, uint16_t timeout_ms, uint32_t *job_id)
{
    if (read_len == 0) return false;
    return submit_job(EXT_BUS_JOB_I2C_READ, addr, nullptr, 0, read_len, timeout_ms, job_id);
}

bool ext_job_submit_i2c_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                                   uint8_t read_len, uint16_t timeout_ms, uint32_t *job_id)
{
    if (wr_len == 0 || read_len == 0) return false;
    return submit_job(EXT_BUS_JOB_I2C_WRITE_READ, addr, wr_data, wr_len, read_len, timeout_ms, job_id);
}

bool ext_job_submit_spi_transfer(const uint8_t *tx_data, size_t tx_len,
                                 uint16_t timeout_ms, uint32_t *job_id)
{
    if (tx_len == 0) return false;
    return submit_job(EXT_BUS_JOB_SPI_TRANSFER, 0, tx_data, tx_len, tx_len, timeout_ms, job_id);
}

bool ext_job_get(uint32_t job_id, uint8_t *status, uint8_t *kind,
                 uint8_t *result, size_t max_result_len, size_t *result_len)
{
    if (!status || !kind || !result_len || !ensure_job_runtime()) return false;
    if (xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
        ExtBusJob &job = s_jobs[i];
        if (job.id == job_id && job.status != EXT_BUS_JOB_EMPTY) {
            *status = job.status;
            *kind = job.kind;
            *result_len = 0;
            if (job.status == EXT_BUS_JOB_DONE && job.rx && result && max_result_len > 0) {
                size_t n = job.rx_len < max_result_len ? job.rx_len : max_result_len;
                memcpy(result, job.rx, n);
                *result_len = n;
            }
            xSemaphoreGive(s_job_mutex);
            return true;
        }
    }
    xSemaphoreGive(s_job_mutex);
    return false;
}
