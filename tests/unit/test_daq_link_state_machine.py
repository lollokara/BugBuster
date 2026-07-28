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
TESTS = Path("tests/ios/main.swift")


@pytest.mark.skipif(shutil.which("swiftc") is None, reason="swiftc not available")
def test_daq_link_state_machine_behavior():
    assert SM.exists(), f"{SM} not created"
    src = SM.read_text()
    for forbidden in ("import Network", "import UIKit", "import SwiftUI",
                      "import NetworkExtension"):
        assert forbidden not in src, (
            f"{SM} must stay platform-free ({forbidden}) so it is host-testable")

    with tempfile.TemporaryDirectory() as td:
        binary = Path(td) / "smtests"
        build = subprocess.run(
            ["swiftc", "-swift-version", "5", str(SM), str(TESTS), "-o", str(binary)],
            capture_output=True, text=True)
        assert build.returncode == 0, f"swiftc failed:\n{build.stderr}"
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        assert run.returncode == 0, f"state machine tests failed:\n{run.stdout}"
