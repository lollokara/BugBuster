#!/usr/bin/env python3
"""Coverage ratchet: coverage may rise freely, but never fall.

CI runs this after pytest-cov writes ``coverage.xml``. It compares the run
against ``tests/coverage-baseline.json`` and fails when either the line or the
branch rate drops by more than ``TOLERANCE_PP`` percentage points.

A plain ``--cov-fail-under`` cannot do this: a fixed threshold either sits so
low it never catches a regression, or has to be edited by hand on every
improvement (and in practice gets edited *downwards* to make a red build go
green). Storing the baseline in the repo makes lowering it an explicit,
reviewable diff.

Usage:
    python tests/tools/coverage_ratchet.py coverage.xml
    python tests/tools/coverage_ratchet.py coverage.xml --update
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE_PATH = REPO_ROOT / "tests" / "coverage-baseline.json"

# Small runner-to-runner variation (import ordering, platform-gated branches)
# should not fail a build. Anything larger is a real regression.
TOLERANCE_PP = 0.2

# Suggest tightening the baseline once coverage has climbed this far past it,
# otherwise the ratchet slowly goes slack.
RATCHET_SUGGEST_PP = 0.5


def _parse_coverage(xml_path: Path) -> dict:
    try:
        root = ET.parse(xml_path).getroot()
    except FileNotFoundError:
        sys.exit(f"coverage report not found: {xml_path}")
    except ET.ParseError as exc:
        sys.exit(f"could not parse {xml_path}: {exc}")

    def rate(attr: str) -> float:
        try:
            return float(root.get(attr, 0.0)) * 100.0
        except (TypeError, ValueError):
            return 0.0

    packages = {}
    for cls in root.iter("class"):
        filename = cls.get("filename")
        if filename:
            packages[filename] = float(cls.get("line-rate", 0.0)) * 100.0

    return {
        "line_rate": round(rate("line-rate"), 2),
        "branch_rate": round(rate("branch-rate"), 2),
        "files": packages,
    }


def _load_baseline() -> dict:
    if not BASELINE_PATH.exists():
        sys.exit(
            f"no baseline at {BASELINE_PATH}\n"
            "Create one with: python tests/tools/coverage_ratchet.py coverage.xml --update"
        )
    return json.loads(BASELINE_PATH.read_text(encoding="utf-8"))


def _write_baseline(current: dict) -> None:
    BASELINE_PATH.write_text(
        json.dumps(
            {
                "line_rate": current["line_rate"],
                "branch_rate": current["branch_rate"],
                "updated": date.today().isoformat(),
                "_comment": (
                    "Coverage floor enforced by tests/tools/coverage_ratchet.py. "
                    "Raise it when coverage improves; lowering it requires an "
                    "explicit, reviewed diff."
                ),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def _summary(lines: list[str]) -> None:
    print("\n".join(lines))
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("coverage_xml", type=Path, help="path to coverage.xml")
    ap.add_argument("--update", action="store_true",
                    help="overwrite the baseline with the current run")
    ap.add_argument("--worst", type=int, default=10,
                    help="how many least-covered files to list")
    args = ap.parse_args()

    current = _parse_coverage(args.coverage_xml)

    if args.update:
        _write_baseline(current)
        print(f"baseline updated: line {current['line_rate']:.2f}%  "
              f"branch {current['branch_rate']:.2f}%")
        return 0

    baseline = _load_baseline()
    line_delta = current["line_rate"] - baseline["line_rate"]
    branch_delta = current["branch_rate"] - baseline["branch_rate"]

    rows = [
        "## Coverage",
        "",
        "| Metric | Baseline | Current | Delta |",
        "|---|---:|---:|---:|",
        f"| Line | {baseline['line_rate']:.2f}% | {current['line_rate']:.2f}% "
        f"| {line_delta:+.2f} pp |",
        f"| Branch | {baseline['branch_rate']:.2f}% | {current['branch_rate']:.2f}% "
        f"| {branch_delta:+.2f} pp |",
    ]

    uncovered = sorted(current["files"].items(), key=lambda kv: kv[1])[: args.worst]
    if uncovered:
        rows += ["", f"<details><summary>{args.worst} least-covered files</summary>", ""]
        rows += ["| File | Line coverage |", "|---|---:|"]
        rows += [f"| `{name}` | {pct:.1f}% |" for name, pct in uncovered]
        rows += ["", "</details>"]

    failed = line_delta < -TOLERANCE_PP or branch_delta < -TOLERANCE_PP
    if failed:
        rows += [
            "",
            f"**FAIL** — coverage dropped more than {TOLERANCE_PP} pp below the baseline.",
            "Add tests for the code you changed. If the drop is genuinely",
            "intended (e.g. you deleted well-covered code), lower the baseline",
            "in `tests/coverage-baseline.json` in the same commit so the",
            "decision is reviewable.",
        ]
    elif min(line_delta, branch_delta) > RATCHET_SUGGEST_PP:
        rows += [
            "",
            f"Coverage is more than {RATCHET_SUGGEST_PP} pp above the baseline. "
            "Tighten it with:",
            "",
            "```",
            "python tests/tools/coverage_ratchet.py coverage.xml --update",
            "```",
        ]

    _summary(rows)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
