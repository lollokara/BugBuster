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
    │   IO_Block 1 (EFUSE1)          IO_Block 2 (EFUSE2)                     │
    │   ┌─────────────────────┐      ┌─────────────────────┐                 │
    │   │ IO 1  — analog/HAT  │      │ IO 4  — analog/HAT  │                 │
    │   │ IO 2  — digital     │      │ IO 5  — digital     │                 │
    │   │ IO 3  — digital     │      │ IO 6  — digital     │                 │
    │   │ VCC   GND           │      │ VCC   GND           │                 │
    │   └─────────────────────┘      └─────────────────────┘                 │
    ├─────────────────────────────────────────────────────────────────────────┤
    │ BLOCK 2 — VADJ2 (3–15 V adjustable, IDAC ch 2)                       │
    │                                                                         │
    │   IO_Block 3 (EFUSE3)          IO_Block 4 (EFUSE4)                     │
    │   ┌─────────────────────┐      ┌─────────────────────┐                 │
    │   │ IO 7  — analog/HAT  │      │ IO 10 — analog/HAT  │                 │
    │   │ IO 8  — digital     │      │ IO 11 — digital     │                 │
    │   │ IO 9  — digital     │      │ IO 12 — digital     │                 │
    │   │ VCC   GND           │      │ VCC   GND           │                 │
    │   └─────────────────────┘      └─────────────────────┘                 │
    └─────────────────────────────────────────────────────────────────────────┘

    VLOGIC: 1.8–5 V adjustable (IDAC ch 0, TPS74601) — common to both blocks.
    All digital IOs are level-shifted to VLOGIC via TXS0108E (OE = GPIO14).

IO capabilities
---------------
Each IO is connected through the ADGS2414D MUX matrix.  Options are
**exclusive** — only one function per IO at a time.

    ┌─────────────────────────────────────────────────────────────────────────┐
    │ IO 1, 4, 7, 10  (first IO of each IO_Block):                          │
    │   ESP GPIO (high drive) │ ESP GPIO (low drive) │ AD74416H channel │ HAT│
    ├─────────────────────────────────────────────────────────────────────────┤
    │ IO 2, 3, 5, 6, 8, 9, 11, 12  (IOs 2 & 3 of each IO_Block):           │
    │   ESP GPIO (high drive) │ ESP GPIO (low drive)                         │
    └─────────────────────────────────────────────────────────────────────────┘

Serial bridge
-------------
A configurable UART bridge can be routed to any two of the 12 IOs (TX + RX)
via the MUX.  The bridge connects to a secondary serial port managed by
external programs — BugBuster only controls the routing.

Quick start::

    with connect_usb("/dev/ttyACM0") as bb:
        hal = bb.hal
        hal.begin()

        # Supply setup
        hal.set_voltage(rail=1, voltage=12.0)    # VADJ1 → 12 V
        hal.set_vlogic(3.3)                       # logic level → 3.3 V

        # Analog I/O (only on IO 1, 4, 7, 10)
        hal.configure(1, PortMode.ANALOG_OUT)
        hal.write_voltage(1, 5.0)

        hal.configure(4, PortMode.ANALOG_IN)
        print(hal.read_voltage(4))

        # Digital I/O (all 12 IOs)
        hal.configure(2, PortMode.DIGITAL_OUT)
        hal.write_digital(2, True)

        # Serial bridge
        hal.set_serial(tx=3, rx=6)

        hal.shutdown()
"""

import time
import logging
from dataclasses import dataclass, field
from typing import Optional
from enum import IntEnum

from .constants import (
    ChannelFunction, AdcMux, AdcRange, AdcRate,
    VoutRange, CurrentLimit, RtdCurrent,
    PowerControl, GpioMode,
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

#: Analog-capable IOs (position 1 in each IO_Block) — all modes.
ANALOG_IO_MODES = frozenset({
    PortMode.DISABLED,
    PortMode.ANALOG_IN, PortMode.ANALOG_OUT,
    PortMode.CURRENT_IN, PortMode.CURRENT_OUT,
    PortMode.DIGITAL_IN, PortMode.DIGITAL_OUT,
    PortMode.DIGITAL_IN_LOW, PortMode.DIGITAL_OUT_LOW,
    PortMode.RTD, PortMode.HART, PortMode.HAT,
})

#: Digital-only IOs (positions 2 & 3 in each IO_Block).
DIGITAL_IO_MODES = frozenset({
    PortMode.DISABLED,
    PortMode.DIGITAL_IN, PortMode.DIGITAL_OUT,
    PortMode.DIGITAL_IN_LOW, PortMode.DIGITAL_OUT_LOW,
})

#: Which IOs are analog-capable (first IO of each IO_Block).
ANALOG_IOS = frozenset({1, 4, 7, 10})


# ---------------------------------------------------------------------------
# IO routing table entry
# ---------------------------------------------------------------------------

@dataclass
class IORouting:
    """
    Maps a physical IO number (1–12) to hardware resources.

    Attributes
    ----------
    io_num : int
        Physical IO number (1–12).
    block : int
        Power block (1 or 2) — determines which VADJ supply.
    io_block : int
        IO_Block index (1–4) — determines which efuse.
    position : int
        Position within the IO_Block (1=analog-capable, 2 or 3=digital-only).
    channel : int | None
        AD74416H channel (0=A, 1=B, 2=C, 3=D) for analog IOs; None otherwise.
    efuse : PowerControl
        E-fuse enable for this IO_Block's VCC pin.
    supply : PowerControl
        VADJ regulator enable (VADJ1 or VADJ2).
    supply_idac : int
        IDAC channel (1 or 2) that sets the supply voltage.
    valid_modes : frozenset
        Set of PortMode values this IO supports.
    mux_states : dict
        Maps PortMode → ``(device_index, switch_bitmask)`` for the ADGS2414D.
        ``None`` values mean the MUX entry is unknown / TODO.
    """
    io_num:      int
    block:       int
    io_block:    int
    position:    int
    channel:     Optional[int]
    efuse:       PowerControl
    supply:      PowerControl
    supply_idac: int
    valid_modes: frozenset
    mux_states:  dict = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Default routing table
# ---------------------------------------------------------------------------
#
# MUX switch masks are UNKNOWN — marked None (TODO) below.
#
# To determine them:
#   1. Enable MUX + level shifter
#   2. Sweep each of the 32 switches one at a time
#   3. Apply test signal on AD74416H / ESP GPIO, probe physical IO pins
#   4. Record which (device, bitmask) connects to which IO
#
# Or run ``BugBusterHAL.probe_routing()`` with a test setup.

_TODO = None  # placeholder for unknown (device_idx, switch_bitmask)


def _build_analog_mux() -> dict:
    """MUX states for an analog-capable IO (all modes, all TODO)."""
    return {
        PortMode.ANALOG_OUT:     _TODO,
        PortMode.ANALOG_IN:      _TODO,
        PortMode.CURRENT_OUT:    _TODO,
        PortMode.CURRENT_IN:     _TODO,
        PortMode.DIGITAL_OUT:    _TODO,
        PortMode.DIGITAL_IN:     _TODO,
        PortMode.DIGITAL_OUT_LOW:_TODO,
        PortMode.DIGITAL_IN_LOW: _TODO,
        PortMode.RTD:            _TODO,
        PortMode.HART:           _TODO,
        PortMode.HAT:            _TODO,
        PortMode.DISABLED:       None,  # no MUX change needed
    }


def _build_digital_mux() -> dict:
    """MUX states for a digital-only IO (digital modes, all TODO)."""
    return {
        PortMode.DIGITAL_OUT:     _TODO,
        PortMode.DIGITAL_IN:      _TODO,
        PortMode.DIGITAL_OUT_LOW: _TODO,
        PortMode.DIGITAL_IN_LOW:  _TODO,
        PortMode.DISABLED:        None,
    }


DEFAULT_ROUTING: dict[int, IORouting] = {
    # ── BLOCK 1, IO_BLOCK 1 (VADJ1, EFUSE1) ──────────────────────────────
    1:  IORouting(1,  block=1, io_block=1, position=1, channel=0,
                  efuse=PowerControl.EFUSE1, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=ANALOG_IO_MODES,
                  mux_states=_build_analog_mux()),
    2:  IORouting(2,  block=1, io_block=1, position=2, channel=None,
                  efuse=PowerControl.EFUSE1, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),
    3:  IORouting(3,  block=1, io_block=1, position=3, channel=None,
                  efuse=PowerControl.EFUSE1, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),

    # ── BLOCK 1, IO_BLOCK 2 (VADJ1, EFUSE2) ──────────────────────────────
    4:  IORouting(4,  block=1, io_block=2, position=1, channel=1,
                  efuse=PowerControl.EFUSE2, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=ANALOG_IO_MODES,
                  mux_states=_build_analog_mux()),
    5:  IORouting(5,  block=1, io_block=2, position=2, channel=None,
                  efuse=PowerControl.EFUSE2, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),
    6:  IORouting(6,  block=1, io_block=2, position=3, channel=None,
                  efuse=PowerControl.EFUSE2, supply=PowerControl.VADJ1,
                  supply_idac=1, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),

    # ── BLOCK 2, IO_BLOCK 3 (VADJ2, EFUSE3) ──────────────────────────────
    7:  IORouting(7,  block=2, io_block=3, position=1, channel=2,
                  efuse=PowerControl.EFUSE3, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=ANALOG_IO_MODES,
                  mux_states=_build_analog_mux()),
    8:  IORouting(8,  block=2, io_block=3, position=2, channel=None,
                  efuse=PowerControl.EFUSE3, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),
    9:  IORouting(9,  block=2, io_block=3, position=3, channel=None,
                  efuse=PowerControl.EFUSE3, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),

    # ── BLOCK 2, IO_BLOCK 4 (VADJ2, EFUSE4) ──────────────────────────────
    10: IORouting(10, block=2, io_block=4, position=1, channel=3,
                  efuse=PowerControl.EFUSE4, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=ANALOG_IO_MODES,
                  mux_states=_build_analog_mux()),
    11: IORouting(11, block=2, io_block=4, position=2, channel=None,
                  efuse=PowerControl.EFUSE4, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),
    12: IORouting(12, block=2, io_block=4, position=3, channel=None,
                  efuse=PowerControl.EFUSE4, supply=PowerControl.VADJ2,
                  supply_idac=2, valid_modes=DIGITAL_IO_MODES,
                  mux_states=_build_digital_mux()),
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
        Default VADJ output voltage (volts) when IO_Blocks are first
        enabled (default 12.0 V).
    vlogic : float
        Default logic-level voltage (volts) for digital IOs (default 3.3 V).
    adc_rate : AdcRate
        Default ADC sample rate for analog reads (default 200 SPS).
    """

    def __init__(
        self,
        client,
        routing:        dict    = None,
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
        4. Opens all MUX switches (safe state).
        5. Resets all AD74416H channels to HIGH_IMP.

        Parameters
        ----------
        supply_voltage : float, optional
            Override default VADJ voltage (volts).
        vlogic : float, optional
            Override default logic-level voltage (volts, 1.8–5.0 V).
        """
        if supply_voltage is not None:
            self._supply_v = supply_voltage
        if vlogic is not None:
            self._vlogic = vlogic

        bb = self._bb
        log.info("HAL begin — VADJ=%.1f V, VLOGIC=%.1f V", self._supply_v, self._vlogic)

        # 1. Enable ±15 V analog supply (required for AD74416H)
        bb.power_set(PowerControl.V15A, on=True)
        time.sleep(0.05)

        # 2. Enable MUX power + level shifter OE
        bb.power_set(PowerControl.MUX, on=True)
        bb.set_level_shifter_oe(on=True)
        time.sleep(0.02)

        # 3. Set VLOGIC (IDAC channel 0 → TPS74601)
        bb.idac_set_voltage(0, self._vlogic)

        # 4. Open all MUX switches
        self._mux_state = [0, 0, 0, 0]
        bb.mux_set_all(self._mux_state)

        # 5. Reset AD74416H channels to HIGH_IMP
        bb.reset()

        self._powered_up  = True
        self._io_mode     = {io: PortMode.DISABLED for io in self._routing}
        self._supplies_on = set()
        self._efuses_on   = set()
        log.info("HAL ready — 12 IOs available.")

    def shutdown(self) -> None:
        """
        Safe power-down sequence.

        Disables all outputs, opens all MUX switches, then powers down
        in reverse order.
        """
        bb = self._bb
        log.info("HAL shutdown.")

        # 1. Reset AD74416H channels
        bb.reset()

        # 2. Open all MUX switches
        bb.mux_set_all([0, 0, 0, 0])
        self._mux_state = [0, 0, 0, 0]

        # 3. Disable e-fuses
        for ctrl in [PowerControl.EFUSE4, PowerControl.EFUSE3,
                     PowerControl.EFUSE2, PowerControl.EFUSE1]:
            try:
                bb.power_set(ctrl, on=False)
            except Exception:
                pass

        # 4. Disable VADJ supplies
        bb.power_set(PowerControl.VADJ2, on=False)
        bb.power_set(PowerControl.VADJ1, on=False)
        time.sleep(0.05)

        # 5. Disable analog supply
        bb.power_set(PowerControl.V15A, on=False)

        # 6. Disable level shifter + MUX
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
        Configure a physical IO (1–12) for a given operating mode.

        Equivalent to Arduino ``pinMode()``.  Internally:
          1. Validates the mode is supported on the requested IO.
          2. Enables the IO_Block's power supply (VADJ + e-fuse) if needed.
          3. Sets the MUX switches for the correct signal path.
          4. Configures the AD74416H channel or ESP GPIO.

        Parameters
        ----------
        io : int
            Physical IO number (1–12).
        mode : PortMode
            Desired operating mode — see :class:`PortMode` table.
        bipolar : bool
            For ANALOG_OUT / ANALOG_IN: enable ±12 V range (default 0–12 V).
        rtd_ma_1 : bool
            For RTD: use 1 mA excitation (True) or 500 µA (False).

        Raises
        ------
        ValueError
            If *io* is unknown or *mode* is not supported.
        RuntimeError
            If :meth:`begin` has not been called.

        Example::

            hal.configure(1,  PortMode.ANALOG_OUT)       # voltage output
            hal.configure(4,  PortMode.ANALOG_IN)         # voltage input
            hal.configure(7,  PortMode.CURRENT_OUT)       # 4–20 mA source
            hal.configure(10, PortMode.RTD)               # resistance
            hal.configure(2,  PortMode.DIGITAL_OUT)       # digital high-drive
            hal.configure(3,  PortMode.DIGITAL_OUT_LOW)   # digital low-drive
        """
        if not self._powered_up:
            raise RuntimeError("Call hal.begin() before configure()")

        rt = self._get_routing(io)

        if mode not in rt.valid_modes:
            raise ValueError(
                f"IO {io} does not support {mode.name}. "
                f"Valid: {[m.name for m in sorted(rt.valid_modes)]}"
            )

        # ── Safe transition: disable current mode first ──────────────────
        if rt.channel is not None:
            self._bb.set_channel_function(rt.channel, ChannelFunction.HIGH_IMP)
        self._apply_mux(io, PortMode.DISABLED)

        if mode == PortMode.DISABLED:
            self._io_mode[io] = PortMode.DISABLED
            return

        # ── Power on the IO_Block if needed ──────────────────────────────
        self._enable_io_block_power(rt)

        # ── Set MUX routing for the chosen mode ─────────────────────────
        self._apply_mux(io, mode)

        # ── Configure underlying hardware ────────────────────────────────
        if rt.channel is not None and mode in _CHANNEL_MODES:
            self._configure_channel(rt.channel, mode,
                                    bipolar=bipolar, rtd_ma_1=rtd_ma_1)
        # For digital modes on any IO (including analog-capable ones in
        # digital mode), the MUX routes to an ESP GPIO — no AD74416H
        # channel configuration needed; the ESP GPIO direction is handled
        # by the firmware's MUX routing configuration.

        self._io_mode[io] = mode
        log.debug("IO %d → %s (ch=%s, block=%d, io_block=%d)",
                  io, mode.name, rt.channel, rt.block, rt.io_block)

    # ------------------------------------------------------------------
    # Analog read / write (IO 1, 4, 7, 10 only)
    # ------------------------------------------------------------------

    def read_voltage(self, io: int) -> float:
        """Read voltage in volts on *io* (must be ANALOG_IN)."""
        self._require_mode(io, PortMode.ANALOG_IN, "read_voltage")
        return self._bb.get_adc_value(self._routing[io].channel).value

    def write_voltage(self, io: int, voltage: float, *, bipolar: bool = False) -> None:
        """Set output voltage on *io* (must be ANALOG_OUT)."""
        self._require_mode(io, PortMode.ANALOG_OUT, "write_voltage")
        self._bb.set_dac_voltage(self._routing[io].channel, voltage, bipolar=bipolar)

    def read_current(self, io: int) -> float:
        """Read 4–20 mA loop current on *io* (must be CURRENT_IN).  Returns mA."""
        self._require_mode(io, PortMode.CURRENT_IN, "read_current")
        adc = self._bb.get_adc_value(self._routing[io].channel)
        return (adc.value / 12.0) * 1000.0   # V across 12 Ω RSENSE → mA

    def write_current(self, io: int, current_ma: float) -> None:
        """Set 4–20 mA output on *io* (must be CURRENT_OUT).  *current_ma* in mA."""
        self._require_mode(io, PortMode.CURRENT_OUT, "write_current")
        self._bb.set_dac_current(self._routing[io].channel, current_ma)

    def read_resistance(self, io: int) -> float:
        """Read resistance in ohms on *io* (must be RTD mode)."""
        self._require_mode(io, PortMode.RTD, "read_resistance")
        return self._bb.get_adc_value(self._routing[io].channel).value

    def read_temperature_pt100(self, io: int) -> float:
        """Read PT100 RTD temperature in °C on *io* (must be RTD mode)."""
        r = self.read_resistance(io)
        return (r - 100.0) / (100.0 * 3.9083e-3)

    def read_temperature_pt1000(self, io: int) -> float:
        """Read PT1000 RTD temperature in °C on *io* (RTD mode, 500 µA excitation)."""
        r = self.read_resistance(io)
        return (r - 1000.0) / (1000.0 * 3.9083e-3)

    # ------------------------------------------------------------------
    # Digital read / write (all 12 IOs)
    # ------------------------------------------------------------------

    _DIGITAL_READ_MODES  = {PortMode.DIGITAL_IN, PortMode.DIGITAL_IN_LOW}
    _DIGITAL_WRITE_MODES = {PortMode.DIGITAL_OUT, PortMode.DIGITAL_OUT_LOW}

    def read_digital(self, io: int) -> bool:
        """
        Read logic level on *io* (must be DIGITAL_IN or DIGITAL_IN_LOW).
        Returns ``True`` for high, ``False`` for low.
        """
        actual = self._io_mode.get(io, PortMode.DISABLED)
        if actual not in self._DIGITAL_READ_MODES:
            raise RuntimeError(
                f"read_digital() requires IO {io} in DIGITAL_IN or DIGITAL_IN_LOW "
                f"mode, but it is {actual.name}."
            )
        rt = self._routing[io]
        if rt.channel is not None and actual in (PortMode.DIGITAL_IN,):
            # Analog-capable IO in DIN_LOGIC mode on the AD74416H channel
            status  = self._bb.get_status()
            ch_data = status["channels"][rt.channel]
            return bool(ch_data.get("din_state", False))
        else:
            # Digital-only IO or low-drive — read via ESP GPIO
            # The firmware exposes this through the MUX GPIO read mechanism.
            # TODO: implement once the ESP GPIO read path is available
            log.warning("read_digital(IO %d): ESP GPIO read not yet implemented", io)
            return False

    def write_digital(self, io: int, state: bool) -> None:
        """
        Drive *io* high (``True``) or low (``False``).
        Must be DIGITAL_OUT or DIGITAL_OUT_LOW.
        """
        actual = self._io_mode.get(io, PortMode.DISABLED)
        if actual not in self._DIGITAL_WRITE_MODES:
            raise RuntimeError(
                f"write_digital() requires IO {io} in DIGITAL_OUT or DIGITAL_OUT_LOW "
                f"mode, but it is {actual.name}."
            )
        rt = self._routing[io]
        if rt.channel is not None and actual == PortMode.DIGITAL_OUT:
            # Analog-capable IO using AD74416H DO driver (high drive)
            self._bb.set_digital_output(rt.channel, on=state)
        else:
            # Digital-only IO or low-drive — drive via ESP GPIO
            # TODO: implement once the ESP GPIO write path is available
            log.warning("write_digital(IO %d): ESP GPIO write not yet implemented", io)

    # ------------------------------------------------------------------
    # HART (IO 1, 4, 7, 10 only)
    # ------------------------------------------------------------------

    def write_hart_current(self, io: int, current_ma: float) -> None:
        """Set 4–20 mA output on HART port.  *current_ma* in mA."""
        self._require_mode(io, PortMode.HART, "write_hart_current")
        self._bb.set_dac_current(self._routing[io].channel, current_ma)

    def read_hart_current(self, io: int) -> float:
        """Read current on HART port.  Returns mA."""
        self._require_mode(io, PortMode.HART, "read_hart_current")
        adc = self._bb.get_adc_value(self._routing[io].channel)
        return (adc.value / 12.0) * 1000.0

    # ------------------------------------------------------------------
    # Supply voltage control
    # ------------------------------------------------------------------

    def set_voltage(self, rail: int, voltage: float) -> None:
        """
        Set the adjustable supply voltage for a power rail (VADJ).

        ┌────────┬───────────────────────────────────────────────────────┐
        │ rail   │ Description                                           │
        ├────────┼───────────────────────────────────────────────────────┤
        │   1    │ VADJ1 — powers IO_Block 1 & 2 (IO 1–6)              │
        │   2    │ VADJ2 — powers IO_Block 3 & 4 (IO 7–12)             │
        └────────┴───────────────────────────────────────────────────────┘

        Parameters
        ----------
        rail : int
            Supply rail (1 or 2).
        voltage : float
            Target voltage in volts (3–15 V).

        Example::

            hal.set_voltage(1, 12.0)   # IO 1–6 VCC → 12 V
            hal.set_voltage(2, 5.0)    # IO 7–12 VCC → 5 V
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
        Set the logic-level voltage (VLOGIC) for all digital IOs.

        All digital signals are level-shifted to this voltage.

        Parameters
        ----------
        voltage : float
            Logic voltage in volts (1.8–5.0 V).

        Example::

            hal.set_vlogic(3.3)   # standard 3.3 V logic
            hal.set_vlogic(1.8)   # low-voltage logic
            hal.set_vlogic(5.0)   # 5 V TTL compatible
        """
        if not (1.8 <= voltage <= 5.0):
            raise ValueError(f"VLOGIC must be 1.8–5.0 V, got {voltage}")
        self._bb.idac_set_voltage(0, voltage)
        self._vlogic = voltage
        log.debug("VLOGIC → %.2f V", voltage)

    # ------------------------------------------------------------------
    # Serial bridge
    # ------------------------------------------------------------------

    def set_serial(self, tx: int, rx: int) -> None:
        """
        Route the UART serial bridge to two IOs.

        The serial bridge connects to a secondary serial port used by
        external programs to communicate with the device connected to
        these IO pins.  BugBuster only sets up the routing.

        Parameters
        ----------
        tx : int
            IO number (1–12) to use as UART TX output.
        rx : int
            IO number (1–12) to use as UART RX input.

        Example::

            hal.set_serial(tx=3, rx=6)
        """
        if tx not in self._routing:
            raise ValueError(f"Invalid TX IO {tx}. Valid: 1–12")
        if rx not in self._routing:
            raise ValueError(f"Invalid RX IO {rx}. Valid: 1–12")
        if tx == rx:
            raise ValueError("TX and RX must be different IOs")

        # TODO: Call client UART pin routing once implemented.
        # The firmware commands GET_UART_CONFIG (0x50), SET_UART_CONFIG (0x51),
        # and GET_UART_PINS (0x52) handle this, but the client methods are
        # not yet implemented.
        log.warning(
            "set_serial(tx=%d, rx=%d): UART bridge routing not yet implemented "
            "in the client layer. Firmware commands 0x50–0x52 need client methods.",
            tx, rx,
        )

    # ------------------------------------------------------------------
    # Private — MUX routing
    # ------------------------------------------------------------------

    def _apply_mux(self, io: int, mode: PortMode) -> None:
        rt        = self._get_routing(io)
        mux_entry = rt.mux_states.get(mode)

        if mux_entry is None:
            return  # no MUX change needed (DISABLED or explicit None)

        if mux_entry is _TODO:
            log.warning(
                "MUX routing for IO %d mode %s is not configured. "
                "Run probe_routing() or update DEFAULT_ROUTING in hal.py.",
                io, mode.name,
            )
            return

        device_idx, switch_mask = mux_entry
        self._mux_state[device_idx] = switch_mask
        self._bb.mux_set_all(self._mux_state)

    # ------------------------------------------------------------------
    # Private — power management
    # ------------------------------------------------------------------

    def _enable_io_block_power(self, rt: IORouting) -> None:
        """Enable the VADJ supply and e-fuse for an IO_Block."""
        # Enable VADJ regulator if not already on
        if rt.supply not in self._supplies_on:
            self._bb.power_set(rt.supply, on=True)
            self._bb.idac_set_voltage(rt.supply_idac, self._supply_v)
            self._supplies_on.add(rt.supply)
            time.sleep(0.1)

        # Enable e-fuse if not already on
        if rt.efuse not in self._efuses_on:
            self._bb.power_set(rt.efuse, on=True)
            self._efuses_on.add(rt.efuse)

    # ------------------------------------------------------------------
    # Private — AD74416H channel configuration
    # ------------------------------------------------------------------

    def _configure_channel(
        self,
        ch:       int,
        mode:     PortMode,
        *,
        bipolar:  bool = False,
        rtd_ma_1: bool = True,
    ) -> None:
        bb = self._bb

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
    # Private — validation
    # ------------------------------------------------------------------

    def _get_routing(self, io: int) -> IORouting:
        if io not in self._routing:
            raise ValueError(f"Unknown IO {io}. Valid: {sorted(self._routing)}")
        return self._routing[io]

    def _require_mode(self, io: int, expected: PortMode, method: str) -> None:
        actual = self._io_mode.get(io, PortMode.DISABLED)
        if actual != expected:
            raise RuntimeError(
                f"{method}() requires IO {io} in {expected.name} mode, "
                f"but it is {actual.name}. "
                f"Call hal.configure({io}, PortMode.{expected.name}) first."
            )

    # ------------------------------------------------------------------
    # Routing probe utility
    # ------------------------------------------------------------------

    def probe_routing(
        self,
        source_io:    int   = 1,
        test_voltage: float = 3.0,
        threshold:    float = 0.5,
    ) -> dict:
        """
        Semi-automated MUX routing discovery.

        Sets *source_io*'s AD74416H channel to VOUT at *test_voltage*,
        then closes each of the 32 switches one at a time and reads back
        all other analog channels.

        Only works with analog-capable IOs (1, 4, 7, 10).

        Returns ``{(device, switch): {"readings": {io: V}, "hits": {io: V}}}``
        to help fill in the routing table.

        .. warning::
            Only use with no external signals connected.
        """
        if source_io not in ANALOG_IOS:
            raise ValueError(f"probe_routing needs analog IO (1/4/7/10), got {source_io}")

        bb      = self._bb
        results = {}
        src_ch  = self._routing[source_io].channel

        # Source → VOUT
        bb.set_channel_function(src_ch, ChannelFunction.VOUT)
        bb.set_dac_voltage(src_ch, test_voltage)

        # Other analog IOs → VIN
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
        print("\nProbe complete. Update DEFAULT_ROUTING.mux_states with results.")
        return results


# ---------------------------------------------------------------------------
# Channel function mapping for analog-capable IOs
# ---------------------------------------------------------------------------

#: Modes that require AD74416H channel configuration.
_CHANNEL_MODES = {
    PortMode.ANALOG_IN, PortMode.ANALOG_OUT,
    PortMode.CURRENT_IN, PortMode.CURRENT_OUT,
    PortMode.DIGITAL_IN, PortMode.DIGITAL_OUT,
    PortMode.RTD, PortMode.HART,
}

#: Maps PortMode → AD74416H ChannelFunction.
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
