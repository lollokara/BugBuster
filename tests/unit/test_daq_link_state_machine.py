"""Compile and run the pure-Swift DaqLinkStateMachine tests on the host toolchain.

The iOS app has no XCTest target, but this state machine is deliberately
Foundation-only so its behavior can be tested for real rather than by
source-string assertion.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

SM = Path("iOSApp/Sources/Services/DaqLinkState.swift")
AXIS = Path("iOSApp/Sources/Services/ScopeAxis.swift")
ACQ_CFG = Path("iOSApp/Sources/Services/DaqAcquisitionConfig.swift")
TESTS = Path("tests/ios/main.swift")
AXIS_TESTS = Path("tests/ios/ScopeAxisTests.swift")
ACQ_CFG_TESTS = Path("tests/ios/DaqAcquisitionConfigTests.swift")


@pytest.mark.skipif(shutil.which("swiftc") is None, reason="swiftc not available")
def test_daq_link_state_machine_behavior():
    assert SM.exists(), f"{SM} not created"
    src = SM.read_text()
    for forbidden in ("import Network", "import UIKit", "import SwiftUI",
                      "import NetworkExtension"):
        assert forbidden not in src, (
            f"{SM} must stay platform-free ({forbidden}) so it is host-testable")

    assert ACQ_CFG.exists(), f"{ACQ_CFG} not created"
    acq_src = ACQ_CFG.read_text()
    for forbidden in ("import Network", "import UIKit", "import SwiftUI",
                      "import NetworkExtension"):
        assert forbidden not in acq_src, (
            f"{ACQ_CFG} must stay Foundation-only ({forbidden}) so it is host-testable")

    with tempfile.TemporaryDirectory() as td:
        binary = Path(td) / "smtests"
        build = subprocess.run(
            ["swiftc", "-swift-version", "5", str(SM), str(AXIS), str(ACQ_CFG),
             str(TESTS), str(AXIS_TESTS), str(ACQ_CFG_TESTS), "-o", str(binary)],
            capture_output=True, text=True)
        assert build.returncode == 0, f"swiftc failed:\n{build.stderr}"
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        assert run.returncode == 0, f"state machine tests failed:\n{run.stdout}"


# --- Task 8: DaqWifiStreamManager adopts the state machine ------------------

MGR = Path("iOSApp/Sources/Services/DaqWifiStreamManager.swift")


def test_manager_adopts_the_state_machine_and_drops_the_ad_hoc_flags():
    src = MGR.read_text()
    assert "DaqLinkStateMachine" in src, "manager does not use the state machine"
    assert "@Published private(set) var linkState" in src
    # The replaced flags must no longer be independently stored state.
    for gone in ("@Published var isConnected", "@Published var isStreaming",
                 "@Published var provisioningState", "private var reconnectAttempts"):
        assert gone not in src, f"stale ad-hoc state still present: {gone}"


def test_every_effect_has_exactly_one_performer():
    src = MGR.read_text()
    for effect in ("requestProvisioning", "joinHotspot", "openSocket", "sendStart",
                   "sendStop", "closeSocket", "removeHotspotConfig", "recycleDevice",
                   "resetBuffers", "scheduleRetry"):
        assert src.count(f"case .{effect}") == 1, (
            f"effect .{effect} must have exactly one performer, "
            f"got {src.count(f'case .{effect}')}")


def test_hotspot_configuration_removed_in_exactly_one_place():
    src = MGR.read_text()
    assert src.count("removeConfiguration(forSSID") == 1, (
        "hotspot removal must have a single owner — multiple call sites caused "
        "the removal-races-connect bug")


def test_watchdog_detects_a_connected_but_silent_link():
    src = MGR.read_text()
    assert "lastFrameAt" in src
    assert "dataStalled" in src, "nothing ever raises the stall event"


def test_recovery_calls_the_device_recycle_endpoint():
    src = MGR.read_text()
    assert "/api/daq/wifi_stream/recycle" in src


# --- Task 9: ScopeTab renders from the state projection ---------------------

SCOPE = Path("iOSApp/Sources/Views/ScopeTab.swift")


def test_scope_tab_uses_the_state_projection_not_deleted_apis():
    src = SCOPE.read_text()
    for gone in ("startFullStreamFlow", "requestStreamStop", "provisioningState"):
        assert gone not in src, f"ScopeTab still calls removed API: {gone}"
    assert "linkState" in src


def test_failure_offers_retry_rather_than_a_dead_end():
    src = SCOPE.read_text()
    assert "daqStream.retry()" in src, "no Retry affordance — user is stranded"


def test_recovering_state_is_visible_to_the_user():
    """Silent recovery looks like a hang; show what is happening."""
    src = SCOPE.read_text()
    assert "userFacingLabel" in src
