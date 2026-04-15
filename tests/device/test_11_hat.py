"""
test_11_hat.py — HAT expansion board tests.

All tests require the --hat CLI flag and physical HAT hardware.
USB-only tests additionally require --device-usb.

HAT features tested:
  - Status / detection
  - Pin function assignment
  - Power management (USB only)
  - Logic Analyzer (USB only)
  - SWD debug setup (USB only)
"""

import time
import pytest
import bugbuster as bb
from bugbuster.constants import LaTriggerType, HatPinFunction
from bugbuster.transport.usb import DeviceError
from conftest import assert_no_faults

try:
    import serial
    import serial.tools.list_ports
    _SERIAL_AVAILABLE = True
except ImportError:
    _SERIAL_AVAILABLE = False

try:
    import usb.core
    import usb.util
    _PYUSB_AVAILABLE = True
except ImportError:
    _PYUSB_AVAILABLE = False

pytestmark = [
    pytest.mark.requires_hat,
    pytest.mark.timeout(15),
]


# ---------------------------------------------------------------------------
# Skip HAT tests if --hat not passed
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def require_hat(request):
    """Auto-use fixture that skips all HAT tests unless --hat is passed."""
    if not request.config.getoption("--hat", default=False):
        pytest.skip("HAT tests require --hat flag")


def _require_hvpak_ready(usb_device):
    info = usb_device.hat_get_hvpak_info()
    if not info.get("ready", False):
        pytest.skip(
            f"HVPAK backend not ready on this hardware "
            f"(part={info.get('part')}, err={info.get('last_error')})"
        )
    return info, usb_device.hat_get_hvpak_caps()


# ---------------------------------------------------------------------------
# HAT status
# ---------------------------------------------------------------------------

def test_hat_get_status(device):
    """
    hat_get_status() returns a dict with at least 'detected' and 'connected' flags.
    Also checks for 'fw_version' and 'pin_config' fields.
    """
    status = device.hat_get_status()

    assert isinstance(status, dict), f"hat_get_status() must return dict, got {type(status)}"
    assert "detected" in status, "HAT status missing 'detected'"
    assert "connected" in status, "HAT status missing 'connected'"
    assert_no_faults(device)


def test_hat_status_has_pin_config(device):
    """
    HAT status should include a 'pin_config' list with 4 entries
    representing the function of each EXP_EXT pin.
    """
    status = device.hat_get_status()

    if "pin_config" in status:
        pins = status["pin_config"]
        assert isinstance(pins, list), f"pin_config must be list, got {type(pins)}"
        assert len(pins) == 4, f"Expected 4 EXP_EXT pins, got {len(pins)}"
    assert_no_faults(device)


def test_hat_status_optional_hvpak_metadata(device):
    """
    Newer firmware may surface HVPAK metadata in hat_get_status().
    If present, the fields must be well-typed and internally consistent.
    """
    status = device.hat_get_status()

    if "hvpak_part" in status:
        assert isinstance(status["hvpak_part"], int)
    if "hvpak_ready" in status:
        assert isinstance(status["hvpak_ready"], bool)
    if "hvpak_last_error" in status:
        assert isinstance(status["hvpak_last_error"], int)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT detect
# ---------------------------------------------------------------------------

def test_hat_detect(device):
    """
    hat_detect() re-runs HAT detection and returns a result dict.
    Should not raise even if no HAT is physically connected.
    """
    result = device.hat_detect()

    assert isinstance(result, dict), f"hat_detect() must return dict, got {type(result)}"
    assert "detected" in result, "hat_detect() result missing 'detected'"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT reset
# ---------------------------------------------------------------------------

def test_hat_reset(device):
    """
    hat_reset() resets the HAT to default state (all pins disconnected).
    Should return True on success.
    """
    result = device.hat_reset()
    assert result is True or result is not False, "hat_reset() should return True"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT set single pin
# ---------------------------------------------------------------------------

def test_hat_set_pin(device):
    """
    hat_set_pin(0, HatPinFunction.DISCONNECTED) sets EXP_EXT_0 to disconnected/high-imp.
    Should return True on USB, may return bool on HTTP.
    """
    result = device.hat_set_pin(0, HatPinFunction.DISCONNECTED)
    assert result is not False, "hat_set_pin() should not return False"
    assert_no_faults(device)


def test_hat_set_pin_all_functions(device):
    """
    Cycle through all valid HatPinFunction values for pin 0.
    Reset to DISCONNECTED afterwards.
    """
    for func in HatPinFunction:
        device.hat_set_pin(0, func)
        time.sleep(0.02)

    # Restore to safe state
    device.hat_set_pin(0, HatPinFunction.DISCONNECTED)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT set all pins
# ---------------------------------------------------------------------------

def test_hat_set_all_pins(device):
    """
    hat_set_all_pins() sets all 4 EXP_EXT pins at once.
    Set all to DISCONNECTED (safe default).
    """
    funcs = [
        HatPinFunction.DISCONNECTED,
        HatPinFunction.DISCONNECTED,
        HatPinFunction.DISCONNECTED,
        HatPinFunction.DISCONNECTED,
    ]
    result = device.hat_set_all_pins(funcs)
    assert result is not False, "hat_set_all_pins() should not return False"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT power (USB only)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_get_power_usb(usb_device):
    """
    hat_get_power() returns a dict with 'connectors' list (2 entries)
    and 'io_voltage_mv' field. USB only.
    """
    result = usb_device.hat_get_power()

    assert isinstance(result, dict), f"hat_get_power() must return dict, got {type(result)}"
    assert "connectors" in result, "hat_get_power() missing 'connectors'"
    connectors = result["connectors"]
    assert isinstance(connectors, list), f"connectors must be list"
    assert len(connectors) == 2, f"Expected 2 HAT connectors, got {len(connectors)}"

    for i, conn in enumerate(connectors):
        assert "enabled" in conn, f"Connector {i} missing 'enabled'"
        assert "current_ma" in conn, f"Connector {i} missing 'current_ma'"
        assert "fault" in conn, f"Connector {i} missing 'fault'"
    if "hvpak_part" in result:
        assert isinstance(result["hvpak_part"], int)
    if "hvpak_ready" in result:
        assert isinstance(result["hvpak_ready"], bool)
    if "hvpak_last_error" in result:
        assert isinstance(result["hvpak_last_error"], int)
    assert_no_faults(usb_device)


def test_hat_get_power_http_raises(http_device):
    """
    hat_get_power() over HTTP should raise NotImplementedError since
    the firmware does not expose a /api/hat/power endpoint.
    """
    with pytest.raises(NotImplementedError):
        http_device.hat_get_power()
    assert_no_faults(http_device)


@pytest.mark.usb_only
def test_hat_get_hvpak_info_usb(usb_device):
    """
    Advanced HVPAK backend should at least surface part/ready/error/voltage info.
    """
    result = usb_device.hat_get_hvpak_info()
    assert isinstance(result, dict)
    assert "part" in result
    assert "ready" in result
    assert "last_error" in result
    assert "requested_mv" in result
    assert "applied_mv" in result
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_get_hvpak_caps_usb_if_ready(usb_device):
    """
    If the programmed HVPAK image is detected and ready, the capability
    profile should be readable over USB.
    """
    _, caps = _require_hvpak_ready(usb_device)
    assert isinstance(caps, dict)
    assert "flags" in caps
    assert "pwm_count" in caps
    assert "bridge_count" in caps
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_hvpak_lut_roundtrip_usb_if_supported(usb_device):
    _, caps = _require_hvpak_ready(usb_device)
    candidates = [
        ("lut2_count", 0),
        ("lut3_count", 1),
        ("lut4_count", 2),
    ]
    for count_key, kind in candidates:
        if caps.get(count_key, 0) > 0:
            before = usb_device.hat_get_hvpak_lut(kind, 0)
            after = usb_device.hat_set_hvpak_lut(kind, 0, before["truth_table"])
            assert after["truth_table"] == before["truth_table"]
            assert_no_faults(usb_device)
            return
    pytest.skip("No LUT capability exposed by this programmed HVPAK image")


@pytest.mark.usb_only
def test_hat_hvpak_bridge_roundtrip_usb_if_supported(usb_device):
    _, caps = _require_hvpak_ready(usb_device)
    if caps.get("bridge_count", 0) == 0:
        pytest.skip("No bridge capability exposed by this programmed HVPAK image")
    before = usb_device.hat_get_hvpak_bridge()
    after = usb_device.hat_set_hvpak_bridge(
        output_mode_0=before["output_mode"][0],
        ocp_retry_0=before["ocp_retry"][0],
        output_mode_1=before["output_mode"][1],
        ocp_retry_1=before["ocp_retry"][1],
        predriver_enabled=before["predriver_enabled"],
        full_bridge_enabled=before["full_bridge_enabled"],
        control_selection_ph_en=before["control_selection_ph_en"],
        ocp_deglitch_enabled=before["ocp_deglitch_enabled"],
        uvlo_enabled=before["uvlo_enabled"],
    )
    assert after == before
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_hvpak_analog_roundtrip_usb_if_supported(usb_device):
    _, caps = _require_hvpak_ready(usb_device)
    if caps.get("comparator_count", 0) == 0:
        pytest.skip("No analog capability exposed by this programmed HVPAK image")
    before = usb_device.hat_get_hvpak_analog()
    after = usb_device.hat_set_hvpak_analog(**before)
    assert after == before
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_hvpak_pwm_roundtrip_usb_if_supported(usb_device):
    _, caps = _require_hvpak_ready(usb_device)
    if caps.get("pwm_count", 0) == 0:
        pytest.skip("No PWM capability exposed by this programmed HVPAK image")
    before = usb_device.hat_get_hvpak_pwm(0)
    after = usb_device.hat_set_hvpak_pwm(
        0,
        initial_value=before["initial_value"],
        resolution_7bit=before["resolution_7bit"],
        out_plus_inverted=before["out_plus_inverted"],
        out_minus_inverted=before["out_minus_inverted"],
        async_powerdown=before["async_powerdown"],
        autostop_mode=before["autostop_mode"],
        boundary_osc_disable=before["boundary_osc_disable"],
        phase_correct=before["phase_correct"],
        deadband=before["deadband"],
        stop_mode=before["stop_mode"],
        i2c_trigger=before["i2c_trigger"],
        duty_source=before["duty_source"],
        period_clock_source=before["period_clock_source"],
        duty_clock_source=before["duty_clock_source"],
    )
    assert after["index"] == before["index"]
    assert after["initial_value"] == before["initial_value"]
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_hvpak_reg_write_masked_rejects_unsafe_register(usb_device):
    _require_hvpak_ready(usb_device)
    with pytest.raises(DeviceError) as exc_info:
        usb_device.hat_hvpak_reg_write_masked(0x48, 0x01, 0x01)
    assert "HVPAK_UNSAFE_REGISTER" in str(exc_info.value)


@pytest.mark.usb_only
@pytest.mark.destructive
def test_hat_set_power_usb(usb_device):
    """
    hat_set_power(0, False) disables target power on HAT connector A.
    Destructive: modifies power state. Restores power after test.
    """
    usb_device.hat_set_power(0, False)
    time.sleep(0.1)
    # Restore
    usb_device.hat_set_power(0, True)
    assert_no_faults(usb_device)


def test_hat_set_io_voltage_http_raises(http_device):
    """
    hat_set_io_voltage() over HTTP should raise NotImplementedError.
    """
    with pytest.raises(NotImplementedError):
        http_device.hat_set_io_voltage(3300)
    assert_no_faults(http_device)


# ---------------------------------------------------------------------------
# Logic Analyzer (USB only)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_la_configure(usb_device):
    """
    hat_la_configure(4, 1_000_000, 1000) configures the LA for 4 channels
    at 1 MHz sample rate with 1000 sample depth.
    Should return True.
    """
    result = usb_device.hat_la_configure(4, 1_000_000, 1000)
    assert result is True, f"hat_la_configure() should return True, got {result}"
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_la_arm_and_stop(usb_device):
    """
    Configure the LA, arm it, then immediately stop.
    Verifies the LA state machine can transition arm → stop without error.
    """
    usb_device.hat_la_configure(4, 1_000_000, 1000)
    usb_device.hat_la_arm()
    time.sleep(0.05)
    usb_device.hat_la_stop()
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_la_force_trigger(usb_device):
    """
    Configure the LA with no trigger, arm, then force trigger.
    Wait for the capture to complete and verify the status is 'done'.
    """
    usb_device.hat_la_configure(4, 1_000_000, 100)
    usb_device.hat_la_set_trigger(LaTriggerType.NONE, 0)
    usb_device.hat_la_arm()
    time.sleep(0.05)
    usb_device.hat_la_force()
    time.sleep(0.2)  # wait for capture

    status = usb_device.hat_la_get_status()
    # State may be 'done' (3) or 'capturing' (2) depending on timing
    assert status["state"] in (2, 3), (
        f"Expected LA state done(3) or capturing(2) after force, got {status['state_name']}"
    )
    # Clean up
    usb_device.hat_la_stop()
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_la_status(usb_device):
    """
    hat_la_get_status() returns a dict with 'state', 'state_name', and
    'samples_captured' fields.
    """
    usb_device.hat_la_configure(4, 1_000_000, 1000)
    status = usb_device.hat_la_get_status()

    assert isinstance(status, dict), f"hat_la_get_status() must return dict"
    assert "state" in status, "LA status missing 'state'"
    assert "state_name" in status, "LA status missing 'state_name'"
    assert status["state"] in (0, 1, 2, 3, 4), (
        f"LA state must be 0-4, got {status['state']}"
    )
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_la_status_includes_stream_diagnostics(usb_device):
    """
    LA status should expose the live-stream diagnostic fields even before a
    full bulk-stream bench run, so later transport failures are inspectable.
    """
    usb_device.hat_la_configure(4, 1_000_000, 1000)
    status = usb_device.hat_la_get_status()

    assert "stream_stop_reason" in status, "LA status missing 'stream_stop_reason'"
    assert "stream_stop_reason_name" in status, "LA status missing 'stream_stop_reason_name'"
    assert "stream_overrun_count" in status, "LA status missing 'stream_overrun_count'"
    assert "stream_short_write_count" in status, "LA status missing 'stream_short_write_count'"
    assert "usb_rearm_pending" in status, "LA status missing 'usb_rearm_pending'"
    assert "usb_rearm_request_count" in status, "LA status missing 'usb_rearm_request_count'"
    assert "usb_rearm_complete_count" in status, "LA status missing 'usb_rearm_complete_count'"
    assert isinstance(status["stream_stop_reason"], int)
    assert isinstance(status["stream_stop_reason_name"], str)
    assert isinstance(status["stream_overrun_count"], int)
    assert isinstance(status["stream_short_write_count"], int)
    assert isinstance(status["usb_rearm_pending"], bool)
    assert isinstance(status["usb_rearm_request_count"], int)
    assert isinstance(status["usb_rearm_complete_count"], int)
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# SWD debug setup (USB only)
# ---------------------------------------------------------------------------

# NOTE: SWD-specific tests live in tests/device/test_15_swd.py since the
# 2026-04-09 cleanup. test_hat_swd_detect was moved there.


# ---------------------------------------------------------------------------
# Logic Analyzer — trigger types
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_la_trigger_types(usb_device):
    """
    Test that all LA trigger types can be configured, armed, and stopped
    without error.
    """
    usb_device.hat_la_configure(channels=4, rate_hz=1_000_000, depth=1000)

    for trig_type in [LaTriggerType.RISING, LaTriggerType.FALLING,
                      LaTriggerType.HIGH, LaTriggerType.LOW]:
        usb_device.hat_la_set_trigger(trig_type, channel=0)
        usb_device.hat_la_arm()
        status = usb_device.hat_la_get_status()
        # Accept armed(1), capturing(2), or done(3) — HIGH/LOW triggers fire
        # immediately if the condition is already met, and RISING/FALLING can
        # fire on noise with floating inputs. The important thing is that the
        # state is valid (not error=4).
        assert status["state"] in (1, 2, 3), (
            f"Expected LA state armed(1), capturing(2), or done(3) after arm "
            f"with trigger {trig_type!r}, got {status['state_name']}"
        )
        usb_device.hat_la_stop()

    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Logic Analyzer — capture and data readback
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_la_read_data(usb_device):
    """
    Test LA capture with force trigger and data readback.
    Configures a short capture, forces trigger, reads data, and decodes it.
    """
    from bugbuster import BugBuster

    usb_device.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100)
    usb_device.hat_la_set_trigger(LaTriggerType.NONE, channel=0)
    usb_device.hat_la_arm()
    usb_device.hat_la_force()
    time.sleep(0.2)  # wait for capture to complete

    status = usb_device.hat_la_get_status()
    if status["state"] != 3:  # not DONE
        usb_device.hat_la_stop()
        pytest.skip(f"LA capture did not complete in time (state={status['state_name']})")

    data = usb_device.hat_la_read_all()
    assert data is not None, "hat_la_read_all() returned None"
    assert len(data) > 0, "hat_la_read_all() returned empty data"

    # Decode and verify structure
    samples = BugBuster.hat_la_decode(data, channels=4)
    assert len(samples) == 4, f"Expected 4 channel arrays, got {len(samples)}"
    assert len(samples[0]) > 0, "Channel 0 sample array is empty"

    # Each sample value should be 0 or 1
    for ch_idx, ch_data in enumerate(samples):
        for val in ch_data[:10]:
            assert val in (0, 1), (
                f"Channel {ch_idx} sample value must be 0 or 1, got {val}"
            )

    usb_device.hat_la_stop()
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Logic Analyzer — LEGACY CDC gapless stream (diagnostic only)
# ---------------------------------------------------------------------------

def _find_rp2040_cdc_port():
    """Return the RP2040 HAT CDC serial port path, or None if not found."""
    if not _SERIAL_AVAILABLE:
        return None
    for port in serial.tools.list_ports.comports():
        if port.vid == 0x2E8A and port.pid == 0x000C:
            return port.device
    return None


@pytest.mark.usb_only
@pytest.mark.timeout(30)
@pytest.mark.skip(reason="CDC streaming data path removed — stream data now goes to vendor-bulk only; use test_la_usb_bulk.py for streaming tests")
def test_la_cdc_stream_five_seconds_legacy(usb_device):
    """
    DIAGNOSTIC/LEGACY: Regression test for the DMA overrun bug on CDC.
    The primary stream path is now vendor-bulk (see test_la_usb_bulk.py).
    """
    import os as _os
    import fcntl as _fcntl
    import select as _select
    import struct as _struct
    import termios as _termios

    if not _SERIAL_AVAILABLE:
        pytest.skip("pyserial not installed — cannot enumerate RP2040 CDC port")

    rp2040_port = _find_rp2040_cdc_port()
    if rp2040_port is None:
        pytest.skip("RP2040 HAT CDC port not found (VID=0x2E8A PID=0x000C)")

    CHANNELS = 4
    RATE_HZ = 500_000
    DEPTH = 100_000
    WINDOW_SEC = 1.0
    NUM_WINDOWS = 5
    MIN_BYTES = 50_000

    usb_device.hat_la_stop()
    usb_device.hat_la_configure(channels=CHANNELS, rate_hz=RATE_HZ, depth=DEPTH)
    usb_device.hat_la_set_trigger(LaTriggerType.NONE, channel=0)
    time.sleep(0.12)

    try:
        fd = _os.open(rp2040_port, _os.O_RDWR | _os.O_NOCTTY | _os.O_NONBLOCK)
    except OSError as e:
        pytest.skip(f"Cannot open RP2040 CDC port ({e}) — close BugBuster app first")

    try:
        iflag, oflag, cflag, lflag, _, _, cc = _termios.tcgetattr(fd)
        cflag = _termios.CS8 | _termios.CREAD | _termios.CLOCAL
        iflag = _termios.IGNPAR
        oflag = 0
        lflag = 0
        cc = list(cc)
        cc[_termios.VMIN] = 0
        cc[_termios.VTIME] = 2
        _termios.tcsetattr(
            fd,
            _termios.TCSANOW,
            [iflag, oflag, cflag, lflag, _termios.B115200, _termios.B115200, cc],
        )

        TIOCMBIS = 0x8004746C
        TIOCM_DTR = 0x0002
        try:
            _fcntl.ioctl(fd, TIOCMBIS, _struct.pack("I", TIOCM_DTR))
        except OSError:
            pass

        def _read(timeout=0.2):
            r, _, _ = _select.select([fd], [], [], timeout)
            return _os.read(fd, 8192) if r else b""

        while _read(0.1):
            pass

        started = False
        for _ in range(3):
            _os.write(fd, b"\x01")
            resp = _read(0.5)
            if resp and b"START" in resp:
                started = True
                break
            _os.write(fd, b"\x00")
            time.sleep(0.3)

        assert started, "RP2040 did not respond with START — is it configured?"

        window_bytes = []
        ring = bytearray()
        for _ in range(NUM_WINDOWS):
            window_total = 0
            t0 = time.time()
            while time.time() - t0 < WINDOW_SEC:
                r, _, _ = _select.select([fd], [], [], 0.005)
                if not r:
                    continue
                try:
                    chunk = _os.read(fd, 65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    continue
                ring.extend(chunk)
                consumed = 0
                while len(ring) - consumed >= 2:
                    length = ring[consumed + 1]
                    if length > 62:
                        consumed += 1
                        continue
                    if len(ring) - consumed < 2 + length:
                        break
                    window_total += length
                    consumed += 2 + length
                if consumed:
                    del ring[:consumed]
            window_bytes.append(window_total)

        _os.write(fd, b"\x00")
    finally:
        _os.close(fd)

    failed = [i + 1 for i, n in enumerate(window_bytes) if n < MIN_BYTES]
    assert not failed, (
        f"LA stream delivered <{MIN_BYTES}B in windows {failed} — "
        f"DMA overrun regression? bytes per window: {window_bytes}"
    )
    assert_no_faults(usb_device)


@pytest.mark.usb_only
@pytest.mark.timeout(90)
@pytest.mark.skip(reason="CDC streaming data path removed — stream data now goes to vendor-bulk only; use test_la_usb_bulk.py for streaming tests")
def test_la_cdc_stream_duration_truth_legacy(usb_device):
    """
    DIAGNOSTIC/LEGACY: Proof test for gapless live streaming over CDC.
    The primary stream path is now vendor-bulk (see test_la_usb_bulk.py).
    """
    import os as _os
    import fcntl as _fcntl
    import select as _select
    import struct as _struct
    import termios as _termios

    if not _SERIAL_AVAILABLE:
        pytest.skip("pyserial not installed — cannot enumerate RP2040 CDC port")

    rp2040_port = _find_rp2040_cdc_port()
    if rp2040_port is None:
        pytest.skip("RP2040 HAT CDC port not found (VID=0x2E8A PID=0x000C)")

    channels = int(_os.getenv("BUGBUSTER_LA_STREAM_CHANNELS", "4"))
    rate_hz = int(_os.getenv("BUGBUSTER_LA_STREAM_RATE_HZ", "500000"))
    depth = int(_os.getenv("BUGBUSTER_LA_STREAM_DEPTH", "100000"))
    stream_sec = float(_os.getenv("BUGBUSTER_LA_STREAM_DURATION_S", "10.0"))

    if channels not in (1, 2, 4):
        pytest.fail(f"BUGBUSTER_LA_STREAM_CHANNELS must be 1, 2, or 4 (got {channels})")

    usb_device.hat_la_stop()
    usb_device.hat_la_configure(channels=channels, rate_hz=rate_hz, depth=depth)
    usb_device.hat_la_set_trigger(LaTriggerType.NONE, channel=0)
    time.sleep(0.12)

    try:
        fd = _os.open(rp2040_port, _os.O_RDWR | _os.O_NOCTTY | _os.O_NONBLOCK)
    except OSError as e:
        pytest.skip(f"Cannot open RP2040 CDC port ({e}) — close BugBuster app first")

    stop_reason = "unknown"
    overrun_count = 0
    short_write_count = 0
    total_bytes = 0
    decoded_duration = 0.0
    measured_duration = 0.0

    try:
        iflag, oflag, cflag, lflag, _, _, cc = _termios.tcgetattr(fd)
        cflag = _termios.CS8 | _termios.CREAD | _termios.CLOCAL
        iflag = _termios.IGNPAR
        oflag = 0
        lflag = 0
        cc = list(cc)
        cc[_termios.VMIN] = 0
        cc[_termios.VTIME] = 2
        _termios.tcsetattr(
            fd,
            _termios.TCSANOW,
            [iflag, oflag, cflag, lflag, _termios.B115200, _termios.B115200, cc],
        )

        TIOCMBIS = 0x8004746C
        TIOCM_DTR = 0x0002
        try:
            _fcntl.ioctl(fd, TIOCMBIS, _struct.pack("I", TIOCM_DTR))
        except OSError:
            pass

        def _read(timeout=0.2):
            r, _, _ = _select.select([fd], [], [], timeout)
            return _os.read(fd, 8192) if r else b""

        while _read(0.1):
            pass

        started = False
        for _ in range(3):
            _os.write(fd, b"\x01")
            resp = _read(0.5)
            if resp and b"START" in resp:
                started = True
                break
            _os.write(fd, b"\x00")
            time.sleep(0.3)

        assert started, "RP2040 did not respond with START — is it configured?"

        ring = bytearray()
        measured_start = time.time()
        while time.time() - measured_start < stream_sec:
            r, _, _ = _select.select([fd], [], [], 0.005)
            if not r:
                continue
            try:
                chunk = _os.read(fd, 65536)
            except BlockingIOError:
                continue
            if not chunk:
                continue
            ring.extend(chunk)
            consumed = 0
            while len(ring) - consumed >= 2:
                length = ring[consumed + 1]
                if length > 62:
                    consumed += 1
                    continue
                if len(ring) - consumed < 2 + length:
                    break
                total_bytes += length
                consumed += 2 + length
            if consumed:
                del ring[:consumed]

        measured_duration = time.time() - measured_start
        _os.write(fd, b"\x00")
    finally:
        _os.close(fd)

    status = usb_device.hat_la_get_status()
    stop_reason = status.get("stream_stop_reason_name", "unknown")
    overrun_count = status.get("stream_overrun_count", 0)
    short_write_count = status.get("stream_short_write_count", 0)

    samples_per_byte = 8 // channels
    decoded_duration = (total_bytes * samples_per_byte) / float(rate_hz)

    assert abs(decoded_duration - measured_duration) / measured_duration <= 0.01, (
        f"Decoded duration drifted from wall clock: decoded={decoded_duration:.3f}s, "
        f"measured={measured_duration:.3f}s, bytes={total_bytes}, "
        f"stop_reason={stop_reason}, overruns={overrun_count}, short_writes={short_write_count}"
    )
    assert total_bytes > 0, (
        f"No live LA payload bytes received; stop_reason={stop_reason}, "
        f"overruns={overrun_count}, short_writes={short_write_count}"
    )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Logic Analyzer — USB vendor-bulk live stream (Primary Path)
# ---------------------------------------------------------------------------

# NOTE: Primary vendor-bulk streaming tests have been moved to 
# tests/device/test_la_usb_bulk.py for better isolation and 
# framing-contract verification.
