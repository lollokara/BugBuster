"""C6-23: the C6 task watchdog must reboot on a hang. The REQUIRED table in
Firmware/tools/check_sdkconfig_effective.py pins it in the TRACKED
sdkconfig.defaults (a value only in the gitignored generated sdkconfig is not
shipped). A: the C6 defaults had no TASK_WDT keys at all."""

import subprocess
import sys

from tests.lib.srcread import REPO_ROOT


def test_sdkconfig_gate_passes_including_required_safety_keys():
    r = subprocess.run([sys.executable, str(REPO_ROOT / "Firmware/tools/check_sdkconfig_effective.py")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
