"""Spec 2026-10-03 section 6 + firmware memory rules for the S3 hub streaming modules."""
import re

from tests.lib.srcread import REPO_ROOT, read_source

HUB = "Firmware/ESP32/src/hub"


def _cpp():
    return {p.name: read_source(f"{HUB}/{p.name}") for p in sorted((REPO_ROOT / HUB).glob("*.cpp"))}


def test_every_big_allocation_is_psram():
    for name, src in _cpp().items():
        for call in re.findall(r"heap_caps_malloc\([^;]*;", src):
            assert "MALLOC_CAP_SPIRAM" in call, f"{name}: {call}"


def test_no_function_scope_static_arrays_of_1k_or_more():
    for name, src in _cpp().items():
        for m in re.finditer(r"^\s+static\s+[\w ]+?\s+\w+\[(\w+)\]", src, re.M):
            size = m.group(1)
            assert size.isdigit() and int(size) < 1024, f"{name}: {m.group(0).strip()} (function-scope statics land in DRAM)"


def test_big_state_tables_are_file_scope_psram():
    assert re.search(r"^static EXT_RAM_BSS_ATTR sync_state_t S;", read_source(f"{HUB}/hub_sync.cpp"), re.M)
    assert re.search(r"^static EXT_RAM_BSS_ATTR live_t L;", read_source(f"{HUB}/hub_live.cpp"), re.M)


def test_task_has_an_internal_6k_stack_because_it_writes_nvs():
    src = read_source(f"{HUB}/hub_task.cpp")
    assert 'xTaskCreatePinnedToCore(hub_task, "hub", 6144' in src
    assert "WithCaps" not in src


def test_script_output_is_teed_into_the_shipper():
    assert "scripting_set_log_tee(mpy_tee)" in read_source(f"{HUB}/hub_logs.cpp")
    assert "void scripting_set_log_tee(" in read_source("Firmware/ESP32/src/mp/scripting.h")
    cpp = read_source("Firmware/ESP32/src/mp/scripting.cpp")
    assert re.search(r"static void ring_emit\(.*?\)\s*\{.*?s_log_tee\(line, len\);", cpp, re.S)


def test_boot_order():
    main = read_source("Firmware/ESP32/src/main.cpp")
    assert main.index("hub_early_init();") < main.index("wifi_init(")
    start = main.index("hub_start();")
    assert start > main.index("initWebServer()") and start > main.index("hat_init()")
