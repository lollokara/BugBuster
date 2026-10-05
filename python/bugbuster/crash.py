"""Crash dump and boot-report consumers for the ESP32-S3 mainboard.

Mirrors Firmware/ESP32/src/diag/crash_report.cpp. Three inputs lead to the same
types:

* ``GET /api/system/crash``            -> :func:`parse_crash_json` (:class:`CrashSummary`)
* ``GET /api/system/crash?report=1``   -> :func:`parse_boot_report_json` (:class:`BootReport`)
* ``BOOTRPT <boot> <section> <i>/<n> <json>`` log lines, as shipped to the remote
  log platform ~30 s after boot -> :func:`parse_bootrpt_lines`.

Nothing here talks to the device; :class:`~bugbuster.client.BugBuster` fetches
the JSON and the MCP tools format it.
"""

from __future__ import annotations

import base64
import json
import re
from dataclasses import dataclass, field
from typing import Any, Iterable, Optional

# The firmware sections, in the order they are logged.
SECTIONS = ("sys", "mem", "net", "hw", "scripts", "crash")

# Same defaults as MemoryStatus.warnings() / the firmware ``heap`` command.
_MIN_INTERNAL_BYTES = 24 * 1024
_MIN_LARGEST_BYTES = 8 * 1024
_LOW_STACK_BYTES = 512
_BOOTLOOP_STREAK = 3

_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_LINE = re.compile(r"BOOTRPT\s+(\d+)\s+(\w+)\s+(\d+)/(\d+)\s*(.*)$")


def _hex(value: Any) -> Optional[int]:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return None
    return None


@dataclass(frozen=True)
class PreCrash:
    """RTC-RAM snapshot taken within ~5 s before the previous boot ended."""

    boot: int
    streak: int
    up_ms: int
    phase: str
    int_free: int
    int_min: int
    int_largest: int
    psram_free: int
    psram_min: int
    min_stack_free: int
    min_stack_task: str
    wifi_sta: bool
    hat: bool
    usb: bool

    def warnings(self) -> list[str]:
        out: list[str] = []
        if self.int_free < _MIN_INTERNAL_BYTES:
            out.append(f"internal heap was down to {self.int_free} B before the crash")
        if self.int_largest < _MIN_LARGEST_BYTES:
            out.append(f"largest internal block was only {self.int_largest} B before the crash")
        if self.min_stack_task and self.min_stack_free < _LOW_STACK_BYTES:
            out.append(f"task {self.min_stack_task} had {self.min_stack_free} B of stack left")
        return out


def _pre(d: Optional[dict]) -> Optional[PreCrash]:
    if not isinstance(d, dict):
        return None
    g = d.get
    return PreCrash(
        boot=int(g("boot", 0)), streak=int(g("streak", 0)), up_ms=int(g("up_ms", 0)),
        phase=str(g("phase", "")), int_free=int(g("int_free", 0)), int_min=int(g("int_min", 0)),
        int_largest=int(g("int_largest", 0)), psram_free=int(g("psram_free", 0)),
        psram_min=int(g("psram_min", 0)), min_stack_free=int(g("min_stack_free", 0)),
        min_stack_task=str(g("min_stack_task", "")), wifi_sta=bool(g("wifi_sta", False)),
        hat=bool(g("hat", False)), usb=bool(g("usb", False)),
    )


@dataclass(frozen=True)
class CrashSummary:
    """Reset state plus, when a coredump is stored, its decoded summary."""

    ready: bool
    present: bool
    valid: bool
    reset_reason: str
    reset_code: int
    abnormal: bool
    boot: int
    streak: int
    size: int = 0
    panic: str = ""
    task: str = ""
    pc: Optional[int] = None
    exccause: Optional[int] = None
    exccause_name: str = ""
    vaddr: Optional[int] = None
    backtrace: tuple[int, ...] = ()
    bt_corrupt: bool = False
    regs: tuple[int, ...] = ()
    dump_elf: str = ""
    elf_match: Optional[bool] = None
    pre: Optional[PreCrash] = None
    check: str = ""

    @property
    def has_dump(self) -> bool:
        """A stored dump that passed its checksum and can be downloaded."""
        return self.valid and self.size > 0

    @property
    def bootloop(self) -> bool:
        return self.streak >= _BOOTLOOP_STREAK

    def warnings(self) -> list[str]:
        out: list[str] = []
        if not self.ready:
            out.append("crash info not ready yet (the device checks it ~1 s after boot)")
        if self.abnormal:
            out.append(f"last reset was abnormal: {self.reset_reason}")
        if self.bootloop:
            out.append(f"{self.streak} consecutive abnormal resets - probable boot loop")
        if self.present and not self.valid:
            out.append(f"stored coredump failed its check ({self.check or 'invalid'})")
        if self.has_dump and self.elf_match is False:
            out.append("the dump was taken by a different firmware build than the running one; "
                       f"fetch the ELF for sha {self.dump_elf[:9] or '?'} to symbolise it")
        if self.has_dump and self.bt_corrupt:
            out.append("backtrace is marked corrupted (stack overflow or smashed frame)")
        if self.pre:
            out.extend(self.pre.warnings())
        return out

    def summary(self) -> str:
        if not self.ready:
            return "crash info not ready"
        head = f"reset {self.reset_reason} (boot {self.boot}"
        head += f", streak {self.streak})" if self.streak else ")"
        if not self.has_dump:
            return head + ", no coredump stored"
        what = self.exccause_name or (f"cause {self.exccause}" if self.exccause is not None else "")
        pc = f"0x{self.pc:08x}" if self.pc is not None else "?"
        return (f"{head}, coredump {self.size} B: {self.panic or what or 'panic'} "
                f"in task '{self.task}' at pc={pc}")

    def addr2line_command(self, elf_path: str = "firmware.elf") -> str:
        """Command that resolves PC + backtrace against the matching build."""
        addrs = [a for a in ([self.pc] if self.pc is not None else []) + list(self.backtrace)]
        if not addrs:
            return ""
        return ("xtensa-esp32s3-elf-addr2line -pfiaC -e " + elf_path + " "
                + " ".join(f"0x{a:08x}" for a in addrs))

    def decode_command(self, core_path: str = "coredump.elf", elf_path: str = "firmware.elf") -> str:
        return f"espcoredump.py info_corefile -c {core_path} {elf_path}"


def parse_crash_json(d: dict) -> CrashSummary:
    """Decode the ``/api/system/crash`` JSON (also the ``crash`` section of the bundle)."""
    reset = d.get("reset") or {}
    bt = tuple(a for a in (_hex(x) for x in d.get("bt", [])) if a is not None)
    regs = tuple(a for a in (_hex(x) for x in d.get("regs", [])) if a is not None)
    return CrashSummary(
        ready=bool(d.get("ready", True)),
        present=bool(d.get("present", False)),
        valid=bool(d.get("valid", False)),
        reset_reason=str(reset.get("reason", "UNKNOWN")),
        reset_code=int(reset.get("code", 0)),
        abnormal=bool(reset.get("abnormal", False)),
        boot=int(reset.get("boot", 0)),
        streak=int(reset.get("streak", 0)),
        size=int(d.get("size", 0)),
        panic=str(d.get("panic", "")),
        task=str(d.get("task", "")),
        pc=_hex(d.get("pc")),
        exccause=d.get("exccause") if isinstance(d.get("exccause"), int) else None,
        exccause_name=str(d.get("exccause_name", "")),
        vaddr=_hex(d.get("vaddr")),
        backtrace=bt,
        bt_corrupt=bool(d.get("bt_corrupt", False)),
        regs=regs,
        dump_elf=str(d.get("dump_elf", "")),
        elf_match=d.get("elf_match") if isinstance(d.get("elf_match"), bool) else None,
        pre=_pre(d.get("pre")),
        check=str(d.get("check", "")),
    )


def decode_chunk(resp: dict) -> bytes:
    """Bytes of one ``?offset=&len=`` reply; raises ValueError on an error reply."""
    if not resp.get("ok"):
        raise ValueError(resp.get("error", "coredump chunk failed"))
    data = base64.b64decode(resp["data"], validate=True)
    if len(data) != int(resp["len"]):
        raise ValueError(f"chunk length mismatch: header {resp['len']}, payload {len(data)}")
    return data


@dataclass
class BootReport:
    """One boot's diagnostics package: ``sections`` maps name -> decoded JSON."""

    boot: int
    sections: dict[str, Any] = field(default_factory=dict)
    complete: bool = False                    # the ``end`` line was seen
    incomplete_sections: list[str] = field(default_factory=list)   # parts were lost

    @property
    def crash(self) -> Optional[CrashSummary]:
        c = self.sections.get("crash")
        return parse_crash_json(c) if isinstance(c, dict) else None

    @property
    def firmware(self) -> str:
        return str(self.sections.get("sys", {}).get("fw", ""))

    def warnings(self) -> list[str]:
        out: list[str] = []
        for s in self.incomplete_sections:
            out.append(f"section '{s}' is missing log lines")
        mem = self.sections.get("mem")
        if isinstance(mem, dict):
            i = mem.get("int", {})
            if i.get("min", 1 << 30) < _MIN_INTERNAL_BYTES:
                out.append(f"internal heap all-time minimum {i['min']} B is below "
                           f"{_MIN_INTERNAL_BYTES // 1024} KB")
            if i.get("largest", 1 << 30) < _MIN_LARGEST_BYTES:
                out.append(f"largest internal block {i['largest']} B is below "
                           f"{_MIN_LARGEST_BYTES // 1024} KB")
            for t in mem.get("tasks", []):
                if t.get("low"):
                    out.append(f"task {t.get('n')} has only {t.get('hwm')} B of stack headroom")
        hat = self.sections.get("hw", {}).get("hat", {})
        if hat.get("degraded"):
            out.append("HAT link is degraded")
        sys_sec = self.sections.get("sys", {})
        if sys_sec.get("ota") == "pending":
            out.append("firmware is still pending OTA verification")
        c = self.crash
        if c:
            out.extend(c.warnings())
        return out

    def summary(self) -> str:
        sysd = self.sections.get("sys", {})
        parts = [f"boot {self.boot}"]
        if self.firmware:
            parts.append(f"fw {self.firmware}")
        mem = self.sections.get("mem", {}).get("int")
        if mem:
            parts.append(f"internal {mem.get('free', 0) // 1024} KB free "
                         f"(min {mem.get('min', 0) // 1024} KB)")
        if "up_ms" in sysd:
            parts.append(f"up {sysd['up_ms'] / 1000:.0f} s")
        c = self.crash
        if c:
            parts.append(c.summary())
        return ", ".join(parts)

    def to_dict(self) -> dict:
        return {"boot": self.boot, "complete": self.complete,
                "sections": self.sections, "warnings": self.warnings()}


def parse_boot_report_json(d: dict, boot: Optional[int] = None) -> BootReport:
    """Decode the ``?report=1`` object (sections as keys)."""
    sections = {k: d[k] for k in SECTIONS if k in d}
    if boot is None:
        boot = int(sections.get("sys", {}).get("reset", {}).get("boot", 0))
    return BootReport(boot=boot, sections=sections, complete=True)


def parse_bootrpt_lines(lines: Iterable[str] | str) -> list[BootReport]:
    """Reassemble reports from raw log text (log platform export or serial capture).

    Lines may carry an ESP-IDF prefix (``W (12345) bootrpt: ``), ANSI colour codes
    or a platform timestamp; anything before ``BOOTRPT`` is ignored. Reports are
    returned in boot order. A section whose parts are not all present is dropped
    from ``sections`` and listed in ``incomplete_sections``.
    """
    if isinstance(lines, str):
        lines = lines.splitlines()
    parts: dict[tuple[int, str], dict[int, str]] = {}
    totals: dict[tuple[int, str], int] = {}
    ended: set[int] = set()
    boots: list[int] = []
    for raw in lines:
        m = _LINE.search(_ANSI.sub("", raw).rstrip())
        if not m:
            continue
        boot, sec, i, n, payload = int(m[1]), m[2], int(m[3]), int(m[4]), m[5]
        if boot not in boots:
            boots.append(boot)
        if sec == "end":
            ended.add(boot)
            continue
        if n < 1 or not 1 <= i <= n:
            continue
        key = (boot, sec)
        totals[key] = n
        parts.setdefault(key, {})[i] = payload

    reports: dict[int, BootReport] = {b: BootReport(boot=b, complete=b in ended) for b in boots}
    for (boot, sec), got in parts.items():
        rep = reports[boot]
        if len(got) != totals[(boot, sec)]:
            rep.incomplete_sections.append(sec)
            continue
        text = "".join(got[k] for k in sorted(got))
        try:
            rep.sections[sec] = json.loads(text)
        except ValueError:
            rep.incomplete_sections.append(sec)
    return [reports[b] for b in sorted(reports)]
