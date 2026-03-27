#pragma once

#include <stdint.h>

// =============================================================================
// ad74416h_regs.h - Complete register map for the AD74416H
//
// Per-channel register stride: 0x0C (12 decimal)
// Channel n base offset = n * AD74416H_CH_STRIDE
// Example: CH_FUNC_SETUP for channel 2 = AD74416H_REG_CH_FUNC_SETUP(2) = 0x01 + 0x18 = 0x19
// =============================================================================

// -----------------------------------------------------------------------------
// Channel Register Stride
// -----------------------------------------------------------------------------
#define AD74416H_CH_STRIDE      0x0C    // 12 registers per channel block

// -----------------------------------------------------------------------------
// Per-Channel Register Base Addresses (use macros below for channel access)
// -----------------------------------------------------------------------------
#define REG_CH_FUNC_SETUP_BASE      0x01
#define REG_ADC_CONFIG_BASE         0x02
#define REG_DIN_CONFIG0_BASE        0x03
#define REG_DIN_CONFIG1_BASE        0x04
#define REG_OUTPUT_CONFIG_BASE      0x05
#define REG_RTD_CONFIG_BASE         0x06
#define REG_FET_LKG_COMP_BASE       0x07
#define REG_DO_EXT_CONFIG_BASE      0x08
#define REG_I_BURNOUT_CONFIG_BASE   0x09
#define REG_DAC_CODE_BASE           0x0A
// 0x0B is reserved per-channel
#define REG_DAC_ACTIVE_BASE         0x0C    // Read-only, DAC active code

// -----------------------------------------------------------------------------
// Per-Channel Register Access Macros
// ch = 0..3
// -----------------------------------------------------------------------------
#define AD74416H_REG_CH_FUNC_SETUP(ch)     (REG_CH_FUNC_SETUP_BASE    + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_ADC_CONFIG(ch)        (REG_ADC_CONFIG_BASE        + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_DIN_CONFIG0(ch)       (REG_DIN_CONFIG0_BASE       + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_DIN_CONFIG1(ch)       (REG_DIN_CONFIG1_BASE       + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_OUTPUT_CONFIG(ch)     (REG_OUTPUT_CONFIG_BASE     + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_RTD_CONFIG(ch)        (REG_RTD_CONFIG_BASE        + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_FET_LKG_COMP(ch)     (REG_FET_LKG_COMP_BASE      + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_DO_EXT_CONFIG(ch)    (REG_DO_EXT_CONFIG_BASE     + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_I_BURNOUT_CONFIG(ch) (REG_I_BURNOUT_CONFIG_BASE  + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_DAC_CODE(ch)         (REG_DAC_CODE_BASE          + (ch) * AD74416H_CH_STRIDE)
#define AD74416H_REG_DAC_ACTIVE(ch)       (REG_DAC_ACTIVE_BASE        + (ch) * AD74416H_CH_STRIDE)

// -----------------------------------------------------------------------------
// CH_FUNC_SETUP register bit fields (base 0x01)
// -----------------------------------------------------------------------------
#define CH_FUNC_SETUP_CH_FUNC_SHIFT     0
#define CH_FUNC_SETUP_CH_FUNC_MASK      (0x0F << CH_FUNC_SETUP_CH_FUNC_SHIFT)

// -----------------------------------------------------------------------------
// ADC_CONFIG register bit fields (base 0x02, per datasheet Table 43)
// Bit layout: [2:0] CONV_MUX, [3] RSVD, [6:4] CONV_RANGE, [7] RSVD,
//             [11:8] CONV_RATE, [15:12] RSVD
// -----------------------------------------------------------------------------
#define ADC_CONFIG_CONV_MUX_SHIFT       0
#define ADC_CONFIG_CONV_MUX_MASK        (0x07 << ADC_CONFIG_CONV_MUX_SHIFT)

#define ADC_CONFIG_CONV_RANGE_SHIFT     4
#define ADC_CONFIG_CONV_RANGE_MASK      (0x07 << ADC_CONFIG_CONV_RANGE_SHIFT)

#define ADC_CONFIG_CONV_RATE_SHIFT      8
#define ADC_CONFIG_CONV_RATE_MASK       (0x0F << ADC_CONFIG_CONV_RATE_SHIFT)

// -----------------------------------------------------------------------------
// DIN_CONFIG0 register bit fields (base 0x03, per datasheet Table 44)
// Bit layout: [4:0] DEBOUNCE_TIME, [5] RSVD, [6] DEBOUNCE_MODE,
//             [11:7] DIN_SINK, [12] DIN_SINK_RANGE, [13] COMPARATOR_EN,
//             [14] DIN_INV_COMP_OUT, [15] COUNT_EN
// -----------------------------------------------------------------------------
#define DIN_CONFIG0_DEBOUNCE_TIME_SHIFT     0
#define DIN_CONFIG0_DEBOUNCE_TIME_MASK      (0x1F << DIN_CONFIG0_DEBOUNCE_TIME_SHIFT)

#define DIN_CONFIG0_DEBOUNCE_MODE_SHIFT     6
#define DIN_CONFIG0_DEBOUNCE_MODE_MASK      (0x01 << DIN_CONFIG0_DEBOUNCE_MODE_SHIFT)

#define DIN_CONFIG0_DIN_SINK_SHIFT          7
#define DIN_CONFIG0_DIN_SINK_MASK           (0x1F << DIN_CONFIG0_DIN_SINK_SHIFT)

#define DIN_CONFIG0_DIN_SINK_RANGE_SHIFT    12
#define DIN_CONFIG0_DIN_SINK_RANGE_MASK     (0x01 << DIN_CONFIG0_DIN_SINK_RANGE_SHIFT)

#define DIN_CONFIG0_COMPARATOR_EN_SHIFT     13
#define DIN_CONFIG0_COMPARATOR_EN_MASK      (0x01 << DIN_CONFIG0_COMPARATOR_EN_SHIFT)

#define DIN_CONFIG0_DIN_INV_COMP_OUT_SHIFT  14
#define DIN_CONFIG0_DIN_INV_COMP_OUT_MASK   (0x01 << DIN_CONFIG0_DIN_INV_COMP_OUT_SHIFT)

#define DIN_CONFIG0_COUNT_EN_SHIFT          15
#define DIN_CONFIG0_COUNT_EN_MASK           (0x01 << DIN_CONFIG0_COUNT_EN_SHIFT)

// -----------------------------------------------------------------------------
// DIN_CONFIG1 register bit fields (base 0x04, per datasheet Table 45)
// Bit layout: [6:0] COMP_THRESH, [7] DIN_THRESH_MODE, [8] DIN_OC_DET_EN,
//             [9] DIN_SC_DET_EN, [10] DIN_INPUT_SELECT
// -----------------------------------------------------------------------------
#define DIN_CONFIG1_COMP_THRESH_SHIFT       0
#define DIN_CONFIG1_COMP_THRESH_MASK        (0x7F << DIN_CONFIG1_COMP_THRESH_SHIFT)

#define DIN_CONFIG1_DIN_THRESH_MODE_SHIFT   7
#define DIN_CONFIG1_DIN_THRESH_MODE_MASK    (0x01 << DIN_CONFIG1_DIN_THRESH_MODE_SHIFT)

#define DIN_CONFIG1_DIN_OC_DET_EN_SHIFT     8
#define DIN_CONFIG1_DIN_OC_DET_EN_MASK      (0x01 << DIN_CONFIG1_DIN_OC_DET_EN_SHIFT)

#define DIN_CONFIG1_DIN_SC_DET_EN_SHIFT     9
#define DIN_CONFIG1_DIN_SC_DET_EN_MASK      (0x01 << DIN_CONFIG1_DIN_SC_DET_EN_SHIFT)

#define DIN_CONFIG1_DIN_INPUT_SELECT_SHIFT  10
#define DIN_CONFIG1_DIN_INPUT_SELECT_MASK   (0x01 << DIN_CONFIG1_DIN_INPUT_SELECT_SHIFT)

// -----------------------------------------------------------------------------
// OUTPUT_CONFIG register bit fields (base 0x05, per datasheet Table 46)
// Bit layout: [0] I_LIMIT, [2:1] SLEW_LIN_RATE, [4:3] SLEW_LIN_STEP,
//             [6:5] SLEW_EN, [7] VOUT_RANGE, [8] HART_COMPL_SETTLED (RO),
//             [9] VIOUT_DRV_EN_DLY, [10] WAIT_LDAC_CMD, [11] VOUT_4W_EN,
//             [12] ALARM_DEG_PERIOD, [13] RSVD, [15:14] AVDD_SELECT
// -----------------------------------------------------------------------------
#define OUTPUT_CONFIG_I_LIMIT_SHIFT         0
#define OUTPUT_CONFIG_I_LIMIT_MASK          (0x01 << OUTPUT_CONFIG_I_LIMIT_SHIFT)

#define OUTPUT_CONFIG_SLEW_LIN_RATE_SHIFT   1
#define OUTPUT_CONFIG_SLEW_LIN_RATE_MASK    (0x03 << OUTPUT_CONFIG_SLEW_LIN_RATE_SHIFT)

#define OUTPUT_CONFIG_SLEW_LIN_STEP_SHIFT   3
#define OUTPUT_CONFIG_SLEW_LIN_STEP_MASK    (0x03 << OUTPUT_CONFIG_SLEW_LIN_STEP_SHIFT)

#define OUTPUT_CONFIG_SLEW_EN_SHIFT         5
#define OUTPUT_CONFIG_SLEW_EN_MASK          (0x03 << OUTPUT_CONFIG_SLEW_EN_SHIFT)

#define OUTPUT_CONFIG_VOUT_RANGE_SHIFT      7
#define OUTPUT_CONFIG_VOUT_RANGE_MASK       (0x01 << OUTPUT_CONFIG_VOUT_RANGE_SHIFT)

#define OUTPUT_CONFIG_VOUT_4W_EN_SHIFT      11
#define OUTPUT_CONFIG_VOUT_4W_EN_MASK       (0x01 << OUTPUT_CONFIG_VOUT_4W_EN_SHIFT)

#define OUTPUT_CONFIG_ALARM_DEG_PERIOD_SHIFT 12
#define OUTPUT_CONFIG_ALARM_DEG_PERIOD_MASK (0x01 << OUTPUT_CONFIG_ALARM_DEG_PERIOD_SHIFT)

#define OUTPUT_CONFIG_AVDD_SELECT_SHIFT     14
#define OUTPUT_CONFIG_AVDD_SELECT_MASK      (0x03 << OUTPUT_CONFIG_AVDD_SELECT_SHIFT)

// -----------------------------------------------------------------------------
// RTD_CONFIG register bit fields (base 0x06, per datasheet Table 47)
// Bit layout: [0] RTD_CURRENT, [1] RTD_EXC_SWAP, [2] RTD_MODE_SEL,
//             [3] RTD_ADC_REF
// -----------------------------------------------------------------------------
#define RTD_CONFIG_RTD_CURRENT_SHIFT        0
#define RTD_CONFIG_RTD_CURRENT_MASK         (0x01 << RTD_CONFIG_RTD_CURRENT_SHIFT)

#define RTD_CONFIG_RTD_EXC_SWAP_SHIFT       1
#define RTD_CONFIG_RTD_EXC_SWAP_MASK        (0x01 << RTD_CONFIG_RTD_EXC_SWAP_SHIFT)

#define RTD_CONFIG_RTD_MODE_SEL_SHIFT       2
#define RTD_CONFIG_RTD_MODE_SEL_MASK        (0x01 << RTD_CONFIG_RTD_MODE_SEL_SHIFT)

#define RTD_CONFIG_RTD_ADC_REF_SHIFT        3
#define RTD_CONFIG_RTD_ADC_REF_MASK         (0x01 << RTD_CONFIG_RTD_ADC_REF_SHIFT)

// -----------------------------------------------------------------------------
// DO_EXT_CONFIG register bit fields (base 0x08, per datasheet Table 49)
// Bit layout: [0] DO_MODE, [1] DO_SRC_SEL, [6:2] DO_T1, [7] DO_DATA,
//             [12:8] DO_T2, [15:13] RSVD
// -----------------------------------------------------------------------------
#define DO_EXT_CONFIG_DO_MODE_SHIFT         0
#define DO_EXT_CONFIG_DO_MODE_MASK          (0x01 << DO_EXT_CONFIG_DO_MODE_SHIFT)

#define DO_EXT_CONFIG_DO_SRC_SEL_SHIFT      1
#define DO_EXT_CONFIG_DO_SRC_SEL_MASK       (0x01 << DO_EXT_CONFIG_DO_SRC_SEL_SHIFT)

#define DO_EXT_CONFIG_DO_T1_SHIFT           2
#define DO_EXT_CONFIG_DO_T1_MASK            (0x1F << DO_EXT_CONFIG_DO_T1_SHIFT)   // [6:2]

#define DO_EXT_CONFIG_DO_DATA_SHIFT         7
#define DO_EXT_CONFIG_DO_DATA_MASK          (0x01 << DO_EXT_CONFIG_DO_DATA_SHIFT)

#define DO_EXT_CONFIG_DO_T2_SHIFT           8
#define DO_EXT_CONFIG_DO_T2_MASK            (0x1F << DO_EXT_CONFIG_DO_T2_SHIFT)   // [12:8]

// =============================================================================
// Global Register Addresses
// =============================================================================
#define REG_NOP                     0x00

// GPIO Configuration (6 GPIOs A-F, addresses 0x32..0x37)
#define REG_GPIO_CONFIG_BASE        0x32
#define REG_GPIO_CONFIG0            0x32    // GPIO_A
#define REG_GPIO_CONFIG1            0x33    // GPIO_B
#define REG_GPIO_CONFIG2            0x34    // GPIO_C
#define REG_GPIO_CONFIG3            0x35    // GPIO_D
#define REG_GPIO_CONFIG4            0x36    // GPIO_E
#define REG_GPIO_CONFIG5            0x37    // GPIO_F
#define AD74416H_REG_GPIO_CONFIG(gpio) (REG_GPIO_CONFIG_BASE + (gpio))
#define AD74416H_NUM_GPIOS          6

// GPIO_CONFIGn bit fields (per datasheet Table 53)
// Bit layout: [2:0] GPIO_SELECT, [3] GP_WK_PD_EN, [4] GPO_DATA,
//             [5] GPI_DATA (RO), [7:6] DIN_DO_CH
#define GPIO_CONFIG_GPIO_SELECT_SHIFT   0
#define GPIO_CONFIG_GPIO_SELECT_MASK    (0x07 << GPIO_CONFIG_GPIO_SELECT_SHIFT)

#define GPIO_CONFIG_GP_WK_PD_EN_SHIFT   3
#define GPIO_CONFIG_GP_WK_PD_EN_MASK    (0x01 << GPIO_CONFIG_GP_WK_PD_EN_SHIFT)

#define GPIO_CONFIG_GPO_DATA_SHIFT      4
#define GPIO_CONFIG_GPO_DATA_MASK       (0x01 << GPIO_CONFIG_GPO_DATA_SHIFT)

#define GPIO_CONFIG_GPI_DATA_SHIFT      5
#define GPIO_CONFIG_GPI_DATA_MASK       (0x01 << GPIO_CONFIG_GPI_DATA_SHIFT)

#define GPIO_CONFIG_DIN_DO_CH_SHIFT     6
#define GPIO_CONFIG_DIN_DO_CH_MASK      (0x03 << GPIO_CONFIG_DIN_DO_CH_SHIFT)

// Power Optimisation
#define REG_PWR_OPTIM_CONFIG        0x38
#define PWR_OPTIM_REF_EN_SHIFT      13
#define PWR_OPTIM_REF_EN_MASK       (0x01 << PWR_OPTIM_REF_EN_SHIFT)   // Internal reference enable

// ADC Conversion Control (corrected per datasheet Table 55)
// Bit layout: [15:14] RSVD, [13] ADC_RDY_CTRL, [12:10] CONV_RATE_DIAG,
//             [9:8] CONV_SEQ, [7] DIAG_3_EN, [6] DIAG_2_EN,
//             [5] DIAG_1_EN, [4] DIAG_0_EN, [3] CONV_D_EN,
//             [2] CONV_C_EN, [1] CONV_B_EN, [0] CONV_A_EN
#define REG_ADC_CONV_CTRL           0x39
#define ADC_CONV_CTRL_CONV_A_EN_SHIFT       0
#define ADC_CONV_CTRL_CONV_A_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_A_EN_SHIFT)
#define ADC_CONV_CTRL_CONV_B_EN_SHIFT       1
#define ADC_CONV_CTRL_CONV_B_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_B_EN_SHIFT)
#define ADC_CONV_CTRL_CONV_C_EN_SHIFT       2
#define ADC_CONV_CTRL_CONV_C_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_C_EN_SHIFT)
#define ADC_CONV_CTRL_CONV_D_EN_SHIFT       3
#define ADC_CONV_CTRL_CONV_D_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_D_EN_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN0_SHIFT        4
#define ADC_CONV_CTRL_DIAG_EN0_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN0_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN1_SHIFT        5
#define ADC_CONV_CTRL_DIAG_EN1_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN1_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN2_SHIFT        6
#define ADC_CONV_CTRL_DIAG_EN2_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN2_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN3_SHIFT        7
#define ADC_CONV_CTRL_DIAG_EN3_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN3_SHIFT)
#define ADC_CONV_CTRL_CONV_SEQ_SHIFT        8
#define ADC_CONV_CTRL_CONV_SEQ_MASK         (0x03 << ADC_CONV_CTRL_CONV_SEQ_SHIFT)
#define ADC_CONV_CTRL_CONV_RATE_DIAG_SHIFT  10
#define ADC_CONV_CTRL_CONV_RATE_DIAG_MASK   (0x07 << ADC_CONV_CTRL_CONV_RATE_DIAG_SHIFT)
#define ADC_CONV_CTRL_ADC_RDY_CTRL_SHIFT    13
#define ADC_CONV_CTRL_ADC_RDY_CTRL_MASK     (0x01 << ADC_CONV_CTRL_ADC_RDY_CTRL_SHIFT)

// Diagnostic Assignment
#define REG_DIAG_ASSIGN             0x3A

// Watchdog Timer
#define REG_WDT_CONFIG              0x3B

// Digital Input Comparator Output (4-bit, one per channel)
#define REG_DIN_COMP_OUT            0x3E
#define DIN_COMP_OUT_MASK           0x0F

// ADC Results - Upper registers (latches lower on read)
// ch = 0..3
#define REG_ADC_RESULT_UPR_BASE     0x41    // Addresses: 0x41, 0x43, 0x45, 0x47
#define REG_ADC_RESULT_BASE         0x42    // Addresses: 0x42, 0x44, 0x46, 0x48

#define AD74416H_REG_ADC_RESULT_UPR(ch)    (REG_ADC_RESULT_UPR_BASE + (ch) * 2)
#define AD74416H_REG_ADC_RESULT(ch)        (REG_ADC_RESULT_BASE     + (ch) * 2)

// ADC_RESULT_UPR bit fields (per datasheet Table 61)
// Bit layout: [7:0] CONV_RES[23:16], [9:8] CONV_SEQ_COUNT,
//             [12:10] CONV_RES_RANGE, [15:13] CONV_RES_MUX
#define ADC_RESULT_UPR_CONV_RES_SHIFT       0
#define ADC_RESULT_UPR_CONV_RES_MASK        (0xFF << ADC_RESULT_UPR_CONV_RES_SHIFT)

#define ADC_RESULT_UPR_CONV_SEQ_COUNT_SHIFT 8
#define ADC_RESULT_UPR_CONV_SEQ_COUNT_MASK  (0x03 << ADC_RESULT_UPR_CONV_SEQ_COUNT_SHIFT)

#define ADC_RESULT_UPR_CONV_RES_RANGE_SHIFT 10
#define ADC_RESULT_UPR_CONV_RES_RANGE_MASK  (0x07 << ADC_RESULT_UPR_CONV_RES_RANGE_SHIFT)

#define ADC_RESULT_UPR_CONV_RES_MUX_SHIFT   13
#define ADC_RESULT_UPR_CONV_RES_MUX_MASK    (0x07 << ADC_RESULT_UPR_CONV_RES_MUX_SHIFT)

// ADC Diagnostic Results
#define REG_ADC_DIAG_RESULT_BASE    0x49    // Addresses: 0x49, 0x4A, 0x4B, 0x4C
#define AD74416H_REG_ADC_DIAG_RESULT(diag) (REG_ADC_DIAG_RESULT_BASE + (diag))

// Last ADC Results (upper/lower pair)
#define REG_LAST_ADC_RESULT_UPR         0x4D
#define REG_LAST_ADC_RESULT             0x4E

// Digital Input Counters (32-bit, split across two 16-bit registers)
// Upper 16 bits (read first to latch lower): 0x4F, 0x51, 0x53, 0x55
// Lower 16 bits: 0x50, 0x52, 0x54, 0x56
#define REG_DIN_COUNTER_UPR_BASE    0x4F
#define REG_DIN_COUNTER_BASE        0x50

#define AD74416H_REG_DIN_COUNTER_UPR(ch) (REG_DIN_COUNTER_UPR_BASE + (ch) * 2)
#define AD74416H_REG_DIN_COUNTER(ch)     (REG_DIN_COUNTER_BASE     + (ch) * 2)

// =============================================================================
// Alert / Status Registers (CORRECTED per ADI no-OS driver register map)
// =============================================================================

// ALERT_STATUS (0x3F) - Global alert status, write 1 to clear bits
#define REG_ALERT_STATUS            0x3F

// ALERT_STATUS bit fields (corrected per ADI driver)
#define ALERT_STATUS_RESET_OCCURRED_SHIFT   0
#define ALERT_STATUS_RESET_OCCURRED_MASK    (0x01 << 0)
#define ALERT_STATUS_SUPPLY_ERR_SHIFT       2
#define ALERT_STATUS_SUPPLY_ERR_MASK        (0x01 << 2)
#define ALERT_STATUS_SPI_ERR_SHIFT          3
#define ALERT_STATUS_SPI_ERR_MASK           (0x01 << 3)
#define ALERT_STATUS_TEMP_ALERT_SHIFT       4
#define ALERT_STATUS_TEMP_ALERT_MASK        (0x01 << 4)
#define ALERT_STATUS_ADC_ERR_SHIFT          5
#define ALERT_STATUS_ADC_ERR_MASK           (0x01 << 5)
#define ALERT_STATUS_CH_ALERT_A_SHIFT       8
#define ALERT_STATUS_CH_ALERT_A_MASK        (0x01 << 8)
#define ALERT_STATUS_CH_ALERT_B_SHIFT       9
#define ALERT_STATUS_CH_ALERT_B_MASK        (0x01 << 9)
#define ALERT_STATUS_CH_ALERT_C_SHIFT       10
#define ALERT_STATUS_CH_ALERT_C_MASK        (0x01 << 10)
#define ALERT_STATUS_CH_ALERT_D_SHIFT       11
#define ALERT_STATUS_CH_ALERT_D_MASK        (0x01 << 11)
#define ALERT_STATUS_HART_ALERT_A_SHIFT     12
#define ALERT_STATUS_HART_ALERT_B_SHIFT     13
#define ALERT_STATUS_HART_ALERT_C_SHIFT     14
#define ALERT_STATUS_HART_ALERT_D_SHIFT     15

// LIVE_STATUS (0x40) - Real-time status (read-only, not latched)
#define REG_LIVE_STATUS             0x40

// LIVE_STATUS bit fields (corrected per ADI driver)
#define LIVE_STATUS_SUPPLY_STATUS_SHIFT     0
#define LIVE_STATUS_SUPPLY_STATUS_MASK      (0x01 << 0)
#define LIVE_STATUS_ADC_BUSY_SHIFT          1
#define LIVE_STATUS_ADC_BUSY_MASK           (0x01 << 1)
#define LIVE_STATUS_ADC_DATA_RDY_SHIFT      2
#define LIVE_STATUS_ADC_DATA_RDY_MASK       (0x01 << 2)
#define LIVE_STATUS_TEMP_ALERT_SHIFT        3
#define LIVE_STATUS_TEMP_ALERT_MASK         (0x01 << 3)
#define LIVE_STATUS_DIN_STATUS_A_SHIFT      4
#define LIVE_STATUS_DIN_STATUS_B_SHIFT      5
#define LIVE_STATUS_DIN_STATUS_C_SHIFT      6
#define LIVE_STATUS_DIN_STATUS_D_SHIFT      7

// SUPPLY_ALERT_STATUS (0x57)
#define REG_SUPPLY_ALERT_STATUS     0x57

// Per-channel alert status (0x58 + ch)
#define REG_CHANNEL_ALERT_STATUS_BASE   0x58
#define AD74416H_REG_CHANNEL_ALERT_STATUS(ch)  (REG_CHANNEL_ALERT_STATUS_BASE + (ch))

// ALERT_MASK (0x5C)
#define REG_ALERT_MASK              0x5C

// SUPPLY_ALERT_MASK (0x5D)
#define REG_SUPPLY_ALERT_MASK       0x5D

// Per-channel alert mask (0x5E + ch)
#define REG_CHANNEL_ALERT_MASK_BASE     0x5E
#define AD74416H_REG_CHANNEL_ALERT_MASK(ch)    (REG_CHANNEL_ALERT_MASK_BASE + (ch))

// HART Alert Status (ch = 0..3) - address 0x80 + ch*16
#define AD74416H_REG_HART_ALERT_STATUS(ch) (0x80 + (ch) * 16)

// Readback / Burst
#define REG_READ_SELECT             0x6E
#define REG_BURST_READ_SEL          0x6F

// Miscellaneous Global Registers
#define REG_THERM_RST               0x73
#define THERM_RST_EN_SHIFT          0
#define THERM_RST_EN_MASK           (0x01 << THERM_RST_EN_SHIFT)

#define REG_CMD_KEY                 0x74
#define CMD_KEY_RESET_1             0x15FA  // First software reset key
#define CMD_KEY_RESET_2             0xAF51  // Second software reset key

#define REG_BROADCAST_CMD_KEY       0x75

#define REG_SCRATCH                 0x76    // Read/write scratch register (SPI comm test)
#define REG_SILICON_REV             0x7B    // Read-only silicon revision (corrected from 0x77)
#define REG_SILICON_ID0             0x7D
#define REG_SILICON_ID1             0x7E

// =============================================================================
// Enumerations
// =============================================================================

/**
 * @brief Channel function codes for CH_FUNC_SETUP[3:0]
 */
typedef enum {
    CH_FUNC_HIGH_IMP            = 0,    // High impedance (safe switching state)
    CH_FUNC_VOUT                = 1,    // Voltage output
    CH_FUNC_IOUT                = 2,    // Current output (external power)
    CH_FUNC_VIN                 = 3,    // Voltage input (ADC)
    CH_FUNC_IIN_EXT_PWR         = 4,    // Current input, externally powered
    CH_FUNC_IIN_LOOP_PWR        = 5,    // Current input, loop powered
    CH_FUNC_RES_MEAS            = 7,    // Resistance measurement (RTD)
    CH_FUNC_DIN_LOGIC           = 8,    // Digital input (logic level)
    CH_FUNC_DIN_LOOP            = 9,    // Digital input (loop powered)
    CH_FUNC_IOUT_HART           = 10,   // Current output with HART
    CH_FUNC_IIN_EXT_PWR_HART    = 11,   // Current input, ext pwr, with HART
    CH_FUNC_IIN_LOOP_PWR_HART   = 12,   // Current input, loop pwr, with HART
} ChannelFunction;

/**
 * @brief ADC conversion range codes for ADC_CONFIG[5:3] CONV_RANGE
 */
typedef enum {
    ADC_RNG_0_12V               = 0,    //  0 V to +12 V
    ADC_RNG_NEG12_12V           = 1,    // -12 V to +12 V
    ADC_RNG_NEG0_3125_0_3125V   = 2,    // -312.5 mV to +312.5 mV
    ADC_RNG_NEG0_3125_0V        = 3,    // -0.3125 V to 0 V
    ADC_RNG_0_0_3125V           = 4,    //  0 V to +0.3125 V
    ADC_RNG_0_0_625V            = 5,    //  0 V to +0.625 V
    ADC_RNG_NEG104MV_104MV      = 6,    // -104 mV to +104 mV
    ADC_RNG_NEG2_5_2_5V         = 7,    // -2.5 V to +2.5 V
} AdcRange;

/**
 * @brief ADC conversion rate codes for ADC_CONFIG[9:6] CONV_RATE
 */
typedef enum {
    ADC_RATE_10SPS_H            = 0,    //  10 SPS (High rejection)
    ADC_RATE_20SPS              = 1,    //  20 SPS
    ADC_RATE_20SPS_H            = 3,    //  20 SPS (High rejection)
    ADC_RATE_200SPS_H1          = 4,    // 200 SPS (High rejection variant 1)
    ADC_RATE_200SPS_H           = 6,    // 200 SPS (High rejection)
    ADC_RATE_1_2KSPS            = 8,    // 1.2 kSPS
    ADC_RATE_1_2KSPS_H          = 9,    // 1.2 kSPS (High rejection)
    ADC_RATE_4_8KSPS            = 12,   // 4.8 kSPS
    ADC_RATE_9_6KSPS            = 13,   // 9.6 kSPS
} AdcRate;

/**
 * @brief ADC conversion multiplexer codes for ADC_CONFIG[2:0] CONV_MUX
 */
typedef enum {
    ADC_MUX_LF_TO_AGND          = 0,   // LF terminal to AGND
    ADC_MUX_HF_TO_LF            = 1,   // HF terminal to LF terminal (differential)
    ADC_MUX_VSENSEN_TO_AGND     = 2,   // VSENSE- to AGND
    ADC_MUX_LF_TO_VSENSEN       = 3,   // LF to VSENSE- (4-wire sense)
    ADC_MUX_AGND_TO_AGND        = 4,   // AGND to AGND (self-test / offset)
} AdcConvMux;

/**
 * @brief GPIO_SELECT modes for GPIO_CONFIGn[2:0]
 */
typedef enum {
    GPIO_SEL_HIGH_IMP       = 0,   // High impedance (input buffer off)
    GPIO_SEL_OUTPUT         = 1,   // Logic output (GPO_DATA sets level) + input
    GPIO_SEL_INPUT          = 2,   // Input only (output high-Z)
    GPIO_SEL_DIN_OUT        = 3,   // DIN comparator output
    GPIO_SEL_DO_EXT         = 4,   // DO external source
} GpioSelect;

/**
 * @brief ADC conversion sequence control codes for ADC_CONV_CTRL[9:8] CONV_SEQ
 */
typedef enum {
    ADC_CONV_SEQ_IDLE           = 0,   // Stop / idle
    ADC_CONV_SEQ_START_SINGLE   = 1,   // Start single conversion
    ADC_CONV_SEQ_START_CONT     = 2,   // Start continuous conversion
    ADC_CONV_SEQ_STOP           = 3,   // Stop continuous conversion
} AdcConvSeq;
