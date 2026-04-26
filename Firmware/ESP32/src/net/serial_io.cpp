// =============================================================================
// serial_io.cpp - Serial I/O via TinyUSB CDC #0 (CLI console)
// =============================================================================

#include "serial_io.h"
#include "usb_cdc.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "serial_io";
static SemaphoreHandle_t s_serial_write_mutex = NULL;

void serial_init(void)
{
    // USB CDC is already initialized by usb_cdc_init() called earlier in main
    if (s_serial_write_mutex == NULL) {
        s_serial_write_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "Serial I/O ready (TinyUSB CDC #0)");
}

int serial_available(void)
{
    return (int)usb_cdc_cli_available();
}

int serial_read(void)
{
    uint8_t ch;
    uint32_t n = usb_cdc_cli_read(&ch, 1);
    return (n > 0) ? (int)ch : -1;
}

void serial_print(const char* s)
{
    if (!s) return;
    if (s_serial_write_mutex == NULL) s_serial_write_mutex = xSemaphoreCreateMutex();
    if (s_serial_write_mutex) xSemaphoreTake(s_serial_write_mutex, portMAX_DELAY);
    usb_cdc_cli_write((const uint8_t*)s, strlen(s));
    if (s_serial_write_mutex) xSemaphoreGive(s_serial_write_mutex);
}

void serial_println(const char* s)
{
    if (s_serial_write_mutex == NULL) s_serial_write_mutex = xSemaphoreCreateMutex();
    if (s_serial_write_mutex) xSemaphoreTake(s_serial_write_mutex, portMAX_DELAY);
    if (s) usb_cdc_cli_write((const uint8_t*)s, strlen(s));
    usb_cdc_cli_write((const uint8_t*)"\r\n", 2);
    if (s_serial_write_mutex) xSemaphoreGive(s_serial_write_mutex);
}

void serial_printf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if ((size_t)len > sizeof(buf)) len = (int)sizeof(buf);
        if (s_serial_write_mutex == NULL) s_serial_write_mutex = xSemaphoreCreateMutex();
        if (s_serial_write_mutex) xSemaphoreTake(s_serial_write_mutex, portMAX_DELAY);
        usb_cdc_cli_write((const uint8_t*)buf, (size_t)len);
        if (s_serial_write_mutex) xSemaphoreGive(s_serial_write_mutex);
    }
}
