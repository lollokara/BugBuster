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
// ADC_CONFIG register bit fields (base 0x02)
// -----------------------------------------------------------------------------
#define ADC_CONFIG_CONV_MUX_SHIFT       0
#define ADC_CONFIG_CONV_MUX_MASK        (0x07 << ADC_CONFIG_CONV_MUX_SHIFT)

#define ADC_CONFIG_CONV_RANGE_SHIFT     3
#define ADC_CONFIG_CONV_RANGE_MASK      (0x07 << ADC_CONFIG_CONV_RANGE_SHIFT)

#define ADC_CONFIG_CONV_RATE_SHIFT      6
#define ADC_CONFIG_CONV_RATE_MASK       (0x0F << ADC_CONFIG_CONV_RATE_SHIFT)

// -----------------------------------------------------------------------------
// DIN_CONFIG0 register bit fields (base 0x03)
// -----------------------------------------------------------------------------
#define DIN_CONFIG0_DEBOUNCE_TIME_SHIFT     0
#define DIN_CONFIG0_DEBOUNCE_TIME_MASK      (0x1F << DIN_CONFIG0_DEBOUNCE_TIME_SHIFT)

#define DIN_CONFIG0_DEBOUNCE_MODE_SHIFT     5
#define DIN_CONFIG0_DEBOUNCE_MODE_MASK      (0x01 << DIN_CONFIG0_DEBOUNCE_MODE_SHIFT)

#define DIN_CONFIG0_DIN_INV_COMP_OUT_SHIFT  6
#define DIN_CONFIG0_DIN_INV_COMP_OUT_MASK   (0x01 << DIN_CONFIG0_DIN_INV_COMP_OUT_SHIFT)

#define DIN_CONFIG0_COMPARATOR_EN_SHIFT     7
#define DIN_CONFIG0_COMPARATOR_EN_MASK      (0x01 << DIN_CONFIG0_COMPARATOR_EN_SHIFT)

#define DIN_CONFIG0_DIN_SINK_SHIFT          8
#define DIN_CONFIG0_DIN_SINK_MASK           (0x1F << DIN_CONFIG0_DIN_SINK_SHIFT)

#define DIN_CONFIG0_DIN_SINK_RANGE_SHIFT    13
#define DIN_CONFIG0_DIN_SINK_RANGE_MASK     (0x01 << DIN_CONFIG0_DIN_SINK_RANGE_SHIFT)

#define DIN_CONFIG0_COUNT_EN_SHIFT          14
#define DIN_CONFIG0_COUNT_EN_MASK           (0x01 << DIN_CONFIG0_COUNT_EN_SHIFT)

#define DIN_CONFIG0_DIN_OC_DET_EN_SHIFT     15
#define DIN_CONFIG0_DIN_OC_DET_EN_MASK      (0x01 << DIN_CONFIG0_DIN_OC_DET_EN_SHIFT)

// Note: DIN_SC_DET_EN is bit 16 - requires 17-bit field; stored as bit 0 of
// upper byte in 32-bit extended register. In 16-bit register context, use
// extended config if device supports it; otherwise see datasheet for mapping.
#define DIN_CONFIG0_DIN_SC_DET_EN_SHIFT     16  // Extended bit

// -----------------------------------------------------------------------------
// DIN_CONFIG1 register bit fields (base 0x04)
// -----------------------------------------------------------------------------
#define DIN_CONFIG1_COMP_THRESH_SHIFT       0
#define DIN_CONFIG1_COMP_THRESH_MASK        (0x7F << DIN_CONFIG1_COMP_THRESH_SHIFT)

#define DIN_CONFIG1_DIN_THRESH_MODE_SHIFT   7
#define DIN_CONFIG1_DIN_THRESH_MODE_MASK    (0x01 << DIN_CONFIG1_DIN_THRESH_MODE_SHIFT)

#define DIN_CONFIG1_DIN_INPUT_SELECT_SHIFT  8
#define DIN_CONFIG1_DIN_INPUT_SELECT_MASK   (0x01 << DIN_CONFIG1_DIN_INPUT_SELECT_SHIFT)

// -----------------------------------------------------------------------------
// OUTPUT_CONFIG register bit fields (base 0x05)
// -----------------------------------------------------------------------------
#define OUTPUT_CONFIG_SLEW_EN_SHIFT         0
#define OUTPUT_CONFIG_SLEW_EN_MASK          (0x03 << OUTPUT_CONFIG_SLEW_EN_SHIFT)

#define OUTPUT_CONFIG_SLEW_RANGE_SHIFT      2
#define OUTPUT_CONFIG_SLEW_RANGE_MASK       (0x03 << OUTPUT_CONFIG_SLEW_RANGE_SHIFT)

#define OUTPUT_CONFIG_I_LIMIT_SHIFT         4
#define OUTPUT_CONFIG_I_LIMIT_MASK          (0x01 << OUTPUT_CONFIG_I_LIMIT_SHIFT)

#define OUTPUT_CONFIG_VOUT_RANGE_SHIFT      5
#define OUTPUT_CONFIG_VOUT_RANGE_MASK       (0x01 << OUTPUT_CONFIG_VOUT_RANGE_SHIFT)

#define OUTPUT_CONFIG_VOUT_4W_EN_SHIFT      6
#define OUTPUT_CONFIG_VOUT_4W_EN_MASK       (0x01 << OUTPUT_CONFIG_VOUT_4W_EN_SHIFT)

#define OUTPUT_CONFIG_AVDD_SELECT_SHIFT     7
#define OUTPUT_CONFIG_AVDD_SELECT_MASK      (0x03 << OUTPUT_CONFIG_AVDD_SELECT_SHIFT)

// -----------------------------------------------------------------------------
// RTD_CONFIG register bit fields (base 0x06)
// -----------------------------------------------------------------------------
#define RTD_CONFIG_RTD_MODE_SEL_SHIFT       0
#define RTD_CONFIG_RTD_MODE_SEL_MASK        (0x01 << RTD_CONFIG_RTD_MODE_SEL_SHIFT)

#define RTD_CONFIG_RTD_EXC_SWAP_SHIFT       1
#define RTD_CONFIG_RTD_EXC_SWAP_MASK        (0x01 << RTD_CONFIG_RTD_EXC_SWAP_SHIFT)

#define RTD_CONFIG_RTD_CURRENT_SHIFT        2
#define RTD_CONFIG_RTD_CURRENT_MASK         (0x03 << RTD_CONFIG_RTD_CURRENT_SHIFT)

#define RTD_CONFIG_RTD_ADC_REF_SHIFT        4
#define RTD_CONFIG_RTD_ADC_REF_MASK         (0x01 << RTD_CONFIG_RTD_ADC_REF_SHIFT)

// -----------------------------------------------------------------------------
// DO_EXT_CONFIG register bit fields (base 0x08)
// -----------------------------------------------------------------------------
#define DO_EXT_CONFIG_DO_T2_SHIFT           0
#define DO_EXT_CONFIG_DO_T2_MASK            (0xFF << DO_EXT_CONFIG_DO_T2_SHIFT)   // [7:0]

#define DO_EXT_CONFIG_DO_T1_SHIFT           8
#define DO_EXT_CONFIG_DO_T1_MASK            (0x0F << DO_EXT_CONFIG_DO_T1_SHIFT)   // [11:8]

#define DO_EXT_CONFIG_DO_DATA_SHIFT         12
#define DO_EXT_CONFIG_DO_DATA_MASK          (0x01 << DO_EXT_CONFIG_DO_DATA_SHIFT)

#define DO_EXT_CONFIG_DO_SRC_SEL_SHIFT      13
#define DO_EXT_CONFIG_DO_SRC_SEL_MASK       (0x01 << DO_EXT_CONFIG_DO_SRC_SEL_SHIFT)

#define DO_EXT_CONFIG_DO_MODE_SHIFT         14
#define DO_EXT_CONFIG_DO_MODE_MASK          (0x03 << DO_EXT_CONFIG_DO_MODE_SHIFT) // [15:14]

// =============================================================================
// Global Register Addresses
// =============================================================================
#define REG_NOP                     0x00

// GPIO Configuration (6 GPIOs, addresses 0x32..0x37)
#define REG_GPIO_CONFIG0            0x32
#define REG_GPIO_CONFIG1            0x33
#define REG_GPIO_CONFIG2            0x34
#define REG_GPIO_CONFIG3            0x35
#define REG_GPIO_CONFIG4            0x36
#define REG_GPIO_CONFIG5            0x37

// GPIO_CONFIGn bit fields
#define GPIO_CONFIG_GPIO_SELECT_SHIFT   0
#define GPIO_CONFIG_GPIO_SELECT_MASK    (0x07 << GPIO_CONFIG_GPIO_SELECT_SHIFT)

#define GPIO_CONFIG_GPO_DATA_SHIFT      3
#define GPIO_CONFIG_GPO_DATA_MASK       (0x01 << GPIO_CONFIG_GPO_DATA_SHIFT)

#define GPIO_CONFIG_GP_WK_PD_EN_SHIFT   4
#define GPIO_CONFIG_GP_WK_PD_EN_MASK    (0x01 << GPIO_CONFIG_GP_WK_PD_EN_SHIFT)

#define GPIO_CONFIG_GPI_DATA_SHIFT      5
#define GPIO_CONFIG_GPI_DATA_MASK       (0x01 << GPIO_CONFIG_GPI_DATA_SHIFT)

// Power Optimisation
#define REG_PWR_OPTIM_CONFIG        0x38
#define PWR_OPTIM_REF_EN_SHIFT      13
#define PWR_OPTIM_REF_EN_MASK       (0x01 << PWR_OPTIM_REF_EN_SHIFT)   // Internal reference enable

// ADC Conversion Control
#define REG_ADC_CONV_CTRL           0x39
#define ADC_CONV_CTRL_CONV_SEQ_SHIFT        0
#define ADC_CONV_CTRL_CONV_SEQ_MASK         (0x03 << ADC_CONV_CTRL_CONV_SEQ_SHIFT)
#define ADC_CONV_CTRL_CONV_A_EN_SHIFT       2
#define ADC_CONV_CTRL_CONV_A_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_A_EN_SHIFT)
#define ADC_CONV_CTRL_CONV_B_EN_SHIFT       3
#define ADC_CONV_CTRL_CONV_B_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_B_EN_SHIFT)
#define ADC_CONV_CTRL_CONV_C_EN_SHIFT       4
#define ADC_CONV_CTRL_CONV_C_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_C_EN_SHIFT)
#define ADC_CONV_CTRL_CONV_D_EN_SHIFT       5
#define ADC_CONV_CTRL_CONV_D_EN_MASK        (0x01 << ADC_CONV_CTRL_CONV_D_EN_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN0_SHIFT        6
#define ADC_CONV_CTRL_DIAG_EN0_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN0_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN1_SHIFT        7
#define ADC_CONV_CTRL_DIAG_EN1_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN1_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN2_SHIFT        8
#define ADC_CONV_CTRL_DIAG_EN2_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN2_SHIFT)
#define ADC_CONV_CTRL_DIAG_EN3_SHIFT        9
#define ADC_CONV_CTRL_DIAG_EN3_MASK         (0x01 << ADC_CONV_CTRL_DIAG_EN3_SHIFT)
#define ADC_CONV_CTRL_ADC_RDY_CTRL_SHIFT    10
#define ADC_CONV_CTRL_ADC_RDY_CTRL_MASK     (0x01 << ADC_CONV_CTRL_ADC_RDY_CTRL_SHIFT)
#define ADC_CONV_CTRL_CONV_RATE_DIAG_SHIFT  11
#define ADC_CONV_CTRL_CONV_RATE_DIAG_MASK   (0x07 << ADC_CONV_CTRL_CONV_RATE_DIAG_SHIFT)

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

// ADC_RESULT_UPR bit fields
#define ADC_RESULT_UPR_CONV_SEQ_COUNT_SHIFT 14
#define ADC_RESULT_UPR_CONV_SEQ_COUNT_MASK  (0x03 << ADC_RESULT_UPR_CONV_SEQ_COUNT_SHIFT)
#define ADC_RESULT_UPR_CONV_RES_SHIFT       0
#define ADC_RESULT_UPR_CONV_RES_MASK        (0xFF << ADC_RESULT_UPR_CONV_RES_SHIFT)  // bits [23:16] of result

// ADC Diagnostic Results
#define REG_ADC_DIAG_RESULT_BASE    0x49    // Addresses: 0x49, 0x4A, 0x4B, 0x4C
#define AD74416H_REG_ADC_DIAG_RESULT(diag) (REG_ADC_DIAG_RESULT_BASE + (diag))

// Last ADC Results (single register, not per-channel upper/lower split)
#define REG_LAST_ADC_RESULT             0x4E

// Digital Input Counters (32-bit, split across two 16-bit registers)
// Per ADI driver: DIN_COUNTER(x) = 0x50 + (x * 2)
// Lower 16 bits: 0x50, 0x52, 0x54, 0x56
// Upper 16 bits: 0x51, 0x53, 0x55, 0x57
#define REG_DIN_COUNTER_BASE        0x50
#define REG_DIN_COUNTER_UPR_BASE    0x51

#define AD74416H_REG_DIN_COUNTER(ch)     (REG_DIN_COUNTER_BASE     + (ch) * 2)
#define AD74416H_REG_DIN_COUNTER_UPR(ch) (REG_DIN_COUNTER_UPR_BASE + (ch) * 2)

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
    ADC_RNG_NEG2_5_2_5V         = 2,    // -2.5 V to +2.5 V
    ADC_RNG_NEG0_3125_0V        = 3,    // -0.3125 V to 0 V
    ADC_RNG_0_0_3125V           = 4,    //  0 V to +0.3125 V
    ADC_RNG_0_0_625V            = 5,    //  0 V to +0.625 V
    ADC_RNG_NEG0_3125_0_3125V   = 6,    // -0.3125 V to +0.3125 V
    ADC_RNG_NEG104MV_104MV      = 7,    // -104 mV to +104 mV
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
 * @brief ADC conversion sequence control codes for ADC_CONV_CTRL[1:0] CONV_SEQ
 */
typedef enum {
    ADC_CONV_SEQ_IDLE           = 0,   // Stop / idle
    ADC_CONV_SEQ_START_SINGLE   = 1,   // Start single conversion
    ADC_CONV_SEQ_START_CONT     = 2,   // Start continuous conversion
    ADC_CONV_SEQ_STOP           = 3,   // Stop continuous conversion
} AdcConvSeq;
