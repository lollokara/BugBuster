"""
BugBuster Hardware Abstraction Layer (HAL)
==========================================

Arduino-style port API for the BugBuster industrial I/O board.

Physical layout
---------------
The board has **2 BLOCKs**, each containing **2 IO_BLOCKs** of **3 IOs**,
giving **12 IOs total**.  Each IO_BLOCK also exposes a VCC pin (from its
e-fuse output) and a GND pin.

    ┌─────────────────────────────────────────────────────────────────────────┐
    │ BLOCK 1 — VADJ1 (3–15 V adjustable, IDAC ch 1)                       │
    │                                                                         │
    │   IO_Block 1 (EFUSE1, MUX U10)    IO_Block 2 (EFUSE2, MUX U11)       │
    │   ┌─────────────────────┐          ┌─────────────────────┐             │
    │   │ IO 1  — analog/HAT  │          │ IO 4  — analog/HAT  │             │
    │   │ IO 2  — digital     │          │ IO 5  — digital     │             │
    │   │ IO 3  — digital     │          │ IO 6  — digital     │             │
    │   │ VCC   GND           │          │ VCC   GND           │             │
    │   └─────────────────────┘          └─────────────────────┘             │
    ├─────────────────────────────────────────────────────────────────────────┤
    │ BLOCK 2 — VADJ2 (3–15 V adjustable, IDAC ch 2)                       │
    │                                                                         │
    │   IO_Block 3 (EFUSE3, MUX U16)    IO_Block 4 (EFUSE4, MUX U17)       │
    │   ┌─────────────────────┐          ┌─────────────────────┐             │
    │   │ IO 7  — analog/HAT  │          │ IO 10 — analog/HAT  │             │
    │   │ IO 8  — digital     │          │ IO 11 — digital     │             │
    │   │ IO 9  — digital     │          │ IO 12 — digital     │             │
    │   │ VCC   GND           │          │ VCC   GND           │             │
    │   └─────────────────────┘          └─────────────────────┘             │
    └─────────────────────────────────────────────────────────────────────────┘

    VLOGIC: 1.8–5 V adjustable (IDAC ch 0, TPS74601) — common to both blocks.
    All digital IOs are level-shifted to VLOGIC via TXS0108E (OE = GPIO14).

MUX switch matrix
-----------------
Each IO_Block is served by one ADGS2414D (8 SPST switches):

    ┌─────────┬────────────────┬───────────────────────────────────────────┐
    │ Group   │ Switches       │ IO / function                             │
    ├─────────┼────────────────┼───────────────────────────────────────────┤
    │ A       │ S1–S4 (bits 0-3)│ Analog-capable IO (pos. 1 in IO_Block)   │
    │         │                │  S1 = ESP GPIO high drive                 │
    │         │                │  S2 = AD74416H channel (analog modes)     │
    │         │                │  S3 = ESP GPIO low drive                  │
    │         │                │  S4 = HAT passthrough                     │
    ├─────────┼────────────────┼───────────────────────────────────────────┤
    │ B       │ S5–S6 (bits 4-5)│ Digital IO (pos. 2 in IO_Block)          │
    │         │                │  S5 = ESP GPIO high drive                 │
    │         │                │  S6 = ESP GPIO low drive                  │
    ├─────────┼────────────────┼───────────────────────────────────────────┤
    │ C       │ S7–S8 (bits 6-7)│ Digital IO (pos. 3 in IO_Block)          │
    │         │                │  S7 = ESP GPIO high drive                 │
    │         │                │  S8 = ESP GPIO low drive                  │
    └─────────┴────────────────┴───────────────────────────────────────────┘

    NOTE: Switch-to-function mapping above is derived from firmware
    MUX_GPIO_MAP and group structure.  Verify S1–S4 assignment order
    on the actual PCB if analog modes don't work correctly.

Quick start::

    with connect_usb("/dev/ttyACM0") as bb:
        hal = bb.hal
        hal.begin()

        hal.set_voltage(rail=1, voltage=12.0)
        hal.set_vlogic(3.3)

        hal.configure(1, PortMode.ANALOG_OUT)
        hal.write_voltage(1, 5.0)

        hal.configure(4, PortMode.ANALOG_IN)
        print(hal.read_voltage(4))

        hal.configure(2, PortMode.DIGITAL_OUT)
        hal.write_digital(2, True)

        hal.set_serial(tx=3, rx=6)
        hal.shutdown()
"""

import time
import logging
from dataclasses import dataclass
from typing import Optional
from enum import IntEnum

from .constants import (
    ChannelFunction, AdcMux, AdcRange, AdcRate,
    VoutRange, CurrentLimit, RtdCurrent,
    PowerControl,
)

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Port mode enum
# ---------------------------------------------------------------------------

class PortMode(IntEnum):
    """
    Operating mode for a physical IO (1–12).

    ┌──────────────────┬────────────────────────────────┬──────────┬──────────┐
    │ Mode             │ Description                    │ IO 1,4,  │ IO 2,3,  │
    │                  │                                │ 7,10     │ 5,6,8,9, │
    │                  │                                │ (analog) │ 11,12    │
    ├──────────────────┼────────────────────────────────┼──────────┼──────────┤
    │ DISABLED         │ Safe default / disconnected     │ ✓        │ ✓        │
    │ ANALOG_IN        │ ADC voltage input (0–12 V)     │ ✓        │ ✗        │
    │ ANALOG_OUT       │ DAC voltage output (0–12 V)    │ ✓        │ ✗        │
    │ CURRENT_IN       │ 4–20 mA input (ext. powered)   │ ✓        │ ✗        │
    │ CURRENT_OUT      │ 4–20 mA current source         │ ✓        │ ✗        │
    │ DIGITAL_IN       │ GPIO input — high drive         │ ✓        │ ✓        │
    │ DIGITAL_OUT      │ GPIO output — high drive        │ ✓        │ ✓        │
    │ DIGITAL_IN_LOW   │ GPIO input — low drive          │ ✓        │ ✓        │
    │ DIGITAL_OUT_LOW  │ GPIO output — low drive         │ ✓        │ ✓        │
    │ RTD              │ Resistance / PT100 measurement  │ ✓        │ ✗        │
    │ HART             │ HART modem overlay (4–20 mA)    │ ✓        │ ✗        │
    │ HAT              │ Passthrough to HAT expansion    │ ✓        │ ✗        │
    └──────────────────┴────────────────────────────────┴──────────┴──────────┘
    """
    DISABLED        = 0
    ANALOG_IN       = 1
    ANALOG_OUT      = 2
    CURRENT_IN      = 3
    CURRENT_OUT     = 4
    DIGITAL_IN      = 5
    DIGITAL_OUT     = 6
    DIGITAL_IN_LOW  = 7
    DIGITAL_OUT_LOW = 8
    RTD             = 9
    HART            = 10
    HAT             = 11


# ---------------------------------------------------------------------------
# Capability sets
# ---------------------------------------------------------------------------

ANALOG_IO_MODES = frozenset({
    PortMode.DISABLED,
    PortMode.ANALOG_IN, PortMode.ANALOG_OUT,
    PortMode.CURRENT_IN, PortMode.CURRENT_OUT,
    PortMode.DIGITAL_IN, PortMode.DIGITAL_OUT,
    PortMode.DIGITAL_IN_LOW, PortMode.DIGITAL_OUT_LOW,
    PortMode.RTD, PortMode.HART, PortMode.HAT,
})

DIGITAL_IO_MODES = frozenset({
    PortMode.DISABLED,
    PortMode.DIGITAL_IN, PortMode.DIGITAL_OUT,
    PortMode.DIGITAL_IN_LOW, PortMode.DIGITAL_OUT_LOW,
})

ANALOG_IOS = frozenset({1, 4, 7, 10})


# ---------------------------------------------------------------------------
# MUX switch bit assignments
# ---------------------------------------------------------------------------
# Derived from firmware adgs2414d.h MUX_GPIO_MAP and group masks.
#
# Group A (bits 0–3): analog-capable IO (position 1 in each IO_Block)
#   S1 (bit 0) = ESP GPIO high drive
#   S2 (bit 1) = AD74416H channel (for all analog/current/RTD/HART modes)
#   S3 (bit 2) = ESP GPIO low drive
#   S4 (bit 3) = HAT passthrough
#
# Group B (bits 4–5): digital IO (position 2)
#   S5 (bit 4) = ESP GPIO high drive
#   S6 (bit 5) = ESP GPIO low drive
#
# Group C (bits 6–7): digital IO (position 3)
#   S7 (bit 6) = ESP GPIO high drive
#   S8 (bit 7) = ESP GPIO low drive

_SW_A_ESP_HIGH = 0x01   # S1 — Group A ESP high drive
_SW_A_ADC      = 0x02   # S2 — Group A AD74416H channel
_SW_A_ESP_LOW  = 0x04   # S3 — Group A ESP low drive
_SW_A_HAT      = 0x08   # S4 — Group A HAT passthrough
_SW_B_ESP_HIGH = 0x10   # S5 — Group B ESP high drive
_SW_B_ESP_LOW  = 0x20   # S6 — Group B ESP low drive
_SW_C_ESP_HIGH = 0x40   # S7 — Group C ESP high drive
_SW_C_ESP_LOW  = 0x80   # S8 — Group C ESP low drive


def _analog_mux() -> dict:
    """MUX states for an analog-capable IO (Group A, 4 options)."""
    adc = _SW_A_ADC
    return {
        PortMode.ANALOG_IN:       adc,
        PortMode.ANALOG_OUT:      adc,
        PortMode.CURRENT_IN:      adc,
        PortMode.CURRENT_OUT:     adc,
        PortMode.RTD:             adc,
        PortMode.HART:            adc,
        PortMode.DIGITAL_IN:      _SW_A_ESP_HIGH,
        PortMode.DIGITAL_OUT:     _SW_A_ESP_HIGH,
        PortMode.DIGITAL_IN_LOW:  _SW_A_ESP_LOW,
        PortMode.DIGITAL_OUT_LOW: _SW_A_ESP_LOW,
        PortMode.HAT:             _SW_A_HAT,
        PortMode.DISABLED:        0,
    }


def _digital_mux_b() -> dict:
    """MUX states for digital IO at position 2 (Group B)."""
    return {
        PortMode.DIGITAL_IN:      _SW_B_ESP_HIGH,
        PortMode.DIGITAL_OUT:     _SW_B_ESP_HIGH,
        PortMode.DIGITAL_IN_LOW:  _SW_B_ESP_LOW,
        PortMode.DIGITAL_OUT_LOW: _SW_B_ESP_LOW,
        PortMode.DISABLED:        0,
    }


def _digital_mux_c() -> dict:
    """MUX states for digital IO at position 3 (Group C)."""
    return {
        PortMode.DIGITAL_IN:      _SW_C_ESP_HIGH,
        PortMode.DIGITAL_OUT:     _SW_C_ESP_HIGH,
        PortMode.DIGITAL_IN_LOW:  _SW_C_ESP_LOW,
        PortMode.DIGITAL_OUT_LOW: _SW_C_ESP_LOW,
        PortMode.DISABLED:        0,
    }


# ---------------------------------------------------------------------------
# ESP32 GPIO pin mapping (from firmware MUX_GPIO_MAP in adgs2414d.h)
# ---------------------------------------------------------------------------
# Maps IO number → ESP32 GPIO pin used for the digital drive signal.
# This pin is routed through the MUX to the physical IO terminal.
#
ESP_GPIO_MAP: dict[int, int] = {
    # From MUX_GPIO_MAP[0] — U10 (IO_Block 1)
    1:  1,    # ESP GPIO 1
    2:  2,    # ESP GPIO 2
    3:  4,    # ESP GPIO 4
    # From MUX_GPIO_MAP[1] — U11 (IO_Block 2)
    4:  5,    # ESP GPIO 5
    5:  6,    # ESP GPIO 6
    6:  7,    # ESP GPIO 7
    # From MUX_GPIO_MAP[3] — U17 (IO_Block 3)
    7:  10,   # ESP GPIO 10
    8:  9,    # ESP GPIO 9
    9:  8,    # ESP GPIO 8
    # From MUX_GPIO_MAP[2] — U16 (IO_Block 4)
    10: 13,   # ESP GPIO 13
    11: 12,   # ESP GPIO 12
    12: 11,   # ESP GPIO 11
}


# ---------------------------------------------------------------------------
# IO routing table entry
# ---------------------------------------------------------------------------

@dataclass
class IORouting:
    """Maps a physical IO (1–12) to hardware resources."""
    io_num:      int
    block:       int                    # Power block (1 or 2)
    io_block:    int                    # IO_Block (1–4)
    position:    int                    # Position within IO_Block (1=analog, 2–3=digital)
    channel:     Optional[int]          # AD74416H channel (0–3) or None
    mux_device:  int                    # ADGS2414D device index (0–3)
    mux_map:     dict                   # PortMode → switch bitmask
    esp_gpio:    int                    # ESP32 GPIO pin for digital drive
    efuse:       PowerControl
    supply:      PowerControl
    supply_idac: int                    # IDAC channel (1 or 2)
    valid_modes: frozenset


# ---------------------------------------------------------------------------
# Default routing table
# ---------------------------------------------------------------------------

DEFAULT_ROUTING: dict[int, IORouting] = {
    # ── BLOCK 1, IO_BLOCK 1 — device 0 (U10), VADJ1, EFUSE1 ─────────────
    1:  IORouting(1,  block=1, io_block=1, position=1, channel=0,
                  mux_device=0, mux_map=_analog_mux(),    esp_gpio=1,
                  efuse=PowerControl.EFUSE1, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=ANALOG_IO_MODES),
    2:  IORouting(2,  block=1, io_block=1, position=2, channel=None,
                  mux_device=0, mux_map=_digital_mux_b(), esp_gpio=2,
                  efuse=PowerControl.EFUSE1, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES),
    3:  IORouting(3,  block=1, io_block=1, position=3, channel=None,
                  mux_device=0, mux_map=_digital_mux_c(), esp_gpio=4,
                  efuse=PowerControl.EFUSE1, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES),

    # ── BLOCK 1, IO_BLOCK 2 — device 1 (U11), VADJ1, EFUSE2 ─────────────
    4:  IORouting(4,  block=1, io_block=2, position=1, channel=1,
                  mux_device=1, mux_map=_analog_mux(),    esp_gpio=5,
                  efuse=PowerControl.EFUSE2, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=ANALOG_IO_MODES),
    5:  IORouting(5,  block=1, io_block=2, position=2, channel=None,
                  mux_device=1, mux_map=_digital_mux_b(), esp_gpio=6,
                  efuse=PowerControl.EFUSE2, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES),
    6:  IORouting(6,  block=1, io_block=2, position=3, channel=None,
                  mux_device=1, mux_map=_digital_mux_c(), esp_gpio=7,
                  efuse=PowerControl.EFUSE2, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES),

    # ── BLOCK 2, IO_BLOCK 3 — device 3 (U17), VADJ2, EFUSE4 ─────────────
    # PCB swap: physical connector 3 is wired to EFUSE4 (silkscreen EFUSE3↔EFUSE4 crossed)
    7:  IORouting(7,  block=2, io_block=3, position=1, channel=3,
                  mux_device=3, mux_map=_analog_mux(),    esp_gpio=10,
                  efuse=PowerControl.EFUSE4, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=ANALOG_IO_MODES),
    8:  IORouting(8,  block=2, io_block=3, position=2, channel=None,
                  mux_device=3, mux_map=_digital_mux_b(), esp_gpio=9,
                  efuse=PowerControl.EFUSE4, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES),
    9:  IORouting(9,  block=2, io_block=3, position=3, channel=None,
                  mux_device=3, mux_map=_digital_mux_c(), esp_gpio=8,
                  efuse=PowerControl.EFUSE4, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES),

    # ── BLOCK 2, IO_BLOCK 4 — device 2 (U16), VADJ2, EFUSE3 ─────────────
    # PCB swap: physical connector 4 is wired to EFUSE3
    10: IORouting(10, block=2, io_block=4, position=1, channel=2,
                  mux_device=2, mux_map=_analog_mux(),    esp_gpio=13,
                  efuse=PowerControl.EFUSE3, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=ANALOG_IO_MODES),
    11: IORouting(11, block=2, io_block=4, position=2, channel=None,
                  mux_device=2, mux_map=_digital_mux_b(), esp_gpio=12,
                  efuse=PowerControl.EFUSE3, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES),
    12: IORouting(12, block=2, io_block=4, position=3, channel=None,
                  mux_device=2, mux_map=_digital_mux_c(), esp_gpio=11,
                  efuse=PowerControl.EFUSE3, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES),
}


# ---------------------------------------------------------------------------
# HAL class
# ---------------------------------------------------------------------------

class BugBusterHAL:
    """
    Hardware Abstraction Layer — Arduino-style IO API for BugBuster.

    Access via ``bb.hal`` rather than instantiating directly.

    Parameters
    ----------
    client : BugBuster
        Parent BugBuster client instance.
    routing : dict, optional
        Custom routing table overriding :data:`DEFAULT_ROUTING`.
    supply_voltage : float
        Default VADJ voltage (volts) when IO_Blocks are first enabled.
    vlogic : float
        Default logic-level voltage (volts) for all digital IOs.
    adc_rate : AdcRate
        Default ADC sample rate for analog reads.
    """

    def __init__(
        self,
        client,
        routing:        dict | None    = None,
        supply_voltage: float   = 12.0,
        vlogic:         float   = 3.3,
        adc_rate:       AdcRate = AdcRate.SPS_200_H,
    ):
        self._bb            = client
        self._routing       = routing or DEFAULT_ROUTING
        self._supply_v      = supply_voltage
        self._vlogic        = vlogic
        self._adc_rate      = adc_rate
        self._io_mode:      dict[int, PortMode]  = {}
        self._mux_state:    list[int]            = [0, 0, 0, 0]
        self._powered_up    = False
        self._supplies_on:  set[PowerControl]    = set()
        self._efuses_on:    set[PowerControl]    = set()

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def begin(
        self,
        supply_voltage: float = None,
        vlogic:         float = None,
    ) -> None:
        """
        Power-up sequence — call once before :meth:`configure`.

        1. Enables ±15 V analog supply (AD74416H AVDD).
        2. Enables MUX switch matrix + level shifters.
        3. Sets VLOGIC to the requested level.
        4. Opens all MUX switches.
        5. Resets all AD74416H channels to HIGH_IMP.

        Parameters
        ----------
        supply_voltage : float, optional
            Override default VADJ voltage.
        vlogic : float, optional
            Override default logic-level voltage (1.8–5.0 V).
        """
        if supply_voltage is not None:
            self._supply_v = supply_voltage
        if vlogic is not None:
            self._vlogic = vlogic

        bb = self._bb
        log.info("HAL begin — VADJ=%.1f V, VLOGIC=%.1f V", self._supply_v, self._vlogic)

        bb.power_set(PowerControl.V15A, on=True)
        time.sleep(0.05)

        bb.power_set(PowerControl.MUX, on=True)
        bb.set_level_shifter_oe(on=True)
        time.sleep(0.02)

        bb.idac_set_voltage(0, self._vlogic)

        self._mux_state = [0, 0, 0, 0]
        bb.mux_set_all(self._mux_state)

        bb.reset()

        self._powered_up  = True
        self._io_mode     = {io: PortMode.DISABLED for io in self._routing}
        self._supplies_on = set()
        self._efuses_on   = set()
        log.info("HAL ready — 12 IOs available.")

    def shutdown(self) -> None:
        """Safe power-down: disable outputs, open MUX, power off rails."""
        bb = self._bb
        log.info("HAL shutdown.")

        bb.reset()
        bb.mux_set_all([0, 0, 0, 0])
        self._mux_state = [0, 0, 0, 0]

        for ctrl in [PowerControl.EFUSE4, PowerControl.EFUSE3,
                     PowerControl.EFUSE2, PowerControl.EFUSE1]:
            try:
                bb.power_set(ctrl, on=False)
            except Exception:
                pass

        bb.power_set(PowerControl.VADJ2, on=False)
        bb.power_set(PowerControl.VADJ1, on=False)
        time.sleep(0.05)

        bb.power_set(PowerControl.V15A, on=False)
        bb.set_level_shifter_oe(on=False)
        bb.power_set(PowerControl.MUX, on=False)

        self._powered_up  = False
        self._supplies_on = set()
        self._efuses_on   = set()
        self._io_mode     = {}

    # ------------------------------------------------------------------
    # IO configuration
    # ------------------------------------------------------------------

    def configure(
        self,
        io:       int,
        mode:     PortMode,
        *,
        bipolar:  bool = False,
        rtd_ma_1: bool = True,
    ) -> None:
        """
        Configure a physical IO (1–12) — equivalent to Arduino ``pinMode()``.

        Parameters
        ----------
        io : int
            Physical IO number (1–12).
        mode : PortMode
            Desired operating mode.
        bipolar : bool
            For ANALOG_OUT/IN: enable ±12 V range (default 0–12 V).
        rtd_ma_1 : bool
            For RTD: 1 mA excitation (True) or 500 µA (False).

        Example::

            hal.configure(1,  PortMode.ANALOG_OUT)       # voltage output
            hal.configure(4,  PortMode.ANALOG_IN)         # voltage input
            hal.configure(2,  PortMode.DIGITAL_OUT)       # digital high-drive
            hal.configure(3,  PortMode.DIGITAL_OUT_LOW)   # digital low-drive
            hal.configure(10, PortMode.RTD)               # resistance measurement
        """
        if not self._powered_up:
            raise RuntimeError("Call hal.begin() before configure()")

        rt = self._get_routing(io)

        if mode not in rt.valid_modes:
            raise ValueError(
                f"IO {io} does not support {mode.name}. "
                f"Valid: {[m.name for m in sorted(rt.valid_modes)]}"
            )

        # ── Disable current mode ─────────────────────────────────────────
        if rt.channel is not None:
            self._bb.set_channel_function(rt.channel, ChannelFunction.HIGH_IMP)
        self._set_mux(rt, PortMode.DISABLED)

        if mode == PortMode.DISABLED:
            self._io_mode[io] = PortMode.DISABLED
            return

        # ── Power on the IO_Block ─────────────────────────────────────────
        self._enable_io_block_power(rt)

        # ── HAT presence check ────────────────────────────────────────────
        if mode == PortMode.HAT:
            self._bb._require_hat_present()

        # ── Set MUX routing ───────────────────────────────────────────────
        self._set_mux(rt, mode)

        # ── Configure underlying hardware ─────────────────────────────────
        if rt.channel is not None and mode in _CHANNEL_MODES:
            self._configure_channel(rt.channel, mode,
                                    bipolar=bipolar, rtd_ma_1=rtd_ma_1)

        self._io_mode[io] = mode
        log.debug("IO %d → %s (dev=%d mask=0x%02X ch=%s)",
                  io, mode.name, rt.mux_device,
                  rt.mux_map.get(mode, 0), rt.channel)

    # ------------------------------------------------------------------
    # Analog read / write (IO 1, 4, 7, 10 only)
    # ------------------------------------------------------------------

    def read_voltage(self, io: int) -> float:
        """Read voltage in volts (ANALOG_IN)."""
        self._require_mode(io, PortMode.ANALOG_IN, "read_voltage")
        return self._bb.get_adc_value(self._routing[io].channel).value

    def write_voltage(self, io: int, voltage: float, *, bipolar: bool = False) -> None:
        """Set output voltage (ANALOG_OUT)."""
        self._require_mode(io, PortMode.ANALOG_OUT, "write_voltage")
        self._bb.set_dac_voltage(self._routing[io].channel, voltage, bipolar=bipolar)

    def read_current(self, io: int) -> float:
        """Read 4–20 mA loop current in mA (CURRENT_IN)."""
        self._require_mode(io, PortMode.CURRENT_IN, "read_current")
        adc = self._bb.get_adc_value(self._routing[io].channel)
        return (adc.value / 12.0) * 1000.0

    def write_current(self, io: int, current_ma: float) -> None:
        """Set 4–20 mA output in mA (CURRENT_OUT)."""
        self._require_mode(io, PortMode.CURRENT_OUT, "write_current")
        self._bb.set_dac_current(self._routing[io].channel, current_ma)

    def read_resistance(self, io: int) -> float:
        """Read resistance in ohms (RTD)."""
        self._require_mode(io, PortMode.RTD, "read_resistance")
        return self._bb.get_adc_value(self._routing[io].channel).value

    def read_temperature_pt100(self, io: int) -> float:
        """Read PT100 temperature in °C (RTD, 1 mA excitation)."""
        r = self.read_resistance(io)
        return (r - 100.0) / (100.0 * 3.9083e-3)

    def read_temperature_pt1000(self, io: int) -> float:
        """Read PT1000 temperature in °C (RTD, 500 µA excitation)."""
        r = self.read_resistance(io)
        return (r - 1000.0) / (1000.0 * 3.9083e-3)

    # ------------------------------------------------------------------
    # Digital read / write (all 12 IOs)
    # ------------------------------------------------------------------

    _DIGITAL_READ_MODES  = {PortMode.DIGITAL_IN, PortMode.DIGITAL_IN_LOW}
    _DIGITAL_WRITE_MODES = {PortMode.DIGITAL_OUT, PortMode.DIGITAL_OUT_LOW}

    def read_digital(self, io: int) -> bool:
        """
        Read logic level (DIGITAL_IN or DIGITAL_IN_LOW).
        Returns True for high, False for low.

        For analog-capable IOs in DIGITAL_IN mode: reads via AD74416H DIN
        comparator.  For other cases: reads via ESP GPIO through the MUX.
        """
        actual = self._io_mode.get(io, PortMode.DISABLED)
        if actual not in self._DIGITAL_READ_MODES:
            raise RuntimeError(
                f"read_digital() requires IO {io} in DIGITAL_IN or DIGITAL_IN_LOW, "
                f"got {actual.name}."
            )
        rt = self._routing[io]
        if rt.channel is not None and actual == PortMode.DIGITAL_IN:
            status  = self._bb.get_status()
            ch_data = status["channels"][rt.channel]
            return bool(ch_data.get("din_state", False))
        else:
            log.debug("read_digital(IO %d)", io)
            # Ensure GPIO is configured as input before reading
            self._bb.dio_configure(io, 1)  # DIO_MODE_INPUT
            resp = self._bb.dio_read(io)
            if resp and isinstance(resp, dict):
                return bool(resp.get("value", False))
            return False

    def write_digital(self, io: int, state: bool) -> None:
        """
        Drive IO high (True) or low (False).
        Must be DIGITAL_OUT or DIGITAL_OUT_LOW.

        For analog-capable IOs in DIGITAL_OUT mode using the AD74416H channel:
        drives via DO (digital output) driver.  For all other cases: logical
        IO number is used to drive the pin.
        """
        actual = self._io_mode.get(io, PortMode.DISABLED)
        if actual not in self._DIGITAL_WRITE_MODES:
            raise RuntimeError(
                f"write_digital() requires IO {io} in DIGITAL_OUT or DIGITAL_OUT_LOW, "
                f"got {actual.name}."
            )
        rt = self._routing[io]
        if rt.channel is not None and actual == PortMode.DIGITAL_OUT:
            self._bb.set_digital_output(rt.channel, on=state)
        else:
            log.debug("write_digital(IO %d, %s)", io, state)
            # Ensure GPIO is configured as output before writing
            self._bb.dio_configure(io, 2)  # DIO_MODE_OUTPUT
            self._bb.dio_write(io, state)

    # ------------------------------------------------------------------
    # HART (IO 1, 4, 7, 10)
    # ------------------------------------------------------------------

    def write_hart_current(self, io: int, current_ma: float) -> None:
        """Set 4–20 mA on HART port (HART mode)."""
        self._require_mode(io, PortMode.HART, "write_hart_current")
        self._bb.set_dac_current(self._routing[io].channel, current_ma)

    def read_hart_current(self, io: int) -> float:
        """Read current on HART port in mA (HART mode)."""
        self._require_mode(io, PortMode.HART, "read_hart_current")
        adc = self._bb.get_adc_value(self._routing[io].channel)
        return (adc.value / 12.0) * 1000.0

    # ------------------------------------------------------------------
    # Supply voltage control
    # ------------------------------------------------------------------

    def set_voltage(self, rail: int, voltage: float) -> None:
        """
        Set the VADJ supply voltage for a power rail.

        ┌────────┬───────────────────────────────────────────┐
        │ rail   │ Description                               │
        ├────────┼───────────────────────────────────────────┤
        │   1    │ VADJ1 — IO_Blocks 1 & 2 (IO 1–6)        │
        │   2    │ VADJ2 — IO_Blocks 3 & 4 (IO 7–12)       │
        └────────┴───────────────────────────────────────────┘

        *voltage* in volts (3–15 V).
        """
        if rail not in (1, 2):
            raise ValueError(f"rail must be 1 or 2, got {rail!r}")

        supply = PowerControl.VADJ1 if rail == 1 else PowerControl.VADJ2
        if supply not in self._supplies_on:
            self._bb.power_set(supply, on=True)
            self._supplies_on.add(supply)
            time.sleep(0.05)

        self._bb.idac_set_voltage(rail, voltage)
        log.debug("VADJ%d → %.2f V", rail, voltage)

    def set_vlogic(self, voltage: float) -> None:
        """
        Set the logic-level voltage for all digital IOs (1.8–5.0 V).

        All digital signals pass through TXS0108E level shifters to this level.
        """
        if not (1.8 <= voltage <= 5.0):
            raise ValueError(f"VLOGIC must be 1.8–5.0 V, got {voltage}")
        self._bb.idac_set_voltage(0, voltage)
        self._vlogic = voltage
        log.debug("VLOGIC → %.2f V", voltage)

    # ------------------------------------------------------------------
    # Serial bridge
    # ------------------------------------------------------------------

    def set_serial(
        self,
        tx:       int,
        rx:       int,
        baudrate: int  = 115200,
        bridge:   int  = 0,
    ) -> None:
        """
        Route the UART serial bridge to two IOs.

        The MUX is configured to connect the specified IOs to their ESP GPIO
        pins, and the UART bridge is pointed at those GPIO pins.

        Parameters
        ----------
        tx : int
            IO number (1–12) for UART TX output.
        rx : int
            IO number (1–12) for UART RX input.
        baudrate : int
            Baud rate (default 115200).
        bridge : int
            Bridge index (0 or 1, default 0).

        Example::

            hal.set_serial(tx=3, rx=6)
            hal.set_serial(tx=2, rx=5, baudrate=9600)
        """
        if tx not in self._routing:
            raise ValueError(f"Invalid TX IO {tx}")
        if rx not in self._routing:
            raise ValueError(f"Invalid RX IO {rx}")
        if tx == rx:
            raise ValueError("TX and RX must be different IOs")

        tx_rt = self._routing[tx]
        rx_rt = self._routing[rx]

        # Configure MUX to route TX/RX IOs to their ESP GPIOs (high drive)
        self._enable_io_block_power(tx_rt)
        self._enable_io_block_power(rx_rt)

        # TX needs a digital output path, RX needs digital input path
        self._set_mux(tx_rt, PortMode.DIGITAL_OUT)
        self._set_mux(rx_rt, PortMode.DIGITAL_IN)
        self._io_mode[tx] = PortMode.DIGITAL_OUT
        self._io_mode[rx] = PortMode.DIGITAL_IN

        # Configure UART bridge to use the corresponding ESP GPIO pins
        tx_gpio = tx_rt.esp_gpio
        rx_gpio = rx_rt.esp_gpio

        self._bb.set_uart_config(
            bridge_id=bridge,
            uart_num=bridge + 1,   # UART1 for bridge 0, UART2 for bridge 1
            tx_pin=tx_gpio,
            rx_pin=rx_gpio,
            baudrate=baudrate,
            enabled=True,
        )
        log.info("Serial bridge %d: TX=IO%d (GPIO%d), RX=IO%d (GPIO%d), %d baud",
                 bridge, tx, tx_gpio, rx, rx_gpio, baudrate)

    # ------------------------------------------------------------------
    # Internal — MUX
    # ------------------------------------------------------------------

    def _set_mux(self, rt: IORouting, mode: PortMode) -> None:
        """Apply the MUX switch mask for the given mode on the given IO."""
        mask = rt.mux_map.get(mode, 0)

        # Clear this IO's group bits, then set the new mask
        if rt.position == 1:
            group_clear = 0x0F   # Group A
        elif rt.position == 2:
            group_clear = 0x30   # Group B
        else:
            group_clear = 0xC0   # Group C

        self._mux_state[rt.mux_device] = \
            (self._mux_state[rt.mux_device] & ~group_clear) | mask
        self._bb.mux_set_all(self._mux_state)

    # ------------------------------------------------------------------
    # Internal — power
    # ------------------------------------------------------------------

    def _enable_io_block_power(self, rt: IORouting) -> None:
        if rt.supply not in self._supplies_on:
            self._bb.power_set(rt.supply, on=True)
            self._bb.idac_set_voltage(rt.supply_idac, self._supply_v)
            self._supplies_on.add(rt.supply)
            time.sleep(0.1)
        if rt.efuse not in self._efuses_on:
            self._bb.power_set(rt.efuse, on=True)
            self._efuses_on.add(rt.efuse)

    # ------------------------------------------------------------------
    # Internal — AD74416H channel setup
    # ------------------------------------------------------------------

    def _configure_channel(self, ch, mode, *, bipolar=False, rtd_ma_1=True):
        bb   = self._bb
        func = _MODE_TO_CHAN_FUNC.get(mode)
        if func is None:
            return
        bb.set_channel_function(ch, func)

        if mode == PortMode.ANALOG_IN:
            bb.set_adc_config(
                ch,
                mux    = AdcMux.LF_TO_AGND,
                range_ = AdcRange.V_NEG12_12 if bipolar else AdcRange.V_0_12,
                rate   = self._adc_rate,
            )
        elif mode == PortMode.ANALOG_OUT:
            bb.set_vout_range(ch, VoutRange.BIPOLAR if bipolar else VoutRange.UNIPOLAR)
            bb.set_dac_voltage(ch, 0.0, bipolar=bipolar)
        elif mode in (PortMode.CURRENT_OUT, PortMode.HART):
            bb.set_current_limit(ch, CurrentLimit.MA_25)
            bb.set_dac_current(ch, 4.0)
        elif mode == PortMode.DIGITAL_OUT:
            bb.set_digital_output(ch, on=False)
        elif mode == PortMode.RTD:
            bb.set_rtd_config(ch, RtdCurrent.MA_1 if rtd_ma_1 else RtdCurrent.UA_500)

    # ------------------------------------------------------------------
    # Internal — validation
    # ------------------------------------------------------------------

    def _get_routing(self, io: int) -> IORouting:
        if io not in self._routing:
            raise ValueError(f"Unknown IO {io}. Valid: {sorted(self._routing)}")
        return self._routing[io]

    def _require_mode(self, io: int, expected: PortMode, method: str) -> None:
        actual = self._io_mode.get(io, PortMode.DISABLED)
        if actual != expected:
            raise RuntimeError(
                f"{method}() requires IO {io} in {expected.name}, "
                f"got {actual.name}. Call hal.configure({io}, PortMode.{expected.name})."
            )

    # ------------------------------------------------------------------
    # Routing probe utility
    # ------------------------------------------------------------------

    def probe_routing(self, source_io: int = 1, test_voltage: float = 3.0,
                      threshold: float = 0.5) -> dict:
        """
        Semi-automated MUX routing discovery for analog IOs (1, 4, 7, 10).

        Sets *source_io* to VOUT, then sweeps all 32 switches and reads
        the other analog channels.  Returns hit map to fill in the routing.
        """
        if source_io not in ANALOG_IOS:
            raise ValueError(f"Need analog IO (1/4/7/10), got {source_io}")

        bb      = self._bb
        results = {}
        src_ch  = self._routing[source_io].channel

        bb.set_channel_function(src_ch, ChannelFunction.VOUT)
        bb.set_dac_voltage(src_ch, test_voltage)

        for io_num in ANALOG_IOS:
            if io_num == source_io:
                continue
            ch = self._routing[io_num].channel
            bb.set_channel_function(ch, ChannelFunction.VIN)
            bb.set_adc_config(ch, mux=AdcMux.LF_TO_AGND,
                              range_=AdcRange.V_0_12, rate=AdcRate.SPS_200_H)
        time.sleep(0.5)

        for device in range(4):
            for switch in range(8):
                state = [0, 0, 0, 0]
                state[device] = (1 << switch)
                bb.mux_set_all(state)
                time.sleep(0.15)

                readings = {}
                for io_num in ANALOG_IOS:
                    if io_num == source_io:
                        continue
                    r = bb.get_adc_value(self._routing[io_num].channel)
                    readings[io_num] = round(r.value, 4)

                hits = {p: v for p, v in readings.items() if v >= threshold}
                results[(device, switch)] = {"readings": readings, "hits": hits}
                if hits:
                    print(f"  SW dev={device} S{switch+1}: "
                          f"signal on IOs {list(hits.keys())}  {hits}")

        bb.mux_set_all([0, 0, 0, 0])
        bb.reset()
        print("\nProbe complete.")
        return results


# ---------------------------------------------------------------------------
# Constants — channel function mapping for analog modes
# ---------------------------------------------------------------------------

_CHANNEL_MODES = {
    PortMode.ANALOG_IN, PortMode.ANALOG_OUT,
    PortMode.CURRENT_IN, PortMode.CURRENT_OUT,
    PortMode.DIGITAL_IN, PortMode.DIGITAL_OUT,
    PortMode.RTD, PortMode.HART,
}

_MODE_TO_CHAN_FUNC = {
    PortMode.ANALOG_IN:   ChannelFunction.VIN,
    PortMode.ANALOG_OUT:  ChannelFunction.VOUT,
    PortMode.CURRENT_IN:  ChannelFunction.IIN_EXT_PWR,
    PortMode.CURRENT_OUT: ChannelFunction.IOUT,
    PortMode.DIGITAL_IN:  ChannelFunction.DIN_LOGIC,
    PortMode.DIGITAL_OUT: ChannelFunction.DIN_LOGIC,
    PortMode.RTD:         ChannelFunction.RES_MEAS,
    PortMode.HART:        ChannelFunction.IOUT_HART,
}
