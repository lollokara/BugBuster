"""
BugBuster constants — enums for channel functions, ADC ranges/rates/mux,
GPIO modes, waveform types, power controls, and all binary command IDs.
"""

from enum import IntEnum


# ---------------------------------------------------------------------------
# Channel functions (AD74416H CH_FUNC_SETUP register codes)
# ---------------------------------------------------------------------------

class ChannelFunction(IntEnum):
    HIGH_IMP         = 0   # High-impedance — safe default / switching state
    VOUT             = 1   # Voltage output  0–12 V or ±12 V
    IOUT             = 2   # Current output  0–25 mA
    VIN              = 3   # Voltage input   (ADC reads loop voltage)
    IIN_EXT_PWR      = 4   # Current input   externally powered 4–20 mA
    IIN_LOOP_PWR     = 5   # Current input   loop-powered 4–20 mA
    # code 6 is not valid on the AD74416H — never use
    RES_MEAS         = 7   # Resistance / RTD measurement (2/3/4-wire)
    DIN_LOGIC        = 8   # Digital input   logic level
    DIN_LOOP         = 9   # Digital input   loop-powered
    IOUT_HART        = 10  # Current output  with HART modem overlay
    IIN_EXT_PWR_HART = 11  # Current input   ext-powered + HART
    IIN_LOOP_PWR_HART= 12  # Current input   loop-powered + HART


# ---------------------------------------------------------------------------
# ADC configuration
# ---------------------------------------------------------------------------

class AdcRange(IntEnum):
    """CONV_RANGE field codes (bits [6:4] of ADC_CONFIG register)."""
    V_0_12          = 0   # 0 V  to +12 V           — best SNR for most measurements
    V_NEG12_12      = 1   # -12 V to +12 V           — bipolar full-range
    V_NEG312_312MV  = 2   # ±312.5 mV               — precision / current sensing
    V_NEG312_0MV    = 3   # -312.5 mV to 0 V        — NOTE: firmware corrects this to code 2
    V_0_312MV       = 4   # 0 V  to +312.5 mV
    V_0_625MV       = 5   # 0 V  to +625 mV
    V_NEG104_104MV  = 6   # ±104.16 mV              — thermocouple / ultra-precision
    V_NEG2_5_2_5    = 7   # ±2.5 V                  — mid-range bipolar


class AdcRate(IntEnum):
    """CONV_RATE field codes (bits [11:8] of ADC_CONFIG register)."""
    SPS_10_H     = 0    # 10 SPS   high-resolution,  50/60 Hz rejection (-96 dB)
    SPS_20       = 1    # 20 SPS   standard
    SPS_20_H     = 3    # 20 SPS   high-resolution,  50/60 Hz rejection (-96 dB)
    SPS_200_H1   = 4    # 200 SPS  moderate rejection (-64 dB)
    SPS_200_H    = 6    # 200 SPS  high rejection     (-90 dB)
    SPS_1200     = 8    # 1.2 kSPS
    SPS_1200_H   = 9    # 1.2 kSPS high rejection     (-57 dB)
    SPS_4800     = 12   # 4.8 kSPS
    SPS_9600     = 13   # 9.6 kSPS — maximum rate per channel


class AdcMux(IntEnum):
    """CONV_MUX field codes (bits [2:0] of ADC_CONFIG register)."""
    LF_TO_AGND      = 0  # I/ON terminal vs AGND   — single-ended measurement
    HF_TO_LF        = 1  # RSENSE differential      — current sensing, default for VOUT
    VSENSEN_TO_AGND = 2  # VSENSEN pin vs AGND      — Kelvin sense negative
    LF_TO_VSENSEN   = 3  # I/ON vs VSENSEN          — 3-wire RTD path
    AGND_TO_AGND    = 4  # Self-test / zero-offset calibration


# ---------------------------------------------------------------------------
# GPIO
# ---------------------------------------------------------------------------

class GpioMode(IntEnum):
    """Modes for the 6 AD74416H GPIO pins (A–F)."""
    HIGH_IMP = 0  # Input buffer off (floating / safe)
    OUTPUT   = 1  # Logic output (can also read back)
    INPUT    = 2  # Input only
    DIN_OUT  = 3  # Mirrors the DIN comparator output of the associated channel
    DO_EXT   = 4  # External source for the DO driver of the associated channel


# ---------------------------------------------------------------------------
# Waveform generator
# ---------------------------------------------------------------------------

class WaveformType(IntEnum):
    SINE     = 0
    SQUARE   = 1
    TRIANGLE = 2
    SAWTOOTH = 3


class OutputMode(IntEnum):
    VOLTAGE = 0
    CURRENT = 1


# ---------------------------------------------------------------------------
# USB Power Delivery PDO voltage codes (HUSB238)
# ---------------------------------------------------------------------------

class UsbPdVoltage(IntEnum):
    V5  = 1
    V9  = 2
    V12 = 3
    V15 = 4
    V18 = 5
    V20 = 6


# ---------------------------------------------------------------------------
# PCA9535 power/enable controls
# ---------------------------------------------------------------------------

class PowerControl(IntEnum):
    """
    Controls for PCA9535 output bits exposed via PCA_SET_CONTROL / /api/ioexp/control.
    """
    VADJ1   = 0   # Enable LTM8063 regulator #1  (feeds ports P1+P2)
    VADJ2   = 1   # Enable LTM8063 regulator #2  (feeds ports P3+P4)
    V15A    = 2   # Enable ±15 V analog supply
    MUX     = 3   # Enable MUX switch matrix power
    USB_HUB = 4   # Enable downstream USB hub
    EFUSE1  = 5   # Enable e-fuse 1  (port P1 output protection)
    EFUSE2  = 6   # Enable e-fuse 2  (port P2 output protection)
    EFUSE3  = 7   # Enable e-fuse 3  (port P3 output protection)
    EFUSE4  = 8   # Enable e-fuse 4  (port P4 output protection)


# ---------------------------------------------------------------------------
# RTD excitation current
# ---------------------------------------------------------------------------

class RtdCurrent(IntEnum):
    UA_500 = 0  # 500 µA excitation — use for high-resistance sensors (> ~600 Ω)
    MA_1   = 1  # 1 mA  excitation — default, good for PT100 / low-R RTDs


# ---------------------------------------------------------------------------
# Voltage output range
# ---------------------------------------------------------------------------

class VoutRange(IntEnum):
    """
    Output voltage range for channels in VOUT mode.

    ┌────────────────┬──────────────┬──────────────────────────────────────┐
    │ Name           │ Range        │ Use case                             │
    ├────────────────┼──────────────┼──────────────────────────────────────┤
    │ UNIPOLAR       │ 0 V – +12 V  │ Standard positive-only outputs       │
    │ BIPOLAR        │ -12 V – +12 V│ Bipolar / signed control signals     │
    └────────────────┴──────────────┴──────────────────────────────────────┘
    """
    UNIPOLAR = 0   # 0 V to +12 V
    BIPOLAR  = 1   # -12 V to +12 V


# ---------------------------------------------------------------------------
# Current output limit
# ---------------------------------------------------------------------------

class CurrentLimit(IntEnum):
    """
    Maximum current for channels in IOUT mode.

    ┌──────────┬──────────┬────────────────────────────────────────────────┐
    │ Name     │ Max      │ Use case                                       │
    ├──────────┼──────────┼────────────────────────────────────────────────┤
    │ MA_25    │ 25 mA    │ Full-range 4–20 mA loops (default)             │
    │ MA_8     │  8 mA    │ Low-power outputs, protect sensitive loads     │
    └──────────┴──────────┴────────────────────────────────────────────────┘
    """
    MA_25 = 0   # Full 0–25 mA range (default)
    MA_8  = 1   # Limited to 0–8 mA


# ---------------------------------------------------------------------------
# Digital output mode
# ---------------------------------------------------------------------------

class DoMode(IntEnum):
    """
    Operating mode for a channel configured as digital output.

    ┌──────────────┬────────────────────────────────────────────────────────┐
    │ Name         │ Description                                            │
    ├──────────────┼────────────────────────────────────────────────────────┤
    │ HIGH_SIDE    │ High-side switch (source current to load)              │
    │ LOW_SIDE     │ Low-side switch  (sink current from load)              │
    │ PUSH_PULL    │ Push-pull driver (both source and sink)                │
    └──────────────┴────────────────────────────────────────────────────────┘
    """
    HIGH_SIDE  = 0
    LOW_SIDE   = 1
    PUSH_PULL  = 2


# ---------------------------------------------------------------------------
# AVDD (analog supply) selection
# ---------------------------------------------------------------------------

class AvddSelect(IntEnum):
    """
    AVDD rail source for a channel.  Controls whether the channel uses the
    high (AVDD_HI) or low (AVDD_LO) analog supply.

    ┌─────────┬───────────┬────────────────────────────────────────────────┐
    │ Name    │ Rail      │ Typical use                                    │
    ├─────────┼───────────┼────────────────────────────────────────────────┤
    │ LO      │ AVDD_LO   │ Low-voltage loads; saves power                 │
    │ HI      │ AVDD_HI   │ High-voltage outputs; required for > ~10 V    │
    └─────────┴───────────┴────────────────────────────────────────────────┘
    """
    LO = 0
    HI = 1


# ---------------------------------------------------------------------------
# Binary protocol message types
# ---------------------------------------------------------------------------

class MsgType(IntEnum):
    CMD = 0x01  # Host → Device
    RSP = 0x02  # Device → Host (response to CMD)
    EVT = 0x03  # Device → Host (unsolicited: stream data, alerts, DIN edges)
    ERR = 0x04  # Device → Host (error response)


# ---------------------------------------------------------------------------
# Binary protocol command IDs
# ---------------------------------------------------------------------------

class CmdId(IntEnum):
    # Status & information
    GET_STATUS       = 0x01
    GET_DEVICE_INFO  = 0x02
    GET_FAULTS       = 0x03
    GET_DIAGNOSTICS  = 0x04

    # Channel configuration
    SET_CHANNEL_FUNC = 0x10
    SET_DAC_CODE     = 0x11
    SET_DAC_VOLTAGE  = 0x12
    SET_DAC_CURRENT  = 0x13
    SET_ADC_CONFIG   = 0x14
    SET_DIN_CONFIG   = 0x15
    SET_DO_CONFIG    = 0x16
    SET_DO_STATE     = 0x17
    SET_VOUT_RANGE   = 0x18
    SET_CURRENT_LIMIT= 0x19
    SET_AVDD_SELECT  = 0x1A
    GET_ADC_VALUE    = 0x1B
    GET_DAC_READBACK = 0x1C
    SET_RTD_CONFIG   = 0x1D

    # Fault management
    CLEAR_ALL_ALERTS  = 0x20
    CLEAR_CHAN_ALERT  = 0x21
    SET_ALERT_MASK    = 0x22
    SET_CH_ALERT_MASK = 0x23

    # Diagnostics
    SET_DIAG_CONFIG  = 0x30

    # GPIO (AD74416H on-chip GPIO pins A-F)
    GET_GPIO_STATUS  = 0x40
    SET_GPIO_CONFIG  = 0x41
    SET_GPIO_VALUE   = 0x42

    # Digital IO (ESP32 GPIO-based, 12 logical IOs through MUX)
    DIO_GET_ALL      = 0x43
    DIO_CONFIG       = 0x44
    DIO_WRITE        = 0x45
    DIO_READ         = 0x46

    # UART bridge
    GET_UART_CONFIG  = 0x50
    SET_UART_CONFIG  = 0x51
    GET_UART_PINS    = 0x52

    # ADC streaming
    START_ADC_STREAM = 0x60
    STOP_ADC_STREAM  = 0x61

    # System
    DEVICE_RESET     = 0x70
    REGISTER_READ    = 0x71
    REGISTER_WRITE   = 0x72

    # Unsolicited events
    ADC_DATA_EVT     = 0x80
    SCOPE_DATA_EVT   = 0x81
    ALERT_EVT        = 0x82
    DIN_EVT          = 0x83

    # MUX switch matrix
    MUX_SET_ALL      = 0x90
    MUX_GET_ALL      = 0x91
    MUX_SET_SWITCH   = 0x92

    # DS4424 IDAC
    IDAC_GET_STATUS   = 0xA0
    IDAC_SET_CODE     = 0xA1
    IDAC_SET_VOLTAGE  = 0xA2
    IDAC_CALIBRATE    = 0xA3
    IDAC_CAL_ADD_POINT= 0xA4
    IDAC_CAL_CLEAR    = 0xA5
    IDAC_CAL_SAVE     = 0xA6

    # PCA9535 I/O expander
    PCA_GET_STATUS   = 0xB0
    PCA_SET_CONTROL  = 0xB1
    PCA_SET_PORT     = 0xB2

    # USB PD
    USBPD_GET_STATUS = 0xC0
    USBPD_SELECT_PDO = 0xC1
    USBPD_GO         = 0xC2

    # Waveform generator
    START_WAVEGEN    = 0xD0
    STOP_WAVEGEN     = 0xD1

    # Misc hardware
    SET_LSHIFT_OE    = 0xE0
    WIFI_GET_STATUS  = 0xE1
    WIFI_CONNECT     = 0xE2
    SET_SPI_CLOCK    = 0xE3
    WIFI_SCAN        = 0xE4

    # Session control
    PING             = 0xFE
    DISCONNECT       = 0xFF


# ---------------------------------------------------------------------------
# Device error codes (ERR message payload byte 0)
# ---------------------------------------------------------------------------

class ErrorCode(IntEnum):
    INVALID_CMD     = 0x01
    INVALID_CHANNEL = 0x02
    INVALID_PARAM   = 0x03
    SPI_FAIL        = 0x04
    QUEUE_FULL      = 0x05
    BUSY            = 0x06
    INVALID_STATE   = 0x07
    CRC_FAIL        = 0x08
    FRAME_TOO_LARGE = 0x09
    STREAM_ACTIVE   = 0x0A
