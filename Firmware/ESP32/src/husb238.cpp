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

    // Decode PD_STATUS0
    s_state.cc_direction = (status0 & HUSB238_CC_DIR_MASK) != 0;
    s_state.attached = (status0 & HUSB238_ATTACH_MASK) != 0;
    s_state.pd_response = (status0 & HUSB238_PD_STATUS_MASK) >> 1;
    s_state.has_5v_contract = (status0 & HUSB238_5V_CONTRACT) != 0;

    // Decode PD_STATUS1
    s_state.voltage = (Husb238Voltage)((status1 & HUSB238_VOLTAGE_MASK) >> 4);
    s_state.current = (Husb238Current)(status1 & HUSB238_CURRENT_MASK);
    s_state.voltage_v = husb238_decode_voltage(s_state.voltage);
    s_state.current_a = husb238_decode_current(s_state.current);
    s_state.power_w = s_state.voltage_v * s_state.current_a;

    // Read source PDOs
    uint8_t pdo_val;
    if (read_reg(HUSB238_REG_SRC_PDO_5V, &pdo_val))  decode_pdo(pdo_val, &s_state.pdo_5v);
    if (read_reg(HUSB238_REG_SRC_PDO_9V, &pdo_val))  decode_pdo(pdo_val, &s_state.pdo_9v);
    if (read_reg(HUSB238_REG_SRC_PDO_12V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_12v);
    if (read_reg(HUSB238_REG_SRC_PDO_15V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_15v);
    if (read_reg(HUSB238_REG_SRC_PDO_18V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_18v);
    if (read_reg(HUSB238_REG_SRC_PDO_20V, &pdo_val)) decode_pdo(pdo_val, &s_state.pdo_20v);

    // Read selected PDO
    uint8_t sel;
    if (read_reg(HUSB238_REG_SRC_PDO, &sel)) s_state.selected_pdo = sel;

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

    // Map voltage to SRC_PDO register value
    uint8_t pdo_sel;
    switch (voltage) {
        case HUSB238_V_5V:  pdo_sel = 0x01; break;
        case HUSB238_V_9V:  pdo_sel = 0x02; break;
        case HUSB238_V_12V: pdo_sel = 0x03; break;
        case HUSB238_V_15V: pdo_sel = 0x04; break;
        case HUSB238_V_18V: pdo_sel = 0x05; break;
        case HUSB238_V_20V: pdo_sel = 0x06; break;
        default: return false;
    }

    if (!write_reg(HUSB238_REG_SRC_PDO, pdo_sel)) return false;
    s_state.selected_pdo = pdo_sel;

    return true;
}

bool husb238_go_command(uint8_t cmd)
{
    if (!s_state.present) return false;
    return write_reg(HUSB238_REG_GO_COMMAND, cmd);
}
