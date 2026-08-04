"""ADGS2414D MUX device-index parity across every surface that encodes it.

The MUX device index for IO_Block 3/4 is physically swapped (IO_Block 3's
switches live on device 3, IO_Block 4's on device 2).  That fact is currently
duplicated by hand in six places across four languages.  It has already drifted
once: it was flipped in the web UI (2026-06-04), reverted there (2026-06-13),
then flipped in hal.py alone (2026-07-21, commit 435de28), leaving the Python
client disagreeing with the firmware, the desktop app and the web UI for two
weeks -- during which the Python bus planner and the firmware bus planner
computed different switch states for the same request.

`bus_planner.cpp` even carries a "SYNC: keep aligned with hal.py" comment.
Comments do not fail builds.  This test does.

See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S1-1.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from bugbuster.hal import DEFAULT_ROUTING

REPO = Path(__file__).resolve().parents[2]

# The canonical mapping, expressed as logical channel / IO_Block index -> device.
#   index 0 = IO_Block 1 (IO 1-3, logical A)
#   index 1 = IO_Block 2 (IO 4-6, logical B)
#   index 2 = IO_Block 3 (IO 7-9, logical C)  <- swapped
#   index 3 = IO_Block 4 (IO 10-12, logical D) <- swapped
EXPECTED_BY_LOGICAL = [0, 1, 3, 2]


def _read(rel: str) -> str:
    path = REPO / rel
    if not path.is_file():
        pytest.skip(f"{rel} not present in this checkout")
    return path.read_text(encoding="utf-8", errors="replace")


def test_python_hal_routing_matches_expected():
    """hal.py DEFAULT_ROUTING is the reference every other surface mirrors."""
    for io_num, rt in DEFAULT_ROUTING.items():
        expected = EXPECTED_BY_LOGICAL[rt.io_block - 1]
        assert rt.mux_device == expected, (
            f"IO {io_num} (IO_Block {rt.io_block}) has mux_device="
            f"{rt.mux_device}, expected {expected}"
        )


def test_firmware_bus_planner_matches_hal():
    """Firmware/ESP32/src/bus/bus_planner.cpp IO_ROUTES vs hal.py."""
    src = _read("Firmware/ESP32/src/bus/bus_planner.cpp")
    table = re.search(r"IO_ROUTES\[12\]\s*=\s*\{(.*?)\n\};", src, re.S)
    assert table, "could not locate IO_ROUTES table in bus_planner.cpp"

    # Entries look like:  { 9,  1, 3, 10, PCA_CTRL_EFUSE3_EN, ...
    #                       io  pos dev gpio
    entries = re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),", table.group(1))
    assert len(entries) == 12, f"expected 12 IO_ROUTES entries, found {len(entries)}"

    for io_str, _pos, dev_str, _gpio in entries:
        io_num, dev = int(io_str), int(dev_str)
        expected = DEFAULT_ROUTING[io_num].mux_device
        assert dev == expected, (
            f"bus_planner.cpp IO {io_num} mux_device={dev}, "
            f"hal.py says {expected}"
        )


def test_firmware_tasks_logical_to_physical_matches_hal():
    """tasks.cpp maps logical channel -> mux device for the analog IOs."""
    src = _read("Firmware/ESP32/src/tasks.cpp")
    # Only IO_Block 3 (logical 2) and 4 (logical 3) are remapped; the function
    # defaults mux_dev = logical_channel for A/B.
    block = re.search(
        r"if\s*\(logical_channel\s*==\s*2\)\s*\{(.*?)\}\s*else\s+if\s*"
        r"\(logical_channel\s*==\s*3\)\s*\{(.*?)\}",
        src,
        re.S,
    )
    assert block, "could not locate logical->physical remap in tasks.cpp"

    for logical, body in ((2, block.group(1)), (3, block.group(2))):
        m = re.search(r"mux_dev\s*=\s*(\d+)\s*;", body)
        assert m, f"no mux_dev assignment for logical channel {logical}"
        assert int(m.group(1)) == EXPECTED_BY_LOGICAL[logical], (
            f"tasks.cpp logical {logical} -> mux_dev {m.group(1)}, "
            f"expected {EXPECTED_BY_LOGICAL[logical]}"
        )


def test_firmware_hat_block_to_mux_dev_matches_hal():
    """hat.cpp block_to_mux_dev drives the per-block status LEDs."""
    src = _read("Firmware/ESP32/src/hat/hat.cpp")
    m = re.search(r"block_to_mux_dev\[4\]\s*=\s*\{([^}]*)\}", src)
    assert m, "could not locate block_to_mux_dev in hat.cpp"
    got = [int(x.strip()) for x in m.group(1).split(",") if x.strip()]
    assert got == EXPECTED_BY_LOGICAL, (
        f"hat.cpp block_to_mux_dev={got}, expected {EXPECTED_BY_LOGICAL}"
    )


def test_desktop_signal_path_matches_hal():
    """DesktopApp signal_path.rs MUX_DEVICE_BY_LOGICAL."""
    src = _read("DesktopApp/BugBuster/src/tabs/signal_path.rs")
    m = re.search(r"MUX_DEVICE_BY_LOGICAL:\s*\[usize;\s*4\]\s*=\s*\[([^\]]*)\]", src)
    assert m, "could not locate MUX_DEVICE_BY_LOGICAL in signal_path.rs"
    got = [int(x.strip()) for x in m.group(1).split(",") if x.strip()]
    assert got == EXPECTED_BY_LOGICAL, (
        f"signal_path.rs MUX_DEVICE_BY_LOGICAL={got}, expected {EXPECTED_BY_LOGICAL}"
    )


def test_web_signal_path_matches_hal():
    """On-device web UI SignalPath.tsx MUX_DEVICE_BY_LOGICAL."""
    src = _read("Firmware/ESP32/web/src/tabs/signal/SignalPath.tsx")
    m = re.search(r"MUX_DEVICE_BY_LOGICAL\s*=\s*\[([^\]]*)\]", src)
    assert m, "could not locate MUX_DEVICE_BY_LOGICAL in SignalPath.tsx"
    got = [int(x.strip()) for x in m.group(1).split(",") if x.strip()]
    assert got == EXPECTED_BY_LOGICAL, (
        f"SignalPath.tsx MUX_DEVICE_BY_LOGICAL={got}, expected {EXPECTED_BY_LOGICAL}"
    )


def test_no_dead_jsx_twin_reintroduces_a_stale_mapping():
    """A dead .jsx twin of SignalPath.tsx once held the opposite mapping.

    The twin tree was deleted; this guards against it coming back with a
    divergent constant (the mechanism by which the 2026-06-13 fix was lost).
    """
    twin = REPO / "Firmware/ESP32/web/src/tabs/signal/SignalPath.jsx"
    assert not twin.exists(), (
        "SignalPath.jsx has reappeared -- a dead .jsx twin previously held a "
        "stale MUX_DEVICE_BY_LOGICAL and hid a reverted fix"
    )
