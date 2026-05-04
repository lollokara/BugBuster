#!/usr/bin/env python3
"""
run_tests.py — BugBuster hardware-in-the-loop test runner.

A standalone CLI script that runs pytest with appropriate arguments
and prints a rich summary table of pass/fail/skip counts per test category.

Usage:
    python tests/run_tests.py --usb /dev/ttyACM0
    python tests/run_tests.py --http 192.168.4.1
    python tests/run_tests.py --usb /dev/ttyACM0 --http 192.168.4.1 --hat
    python tests/run_tests.py --usb /dev/ttyACM0 --transport usb --html-report
    python tests/run_tests.py --usb /dev/ttyACM0 --skip-destructive

Exit codes:
    0 — all tests passed (or skipped)
    1 — one or more tests failed
    2 — internal error
"""

import argparse
import subprocess
import sys
import re
from pathlib import Path

# ---------------------------------------------------------------------------
# Try to import rich for pretty output; fall back to plain text
# ---------------------------------------------------------------------------

try:
    from rich.console import Console
    from rich.table import Table
    from rich import box
    _RICH = True
except ImportError:
    _RICH = False

TESTS_DIR = Path(__file__).parent.resolve()
ROOT_DIR  = TESTS_DIR.parent


# ---------------------------------------------------------------------------
# Category definitions
# ---------------------------------------------------------------------------

CATEGORIES = [
    ("core",        "device/test_01_core.py",           "Core device / firmware info"),
    ("channels",    "device/test_02_channels.py",       "Channel configuration (AD74416H)"),
    ("gpio",        "device/test_03_gpio.py",            "GPIO pins (A–F)"),
    ("mux",         "device/test_04_mux.py",             "MUX switch matrix"),
    ("power",       "device/test_05_power.py",           "Power management (IDAC, PCA9535)"),
    ("usbpd",       "device/test_06_usbpd.py",           "USB Power Delivery (HUSB238)"),
    ("wavegen",     "device/test_07_wavegen.py",         "Waveform generator"),
    ("wifi",        "device/test_08_wifi.py",            "WiFi management"),
    ("selftest",    "device/test_09_selftest.py",        "Self-test & calibration"),
    ("streaming",   "device/test_10_streaming.py",      "ADC/scope streaming (USB only)"),
    ("hat",         "device/test_11_hat.py",             "HAT expansion board"),
    ("faults",      "device/test_12_faults.py",          "Fault management"),
    ("dio",         "device/test_13_dio.py",              "ESP32 GPIO digital I/O"),
    ("uart",        "device/test_14_uart.py",             "UART bridge configuration"),
    ("swd",         "device/test_15_swd.py",              "SWD debug port — requires --swd-target"),
    ("la_bulk",     "device/test_la_usb_bulk.py",        "LA USB bulk protocol (synthetic)"),
    ("la_1mhz",    "device/test_la_usb_1mhz.py",        "LA 1 MHz streaming — requires HAT"),
    ("la_synth",    "synthetic/test_la_usb_synthetic.py","LA synthetic / regression tests"),
    ("http",        "http_api/test_http_endpoints.py",   "HTTP REST endpoints"),
]


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="run_tests.py",
        description="BugBuster test runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument(
        "--usb", metavar="PORT",
        help="Serial port for USB device (e.g. /dev/ttyACM0 or COM3)",
    )
    p.add_argument(
        "--http", metavar="IP",
        help="IP address for HTTP device (e.g. 192.168.4.1)",
    )
    p.add_argument(
        "--hat", action="store_true",
        help="Enable HAT expansion board tests",
    )
    p.add_argument(
        "--skip-destructive", action="store_true",
        help="Skip tests that modify device state irreversibly",
    )
    p.add_argument(
        "--transport", choices=["usb", "http", "both"], default="both",
        help="Which transport to parametrize (default: both)",
    )
    p.add_argument(
        "--category", metavar="NAME",
        help=f"Run only a specific category: {', '.join(k for k,_,_ in CATEGORIES)}",
    )
    p.add_argument(
        "--html-report", action="store_true",
        help="Generate HTML report at tests/report.html",
    )
    p.add_argument(
        "--no-color", action="store_true",
        help="Disable colored output",
    )
    p.add_argument(
        "-x", "--exitfirst", action="store_true",
        help="Stop on first failure",
    )
    p.add_argument(
        "--timeout", type=int, default=30,
        help="Per-test timeout in seconds (default: 30)",
    )
    p.add_argument(
        "--pytest-args", metavar="ARGS", default="",
        help="Additional arguments passed directly to pytest",
    )
    return p


# ---------------------------------------------------------------------------
# Build pytest command
# ---------------------------------------------------------------------------

def build_pytest_cmd(args) -> list[str]:
    cmd = [sys.executable, "-m", "pytest"]

    # Device arguments
    if args.usb:
        cmd += [f"--device-usb={args.usb}"]
    if args.http:
        cmd += [f"--device-http={args.http}"]
    if args.hat:
        cmd.append("--hat")
    if args.skip_destructive:
        cmd.append("--skip-destructive")

    # Transport filtering
    if args.transport == "usb" and args.usb:
        cmd += ["-k", "not http_only"]
    elif args.transport == "http" and args.http:
        cmd += ["-k", "not usb_only"]

    # Timeout
    cmd += [f"--timeout={args.timeout}"]

    # HTML report
    if args.html_report:
        report_path = TESTS_DIR / "report.html"
        cmd += [f"--html={report_path}", "--self-contained-html"]

    # Color
    if args.no_color:
        cmd.append("--no-header")
        cmd.extend(["-p", "no:terminal"])

    # Stop on first failure
    if args.exitfirst:
        cmd.append("-x")

    # Extra pytest args
    if args.pytest_args:
        cmd.extend(args.pytest_args.split())

    # Category or all tests
    if args.category:
        matches = [(k, f, d) for k, f, d in CATEGORIES if k == args.category]
        if not matches:
            valid = ", ".join(k for k, _, _ in CATEGORIES)
            print(f"Unknown category '{args.category}'. Valid: {valid}", file=sys.stderr)
            sys.exit(2)
        cmd.append(str(TESTS_DIR / matches[0][1]))
    else:
        cmd.append(str(TESTS_DIR))

    # JSON results for parsing
    cmd += ["--json-report", f"--json-report-file={TESTS_DIR / '.test_results.json'}"]

    return cmd


# ---------------------------------------------------------------------------
# Parse results from pytest output
# ---------------------------------------------------------------------------

def parse_pytest_output(output: str) -> dict[str, dict]:
    """Parse pytest verbose output to extract per-file pass/fail/skip counts."""
    results: dict[str, dict] = {}

    # Initialize all categories
    for key, file_path, desc in CATEGORIES:
        results[key] = {"key": key, "desc": desc, "passed": 0, "failed": 0, "skipped": 0, "errors": 0}

    # Parse test result lines: "tests/device/test_01_core.py::test_ping PASSED"
    pattern = re.compile(r"(tests[/\\].+?\.py)::(\S+)\s+(PASSED|FAILED|SKIPPED|ERROR)")
    for match in pattern.finditer(output):
        file_path_str = match.group(1).replace("\\", "/")
        status = match.group(3)

        # Find matching category
        for key, cat_file, desc in CATEGORIES:
            if cat_file.replace("\\", "/") in file_path_str:
                if status == "PASSED":
                    results[key]["passed"] += 1
                elif status == "FAILED":
                    results[key]["failed"] += 1
                elif status == "SKIPPED":
                    results[key]["skipped"] += 1
                elif status == "ERROR":
                    results[key]["errors"] += 1
                break

    return results


# ---------------------------------------------------------------------------
# Print summary table
# ---------------------------------------------------------------------------

def print_summary(results: dict[str, dict], returncode: int):
    total_passed = sum(v["passed"] for v in results.values())
    total_failed = sum(v["failed"] for v in results.values())
    total_skipped = sum(v["skipped"] for v in results.values())
    total_errors = sum(v["errors"] for v in results.values())

    if _RICH:
        console = Console()
        table = Table(
            title="BugBuster Test Summary",
            box=box.ROUNDED,
            show_header=True,
            header_style="bold cyan",
        )
        table.add_column("Category", style="white", no_wrap=True)
        table.add_column("Description", style="dim")
        table.add_column("Passed", justify="right", style="green")
        table.add_column("Failed", justify="right", style="red")
        table.add_column("Skipped", justify="right", style="yellow")

        for key, data in results.items():
            has_failures = data["failed"] > 0 or data["errors"] > 0
            passed_str = str(data["passed"]) if data["passed"] > 0 else "-"
            failed_str = str(data["failed"] + data["errors"]) if data["failed"] + data["errors"] > 0 else "-"
            skipped_str = str(data["skipped"]) if data["skipped"] > 0 else "-"
            row_style = "bold red" if has_failures else ""
            table.add_row(
                data["key"], data["desc"],
                passed_str, failed_str, skipped_str,
                style=row_style,
            )

        table.add_section()
        total_str = f"TOTAL ({total_passed + total_failed + total_skipped})"
        table.add_row(
            total_str, "",
            str(total_passed), str(total_failed), str(total_skipped),
            style="bold",
        )

        console.print()
        console.print(table)
        console.print()

        if returncode == 0:
            console.print("[bold green]All tests passed![/bold green]")
        else:
            console.print(f"[bold red]{total_failed} test(s) failed.[/bold red]")
        console.print()

    else:
        # Plain text fallback
        print("\n" + "=" * 70)
        print("BugBuster Test Summary")
        print("=" * 70)
        print(f"{'Category':<15} {'Description':<40} {'P':>5} {'F':>5} {'S':>5}")
        print("-" * 70)

        for key, data in results.items():
            print(
                f"{data['key']:<15} {data['desc']:<40} "
                f"{data['passed']:>5} {data['failed']:>5} {data['skipped']:>5}"
            )

        print("-" * 70)
        print(
            f"{'TOTAL':<15} {'':<40} "
            f"{total_passed:>5} {total_failed:>5} {total_skipped:>5}"
        )
        print("=" * 70)

        if returncode == 0:
            print("All tests passed!")
        else:
            print(f"{total_failed} test(s) failed.")
        print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if not args.usb and not args.http:
        print(
            "Warning: no device specified. Most tests will be skipped.\n"
            "Pass --usb <port> and/or --http <ip> to run hardware tests.",
            file=sys.stderr,
        )

    # Remove JSON report plugin requirement if not installed — it's optional
    cmd = build_pytest_cmd(args)

    # Remove --json-report args if json-report not installed
    filtered_cmd = []
    skip_next = False
    for arg in cmd:
        if skip_next:
            skip_next = False
            continue
        if "--json-report" in arg:
            # Check if plugin is available
            check = subprocess.run(
                [sys.executable, "-c", "import pytest_json_report"],
                capture_output=True,
            )
            if check.returncode != 0:
                continue  # skip JSON report args
        filtered_cmd.append(arg)

    print("Running:", " ".join(str(a) for a in filtered_cmd))
    print()

    # Run pytest
    result = subprocess.run(filtered_cmd, cwd=ROOT_DIR, capture_output=False)
    returncode = result.returncode

    # Try to parse output for summary (run again with -v capture)
    summary_cmd = filtered_cmd + ["--no-header", "-q"]
    summary_result = subprocess.run(
        summary_cmd, cwd=ROOT_DIR,
        capture_output=True, text=True,
    )

    results = parse_pytest_output(summary_result.stdout + summary_result.stderr)
    print_summary(results, returncode)

    return min(returncode, 1)  # normalize to 0/1


if __name__ == "__main__":
    sys.exit(main())
