"""
SimulatedDevice — in-memory BugBuster device for hardware-free testing.

Handlers register themselves via register(device) in their module.
The device dispatches binary commands to the registered handlers and
routes HTTP requests to http_routes.py.
"""

import threading
from bugbuster.transport.usb import DeviceError
from bugbuster.constants import ErrorCode


class SimulatedDevice:
    """Simulated BugBuster device state machine."""

    PROTO_VERSION = 4  # must match BBP_PROTO_VERSION in protocol.py

    def __init__(self):
        self.fw_version = (1, 0, 0)
        self.uptime_ms = 0
        self.hat_present = False

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

        # GPIO state: 6 GPIOs
        self.gpio = [{"id": i, "mode": 0, "output": False, "input": False, "pulldown": False} for i in range(6)]

        # Power / PCA9535 state
        self.pca_control = {}

        # UART state (list of bridge configs)
        self.uart_config = [
            {"uart_num": 1, "tx_pin": 17, "rx_pin": 18, "baudrate": 115200,
             "data_bits": 8, "parity": 0, "stop_bits": 0, "enabled": False}
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

        # DIO state: 12 logical digital IOs (1-indexed in protocol, 0-indexed here)
        self.dio = [{"mode": 0, "output": False, "input": False} for _ in range(12)]

        # MUX state: 4 bytes (one per ADGS2414D), bit n = switch n
        self.mux_states = [0, 0, 0, 0]

        # Alert / supply state
        self.alert_status = 0
        self.alert_mask = 0xFFFF
        self.supply_alert_status = 0
        self.supply_alert_mask = 0xFFFF
        self.live_status = 0
        self.die_temp_c = 25.0
        self.spi_ok = True
        self.admin_token = "BB-ADMIN-DEBUG"

        # Handler registry: cmd_id (int) -> callable(payload: bytes) -> bytes
        self._handlers: dict = {}

        # Back-reference set by transport
        self._transport = None

        # Streaming support
        self._stream_stop = threading.Event()
        self._stream_thread = None

        self._register_all_handlers()

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
        except Exception:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)

    def http_dispatch(self, method: str, path: str, params: dict, body: dict, headers: dict = None) -> dict:
        try:
            from tests.mock import http_routes
            return http_routes.dispatch(self, method, path, params, body, headers or {})
        except ImportError:
            return {"error": "not implemented"}

    # ------------------------------------------------------------------
    # Streaming
    # ------------------------------------------------------------------

    def stop_all_streams(self) -> None:
        self._stream_stop.set()
        if self._stream_thread and self._stream_thread.is_alive():
            self._stream_thread.join(timeout=2.0)
        self._stream_stop.clear()

    # ------------------------------------------------------------------
    # Handler auto-registration
    # ------------------------------------------------------------------

    def _register_all_handlers(self) -> None:
        try:
            from tests.mock.handlers import core
            core.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import channels
            channels.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import gpio
            gpio.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import mux
            mux.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import power
            power.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import uart
            uart.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import idac
            idac.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import misc
            misc.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import hat
            hat.register(self)
        except ImportError:
            pass

        try:
            from tests.mock.handlers import streaming
            streaming.register(self)
        except ImportError:
            pass
