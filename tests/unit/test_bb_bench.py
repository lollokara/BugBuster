"""
tests/unit/test_bb_bench.py — unit tests for tests/tools/bb_bench.py.

Covers only the pure logic that needs no hardware: the stats computation
(min/median/p95), the compare/delta formatting, the mem-surface serial
parsing (against a FakeSerial double), and graceful degradation when a
surface's dependency (network, serial port) is unavailable. Nothing here
touches a real device, USB endpoint, or serial port.
"""

from __future__ import annotations

import importlib

import pytest

bb_bench = importlib.import_module("tests.tools.bb_bench")


# ---------------------------------------------------------------------------
# percentile / compute_stats
# ---------------------------------------------------------------------------

def test_percentile_empty_is_none():
    assert bb_bench.percentile([], 95) is None


def test_percentile_single_value():
    assert bb_bench.percentile([42.0], 95) == 42.0


def test_percentile_known_dataset():
    # 1..10, p50 (median-equivalent) and p95 have known linear-interp values.
    data = [float(x) for x in range(1, 11)]
    assert bb_bench.percentile(data, 50) == pytest.approx(5.5)
    # rank = (10-1)*0.95 = 8.55 -> interpolate between data[8]=9 and data[9]=10
    assert bb_bench.percentile(data, 95) == pytest.approx(9.55)


def test_compute_stats_empty_samples_has_zero_count_and_unit():
    stats = bb_bench.compute_stats([], "ms")
    assert stats == {"count": 0, "unit": "ms", "min": None, "median": None,
                      "p95": None, "max": None}


def test_compute_stats_reports_min_median_p95_max_with_unit_and_count():
    samples = [10.0, 20.0, 30.0, 40.0, 50.0]
    stats = bb_bench.compute_stats(samples, "ms")
    assert stats["unit"] == "ms"
    assert stats["count"] == 5
    assert stats["min"] == 10.0
    assert stats["max"] == 50.0
    assert stats["median"] == 30.0
    assert stats["p95"] == pytest.approx(48.0)  # rank=(5-1)*.95=3.8 -> 40+.8*(50-40)


# ---------------------------------------------------------------------------
# compare / delta formatting
# ---------------------------------------------------------------------------

def test_format_delta_row_flags_regression_on_lower_better_metric():
    # "error" hints lower-is-better; after > before is a regression.
    row = bb_bench.format_delta_row("http.endpoints.status.error_rate", 0.0, 0.5)
    assert "REGRESSION" in row


def test_format_delta_row_flags_ok_when_lower_better_metric_improves():
    row = bb_bench.format_delta_row("link.round_trip.median", 100.0, 50.0)
    assert "REGRESSION" not in row
    assert "ok" in row


def test_format_delta_row_flags_regression_on_higher_better_metric_drop():
    row = bb_bench.format_delta_row("daq_usb.mb_per_s", 10.0, 5.0)
    assert "REGRESSION" in row


def test_format_delta_row_neutral_metric_gets_no_flag():
    row = bb_bench.format_delta_row("daq_usb.device.relay_state", 4.0, 5.0)
    assert "REGRESSION" not in row
    assert "  ok" not in row


def test_format_delta_row_small_change_is_not_flagged():
    # < 1% change should not be flagged even for a hinted metric.
    row = bb_bench.format_delta_row("link.round_trip.median", 100.0, 100.5)
    assert "REGRESSION" not in row


def test_compare_reports_detects_regression_and_returns_1(capsys):
    before = {"surfaces": {"daq_usb": {"mb_per_s": 10.0}}}
    after = {"surfaces": {"daq_usb": {"mb_per_s": 5.0}}}
    rc = bb_bench.compare_reports(before, after)
    out = capsys.readouterr().out
    assert rc == 1
    assert "FAIL" in out


def test_compare_reports_no_regression_returns_0(capsys):
    before = {"surfaces": {"daq_usb": {"mb_per_s": 10.0}}}
    after = {"surfaces": {"daq_usb": {"mb_per_s": 12.0}}}
    rc = bb_bench.compare_reports(before, after)
    out = capsys.readouterr().out
    assert rc == 0
    assert "OK" in out


def test_compare_reports_lists_metrics_only_in_one_side(capsys):
    before = {"surfaces": {"mem": {"heap": {"internal_free_kb": 30}}}}
    after = {"surfaces": {"mem": {"heap": {"internal_free_kb": 30},
                                  "stack_hwm": {"adcPoll": {"declared_bytes": 2560}}}}}
    bb_bench.compare_reports(before, after)
    out = capsys.readouterr().out
    assert "only in AFTER" in out
    assert "declared_bytes" in out


# ---------------------------------------------------------------------------
# http surface: injectable get_fn, graceful degradation on errors
# ---------------------------------------------------------------------------

def test_bench_http_reports_cold_sample_separately():
    calls = []

    def fake_get(path, params, need_admin):
        calls.append(path)
        return 5.0 if len(calls) == 1 else 1.0

    endpoints = [("status", "/status", None, False)]
    result = bb_bench.bench_http(fake_get, samples=3, endpoints=endpoints)
    assert result["cold_ms"] == 5.0
    assert result["cold_endpoint"] == "status"
    # cold call + 3 stats samples = 4 total invocations
    assert len(calls) == 4
    assert result["endpoints"]["status"]["count"] == 3


def test_bench_http_degrades_gracefully_on_every_request_failing():
    def failing_get(path, params, need_admin):
        raise ConnectionError("device unreachable")

    endpoints = [("status", "/status", None, False)]
    result = bb_bench.bench_http(failing_get, samples=4, endpoints=endpoints)
    assert result["available"] is True   # the surface ran; the request failed
    entry = result["endpoints"]["status"]
    assert entry["count"] == 0
    assert entry["errors"] == 4
    assert entry["error_rate"] == 1.0
    assert "last_error" in entry


def test_run_http_reports_unavailable_when_requests_missing(monkeypatch):
    import builtins
    real_import = builtins.__import__

    def blocking_import(name, *args, **kwargs):
        if name == "requests":
            raise ImportError("no requests")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", blocking_import)
    result = bb_bench.run_http("10.0.0.1", 80, None, 1, 1.0, 0.0)
    assert result["available"] is False
    assert "error" in result


# ---------------------------------------------------------------------------
# mem surface: parsing against a real captured device transcript
# ---------------------------------------------------------------------------

HEAP_TRANSCRIPT = (
    "heap\r\n\r\n--- Heap Status ---\r\n"
    "  Internal free:         37 KB\r\n"
    "  Internal min-ever:     27 KB  (all-time low since boot)\r\n"
    "  Internal largest:      19 KB  (biggest contiguous block)\r\n"
    "  PSRAM free:          6991 KB\r\n"
    "  PSRAM largest:       6912 KB\r\n\r\n"
    "[BugBuster]> "
)

STACK_TRANSCRIPT = (
    "stack_hwm\r\n\r\n--- Stack High-Water Marks (bytes never used) ---\r\n"
    "  task        declared   unused     peak-used\r\n"
    "  adcPoll       2560     1276     1284\r\n"
    "  faultMon      2560     1232     1328\r\n"
    "  cmdProc       2048     1220      828\r\n"
    "I (320867) tasks: Stack HWM adcPoll: 1276 words free\r\n"
    "\r\n[BugBuster]> "
)


def test_parse_heap_extracts_all_fields():
    parsed = bb_bench.parse_heap(HEAP_TRANSCRIPT)
    assert parsed["internal_free_kb"] == 37
    assert parsed["internal_min_ever_kb"] == 27
    assert parsed["internal_largest_kb"] == 19
    assert parsed["psram_free_kb"] == 6991
    assert parsed["psram_largest_kb"] == 6912
    assert parsed["unit"] == "KB"


def test_parse_stack_hwm_skips_header_and_log_lines():
    parsed = bb_bench.parse_stack_hwm(STACK_TRANSCRIPT)
    assert set(parsed) == {"adcPoll", "faultMon", "cmdProc"}
    assert parsed["adcPoll"] == {
        "declared_bytes": 2560, "unused_bytes": 1276,
        "peak_used_bytes": 1284, "unit": "bytes", "flag": "",
    }


def test_parse_stack_hwm_marks_missing_handle():
    text = "  bbpCli    handle not found\r\n"
    parsed = bb_bench.parse_stack_hwm(text)
    assert parsed["bbpCli"] == {"available": False}


def test_bench_mem_end_to_end_with_fake_serial():
    p4_console = importlib.import_module("tests.lib.p4_console")
    responses = [HEAP_TRANSCRIPT, STACK_TRANSCRIPT]
    fake = p4_console.FakeSerial(responses=responses)
    cli = bb_bench.SerialCli("fake-port", _serial=fake)
    result = bb_bench.bench_mem(cli)
    assert result["available"] is True
    assert result["heap"]["internal_largest_kb"] == 19
    assert "adcPoll" in result["stack_hwm"]


def test_run_mem_unavailable_without_serial_port():
    result = bb_bench.run_mem(None, 115200)
    assert result["available"] is False
    assert "serial-port" in result["error"]


def test_run_mem_unavailable_when_pyserial_missing(monkeypatch):
    import builtins
    real_import = builtins.__import__

    def blocking_import(name, *args, **kwargs):
        if name == "serial":
            raise ImportError("no pyserial")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", blocking_import)
    result = bb_bench.run_mem("/dev/does-not-matter", 115200)
    assert result["available"] is False
    assert "pyserial" in result["error"]


def test_run_mem_unavailable_when_port_open_fails():
    # A real port path that cannot possibly exist -- exercises the "device
    # not attached" branch without needing pyserial mocked out entirely.
    result = bb_bench.run_mem("/dev/cu.this-port-does-not-exist-xyz", 115200)
    assert result["available"] is False
    assert "error" in result
