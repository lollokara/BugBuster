"""
BugBuster constants — enums for channel functions, ADC ranges/rates/mux,
GPIO modes, waveform types, power controls, and all binary command IDs.
"""

from enum import IntEnum
from typing import Dict

# BBP_PROTO_VERSION deliberately does NOT live here.
#
# The canonical copy is `bugbuster/protocol.py`, which is what the wire, the
# tests and Firmware/tools/check_proto_version.py all read. A duplicate literal
# used to sit here; nothing ever imported it, and it silently stayed at 9 while
# the three canonical sources moved to 10 -- wire-wrong and unguarded, since
# check_proto_version.py only reads bbp.h, protocol.py and bbp.rs.
#
# It cannot be re-exported from protocol.py either: protocol.py imports from
# this module, so that would be a circular import. Import it from
# bugbuster.protocol directly.
#
# check_proto_version.py now fails if a literal reappears here.
# See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S2-14.

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
# HAT Expansion Board pin functions
# ---------------------------------------------------------------------------

class LaTriggerType(IntEnum):
    """Trigger types for the HAT logic analyzer."""
    NONE    = 0   # No trigger — capture immediately
    RISING  = 1   # Rising edge
    FALLING = 2   # Falling edge
    BOTH    = 3   # Any edge
    HIGH    = 4   # Level high
    LOW     = 5   # Level low


class HatPinFunction(IntEnum):
    """
    Functions assignable to each EXP_EXT pin on the HAT connector.

    Numeric slots 1..4 are reserved for wire-protocol compatibility.
    They used to be SWDIO, SWCLK, TRACE1, TRACE2 but were removed when
    the new HAT PCB (2026-04-09) gave SWD its own dedicated 3-pin
    connector (SWDIO/SWCLK/TRACE) driven directly by the RP2040
    debugprobe pins. Enabling SWD is now a single ``hat_setup_swd()``
    call — it does NOT touch EXP_EXT.

    ``BugBuster.hat_set_pin()`` raises :class:`HatPinFunctionError` if
    called with any reserved numeric code.
    """
    DISCONNECTED = 0
    # 1, 2, 3, 4 are reserved (formerly SWDIO/SWCLK/TRACE1/TRACE2).
    GPIO1        = 5
    GPIO2        = 6
    GPIO3        = 7
    GPIO4        = 8


# Reserved HatPinFunction numeric codes — module-level constants kept so
# client.py can reject them without re-introducing them into the enum.
HAT_FUNC_RESERVED_CODES: frozenset[int] = frozenset({1, 2, 3, 4})


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
    # Command IDs are generated from bbp.h by Firmware/tools/gen_error_tables.py - do not edit.
    # BEGIN GENERATED: bbp-commands
    GET_STATUS                      = 0x01
    GET_DEVICE_INFO                 = 0x02
    GET_FAULTS                      = 0x03
    GET_DIAGNOSTICS                 = 0x04
    SELFTEST_STATUS                 = 0x05  # Get boot self-test result + cal status
    SELFTEST_MEASURE_SUPPLY         = 0x06  # Measure a supply rail via U23
    SELFTEST_SUPPLY_VOLTAGES_CACHED = 0x07  # Get cached supply rail voltages
    SELFTEST_AUTO_CAL               = 0x08  # Start automatic IDAC calibration
    SELFTEST_INT_SUPPLIES           = 0x09  # Measure internal ADC supplies (AVDD, DVCC, etc.)
    SELFTEST_WORKER                 = 0x0B  # Enable/disable/query supply monitor worker
    WIFI_FORGET                     = 0x0A  # Erase saved STA credentials from NVS and disconnect
    MEM_STATUS                      = 0x0C  # Heap + per-task stack telemetry (see MEM_STATUS_SCHEMA)
    EFUSE_IMON_SET                  = 0x0D  # Select monitored e-fuse (0=off,1..4) via U23 IMON path
    EFUSE_IMON_GET                  = 0x0E  # Read monitored e-fuse current
    SET_CHANNEL_FUNC                = 0x10
    SET_DAC_CODE                    = 0x11
    SET_DAC_VOLTAGE                 = 0x12
    SET_DAC_CURRENT                 = 0x13
    SET_ADC_CONFIG                  = 0x14
    SET_DIN_CONFIG                  = 0x15
    SET_DO_CONFIG                   = 0x16
    SET_DO_STATE                    = 0x17
    SET_VOUT_RANGE                  = 0x18
    SET_CURRENT_LIMIT               = 0x19
    SET_AVDD_SELECT                 = 0x1A
    GET_ADC_VALUE                   = 0x1B
    GET_DAC_READBACK                = 0x1C
    SET_RTD_CONFIG                  = 0x1D
    CLEAR_ALL_ALERTS                = 0x20
    CLEAR_CHAN_ALERT                = 0x21
    SET_ALERT_MASK                  = 0x22
    SET_CH_ALERT_MASK               = 0x23
    SET_DIAG_CONFIG                 = 0x30
    GET_GPIO_STATUS                 = 0x40
    SET_GPIO_CONFIG                 = 0x41
    SET_GPIO_VALUE                  = 0x42
    ADC_LEDS_SET_MODE               = 0x47  # 1-byte payload: 0=auto, 1=manual
    DIO_GET_ALL                     = 0x43  # Read all 12 IO states
    DIO_CONFIG                      = 0x44  # Configure IO direction (input/output/disabled)
    DIO_WRITE                       = 0x45  # Set output level (high/low)
    DIO_READ                        = 0x46  # Read single IO input level
    GET_UART_CONFIG                 = 0x50
    SET_UART_CONFIG                 = 0x51
    GET_UART_PINS                   = 0x52
    START_ADC_STREAM                = 0x60
    STOP_ADC_STREAM                 = 0x61
    START_SCOPE_STREAM              = 0x62
    STOP_SCOPE_STREAM               = 0x63
    START_ADC_DSP_STREAM            = 0x64
    STOP_ADC_DSP_STREAM             = 0x65
    MUX_SET_ALL                     = 0x90
    MUX_GET_ALL                     = 0x91
    MUX_SET_SWITCH                  = 0x92
    RAIL_POWER_UP                   = 0x93  # Sequenced VADJ rail + e-fuse power-up (power/rail_power.h)
    IDAC_GET_STATUS                 = 0xA0  # Get all IDAC channel states
    IDAC_SET_CODE                   = 0xA1  # Set raw DAC code
    IDAC_SET_VOLTAGE                = 0xA2  # Set target voltage
    IDAC_CALIBRATE                  = 0xA3  # Run auto-calibration (unused, UI-driven)
    IDAC_CAL_ADD_POINT              = 0xA4  # Add a single calibration point
    IDAC_CAL_CLEAR                  = 0xA5  # Clear calibration for a channel
    IDAC_CAL_SAVE                   = 0xA6  # Save calibration to NVS
    IO_CLAIM                        = 0xA7  # Claim one or more IO/CH slots
    IO_RELEASE                      = 0xA8  # Release previously claimed slots
    IO_OWNER_STATUS                 = 0xA9  # Query ownership table (all 16 slots)
    IO_FORCE_RELEASE                = 0xAA  # Admin: force-release any slot
    PCA_GET_STATUS                  = 0xB0  # Get all PCA9535 state
    PCA_SET_CONTROL                 = 0xB1  # Set named control output
    PCA_SET_PORT                    = 0xB2  # Set raw port value
    PCA_SET_FAULT_CFG               = 0xB3  # Configure fault auto-disable + logging
    PCA_GET_FAULT_LOG               = 0xB4  # Get recent PCA fault events
    EXT_I2C_SETUP                   = 0xB8  # Configure external I2C peripheral pins/frequency
    EXT_I2C_SCAN                    = 0xB9  # Scan external I2C bus
    EXT_I2C_WRITE                   = 0xBA  # Write bytes to external I2C device
    EXT_I2C_READ                    = 0xBB  # Read bytes from external I2C device
    EXT_I2C_WRITE_READ              = 0xBC  # Register-style write then repeated-start read
    EXT_SPI_SETUP                   = 0xBD  # Configure external SPI peripheral pins/frequency
    EXT_SPI_TRANSFER                = 0xBE  # Full-duplex SPI transfer
    START_WAVEGEN                   = 0xD0  # Start waveform generation
    STOP_WAVEGEN                    = 0xD1  # Stop waveform generation
    HAT_GET_STATUS                  = 0xC5  # Get HAT state (detect, type, pin config, power)
    HAT_SET_PIN                     = 0xC6  # Set single EXP_EXT pin function
    HAT_SET_ALL_PINS                = 0xC7  # Set all 4 EXP_EXT pin functions
    HAT_RESET                       = 0xC8  # Reset HAT to default
    HAT_DETECT                      = 0xC9  # Re-run HAT detection
    HAT_SET_POWER                   = 0xCA  # Enable/disable connector (A/B)
    HAT_GET_POWER                   = 0xCB  # Get power status (both connectors)
    HAT_SET_IO_VOLT                 = 0xCC  # Set I/O voltage (mV)
    HAT_SETUP_SWD                   = 0xCD  # One-call SWD quick-setup
    HAT_GET_CAPS                    = 0xC3
    HAT_GET_RAIL_STATUS             = 0xC4
    HAT_SET_RAIL_VOLTAGE            = 0xB5
    HAT_SET_RAIL_ENABLE             = 0xD2
    HAT_SET_LED_STATE               = 0xD3
    HAT_CALIBRATE_START             = 0xAB
    HAT_CALIBRATE_STATUS            = 0xAC
    HAT_CALIBRATE_IMPORT            = 0xAD
    HAT_SET_IO_BANK                 = 0xAE
    HAT_SET_LEVEL_SHIFT             = 0xAF
    HAT_LA_CONFIG                   = 0xCF  # Configure LA capture
    HAT_LA_ARM                      = 0xD5  # Arm trigger / start capture
    HAT_LA_FORCE                    = 0xD6  # Force trigger
    HAT_LA_STATUS                   = 0xD7  # Get capture status
    HAT_LA_READ                     = 0xD8  # Read captured data chunk
    HAT_LA_SET_ROUTE                = 0xD4
    HAT_LA_STOP                     = 0xD9  # Stop capture
    HAT_LA_TRIGGER                  = 0xDA  # Set trigger condition
    HAT_DETECT_TARGET               = 0xDB  # Actively probe SWD DPIDR (target detect)
    USBPD_GET_STATUS                = 0xC0  # Get USB PD contract status
    USBPD_SELECT_PDO                = 0xC1  # Select PDO voltage
    USBPD_GO                        = 0xC2  # Trigger re-negotiation
    SET_LSHIFT_OE                   = 0xE0  # Set level shifter output enable
    WIFI_GET_STATUS                 = 0xE1  # Get WiFi status
    WIFI_CONNECT                    = 0xE2  # Connect to WiFi network
    WIFI_SCAN                       = 0xE4  # Scan for WiFi networks
    WIFI_SET_AP_PASSWORD            = 0xEF  # Set SoftAP password (persist to NVS, apply live)
    HAT_LA_LOG_ENABLE               = 0xEB  # Enable/disable RP2040 log relay
    HAT_LA_USB_RESET                = 0xED  # Reinitialize vendor bulk endpoint
    HAT_LA_STREAM_START             = 0xEE  # Start LA streaming over vendor bulk
    QS_LIST                         = 0xF0  # List quick setup slots
    QS_GET                          = 0xF1  # Get quick setup JSON
    QS_SAVE                         = 0xF2  # Save current state to slot
    QS_APPLY                        = 0xF3  # Apply slot
    QS_DELETE                       = 0xF4  # Delete slot
    SCRIPT_EVAL                     = 0xF5  # Submit Python source for eval (max 32 KB)
    SCRIPT_STATUS                   = 0xF6  # Get script engine status
    SCRIPT_LOGS                     = 0xF7  # Drain script log ring (up to 1020 bytes)
    SCRIPT_STOP                     = 0xF8  # Request cooperative stop
    SCRIPT_UPLOAD                   = 0xF9  # Upload script file to SPIFFS
    SCRIPT_LIST                     = 0xFA  # List stored script files
    SCRIPT_RUN_FILE                 = 0xFB  # Run a stored script file
    SCRIPT_DELETE                   = 0xFC  # Delete a stored script file
    SCRIPT_AUTORUN                  = 0xFD  # Autorun control (sub-byte multiplex)
    SET_SPI_CLOCK                   = 0xE3  # Set SPI clock speed (Hz, u32)
    DAQ_CONFIG                      = 0xB6  # sub-op multiplexed (see BBP_DAQ_CFG_*)
    DAQ_CAL                         = 0xB7  # sub-op multiplexed (see BBP_DAQ_CAL_*)
    DAQ_MEASURE                     = 0xBF  # no payload -> s3link_daq_status_t
    DAQ_TRIG                        = 0xCE  # sub-op multiplexed (see BBP_DAQ_TRIG_*)
    DEVICE_RESET                    = 0x70
    REGISTER_READ                   = 0x71
    REGISTER_WRITE                  = 0x72
    SET_WATCHDOG                    = 0x73  # Enable/disable AD74416H watchdog timer
    GET_ADMIN_TOKEN                 = 0x74  # Retrieve derived admin token (USB only)
    EXT_JOB_SUBMIT                  = 0x75  # Queue deferred external I2C/SPI operation
    EXT_JOB_GET                     = 0x76  # Poll deferred external I2C/SPI operation
    OTA                             = 0x77  # USB OTA info/upload/rollback control
    STANDBY                         = 0x78
    PING                            = 0xFE
    DISCONNECT                      = 0xFF
    # END GENERATED: bbp-commands

    # Older name for the same opcode.
    SELFTEST_EFUSE_CURRENTS = SELFTEST_SUPPLY_VOLTAGES_CACHED

    # Events (device -> host) share this enum; BBP_EVT_* in bbp.h.
    ADC_DATA_EVT     = 0x80
    SCOPE_DATA_EVT   = 0x81
    ALERT_EVT        = 0x82
    DIN_EVT          = 0x83
    PCA_FAULT_EVT    = 0x84
    ADC_DSP_EVT      = 0x88
    HAT_LA_LOG_EVT   = 0xEC


# ---------------------------------------------------------------------------
# Device error codes (ERR message payload byte 0)
# ---------------------------------------------------------------------------

class ErrorCode(IntEnum):
    """BBP error codes returned in ERR messages (see bbp-protocol.md §3.3).
    
    This enum MUST match the firmware error table exactly (Firmware/ESP32/src/bbp/bbp.h).
    It is GENERATED by Firmware/tools/gen_error_tables.py - do not edit by hand.
    """
    # BEGIN GENERATED: bbp-errors
    INVALID_CMD           = 0x01  # The command is not recognized by the device
    INVALID_CHANNEL       = 0x02  # Channel number is out of range
    INVALID_PARAM         = 0x03  # One or more parameters are invalid
    SPI_FAIL              = 0x04  # SPI communication with the AD74416H failed
    QUEUE_FULL            = 0x05  # Device command queue is full - retry after a short delay
    BUSY                  = 0x06  # Device is busy processing another operation
    INVALID_STATE         = 0x07  # Operation cannot be performed in the current device state
    CRC_FAIL              = 0x08  # Payload CRC check failed
    FRAME_TOO_LARGE       = 0x09  # Command or response payload exceeds the maximum frame size
    STREAM_ACTIVE         = 0x0A  # Cannot configure while streaming is active
    TIMEOUT               = 0x11  # Device operation timed out or no response received
    IO_OWNERSHIP_REQUIRED = 0x12  # IO slot is owned by another session
    ADGS_ROUTE_REJECTED   = 0x13  # MUX route rejected by the U17-S3 / U23 self-test interlock
    UNSUPPORTED_HAT       = 0x14  # Command requires a different HAT type, or no HAT is attached
    # END GENERATED: bbp-errors

# ---------------------------------------------------------------------------
# IO Ownership (BBP v5+)
# ---------------------------------------------------------------------------

class IoOwnerKind(IntEnum):
    """Owner kind codes for the 16-slot IO ownership table."""
    NONE     = 0  # Slot is free
    USB      = 1  # BBP over CDC0
    HTTP     = 2  # /api/* HTTP route
    SCRIPT   = 3  # On-device MicroPython VM
    CLI      = 4  # On-device serial CLI
    INTERNAL = 5  # Selftest / boot / autocal — pre-empts all user owners


class IoClaimStatus(IntEnum):
    """Per-slot status bytes in the IO_CLAIM reply ``[n, status[n]]``.

    These are firmware ``CmdError`` values (cmd_io_owner.cpp). IO_RELEASE
    replies ``[n, released[n]]`` with bools instead.
    """
    OK                = 0x00
    HELD_BY_OTHER     = 11    # CMD_ERR_IO_OWNERSHIP: slot owned by another session


# ---------------------------------------------------------------------------
# Per-command response timeouts (seconds)
# ---------------------------------------------------------------------------
# The default transport timeout suits commands the firmware answers from RAM.
# These do real work first - reprogramming converters, restarting the
# acquisition pipeline, sweeping a calibration, or waiting on the HAT UART
# while the P4 is busy - and timing them out looks exactly like a dead link
# while the device is still working normally. Anything absent uses the
# transport default.
# Keyed by int, not CmdId: CmdId is an IntEnum and the transport looks these up
# with whatever the caller passed, which is often a bare int.
CMD_TIMEOUTS_S: Dict[int, float] = {
    # Bridged to the HAT over UART; the P4 may be mid-restart after an
    # acquisition change or a reboot. Every MCP tool calls this via
    # require_hat(), so a short timeout here masquerades as a wedged link.
    CmdId.HAT_GET_STATUS:      15.0,
    # Writing filter / decimation / sample rate stops and restarts the whole
    # capture path on the P4.
    CmdId.DAQ_CONFIG:          20.0,
    CmdId.DAQ_CAL:             20.0,
    CmdId.DAQ_TRIG:            10.0,
    CmdId.DAQ_MEASURE:         10.0,
    # Calibration sweeps and supply diagnostics measure per point.
    CmdId.HAT_CALIBRATE_START:  60.0,
    # Firmware-sequenced rail power-up: settle (<= 5 s) + discharge + blackout.
    CmdId.RAIL_POWER_UP:        10.0,
    CmdId.HAT_CALIBRATE_IMPORT: 30.0,
    CmdId.SELFTEST_AUTO_CAL:    60.0,
    CmdId.SELFTEST_INT_SUPPLIES: 30.0,
    CmdId.SELFTEST_MEASURE_SUPPLY: 20.0,
    # Arbitrary user code on the device.
    CmdId.SCRIPT_EVAL:          30.0,
}
