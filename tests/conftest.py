"""
conftest.py — BugBuster hardware-in-the-loop test framework fixtures.

Provides:
  - CLI options: --device-usb, --device-http, --skip-destructive, --hat
  - Fixtures: device (USB+HTTP), usb_device, http_device, asserter, device_info
  - DeviceAsserter helper class
  - Session-scoped autouse reset fixture
"""

import pytest
import bugbuster as bb
from bugbuster import ChannelFunction, GpioMode, PowerControl
from bugbuster.constants import CmdId


# ---------------------------------------------------------------------------
# CLI options
# ---------------------------------------------------------------------------

def pytest_addoption(parser):
    parser.addoption(
        "--device-usb",
        metavar="PORT",
        default=None,
        help="Serial port for USB device (e.g. /dev/ttyACM0 or COM3)",
    )
    parser.addoption(
        "--device-http",
        metavar="IP",
        default=None,
        help="IP address or hostname for HTTP device (e.g. 192.168.4.1)",
    )
    parser.addoption(
        "--skip-destructive",
        action="store_true",
        default=False,
        help="Skip tests marked @pytest.mark.destructive",
    )
    parser.addoption(
        "--hat",
        action="store_true",
        default=False,
        help="Enable HAT expansion board tests (requires physical HAT)",
    )
    parser.addoption(
        "--swd-target",
        action="store_true",
        default=False,
        help=(
            "Enable SWD tests that require a real target wired to the "
            "dedicated 3-pin SWD connector (e.g. a second RP2040 or STM32). "
            "Without this flag, tests that need a live target are skipped."
        ),
    )
    parser.addoption(
        "--daq",
        action="store_true",
        default=False,
        help=(
            "Enable DAQ HAT hardware tests (requires a DAQ HAT with its "
            "USB-HS port connected). Without this flag every test marked "
            "requires_daq is skipped."
        ),
    )
    parser.addoption(
        "--daq-wifi",
        action="store_true",
        default=False,
        help=(
            "Enable the DAQ WiFi streaming tier. WARNING: this joins the "
            "DAQ HAT's softAP, which takes this machine off its normal "
            "network for the duration of those tests."
        ),
    )
    parser.addoption(
        "--daq-p4-serial",
        metavar="PORT",
        default=None,
        help="P4 console serial port. Auto-detected when omitted.",
    )
    parser.addoption(
        "--daq-load-ohms",
        metavar="R",
        type=float,
        default=None,
        help=(
            "Nominal DUT load resistance. REPORTING ONLY — never asserted "
            "against, because the suite verifies relational properties "
            "(linearity, monotonicity, self-consistency) rather than "
            "absolute accuracy."
        ),
    )
    parser.addoption(
        "--sim",
        action="store_true",
        default=False,
        help="Use simulated device (no hardware required)",
    )
    parser.addoption(
        "--sim-full",
        action="store_true",
        default=False,
        help=(
            "Use simulated device with a real localhost HTTP server so "
            "HTTPTransport exercises actual requests serialization and "
            "HTTP status-code handling (implies --sim for the HTTP path)."
        ),
    )
    parser.addoption(
        "--admin-token",
        metavar="TOKEN",
        default=None,
        help=(
            "Admin token for HTTP authentication (X-BugBuster-Admin-Token). "
            "If omitted and --device-usb is also given, the token is fetched "
            "from the device over USB once at session start."
        ),
    )


# ---------------------------------------------------------------------------
# Sim mutual exclusivity + proto version check
# ---------------------------------------------------------------------------

def pytest_configure(config):
    try:
        sim = config.getoption("--sim", default=False)
        sim_full = config.getoption("--sim-full", default=False)
        usb = config.getoption("--device-usb", default=None)
        http = config.getoption("--device-http", default=None)
        hat = config.getoption("--hat", default=False)
        if sim and sim_full:
            pytest.exit("ERROR: --sim and --sim-full are mutually exclusive")
        if (sim or sim_full) and (usb or http):
            pytest.exit("ERROR: --sim/--sim-full are mutually exclusive with --device-usb / --device-http")
        if (sim or sim_full) and hat:
            raise pytest.UsageError("--sim/--sim-full and --hat are mutually exclusive")
    except pytest.UsageError:
        raise
    except ValueError:
        pass  # options not registered yet during collection


def pytest_sessionstart(session):
    try:
        if session.config.getoption("--sim", default=False) or session.config.getoption("--sim-full", default=False):
            from tests.mock.simulated_device import SimulatedDevice
            from bugbuster.protocol import BBP_PROTO_VERSION
            assert SimulatedDevice.PROTO_VERSION == BBP_PROTO_VERSION, (
                f"Sim PROTO_VERSION {SimulatedDevice.PROTO_VERSION} != "
                f"BBP_PROTO_VERSION {BBP_PROTO_VERSION}"
            )
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Skip logic for destructive tests
# ---------------------------------------------------------------------------

def pytest_collection_modifyitems(config, items):
    if config.getoption("--skip-destructive"):
        skip_destructive = pytest.mark.skip(reason="--skip-destructive passed")
        for item in items:
            if "destructive" in item.keywords:
                item.add_marker(skip_destructive)

    _daq_skips(config, items)


def _daq_skips(config, items):
    """Skip DAQ-tier tests unless their flags are given."""
    if not config.getoption("--daq", default=False):
        skip = pytest.mark.skip(reason="needs --daq (DAQ HAT not requested)")
        for item in items:
            if "requires_daq" in item.keywords:
                item.add_marker(skip)

    if not config.getoption("--daq-wifi", default=False):
        skip = pytest.mark.skip(
            reason="needs --daq-wifi (this tier changes the host's WiFi network)")
        for item in items:
            if "daq_wifi" in item.keywords:
                item.add_marker(skip)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    setattr(item, f"rep_{call.when}", outcome.get_result())


# ---------------------------------------------------------------------------
# DeviceAsserter helper
# ---------------------------------------------------------------------------

class DeviceAsserter:
    """Assertion helpers for hardware measurements with tolerances."""

    def assert_near(self, value: float, expected: float, tol_pct: float = 5.0, msg: str = "") -> None:
        """Assert that *value* is within *tol_pct* percent of *expected*."""
        if expected == 0.0:
            # Fall back to absolute tolerance for zero-expected
            self.assert_near_abs(value, expected, tol_abs=abs(tol_pct / 100.0), msg=msg)
            return
        pct_error = abs(value - expected) / abs(expected) * 100.0
        assert pct_error <= tol_pct, (
            f"{msg + ': ' if msg else ''}value {value!r} deviates {pct_error:.2f}% "
            f"from expected {expected!r} (tolerance {tol_pct}%)"
        )

    def assert_near_abs(self, value: float, expected: float, tol_abs: float, msg: str = "") -> None:
        """Assert that *value* is within *tol_abs* (absolute) of *expected*."""
        diff = abs(value - expected)
        assert diff <= tol_abs, (
            f"{msg + ': ' if msg else ''}value {value!r} differs by {diff!r} "
            f"from expected {expected!r} (tolerance ±{tol_abs!r})"
        )

    def assert_ok(self, result) -> None:
        """Assert that *result* is truthy and not an error object."""
        assert result is not None, "Result must not be None"
        assert result is not False, "Result must not be False"
        if isinstance(result, dict):
            assert "error" not in result, f"Result contains error key: {result.get('error')}"


# ---------------------------------------------------------------------------
# asserter fixture
# ---------------------------------------------------------------------------

@pytest.fixture
def asserter() -> DeviceAsserter:
    """Return a DeviceAsserter instance for hardware measurement assertions."""
    return DeviceAsserter()


# ---------------------------------------------------------------------------
# Shared fault check helper
# ---------------------------------------------------------------------------

def assert_no_faults(device):
    """Read fault registers; xfail if any ADC alert is set."""
    try:
        faults = device.get_faults()
    except Exception:
        return
    alert = faults["alert_status"]
    ch_alerts = [c["alert"] for c in faults["channels"]]
    supply = faults["supply_alert_status"]
    if not (alert or supply or any(ch_alerts)):
        return
    try:
        device.clear_alerts()
    except Exception:
        pass
    parts = []
    if alert:
        parts.append(f"alert=0x{alert:04X}")
    if supply:
        parts.append(f"supply=0x{supply:04X}")
    for i, ca in enumerate(ch_alerts):
        if ca:
            parts.append(f"ch{i}=0x{ca:04X}")
    pytest.xfail(f"FAULT: {', '.join(parts)}")


# ---------------------------------------------------------------------------
# Connection factory helpers
# ---------------------------------------------------------------------------

# Fixed admin token used by --sim-full so HTTPTransport and SimulatedDevice agree.
_SIM_FULL_ADMIN_TOKEN = "aa" * 32  # 64-char hex, never matches a real device token


def _make_usb_device(config):
    """Open a USB BugBuster connection or skip if not configured."""
    if config.getoption("--sim", default=False):
        from tests.mock import SimulatedDevice, SimulatedUSBTransport
        hat = config.getoption("--hat", default=False)
        device = SimulatedDevice()
        transport = SimulatedUSBTransport(device, hat=hat)
        return bb.BugBuster(transport)
    port = config.getoption("--device-usb")
    if not port:
        pytest.skip("USB device not specified — pass --device-usb <port>")
    dev = bb.connect_usb(port)
    return dev


# Session-cached admin token so we fetch from USB at most once.
_admin_token_cache: dict = {"resolved": False, "token": None}


def _resolve_admin_token(config):
    """
    Resolve the admin token for HTTP authentication.

    Priority:
      1. --admin-token CLI option (explicit override)
      2. Fetch from device over USB if --device-usb is given
      3. None (HTTP-only mode without auth — destructive endpoints will 401)
    """
    if _admin_token_cache["resolved"]:
        return _admin_token_cache["token"]

    token = config.getoption("--admin-token", default=None)
    if not token:
        port = config.getoption("--device-usb", default=None)
        if port:
            try:
                tmp = bb.connect_usb(port)
                try:
                    token = tmp.get_admin_token()
                finally:
                    tmp.disconnect()
            except Exception as e:
                # Don't fail collection — HTTP tests requiring auth will surface
                # the issue with their own 401, while unauth HTTP tests still run.
                print(f"[conftest] USB admin-token fetch failed: {e}")
                token = None

    _admin_token_cache["resolved"] = True
    _admin_token_cache["token"] = token
    return token


def _make_http_device(config, request=None):
    """Open an HTTP BugBuster connection or skip if not configured."""
    if config.getoption("--sim", default=False):
        from tests.mock import SimulatedDevice, SimulatedHTTPTransport
        hat = config.getoption("--hat", default=False)
        device = SimulatedDevice()
        transport = SimulatedHTTPTransport(device, hat=hat)
        return bb.BugBuster(transport)

    if config.getoption("--sim-full", default=False):
        from tests.mock import SimulatedDevice
        from tests.mock.sim_http_server import SimHTTPServer
        from bugbuster.transport.http import HTTPTransport
        hat = config.getoption("--hat", default=False)
        device = SimulatedDevice()
        device.hat.present = hat
        server = SimHTTPServer(device, admin_token=_SIM_FULL_ADMIN_TOKEN)
        server.start()
        transport = HTTPTransport(
            host="127.0.0.1",
            port=server.port,
            admin_token=_SIM_FULL_ADMIN_TOKEN,
        )
        transport.connect()
        dev = bb.BugBuster(transport)
        # Store server on the device wrapper so the fixture finalizer can stop it.
        dev._sim_http_server = server  # noqa: SLF001
        if request is not None:
            request.addfinalizer(server.stop)
        return dev

    host = config.getoption("--device-http")
    if not host:
        pytest.skip("HTTP device not specified — pass --device-http <ip>")
    dev = bb.connect_http(host)
    token = _resolve_admin_token(config)
    if token:
        dev._admin_token = token  # noqa: SLF001 — public-by-convention
    return dev


# ---------------------------------------------------------------------------
# Per-test touched-resource cleanup
# ---------------------------------------------------------------------------

_CHANNEL_MUTATORS = {
    "set_channel_function",
    "set_dac_voltage",
    "set_dac_current",
    "set_dac_code",
    "set_vout_range",
    "set_current_limit",
    "set_adc_config",
    "set_rtd_config",
    "set_digital_output",
    "set_din_config",
}

_GPIO_MUTATORS = {"set_gpio_config", "set_gpio_value"}
_DIO_MUTATORS = {"dio_configure", "dio_write"}
_MUX_MUTATORS = {"mux_set_all", "mux_set_switch"}
_POWER_MUTATORS = {"power_set"}
_IDAC_MUTATORS = {"idac_set_voltage", "idac_set_code", "idac_cal_save"}
_ALERT_MUTATORS = {"clear_alerts", "set_channel_alert_mask", "set_alert_mask"}
_SELFTEST_MUTATORS = {
    "selftest_measure_supply",
    "selftest_auto_calibrate",
    "selftest_internal_supplies",
}
_STREAM_MUTATORS = {"start_adc_stream", "stop_adc_stream"}
_WAVEGEN_MUTATORS = {"start_waveform", "stop_waveform"}
_UART_MUTATORS = {"set_uart_config"}
_WATCHDOG_MUTATORS = {"set_watchdog"}
_LSHIFT_MUTATORS = {"set_level_shifter_oe"}
_IO_OWNER_MUTATORS = {"io_claim", "io_release", "io_force_release"}
_RESET_MUTATORS = {"reset"}

_KNOWN_MUTATORS = (
    _CHANNEL_MUTATORS
    | _GPIO_MUTATORS
    | _DIO_MUTATORS
    | _MUX_MUTATORS
    | _POWER_MUTATORS
    | _IDAC_MUTATORS
    | _ALERT_MUTATORS
    | _SELFTEST_MUTATORS
    | _STREAM_MUTATORS
    | _WAVEGEN_MUTATORS
    | _UART_MUTATORS
    | _WATCHDOG_MUTATORS
    | _LSHIFT_MUTATORS
    | _IO_OWNER_MUTATORS
    | _RESET_MUTATORS
)

_MUTATING_PREFIXES = (
    "set_",
    "start_",
    "stop_",
    "clear_",
    "io_",
    "power_",
    "mux_",
    "idac_",
    "selftest_",
    "wifi_",
    "hat_",
    "dio_",
)

_HAL_MUTATORS = {
    "begin",
    "shutdown",
    "configure",
    "write_voltage",
    "write_current",
    "write_digital",
    "write_hart_current",
    "set_voltage",
    "set_vlogic",
    "set_serial",
}


class _MutationTracker:
    def __init__(self):
        self.channels: set[int] = set()
        self.gpios: set[int] = set()
        self.dios: set[int] = set()
        self.slots: set[int] = set()
        self.power_controls: set[PowerControl] = set()
        self.idac_channels: set[int] = set()
        self.mux = False
        self.alert_global = False
        self.alert_channels: set[int] = set()
        self.selftest = False
        self.stream = False
        self.wavegen = False
        self.uart = False
        self.watchdog = False
        self.lshift = False
        self.unknown = False
        self.full_reset = False

    @property
    def touched(self) -> bool:
        return any((
            self.channels,
            self.gpios,
            self.dios,
            self.slots,
            self.power_controls,
            self.idac_channels,
            self.mux,
            self.alert_global,
            self.alert_channels,
            self.selftest,
            self.stream,
            self.wavegen,
            self.uart,
            self.watchdog,
            self.lshift,
            self.unknown,
            self.full_reset,
        ))

    def record(self, name: str, args: tuple, kwargs: dict) -> None:
        if name in _CHANNEL_MUTATORS:
            self._add_channel(args, kwargs)
        elif name in _GPIO_MUTATORS:
            self._add_gpio(args, kwargs)
        elif name in _DIO_MUTATORS:
            self._add_dio(args, kwargs)
        elif name in _MUX_MUTATORS:
            self.mux = True
        elif name in _POWER_MUTATORS:
            self._add_power(args, kwargs)
        elif name in _IDAC_MUTATORS:
            self._add_idac(args, kwargs)
        elif name in _ALERT_MUTATORS:
            self._add_alert(name, args, kwargs)
        elif name in _SELFTEST_MUTATORS:
            self.selftest = True
            self.slots.add(14)
        elif name in _STREAM_MUTATORS:
            self.stream = True
        elif name in _WAVEGEN_MUTATORS:
            self.wavegen = True
            self._add_channel(args, kwargs)
        elif name in _UART_MUTATORS:
            self.uart = True
        elif name in _WATCHDOG_MUTATORS:
            self.watchdog = True
        elif name in _LSHIFT_MUTATORS:
            self.lshift = True
        elif name in _IO_OWNER_MUTATORS:
            self._add_io_owner(args, kwargs)
        elif name in _RESET_MUTATORS:
            self.channels.update(range(4))
            self.slots.update(range(12, 16))
            self.alert_global = True
        elif name.startswith(_MUTATING_PREFIXES):
            self.unknown = True

    def record_hal(self, name: str, args: tuple, kwargs: dict) -> None:
        if name in _HAL_MUTATORS:
            # HAL methods fan out through multiple private helpers. Treat them
            # as unknown unless a future tracker wraps each HAL path directly.
            self.unknown = True

    def _add_channel(self, args: tuple, kwargs: dict) -> None:
        value = args[0] if args else kwargs.get("channel")
        if isinstance(value, int):
            self.channels.add(value)
            self.slots.add(value + 12)

    def _add_gpio(self, args: tuple, kwargs: dict) -> None:
        value = args[0] if args else kwargs.get("gpio")
        if isinstance(value, int):
            self.gpios.add(value)

    def _add_dio(self, args: tuple, kwargs: dict) -> None:
        value = args[0] if args else kwargs.get("io")
        if isinstance(value, int):
            self.dios.add(value)
            self.slots.add(value - 1)

    def _add_power(self, args: tuple, kwargs: dict) -> None:
        value = args[0] if args else kwargs.get("control")
        if value is not None:
            try:
                self.power_controls.add(PowerControl(value))
            except ValueError:
                self.unknown = True

    def _add_idac(self, args: tuple, kwargs: dict) -> None:
        value = args[0] if args else kwargs.get("channel")
        if isinstance(value, int):
            self.idac_channels.add(value)
        else:
            self.unknown = True

    def _add_alert(self, name: str, args: tuple, kwargs: dict) -> None:
        if name == "set_alert_mask":
            self.alert_global = True
            return
        value = None
        if args:
            value = args[0]
        if "channel" in kwargs:
            value = kwargs["channel"]
        if value is None:
            self.alert_global = True
        elif isinstance(value, int):
            self.alert_channels.add(value)
        else:
            self.unknown = True

    def _add_io_owner(self, args: tuple, kwargs: dict) -> None:
        value = args[0] if args else kwargs.get("slots")
        if value is None:
            self.slots.update(range(16))
        elif isinstance(value, int):
            self.slots.add(value)
        else:
            try:
                self.slots.update(int(slot) for slot in value)
            except (TypeError, ValueError):
                self.unknown = True


class _HalTrackingProxy:
    def __init__(self, hal, tracker: _MutationTracker):
        self._hal = hal
        self._tracker = tracker

    def __getattr__(self, name):
        attr = getattr(self._hal, name)
        if not callable(attr):
            return attr

        def _wrapped(*args, **kwargs):
            self._tracker.record_hal(name, args, kwargs)
            return attr(*args, **kwargs)

        return _wrapped


class _BugBusterTrackingProxy:
    def __init__(self, dev, tracker: _MutationTracker):
        self._dev = dev
        self._tracker = tracker
        self._hal_proxy = None

    @property
    def hal(self):
        if self._hal_proxy is None:
            self._hal_proxy = _HalTrackingProxy(self._dev.hal, self._tracker)
        return self._hal_proxy

    def __getattr__(self, name):
        attr = getattr(self._dev, name)
        if not callable(attr):
            return attr

        def _wrapped(*args, **kwargs):
            self._tracker.record(name, args, kwargs)
            return attr(*args, **kwargs)

        return _wrapped


def _force_release_slot(dev, slot: int) -> None:
    token = getattr(dev, "_admin_token", None)
    if getattr(dev, "_usb", False):
        if not token:
            try:
                token = dev.get_admin_token()
            except Exception:
                token = None
        if token:
            dev.io_force_release(slot, token)
    else:
        dev._http_post("/io/owner/force", {"slot": slot})  # noqa: SLF001


def _cleanup_touched_resources(dev, tracker: _MutationTracker, label: str) -> list[str]:
    errors: list[str] = []

    def _try(name: str, fn):
        try:
            fn()
        except Exception as exc:  # noqa: BLE001 — cleanup is fail-soft
            print(f"[conftest] {label} cleanup step {name} failed: {exc}")
            errors.append(name)

    if tracker.stream:
        _try("stop_adc_stream", dev.stop_adc_stream)
    if tracker.wavegen:
        _try("stop_waveform", dev.stop_waveform)

    if tracker.selftest:
        if getattr(dev, "_usb", False):
            _try("selftest_worker_off", lambda: dev._usb_cmd(CmdId.SELFTEST_WORKER, b"\x00"))  # noqa: SLF001
        else:
            _try("selftest_worker_off", lambda: dev._http_post("/selftest/worker", {"enabled": False}))  # noqa: SLF001

    for slot in sorted(slot for slot in tracker.slots if 0 <= slot < 16):
        _try(f"io_force_release[{slot}]", lambda s=slot: _force_release_slot(dev, s))

    if tracker.mux:
        _try("mux_all_open", lambda: dev.mux_set_all([0, 0, 0, 0]))

    for ch in sorted(ch for ch in tracker.channels if 0 <= ch < 4):
        _try(f"ch{ch}_high_imp", lambda c=ch: dev.set_channel_function(c, ChannelFunction.HIGH_IMP))
        _try(f"ch{ch}_alert_mask", lambda c=ch: dev.set_channel_alert_mask(c, 0))
        _try(f"clear_alerts[{ch}]", lambda c=ch: dev.clear_alerts(c))

    for gpio in sorted(g for g in tracker.gpios if 0 <= g < 12):
        _try(f"gpio{gpio}_high_imp", lambda g=gpio: dev.set_gpio_config(g, GpioMode.HIGH_IMP))

    for io in sorted(i for i in tracker.dios if 1 <= i <= 12):
        _try(f"dio{io}_disabled", lambda i=io: dev.dio_configure(i, 0))

    if tracker.alert_global:
        _try("alert_mask_global", lambda: dev.set_alert_mask(0, 0))
        _try("clear_alerts", dev.clear_alerts)
    for ch in sorted(ch for ch in tracker.alert_channels if 0 <= ch < 4):
        _try(f"ch{ch}_alert_mask", lambda c=ch: dev.set_channel_alert_mask(c, 0))
        _try(f"clear_alerts[{ch}]", lambda c=ch: dev.clear_alerts(c))

    for ch in sorted(c for c in tracker.idac_channels if 0 <= c <= 3):
        _try(f"idac{ch}_code_zero", lambda c=ch: dev.idac_set_code(c, 0))

    for ctrl in sorted(tracker.power_controls, key=int):
        if ctrl in (PowerControl.EFUSE1, PowerControl.EFUSE2, PowerControl.EFUSE3, PowerControl.EFUSE4,
                    PowerControl.VADJ1, PowerControl.VADJ2):
            _try(f"{ctrl.name}_off", lambda c=ctrl: dev.power_set(c, False))
        elif ctrl in (PowerControl.V15A, PowerControl.MUX, PowerControl.USB_HUB):
            _try(f"{ctrl.name}_on", lambda c=ctrl: dev.power_set(c, True))

    if tracker.lshift:
        _try("lshift_oe_off", lambda: dev.set_level_shifter_oe(False))
    if tracker.watchdog:
        _try("watchdog_off", lambda: dev.set_watchdog(False, 0))
    if tracker.uart:
        _try("uart_bridge_off", lambda: dev.set_uart_config(enabled=False))

    for slot in sorted(slot for slot in tracker.slots if 0 <= slot < 16):
        _try(f"io_force_release_final[{slot}]", lambda s=slot: _force_release_slot(dev, s))

    return errors


def _test_failed(request) -> bool:
    report = getattr(request.node, "rep_call", None)
    return bool(report and report.failed)


def _needs_full_reset(request, tracker: _MutationTracker) -> bool:
    return (
        tracker.full_reset
        or tracker.unknown
        or "full_reset" in request.keywords
        or "destructive" in request.keywords
        or _test_failed(request)
    )


def _cleanup_after_test(dev, tracker: _MutationTracker, request, label: str) -> None:
    if not tracker.touched and not _needs_full_reset(request, tracker):
        return
    if _needs_full_reset(request, tracker):
        reason = "unknown mutation" if tracker.unknown else "marker/failure"
        print(f"[conftest] {label} using full reset fallback ({reason})")
        _reset_inplace(dev, label)
        return
    errors = _cleanup_touched_resources(dev, tracker, label)
    if errors:
        print(f"[conftest] {label} targeted cleanup failed; running full reset fallback")
        _reset_inplace(dev, f"{label}-fallback")


# ---------------------------------------------------------------------------
# Parametrized device fixture (USB + HTTP)
# ---------------------------------------------------------------------------

def _reset_inplace(dev, label):
    """Call dev.reset_to_defaults() on an already-open connection. Fail-soft."""
    if dev is None or getattr(dev, "_t", None) is None:
        return
    try:
        token = getattr(dev, "_admin_token", None)
        if getattr(dev, "_usb", False) and not token:
            try:
                token = dev.get_admin_token()
            except Exception:
                token = None
        errors = dev.reset_to_defaults(admin_token=token)
        if errors:
            print(f"[conftest] {label} reset_to_defaults failed steps: {errors}")
    except Exception as exc:  # noqa: BLE001 — fail-soft
        print(f"[conftest] {label} reset_to_defaults raised: {exc}")


@pytest.fixture(params=["usb", "http"])
def device(request):
    """
    Parametrized fixture yielding a connected BugBuster over USB or HTTP.
    Automatically skips if the CLI argument for that transport is not provided.

    Tracks hardware mutations during the test and cleans up only touched
    resources after yield. Broad reset is reserved for session bookends,
    explicit full_reset/destructive tests, failures, or unknown mutations.
    """
    transport = request.param
    if transport == "usb":
        dev = _make_usb_device(request.config)
    else:
        dev = _make_http_device(request.config, request=request)

    _sim_mode = request.config.getoption("--sim", default=False) or request.config.getoption("--sim-full", default=False)
    skip_reset = "no_reset" in request.keywords or _sim_mode
    tracker = _MutationTracker()
    yielded = dev if skip_reset else _BugBusterTrackingProxy(dev, tracker)

    yield yielded

    if not skip_reset:
        _cleanup_after_test(dev, tracker, request, "post-yield")

    try:
        dev.disconnect()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# USB-only fixture
# ---------------------------------------------------------------------------

@pytest.fixture
def usb_device(request):
    """USB-only BugBuster fixture.  Skips if --device-usb not given."""
    dev = _make_usb_device(request.config)
    skip_reset = "no_reset" in request.keywords or request.config.getoption("--sim", default=False)
    tracker = _MutationTracker()
    yielded = dev if skip_reset else _BugBusterTrackingProxy(dev, tracker)
    yield yielded
    if not skip_reset:
        _cleanup_after_test(dev, tracker, request, "post-yield(usb)")
    try:
        dev.disconnect()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# HTTP-only fixture
# ---------------------------------------------------------------------------

@pytest.fixture
def http_device(request):
    """HTTP-only BugBuster fixture.  Skips if --device-http not given."""
    dev = _make_http_device(request.config, request=request)
    _sim_mode = request.config.getoption("--sim", default=False) or request.config.getoption("--sim-full", default=False)
    skip_reset = "no_reset" in request.keywords or _sim_mode
    tracker = _MutationTracker()
    yielded = dev if skip_reset else _BugBusterTrackingProxy(dev, tracker)
    yield yielded
    if not skip_reset:
        _cleanup_after_test(dev, tracker, request, "post-yield(http)")
    try:
        dev.disconnect()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Session-scoped device_info cache
# ---------------------------------------------------------------------------

_fw_version_cache: dict = {}


@pytest.fixture(scope="session")
def device_info(request):
    """
    Session-scoped fixture that caches firmware version from first available connection.
    Returns a dict with 'transport', 'fw_major', 'fw_minor', 'fw_patch'.
    """
    # Try USB first, then HTTP
    port = request.config.getoption("--device-usb")
    host = request.config.getoption("--device-http")

    if port:
        dev = bb.connect_usb(port)
        transport = "usb"
    elif host:
        dev = bb.connect_http(host)
        transport = "http"
    else:
        pytest.skip("No device configured — pass --device-usb or --device-http")

    try:
        major, minor, patch = dev.get_firmware_version()
        info = {
            "transport": transport,
            "fw_major": major,
            "fw_minor": minor,
            "fw_patch": patch,
        }
    finally:
        dev.disconnect()

    return info


# ---------------------------------------------------------------------------
# Per-test device-reset fixture — leave the device exactly as we found it.
#
# Every `device` / `usb_device` / `http_device` test depends (via its
# transport fixture) on `_reset_device_state`. After each test, the fixture
# opens a short-lived connection and calls `BugBuster.reset_to_defaults()` so
# the next test starts from a known state (channels HIGH_IMP, alert masks 0,
# alerts cleared, e-fuses off, VADJ off, +15V/USB_HUB/MUX on, IO-owner table
# released, level-shifter OE off).
#
# Tests that intentionally probe an "out of default" boot state can opt out
# with `@pytest.mark.no_reset`.
# ---------------------------------------------------------------------------

def _open_reset_connection(config):
    """Open a short-lived connection used solely for reset_to_defaults()."""
    if config.getoption("--sim", default=False):
        return None  # simulator does not need physical reset
    port = config.getoption("--device-usb", default=None)
    host = config.getoption("--device-http", default=None)
    if port:
        dev = bb.connect_usb(port)
        token = _resolve_admin_token(config)
        if token:
            dev._admin_token = token  # noqa: SLF001
        return dev
    if host:
        dev = bb.connect_http(host)
        token = _resolve_admin_token(config)
        if token:
            dev._admin_token = token  # noqa: SLF001
        return dev
    return None


def _perform_device_reset(config):
    """Best-effort reset of the device to boot defaults."""
    dev = None
    try:
        dev = _open_reset_connection(config)
        if dev is None:
            return
        errors = dev.reset_to_defaults()
        if errors:
            print(f"[conftest] reset_to_defaults reported failed steps: {errors}")
    except Exception as exc:  # noqa: BLE001 — fail-soft
        print(f"[conftest] reset_to_defaults raised: {exc}")
    finally:
        if dev:
            try:
                dev.disconnect()
            except Exception:
                pass


@pytest.fixture(scope="session", autouse=True)
def _reset_device_session_bookends(request):
    """
    Session-scoped autouse fixture. Resets the device once at session start
    so an inherited dirty state does not poison the first test, and once at
    session end so the bench is left clean for the operator.
    """
    _perform_device_reset(request.config)
    yield
    _perform_device_reset(request.config)


# ---------------------------------------------------------------------------
# DAQ HAT fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def daq_link(request):
    """A live DaqLink to the P4 over USB-HS.

    Session-scoped because claiming the vendor interface per-test is slow and
    the device holds stream state across tests anyway. The finalizer is the
    safety contract: it runs even on failure, error and KeyboardInterrupt, so a
    dead run cannot leave V_DUT enabled into whatever is wired to the DUT
    terminals.
    """
    from tests.lib.daq_link import DaqLink, DaqLinkUnavailable

    try:
        link = DaqLink.usb()
    except DaqLinkUnavailable as exc:
        pytest.skip("DAQ HAT unavailable: %s" % exc)

    yield link

    link.safe_state()
    link.close()


@pytest.fixture(scope="session")
def p4_console(request):
    """The P4's serial CLI, for capabilities not reachable over USB bulk."""
    from tests.lib.p4_console import P4Console, P4ConsoleUnavailable, find_p4_port

    port = request.config.getoption("--daq-p4-serial") or find_p4_port()
    try:
        con = P4Console(port)
    except P4ConsoleUnavailable as exc:
        pytest.skip("P4 console unavailable: %s" % exc)

    yield con
    con.close()


@pytest.fixture
def daq_safe(daq_link):
    """Return the link to a safe state after every DAQ test.

    Per-test, in addition to the session finalizer, so one test leaving the SMU
    enabled or a range locked cannot silently change the meaning of the next.
    """
    yield daq_link
    daq_link.safe_state()


@pytest.fixture(scope="session")
def daq_load_ohms(request):
    """Reporting-only hint. Never assert against this."""
    return request.config.getoption("--daq-load-ohms")
