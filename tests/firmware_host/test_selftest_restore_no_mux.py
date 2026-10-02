"""IO-24 (part): after a supply-monitor measurement the self-test restored
logical channel C with tasks_apply_channel_function() even when it was
HIGH_IMP. read_channel_d() already leaves physical D in HIGH_IMP over direct
SPI, so that call only re-applied channel C's MUX route - and wiped a raw MUX
write (device 3 S3) that landed during the measurement window (seen on the
board: wrote [0,0,0,0xFF], read back [0,0,0,0xFB]).
"""
import re

import pytest

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import REPO_ROOT

SELFTEST = REPO_ROOT / "Firmware/ESP32/src/diag/selftest.cpp"


@pytest.mark.xfail(strict=True, reason="IO-24")
def test_high_imp_restore_does_not_reapply_the_route():
    body = extract_function(SELFTEST, r"static float measure_via_u23_locked\(")
    body = re.sub(r"//[^\n]*", "", body)
    m = re.search(r"if\s*\(([^)]*)\)\s*\{?\s*tasks_apply_channel_function\(\s*SELFTEST_LOGICAL_CH\s*,\s*prev_func\s*\)", body)
    assert m, "restore must be conditional"
    assert "prev_func != CH_FUNC_HIGH_IMP" in m.group(1)
