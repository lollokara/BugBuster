"""Memory telemetry: wire decoding, derived metrics, and cross-surface parity.

Covers python/bugbuster/memory.py, the MEM_STATUS simulator handler and the
/api/system/memory simulator route, and pins the firmware<->host contract by
reading the layout constants straight out of bbp.h.
"""

import re
import struct

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId
from bugbuster.memory import (
    MEM_STATUS_SCHEMA,
    HeapPool,
    MemoryStatus,
    TaskStack,
    parse_mem_status,
    parse_mem_status_json,
)
from tests.lib.srcread import read_source
from tests.mock import SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")


def _usb_client():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedUSBTransport(device, hat=True))
    client.connect()
    return client, device


def _http_client():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedHTTPTransport(device, hat=True))
    client.connect()
    return client, device


# ---------------------------------------------------------------------------
# Firmware <-> host contract
# ---------------------------------------------------------------------------

def test_command_id_matches_the_firmware():
    m = re.search(r"#define\s+BBP_CMD_MEM_STATUS\s+0x([0-9A-Fa-f]+)", BBP_H)
    assert m, "BBP_CMD_MEM_STATUS is not defined in bbp.h"
    assert int(m.group(1), 16) == int(CmdId.MEM_STATUS)


def test_schema_version_matches_the_firmware():
    m = re.search(r"#define\s+BBP_MEM_STATUS_SCHEMA\s+(\d+)", BBP_H)
    assert m, "BBP_MEM_STATUS_SCHEMA is not defined in bbp.h"
    assert int(m.group(1)) == MEM_STATUS_SCHEMA, (
        "the firmware bumped the MEM_STATUS schema without updating "
        "python/bugbuster/memory.py")


def test_task_name_length_matches_the_firmware():
    m = re.search(r"#define\s+BBP_MEM_TASK_NAME_LEN\s+(\d+)", BBP_H)
    assert m
    # A mismatch here silently shifts every field after the first task name.
    assert int(m.group(1)) == 12


def test_command_id_is_not_reused_by_another_command():
    ids = re.findall(r"#define\s+BBP_CMD_(\w+)\s+0x([0-9A-Fa-f]{2})", BBP_H)
    owners = [name for name, val in ids if int(val, 16) == int(CmdId.MEM_STATUS)]
    assert owners == ["MEM_STATUS"], f"opcode 0x0C is claimed by {owners}"


# ---------------------------------------------------------------------------
# Wire decoding
# ---------------------------------------------------------------------------

def _build_payload(schema=MEM_STATUS_SCHEMA, tasks=(("adcPoll", 2560, 1268),)):
    buf = bytearray([schema])
    buf += struct.pack("<8I", 100_000, 80_000, 40_000, 327_680,
                       7_000_000, 6_900_000, 6_000_000, 8_388_608)
    buf += struct.pack("<I", 12_345)
    buf.append(len(tasks))
    for name, declared, free in tasks:
        buf += struct.pack("<12sHH", name.encode(), declared, free)
    return bytes(buf)


def test_parses_a_well_formed_payload():
    m = parse_mem_status(_build_payload())
    assert m.internal.free_bytes == 100_000
    assert m.internal.min_ever_bytes == 80_000
    assert m.internal.largest_block_bytes == 40_000
    assert m.internal.total_bytes == 327_680
    assert m.uptime_ms == 12_345
    assert [t.name for t in m.tasks] == ["adcPoll"]
    assert m.tasks[0].declared_bytes == 2560
    assert m.tasks[0].free_bytes == 1268


def test_rejects_an_unknown_schema_rather_than_misreading_it():
    with pytest.raises(ValueError, match="schema"):
        parse_mem_status(_build_payload(schema=MEM_STATUS_SCHEMA + 1))


def test_rejects_a_truncated_header():
    with pytest.raises(ValueError, match="short"):
        parse_mem_status(_build_payload()[:10])


def test_rejects_a_payload_that_promises_more_tasks_than_it_carries():
    payload = bytearray(_build_payload(tasks=(("adcPoll", 2560, 1268),)))
    payload[37] = 4  # task_count byte claims 4 tasks, only 1 follows
    with pytest.raises(ValueError, match="ran out"):
        parse_mem_status(bytes(payload))


def test_task_names_are_trimmed_at_the_nul_pad():
    m = parse_mem_status(_build_payload(tasks=(("bbpCli", 5120, 2360),)))
    assert m.tasks[0].name == "bbpCli"


# ---------------------------------------------------------------------------
# Derived metrics
# ---------------------------------------------------------------------------

def test_fragmentation_is_zero_when_free_space_is_one_block():
    pool = HeapPool(free_bytes=1000, min_ever_bytes=500,
                    largest_block_bytes=1000, total_bytes=4000)
    assert pool.fragmentation_pct == pytest.approx(0.0)


def test_fragmentation_rises_as_the_largest_block_shrinks():
    pool = HeapPool(free_bytes=1000, min_ever_bytes=500,
                    largest_block_bytes=100, total_bytes=4000)
    assert pool.fragmentation_pct == pytest.approx(90.0)


def test_empty_pool_reports_zero_rather_than_dividing_by_zero():
    pool = HeapPool(0, 0, 0, 0)
    assert pool.used_pct == 0.0
    assert pool.fragmentation_pct == 0.0


def test_worst_task_is_the_one_closest_to_overflow():
    m = MemoryStatus(
        internal=HeapPool(1, 1, 1, 1), psram=HeapPool(0, 0, 0, 0), uptime_ms=0,
        tasks=[TaskStack("a", 2048, 900), TaskStack("b", 2048, 200),
               TaskStack("c", 2048, 1500)])
    assert m.worst_task.name == "b"


def test_worst_task_ignores_tasks_that_are_not_running():
    m = MemoryStatus(
        internal=HeapPool(1, 1, 1, 1), psram=HeapPool(0, 0, 0, 0), uptime_ms=0,
        tasks=[TaskStack("up", 2048, 900), TaskStack("down", 2048, 0, running=False)])
    assert m.worst_task.name == "up"


def test_a_board_without_psram_is_reported_as_such():
    m = MemoryStatus(internal=HeapPool(1, 1, 1, 1), psram=HeapPool(0, 0, 0, 0),
                     uptime_ms=0)
    assert m.has_psram is False


def test_healthy_device_produces_no_warnings():
    m = MemoryStatus(
        internal=HeapPool(120_000, 100_000, 90_000, 327_680),
        psram=HeapPool(7_000_000, 6_000_000, 6_000_000, 8_388_608),
        uptime_ms=0, tasks=[TaskStack("adcPoll", 2560, 1268)])
    assert m.warnings() == []


def test_low_internal_free_is_warned_about():
    m = MemoryStatus(
        internal=HeapPool(10_000, 10_000, 9_000, 327_680),
        psram=HeapPool(0, 0, 0, 0), uptime_ms=0)
    assert any("internal free" in w for w in m.warnings())


def test_a_fragmented_heap_warns_even_when_free_space_looks_healthy():
    """This is the failure mode that produced 'Failed to start update task':
    plenty of free heap, but no block big enough to allocate a task stack."""
    m = MemoryStatus(
        internal=HeapPool(free_bytes=120_000, min_ever_bytes=100_000,
                          largest_block_bytes=4_000, total_bytes=327_680),
        psram=HeapPool(0, 0, 0, 0), uptime_ms=0)
    warns = m.warnings()
    assert any("largest internal block" in w for w in warns)
    assert not any("internal free" in w for w in warns), (
        "free heap is healthy here — only the block-size warning should fire")


def test_a_nearly_full_stack_is_warned_about():
    m = MemoryStatus(
        internal=HeapPool(120_000, 100_000, 90_000, 327_680),
        psram=HeapPool(0, 0, 0, 0), uptime_ms=0,
        tasks=[TaskStack("bbpCli", 5120, 200)])
    assert any("bbpCli" in w for w in m.warnings())


# ---------------------------------------------------------------------------
# JSON decoding (HTTP transport)
# ---------------------------------------------------------------------------

def test_json_accepts_camel_case():
    m = parse_mem_status_json({
        "internal": {"freeBytes": 1, "minEverBytes": 2,
                     "largestBlockBytes": 3, "totalBytes": 4},
        "psram": {}, "tasks": [], "uptimeMs": 9,
    })
    assert (m.internal.free_bytes, m.internal.total_bytes) == (1, 4)
    assert m.uptime_ms == 9


def test_json_accepts_snake_case():
    m = parse_mem_status_json({
        "internal": {"free_bytes": 1, "min_ever_bytes": 2,
                     "largest_block_bytes": 3, "total_bytes": 4},
        "psram": {}, "tasks": [], "uptime_ms": 9,
    })
    assert (m.internal.free_bytes, m.internal.total_bytes) == (1, 4)
    assert m.uptime_ms == 9


# ---------------------------------------------------------------------------
# End-to-end through the simulator
# ---------------------------------------------------------------------------

def test_usb_client_reads_a_snapshot():
    client, _ = _usb_client()
    m = client.get_memory_status()
    assert m.internal.total_bytes > 0
    assert len(m.tasks) == 6
    client.disconnect()


def test_http_client_reads_the_same_figures_as_usb():
    """The BBP handler and the HTTP route are separate code paths in the
    firmware; if the simulated ones disagree, neither transport's tests mean
    anything for the other."""
    usb_c, _ = _usb_client()
    http_c, _ = _http_client()
    a = usb_c.get_memory_status()
    b = http_c.get_memory_status()
    assert a.internal.free_bytes == b.internal.free_bytes
    assert a.internal.total_bytes == b.internal.total_bytes
    assert [t.name for t in a.tasks] == [t.name for t in b.tasks]
    usb_c.disconnect()
    http_c.disconnect()


def test_running_a_stream_measurably_consumes_internal_ram():
    """A constant-valued simulator would let a broken pressure check pass."""
    client, _ = _usb_client()
    idle = client.get_memory_status().internal.free_bytes
    client.start_adc_stream([0], divider=1, callback=lambda *_: None)
    try:
        loaded = client.get_memory_status().internal.free_bytes
    finally:
        client.stop_adc_stream()
    assert loaded < idle, "starting a stream did not reduce reported free RAM"


def test_min_ever_does_not_recover_when_the_pressure_does():
    client, _ = _usb_client()
    client.start_adc_stream([0], divider=1, callback=lambda *_: None)
    under_load = client.get_memory_status().internal.min_ever_bytes
    client.stop_adc_stream()
    after = client.get_memory_status()
    assert after.internal.free_bytes > under_load
    assert after.internal.min_ever_bytes == under_load, (
        "min-ever is a low-water mark — it must not climb back up")


def test_fragmentation_worsens_under_pressure():
    client, _ = _usb_client()
    idle = client.get_memory_status().internal.fragmentation_pct
    client.start_adc_stream([0], divider=1, callback=lambda *_: None)
    try:
        loaded = client.get_memory_status().internal.fragmentation_pct
    finally:
        client.stop_adc_stream()
    assert loaded > idle


def test_simulator_reports_the_same_task_set_as_the_firmware():
    tasks_h = read_source("Firmware/ESP32/src/tasks.h")
    declared = set(re.findall(r"#define\s+TASK_STACK_(\w+)\s+\d+", tasks_h))
    client, _ = _usb_client()
    sim_names = {t.name.upper() for t in client.get_memory_status().tasks}
    client.disconnect()
    assert sim_names == declared, (
        "the simulated task table drifted from tasks.h — a host-side test would "
        "then pass against tasks the device does not have")


def test_reported_stack_sizes_match_the_firmware_constants():
    tasks_h = read_source("Firmware/ESP32/src/tasks.h")
    expected = {name.upper(): int(val)
                for name, val in re.findall(
                    r"#define\s+TASK_STACK_(\w+)\s+(\d+)", tasks_h)}
    client, _ = _usb_client()
    for t in client.get_memory_status().tasks:
        assert t.declared_bytes == expected[t.name.upper()], (
            f"{t.name}: simulator says {t.declared_bytes} B, tasks.h says "
            f"{expected[t.name.upper()]} B")
    client.disconnect()


def test_dispatch_is_registered_for_mem_status():
    device = SimulatedDevice()
    assert int(CmdId.MEM_STATUS) in device._handlers


# ---------------------------------------------------------------------------
# Internal-SRAM pressure guards (2026-08-06 optimisation pass)
# ---------------------------------------------------------------------------

def _registered_descriptor_count() -> int:
    """Count CmdDescriptor entries across every bbp/cmds/*.cpp block.

    Verified against the device's own `cmds` CLI listing (143, fw 5.0.0,
    2026-08-06). Entries are matched inside the descriptor array only, and the
    opcode may sit on the same line as the brace or the next one (cmd_ota.cpp
    uses the multi-line form).
    """
    import pathlib

    root = pathlib.Path(__file__).resolve().parents[2]
    total = 0
    for path in sorted((root / "Firmware/ESP32/src/bbp/cmds").glob("*.cpp")):
        src = read_source(f"Firmware/ESP32/src/bbp/cmds/{path.name}")
        for block in re.findall(
            r"static\s+const\s+CmdDescriptor\s+\w+\[\]\s*=\s*\{(.*?)\n\};",
            src,
            re.DOTALL,
        ):
            total += len(re.findall(r"\{\s*BBP_CMD_", block))
    return total


def test_command_registry_holds_pointers_not_copied_descriptors():
    """The registry must index the flash-resident const blocks, not memcpy them
    into .bss. Copying cost 32 B per slot (8192 B reserved) of internal DRAM on
    a device that only has ~40 KB free; the pointer index costs 4 B per slot."""
    src = read_source("Firmware/ESP32/src/bbp/cmd_registry.cpp")
    assert re.search(r"static\s+const\s+CmdDescriptor\s+\*s_registry\[", src), (
        "s_registry must be an array of `const CmdDescriptor *`. A plain "
        "`CmdDescriptor s_registry[]` puts a full copy of every descriptor in "
        "internal .bss."
    )
    assert "memcpy(&s_registry[" not in src, (
        "descriptors must be referenced, not copied into the registry"
    )


def test_command_registry_ceiling_still_fits_every_registered_command():
    src = read_source("Firmware/ESP32/src/bbp/cmd_registry.cpp")
    m = re.search(r"#define\s+CMD_REGISTRY_MAX\s+(\d+)", src)
    assert m, "CMD_REGISTRY_MAX not found in cmd_registry.cpp"
    ceiling = int(m.group(1))
    registered = _registered_descriptor_count()
    assert registered >= 100, (
        f"descriptor scan found only {registered} entries; the device's `cmds` "
        f"listing reported 143 on fw 5.0.0, so the block regex has drifted and "
        f"this guard is no longer measuring anything"
    )
    assert registered <= ceiling, (
        f"{registered} descriptors are registered but CMD_REGISTRY_MAX is "
        f"{ceiling}; cmd_registry_register_block() would silently drop a whole "
        f"subsystem block at boot"
    )


def test_wavegen_stop_only_reclaims_the_channel_when_a_waveform_was_running():
    """wavegen_stop_and_reset() runs on every USB DISCONNECT. Unconditionally
    enqueueing CMD_SET_CHANNEL_FUNC there forced channel 0 to HIGH_IMP behind
    the host's back and drove cmdProc's stack peak from 824 B to 1672 B."""
    src = read_source("Firmware/ESP32/src/bbp/bbp.cpp")
    body = src.split("static void wavegen_stop_and_reset", 1)[1]
    body = body.split("\nvoid bbpStopWavegen", 1)[0]
    assert "was_active" in body, "the wavegen.active snapshot is gone"
    assert re.search(r"if\s*\(\s*!was_active\s*\)\s*return\s*;", body), (
        "wavegen_stop_and_reset() must return early when no waveform was "
        "active, before it enqueues CMD_SET_CHANNEL_FUNC"
    )
    assert body.index("was_active") < body.index("CMD_SET_CHANNEL_FUNC"), (
        "the guard must precede the enqueue"
    )


# EXT_RAM_BSS_ATTR is silently ignored on function-scope statics, so a `static`
# array declared inside a function lands in internal DRAM no matter what. These
# four were found that way on 2026-08-06 (8220 B combined).
_PSRAM_BUFFERS = [
    ("Firmware/ESP32/src/hat/hat.cpp", "s_script_names"),
    ("Firmware/ESP32/src/web/ws_stream.cpp", "s_tx_ring"),
    ("Firmware/ESP32/src/web/ws_stream.cpp", "s_tx_frame"),
    ("Firmware/ESP32/src/mp/repl_ws.cpp", "s_tx_ring"),
    ("Firmware/ESP32/src/mp/repl_ws.cpp", "s_tx_frame"),
    ("Firmware/ESP32/src/bbp/cmds/cmd_script.cpp", "s_script_names"),
    ("Firmware/ESP32/src/web/webserver.cpp", "s_script_names"),
]


@pytest.mark.parametrize(("path", "name"), _PSRAM_BUFFERS)
def test_large_scratch_buffers_stay_in_psram_at_file_scope(path, name):
    src = read_source(path)
    decl = re.search(
        rf"^static\s+EXT_RAM_BSS_ATTR\s+\w+\s+{name}\s*\[", src, re.MULTILINE
    )
    assert decl, (
        f"{path}: {name} must be a FILE-SCOPE `static EXT_RAM_BSS_ATTR` array. "
        f"Declaring it inside a function silently puts it in internal DRAM — "
        f"the attribute has no effect on function-scope statics."
    )


def test_no_function_scope_static_arrays_in_the_websocket_tx_paths():
    """The trap above, pinned at the sites where it actually bit."""
    for path in ("Firmware/ESP32/src/web/ws_stream.cpp",
                 "Firmware/ESP32/src/mp/repl_ws.cpp",
                 "Firmware/ESP32/src/hat/hat.cpp"):
        src = read_source(path)
        offenders = [
            m.group(0).strip()
            for m in re.finditer(r"^[ \t]+static\s+(?!EXT_RAM_BSS_ATTR)"
                                 r"[\w ]+\s+\w+\s*\[[^\]]+\]\s*;", src, re.MULTILINE)
        ]
        assert not offenders, (
            f"{path}: function-scope `static` arrays land in internal DRAM "
            f"regardless of EXT_RAM_BSS_ATTR — move them to file scope: "
            f"{offenders}"
        )
