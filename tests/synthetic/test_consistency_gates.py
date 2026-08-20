"""Guards for the three consistency gates and the firmware invariants they protect.

The gates themselves live in Firmware/tools/. Running them from pytest as well
means a developer who never looks at CI still sees a failure locally, which is
where doc and config drift is cheapest to fix.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "Firmware" / "tools"


def run_gate(script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOLS / script)],
        capture_output=True,
        text=True,
        cwd=REPO,
    )


@pytest.mark.parametrize(
    "script",
    ["check_proto_version.py", "check_doc_counts.py", "check_sdkconfig_effective.py"],
)
def test_consistency_gate_passes(script: str) -> None:
    result = run_gate(script)
    assert result.returncode == 0, (
        f"{script} failed:\n{result.stdout}\n{result.stderr}"
    )


def test_doc_counts_derives_every_metric_it_claims() -> None:
    """--print must emit every metric CLAIMS refers to, or a claim can never fail."""
    sys.path.insert(0, str(TOOLS))
    try:
        import check_doc_counts  # type: ignore[import-not-found]
    finally:
        sys.path.pop(0)

    derived = check_doc_counts.derive()
    referenced = {m for _, _, metrics in check_doc_counts.CLAIMS for m in metrics}
    missing = referenced - set(derived)
    assert not missing, f"CLAIMS reference metrics that derive() does not produce: {missing}"


# ---------------------------------------------------------------------------
# FEAT-8: no IO_OWNER_INTERNAL claim may be infinite.
#
# INTERNAL is the one kind io_owner_release_all_except() preserves, so an
# infinite INTERNAL lease survives every client reconnect and can only be
# cleared by a reboot. That is what wedged slot 14 (JIG-1).
# ---------------------------------------------------------------------------

IO_OWNER_CPP = REPO / "Firmware/ESP32/src/io_owner.cpp"
IO_OWNER_H = REPO / "Firmware/ESP32/src/io_owner.h"
SELFTEST_CPP = REPO / "Firmware/ESP32/src/diag/selftest.cpp"


def test_internal_lease_is_capped_in_acquire() -> None:
    src = IO_OWNER_CPP.read_text(encoding="utf-8")
    assert "IO_OWNER_INTERNAL_MAX_LEASE_MS" in src, (
        "io_owner_acquire must cap IO_OWNER_INTERNAL leases; an infinite one "
        "cannot be cleared by a client reconnect."
    )
    assert re.search(
        r"kind\s*==\s*IO_OWNER_INTERNAL\s*&&\s*\n?\s*\(lease_ms\s*==\s*0", src
    ), "the cap must treat lease_ms == 0 (infinite) as needing a ceiling"


def test_internal_lease_constants_defined() -> None:
    src = IO_OWNER_H.read_text(encoding="utf-8")
    for macro in ("IO_OWNER_INTERNAL_MAX_LEASE_MS", "IO_OWNER_SELFTEST_LEASE_MS"):
        assert re.search(rf"#define\s+{macro}\s+\d+u", src), f"{macro} not defined"


def test_selftest_claims_slot_with_bounded_lease() -> None:
    src = SELFTEST_CPP.read_text(encoding="utf-8")
    match = re.search(
        r"io_owner_acquire\(SELFTEST_CH_SLOT,\s*IO_OWNER_INTERNAL,\s*0xFF,\s*0,\s*([A-Za-z0-9_]+)",
        src,
    )
    assert match, "selftest INTERNAL claim not found"
    assert match.group(1) != "0", (
        "selftest must not claim its slot with an infinite lease - if the caller "
        "bails before selftest_restore_ch_slot() the slot stays locked until reboot"
    )


# ---------------------------------------------------------------------------
# FEAT-9: a refused MUX write must leave no trace.
# ---------------------------------------------------------------------------

ADGS_CPP = REPO / "Firmware/ESP32/src/hal/adgs2414d.cpp"
ADGS_H = REPO / "Firmware/ESP32/src/hal/adgs2414d.h"


def test_api_all_safe_rolls_back_on_refusal() -> None:
    src = ADGS_CPP.read_text(encoding="utf-8")
    body = src[src.index("bool adgs_set_api_all_safe") :]
    body = body[: body.index("\n}\n") + 3]

    assert "prev_api" in body, "adgs_set_api_all_safe must snapshot the API shadow"
    assert re.search(r"if\s*\(!ok\)\s*\{\s*\n\s*memcpy\(s_api_main_state,\s*prev_api", body), (
        "a refused write must restore the previous API shadow. Without this an "
        "API device with no physical device behind it keeps the rejected value "
        "and reads back as though the write succeeded."
    )


def test_set_all_safe_declaration_matches_definition_width() -> None:
    """The header said ADGS_NUM_DEVICES while the definition said MAIN_DEVICES.

    Array parameters decay to pointers so the compiler never objected, and a
    caller sized its buffer from the header and overflowed the stack (MUX-2).
    """
    header = ADGS_H.read_text(encoding="utf-8")
    assert re.search(
        r"bool\s+adgs_set_all_safe\(const uint8_t states\[ADGS_MAIN_DEVICES\]\)", header
    ), "adgs_set_all_safe declaration must be ADGS_MAIN_DEVICES wide"


def test_every_set_api_all_safe_caller_checks_the_result() -> None:
    """A refusal that nobody checks is answered as success (MUX-1 / MUX-3)."""
    offenders: list[str] = []
    for path in (REPO / "Firmware/ESP32/src").rglob("*.cpp"):
        if path.name == "adgs2414d.cpp":
            continue
        for num, line in enumerate(path.read_text(encoding="utf-8", errors="ignore").splitlines(), 1):
            if "adgs_set_api_all_safe(" not in line and "adgs_set_all_safe(" not in line:
                continue
            stripped = line.strip()
            if stripped.startswith(("//", "*", "/*")):
                continue
            checked = stripped.startswith(("if (!", "if (", "bool ", "return ")) or "=" in stripped
            if not checked:
                offenders.append(f"{path.relative_to(REPO)}:{num}: {stripped}")
    assert not offenders, "these call sites ignore the refusal result:\n" + "\n".join(offenders)
