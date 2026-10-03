"""Compile and run the pure-Swift HubClient tests on the host toolchain (URLProtocol stub, no network)."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from tests.lib.srcread import read_source

SRCS = [Path("iOSApp/Sources/Services/HubMerge.swift"), Path("iOSApp/Sources/Services/HubClient.swift")]
TESTS = Path("tests/ios/HubClientTests.swift")
XCRUN = shutil.which("xcrun")


@pytest.mark.skipif(shutil.which("swiftc") is None and XCRUN is None, reason="no Swift toolchain available")
def test_hub_client_behavior():
    for src in SRCS:
        assert src.exists(), f"{src} not created"
        for forbidden in ("import UIKit", "import SwiftUI", "import Network", "import Combine"):
            assert forbidden not in read_source(src), f"{src} must stay Foundation-only ({forbidden})"
    env = {k: v for k, v in os.environ.items() if k != "SDKROOT"}
    cmd = [XCRUN, "swiftc"] if XCRUN else ["swiftc"]
    if XCRUN:
        sdk = subprocess.run([XCRUN, "--sdk", "macosx", "--show-sdk-path"], capture_output=True, text=True, env=env)
        if sdk.returncode == 0 and sdk.stdout.strip():
            cmd += ["-sdk", sdk.stdout.strip()]
    with tempfile.TemporaryDirectory() as td:
        binary = Path(td) / "hubclienttests"
        build = subprocess.run(cmd + ["-parse-as-library", "-swift-version", "5", *map(str, SRCS), str(TESTS), "-o", str(binary)],
                               capture_output=True, text=True, env=env)
        assert build.returncode == 0, f"swiftc failed:\n{build.stderr}"
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        assert run.returncode == 0, f"hub client tests failed:\n{run.stdout}"
