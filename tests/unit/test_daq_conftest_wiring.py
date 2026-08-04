"""Verify the DAQ tier's pytest wiring exists and is inert without --daq.

This is a wiring test, not a hardware test: it runs pytest in-process against a
tiny generated test file and asserts on collection/skip behaviour.
"""
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

# Generated test files must live inside the `tests/` tree, not in the OS tmp
# dir. pytest discovers rootdir/conftest.py/pytest.ini by walking UP from the
# target file's own ancestors -- never from the invoking process's cwd -- so a
# target outside `tests/` would never see tests/pytest.ini or
# tests/conftest.py, and every option/marker under test here would look
# "unregistered" no matter how correctly the real wiring is implemented.
_SCRATCH_ROOT = Path(__file__).resolve().parent / "_daq_wiring_scratch"


def _run_pytest(tmp_path, args, body):
    scratch = _SCRATCH_ROOT / tmp_path.name
    scratch.mkdir(parents=True, exist_ok=True)
    try:
        f = scratch / "test_generated_daq.py"
        f.write_text(textwrap.dedent(body))
        return subprocess.run(
            [sys.executable, "-m", "pytest", str(f), "-p", "no:cacheprovider", "-q", *args],
            capture_output=True, text=True, cwd=".",
        )
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def test_requires_daq_marker_is_registered(tmp_path):
    r = _run_pytest(tmp_path, ["--strict-markers", "--collect-only"], """
        import pytest

        @pytest.mark.requires_daq
        def test_noop():
            pass
    """)
    assert "Unknown pytest.mark.requires_daq" not in r.stdout + r.stderr
    assert r.returncode == 0


def test_daq_wifi_marker_is_registered(tmp_path):
    r = _run_pytest(tmp_path, ["--strict-markers", "--collect-only"], """
        import pytest

        @pytest.mark.daq_wifi
        def test_noop():
            pass
    """)
    assert r.returncode == 0


def test_daq_tests_skip_without_the_flag(tmp_path):
    r = _run_pytest(tmp_path, [], """
        import pytest

        @pytest.mark.requires_daq
        def test_needs_hardware():
            assert False, "must never run without --daq"
    """)
    assert r.returncode == 0
    assert "1 skipped" in r.stdout


def test_daq_load_ohms_option_is_accepted(tmp_path):
    r = _run_pytest(tmp_path, ["--daq-load-ohms", "470", "--collect-only"], """
        def test_noop():
            pass
    """)
    assert r.returncode == 0


def test_requires_daq_bbp_marker_is_registered(tmp_path):
    r = _run_pytest(tmp_path, ["--strict-markers", "--collect-only"], """
        import pytest

        @pytest.mark.requires_daq_bbp
        def test_noop():
            pass
    """)
    assert r.returncode == 0


def test_daq_bbp_tests_skip_without_device_usb(tmp_path):
    r = _run_pytest(tmp_path, ["--daq"], """
        import pytest

        @pytest.mark.requires_daq_bbp
        def test_needs_both_links():
            assert False, "must not run without --device-usb"
    """)
    assert r.returncode == 0
    assert "1 skipped" in r.stdout


def test_daq_bbp_tests_skip_without_daq_flag(tmp_path):
    """requires_daq_bbp must imply --daq as well as --device-usb.

    item.keywords is a mapping, so "requires_daq" in keywords is an exact key
    lookup -- it does NOT match a test marked only requires_daq_bbp. Without an
    explicit check, such a test would run against hardware that was never
    requested.
    """
    r = _run_pytest(tmp_path, ["--device-usb", "/dev/null"], """
        import pytest

        @pytest.mark.requires_daq_bbp
        def test_needs_the_p4_stream_too():
            assert False, "must not run without --daq"
    """)
    assert r.returncode == 0
    assert "1 skipped" in r.stdout
