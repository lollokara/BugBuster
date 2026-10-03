"""Host-compile the pure-Swift RunWallMap tests (no simulator)."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

SRC = Path("iOSApp/Sources/Services/RunWallMap.swift")
TESTS = Path("tests/ios/RunWallMapTests.swift")
XCRUN = shutil.which("xcrun")


@pytest.mark.skipif(shutil.which("swiftc") is None and XCRUN is None, reason="no Swift toolchain available")
def test_run_wall_map_behavior():
    env = {k: v for k, v in os.environ.items() if k != "SDKROOT"}
    cmd = [XCRUN, "swiftc"] if XCRUN else ["swiftc"]
    if XCRUN:
        sdk = subprocess.run([XCRUN, "--sdk", "macosx", "--show-sdk-path"], capture_output=True, text=True, env=env)
        if sdk.returncode == 0 and sdk.stdout.strip():
            cmd += ["-sdk", sdk.stdout.strip()]
    with tempfile.TemporaryDirectory() as td:
        binary = Path(td) / "rwm"
        build = subprocess.run(cmd + ["-parse-as-library", "-swift-version", "5", str(SRC), str(TESTS), "-o", str(binary)],
                               capture_output=True, text=True, env=env)
        assert build.returncode == 0, build.stderr
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        assert run.returncode == 0, run.stdout
