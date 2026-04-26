// =============================================================================
// husb238.cpp - HUSB238 USB PD Sink Controller Driver
// =============================================================================

#include "husb238.h"
#include "i2c_bus.h"
#include "config.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "husb238";

static Husb238State s_state = {};
static uint8_t s_requested_pdo = 0;

// Canonical PDO index used by BBP/UI: 1..6 => 5/9/12/15/18/20.
static Husb238Voltage voltage_from_selected_pdo(uint8_t sel)
{
    switch (sel & 0x0F) {
        case 1: return HUSB238_V_5V;
        case 2: return HUSB238_V_9V;
        case 3: return HUSB238_V_12V;
        case 4: return HUSB238_V_15V;
        case 5: return HUSB238_V_18V;
        case 6: return HUSB238_V_20V;
        default: return HUSB238_V_UNATTACHED;
    }
}

// HUSB238 SRC_PDO selection nibble in bits [7:4]:
// 1=5V, 2=9V, 3=12V, 8=15V, 9=18V, 10=20V (Adafruit reference driver).
static uint8_t pdo_sel_nibble_from_voltage(Husb238Voltage voltage)
{
    switch (voltage) {
        case HUSB238_V_5V:  return 0x01;
        case HUSB238_V_9V:  return 0x02;
        case HUSB238_V_12V: return 0x03;
        case HUSB238_V_15V: return 0x08;
        case HUSB238_V_18V: return 0x09;
        case HUSB238_V_20V: return 0x0A;
        default:            return 0x00;
    }
}

static Husb238Voltage voltage_from_sel_nibble(uint8_t nibble)
{
    switch (nibble & 0x0F) {
        case 0x01: return HUSB238_V_5V;
        case 0x02: return HUSB238_V_9V;
        case 0x03: return HUSB238_V_12V;
        case 0x08: return HUSB238_V_15V;
        case 0x09: return HUSB238_V_18V;
        case 0x0A: return HUSB238_V_20V;
        default:   return HUSB238_V_UNATTACHED;
    }
}

static uint8_t canonical_pdo_from_voltage(Husb238Voltage voltage)
{
    switch (voltage) {
        case HUSB238_V_5V:  return 1;
        case HUSB238_V_9V:  return 2;
        case HUSB238_V_12V: return 3;
        case HUSB238_V_15V: return 4;
        case HUSB238_V_18V: return 5;
        case HUSB238_V_20V: return 6;
        default:            return 0;
    }
}

static Husb238Voltage decode_status_voltage(uint8_t code, bool attached)
{
    if (!attached) return HUSB238_V_UNATTACHED;
    switch (code & 0x0F) {
        case 1:  return HUSB238_V_5V;
        case 2:  return HUSB238_V_9V;
        case 3:  return HUSB238_V_12V;
        case 4:  return HUSB238_V_15V;
        case 5:  return HUSB238_V_18V;
        case 6:  return HUSB238_V_20V;
        default: return HUSB238_V_UNATTACHED;
    }
}

static bool read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_bus_write_read(HUSB238_I2C_ADDR, &reg, 1, val, 1, 50);
}

static bool write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_bus_write(HUSB238_I2C_ADDR, buf, 2, 50);
}

float husb238_decode_voltage(Husb238Voltage v)
{
    switch (v) {
        case HUSB238_V_5V:  return 5.0f;
        case HUSB238_V_9V:  return 9.0f;
        case HUSB238_V_12V: return 12.0f;
        case HUSB238_V_15V: return 15.0f;
        case HUSB238_V_18V: return 18.0f;
        case HUSB238_V_20V: return 20.0f;
        default:            return 0.0f;
    }
}

float husb238_decode_current(Husb238Current i)
{
    switch (i) {
        case HUSB238_I_0_5A:  return 0.5f;
        case HUSB238_I_0_7A:  return 0.7f;
        case HUSB238_I_1_0A:  return 1.0f;
        case HUSB238_I_1_25A: return 1.25f;
        case HUSB238_I_1_5A:  return 1.5f;
        case HUSB238_I_1_75A: return 1.75f;
        case HUSB238_I_2_0A:  return 2.0f;
        case HUSB238_I_2_25A: return 2.25f;
        case HUSB238_I_2_5A:  return 2.5f;
        case HUSB238_I_2_75A: return 2.75f;
        case HUSB238_I_3_0A:  return 3.0f;
        case HUSB238_I_3_25A: return 3.25f;
        case HUSB238_I_3_5A:  return 3.5f;
        case HUSB238_I_4_0A:  return 4.0f;
        case HUSB238_I_4_5A:  return 4.5f;
        case HUSB238_I_5_0A:  return 5.0f;
        default:              return 0.0f;
    }
}

static void decode_pdo(uint8_t reg_val, Husb238PdoInfo *pdo)
{
    pdo->detected = (reg_val & HUSB238_PDO_DETECTED) != 0;
    pdo->max_current = (Husb238Current)(reg_val & HUSB238_PDO_CURRENT_MASK);
}

bool husb238_init(void)
{
    memset(&s_state, 0, sizeof(s_state));

    if (!i2c_bus_ready()) {
        ESP_LOGW(TAG, "I2C bus not ready");
        return false;
    }

    s_state.present = i2c_bus_probe(HUSB238_I2C_ADDR);
    if (!s_state.present) {
        ESP_LOGW(TAG, "HUSB238 not found at 0x%02X", HUSB238_I2C_ADDR);
        return false;
    }

    ESP_LOGI(TAG, "HUSB238 found at 0x%02X", HUSB238_I2C_ADDR);

    // Read initial status
    husb238_update();
    if (s_requested_pdo == 0) {
        s_requested_pdo = s_state.selected_pdo;
    }

    return true;
}

bool husb238_present(void)
{
    return s_state.present;
}

const Husb238State* husb238_get_state(void)
{
    return &s_state;
}

bool husb238_update(void)
{
    if (!s_state.present) return false;

    uint8_t status0 = 0, status1 = 0;

    if (!read_reg(HUSB238_REG_PD_STATUS0, &status0)) return false;
    if (!read_reg(HUSB238_REG_PD_STATUS1, &status1)) return false;

    // Adafruit reference mapping:
    // PD_STATUS1: bit7=CC dir, bit6=attached, bits5:3=pd response, bit2=5V contract
    // PD_STATUS0: bits7:4=source voltage code, bits3:0=source current code
    s_state.cc_direction = (status1 & 0x80) != 0;
    s_state.attached = (status1 & 0x40) != 0;
    s_state.pd_response = (uint8_t)((status1 >> 3) & 0x07);
    s_state.has_5v_contract = (status1 & 0x04) != 0;

    uint8_t voltage_code = (uint8_t)((status0 >> 4) & 0x0F);
    s_state.current = (Husb238Current)(status0 & 0x0F);
    s_state.current_a = husb238_decode_current(s_state.current);

    // Read source PDOs
    uint8_t pdo_val;
    if (read_reg(HUSB238_REG_SRC_PDO_5V, &pdo_val))  decode_pdo(pdo_val, &s_state.pdo_5v);
    if (read_reg(HUSB238_REG_SRC_PDO_9V, &pdo_val))  decode_pdo(pdo_val, &s_state.pdo_9v);
    if (read_reg(HUSB238_REG_SRC_PDO_12V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_12v);
    if (read_reg(HUSB238_REG_SRC_PDO_15V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_15v);
    if (read_reg(HUSB238_REG_SRC_PDO_18V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_18v);
    if (read_reg(HUSB238_REG_SRC_PDO_20V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_20v);

    // Read selected PDO nibble [7:4] and normalize to canonical 1..6.
    uint8_t sel_reg = 0;
    if (read_reg(HUSB238_REG_SRC_PDO, &sel_reg)) {
        Husb238Voltage sel_v = voltage_from_sel_nibble((uint8_t)((sel_reg >> 4) & 0x0F));
        uint8_t sel_canon = canonical_pdo_from_voltage(sel_v);
        if (sel_canon != 0) {
            s_state.selected_pdo = sel_canon;
            if (s_requested_pdo == 0) {
                s_requested_pdo = sel_canon;
            }
        }
    }
    s_state.voltage = decode_status_voltage(voltage_code, s_state.attached);
    s_state.voltage_v = husb238_decode_voltage(s_state.voltage);
    s_state.power_w = s_state.voltage_v * s_state.current_a;

    // Some adapters/controllers report selected PDO readback inconsistently.
    // Keep selected_pdo stable for UI/debug using requested value when available.
    uint8_t effective_sel = s_requested_pdo ? s_requested_pdo : s_state.selected_pdo;
    Husb238Voltage selected_v = voltage_from_selected_pdo(effective_sel);
    if (s_state.attached && selected_v != HUSB238_V_UNATTACHED) {
        // Only fall back to selected PDO when status decode is unavailable.
        // Do not override a valid status code; that can hide real negotiation failures.
        if (s_state.voltage == HUSB238_V_UNATTACHED) {
            ESP_LOGW(TAG,
                     "PD status voltage unavailable (code=%u), falling back to effective PDO=0x%02X (req=0x%02X reg=0x%02X)",
                     (unsigned)voltage_code, (unsigned)effective_sel,
                     (unsigned)s_requested_pdo, (unsigned)s_state.selected_pdo);
            s_state.voltage = selected_v;
            s_state.voltage_v = husb238_decode_voltage(selected_v);
            s_state.power_w = s_state.voltage_v * s_state.current_a;
        } else if (s_state.voltage != selected_v) {
            ESP_LOGW(TAG,
                     "PD status voltage code=%u differs from effective PDO=0x%02X (req=0x%02X reg=0x%02X)",
                     (unsigned)voltage_code, (unsigned)effective_sel,
                     (unsigned)s_requested_pdo, (unsigned)s_state.selected_pdo);
        }
        s_state.selected_pdo = effective_sel;
    }

    return true;
}

bool husb238_get_src_cap(void)
{
    if (!s_state.present) return false;
    return write_reg(HUSB238_REG_GO_COMMAND, HUSB238_GO_GET_SRC_CAP);
}

bool husb238_select_pdo(Husb238Voltage voltage)
{
    if (!s_state.present) return false;

    uint8_t sel_nibble = pdo_sel_nibble_from_voltage(voltage);
    uint8_t canonical = canonical_pdo_from_voltage(voltage);
    if (sel_nibble == 0 || canonical == 0) return false;

    uint8_t reg_val = 0;
    if (!read_reg(HUSB238_REG_SRC_PDO, &reg_val)) return false;
    reg_val = (uint8_t)((reg_val & 0x0F) | (uint8_t)(sel_nibble << 4));
    if (!write_reg(HUSB238_REG_SRC_PDO, reg_val)) return false;

    s_state.selected_pdo = canonical;
    s_requested_pdo = canonical;
    ESP_LOGI(TAG, "Selected PDO nibble=0x%X (canonical=%u) for request %.0fV",
             (unsigned)sel_nibble, (unsigned)canonical,
             (double)husb238_decode_voltage(voltage));

    return true;
}

bool husb238_go_command(uint8_t cmd)
{
    if (!s_state.present) return false;
    // GO command occupies low bits (per Adafruit reference usage).
    // Preserve upper bits in case controller stores status/control there.
    uint8_t reg = 0;
    if (!read_reg(HUSB238_REG_GO_COMMAND, &reg)) return false;
    reg = (uint8_t)((reg & 0xE0) | (cmd & 0x1F));
    return write_reg(HUSB238_REG_GO_COMMAND, reg);
}
