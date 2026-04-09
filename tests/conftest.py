"""
conftest.py — BugBuster hardware-in-the-loop test framework fixtures.

Provides:
  - CLI options: --device-usb, --device-http, --skip-destructive, --hat
  - Fixtures: device (USB+HTTP), usb_device, http_device, asserter, device_info
  - DeviceAsserter helper class
  - Session-scoped autouse reset fixture
"""

import time
import pytest
import bugbuster as bb
from bugbuster import ChannelFunction


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


# ---------------------------------------------------------------------------
# Skip logic for destructive tests
# ---------------------------------------------------------------------------

def pytest_collection_modifyitems(config, items):
    if config.getoption("--skip-destructive"):
        skip_destructive = pytest.mark.skip(reason="--skip-destructive passed")
        for item in items:
            if "destructive" in item.keywords:
                item.add_marker(skip_destructive)


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

def _make_usb_device(config):
    """Open a USB BugBuster connection or skip if not configured."""
    port = config.getoption("--device-usb")
    if not port:
        pytest.skip("USB device not specified — pass --device-usb <port>")
    dev = bb.connect_usb(port)
    return dev


def _make_http_device(config):
    """Open an HTTP BugBuster connection or skip if not configured."""
    host = config.getoption("--device-http")
    if not host:
        pytest.skip("HTTP device not specified — pass --device-http <ip>")
    dev = bb.connect_http(host)
    return dev


# ---------------------------------------------------------------------------
# Parametrized device fixture (USB + HTTP)
# ---------------------------------------------------------------------------

@pytest.fixture(params=["usb", "http"])
def device(request):
    """
    Parametrized fixture yielding a connected BugBuster over USB or HTTP.
    Automatically skips if the CLI argument for that transport is not provided.
    """
    transport = request.param
    if transport == "usb":
        dev = _make_usb_device(request.config)
    else:
        dev = _make_http_device(request.config)

    yield dev

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
    yield dev
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
    dev = _make_http_device(request.config)
    yield dev
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
# Session-scoped autouse fixture — restore safe state after all tests
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session", autouse=True)
def reset_after_test(request):
    """
    Session-scoped autouse fixture.
    After the entire test session, puts all 4 channels to HIGH_IMP
    to leave the device in a safe state.
    """
    yield  # run all tests first

    port = request.config.getoption("--device-usb")
    host = request.config.getoption("--device-http")

    dev = None
    try:
        if port:
            dev = bb.connect_usb(port)
        elif host:
            dev = bb.connect_http(host)
        else:
            return

        for ch in range(4):
            try:
                dev.set_channel_function(ch, ChannelFunction.HIGH_IMP)
            except Exception:
                pass
    except Exception:
        pass
    finally:
        if dev:
            try:
                dev.disconnect()
            except Exception:
                pass
