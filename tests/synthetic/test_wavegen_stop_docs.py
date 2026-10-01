"""AN-05 (decision B): stop_waveform() returns the channel to HIGH_IMP. Docs
that promise the DAC "holds the last value" are wrong and must not come back."""

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FILES = [
    "python/bugbuster/client.py",
    "python/bugbuster_mcp/tools/waveform.py",
    "python/examples/04_waveform_and_mux.py",
]
BAD = re.compile(r"(holds?|stays? at|retains?) (the )?last (output )?value", re.I)


@pytest.mark.xfail(strict=True, reason="AN-05")
@pytest.mark.parametrize("rel", FILES)
def test_stop_waveform_docs_do_not_promise_hold(rel):
    hits = [ln for ln in (ROOT / rel).read_text(encoding="utf-8").splitlines() if BAD.search(ln)]
    assert not hits, hits
