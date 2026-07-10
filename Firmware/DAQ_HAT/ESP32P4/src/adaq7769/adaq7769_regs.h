#pragma once

// =============================================================================
// adaq7769_regs.h — Complete ADAQ7769-1 register and bit-field map
//
// Source: ADAQ7769-1 Data Sheet, Rev. 0 (7/2025), "Register Summary" (Table 42)
//         and "Register Details" (Tables 43..79).
//
// All registers are 8 bits wide EXCEPT ADC_DATA (0x2C) which is 24 bits.
// Reset values reflect the datasheet power-on defaults.
// =============================================================================

#include <stdint.h>

// -----------------------------------------------------------------------------
// SPI instruction byte (datasheet "SPI Reading and Writing", Fig 159):
//   bit7      = FS    frame-start, ACTIVE LOW -> must be 0 to begin a transaction
//   bit6      = R/W   (1 = read, 0 = write)
//   bits[5:0] = register address
// e.g. read ADC_DATA (0x2C) -> 0b0_1_101100 = 0x6C (matches the datasheet CRC
// example). Read = 0x40 | addr, Write = 0x00 | addr. NOTE: the read bit is bit6
// (0x40), NOT bit7 — putting R/W in bit7 leaves FS high so the device ignores
// the frame entirely and every read returns 0x00.
// -----------------------------------------------------------------------------
#define ADAQ_SPI_READ_BIT          0x40u
#define ADAQ_SPI_ADDR_MASK         0x3Fu
#define ADAQ_INSTR_WRITE(addr)     ((uint8_t)((addr) & ADAQ_SPI_ADDR_MASK))
#define ADAQ_INSTR_READ(addr)      ((uint8_t)(ADAQ_SPI_READ_BIT | ((addr) & ADAQ_SPI_ADDR_MASK)))

// Special continuous-read-mode key bytes written on SDI (between DRDY pulses).
#define ADAQ_CONTREAD_EXIT_KEY     0x6Cu   // leave continuous read mode
#define ADAQ_CONTREAD_RESET_KEY    0xADu   // soft reset from continuous read mode

// -----------------------------------------------------------------------------
// Register addresses
// -----------------------------------------------------------------------------
#define ADAQ_REG_CHIP_TYPE         0x03u   // R   reset 0x07
#define ADAQ_REG_PRODUCT_ID_L      0x04u   // R   reset 0x01
#define ADAQ_REG_PRODUCT_ID_H      0x05u   // R   reset 0x00
#define ADAQ_REG_CHIP_GRADE        0x06u   // R   reset 0x00
#define ADAQ_REG_SCRATCH_PAD       0x0Au   // R/W reset 0x00
#define ADAQ_REG_VENDOR_L          0x0Cu   // R   reset 0x56
#define ADAQ_REG_VENDOR_H          0x0Du   // R   reset 0x04
#define ADAQ_REG_INTERFACE_FORMAT  0x14u   // R/W reset 0x00
#define ADAQ_REG_POWER_CLOCK       0x15u   // R/W reset 0x00
#define ADAQ_REG_ANALOG            0x16u   // R/W reset 0x00
#define ADAQ_REG_CONVERSION        0x18u   // R/W reset 0x00
#define ADAQ_REG_DIGITAL_FILTER    0x19u   // R/W reset 0x00
#define ADAQ_REG_SINC3_DEC_MSB     0x1Au   // R/W reset 0x00
#define ADAQ_REG_SINC3_DEC_LSB     0x1Bu   // R/W reset 0x00
#define ADAQ_REG_DUTY_CYCLE_RATIO  0x1Cu   // R/W reset 0x00
#define ADAQ_REG_SYNC_RESET        0x1Du   // R/W reset 0x80
#define ADAQ_REG_GPIO_CONTROL      0x1Eu   // R/W reset 0x00
#define ADAQ_REG_GPIO_WRITE        0x1Fu   // R/W reset 0x00
#define ADAQ_REG_GPIO_READ         0x20u   // R   reset 0x00
#define ADAQ_REG_OFFSET_HI         0x21u   // R/W reset 0x00
#define ADAQ_REG_OFFSET_MID        0x22u   // R/W reset 0x00
#define ADAQ_REG_OFFSET_LO         0x23u   // R/W reset 0x00
#define ADAQ_REG_GAIN_HI           0x24u   // R/W reset 0x00
#define ADAQ_REG_GAIN_MID          0x25u   // R/W reset 0x00
#define ADAQ_REG_GAIN_LO           0x26u   // R/W reset 0x00
#define ADAQ_REG_SPI_DIAG_ENABLE   0x28u   // R/W reset 0x10
#define ADAQ_REG_ADC_DIAG_ENABLE   0x29u   // R/W reset 0x07
#define ADAQ_REG_DIG_DIAG_ENABLE   0x2Au   // R/W reset 0x0D
#define ADAQ_REG_ADC_DATA          0x2Cu   // R   24-bit conversion result
#define ADAQ_REG_MASTER_STATUS     0x2Du   // R   8-bit status header
#define ADAQ_REG_SPI_DIAG_STATUS   0x2Eu   // R/W1C
#define ADAQ_REG_ADC_DIAG_STATUS   0x2Fu   // R
#define ADAQ_REG_DIG_DIAG_STATUS   0x30u   // R
#define ADAQ_REG_MCLK_COUNTER      0x31u   // R
#define ADAQ_REG_COEFF_CONTROL     0x32u   // R/W
#define ADAQ_REG_COEFF_DATA        0x33u   // R/W 24-bit
#define ADAQ_REG_ACCESS_KEY        0x34u   // R/W

// Identity constants for probe / verification.
#define ADAQ_CHIP_TYPE_CLASS_ADC   0x07u   // CHIP_TYPE[3:0] == ADC
#define ADAQ_PRODUCT_ID_LSB        0x01u
#define ADAQ_PRODUCT_ID_MSB        0x00u
#define ADAQ_VENDOR_ID             0x0456u // VID (VENDOR_H:VENDOR_L)

// -----------------------------------------------------------------------------
// 0x14 INTERFACE_FORMAT
// -----------------------------------------------------------------------------
#define ADAQ_IF_LV_BOOST           (1u << 7)  // boost DOUT drive (1.8V IOVDD)
#define ADAQ_IF_EN_SPI_CRC         (1u << 6)  // CRC on all SPI transfers
#define ADAQ_IF_CRC_TYPE_XOR       (1u << 5)  // 1=XOR (reads only), 0=CRC-8 poly
#define ADAQ_IF_STATUS_EN          (1u << 4)  // append 8-bit status after data
#define ADAQ_IF_CONVLEN_16         (1u << 3)  // 1=16 MSBs only, 0=full 24 bits
#define ADAQ_IF_EN_RDY_DOUT        (1u << 2)  // RDY on DOUT/RDY pin
#define ADAQ_IF_EN_CONT_READ       (1u << 0)  // continuous read mode

// -----------------------------------------------------------------------------
// 0x15 POWER_CLOCK
// -----------------------------------------------------------------------------
#define ADAQ_PC_CLOCK_SEL_SHIFT    6
#define ADAQ_PC_CLOCK_SEL_MASK     (3u << 6)
#define   ADAQ_CLKSEL_CMOS         0u   // CMOS on XTAL2_MCLK
#define   ADAQ_CLKSEL_XTAL         1u   // external crystal
#define   ADAQ_CLKSEL_LVDS         2u   // LVDS (SPI mode only)
#define   ADAQ_CLKSEL_INT_RC       3u   // internal coarse RC (diagnostics)

#define ADAQ_PC_MCLK_DIV_SHIFT     4
#define ADAQ_PC_MCLK_DIV_MASK      (3u << 4)
#define   ADAQ_MCLK_DIV_16         0u   // fMOD = MCLK/16
#define   ADAQ_MCLK_DIV_8          1u   // fMOD = MCLK/8
#define   ADAQ_MCLK_DIV_4          2u   // fMOD = MCLK/4
#define   ADAQ_MCLK_DIV_2          3u   // fMOD = MCLK/2

#define ADAQ_PC_ADC_POWER_DOWN     (1u << 3)  // write 0x08 alone to power down

#define ADAQ_PC_ADC_MODE_SHIFT     0
#define ADAQ_PC_ADC_MODE_MASK      (3u << 0)
#define   ADAQ_ADC_MODE_LOW        0u   // low-power
#define   ADAQ_ADC_MODE_MEDIAN     1u   // median-power
#define   ADAQ_ADC_MODE_FAST       3u   // fast-power

// -----------------------------------------------------------------------------
// 0x16 ANALOG  (reference buffers + linearity boost)
// -----------------------------------------------------------------------------
#define ADAQ_AN_REF_BUF_POS_SHIFT  6
#define ADAQ_AN_REF_BUF_POS_MASK   (3u << 6)
#define ADAQ_AN_REF_BUF_NEG_SHIFT  4
#define ADAQ_AN_REF_BUF_NEG_MASK   (3u << 4)
#define   ADAQ_REFBUF_PRECHARGE    0u
#define   ADAQ_REFBUF_UNBUFFERED   1u
#define   ADAQ_REFBUF_FULL         2u
#define ADAQ_AN_LIN_BOOST_A_OFF    (1u << 1)  // 1 = buffer A disabled
#define ADAQ_AN_LIN_BOOST_B_OFF    (1u << 0)  // 1 = buffer B disabled

// -----------------------------------------------------------------------------
// 0x18 CONVERSION  (diagnostic mux + conversion mode)
// -----------------------------------------------------------------------------
#define ADAQ_CV_DIAG_MUX_SHIFT     4
#define ADAQ_CV_DIAG_MUX_MASK      (0xFu << 4)
#define   ADAQ_DIAGMUX_TEMP        0x0u
#define   ADAQ_DIAGMUX_SHORT       0x8u   // input short (zero check)
#define   ADAQ_DIAGMUX_POS_FS      0x9u   // positive full scale
#define   ADAQ_DIAGMUX_NEG_FS      0xAu   // negative full scale
#define ADAQ_CV_CONV_DIAG_SELECT   (1u << 3)  // 1 = convert diagnostic mux
#define ADAQ_CV_CONV_MODE_SHIFT    0
#define ADAQ_CV_CONV_MODE_MASK     (7u << 0)
#define   ADAQ_CONVMODE_CONTINUOUS 0u
#define   ADAQ_CONVMODE_ONESHOT    1u   // continuous one-shot (SYNC_IN gated)
#define   ADAQ_CONVMODE_SINGLE     2u   // single-conversion standby
#define   ADAQ_CONVMODE_DUTYCYCLE  3u   // duty-cycled standby
#define   ADAQ_CONVMODE_STANDBY    4u   // 4..7 all = standby

// -----------------------------------------------------------------------------
// 0x19 DIGITAL_FILTER
// -----------------------------------------------------------------------------
#define ADAQ_DF_EN_60HZ_REJ        (1u << 7)  // Sinc3 50+60 Hz reject
#define ADAQ_DF_FILTER_SHIFT       4
#define ADAQ_DF_FILTER_MASK        (7u << 4)
#define   ADAQ_FILTER_SINC5        0u   // Sinc5, dec x32..x1024 (DEC_RATE)
#define   ADAQ_FILTER_SINC5_X8     1u   // Sinc5 x8 only, 1.024MSPS, 16-bit out
#define   ADAQ_FILTER_SINC5_X16    2u   // Sinc5 x16 only, 512kSPS
#define   ADAQ_FILTER_SINC3        3u   // Sinc3, programmable dec (0x1A/0x1B)
#define   ADAQ_FILTER_WIDEBAND     4u   // wideband low-ripple FIR, dec x32..x1024
#define ADAQ_DF_DEC_RATE_SHIFT     0
#define ADAQ_DF_DEC_RATE_MASK      (7u << 0)
#define   ADAQ_DEC_X32             0u
#define   ADAQ_DEC_X64             1u
#define   ADAQ_DEC_X128            2u
#define   ADAQ_DEC_X256            3u
#define   ADAQ_DEC_X512            4u
#define   ADAQ_DEC_X1024           5u   // 5,6,7 all = x1024

// -----------------------------------------------------------------------------
// 0x1A/0x1B SINC3 decimation. Value written = (DEC_RATE/32) - 1, 13-bit.
// -----------------------------------------------------------------------------
#define ADAQ_SINC3_DEC_MSB_MASK    0x1Fu  // bits [12:8]

// -----------------------------------------------------------------------------
// 0x1D SYNC_RESET
// -----------------------------------------------------------------------------
#define ADAQ_SR_SPI_START          (1u << 7)  // write 0 -> SYNC_OUT pulse
#define ADAQ_SR_SYNC_OUT_POS_EDGE  (1u << 6)  // SYNC_OUT driven on +MCLK edge
#define ADAQ_SR_EN_GPIO_START      (1u << 3)  // GPIO3 becomes START input
#define ADAQ_SR_SPI_RESET_MASK     (3u << 0)
#define   ADAQ_SPI_RESET_ARM       0x3u  // write 11 then ...
#define   ADAQ_SPI_RESET_FIRE      0x2u  // ... 10 to trigger reset

// -----------------------------------------------------------------------------
// 0x1E GPIO_CONTROL / 0x1F GPIO_WRITE / 0x20 GPIO_READ
// On this board GPIO0/1/2 -> PGA GAIN0/1/2 and GPIO3 -> EN_PGA (see board doc).
// -----------------------------------------------------------------------------
#define ADAQ_GC_UGPIO_EN           (1u << 7)  // must be 1 to change GPIO config
#define ADAQ_GC_GPIO2_OPEN_DRAIN   (1u << 6)
#define ADAQ_GC_GPIO1_OPEN_DRAIN   (1u << 5)
#define ADAQ_GC_GPIO0_OPEN_DRAIN   (1u << 4)
#define ADAQ_GC_GPIO3_OP_EN        (1u << 3)  // 1 = output
#define ADAQ_GC_GPIO2_OP_EN        (1u << 2)
#define ADAQ_GC_GPIO1_OP_EN        (1u << 1)
#define ADAQ_GC_GPIO0_OP_EN        (1u << 0)

#define ADAQ_GPIO0_BIT             (1u << 0)
#define ADAQ_GPIO1_BIT             (1u << 1)
#define ADAQ_GPIO2_BIT             (1u << 2)
#define ADAQ_GPIO3_BIT             (1u << 3)

// -----------------------------------------------------------------------------
// 0x28 SPI_DIAG_ENABLE
// -----------------------------------------------------------------------------
#define ADAQ_SPIDIAG_EN_IGNORE     (1u << 4)
#define ADAQ_SPIDIAG_EN_CLK_CNT    (1u << 3)
#define ADAQ_SPIDIAG_EN_RD         (1u << 2)
#define ADAQ_SPIDIAG_EN_WR         (1u << 1)

// -----------------------------------------------------------------------------
// 0x29 ADC_DIAG_ENABLE
// -----------------------------------------------------------------------------
#define ADAQ_ADCDIAG_EN_DLDO_PSM   (1u << 5)
#define ADAQ_ADCDIAG_EN_ALDO_PSM   (1u << 4)
#define ADAQ_ADCDIAG_EN_REF_DET    (1u << 3)
#define ADAQ_ADCDIAG_EN_FILT_SAT   (1u << 2)
#define ADAQ_ADCDIAG_EN_FILT_NSET  (1u << 1)
#define ADAQ_ADCDIAG_EN_EXT_CLK    (1u << 0)

// -----------------------------------------------------------------------------
// 0x2A DIG_DIAG_ENABLE
// -----------------------------------------------------------------------------
#define ADAQ_DIGDIAG_EN_MEMMAP_CRC (1u << 4)
#define ADAQ_DIGDIAG_EN_RAM_CRC    (1u << 3)
#define ADAQ_DIGDIAG_EN_FUSE_CRC   (1u << 2)
#define ADAQ_DIGDIAG_EN_FREQ_COUNT (1u << 0)

// -----------------------------------------------------------------------------
// 0x2D MASTER_STATUS  (also the 8-bit status header appended after a sample)
// -----------------------------------------------------------------------------
#define ADAQ_ST_MASTER_ERROR       (1u << 7)
#define ADAQ_ST_ADC_ERROR          (1u << 6)
#define ADAQ_ST_DIG_ERROR          (1u << 5)
#define ADAQ_ST_ERR_EXT_CLK_QUAL   (1u << 4)
#define ADAQ_ST_FILT_SATURATED     (1u << 3)
#define ADAQ_ST_FILT_NOT_SETTLED   (1u << 2)
#define ADAQ_ST_SPI_ERROR          (1u << 1)
#define ADAQ_ST_POR_FLAG           (1u << 0)

// -----------------------------------------------------------------------------
// 0x2E SPI_DIAG_STATUS (W1C)
// -----------------------------------------------------------------------------
#define ADAQ_SPIERR_IGNORE         (1u << 4)
#define ADAQ_SPIERR_CLK_CNT        (1u << 3)
#define ADAQ_SPIERR_RD             (1u << 2)
#define ADAQ_SPIERR_WR             (1u << 1)
#define ADAQ_SPIERR_CRC            (1u << 0)

// -----------------------------------------------------------------------------
// 0x32 COEFF_CONTROL / 0x33 COEFF_DATA (custom FIR filter upload)
// -----------------------------------------------------------------------------
#define ADAQ_CC_ACCESS_EN          (1u << 7)
#define ADAQ_CC_WRITE_EN           (1u << 6)
#define ADAQ_CC_ADDR_MASK          0x3Fu       // 0..55 (56 symmetric taps)
#define ADAQ_CD_USER_COEFF_EN      (1u << 23)  // bit 23 of 24-bit COEFF_DATA
#define ADAQ_COEFF_TAP_COUNT       56

// 0x34 ACCESS_KEY — key required before a custom filter upload.
#define ADAQ_ACCESS_KEY_VALUE      0x01u

// CRC-8 polynomial used by the ADAQ7769-1 SPI interface: x^8 + x^2 + x + 1.
#define ADAQ_CRC8_POLY             0x07u

// Conversion-result geometry.
#define ADAQ_ADC_BITS              24
#define ADAQ_ADC_FULLSCALE         (1L << 23)   // +/- 2^23 twos-complement
