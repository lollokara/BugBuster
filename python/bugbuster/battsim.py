"""
battsim.py - host access to the DAQ HAT battery simulator (P4 ``battsim/``).

Transport: BBP ``DAQ_CONFIG`` sub-op ``0x0B`` over USB, or ``POST /api/daq/bs`` and
``/api/daq/bs/read`` over HTTP. The S3 passes bytes through verbatim; this module
is the reference decoder for every on-flash struct in
``Firmware/DAQ_HAT/ESP32P4/src/battsim/battsim_store.h`` and the status struct in
``battsim.h``. History record v1 (32 B) and v2 (48 B, see
``Docs/superpowers/specs/2026-10-02-battsim-history-v2.md``) are both decoded;
the layout is chosen from ``RunMeta.version``.

Usage::

    bs = bb.battsim
    st = bs.status()
    for run in bs.list_runs():
        h = bs.history(run)              # merged best-resolution series
        print(h.stats())
"""
from __future__ import annotations

import base64
import functools
import json
import math
import os
import re
import struct
import time
from dataclasses import asdict, dataclass, field
from enum import IntEnum
from typing import Any, Callable, Dict, List, Optional, Tuple

from .constants import CmdId
from .daq_config import DaqAction, DaqKey

DAQ_CFG_BATTSIM = 0x0B
READ_CHUNK_USB = 236
READ_CHUNK_HTTP = 2048


class BsOp(IntEnum):
    STATUS = 0
    LIST_RUNS = 1
    RUN_DIR = 2
    READ = 3
    PROFILE = 4
    SET_EPOCH = 5


class BsState(IntEnum):
    NONE = 0
    PAUSED = 1
    ACTIVE = 2
    DEPLETED = 3
    STOPPED = 4


class BsChem(IntEnum):
    LIPO = 0
    LIFEPO4 = 1
    NIMH = 2
    LEAD = 3


class BsFile(IntEnum):
    META = 0
    CKPT = 1
    EVENTS = 2
    Q15 = 3
    S1 = 4
    M1_BASE = 0x100


class BsEvent(IntEnum):
    CREATED = 1
    START = 2
    PAUSE = 3
    STOP = 4
    DEPLETED = 5
    REBOOT = 6
    PARAM = 7
    STALL = 8
    OUTPUT_OFF = 9
    PD_LOST = 10
    STORE_ERR = 11


BS_ERRORS = ["none", "no store", "no run", "busy", "invalid", "state", "no PD contract",
             "acquisition not running", "I/O error", "not found"]

FLAG_PROVISIONAL = 0x01
FLAG_STORE_OK = 0x02
FLAG_OUTPUT_ON = 0x04
FLAG_SD = 0x08
FLAG_EXT = 0x10
FLAG_DITHER = 0x20
FLAG_REMAIN_OK = 0x40
FLAG_STORE_LOW = 0x80

# Record v2 flags.
RF_GAP = 0x0001
RF_I_CLAMP = 0x0002
RF_RESUME = 0x0004
RF_OUTPUT_OFF = 0x0008
RF_CUTOFF = 0x0010

_STATUS_FMT = "<BBBBHBBIHHIIffffqqqqqII"
_PARAMS_FMT = "<BBBBB3xIHHIHHI"
_META_FMT = "<IHHI24s28sI"
_EVENT_FMT = "<IHHii"
_REC_V1_FMT = "<IHHHHiiiq"
_REC_V2_FMT = "<IHHHHiiHHqqq"
_CKPT_HEAD_FMT = "<IHBBII7q2d"
STATUS_SIZE = struct.calcsize(_STATUS_FMT)
META_SIZE = struct.calcsize(_META_FMT)
EVENT_SIZE = struct.calcsize(_EVENT_FMT)
REC_SIZE = {1: struct.calcsize(_REC_V1_FMT), 2: struct.calcsize(_REC_V2_FMT)}
CKPT_SIZE = 352
assert STATUS_SIZE == 88 and META_SIZE == 68 and EVENT_SIZE == 16
assert REC_SIZE[1] == 32 and REC_SIZE[2] == 48


# --------------------------------------------------------------------------
# Decoded structures
# --------------------------------------------------------------------------
@dataclass
class BsParams:
    chem: int
    cells: int
    sd_enable: bool
    ext_enable: bool
    dither: bool
    capacity_mah: int
    start_soc_pct: float
    cutoff_mv_cell: int
    rint_uohm_cell: int
    peukert: float
    sd_pct_month: float
    ext_load_ua: int

    @property
    def chem_name(self) -> str:
        return BsChem(self.chem).name if self.chem in BsChem._value2member_map_ else str(self.chem)


def parse_params(raw: bytes) -> BsParams:
    (chem, cells, sd, ext, dither, cap, soc10, cut, rint, peuk, sd100, ext_ua) = \
        struct.unpack_from(_PARAMS_FMT, raw, 0)
    return BsParams(chem, cells, bool(sd), bool(ext), bool(dither), cap, soc10 / 10.0,
                    cut, rint, peuk / 1000.0, sd100 / 100.0, ext_ua)


@dataclass
class BsStatus:
    version: int
    state: int
    flags: int
    last_error: int
    run_id: int
    chem: int
    cells: int
    capacity_mah: int
    soc_pct: float
    profile_mask: int
    elapsed_s: int
    remaining_s: Optional[int]
    v_target: float
    v_meas: float
    i_meas: float
    i_avg: float
    q_used_c: float
    q_dut_c: float
    q_ext_c: float
    q_sd_c: float
    q_peuk_c: float
    fs_total: int
    fs_used: int
    e_dut_j: Optional[float] = None   # status v2

    @property
    def state_name(self) -> str:
        return BsState(self.state).name if self.state in BsState._value2member_map_ else str(self.state)

    @property
    def error_name(self) -> str:
        return BS_ERRORS[self.last_error] if self.last_error < len(BS_ERRORS) else str(self.last_error)


def parse_status(raw: bytes) -> BsStatus:
    if len(raw) < STATUS_SIZE:
        raise ValueError(f"short battsim status: {len(raw)} < {STATUS_SIZE}")
    f = struct.unpack_from(_STATUS_FMT, raw, 0)
    flags = f[2]
    return BsStatus(
        version=f[0], state=f[1], flags=flags, last_error=f[3], run_id=f[4], chem=f[5],
        cells=f[6], capacity_mah=f[7], soc_pct=f[8] / 100.0, profile_mask=f[9],
        elapsed_s=f[10], remaining_s=f[11] if flags & FLAG_REMAIN_OK else None,
        v_target=f[12], v_meas=f[13], i_meas=f[14], i_avg=f[15],
        q_used_c=f[16] * 1e-9, q_dut_c=f[17] * 1e-9, q_ext_c=f[18] * 1e-9,
        q_sd_c=f[19] * 1e-9, q_peuk_c=f[20] * 1e-9, fs_total=f[21], fs_used=f[22],
        e_dut_j=struct.unpack_from("<q", raw, 88)[0] * 1e-6 if len(raw) >= 96 else None)


@dataclass
class RunMeta:
    run_id: int
    version: int
    created_epoch: int
    name: str
    params: BsParams


def parse_meta(raw: bytes) -> RunMeta:
    if len(raw) < META_SIZE:
        raise ValueError("short run meta")
    magic, ver, run_id, epoch, name, params, _crc = struct.unpack_from(_META_FMT, raw, 0)
    if magic != 0x4E525342:
        raise ValueError(f"bad run meta magic 0x{magic:08X}")
    return RunMeta(run_id, ver, epoch, name.split(b"\0", 1)[0].decode("utf-8", "replace"),
                   parse_params(params))


@dataclass
class RunCkpt:
    state: int
    seq: int
    elapsed_s: float
    q_dut_c: float
    q_ext_c: float
    q_sd_c: float
    q_peuk_c: float
    q_init_c: float
    q15_interval_s: int
    q15_count: int


def parse_ckpt(raw: bytes) -> RunCkpt:
    if len(raw) < CKPT_SIZE:
        raise ValueError("short checkpoint")
    h = struct.unpack_from(_CKPT_HEAD_FMT, raw, 0)
    if h[0] != 0x4B435342:
        raise ValueError("bad checkpoint magic")
    q15_iv, q15_n = struct.unpack_from("<II", raw, 336)
    ticks_per_s = 16_384_000  # BS_MCLK_HZ
    return RunCkpt(state=h[2], seq=h[4], elapsed_s=h[7] / ticks_per_s,
                   q_dut_c=h[6] * 1e-12, q_ext_c=h[8] * 1e-12, q_sd_c=h[9] * 1e-12,
                   q_peuk_c=h[10] * 1e-12, q_init_c=h[11] * 1e-12,
                   q15_interval_s=q15_iv, q15_count=q15_n)


@dataclass
class RunEvent:
    t_s: int
    code: int
    a: int
    b: int

    @property
    def name(self) -> str:
        return BsEvent(self.code).name if self.code in BsEvent._value2member_map_ else f"EV{self.code}"


def parse_events(raw: bytes) -> List[RunEvent]:
    n = len(raw) // EVENT_SIZE
    return [RunEvent(*struct.unpack_from(_EVENT_FMT, raw, i * EVENT_SIZE)[:2],
                     *struct.unpack_from("<ii", raw, i * EVENT_SIZE + 8)) for i in range(n)]


@dataclass
class HistRec:
    """One history point. Cumulative fields are absolute since run start."""
    t_s: int                 # end of interval, simulated seconds
    dt_s: float              # interval length
    v_avg: float
    v_min: float
    v_max: float
    soc_pct: float
    i_avg: float             # A
    i_min: float
    i_max: float
    flags: int
    q_dut_c: Optional[float]   # v2 only
    q_used_c: float
    e_dut_j: Optional[float]   # v2 only
    tier: str = ""

    @property
    def p_avg(self) -> float:
        return self.v_avg * self.i_avg


def parse_records(raw: bytes, version: int, nominal_dt: float, tier: str) -> List[HistRec]:
    """Decode a history file. v1 dt is inferred from consecutive timestamps."""
    size = REC_SIZE.get(version)
    if size is None:
        raise ValueError(f"unsupported history version {version}")
    out: List[HistRec] = []
    prev_t: Optional[int] = None
    for i in range(len(raw) // size):
        off = i * size
        if version == 1:
            t, va, vn, vx, soc, ia, imn, imx, qu = struct.unpack_from(_REC_V1_FMT, raw, off)
            dt = float(t - prev_t) if prev_t is not None and t > prev_t else min(float(t), nominal_dt)
            out.append(HistRec(t, dt,
                               va / 1e3, vn / 1e3, vx / 1e3, soc / 100.0,
                               ia * 1e-9, imn * 1e-9, imx * 1e-9, 0, None, qu * 1e-9, None, tier))
        else:
            (t, va, vn, vx, soc, imn, imx, flags, dts, qd, qu, eu) = \
                struct.unpack_from(_REC_V2_FMT, raw, off)
            out.append(HistRec(t, float(dts), va / 1e3, vn / 1e3, vx / 1e3, soc / 100.0,
                               0.0, imn * 1e-9, imx * 1e-9, flags, qd * 1e-9, qu * 1e-9,
                               eu * 1e-6, tier))
        prev_t = t
    if version >= 2:
        derive_interval_current(out)
    return out


def derive_interval_current(recs: List[HistRec]) -> None:
    """v2: exact I_avg = dQ/dt from the cumulative DUT charge of consecutive points."""
    prev: Optional[HistRec] = None
    for r in recs:
        if r.q_dut_c is None or r.dt_s <= 0:
            prev = r
            continue
        start = r.t_s - r.dt_s
        if prev is not None and prev.q_dut_c is not None and abs(prev.t_s - start) <= 1:
            r.i_avg = (r.q_dut_c - prev.q_dut_c) / r.dt_s
        elif start <= 0:
            r.i_avg = r.q_dut_c / r.dt_s
        else:
            r.i_avg = (r.i_min + r.i_max) / 2.0
        prev = r


# --------------------------------------------------------------------------
# History series + statistics
# --------------------------------------------------------------------------
def _pct(sorted_vals: List[float], p: float) -> float:
    if not sorted_vals:
        return math.nan
    k = (len(sorted_vals) - 1) * p
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


@dataclass
class RunHistory:
    meta: RunMeta
    records: List[HistRec]
    events: List[RunEvent] = field(default_factory=list)
    ckpt: Optional[RunCkpt] = None

    @property
    def energy_exact(self) -> bool:
        return self.meta.version >= 2

    def window(self, t0: float = -math.inf, t1: float = math.inf) -> List[HistRec]:
        return [r for r in self.records if r.t_s > t0 and r.t_s - r.dt_s < t1]

    def stats(self, t0: float = -math.inf, t1: float = math.inf) -> Dict[str, Any]:
        """Window statistics. Averages are time-weighted; energy is exact for v2."""
        rs = self.window(t0, t1)
        if not rs:
            return {"points": 0}
        tw = sum(r.dt_s for r in rs) or 1.0
        i_sorted = sorted(r.i_avg for r in rs)
        charge = sum(r.i_avg * r.dt_s for r in rs)
        if self.energy_exact and rs[0].e_dut_j is not None and rs[-1].e_dut_j is not None:
            first = self.records.index(rs[0])
            e0 = self.records[first - 1].e_dut_j if first > 0 else 0.0
            energy = rs[-1].e_dut_j - (e0 or 0.0)
        else:
            energy = sum(r.p_avg * r.dt_s for r in rs)
        span = rs[-1].t_s - (rs[0].t_s - rs[0].dt_s)
        soc_drop = rs[0].soc_pct - rs[-1].soc_pct
        i_mean = charge / tw
        cap_c = self.meta.params.capacity_mah * 3.6
        return {
            "points": len(rs),
            "t_start_s": rs[0].t_s - rs[0].dt_s,
            "t_end_s": rs[-1].t_s,
            "duration_s": span,
            "v_min": min(r.v_min for r in rs),
            "v_max": max(r.v_max for r in rs),
            "v_avg": sum(r.v_avg * r.dt_s for r in rs) / tw,
            "i_min": min(r.i_min for r in rs),
            "i_max": max(r.i_max for r in rs),
            "i_avg": i_mean,
            "i_p10": _pct(i_sorted, 0.10),     # baseline / sleep current estimate
            "i_median": _pct(i_sorted, 0.50),
            "i_p99": _pct(i_sorted, 0.99),
            "p_avg": energy / tw,
            "p_max": max(r.v_max * r.i_max for r in rs),
            "charge_c": charge,
            "charge_mah": charge / 3.6,
            "energy_j": energy,
            "energy_mwh": energy / 3.6,
            "energy_estimated": not self.energy_exact,
            "soc_start": rs[0].soc_pct,
            "soc_end": rs[-1].soc_pct,
            "soc_drop_pct": soc_drop,
            "soc_rate_pct_per_day": soc_drop / span * 86400 if span > 0 else math.nan,
            "projected_life_s": (cap_c / i_mean) if i_mean > 0 else math.inf,
            "remaining_s_at_avg": (rs[-1].soc_pct / 100.0 * cap_c / i_mean) if i_mean > 0 else math.inf,
            "gaps": sum(1 for r in rs if r.flags & RF_GAP),
            "clamped": sum(1 for r in rs if r.flags & RF_I_CLAMP),
        }

    def to_json(self) -> Dict[str, Any]:
        return {
            "schema": "bugbuster.battsim.run/1",
            "meta": asdict(self.meta),
            "checkpoint": asdict(self.ckpt) if self.ckpt else None,
            "events": [dict(asdict(e), name=e.name) for e in self.events],
            "energy_exact": self.energy_exact,
            "columns": ["t_s", "dt_s", "v_avg", "v_min", "v_max", "soc_pct", "i_avg",
                        "i_min", "i_max", "flags", "q_dut_c", "q_used_c", "e_dut_j", "tier"],
            "rows": [[r.t_s, r.dt_s, r.v_avg, r.v_min, r.v_max, r.soc_pct, r.i_avg, r.i_min,
                      r.i_max, r.flags, r.q_dut_c, r.q_used_c, r.e_dut_j, r.tier]
                     for r in self.records],
            "stats": {k: (None if isinstance(v, float) and not math.isfinite(v) else v)
                      for k, v in self.stats().items()},
        }


def merge_tiers(q15: List[HistRec], m1: List[HistRec], s1: List[HistRec]) -> List[HistRec]:
    """Best resolution per time range: s1 over m1 over q15 (each tier covers a suffix)."""
    out: List[HistRec] = []
    m1_start = (m1[0].t_s - m1[0].dt_s) if m1 else math.inf
    s1_start = (s1[0].t_s - s1[0].dt_s) if s1 else math.inf
    out += [r for r in q15 if r.t_s <= min(m1_start, s1_start)]
    out += [r for r in m1 if r.t_s <= s1_start]
    out += s1
    return out


# --------------------------------------------------------------------------
# Device access
# --------------------------------------------------------------------------
@dataclass
class RunFile:
    file_id: int
    size: int

    @property
    def name(self) -> str:
        if self.file_id >= BsFile.M1_BASE:
            return f"m{self.file_id - BsFile.M1_BASE:04d}.bin"
        return {0: "meta.bin", 1: "ckpt.bin", 2: "ev.bin", 3: "q15.bin", 4: "s1.bin"}.get(
            self.file_id, f"f{self.file_id}.bin")


class BattSim:
    """Battery simulator accessor bound to a :class:`BugBuster` client."""

    def __init__(self, client) -> None:
        self._c = client

    # -- transport ---------------------------------------------------------
    def _req(self, op: BsOp, args: bytes = b"") -> bytes:
        if getattr(self._c, "_usb", False):
            return self._c._usb_cmd(CmdId.DAQ_CONFIG, bytes([DAQ_CFG_BATTSIM, int(op)]) + args)
        r = self._c._http_post("/daq/bs", {"op": int(op), "args": args.hex()})
        if not r or not r.get("ok", False):
            raise RuntimeError((r or {}).get("error", "battsim request failed"))
        return base64.b64decode(r.get("data", ""))

    def _read_chunk(self, run: int, file_id: int, off: int, n: int) -> bytes:
        if getattr(self._c, "_usb", False):
            return self._req(BsOp.READ, struct.pack("<HHIB", run, file_id, off, min(n, READ_CHUNK_USB)))
        r = self._c._http_post("/daq/bs/read", {"run": run, "file": file_id, "off": off,
                                                "len": min(n, READ_CHUNK_HTTP)})
        if not r or not r.get("ok", False):
            raise RuntimeError((r or {}).get("error", "battsim read failed"))
        return base64.b64decode(r.get("data", ""))

    # -- queries -----------------------------------------------------------
    def status(self) -> BsStatus:
        return parse_status(self._req(BsOp.STATUS))

    def list_runs(self) -> Tuple[List[int], int]:
        """Returns (run ids ascending, active run id or 0)."""
        ids: List[int] = []
        active = 0
        while True:
            raw = self._req(BsOp.LIST_RUNS, struct.pack("<H", len(ids)))
            total, active = struct.unpack_from("<HH", raw, 0)
            page = [struct.unpack_from("<H", raw, 4 + 2 * i)[0] for i in range((len(raw) - 4) // 2)]
            ids += page
            if not page or len(ids) >= total:
                return ids, active

    def run_dir(self, run: int) -> List[RunFile]:
        files: List[RunFile] = []
        while True:
            raw = self._req(BsOp.RUN_DIR, struct.pack("<HB", run, len(files)))
            _, total, _ = struct.unpack_from("<HBB", raw, 0)
            n = (len(raw) - 4) // 6
            files += [RunFile(*struct.unpack_from("<HI", raw, 4 + 6 * i)) for i in range(n)]
            if n == 0 or len(files) >= total:
                return files

    def read_file(self, run: int, file_id: int, offset: int = 0, size: Optional[int] = None,
                  progress: Optional[Callable[[int], None]] = None) -> bytes:
        buf = bytearray()
        while size is None or len(buf) < size:
            want = READ_CHUNK_HTTP if size is None else size - len(buf)
            chunk = self._read_chunk(run, file_id, offset + len(buf), want)
            buf += chunk
            if progress:
                progress(len(buf))
            if not chunk or len(chunk) < min(want, READ_CHUNK_USB):
                break
        return bytes(buf)

    def profile(self, slot: int) -> Tuple[str, BsParams]:
        raw = self._req(BsOp.PROFILE, bytes([slot]))
        return raw[:24].split(b"\0", 1)[0].decode("utf-8", "replace"), parse_params(raw[24:52])

    def set_epoch(self, unix_s: Optional[int] = None) -> None:
        self._req(BsOp.SET_EPOCH, struct.pack("<I", int(time.time() if unix_s is None else unix_s)))

    # -- history -----------------------------------------------------------
    def sync_run(self, run: int, cache_dir: str,
                 progress: Optional[Callable[[str, int, int], None]] = None) -> str:
        """Mirror a run into ``cache_dir/rNNNNN``, fetching only appended bytes.

        History files are append-only except q15 (compacted in place) and day files
        that age out; a head mismatch or a shrink triggers a full refetch.
        """
        d = os.path.join(cache_dir, f"r{run:05d}")
        os.makedirs(d, exist_ok=True)
        remote = {f.name: f for f in self.run_dir(run)}
        for name in os.listdir(d):
            if name not in remote:
                os.remove(os.path.join(d, name))
        for name, f in remote.items():
            path = os.path.join(d, name)
            local = b""
            if os.path.exists(path) and f.file_id not in (BsFile.META, BsFile.CKPT, BsFile.S1):
                with open(path, "rb") as fh:
                    local = fh.read()
            if local and len(local) <= f.size:
                head = self.read_file(run, f.file_id, 0, min(len(local), READ_CHUNK_USB))
                if head != local[:len(head)]:
                    local = b""
            elif len(local) > f.size:
                local = b""
            if len(local) < f.size:
                cb = (functools.partial(_offset_progress, progress, name, len(local), f.size)
                      if progress is not None else None)
                tail = self.read_file(run, f.file_id, len(local), f.size - len(local), cb)
                local += tail
            with open(path, "wb") as out:
                out.write(local)
        return d

    def history(self, run: int, cache_dir: Optional[str] = None) -> RunHistory:
        if cache_dir:
            d = self.sync_run(run, cache_dir)
            return load_history_dir(d)
        files = {f.name: f for f in self.run_dir(run)}
        blobs = {name: self.read_file(run, f.file_id, 0, f.size) for name, f in files.items()}
        return history_from_blobs(blobs)

    # -- control (settings registry) --------------------------------------
    def configure(self, *, chem: Optional[int] = None, cells: Optional[int] = None,
                  capacity_mah: Optional[int] = None, start_soc_pct: Optional[float] = None,
                  cutoff_mv_cell: Optional[int] = None, rint_uohm_cell: Optional[int] = None,
                  peukert: Optional[float] = None, sd_enable: Optional[bool] = None,
                  sd_pct_month: Optional[float] = None, ext_enable: Optional[bool] = None,
                  ext_load_ua: Optional[int] = None, dither: Optional[bool] = None,
                  name: Optional[str] = None) -> None:
        daq = self._c.daq
        pairs = [
            (DaqKey.BS_CHEM, chem), (DaqKey.BS_CELLS, cells),
            (DaqKey.BS_CAPACITY_MAH, capacity_mah),
            (DaqKey.BS_START_SOC, None if start_soc_pct is None else round(start_soc_pct * 10)),
            (DaqKey.BS_CUTOFF_MV, cutoff_mv_cell), (DaqKey.BS_RINT_UOHM, rint_uohm_cell),
            (DaqKey.BS_PEUKERT, None if peukert is None else round(peukert * 1000)),
            (DaqKey.BS_SD_ENABLE, sd_enable),
            (DaqKey.BS_SD_PCT, None if sd_pct_month is None else round(sd_pct_month * 100)),
            (DaqKey.BS_EXT_ENABLE, ext_enable), (DaqKey.BS_EXT_UA, ext_load_ua),
            (DaqKey.BS_DITHER, dither), (DaqKey.BS_NAME, name),
        ]
        for key, val in pairs:
            if val is not None:
                daq.set(key, val)

    def _act(self, action: DaqAction, run: Optional[int] = None, slot: Optional[int] = None) -> None:
        if run is not None:
            self._c.daq.set(DaqKey.BS_RUN_SELECT, run)
        if slot is not None:
            self._c.daq.set(DaqKey.BS_PROFILE_SLOT, slot)
        self._c.daq.action(action)

    def new_run(self) -> None:
        self._act(DaqAction.BS_RUN_NEW)

    def start(self) -> None:
        self._act(DaqAction.BS_RUN_START)

    def pause(self) -> None:
        self._act(DaqAction.BS_RUN_PAUSE)

    def stop(self) -> None:
        self._act(DaqAction.BS_RUN_STOP)

    def unload(self) -> None:
        self._act(DaqAction.BS_RUN_UNLOAD)

    def load_run(self, run: int) -> None:
        self._act(DaqAction.BS_RUN_LOAD, run=run)

    def delete_run(self, run: int) -> None:
        self._act(DaqAction.BS_RUN_DELETE, run=run)

    def chem_defaults(self) -> None:
        self._act(DaqAction.BS_DEFAULTS)

    def save_profile(self, slot: int) -> None:
        self._act(DaqAction.BS_PROFILE_SAVE, slot=slot)

    def load_profile(self, slot: int) -> None:
        self._act(DaqAction.BS_PROFILE_LOAD, slot=slot)

    def delete_profile(self, slot: int) -> None:
        self._act(DaqAction.BS_PROFILE_DELETE, slot=slot)


def _offset_progress(report: Callable[[str, int, int], None], name: str, base: int,
                     total: int, n: int) -> None:
    report(name, base + n, total)


def history_from_blobs(blobs: Dict[str, bytes]) -> RunHistory:
    """Build a merged series from raw run files keyed by on-device file name."""
    meta = parse_meta(blobs["meta.bin"])
    ver = meta.version
    ckpt = None
    if len(blobs.get("ckpt.bin", b"")) >= CKPT_SIZE:
        try:
            ckpt = parse_ckpt(blobs["ckpt.bin"])
        except ValueError:
            ckpt = None
    q15_dt = float(ckpt.q15_interval_s) if ckpt else 900.0
    q15 = parse_records(blobs.get("q15.bin", b""), ver, q15_dt, "q15")
    m1: List[HistRec] = []
    for name in sorted(n for n in blobs if re.fullmatch(r"m\d{4}\.bin", n)):
        m1 += parse_records(blobs[name], ver, 60.0, "m1")
    s1 = parse_records(blobs.get("s1.bin", b""), ver, 1.0, "s1")
    events = parse_events(blobs.get("ev.bin", b""))
    merged = merge_tiers(q15, m1, s1)
    if ver >= 2:
        derive_interval_current(merged)
    return RunHistory(meta, merged, events, ckpt)


def load_history_dir(path: str) -> RunHistory:
    blobs = {}
    for name in os.listdir(path):
        with open(os.path.join(path, name), "rb") as fh:
            blobs[name] = fh.read()
    return history_from_blobs(blobs)


def export_json(history: RunHistory, path: str) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(history.to_json(), fh, allow_nan=False, default=lambda o: None)


def export_csv(history: RunHistory, path: str) -> None:
    import csv
    doc = history.to_json()
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(doc["columns"])
        w.writerows(doc["rows"])
