// =============================================================================
// bb_power.c — Target connector power management
//
// Controls VADJ3/VADJ4 GPIO enables and keeps the adjustable 3V3 logic rail and
// high-speed level shifter disabled by default.
// Reads current via the LTM8083 ISMON pins.
// =============================================================================

#include "bb_power.h"
#include "bb_config.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"

static ConnectorStatus s_conn[2] = {};
static bool s_3v3_adj_enabled = false;

static void init_output_low(uint pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
}

void bb_power_init(void)
{
    // Configure rail enables and level-shifter controls as outputs, start safe.
    init_output_low(BB_VADJ3_EN_PIN);
    init_output_low(BB_VADJ4_EN_PIN);
    init_output_low(BB_3V3_ADJ_EN_PIN);
    init_output_low(BB_LEVEL_SHIFT_OE_PIN);
    init_output_low(BB_LEVEL_SHIFT_DIR_PIN);

    // Initialize ADC for current sense
    adc_init();
    adc_gpio_init(BB_CURRENT_A_ADC);  // GPIO26 = ADC0
    adc_gpio_init(BB_CURRENT_B_ADC);  // GPIO27 = ADC1
    adc_gpio_init(BB_VADJ3_SENSE_ADC); // GPIO28 = ADC2
    adc_gpio_init(BB_VADJ4_SENSE_ADC); // GPIO29 = ADC3

    s_conn[0] = (ConnectorStatus){ .enabled = false, .current_ma = 0, .voltage_mv = 0, .fault = false };
    s_conn[1] = (ConnectorStatus){ .enabled = false, .current_ma = 0, .voltage_mv = 0, .fault = false };
    s_3v3_adj_enabled = false;
}

void bb_power_set(uint8_t connector, bool enable)
{
    if (connector > 1) return;

    uint pin = (connector == 0) ? BB_EN_A_PIN : BB_EN_B_PIN;
    gpio_put(pin, enable ? 1 : 0);
    s_conn[connector].enabled = enable;

    // If disabling, clear fault
    if (!enable) {
        s_conn[connector].fault = false;
    }
}

bool bb_power_get_enabled(uint8_t connector)
{
    if (connector > 1) return false;
    return s_conn[connector].enabled;
}

void bb_power_set_3v3_adj(bool enable)
{
    gpio_put(BB_3V3_ADJ_EN_PIN, enable ? 1 : 0);
    s_3v3_adj_enabled = enable;

    if (!enable) {
        gpio_put(BB_LEVEL_SHIFT_OE_PIN, 0);
    }
}

bool bb_power_get_3v3_adj_enabled(void)
{
    return s_3v3_adj_enabled;
}

float bb_power_read_current(uint8_t connector)
{
    if (connector > 1) return 0.0f;

    uint8_t adc_ch = (connector == 0) ? 0 : 1;
    adc_select_input(adc_ch);

    // Average 16 readings. Calibration stages will layer rail-specific offsets
    // on top of this raw conversion.
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc_read();
    }
    float raw_avg = (float)sum / 16.0f;

    float vismon_mv = raw_avg * (float)BB_CURRENT_ADC_VREF / 4095.0f;
    float sense_mv = vismon_mv - (float)BB_LTM8083_ISMON_OFFSET_MV;
    if (sense_mv < 0.0f) {
        sense_mv = 0.0f;
    }

    float rcs_ohm = (float)BB_CURRENT_SHUNT_MOHM / 1000.0f;
    float current_a = (sense_mv / 1000.0f) / ((float)BB_LTM8083_ISMON_GAIN * rcs_ohm);
    float current_ma = current_a * 1000.0f;

    s_conn[connector].current_ma = current_ma;
    return current_ma;
}

float bb_power_read_voltage(uint8_t connector)
{
    if (connector > 1) return 0.0f;

    uint8_t adc_ch = (connector == 0) ? 2 : 3;
    adc_select_input(adc_ch);

    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc_read();
    }
    float raw_avg = (float)sum / 16.0f;
    float vadc_mv = raw_avg * (float)BB_CURRENT_ADC_VREF / 4095.0f;
    float scale = ((float)BB_VADJ_SENSE_RTOP_OHM + (float)BB_VADJ_SENSE_RBOT_OHM) /
                  (float)BB_VADJ_SENSE_RBOT_OHM;
    float rail_mv = vadc_mv * scale;

    s_conn[connector].voltage_mv = rail_mv;
    return rail_mv;
}

bool bb_power_get_fault(uint8_t connector)
{
    if (connector > 1) return false;
    s_conn[connector].fault = false;
    return false;
}

void bb_power_update(void)
{
    for (uint8_t i = 0; i < 2; i++) {
        if (s_conn[i].enabled) {
            bb_power_read_current(i);
            bb_power_read_voltage(i);
            bb_power_get_fault(i);
        }
    }
}

void bb_power_get_status(ConnectorStatus *a, ConnectorStatus *b)
{
    if (a) *a = s_conn[0];
    if (b) *b = s_conn[1];
}
