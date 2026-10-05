#include "display.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static uint16_t *s_fb = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
static volatile bool s_flush_pending = false;

// Standby: while asleep nothing may reach the panel, and the backlight level is
// only remembered. s_bl_forced_off parks the active-low pin at its idle level.
static volatile bool s_asleep = false;
static bool s_bl_forced_off = false;
static uint8_t s_bl_level = 255;

#define FB_PIXELS  (DISP_WIDTH * DISP_HEIGHT)
#define FB_BYTES   (FB_PIXELS * (int)sizeof(uint16_t))

// Backlight PWM
#define BL_TIMER    LEDC_TIMER_0
#define BL_CHANNEL  LEDC_CHANNEL_0
#define BL_MODE     LEDC_LOW_SPEED_MODE
#define BL_RES      LEDC_TIMER_8_BIT

static void backlight_channel(uint32_t duty)
{
    ledc_channel_config_t ccfg = {
        .gpio_num   = DISP_PIN_BL,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = duty,
        .hpoint     = 0,
    };
    ledc_channel_config(&ccfg);
}

static uint32_t backlight_duty(uint8_t level)
{
#if DISP_BL_ACTIVE_LOW
    // Active low: pin LOW = full brightness, so invert the duty.
    return 255u - level;
#else
    return level;
#endif
}

static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = BL_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = BL_RES,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);

    // Start at the OFF duty so the pin never pulses the light before the first
    // frame is in panel RAM (display_init parks it hard-off right after).
    backlight_channel(backlight_duty(0));
}

void display_set_backlight(uint8_t level)
{
    s_bl_level = level;
    if (s_asleep || s_bl_forced_off) return;   // remembered; display_backlight_restore() applies it
    ledc_set_duty(BL_MODE, BL_CHANNEL, backlight_duty(level));
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

bool display_backlight_hard_off(void)
{
    s_bl_forced_off = true;
    // PWM at 255/256 is NOT off. Stop the channel and park the pin at its idle level
    // (active-low: HIGH = LED off).
    return ledc_stop(BL_MODE, BL_CHANNEL, DISP_BL_ACTIVE_LOW ? 1 : 0) == ESP_OK;
}

bool display_backlight_restore(uint8_t level)
{
    s_bl_forced_off = false;
    s_bl_level = level;
    return ledc_channel_config(&(ledc_channel_config_t){
        .gpio_num   = DISP_PIN_BL,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = backlight_duty(level),
        .hpoint     = 0,
    }) == ESP_OK;                               // re-arms the channel that ledc_stop parked
}

void display_set_asleep(bool asleep)
{
    s_asleep = asleep;
}

bool display_panel_power(bool on)
{
    if (!s_panel) return false;
    return esp_lcd_panel_disp_on_off(s_panel, on) == ESP_OK;
}

// Called from the SPI ISR when a full-frame transfer completes.
static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

esp_err_t display_init(void)
{
    s_flush_done = xSemaphoreCreateBinary();

    // --- Framebuffer ---
    s_fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed (%d bytes)", FB_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, FB_BYTES);

    // --- SPI bus ---
    spi_bus_config_t buscfg = {
        .sclk_io_num     = DISP_PIN_SCLK,
        .mosi_io_num     = DISP_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = FB_BYTES + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISP_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // --- Panel IO ---
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = DISP_PIN_DC,
        .cs_gpio_num       = DISP_PIN_CS,
        .pclk_hz           = DISP_SPI_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISP_SPI_HOST,
                                             &io_cfg, &s_io));

    // --- ST7789 panel ---
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // The ER-TFTM2.25-1 glass is a windowed area of the 240x320 controller
    // RAM. Rotate to landscape in hardware and apply the RAM offset.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, DISP_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, DISP_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, DISP_MIRROR_X, DISP_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, DISP_GAP_X, DISP_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    backlight_init();
    display_backlight_hard_off();   // the light comes on only after the first frame is in RAM

    ESP_LOGI(TAG, "ST7789 landscape %dx%d ready (fb=%d bytes)", DISP_WIDTH, DISP_HEIGHT, FB_BYTES);
    return ESP_OK;
}

uint16_t *display_framebuffer(void)
{
    return s_fb;
}

bool display_flush(void)
{
    if (!s_panel || !s_fb || s_asleep) return false;
    // Single shared framebuffer: issue the transfer and block until the whole
    // frame has shifted out before returning, so the caller can't start
    // rendering the next frame over pixels that are still being sent (which
    // showed up as "half done" frames on the glass).
    s_flush_pending = true;
    const bool sent = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISP_WIDTH, DISP_HEIGHT, s_fb) == ESP_OK;
    const bool done = sent && xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200)) == pdTRUE;
    s_flush_pending = false;
    return done;
}

// Block until the most recently issued frame has fully transferred. False = it did
// not finish inside the bound (the bus may still be busy).
bool display_wait_flush(void)
{
    if (!s_flush_pending) return true;
    const bool done = xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200)) == pdTRUE;
    s_flush_pending = false;
    return done;
}

void display_flush_rect(int x, int y, int w, int h)
{
    if (!s_panel || !s_fb || s_asleep) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISP_WIDTH)  w = DISP_WIDTH - x;
    if (y + h > DISP_HEIGHT) h = DISP_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    // esp_lcd needs a contiguous buffer for the sub-rect; pack rows.
    static uint16_t *row = NULL;
    if (!row) row = heap_caps_malloc(DISP_WIDTH * sizeof(uint16_t),
                                     MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    for (int yy = 0; yy < h; yy++) {
        memcpy(row, &s_fb[(y + yy) * DISP_WIDTH + x], w * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(s_panel, x, y + yy, x + w, y + yy + 1, row);
    }
}
