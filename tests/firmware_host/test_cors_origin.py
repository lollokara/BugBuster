"""WEB-25: CORS echoed any Origin that merely starts with http://localhost
(e.g. http://localhost.evil.com) back as allowed. Tests the real header."""

import pytest

from tests.firmware_host.fwhost import compile_and_run
from tests.lib.srcread import REPO_ROOT

CASES = {
    "http://localhost": 1,
    "http://localhost:5173": 1,
    "http://127.0.0.1:8080": 1,
    "http://localhost.evil.com": 0,
    "http://127.0.0.1.evil.com": 0,
    "http://localhost:5173.evil.com": 0,
    "http://localhost@evil.com": 0,
    "https://localhost": 0,
    "http://evil.com": 0,
    "": 0,
}


def _run() -> dict[str, int]:
    checks = "\n".join(f'    printf("%d\\n", cors_origin_allowed("{o}"));' for o in CASES)
    out = compile_and_run(
        '#include <stdio.h>\n#include <stdbool.h>\n#include "cors.h"\n'
        f"int main(void) {{\n{checks}\n    return 0;\n}}\n",
        include_dirs=[REPO_ROOT / "Firmware/ESP32/src/web"],
    )
    return dict(zip(CASES, map(int, out.split()), strict=True))


def test_loopback_dev_origins_allowed():
    got = _run()
    assert all(got[o] for o, want in CASES.items() if want)


@pytest.mark.xfail(strict=True, reason="WEB-25")
def test_lookalike_origins_rejected():
    got = _run()
    bad = [o for o, want in CASES.items() if not want and got[o]]
    assert bad == [], f"accepted: {bad}"
