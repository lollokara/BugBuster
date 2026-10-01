"""PLT-07 prerequisite: the MicroPython task ("uPython", MP_TASK_STACK) was not
in the stack-telemetry registry, so its headroom during TLS (bugbuster.http_get
over HTTPS) could not be measured from MCP/HTTP/BBP at all."""

import re

import pytest

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source


@pytest.mark.xfail(strict=True, reason="PLT-07")
def test_micropython_task_is_in_stack_telemetry():
    reg = extract_function("Firmware/ESP32/src/tasks.cpp", r"^size_t tasks_get_registry\(")
    assert '"uPython"' in reg


def test_registry_size_matches_spec():
    reg = extract_function("Firmware/ESP32/src/tasks.cpp", r"^size_t tasks_get_registry\(")
    entries = len(re.findall(r'\{\s*"\w+",\s*\w+\s*\}', reg))
    size = int(re.search(r"#define BB_TASK_REGISTRY_MAX\s+(\d+)",
                         read_source("Firmware/ESP32/src/tasks.h")).group(1))
    assert entries == size
