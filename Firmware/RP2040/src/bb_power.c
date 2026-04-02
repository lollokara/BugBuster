// =============================================================================
// bb_power.c — Target connector power management
//
// Controls EN_A/EN_B GPIO pins to enable/disable target power.
// Reads current via ADC shunt resistor. Monitors fault pins.
// =============================================================================

#include "bb_power.h"
#include "bb_config.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"

static ConnectorStatus s_conn[2] = {};

void bb_power_init(void)
{
    // Configure enable pins as outputs, start disabled
    gpio_init(BB_EN_A_PIN);
    gpio_set_dir(BB_EN_A_PIN, GPIO_OUT);
    gpio_put(BB_EN_A_PIN, 0);

    gpio_init(BB_EN_B_PIN);
    gpio_set_dir(BB_EN_B_PIN, GPIO_OUT);
    gpio_put(BB_EN_B_PIN, 0);

    // Configure fault pins as inputs with pull-up (active low)
    gpio_init(BB_FAULT_A_PIN);
    gpio_set_dir(BB_FAULT_A_PIN, GPIO_IN);
    gpio_pull_up(BB_FAULT_A_PIN);

    gpio_init(BB_FAULT_B_PIN);
    gpio_set_dir(BB_FAULT_B_PIN, GPIO_IN);
    gpio_pull_up(BB_FAULT_B_PIN);

    // Initialize ADC for current sense
    adc_init();
    adc_gpio_init(BB_CURRENT_A_ADC);  // GPIO26 = ADC0
    adc_gpio_init(BB_CURRENT_B_ADC);  // GPIO27 = ADC1

    s_conn[0] = (ConnectorStatus){ .enabled = false, .current_ma = 0, .fault = false };
    s_conn[1] = (ConnectorStatus){ .enabled = false, .current_ma = 0, .fault = false };
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

float bb_power_read_current(uint8_t connector)
{
    if (connector > 1) return 0.0f;

    // Select ADC channel: GPIO26 = ADC0, GPIO27 = ADC1
    uint8_t adc_ch = (connector == 0) ? 0 : 1;
    adc_select_input(adc_ch);

    // Average 4 readings
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        sum += adc_read();
    }
    float raw_avg = (float)sum / 4.0f;

    // Convert: ADC is 12-bit (0-4095), VREF = 3.3V
    // V_shunt = raw * 3.3 / 4095
    // I = V_shunt / R_shunt
    float v_shunt = raw_avg * (float)BB_CURRENT_ADC_VREF / (4095.0f * 1000.0f);  // in V
    float current_a = v_shunt / ((float)BB_CURRENT_SHUNT_MOHM / 1000.0f);         // in A
    float current_ma = current_a * 1000.0f;

    s_conn[connector].current_ma = current_ma;
    return current_ma;
}

bool bb_power_get_fault(uint8_t connector)
{
    if (connector > 1) return false;
    uint pin = (connector == 0) ? BB_FAULT_A_PIN : BB_FAULT_B_PIN;
    bool fault = !gpio_get(pin);  // Active low
    s_conn[connector].fault = fault;
    return fault;
}

void bb_power_update(void)
{
    for (uint8_t i = 0; i < 2; i++) {
        if (s_conn[i].enabled) {
            bb_power_read_current(i);
            bb_power_get_fault(i);

            // Auto-disable on fault
            if (s_conn[i].fault) {
                bb_power_set(i, false);
            }
        }
    }
}

void bb_power_get_status(ConnectorStatus *a, ConnectorStatus *b)
{
    if (a) *a = s_conn[0];
    if (b) *b = s_conn[1];
}
