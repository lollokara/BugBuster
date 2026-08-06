"""Live memory-pressure telemetry for the ESP32-S3 mainboard.

The S3 runs tight on INTERNAL RAM. ``/api/status`` has always carried
``freeHeap``/``minFreeHeap``, but those are whole-heap figures: on a PSRAM
board they are dominated by external RAM and stay comfortable while the
internal pool — the one that actually runs out — is nearly exhausted. These
types split the pools apart and add the two figures that predict a failure:
the largest contiguous block (``xTaskCreate`` needs one, however much total
free there is) and per-task stack headroom.

Wire layout mirrors ``BBP_CMD_MEM_STATUS`` in Firmware/ESP32/src/bbp/bbp.h.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

# Bump in lockstep with BBP_MEM_STATUS_SCHEMA in bbp.h.
MEM_STATUS_SCHEMA = 1

_TASK_NAME_LEN = 12
_HEADER_FMT = "<B8IIB"
_HEADER_LEN = struct.calcsize(_HEADER_FMT)
_TASK_FMT = f"<{_TASK_NAME_LEN}sHH"
_TASK_LEN = struct.calcsize(_TASK_FMT)


@dataclass(frozen=True)
class TaskStack:
    """Stack accounting for one FreeRTOS task."""

    name: str
    declared_bytes: int
    free_bytes: int      # high-water mark: bytes never used
    running: bool = True

    @property
    def peak_used_bytes(self) -> int:
        return max(0, self.declared_bytes - self.free_bytes)

    @property
    def used_pct(self) -> float:
        if not self.declared_bytes:
            return 0.0
        return 100.0 * self.peak_used_bytes / self.declared_bytes


@dataclass(frozen=True)
class HeapPool:
    """One allocator pool (internal SRAM or external PSRAM)."""

    free_bytes: int
    min_ever_bytes: int
    largest_block_bytes: int
    total_bytes: int

    @property
    def used_pct(self) -> float:
        if not self.total_bytes:
            return 0.0
        return 100.0 * (self.total_bytes - self.free_bytes) / self.total_bytes

    @property
    def fragmentation_pct(self) -> float:
        """How much of the free space is unreachable as one allocation.

        0% means all free memory is one contiguous block; 90% means the
        largest single allocation that can succeed is a tenth of the free
        total. This is the figure that predicts "Failed to start update task"
        while free heap still looks healthy.
        """
        if not self.free_bytes:
            return 0.0
        return 100.0 * (1.0 - self.largest_block_bytes / self.free_bytes)


@dataclass(frozen=True)
class MemoryStatus:
    """A full memory snapshot from the S3."""

    internal: HeapPool
    psram: HeapPool
    uptime_ms: int
    tasks: list[TaskStack] = field(default_factory=list)
    schema: int = MEM_STATUS_SCHEMA

    @property
    def worst_task(self) -> TaskStack | None:
        """The task closest to overflowing its stack."""
        running = [t for t in self.tasks if t.running and t.declared_bytes]
        return min(running, key=lambda t: t.free_bytes) if running else None

    @property
    def has_psram(self) -> bool:
        return self.psram.total_bytes > 0

    def warnings(self, *, min_internal_kb: int = 24,
                 max_task_used_pct: float = 80.0,
                 min_largest_block_kb: int = 8) -> list[str]:
        """Human-readable pressure warnings; empty when the device is healthy.

        Thresholds default to the ones the firmware's own `heap` CLI command
        uses, so host and device agree on what "under pressure" means.
        """
        out: list[str] = []
        if self.internal.free_bytes < min_internal_kb * 1024:
            out.append(
                f"internal free {self.internal.free_bytes / 1024:.0f} KB is below "
                f"{min_internal_kb} KB")
        if self.internal.largest_block_bytes < min_largest_block_kb * 1024:
            out.append(
                f"largest internal block {self.internal.largest_block_bytes / 1024:.0f} KB "
                f"is below {min_largest_block_kb} KB — xTaskCreate may fail")
        if self.internal.min_ever_bytes < min_internal_kb * 1024:
            out.append(
                f"internal all-time minimum {self.internal.min_ever_bytes / 1024:.0f} KB "
                "indicates sustained pressure")
        for t in self.tasks:
            if t.running and t.used_pct > max_task_used_pct:
                out.append(
                    f"task {t.name} peaked at {t.used_pct:.0f}% of its "
                    f"{t.declared_bytes} B stack ({t.free_bytes} B free)")
        return out

    def summary(self) -> str:
        parts = [
            f"internal {self.internal.free_bytes / 1024:.0f}/"
            f"{self.internal.total_bytes / 1024:.0f} KB free "
            f"(min-ever {self.internal.min_ever_bytes / 1024:.0f} KB, "
            f"largest {self.internal.largest_block_bytes / 1024:.0f} KB, "
            f"frag {self.internal.fragmentation_pct:.0f}%)"
        ]
        if self.has_psram:
            parts.append(
                f"psram {self.psram.free_bytes / 1024:.0f}/"
                f"{self.psram.total_bytes / 1024:.0f} KB free")
        worst = self.worst_task
        if worst:
            parts.append(f"tightest stack {worst.name} {worst.free_bytes} B free")
        return "; ".join(parts)


def parse_mem_status(raw: bytes) -> MemoryStatus:
    """Decode a ``BBP_CMD_MEM_STATUS`` response."""
    if len(raw) < _HEADER_LEN:
        raise ValueError(f"short MEM_STATUS response: {len(raw)} < {_HEADER_LEN} bytes")

    (schema,
     int_free, int_min, int_largest, int_total,
     psram_free, psram_min, psram_largest, psram_total,
     uptime_ms, task_count) = struct.unpack_from(_HEADER_FMT, raw, 0)

    if schema != MEM_STATUS_SCHEMA:
        raise ValueError(
            f"unsupported MEM_STATUS schema {schema} (this client speaks "
            f"{MEM_STATUS_SCHEMA}) — update the bugbuster package to match the firmware")

    tasks = []
    off = _HEADER_LEN
    for _ in range(task_count):
        if len(raw) - off < _TASK_LEN:
            raise ValueError(
                f"MEM_STATUS declared {task_count} tasks but the payload ran out "
                f"after {len(tasks)}")
        name_b, declared, free = struct.unpack_from(_TASK_FMT, raw, off)
        off += _TASK_LEN
        name = name_b.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        # The firmware reports a task it could not resolve as 0 free bytes.
        tasks.append(TaskStack(name=name, declared_bytes=declared,
                               free_bytes=free, running=free > 0))

    return MemoryStatus(
        internal=HeapPool(int_free, int_min, int_largest, int_total),
        psram=HeapPool(psram_free, psram_min, psram_largest, psram_total),
        uptime_ms=uptime_ms,
        tasks=tasks,
        schema=schema,
    )


def _pool_from_json(obj: dict) -> HeapPool:
    def pick(*names, default=0):
        for n in names:
            if n in obj:
                return int(obj[n] or 0)
        return default

    return HeapPool(
        free_bytes=pick("freeBytes", "free_bytes"),
        min_ever_bytes=pick("minEverBytes", "min_ever_bytes"),
        largest_block_bytes=pick("largestBlockBytes", "largest_block_bytes"),
        total_bytes=pick("totalBytes", "total_bytes"),
    )


def parse_mem_status_json(payload: dict) -> MemoryStatus:
    """Decode a ``GET /api/system/memory`` response.

    The firmware emits both camelCase and snake_case keys for every field;
    accept either so the desktop app and the Python client can share fixtures.
    """
    tasks = []
    for t in payload.get("tasks", []) or []:
        declared = int(t.get("declaredBytes", t.get("declared_bytes", 0)) or 0)
        free = int(t.get("freeBytes", t.get("free_bytes", 0)) or 0)
        tasks.append(TaskStack(
            name=str(t.get("name", "?")),
            declared_bytes=declared,
            free_bytes=free,
            running=bool(t.get("running", free > 0)),
        ))

    return MemoryStatus(
        internal=_pool_from_json(payload.get("internal", {}) or {}),
        psram=_pool_from_json(payload.get("psram", {}) or {}),
        uptime_ms=int(payload.get("uptimeMs", payload.get("uptime_ms", 0)) or 0),
        tasks=tasks,
    )
