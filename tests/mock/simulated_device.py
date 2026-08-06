"""
SimulatedDevice — in-memory BugBuster device for hardware-free testing.

Handlers register themselves via register(device) in their module.
The device dispatches binary commands to the registered handlers and
routes HTTP requests to http_routes.py.
"""

import importlib
import threading
from dataclasses import dataclass, field
from bugbuster.transport.usb import DeviceError
from bugbuster.constants import ErrorCode
from bugbuster.protocol import BBP_PROTO_VERSION, ESP32_FW_VERSION

PROTO_VERSION = BBP_PROTO_VERSION


@dataclass
class HatState:
    present: bool = False
    rail_voltages: dict = field(default_factory=dict)
    cal_flags: dict = field(default_factory=dict)
    la_active: bool = False


_HANDLER_MODULES = [
    "tests.mock.handlers.core",
    "tests.mock.handlers.channels",
    "tests.mock.handlers.gpio",
    "tests.mock.handlers.mux",
    "tests.mock.handlers.power",
    "tests.mock.handlers.uart",
    "tests.mock.handlers.idac",
    "tests.mock.handlers.misc",
    "tests.mock.handlers.hat",
    "tests.mock.handlers.streaming",
    "tests.mock.handlers.scripts",
    "tests.mock.handlers.io_owner",
    "tests.mock.handlers.bus",
    "tests.mock.handlers.quicksetup",
    "tests.mock.handlers.daq",
]

from tests.mock import http_routes as _http_routes


class SimulatedDevice:
    """Simulated BugBuster device state machine."""

    PROTO_VERSION = PROTO_VERSION

    def __init__(self):
        self.fw_version = ESP32_FW_VERSION
        self.uptime_ms = 0
        self.hat = HatState()
        self.hat_power = [False, False]

        # Channel state: 4 channels, each with mutable fields
        self.channels = [
            {
                "id": i,
                "function": 0,       # HIGH_IMP
                "adc_raw": 0,
                "adc_value": 0.0,
                "adc_range": 0,
                "adc_rate": 0,
                "adc_mux": 0,
                "dac_code": 0,
                "dac_value": 0.0,
                "din_state": False,
                "din_counter": 0,
                "din_threshold": 64,
                "din_thresh_mode": True,
                "din_debounce": 5,
                "din_sink": 10,
                "din_sink_range": False,
                "din_oc_detect": False,
                "din_sc_detect": False,
                "do_state": False,
                "do_mode": 0,
                "vout_bipolar": False,
                "current_limit": 0,
                "avdd_select": 0,
                "channel_alert": 0,
                "channel_alert_mask": 0xFFFF,
                "rtd_excitation_ua": 0,
            }
            for i in range(4)
        ]

        # Logical GPIO/IO state: 12 IOs (IO1..IO12, ids 0..11)
        self.gpio = [{"id": i, "mode": 0, "output": False, "input": False, "pulldown": False} for i in range(12)]

        # Power / PCA9535 state
        self.pca_control = {}

        # UART state (list of bridge configs)
        self.uart_config = [
            {"uart_num": 1, "tx_pin": 4, "rx_pin": 2, "baudrate": 921600,
             "data_bits": 8, "parity": 0, "stop_bits": 0, "enabled": False},
            {"uart_num": 2, "tx_pin": 7, "rx_pin": 6, "baudrate": 921600,
             "data_bits": 8, "parity": 0, "stop_bits": 0, "enabled": False},
        ]

        # IDAC state (4 channels)
        self.idac = [
            {"code": 0, "target_v": 5.0, "actual_v": 5.0,
             "v_min": 3.0, "v_max": 15.0, "step_mv": 100.0, "calibrated": False}
            for _ in range(4)
        ]

        # USB-PD state
        self.usbpd_voltage = 1   # code 1 = 5V

        # Waveform generator state
        self.wavegen_running = False
        self.wavegen_config = None

        # WiFi state
        self.wifi_connected = False

        # Watchdog state
        self.watchdog_enable = False
        self.watchdog_timeout_code = 9

        # SPI clock
        self.spi_clock_hz = 1_000_000

        # On-device scripting state
        self.script_running      = False
        self.script_id           = 0
        self.script_total_runs   = 0
        self.script_total_errors = 0
        self.script_last_error   = ""
        self.script_log_ring     = ""
        # V2-A persistent-mode state
        self.script_mode              = 0    # 0=EPHEMERAL, 1=PERSISTENT
        self.script_globals_bytes     = 0
        self.script_auto_reset_count  = 0
        self.script_last_eval_at_ms   = 0

        # Autorun state (Phase 6b)
        self.autorun_enabled     = False
        self.autorun_script_name = None   # name of the script set as autorun
        self.autorun_io12_high   = False  # simulated IO12 gate level
        self.autorun_last_run_ok = False
        self.autorun_last_run_id = 0

        # DIO state: 12 logical digital IOs (1-indexed in protocol, 0-indexed here)
        self.dio = [{"mode": 0, "output": False, "input": False} for _ in range(12)]

        # MUX state: 4 bytes (one per ADGS2414D), bit n = switch n
        self.mux_states = [0, 0, 0, 0]

        # ADGS mutual-exclusion model (FIX 6):
        # One switch closed per non-selftest ADGS device at a time.
        # adgs_active[i] = switch index currently closed on device i, or None.
        # Device index 3 (U23 selftest) is exempt — multiple switches may be closed.
        # Exposed as device.adgs_active for test assertions.
        self.adgs_active = [None, None, None, None]  # type: list[int | None]

        # Alert / supply state
        self.alert_status = 0
        self.alert_mask = 0xFFFF
        self.supply_alert_status = 0
        self.supply_alert_mask = 0xFFFF
        self.live_status = 0
        self.die_temp_c = 25.0
        self.spi_ok = True
        self.admin_token = "BB-ADMIN-DEBUG"

        # IO Ownership table: 16 slots (IO1..IO12 = 0..11, CH0..CH3 = 12..15)
        # Each entry: {"kind": int, "session_id": int, "token_fp32": int,
        #              "lease_until_ms": int, "purpose_tag": int}
        self.io_owner_table = [
            {"kind": 0, "session_id": 0, "token_fp32": 0, "lease_until_ms": 0, "purpose_tag": 0}
            for _ in range(16)
        ]
        # Monotonic "now" for lease expiry — advanced by tick(now_ms)
        self._now_ms: int = 0
        # USB session ID: bumped once per simulated connect(), not per frame.
        # All BBP frames within one simulated USB session share the same session_id,
        # matching the firmware behavior where session_id is set at USB enumeration.
        self._usb_session_id: int = 1
        self._usb_session_id_counter: int = 1  # monotonic, never reset

        # Event channel (FIX 5): pending events queued by emit_event().
        # Consumed by _drain_events() in tests or delivered to the transport's
        # _fire_event() when a transport is attached.
        self._pending_events: list = []

        # Handler registry: cmd_id (int) -> callable(payload: bytes) -> bytes
        self._handlers: dict = {}

        # Back-reference set by transport
        self._transport = None

        # Streaming support
        self._stream_stop = threading.Event()
        self._stream_thread = None

        # Scope stream state
        self.scope_ch_mask = 0x0F    # active channel bitmask (0x01–0x0F)
        self.adc_diag_paused = False  # True while scope stream is active

        # Config of the running ADC DSP stream, or None when stopped.
        self.dsp_stream = None

        # Zero-arg callables registered by handlers that own a stream thread,
        # so stop_all_streams() can reach threads it has no reference to.
        self._stream_stoppers: list = []

        self._register_all_handlers()

    # ------------------------------------------------------------------
    # USB session lifecycle (FIX 4)
    # ------------------------------------------------------------------

    def connect(self) -> None:
        """Simulate a new USB connection — bumps _usb_session_id once per connect."""
        self._usb_session_id_counter += 1
        self._usb_session_id = self._usb_session_id_counter

    def reset(self) -> None:
        """Simulate device reset — equivalent to a new connect."""
        self.connect()

    # ------------------------------------------------------------------
    # Event channel (FIX 5)
    # ------------------------------------------------------------------

    def emit_event(self, evt_id: int, payload: bytes) -> None:
        """
        Queue an event for consumption by tests or transport delivery.

        Events are stored as (evt_id, payload) tuples in _pending_events.
        If a transport is attached it is also notified via _fire_event().
        """
        self._pending_events.append((evt_id, payload))
        if self._transport is not None and hasattr(self._transport, "_fire_event"):
            self._transport._fire_event(evt_id, payload)

    def _drain_events(self) -> list:
        """
        Return and clear all pending events.

        Each item is a (evt_id: int, payload: bytes) tuple.  Tests call this
        to verify that expected events were emitted.
        """
        events = list(self._pending_events)
        self._pending_events.clear()
        return events

    # ------------------------------------------------------------------
    # Handler registry
    # ------------------------------------------------------------------

    def register_handler(self, cmd_id: int, fn) -> None:
        self._handlers[int(cmd_id)] = fn

    def dispatch(self, cmd_id: int, payload: bytes) -> bytes:
        handler = self._handlers.get(int(cmd_id))
        if handler is None:
            raise DeviceError(ErrorCode.INVALID_CMD, 0)
        try:
            return handler(payload)
        except DeviceError:
            raise
        except Exception as exc:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0) from exc

    def http_dispatch(self, method: str, path: str, params: dict, body: dict, headers: dict = None) -> dict:
        return _http_routes.dispatch(self, method, path, params, body, headers or {})

    # ------------------------------------------------------------------
    # Streaming
    # ------------------------------------------------------------------

    def stop_all_streams(self) -> None:
        self._stream_stop.set()
        if self._stream_thread and self._stream_thread.is_alive():
            self._stream_thread.join(timeout=2.0)
        self._stream_stop.clear()
        for stop in self._stream_stoppers:
            try:
                stop()
            except Exception:
                pass

    def tick(self, now_ms: int) -> None:
        """Advance simulated time and expire stale leases."""
        self._now_ms = now_ms
        for slot in self.io_owner_table:
            if slot["lease_until_ms"] > 0 and now_ms > slot["lease_until_ms"]:
                slot["kind"] = 0
                slot["session_id"] = 0
                slot["token_fp32"] = 0
                slot["lease_until_ms"] = 0
                slot["purpose_tag"] = 0

    # ------------------------------------------------------------------
    # Handler auto-registration
    # ------------------------------------------------------------------

    def _register_all_handlers(self) -> None:
        for mod_path in _HANDLER_MODULES:
            mod = importlib.import_module(mod_path)
            mod.register(self)
