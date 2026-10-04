"""Regression guards for MicroPython frozen manifest and CMake configuration.

Pins invariants to prevent silent omission of frozen Python modules (bb_logging,
bb_helpers, bb_devices) from the ESP32-S3 firmware build.
"""

import re

from tests.lib.srcread import REPO_ROOT, read_source

MANIFEST_PATH = "Firmware/ESP32/components/micropython/manifest.py"
CMAKELISTS_PATH = "Firmware/ESP32/components/micropython/CMakeLists.txt"
WORKFLOW_PATH = ".github/workflows/esp32-firmware.yml"


def test_manifest_references_firmware_modules_directory():
    manifest_src = read_source(MANIFEST_PATH)
    assert 'freeze("$(PORT_DIR)/../../../../python/firmware_modules")' in manifest_src or (
        "freeze" in manifest_src and "python/firmware_modules" in manifest_src
    ), f"{MANIFEST_PATH} must freeze python/firmware_modules"


def test_manifest_path_resolves_to_existing_modules():
    port_dir = REPO_ROOT / "Firmware" / "ESP32" / "components" / "micropython"
    # Resolve the relative path as makemanifest.py does:
    resolved_dir = (port_dir / "../../../../python/firmware_modules").resolve()
    expected_dir = (REPO_ROOT / "python" / "firmware_modules").resolve()

    assert resolved_dir == expected_dir, f"Resolved {resolved_dir} != {expected_dir}"
    assert resolved_dir.is_dir(), f"{resolved_dir} is not a directory"

    for required_module in ("bb_logging.py", "bb_helpers.py", "bb_devices.py"):
        mod_path = resolved_dir / required_module
        assert mod_path.is_file(), f"Missing required frozen module source: {mod_path}"
        assert mod_path.stat().st_size > 0, f"Module {required_module} is empty"


def test_bb_logging_defines_logging_methods():
    logging_src = read_source("python/firmware_modules/bb_logging.py")
    assert "def info(" in logging_src, "bb_logging.py must define info()"
    assert "def warn(" in logging_src, "bb_logging.py must define warn()"
    assert "def error(" in logging_src, "bb_logging.py must define error()"


def test_cmakelists_makemanifest_failure_is_fatal():
    cmake_src = read_source(CMAKELISTS_PATH)
    # makemanifest.py failure must be FATAL_ERROR, never a WARNING
    assert 'message(WARNING "MicroPython: makemanifest.py failed' not in cmake_src, (
        "makemanifest.py failure must not be a non-fatal WARNING"
    )
    assert re.search(
        r'makemanifest\.py.*?RESULT_VARIABLE\s+([A-Za-z0-9_]+).*?if\(\1\)\s*message\(FATAL_ERROR',
        cmake_src,
        re.DOTALL,
    ), "makemanifest.py non-zero exit must trigger CMake FATAL_ERROR"


def test_cmakelists_validates_frozen_modules_presence():
    cmake_src = read_source(CMAKELISTS_PATH)
    # CMake must explicitly verify that frozen_content.c contains the bb_* modules
    assert "bb_logging" in cmake_src
    assert "bb_helpers" in cmake_src
    assert "bb_devices" in cmake_src
    assert re.search(
        r'if\(.*?NOT.*?MATCHES.*?"bb_logging".*?\)\s*message\(FATAL_ERROR',
        cmake_src,
        re.DOTALL,
    ), "CMake must fail fatally if frozen_content.c lacks bb_logging"


def test_cmakelists_inits_micropython_lib_submodule():
    cmake_src = read_source(CMAKELISTS_PATH)
    assert "lib/micropython-lib" in cmake_src, (
        "CMakeLists.txt must reference and initialize lib/micropython-lib"
    )


def test_workflow_initializes_and_verifies_frozen_modules():
    wf_src = read_source(WORKFLOW_PATH)
    assert "lib/micropython-lib" in wf_src, (
        f"{WORKFLOW_PATH} must initialize lib/micropython-lib submodule"
    )
    assert "frozen_module_bb_logging" in wf_src, (
        f"{WORKFLOW_PATH} must verify frozen_module_bb_logging in build"
    )
    assert "frozen_module_bb_helpers" in wf_src, (
        f"{WORKFLOW_PATH} must verify frozen_module_bb_helpers in build"
    )
    assert "frozen_module_bb_devices" in wf_src, (
        f"{WORKFLOW_PATH} must verify frozen_module_bb_devices in build"
    )
