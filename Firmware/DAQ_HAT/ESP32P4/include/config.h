// =============================================================================
// config.h — DAQ HAT (ESP32-P4) board configuration
//
// Central place for pin assignments, bus topology and timing constants for the
// BugBuster DAQ HAT application processor.
//
// Board hardware summary:
//   - 3x ADAQ7769-1   24-bit Sigma-Delta DAQ uModules (SPI control mode)
//       * ADAQ #0  -> dedicated SPI bus  (one ADC alone on the bus)
//       * ADAQ #1  -> shared SPI bus     (two ADCs share MOSI/MISO/SCLK)
//       * ADAQ #2  -> shared SPI bus     (same bus as #1, separate CS)
//   - 2x AD7414/AD7415 I2C temperature sensors
//   - 1x DS4424        4-channel I2C IDAC (reuse of ESP32 driver)
//
//   Clocking : shared external 16.384 MHz CMOS oscillator feeds XTAL2_MCLK of
//              all three ADAQ (P4 does NOT generate MCLK).
//   Sync     : one ADAQ is the sync master; its SYNC_OUT is wired to the
//              SYNC_IN of all three. The P4 triggers a sync by writing
//              SPI_START to the master over SPI.
//   PIN/SPI  : pulled to 3V3 on every ADAQ -> SPI control mode.
//
// !!! PIN ASSIGNMENTS BELOW ARE PLACEHOLDERS !!!
// The PCB pinout is not finalized. Every GPIO marked PLACEHOLDER must be
// confirmed against the schematic before bring-up. They are grouped and
// named so a single search ("PLACEHOLDER") finds them all.
// =============================================================================

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "hal/spi_types.h"

// -----------------------------------------------------------------------------
// Inter-processor UART to the on-module ESP32-C6 (P4 GPIO32 -> C6 U0RXD,
// P4 GPIO33 <- C6 U0TXD). This is the C6's UART0, which doubles as the ROM
// download UART used to flash the C6 from the P4 (see C6 boot/reset control
// below). UART0 console is on GPIO37/38 (do not reuse).
// -----------------------------------------------------------------------------
#define DAQ_UART_TX_PIN   32      // P4 TX -> C6 U0RXD
#define DAQ_UART_RX_PIN   33      // P4 RX <- C6 U0TXD
#define DAQ_UART_BAUD     921600
#define DAQ_UART_PORT     2       // P4 UART2 (0=console, 1=S3 link)

// -----------------------------------------------------------------------------
// ESP32-C6 boot/reset control.
//   C6 CHIP_PU (EN/RST)  → P4 GPIO54  (LOW = reset, active-LOW)
//   C6 GPIO9  (BOOT strap) → P4 GPIO43  (LOW at EN release = download mode)
//   C6 GPIO8  (BOOT_EN)   → P4 GPIO44  (0=UART download, 1=SDIO download)
//   C6 GPIO2  (IO2)       → P4 GPIO6   (reserved)
//
// SDIO download mode : GPIO9=0 (GPIO43 LOW) + GPIO8=1 (GPIO44 HIGH / float)
// UART download mode : GPIO9=0 (GPIO43 LOW) + GPIO8=0 (GPIO44 LOW)
// Normal boot        : GPIO9=1 (GPIO43 HIGH / float)
// -----------------------------------------------------------------------------
#define C6_RST_PIN        ((gpio_num_t)54)   // C6 CHIP_PU  (LOW = reset)
#define C6_BOOT_PIN       ((gpio_num_t)44)   // C6 GPIO9 strap (LOW = download mode)
#define C6_BOOT_EN_PIN    ((gpio_num_t)43)   // C6 GPIO8 (0=UART dl, 1=SDIO dl)
#define C6_IO2_PIN        ((gpio_num_t) 6)   // C6 GPIO2  (reserved)

// -----------------------------------------------------------------------------
// Front-panel navigation buttons (active-low, internal pull-ups), wired to the
// P4 and relayed to the C6 over DDP (the C6 has no local buttons). OK long-press
// = Back, mirroring the previous on-C6 button behaviour.
//   UP   -> IO26
//   DOWN -> IO46
//   OK   -> IO45  (hold = Back)
// -----------------------------------------------------------------------------
#define BTN_PIN_UP        ((gpio_num_t)26)
#define BTN_PIN_DOWN      ((gpio_num_t)46)
#define BTN_PIN_OK        ((gpio_num_t)45)

// -----------------------------------------------------------------------------
// SDIO link to the ESP32-C6 (P4 = SDIO host, C6 = SDIO slave / ESP-Hosted).
// Used for both normal WiFi offload and SDIO ROM download mode flashing.
// GPIO9 LOW at EN release → C6 enters SDIO ROM download mode.
// Uses SDMMC_HOST_SLOT_1 (slot 0 is the eMMC-oriented slot on P4).
// -----------------------------------------------------------------------------
#define C6_SDIO_CLK_PIN   ((gpio_num_t)18)   // P4 SDIO CLK  → C6 GPIO18
#define C6_SDIO_CMD_PIN   ((gpio_num_t)19)   // P4 SDIO CMD  → C6 GPIO19
#define C6_SDIO_DAT0_PIN  ((gpio_num_t)14)   // P4 SDIO DAT0 → C6 GPIO20
#define C6_SDIO_DAT1_PIN  ((gpio_num_t)15)   // P4 SDIO DAT1 → C6 GPIO21
#define C6_SDIO_DAT2_PIN  ((gpio_num_t)16)   // P4 SDIO DAT2 → C6 GPIO22
#define C6_SDIO_DAT3_PIN  ((gpio_num_t)17)   // P4 SDIO DAT3 → C6 GPIO23

// -----------------------------------------------------------------------------
// Link to the ESP32-S3 mainboard (HAT control plane).
//
// The P4 DAQ board is a HAT to the S3 mainboard. The S3 reuses the SAME UART
// port + IRQ that the RP2040 HAT used (same connector on the S3 side), so the
// wire protocol is the existing BugBuster HAT protocol (SYNC 0xAA, LEN, CMD,
// PAYLOAD, CRC-8 poly 0x07). The S3 is master; the P4 is the slave.
//
// !!! Pin numbers below are from the user's wiring note (2026-06-19). NOTE the
// apparent overlap with the DAQ board doc: GPIO29 = ADAQ3 DRDY, GPIO27/6 = J5
// expansion. Reconcile against the final schematic before bring-up. !!!
// -----------------------------------------------------------------------------
#define S3LINK_UART_NUM   1                 // UART1 (UART0 = console)
#define S3LINK_TX_PIN     ((gpio_num_t)27)  // P4 TX -> S3 RX
#define S3LINK_RX_PIN     ((gpio_num_t)28)  // S3 TX -> P4 RX (schematic-confirmed; ADAQ3 DRDY is on 29)
#define S3LINK_INT_PIN    ((gpio_num_t)6)   // open-drain IRQ to S3 (pulled up)
#define S3LINK_BAUD       921600

// HAT type code reported in GET_INFO (must not collide with existing codes
// 0x00..0x04 in the legacy detection table).
#define HAT_TYPE_DAQ_POWER  0x10


// Global ADAQ clocking / topology constants
// -----------------------------------------------------------------------------
#define ADAQ_COUNT                3              // total ADAQ7769-1 on board
#define ADAQ_MCLK_HZ              16384000UL     // SiT8208 Y1 -> CDCLVC1104 fan-out
#define ADAQ_SYNC_MASTER_INDEX    0              // ADAQ1 (U1) is the SYNC master

// SPI clock for ADAQ register access vs. data readout. Datasheet max SCLK = 20 MHz.
// Register access runs FULL-DUPLEX (like the ADC-data stream) with a trailing
// dummy byte on reads to avoid the ESP32 last-bit loss; kept modest (8 MHz) as
// it's not throughput-critical. The ADC-data stream (dev_data) runs at 20 MHz.
#define ADAQ_SCLK_CFG_HZ          8000000UL      // 8 MHz register access (full-duplex)
// Data-readout clock. Datasheet max SCLK = 20 MHz. A 30 MHz experiment (2026-07-11)
// corrupted ADC data reads (ESP_ERR_INVALID_CRC on adaq/streaming) — reverted.
// Stay at the rated 20 MHz.
#define ADAQ_SCLK_DATA_HZ         20000000UL     // 20 MHz for sample readout (spec max)
// Slave DOUT valid delay + P4 input path. The ESP-IDF SPI driver uses this to
// place the MISO sample point AFTER DOUT settles; without it the final bit of a
// register read is sampled too early and reads back as 0 (value & 0xFE), and
// high-speed (>~4 MHz) reads corrupt. Tune on the bench if reads misbehave.
#define ADAQ_SPI_INPUT_DELAY_NS   30

// Extra SCLK cycles CS is held low after the last data bit of a register read.
// A normal ADAQ read frames the data LSB on the final rising edge and holds it
// only until CS rises; holding CS a few cycles keeps DOUT = D0 long enough for
// the ESP32-P4 to latch it (otherwise the LSB reads 0 -> value & 0xFE).
#define ADAQ_CS_POSTTRANS_CYCLES  4

// Streaming: read the 8-bit status header only every Nth sample per device
// (data-only 24-bit / 3-byte reads otherwise). The status header (saturation /
// not-settled / master-error flags) changes slowly, so sampling it periodically
// keeps the per-sample SPI frame at 24 bits (~1.8 us) instead of 32 (~2.2 us)
// while still surfacing fault flags. 1 = every sample.
#define ADAQ_STATUS_SAMPLE_DIV    256

// -----------------------------------------------------------------------------
// SPI bus A — GP-SPI3, dedicated to ADAQ #0 = ADAQ1/U1 = FINE current.
// All lines via the GPIO matrix (no native IOMUX on bus A).
// -----------------------------------------------------------------------------
#define ADAQ_BUSA_HOST            SPI3_HOST
#define ADAQ_BUSA_SCLK_PIN        ((gpio_num_t)20)   // U1 SCLK
#define ADAQ_BUSA_MOSI_PIN        ((gpio_num_t)13)   // U1 SDI
#define ADAQ_BUSA_MISO_PIN        ((gpio_num_t)21)   // U1 DOUT

// -----------------------------------------------------------------------------
// SPI bus B — GP-SPI2 (native IOMUX), shared by ADAQ #1 (U22 COARSE) and
// ADAQ #2 (U23 VOLTAGE).
// -----------------------------------------------------------------------------
#define ADAQ_BUSB_HOST            SPI2_HOST
#define ADAQ_BUSB_SCLK_PIN        ((gpio_num_t)9)    // U22+U23 SCLK (SPI2_CK)
#define ADAQ_BUSB_MOSI_PIN        ((gpio_num_t)8)    // U22+U23 SDI  (SPI2_D)
#define ADAQ_BUSB_MISO_PIN        ((gpio_num_t)10)   // U22+U23 DOUT (SPI2_Q)

// -----------------------------------------------------------------------------
// Per-device control lines.
// RESET is SHARED: all 3 ADAQ *RST tie to GPIO2 (resetting one resets all).
// The board does ONE shared HW reset, then per-device SOFT reset over SPI, so
// the per-device reset_pin is left GPIO_NUM_NC (soft-reset path).
// -----------------------------------------------------------------------------
#define ADAQ_SHARED_RESET_PIN     ((gpio_num_t)2)    // all 3 *RST (active low)

// ADAQ #0 = ADAQ1 / U1 — FINE current (bus A)
#define ADAQ0_CS_PIN              ((gpio_num_t)12)
#define ADAQ0_DRDY_PIN            ((gpio_num_t)11)
#define ADAQ0_RESET_PIN           GPIO_NUM_NC        // shared reset (soft per-device)

// ADAQ #1 = ADAQ2 / U22 — COARSE current, 50 mohm (bus B)
#define ADAQ1_CS_PIN              ((gpio_num_t)7)
#define ADAQ1_DRDY_PIN            ((gpio_num_t)5)
#define ADAQ1_RESET_PIN           GPIO_NUM_NC

// ADAQ #2 = ADAQ3 / U23 — VOLTAGE, V_DUT (bus B)
#define ADAQ2_CS_PIN              ((gpio_num_t)30)
#define ADAQ2_DRDY_PIN            ((gpio_num_t)29)
#define ADAQ2_RESET_PIN           GPIO_NUM_NC

// Sync is performed over SPI (SPI_START on the master); no dedicated P4 pin.
#define ADAQ_START_PIN            GPIO_NUM_NC

// -----------------------------------------------------------------------------
// I2C bus: GPIO39 SDA / GPIO40 SCL, external pull-ups to 3V3_ESP, 400 kHz.
// -----------------------------------------------------------------------------
#define DAQ_I2C_PORT              0
#define DAQ_I2C_SDA_PIN           ((gpio_num_t)39)
#define DAQ_I2C_SCL_PIN           ((gpio_num_t)40)
#define DAQ_I2C_FREQ_HZ           400000UL

// AD7415 temperature sensors (NO ALERT — has_alert must be false).
//   U2  = AD7415, AS=GND -> 0x49 (monitors ADG area, 5V3_BUCK)
//   U28 = AD7415, AS=VDD -> 0x4A (monitors power area, 5V_BUCK)
#define AD741X_0_ADDR             0x49
#define AD741X_1_ADDR             0x4A

// DS4424 IDAC (U26): true 7-bit address 0x10 (datasheet 0x20 is the 8-bit byte).
//   Ch0 (0xF8) = LTM8056 current limit, Ch1 (0xF9) = V_DUT voltage set.
#define DS4424_I2C_ADDR           0x10
#define DS4424_CH_ILIMIT          0      // OUT0 -> I_FB_DCDC (current limit)
#define DS4424_CH_VDUT            1      // OUT1 -> V_FB_DCDC (output voltage)

// -----------------------------------------------------------------------------
// Current-sense front-end: shunt ladder + analog autorange
//
// Series shunts 51 / 2 / 0.05 ohm with AD8411A current-sense amps (gain 50 V/V).
// Hardware SR latches (U12 + ADCMP600 comparators) auto-close the bypass
// switches as current rises. The two bypass-control nets are tapped through 1k
// resistors so the P4 can EITHER read the latch-driven state (pin = high-Z
// input) OR override it (pin = driven output).
//
//   FINE ADAQ  (#0, U1, bus A) reads the 51 ohm OR 2 ohm CSA, selected by the
//              FINE input mux U24. Firmware sets the mux to match the range.
//   COARSE ADAQ(#1, U22, bus B) sits permanently on the 50 mohm node (R30) and
//              is the always-valid gap-fill reference during a range switch.
//   VOLTAGE ADAQ(#2, U23, bus B) reads V_dut via the U25 mux.
// -----------------------------------------------------------------------------

// Shunt resistances (ohms) — replace with measured/calibrated values.
#define SHUNT_HI_OHM              51.0f
#define SHUNT_MID_OHM             2.0f
#define SHUNT_LO_OHM              0.05f

// AD8411A fixed gain (V/V).
#define ISENSE_AMP_GAIN           50.0f

// Bidirectional bypass-control lines (read = observe range, drive = override).
//   GPIO52 -> U9  : 51 ohm bypass. HIGH = 51 ohm shorted out.
//   GPIO53 -> U13 : 2 ohm bypass (R27). HIGH = 2 ohm shorted out.
// Range decode from (bypass51, bypass2):
//   (0,0)=HI 51ohm, (1,0)=MID 2ohm, (1,1)=LO 50mohm, (0,1)=transition.
#define RANGE_BYPASS51_PIN        ((gpio_num_t)52)   // U9  (51 ohm bypass)
#define RANGE_BYPASS2_PIN         ((gpio_num_t)53)   // U13 (2 ohm bypass)

// FINE input mux U24 (ADG5204): selects which CSA feeds ADAQ1.
//   A0=GPIO50, A1=GPIO51, EN=GPIO49 (EN is SHARED with the U25 voltage mux).
// Channel-to-CSA mapping is UNCONFIRMED: assume S1(addr 0)=51 ohm CSA (HI) and
// S2(addr 1)=2 ohm CSA (MID). Flip FINE_MUX_ADDR_* if the bench disagrees.
#define FINE_MUX_A0_PIN           ((gpio_num_t)50)
#define FINE_MUX_A1_PIN           ((gpio_num_t)51)
#define MUX_EN_PIN                ((gpio_num_t)49)   // shared U24+U25 enable
#define FINE_MUX_ADDR_HI          0      // 51 ohm CSA channel (S1)
#define FINE_MUX_ADDR_MID         1      // 2 ohm CSA channel  (S2)

// Voltage input mux U25 (ADG5204): A0=GPIO48, A1=GPIO47, EN shared (GPIO49).
#define VOLT_MUX_A0_PIN           ((gpio_num_t)48)
#define VOLT_MUX_A1_PIN           ((gpio_num_t)47)
// U25 channel addresses (2-bit, A1:A0). S4 carries V_DUT taken directly off the
// LTM8056 output and is the node read during SMU voltage calibration.
// TODO: confirm S4 = 0b11 against the ADG5204 channel numbering on the bench.
#define VOLT_MUX_ADDR_VDUT        0x3    // S4 = V_DUT direct off DCDC output

// Which ADAQ index plays each measurement role (see daq_board topology).
#define ADAQ_ROLE_FINE            0
#define ADAQ_ROLE_COARSE          1
#define ADAQ_ROLE_VOLTAGE         2

// V_DUT sense-path scaling. The VOLTAGE ADAQ reads V_DUT via mux U25 on IN3_AAF;
// adaq7769_code_to_volts() already removes the AAF attenuation, so this is the
// residual external divider ratio (1.0 if V_DUT connects straight to the AAF
// input). TODO: set to the measured ratio during factory calibration.
#define V_DUT_SENSE_SCALE         1.0f

// Target ODR for the slow VOLTAGE channel. U22 (COARSE) and U23 (VOLTAGE) share
// SPI bus B and one common SYNC line, so they cannot be phase-staggered. Running
// VOLTAGE well below the current ODR de-collides their DRDY: the shared bus is
// dominated by COARSE reads with only an occasional VOLTAGE read, so COARSE never
// misses its conversion deadline.
#define VOLTAGE_ODR_TARGET_SPS    50000.0f

// Default USB waveform-stream decimation. The full-rate fused stream to the PC
// is gated only by this (default 1 = every sample).
#define WAVE_STREAM_DECIM_DEFAULT 1

// Default DSP-tail decimation: the on-device power/energy/stats/multires/FFT run
// on every Nth fused sample, NOT the full per-channel rate. They don't need the
// full 256k/ch rate, and decimating frees core-0 CPU so the full-rate fused
// stream to the PC keeps flowing. At FINE 256k -> 32k DSP; at 128k -> 16k. The
// PC still receives every fused sample (that path is gated by wave_decim only).
#define DAQ_DSP_DECIM_DEFAULT     8

// -----------------------------------------------------------------------------
// Power-rail enable / monitor GPIOs (all HV rails OFF at boot via pull-downs).
// Sequence: 3V3 -> wait PG -> +/-26V -> +/-24V -> release ADAQ RST -> V_DUT.
// -----------------------------------------------------------------------------
#define PWR_3V3_EN_PIN            ((gpio_num_t)42)   // TPS74601 EN
#define PWR_3V3_PG_PIN            ((gpio_num_t)41)   // TPS74601 PG (input)
#define PWR_26V_EN_PIN            ((gpio_num_t)54)   // ADP5071 +/-26V EN
#define PWR_24V_EN_PIN            ((gpio_num_t)3)    // ADP7142/7182 +/-24V EN
#define PWR_VDUT_RUN_PIN          ((gpio_num_t)4)    // LTM8056 RUN (V_DUT)

// LTM8056 current monitors (ESP32-P4 ADC1).
#define IINMON_ADC_CH             6      // GPIO22, 0-1.0V at 0-2.5A input
#define IOUTMON_ADC_CH            7      // GPIO23, 0-1.2V at 0-2.636A output

// -----------------------------------------------------------------------------
// Source/SMU (LTM8056 U27, programmed by DS4424 U26).
//
// V_DUT  = V0 - R_FB * I_DAC_ch1   (LTM8056 FB pin, DS4424 OUT1)
//        = 10.85 V - 90.9 kohm * I_DAC_ch1
//   I_DAC full-scale = +/-100 uA (Rfs set in hardware) -> 127 codes.
//   I_DAC = -100 uA -> V_DUT ~ 19.94 V (max)
//   I_DAC = +100 uA -> V_DUT ~  1.76 V (min)
//
// Current limit: full-scale 2.636 A (58 mV / 22 mohm). DS4424 OUT0 sinks current
// into the CTL node to reduce the limit below full scale.
// -----------------------------------------------------------------------------
#define SMU_VDUT_V0               10.85f     // V_DUT formula intercept (V)
#define SMU_VDUT_RFB_OHM          90900.0f   // feedback resistor (ohm)
#define SMU_DS4424_IFS_UA         100.0f     // DS4424 full-scale current (uA)
#define SMU_VDUT_MIN              1.76f
#define SMU_VDUT_MAX              19.94f
// Polarity: +1 if a positive DS4424 code (source current) lowers V_DUT
// (I_DAC positive). Flip to -1 if the bench shows the opposite sense.
#define SMU_VDUT_CODE_POLARITY    (+1)

#define SMU_ILIMIT_FULLSCALE_A    2.636f     // LTM8056 max current limit (A)
// Approximate slope: DS4424 ch0 code 0 -> full-scale limit; +127 -> ~min limit.
// Refined by calibration; linear placeholder for now.
#define SMU_ILIMIT_MIN_A          0.05f
#define SMU_ILIMIT_CODE_POLARITY  (+1)

// LTM8056 IINMON / IOUTMON full-scale mapping (volts at ADC -> amps).
#define SMU_IINMON_FS_V           1.0f       // 1.0 V  == 2.5 A input
#define SMU_IINMON_FS_A           2.5f
#define SMU_IOUTMON_FS_V          1.2f       // 1.2 V  == 2.636 A output
#define SMU_IOUTMON_FS_A          2.636f

// -----------------------------------------------------------------------------
// SMU factory calibration (smu_cal.c) — persisted to ESP32-P4 NVS.
//
// Voltage cal sweeps DS4424 ch1 with the load disconnected, reading V_DUT off
// the U25 S4 node (VOLTAGE ADAQ). Current-limit cal sweeps DS4424 ch0 into a
// shorted output with the autorange forced to the 50 mohm (LO) shunt, reading
// the COARSE ADAQ as the primary reference and IOUTMON as a cross-check.
// -----------------------------------------------------------------------------
#define SMU_CAL_NVS_NS            "smu_cal"  // NVS namespace
#define SMU_CAL_NVS_KEY           "blob"     // NVS blob key
#define SMU_BASE_NVS_KEY          "base"     // NVS baseline-offset blob key
#define SMU_CAL_MAGIC             0x534D5543u // "SMUC"
#define SMU_CAL_VERSION           2u
#define SMU_CAL_MAX_POINTS        255        // per channel (full DS4424 code span -127..+127)

// Stability gate for one calibration point (mirrors RP2040 HAT cal).
#define SMU_CAL_SAMPLES_PER_PT    100        // raw reads per measurement (averaged)
#define SMU_CAL_MEDIAN_WINDOW     16         // central samples kept after sort
#define SMU_CAL_SETTLE_WINDOW     5          // consecutive stable measurements
#define SMU_CAL_SETTLE_ITERS_MAX  100        // ~1.5 s/point timeout
#define SMU_CAL_SETTLE_MS         15         // delay between measurements (ms)
#define SMU_CAL_V_NOISE_V         0.030f     // V_DUT settle threshold (V)
#define SMU_CAL_I_NOISE_A         0.005f     // current settle threshold (A)

// Current-limit cal sequence parameters.
#define SMU_CAL_ICAL_VSET_V       3.0f       // V_DUT set during current cal (headroom to reach 2 A)
#define SMU_CAL_ICAL_ENABLE_MS    100        // wait after MIN code before RUN on
#define SMU_CAL_ICAL_TARGET_A     2.0f       // sweep until output reaches this

// Baseline (open-circuit offset) calibration. With the output OPEN, sweep V_DUT
// across every DS4424 code, settle, average many raw current reads, and store
// the resulting ADC offset code per (range, V_DUT code). At runtime the offset
// for the active range + V_DUT is loaded straight into the ADAQ OFFSET register
// (hardware auto-subtract) whenever V_DUT or the range changes — no software math.
#define SMU_BASE_MAGIC            0x53424153u // "SBAS"
#define SMU_BASE_VERSION          3u
#define SMU_BASE_RANGES           3          // HI / MID / LO  (== RANGE_COUNT)
#define SMU_BASE_CODES            255        // V_DUT DAC codes -127..+127
#define SMU_BASE_SETTLE_MS        200        // settle after each V_DUT step (ms)
#define SMU_BASE_SAMPLES_PER_PT   1000       // raw current reads averaged per code
#define SMU_BASE_TEMP_NA          (-32768)   // sentinel: temperature unavailable


