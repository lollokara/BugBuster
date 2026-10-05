// =============================================================================
// standby_analog.c - analog supply standby bound to the DAQ board hardware.
// Logic: standby_rails_core.c (sequence), standby_adaq.c (register shadow).
// =============================================================================

#include "standby_analog.h"
#include "standby_rails_core.h"
#include "standby_adaq.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_sig_map.h"
#include "esp_private/spi_common_internal.h"

#include "config.h"
#include "daq_board.h"
#include "range_manager.h"
#include "adaq7769_ll.h"

static const char *TAG = "standby_analog";

static sb_rails_t        s_rails;
static sb_adaq_shadow_t  s_shadow[ADAQ_COUNT];
static range_standby_t   s_range;
static bool              s_saved;
static volatile bool     s_adc_unavailable;
static bool            (*s_cancel)(void);

// Every pad of the two analog multiplexers and the bypass switches. They are read
// back (input enabled on top of the output driver) to PROVE a disconnect.
static const gpio_num_t k_route_pins[] = {
    MUX_EN_PIN, VOLT_MUX_A0_PIN, VOLT_MUX_A1_PIN, FINE_MUX_A0_PIN, FINE_MUX_A1_PIN,
    RANGE_BYPASS51_PIN, RANGE_BYPASS2_PIN,
};

// Lines the P4 drives into the ADAQ digital supply (VDDIO is the analog 3V3), and
// lines the unpowered parts drive back. All must be quiet while it is off.
static const gpio_num_t k_drive_pins[] = {
    ADAQ_BUSA_SCLK_PIN, ADAQ_BUSA_MOSI_PIN, ADAQ_BUSB_SCLK_PIN, ADAQ_BUSB_MOSI_PIN,
    ADAQ0_CS_PIN, ADAQ1_CS_PIN, ADAQ2_CS_PIN,
};
static const gpio_num_t k_sense_pins[] = {
    ADAQ_BUSA_MISO_PIN, ADAQ_BUSB_MISO_PIN, ADAQ0_DRDY_PIN, ADAQ1_DRDY_PIN, ADAQ2_DRDY_PIN,
};

void standby_analog_init(void)
{
    sb_rails_init(&s_rails, true);
    s_saved = false;
    s_adc_unavailable = false;
}

bool standby_analog_adc_available(void) { return !s_adc_unavailable; }
bool standby_analog_routes_held(void) { return s_rails.mux_open; }
bool standby_analog_measurement_available(void) { return !s_adc_unavailable && !s_rails.mux_open; }
uint32_t standby_analog_fail_mask(void) { return s_rails.fail_mask; }
void standby_analog_set_cancel(bool (*fn)(void)) { s_cancel = fn; }

// ---------------------------------------------------------------------------
// ops
// ---------------------------------------------------------------------------
typedef struct { daq_board_t *b; } hw_t;

static void routes_readback_on(void)
{
    // An output-only pad does not read its own level. Adding the input driver does
    // not touch the output latch, so no pad moves.
    for (size_t i = 0; i < sizeof(k_route_pins) / sizeof(k_route_pins[0]); ++i) {
        gpio_set_direction(k_route_pins[i], GPIO_MODE_INPUT_OUTPUT);
    }
}

static bool op_mux_disconnect(void *ctx)
{
    (void)ctx;
    routes_readback_on();
    gpio_set_level(MUX_EN_PIN, 0);               // U24 + U25: analog path open first
    gpio_set_level(VOLT_MUX_A0_PIN, 0);
    gpio_set_level(VOLT_MUX_A1_PIN, 0);
    return true;
}

static bool op_mux_verify(void *ctx)
{
    (void)ctx;
    bool ok = true;
    for (size_t i = 0; i < sizeof(k_route_pins) / sizeof(k_route_pins[0]); ++i) {
        if (gpio_get_level(k_route_pins[i]) != 0) {
            ESP_LOGE(TAG, "route pad GPIO%d reads high with the muxes supposedly open", (int)k_route_pins[i]);
            ok = false;
        }
    }
    return ok;
}

static bool op_mux_connect(void *ctx)
{
    hw_t *h = ctx;
    routes_readback_on();
    gpio_set_level(VOLT_MUX_A0_PIN, VOLT_MUX_ADDR_VDUT & 1);   // address first, enable last
    gpio_set_level(VOLT_MUX_A1_PIN, (VOLT_MUX_ADDR_VDUT >> 1) & 1);
    range_manager_standby_resume(&h->b->range, &s_range);       // bypass + FINE mux + autorange edges, via their owner
    gpio_set_level(MUX_EN_PIN, 1);
    return gpio_get_level(MUX_EN_PIN) == 1 &&
           gpio_get_level(VOLT_MUX_A0_PIN) == (VOLT_MUX_ADDR_VDUT & 1) &&
           gpio_get_level(VOLT_MUX_A1_PIN) == ((VOLT_MUX_ADDR_VDUT >> 1) & 1);
}

static bool op_cancelled(void *ctx)
{
    (void)ctx;
    return s_cancel && s_cancel();
}

static bool op_bypass_park(void *ctx)
{
    hw_t *h = ctx;
    range_manager_standby_park(&h->b->range, &s_range);
    return true;
}

static bool op_adc_gate(void *ctx, sb_gate_t mode)
{
    (void)ctx;
    if (mode != SB_GATE_OPEN) s_adc_unavailable = true;   // before the first converter pin moves
    adaq_ll_gate_set(mode != SB_GATE_OPEN, mode == SB_GATE_OWNER_ONLY);
    return true;
}

static bool op_adaq_reset(void *ctx, bool asserted)
{
    (void)ctx;
    gpio_set_level(ADAQ_SHARED_RESET_PIN, asserted ? 0 : 1);
    return true;
}

static bool op_bus_release(void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < sizeof(k_drive_pins) / sizeof(k_drive_pins[0]); ++i) {
        const gpio_num_t p = k_drive_pins[i];
        esp_rom_gpio_pad_select_gpio(p);
        esp_rom_gpio_connect_out_signal(p, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(p, GPIO_MODE_DISABLE);
        gpio_set_pull_mode(p, GPIO_PULLDOWN_ONLY);
    }
    for (size_t i = 0; i < sizeof(k_sense_pins) / sizeof(k_sense_pins[0]); ++i) {
        gpio_set_pull_mode(k_sense_pins[i], GPIO_PULLDOWN_ONLY);
    }
    return true;
}

static esp_err_t route_bus(spi_host_device_t host, gpio_num_t sclk, gpio_num_t mosi, gpio_num_t miso)
{
    const spi_bus_config_t cfg = {
        .mosi_io_num = mosi, .miso_io_num = miso, .sclk_io_num = sclk,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .data4_io_num = -1, .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
    };
    uint32_t got = 0;
    return spicommon_bus_initialize_io(host, &cfg,
                                       SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK |
                                       SPICOMMON_BUSFLAG_MISO | SPICOMMON_BUSFLAG_MOSI, &got);
}

static bool op_bus_restore(void *ctx)
{
    hw_t *h = ctx;
    // The existing bus and device handles stay as they are; only the pad routing
    // that op_bus_release took away is put back (the same calls spi_bus_initialize
    // made at boot).
    bool ok = route_bus(ADAQ_BUSA_HOST, ADAQ_BUSA_SCLK_PIN, ADAQ_BUSA_MOSI_PIN, ADAQ_BUSA_MISO_PIN) == ESP_OK;
    ok = (route_bus(ADAQ_BUSB_HOST, ADAQ_BUSB_SCLK_PIN, ADAQ_BUSB_MOSI_PIN, ADAQ_BUSB_MISO_PIN) == ESP_OK) && ok;
    for (size_t i = 0; i < sizeof(k_drive_pins) / sizeof(k_drive_pins[0]); ++i) {
        gpio_set_pull_mode(k_drive_pins[i], GPIO_FLOATING);
    }
    for (size_t i = 0; i < sizeof(k_sense_pins) / sizeof(k_sense_pins[0]); ++i) {
        gpio_set_pull_mode(k_sense_pins[i], GPIO_FLOATING);
    }
    // Chip selects: the established way to hand the CS pad back to the SPI host
    // (it is what ends every streaming session).
    for (int i = 0; i < ADAQ_COUNT; ++i) adaq_ll_cs_manual_end(&h->b->adaq[i].ll);
    return ok;
}

static bool op_rail_set(void *ctx, sb_rail_t rail, bool on)
{
    (void)ctx;
    const gpio_num_t pin = rail == SB_RAIL_24V ? PWR_24V_EN_PIN
                         : rail == SB_RAIL_26V ? PWR_26V_EN_PIN
                                               : PWR_3V3_EN_PIN;
    gpio_set_level(pin, on ? 1 : 0);
    return true;
}

static int op_pg_read(void *ctx)
{
    (void)ctx;
    return gpio_get_level(PWR_3V3_PG_PIN) ? 1 : 0;
}

static bool op_adaq_pulse(void *ctx)
{
    (void)ctx;
    gpio_set_level(ADAQ_SHARED_RESET_PIN, 0);
    esp_rom_delay_us(10);
    gpio_set_level(ADAQ_SHARED_RESET_PIN, 1);
    esp_rom_delay_us(300);                       // datasheet: >= 200 us before the first SPI write
    return true;
}

static void op_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us >= 1000u) vTaskDelay(pdMS_TO_TICKS(us / 1000u) ? pdMS_TO_TICKS(us / 1000u) : 1);
    else esp_rom_delay_us(us);
}

static sb_rails_ops_t make_ops(hw_t *h)
{
    const sb_rails_ops_t o = {
        h, op_mux_disconnect, op_bypass_park, op_mux_verify, op_mux_connect, op_adc_gate, op_adaq_reset,
        op_bus_release, op_bus_restore, op_rail_set, op_pg_read, op_adaq_pulse, op_delay_us, op_cancelled,
    };
    return o;
}

// ---------------------------------------------------------------------------
// stages
// ---------------------------------------------------------------------------
bool standby_analog_save(daq_board_t *b)
{
    bool ok = true;
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        const esp_err_t err = sb_adaq_save(&b->adaq[i], b->adaq_ok[i], &s_shadow[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADAQ #%d configuration could not be read: %s", i, esp_err_to_name(err));
            ok = false;
        }
    }
    s_saved = ok;
    return ok;
}

bool standby_analog_mux_off(daq_board_t *b)
{
    hw_t h = { b };
    const sb_rails_ops_t ops = make_ops(&h);
    const bool ok = sb_rails_mux_off(&s_rails, &ops);   // every analog rail is still present
    ESP_LOGI(TAG, "muxes disconnected: %s (fail mask 0x%x)", ok ? "ok" : "FAILED",
             (unsigned)s_rails.fail_mask);
    return ok;
}

bool standby_analog_off(daq_board_t *b)
{
    if (!s_saved) {
        ESP_LOGE(TAG, "refusing to cut the analog rails: no converter configuration was saved");
        return false;
    }
    hw_t h = { b };
    const sb_rails_ops_t ops = make_ops(&h);
    // The guard inside refuses (touching nothing) unless the muxes read back open.
    const bool ok = sb_rails_off(&s_rails, &ops);
    ESP_LOGI(TAG, "analog rails off: %s (fail mask 0x%x)", ok ? "ok" : "FAILED",
             (unsigned)s_rails.fail_mask);
    return ok;
}

bool standby_analog_on(daq_board_t *b)
{
    // Rails this module never switched off are up and configured: cycling them now
    // would reset the converters and lose the configuration.
    if (!s_rails.cut) return true;
    hw_t h = { b };
    const sb_rails_ops_t ops = make_ops(&h);
    const bool ok = sb_rails_on(&s_rails, &ops);   // the muxes stay disconnected
    ESP_LOGI(TAG, "analog rails on: %s (fail mask 0x%x)", ok ? "ok" : "FAILED",
             (unsigned)s_rails.fail_mask);
    return ok;
}

bool standby_analog_restore(daq_board_t *b)
{
    if (!s_rails.cut) {                          // never switched off: nothing was lost
        s_adc_unavailable = false;
        return true;
    }
    if (!s_rails.powered || !s_saved) return false;

    adaq_ll_gate_set(true, true);                // only this task may talk to the converters now
    bool ok = true;
    for (int i = 0; i < ADAQ_COUNT && ok; ++i) {
        if (s_cancel && s_cancel()) {            // superseded: stop between converters, leave them unavailable
            ESP_LOGW(TAG, "restore superseded before ADAQ #%d", i);
            ok = false;
            break;
        }
        esp_err_t err = sb_adaq_restore(&b->adaq[i], &s_shadow[i]);
        if (err == ESP_OK) err = sb_adaq_check_conversion(&b->adaq[i], s_shadow[i].was_ok);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADAQ #%d restore failed: %s", i, esp_err_to_name(err));
            ok = false;
        }
    }
    if (ok && b->adaq_ok[ADAQ_SYNC_MASTER_INDEX]) {
        ok = adaq7769_sync_pulse(&b->adaq[ADAQ_SYNC_MASTER_INDEX]) == ESP_OK;   // all parts configured: align them
    }
    if (!ok) {
        adaq_ll_gate_set(true, false);           // converters stay unavailable and the muxes stay disconnected
        return false;
    }

    // The converters are configured, identified and converting. The ANALOG PATH is
    // deliberately not reconnected: MUX_EN, both address buses and the bypass
    // switches stay where stage 3 left them until an explicit request asks for a
    // measurement (standby_analog_provision_routes).
    s_rails.cut = false;
    adaq_ll_gate_set(false, false);
    s_adc_unavailable = false;
    s_saved = false;
    ESP_LOGI(TAG, "converters restored; measurement routes held open");
    return true;
}

bool standby_analog_provision_routes(daq_board_t *b)
{
    if (!s_rails.mux_open) return true;          // never disconnected: the board is as it always was
    if (s_adc_unavailable) return false;         // converters not back: nothing to connect them to
    hw_t h = { b };
    const sb_rails_ops_t ops = make_ops(&h);
    const bool ok = sb_rails_mux_connect(&s_rails, &ops);
    ESP_LOGI(TAG, "measurement routes %s (fail mask 0x%x)", ok ? "connected" : "NOT connected",
             (unsigned)s_rails.fail_mask);
    return ok;
}
