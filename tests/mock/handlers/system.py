"""
System telemetry handlers for SimulatedDevice.

Handles: MEM_STATUS.

The heap model is not a constant. Internal free RAM shrinks when streams run
and scripts are resident, ``min_ever`` only ever decreases, and the largest
contiguous block degrades faster than the free total (fragmentation). Without
that, a host-side pressure check would pass against the simulator no matter how
broken it was.
"""

import struct

from bugbuster.constants import CmdId
from bugbuster.memory import MEM_STATUS_SCHEMA

# ESP32-S3 with 8 MB PSRAM, matching the real board.
_INTERNAL_TOTAL = 327_680
_PSRAM_TOTAL = 8_388_608

# Idle baseline, near the figures recorded on hardware in tasks.h.
_INTERNAL_BASELINE_FREE = 105_000

# Costs charged against internal RAM by each active consumer.
_COST_ADC_STREAM = 12_000
_COST_SCOPE_STREAM = 16_000
_COST_DSP_STREAM = 24_000
_COST_SCRIPT_RESIDENT = 20_000

_TASKS = [
    ("adcPoll", 2560, 1268),
    ("faultMon", 2560, 1204),
    ("cmdProc", 2048, 1216),
    ("wavegen", 2048, 1180),
    ("mainLoop", 5120, 2436),
    ("bbpCli", 5120, 2360),
]

_TASK_NAME_LEN = 12


def register(device) -> None:
    device.mem_internal_min_ever = _INTERNAL_BASELINE_FREE

    def _internal_free() -> int:
        used = 0
        if device._stream_thread is not None:
            used += _COST_ADC_STREAM
        if device.adc_diag_paused:
            used += _COST_SCOPE_STREAM
        if device.dsp_stream is not None:
            used += _COST_DSP_STREAM
        if getattr(device, "script_mode", 0) == 1:
            used += _COST_SCRIPT_RESIDENT
        return max(4096, _INTERNAL_BASELINE_FREE - used)

    def handle_mem_status(payload: bytes) -> bytes:
        free = _internal_free()
        # A real min-ever is a low-water mark: it never recovers when the
        # pressure does.
        device.mem_internal_min_ever = min(device.mem_internal_min_ever, free)

        # Fragmentation worsens as the pool drains, so the largest block falls
        # faster than the free total rather than tracking it linearly.
        largest = int(free * (0.30 + 0.45 * (free / _INTERNAL_BASELINE_FREE)))

        psram_free = _PSRAM_TOTAL - 512_000

        buf = bytearray()
        buf.append(MEM_STATUS_SCHEMA)
        buf += struct.pack(
            "<8I",
            free, device.mem_internal_min_ever, largest, _INTERNAL_TOTAL,
            psram_free, psram_free, psram_free, _PSRAM_TOTAL,
        )
        buf += struct.pack("<I", device.uptime_ms)
        buf.append(len(_TASKS))
        for name, declared, hwm in _TASKS:
            buf += struct.pack(
                f"<{_TASK_NAME_LEN}sHH",
                name.encode("ascii")[: _TASK_NAME_LEN - 1], declared, hwm)
        return bytes(buf)

    device.register_handler(CmdId.MEM_STATUS, handle_mem_status)
