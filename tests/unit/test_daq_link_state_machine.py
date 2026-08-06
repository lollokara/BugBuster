"""Compile and run the pure-Swift DaqLinkStateMachine tests on the host toolchain.

The iOS app has no XCTest target, but this state machine is deliberately
Foundation-only so its behavior can be tested for real rather than by
source-string assertion.
"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from tests.lib.srcread import read_source

SM = Path("iOSApp/Sources/Services/DaqLinkState.swift")
AXIS = Path("iOSApp/Sources/Services/ScopeAxis.swift")
ACQ_CFG = Path("iOSApp/Sources/Services/DaqAcquisitionConfig.swift")
TESTS = Path("tests/ios/main.swift")
AXIS_TESTS = Path("tests/ios/ScopeAxisTests.swift")
ACQ_CFG_TESTS = Path("tests/ios/DaqAcquisitionConfigTests.swift")

# Prefer the Xcode-selected toolchain; see the comment at the build call.
XCRUN = shutil.which("xcrun")


@pytest.mark.skipif(shutil.which("swiftc") is None and shutil.which("xcrun") is None,
                    reason="no Swift toolchain available")
def test_daq_link_state_machine_behavior():
    assert SM.exists(), f"{SM} not created"
    src = read_source(SM)
    for forbidden in ("import Network", "import UIKit", "import SwiftUI",
                      "import NetworkExtension"):
        assert forbidden not in src, (
            f"{SM} must stay platform-free ({forbidden}) so it is host-testable")

    assert ACQ_CFG.exists(), f"{ACQ_CFG} not created"
    acq_src = read_source(ACQ_CFG)
    for forbidden in ("import Network", "import UIKit", "import SwiftUI",
                      "import NetworkExtension"):
        assert forbidden not in acq_src, (
            f"{ACQ_CFG} must stay Foundation-only ({forbidden}) so it is host-testable")

    with tempfile.TemporaryDirectory() as td:
        binary = Path(td) / "smtests"
        # Build through `xcrun`, not a bare `swiftc`.
        #
        # A bare `swiftc` resolves to the Command Line Tools toolchain, which
        # on a machine with a newer Xcode installed is OLDER than the macOS SDK
        # it then tries to use. The Swift stdlib .swiftinterface is versioned,
        # so the mismatch fails the build outright with "this SDK is not
        # supported by the compiler" -- nothing to do with the code under test,
        # but it made this whole suite unrunnable. `xcrun` selects the
        # toolchain that matches the active developer directory.
        # An inherited SDKROOT wins over xcrun's own selection, and pytest may
        # itself be running under Xcode's bundled python3, which exports one
        # pointing at the Command Line Tools SDK. Drop it and pin the SDK we
        # actually resolved, so the toolchain and SDK are guaranteed to match.
        env = {k: v for k, v in os.environ.items() if k != "SDKROOT"}
        cmd = [XCRUN, "swiftc"] if XCRUN else ["swiftc"]
        if XCRUN:
            sdk = subprocess.run([XCRUN, "--sdk", "macosx", "--show-sdk-path"],
                                 capture_output=True, text=True, env=env)
            if sdk.returncode == 0 and sdk.stdout.strip():
                cmd += ["-sdk", sdk.stdout.strip()]
        build = subprocess.run(
            cmd + ["-swift-version", "5", str(SM), str(AXIS), str(ACQ_CFG),
                   str(TESTS), str(AXIS_TESTS), str(ACQ_CFG_TESTS),
                   "-o", str(binary)],
            capture_output=True, text=True, env=env)
        assert build.returncode == 0, f"swiftc failed:\n{build.stderr}"
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        assert run.returncode == 0, f"state machine tests failed:\n{run.stdout}"


# --- Task 8: DaqWifiStreamManager adopts the state machine ------------------

MGR = Path("iOSApp/Sources/Services/DaqWifiStreamManager.swift")


def test_manager_adopts_the_state_machine_and_drops_the_ad_hoc_flags():
    src = read_source(MGR)
    assert "DaqLinkStateMachine" in src, "manager does not use the state machine"
    assert "@Published private(set) var linkState" in src
    # The replaced flags must no longer be independently stored state.
    for gone in ("@Published var isConnected", "@Published var isStreaming",
                 "@Published var provisioningState", "private var reconnectAttempts"):
        assert gone not in src, f"stale ad-hoc state still present: {gone}"


def test_every_effect_has_exactly_one_performer():
    src = read_source(MGR)
    for effect in ("requestProvisioning", "joinHotspot", "openSocket", "sendStart",
                   "sendStop", "closeSocket", "removeHotspotConfig", "recycleDevice",
                   "resetBuffers", "scheduleRetry"):
        assert src.count(f"case .{effect}") == 1, (
            f"effect .{effect} must have exactly one performer, "
            f"got {src.count(f'case .{effect}')}")


def test_hotspot_configuration_removed_in_exactly_one_place():
    src = read_source(MGR)
    assert src.count("removeConfiguration(forSSID") == 1, (
        "hotspot removal must have a single owner — multiple call sites caused "
        "the removal-races-connect bug")


def test_watchdog_detects_a_connected_but_silent_link():
    src = read_source(MGR)
    assert "lastFrameAt" in src
    assert "dataStalled" in src, "nothing ever raises the stall event"


def test_recovery_calls_the_device_recycle_endpoint():
    src = read_source(MGR)
    assert "/api/daq/wifi_stream/recycle" in src


# --- Task 9: ScopeTab renders from the state projection ---------------------

SCOPE = Path("iOSApp/Sources/Views/ScopeTab.swift")


def test_scope_tab_uses_the_state_projection_not_deleted_apis():
    src = read_source(SCOPE)
    for gone in ("startFullStreamFlow", "requestStreamStop", "provisioningState"):
        assert gone not in src, f"ScopeTab still calls removed API: {gone}"
    assert "linkState" in src


def test_failure_offers_retry_rather_than_a_dead_end():
    src = read_source(SCOPE)
    assert "daqStream.retry()" in src, "no Retry affordance — user is stranded"


def test_recovering_state_is_visible_to_the_user():
    """Silent recovery looks like a hang; show what is happening."""
    src = read_source(SCOPE)
    assert "userFacingLabel" in src
