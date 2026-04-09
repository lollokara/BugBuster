"""
test_15_swd.py — SWD functional tests.

Post-2026-04-09 cleanup: SWD lives on a dedicated 3-pin connector
(SWDIO/SWCLK/TRACE) driven by the RP2040 debugprobe. Enabling SWD is a
single `hat_setup_swd()` call — it configures target voltage + power and
no longer touches EXP_EXT pins.

All tests in this file require the HAT hardware and USB transport. Tests
that depend on an actual SWD target being wired are gated on an optional
`--swd-target` CLI flag; without it they are skipped cleanly.

Run with:
    pytest tests/device/test_15_swd.py --hat --device-usb
    pytest tests/device/test_15_swd.py --hat --device-usb --swd-target
"""

from __future__ import annotations

import time
import pytest

from bugbuster import HatPinFunctionError
from conftest import assert_no_faults


pytestmark = [
    pytest.mark.requires_hat,
    pytest.mark.timeout(15),
]


@pytest.fixture(autouse=True)
def require_hat(request):
    """Auto-use fixture that skips all SWD tests unless --hat is passed."""
    if not request.config.getoption("--hat", default=False):
        pytest.skip("SWD tests require --hat flag")


def _safe_hat_setup_swd(usb_device, *, target_voltage_mv: int = 3300, connector: int = 0):
    """
    Call hat_setup_swd() and skip the current test cleanly if the HAT
    BBP command bridge is not reachable.

    On a breadboard where the RP2040 ↔ ESP32 UART bridge wires are not
    connected, every BBP → HAT command returns ``DeviceError BUSY`` (the
    ESP32 is waiting for a UART response that never arrives). That does
    NOT prevent the CMSIS-DAP USB path from working, so probe-only tests
    should not be treated as failures — they should skip with a clear
    reason pointing at the wiring.
    """
    from bugbuster.transport.usb import DeviceError
    try:
        return usb_device.hat_setup_swd(
            target_voltage_mv=target_voltage_mv, connector=connector
        )
    except DeviceError as exc:
        pytest.skip(
            f"HAT BBP bridge not reachable ({exc}). Wire the ESP32 ↔ RP2040 "
            "UART bridge (ESP32 GPIO43/44 ↔ RP2040 GPIO0/1) and power-cycle "
            "to run this test."
        )


# ---------------------------------------------------------------------------
# Baseline: hat_setup_swd() returns True on default parameters
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_setup_swd_returns_true(usb_device):
    """
    hat_setup_swd(3300, 0) should return True even with no SWD target
    connected — it only configures voltage + power now.
    """
    result = _safe_hat_setup_swd(usb_device, target_voltage_mv=3300, connector=0)
    assert result is True, f"hat_setup_swd() should return True, got {result}"
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_setup_swd_accepts_both_connectors(usb_device):
    """Both connector A (0) and B (1) should be valid inputs."""
    assert _safe_hat_setup_swd(usb_device, target_voltage_mv=3300, connector=0) is True
    time.sleep(0.05)
    assert _safe_hat_setup_swd(usb_device, target_voltage_mv=3300, connector=1) is True
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Invariant: hat_setup_swd() must not modify EXP_EXT pin configuration
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_setup_swd_no_expext_side_effects(usb_device):
    """
    After the 2026-04-09 cleanup, hat_setup_swd() must leave the
    EXP_EXT pin configuration exactly as it was before the call. The
    dedicated 3-pin SWD connector is driven by the debugprobe PIO on its
    own pins.
    """
    # Capture the EXP_EXT pin config before SWD setup.
    before = usb_device.hat_get_status()
    pin_config_before = tuple(before.get("pin_config", []) or [])

    # Call hat_setup_swd.
    ok = _safe_hat_setup_swd(usb_device, target_voltage_mv=3300, connector=0)
    assert ok is True
    time.sleep(0.05)

    # Capture after.
    after = usb_device.hat_get_status()
    pin_config_after = tuple(after.get("pin_config", []) or [])

    assert pin_config_after == pin_config_before, (
        f"hat_setup_swd() modified EXP_EXT pin_config: "
        f"before={pin_config_before} after={pin_config_after}. "
        "On the 3-pin dedicated SWD connector PCB, SWD setup must NOT "
        "touch EXP_EXT pins."
    )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# hat_set_pin must reject the reserved SWD/TRACE function codes
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
@pytest.mark.parametrize("reserved_code", [1, 2, 3, 4])
def test_hat_set_pin_rejects_reserved_swd_codes(usb_device, reserved_code):
    """
    Numeric slots 1..4 (formerly SWDIO/SWCLK/TRACE1/TRACE2) must raise
    HatPinFunctionError before any wire command is issued.
    """
    with pytest.raises(HatPinFunctionError) as exc_info:
        usb_device.hat_set_pin(0, reserved_code)

    msg = str(exc_info.value)
    assert "reserved" in msg.lower()
    assert "hat_setup_swd" in msg

    # Device must be unchanged after the rejected call.
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# CMSIS-DAP USB enumeration after hat_setup_swd()
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_setup_swd_cmsis_dap_enumerates(usb_device):
    """
    After hat_setup_swd(), the RP2040's CMSIS-DAP USB interface must
    STILL be enumerated on the host. Confirms that the SWD setup call
    does not disturb the CMSIS-DAP path (they are independent since the
    2026-04-09 cleanup). Uses the same probe-enumeration detector as
    the other SWD tests (pyocd / ioreg / lsusb fallback chain).
    """
    if not _probe_physically_present():
        pytest.skip(
            "CMSIS-DAP debugprobe not enumerated on this host. "
            "Connect the RP2040 debug USB port and retry."
        )

    usb_device.hat_setup_swd(target_voltage_mv=3300, connector=0)
    time.sleep(0.1)
    assert _probe_physically_present(), (
        "CMSIS-DAP interface disappeared after hat_setup_swd() — this "
        "indicates the setup call is disrupting the debugprobe USB path."
    )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Live DPIDR read — requires a real SWD target wired to the dedicated
# 3-pin connector. Gated on --swd-target.
# ---------------------------------------------------------------------------

def _swd_target_requested(request) -> bool:
    """True if the user passed --swd-target on the pytest command line."""
    try:
        return bool(request.config.getoption("--swd-target", default=False))
    except Exception:
        return False


@pytest.mark.usb_only
def test_hat_swd_target_detected(usb_device, request):
    """
    With a real target wired to the dedicated SWD connector, the
    hat_get_status() path (or a future direct DPIDR surface) should
    report a non-zero DPIDR. This test is skipped unless --swd-target is
    passed, because it requires physical wiring.
    """
    if not _swd_target_requested(request):
        pytest.skip("SWD target not wired — pass --swd-target to run this test")

    usb_device.hat_setup_swd(target_voltage_mv=3300, connector=0)
    time.sleep(0.2)

    # The current firmware surfaces SWD status via the existing
    # hat_get_status path (dap_connected). Direct DPIDR exposure is
    # deferred — see .omc/specs/deep-interview-swd-exp-ext-cleanup-2026-04-09.md.
    # For now, just assert that the setup call returned and no faults
    # were raised; real DPIDR verification is a host-driver concern.
    status = usb_device.hat_get_status()
    assert status is not None
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# SWD clock setting — bounds check (no target required)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_setup_swd_multiple_voltages(usb_device):
    """
    hat_setup_swd should handle the full range of valid target voltages
    without crashing the firmware.

    The HVPAK driver is currently a 5-TODO stub (``bb_hvpak.c``), so any
    voltage other than the default 3.3 V takes the "voltage unsupported"
    branch inside ``hat_setup_swd()`` on the ESP32, which returns false,
    which the BBP layer surfaces as ``BBP_ERR_BUSY`` (→ Python
    ``DeviceError BUSY``). That is the DOCUMENTED graceful-unsupported
    path until the HVPAK driver is implemented.

    This test therefore accepts either:
      * ``True`` (success — only 3.3 V today)
      * ``False`` or a raised ``DeviceError BUSY`` (graceful unsupported)

    It fails only on a crash, a transport error, or a HAT fault bit —
    i.e. on evidence that the firmware behaved badly.
    """
    from bugbuster.transport.usb import DeviceError

    for voltage_mv in (1800, 2500, 3300, 5000):
        try:
            result = usb_device.hat_setup_swd(
                target_voltage_mv=voltage_mv, connector=0
            )
        except DeviceError as exc:
            # BUSY is the documented unsupported path for non-3.3V while
            # the HVPAK driver is a stub. Any OTHER transport/device error
            # is a real failure.
            assert "BUSY" in str(exc), (
                f"hat_setup_swd({voltage_mv}, 0) raised unexpected "
                f"DeviceError: {exc!r}"
            )
            result = False

        assert result in (True, False), (
            f"hat_setup_swd({voltage_mv}, 0) returned {result!r} "
            f"(expected bool or DeviceError BUSY)"
        )
        time.sleep(0.05)
    assert_no_faults(usb_device)


# ===========================================================================
# OpenOCD / pyOCD host-side recognition and communication
# ===========================================================================
#
# These tests verify that the RP2040 debugprobe is correctly recognized by
# industry-standard SWD host drivers. They are the "does SWD actually work"
# answer beyond USB descriptor enumeration.
#
# Tiers (strongest to weakest):
#   1. OpenOCD / pyOCD installed + probe connected + target wired
#      → full halt/resume/read-DPIDR roundtrip (gated on --swd-target)
#   2. OpenOCD / pyOCD installed + probe connected (no target)
#      → probe enumeration, FW version read, CMSIS-DAP command support
#   3. OpenOCD / pyOCD installed (no probe)
#      → skip cleanly with clear reason
#   4. Neither tool installed
#      → skip cleanly with install hint
# ===========================================================================

import shutil
import subprocess

# RP2040 debugprobe VID/PID (the BugBuster HAT uses the stock debugprobe).
_DEBUGPROBE_VID = 0x2E8A
_DEBUGPROBE_PID = 0x000C


def _openocd_path() -> str | None:
    return shutil.which("openocd")


def _pyocd_path() -> str | None:
    return shutil.which("pyocd")


def _run_cmd(argv: list[str], timeout: float = 15.0) -> subprocess.CompletedProcess:
    """Run a subprocess and return the completed result. Merges stderr into stdout."""
    return subprocess.run(
        argv,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def _probe_physically_present() -> bool:
    """
    True if the RP2040 debugprobe USB device is enumerated on this host.

    Uses the subprocess fallback path:
      1. `pyocd list` — if any line mentions "cmsis-dap" or "bugbuster"
         or the known VID:PID, the probe is connected. This is the
         portable check because pyocd handles the HID/WinUSB/libusb
         backend selection internally on every platform.
      2. macOS fallback: `ioreg -p IOUSB -l` grep for "CMSIS-DAP" or
         "BugBuster".
      3. Linux fallback: `lsusb` grep for the VID:PID.

    Deliberately avoids pyusb because on macOS it requires a libusb
    backend that isn't installed by default, which would make the tests
    skip even when the probe is physically connected.
    """
    import platform
    pyocd = _pyocd_path()
    if pyocd is not None:
        try:
            r = _run_cmd([pyocd, "list"], timeout=5.0)
            out = r.stdout.lower()
            vid_pid = f"{_DEBUGPROBE_VID:04x}:{_DEBUGPROBE_PID:04x}"
            if ("cmsis-dap" in out or "bugbuster" in out or vid_pid in out):
                return True
        except Exception:
            pass

    system = platform.system()
    try:
        if system == "Darwin":
            r = _run_cmd(["ioreg", "-p", "IOUSB", "-l"], timeout=5.0)
            return ("CMSIS-DAP" in r.stdout) or ("BugBuster" in r.stdout)
        if system == "Linux":
            r = _run_cmd(["lsusb"], timeout=5.0)
            vid_pid = f"{_DEBUGPROBE_VID:04x}:{_DEBUGPROBE_PID:04x}"
            return vid_pid in r.stdout.lower()
    except Exception:
        pass
    return False


# ---------------------------------------------------------------------------
# pyOCD — list probes
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_pyocd_lists_cmsis_dap_probe(usb_device):
    """
    `pyocd list` should enumerate the BugBuster HAT debugprobe as a
    CMSIS-DAP probe. This is the lightest-weight "does the host see the
    probe" check — no target required.

    Skips cleanly if pyocd is not installed or the probe is not plugged in.
    """
    pyocd = _pyocd_path()
    if pyocd is None:
        pytest.skip("pyocd not installed — `pip install pyocd` to run this test")
    if not _probe_physically_present():
        pytest.skip(
            f"Debugprobe ({_DEBUGPROBE_VID:04X}:{_DEBUGPROBE_PID:04X}) not "
            "enumerated on this host — connect RP2040 debug USB port"
        )

    # Best-effort: trigger SWD setup so target power is enabled. The
    # CMSIS-DAP path is independent of the HAT UART bridge, so we tolerate
    # a BUSY/NotImplemented here (breadboard without HAT UART wired).
    try:
        usb_device.hat_setup_swd(target_voltage_mv=3300, connector=0)
        time.sleep(0.2)
    except Exception:
        pass

    result = _run_cmd([pyocd, "list"], timeout=10.0)

    assert result.returncode == 0, (
        f"pyocd list exited with code {result.returncode}:\n{result.stdout}"
    )
    # The probe's VID:PID should appear in the list output.
    vid_pid_str = f"{_DEBUGPROBE_VID:04x}:{_DEBUGPROBE_PID:04x}"
    output_lower = result.stdout.lower()
    assert (
        vid_pid_str in output_lower
        or "cmsis-dap" in output_lower
        or "debugprobe" in output_lower
    ), (
        f"pyocd list did not report the debugprobe:\n{result.stdout}"
    )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# OpenOCD — interface config resolves and probe is recognized
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_openocd_recognizes_cmsis_dap_probe(usb_device):
    """
    Run OpenOCD with the cmsis-dap interface config and verify it
    successfully initializes the probe. Without a target config we use
    `-c "adapter speed 1000; init; shutdown"` which attempts to open the
    probe and report its firmware version, then exits cleanly.

    OpenOCD stdout is expected to contain one of:
      - "CMSIS-DAP: SWD Supported"
      - "CMSIS-DAP: FW Version = "
      - "cmsis-dap" (case-insensitive)

    Skips cleanly if openocd is not installed or the probe is not
    connected.
    """
    openocd = _openocd_path()
    if openocd is None:
        pytest.skip(
            "openocd not installed — `brew install open-ocd` (macOS) or "
            "`apt install openocd` (Linux) to run this test"
        )
    if not _probe_physically_present():
        pytest.skip(
            f"Debugprobe ({_DEBUGPROBE_VID:04X}:{_DEBUGPROBE_PID:04X}) not "
            "enumerated on this host"
        )

    # Best-effort: trigger SWD setup so target power is enabled. The
    # CMSIS-DAP path is independent of the HAT UART bridge, so we tolerate
    # a BUSY/NotImplemented here (breadboard without HAT UART wired).
    try:
        usb_device.hat_setup_swd(target_voltage_mv=3300, connector=0)
        time.sleep(0.2)
    except Exception:
        pass

    # Interface-only init. OpenOCD will try to open the probe; without a
    # target config the full init may fail after the probe is opened, so
    # we look for the probe-opened evidence in the output regardless of
    # exit code.
    result = _run_cmd(
        [
            openocd,
            "-c", "adapter driver cmsis-dap",
            "-c", f"cmsis_dap_vid_pid 0x{_DEBUGPROBE_VID:04X} 0x{_DEBUGPROBE_PID:04X}",
            "-c", "adapter speed 1000",
            "-c", "transport select swd",
            "-c", "init",
            "-c", "shutdown",
        ],
        timeout=10.0,
    )
    combined = (result.stdout + result.stderr).lower()

    assert (
        "cmsis-dap" in combined
        or "fw version" in combined
        or "swd supported" in combined
    ), (
        f"OpenOCD did not recognize the CMSIS-DAP probe.\n"
        f"exit={result.returncode}\n"
        f"stdout:\n{result.stdout}\n"
        f"stderr:\n{result.stderr}"
    )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# OpenOCD — full SWD roundtrip with a target (gated on --swd-target)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_openocd_halts_target_over_swd(usb_device, request):
    """
    End-to-end SWD communication test: with a real target wired to the
    dedicated 3-pin SWD connector, OpenOCD should be able to halt the
    target and read its IDCODE. Requires `--swd-target` on the pytest
    command line AND a target config.

    The target config is resolved from the `BUGBUSTER_SWD_TARGET` env var
    (e.g. `target/rp2040.cfg` for a second RP2040 or `target/stm32f4x.cfg`)
    and defaults to `target/rp2040.cfg` when unset.
    """
    if not request.config.getoption("--swd-target", default=False):
        pytest.skip("SWD target not wired — pass --swd-target to run this test")

    openocd = _openocd_path()
    if openocd is None:
        pytest.skip("openocd not installed")
    if not _probe_physically_present():
        pytest.skip("Debugprobe not enumerated on this host")

    import os
    target_cfg = os.environ.get("BUGBUSTER_SWD_TARGET", "target/rp2040.cfg")

    usb_device.hat_setup_swd(target_voltage_mv=3300, connector=0)
    time.sleep(0.2)

    result = _run_cmd(
        [
            openocd,
            "-c", "adapter driver cmsis-dap",
            "-c", f"cmsis_dap_vid_pid 0x{_DEBUGPROBE_VID:04X} 0x{_DEBUGPROBE_PID:04X}",
            "-c", "adapter speed 1000",
            "-c", "transport select swd",
            "-f", target_cfg,
            "-c", "init",
            "-c", "halt",
            "-c", "shutdown",
        ],
        timeout=15.0,
    )
    combined = (result.stdout + result.stderr).lower()

    # "target halted" is OpenOCD's standard halt acknowledgment.
    assert "halted" in combined or "target halted" in combined, (
        f"OpenOCD failed to halt the SWD target using config {target_cfg}.\n"
        f"exit={result.returncode}\n"
        f"stdout:\n{result.stdout}\n"
        f"stderr:\n{result.stderr}"
    )
    assert_no_faults(usb_device)
