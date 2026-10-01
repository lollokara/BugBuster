"""PLT-07 (measured on hardware 2026-10-01): one HTTPS bugbuster.http_get left
the uPython task 1964 bytes of its 6 KiB stack - under the 2 KiB margin the
plan requires, with deeper Python call stacks still to come. MP_TASK_STACK
must leave >= 2 KiB above that measured TLS peak."""

import re

import pytest

from tests.lib.srcread import read_source

MEASURED_TLS_USE = 6144 - 1964


@pytest.mark.xfail(strict=True, reason="PLT-07")
def test_mp_task_stack_has_tls_headroom():
    m = re.search(r"#define MP_TASK_STACK\s+\((\d+)u\s*\*\s*1024u\)", read_source("Firmware/ESP32/src/config.h"))
    stack = int(m.group(1)) * 1024
    assert stack - MEASURED_TLS_USE >= 4096, f"{stack - MEASURED_TLS_USE} B headroom"
