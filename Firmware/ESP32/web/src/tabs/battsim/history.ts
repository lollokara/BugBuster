// =============================================================================
// battsim/history.ts - battery-simulator wire decoders, tier merge, decimated
// views and window statistics. Mirrors python/bugbuster/battsim.py (reference).
// =============================================================================

export const STATE_NAMES = ["No run", "Paused", "Active", "Depleted", "Stopped"];
export const CHEM_NAMES = ["LiPo", "LiFePO4", "NiMH", "Lead-acid"];
export const ERROR_TEXT = ["", "battlog partition missing", "no run loaded", "busy", "invalid parameters",
  "not allowed in this state", "USB-PD contract below 9 V / 3 A", "acquisition not running",
  "flash I/O error", "run not found"];
export const EVENT_NAMES: Record<number, string> = {
  1: "created", 2: "start", 3: "pause", 4: "stop", 5: "depleted", 6: "reboot",
  7: "param", 8: "stall", 9: "output off", 10: "PD lost", 11: "store error",
};

export interface BsParams {
  chem: number; cells: number; sdEnable: boolean; extEnable: boolean; dither: boolean;
  capacityMah: number; startSocPct: number; cutoffMvCell: number; rintUohmCell: number;
  peukert: number; sdPctMonth: number; extLoadUa: number;
}

export interface BsStatus {
  state: number; flags: number; lastError: number; runId: number; chem: number; cells: number;
  capacityMah: number; socPct: number; profileMask: number; elapsedS: number;
  remainingS: number | null; provisional: boolean; vTarget: number; vMeas: number; iMeas: number;
  iAvg: number; qUsedC: number; fsTotal: number; fsUsed: number;
}

export interface BsMeta { runId: number; version: number; createdEpoch: number; name: string; params: BsParams }
export interface BsEvent { t: number; code: number; a: number; b: number }

export interface Rec {
  t: number; dt: number; vAvg: number; vMin: number; vMax: number; soc: number;
  iAvg: number; iMin: number; iMax: number; flags: number;
  qDut: number | null; qUsed: number; eDut: number | null; tier: number;
}

export const STATUS_SIZE = 88;
export const META_SIZE = 68;
const FLAG_PROVISIONAL = 0x01;
const FLAG_REMAIN_OK = 0x40;
const RF_GAP = 0x0001;
const RF_I_CLAMP = 0x0002;

const dv = (b: Uint8Array) => new DataView(b.buffer, b.byteOffset, b.byteLength);

export function parseParams(b: Uint8Array): BsParams {
  const d = dv(b);
  return {
    chem: b[0]!, cells: b[1]!, sdEnable: b[2] !== 0, extEnable: b[3] !== 0, dither: b[4] !== 0,
    capacityMah: d.getUint32(8, true), startSocPct: d.getUint16(12, true) / 10,
    cutoffMvCell: d.getUint16(14, true), rintUohmCell: d.getUint32(16, true),
    peukert: d.getUint16(20, true) / 1000, sdPctMonth: d.getUint16(22, true) / 100,
    extLoadUa: d.getUint32(24, true),
  };
}

export function parseStatus(b: Uint8Array): BsStatus {
  if (b.length < STATUS_SIZE) throw new Error(`short battsim status (${b.length} B)`);
  const d = dv(b);
  const flags = b[2]!;
  return {
    state: b[1]!, flags, lastError: b[3]!, runId: d.getUint16(4, true), chem: b[6]!, cells: b[7]!,
    capacityMah: d.getUint32(8, true), socPct: d.getUint16(12, true) / 100,
    profileMask: d.getUint16(14, true), elapsedS: d.getUint32(16, true),
    remainingS: flags & FLAG_REMAIN_OK ? d.getUint32(20, true) : null,
    provisional: (flags & FLAG_PROVISIONAL) !== 0,
    vTarget: d.getFloat32(24, true), vMeas: d.getFloat32(28, true), iMeas: d.getFloat32(32, true),
    iAvg: d.getFloat32(36, true), qUsedC: Number(d.getBigInt64(40, true)) * 1e-9,
    fsTotal: d.getUint32(80, true), fsUsed: d.getUint32(84, true),
  };
}

export function parseMeta(b: Uint8Array): BsMeta {
  const d = dv(b);
  if (b.length < META_SIZE || d.getUint32(0, true) !== 0x4e525342) throw new Error("bad run meta");
  const nameBytes = b.subarray(12, 36);
  const end = nameBytes.indexOf(0);
  return {
    version: d.getUint16(4, true), runId: d.getUint16(6, true), createdEpoch: d.getUint32(8, true),
    name: new TextDecoder().decode(end >= 0 ? nameBytes.subarray(0, end) : nameBytes),
    params: parseParams(b.subarray(36, 64)),
  };
}

export function parseEvents(b: Uint8Array): BsEvent[] {
  const d = dv(b);
  const out: BsEvent[] = [];
  for (let o = 0; o + 16 <= b.length; o += 16) {
    out.push({ t: d.getUint32(o, true), code: d.getUint16(o + 4, true), a: d.getInt32(o + 8, true), b: d.getInt32(o + 12, true) });
  }
  return out;
}

export function parseRecords(b: Uint8Array, version: number, nominalDt: number, tier: number): Rec[] {
  const d = dv(b);
  const out: Rec[] = [];
  if (version === 1) {
    let prev: number | null = null;
    for (let o = 0; o + 32 <= b.length; o += 32) {
      const t = d.getUint32(o, true);
      const dt = prev !== null && t > prev ? t - prev : Math.min(t, nominalDt);
      out.push({
        t, dt, vAvg: d.getUint16(o + 4, true) / 1e3, vMin: d.getUint16(o + 6, true) / 1e3,
        vMax: d.getUint16(o + 8, true) / 1e3, soc: d.getUint16(o + 10, true) / 100,
        iAvg: d.getInt32(o + 12, true) * 1e-9, iMin: d.getInt32(o + 16, true) * 1e-9,
        iMax: d.getInt32(o + 20, true) * 1e-9, flags: 0, qDut: null,
        qUsed: Number(d.getBigInt64(o + 24, true)) * 1e-9, eDut: null, tier,
      });
      prev = t;
    }
  } else if (version === 2) {
    for (let o = 0; o + 48 <= b.length; o += 48) {
      out.push({
        t: d.getUint32(o, true), vAvg: d.getUint16(o + 4, true) / 1e3, vMin: d.getUint16(o + 6, true) / 1e3,
        vMax: d.getUint16(o + 8, true) / 1e3, soc: d.getUint16(o + 10, true) / 100,
        iMin: d.getInt32(o + 12, true) * 1e-9, iMax: d.getInt32(o + 16, true) * 1e-9,
        flags: d.getUint16(o + 20, true), dt: d.getUint16(o + 22, true), iAvg: 0,
        qDut: Number(d.getBigInt64(o + 24, true)) * 1e-9, qUsed: Number(d.getBigInt64(o + 32, true)) * 1e-9,
        eDut: Number(d.getBigInt64(o + 40, true)) * 1e-6, tier,
      });
    }
  } else {
    throw new Error(`unsupported history version ${version}`);
  }
  return out;
}

/** v2: exact interval current from consecutive cumulative DUT charge. */
export function deriveCurrent(recs: Rec[]): void {
  let prev: Rec | null = null;
  for (const r of recs) {
    if (r.qDut === null) continue;
    if (r.dt > 0) {
      const start = r.t - r.dt;
      if (prev && prev.qDut !== null && Math.abs(prev.t - start) <= 1) r.iAvg = (r.qDut - prev.qDut) / r.dt;
      else if (start <= 0) r.iAvg = r.qDut / r.dt;
      else r.iAvg = (r.iMin + r.iMax) / 2;
    }
    prev = r;
  }
}

export function mergeTiers(q15: Rec[], m1: Rec[], s1: Rec[]): Rec[] {
  const m1Start = m1.length ? m1[0]!.t - m1[0]!.dt : Infinity;
  const s1Start = s1.length ? s1[0]!.t - s1[0]!.dt : Infinity;
  return [
    ...q15.filter((r) => r.t <= Math.min(m1Start, s1Start)),
    ...m1.filter((r) => r.t <= s1Start),
    ...s1,
  ];
}

export interface History { meta: BsMeta; events: BsEvent[]; recs: Rec[]; q15Interval: number }

/** Build the merged series from raw run files keyed by device file name. */
export function buildHistory(files: Record<string, Uint8Array>): History {
  const meta = parseMeta(files["meta.bin"] ?? new Uint8Array());
  const ck = files["ckpt.bin"];
  let q15Interval = 900;
  if (ck && ck.length >= 352 && dv(ck).getUint32(0, true) === 0x4b435342) q15Interval = dv(ck).getUint32(336, true) || 900;
  const q15 = parseRecords(files["q15.bin"] ?? new Uint8Array(), meta.version, q15Interval, 0);
  const m1: Rec[] = [];
  for (const n of Object.keys(files).filter((n) => /^m\d{4}\.bin$/.test(n)).sort()) {
    m1.push(...parseRecords(files[n]!, meta.version, 60, 1));
  }
  const s1 = parseRecords(files["s1.bin"] ?? new Uint8Array(), meta.version, 1, 2);
  const recs = mergeTiers(q15, m1, s1);
  if (meta.version >= 2) deriveCurrent(recs);
  return { meta, events: parseEvents(files["ev.bin"] ?? new Uint8Array()), recs, q15Interval };
}

// ---------------------------------------------------------------------------
// Windows, statistics, decimation
// ---------------------------------------------------------------------------
function lowerBound(recs: Rec[], pred: (r: Rec) => boolean): number {
  let lo = 0, hi = recs.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (pred(recs[mid]!)) lo = mid + 1; else hi = mid;
  }
  return lo;
}

export function windowRange(h: History, t0: number, t1: number): [number, number] {
  const lo = lowerBound(h.recs, (r) => r.t <= t0);
  const hi = lowerBound(h.recs, (r) => r.t - r.dt < t1);
  return [lo, Math.max(lo, hi)];
}

export interface Stats {
  points: number; tStart: number; tEnd: number; duration: number;
  vMin: number; vMax: number; vAvg: number; iMin: number; iMax: number; iAvg: number;
  iP10: number; iMedian: number; iP99: number; pAvg: number; pMax: number;
  chargeC: number; energyJ: number; energyEstimated: boolean;
  socStart: number; socEnd: number; socRatePerDay: number;
  projectedLifeS: number; remainingAtAvgS: number; gaps: number; clamped: number;
}

function pct(sorted: number[], p: number): number {
  if (!sorted.length) return NaN;
  const k = (sorted.length - 1) * p;
  const lo = Math.floor(k), hi = Math.ceil(k);
  return sorted[lo]! + (sorted[hi]! - sorted[lo]!) * (k - lo);
}

export function stats(h: History, t0 = -Infinity, t1 = Infinity): Stats | null {
  const [lo, hi] = windowRange(h, t0, t1);
  const rs = h.recs.slice(lo, hi);
  if (!rs.length) return null;
  const tw = rs.reduce((s, r) => s + r.dt, 0) || 1e-9;
  const charge = rs.reduce((s, r) => s + r.iAvg * r.dt, 0);
  const exact = h.meta.version >= 2;
  const energy = exact
    ? (rs[rs.length - 1]!.eDut ?? 0) - (lo > 0 ? h.recs[lo - 1]!.eDut ?? 0 : 0)
    : rs.reduce((s, r) => s + r.vAvg * r.iAvg * r.dt, 0);
  const first = rs[0]!, last = rs[rs.length - 1]!;
  const span = last.t - (first.t - first.dt);
  const iMean = charge / tw;
  const capC = h.meta.params.capacityMah * 3.6;
  const is = rs.map((r) => r.iAvg).sort((a, b) => a - b);
  const mn = (f: (r: Rec) => number) => rs.reduce((m, r) => Math.min(m, f(r)), Infinity);
  const mx = (f: (r: Rec) => number) => rs.reduce((m, r) => Math.max(m, f(r)), -Infinity);
  return {
    points: rs.length, tStart: first.t - first.dt, tEnd: last.t, duration: span,
    vMin: mn((r) => r.vMin), vMax: mx((r) => r.vMax),
    vAvg: rs.reduce((s, r) => s + r.vAvg * r.dt, 0) / tw,
    iMin: mn((r) => r.iMin), iMax: mx((r) => r.iMax), iAvg: iMean,
    iP10: pct(is, 0.1), iMedian: pct(is, 0.5), iP99: pct(is, 0.99),
    pAvg: energy / tw, pMax: mx((r) => r.vMax * r.iMax),
    chargeC: charge, energyJ: energy, energyEstimated: !exact,
    socStart: first.soc, socEnd: last.soc,
    socRatePerDay: span > 0 ? ((first.soc - last.soc) / span) * 86400 : NaN,
    projectedLifeS: iMean > 0 ? capC / iMean : Infinity,
    remainingAtAvgS: iMean > 0 ? ((last.soc / 100) * capC) / iMean : Infinity,
    gaps: rs.filter((r) => r.flags & RF_GAP).length, clamped: rs.filter((r) => r.flags & RF_I_CLAMP).length,
  };
}

export interface View {
  t: number[]; vMin: number[]; vAvg: number[]; vMax: number[];
  iMin: number[]; iAvg: number[]; iMax: number[]; pAvg: number[]; soc: number[]; tier: number[];
}

/** Min/max envelope + dt-weighted means per bucket; raw points when they fit. */
export function view(h: History, t0: number, t1: number, buckets: number): View {
  const [lo, hi] = windowRange(h, t0, t1);
  const rs = h.recs.slice(lo, hi);
  const v: View = { t: [], vMin: [], vAvg: [], vMax: [], iMin: [], iAvg: [], iMax: [], pAvg: [], soc: [], tier: [] };
  const push = (t: number, r: Rec, p: number) => {
    v.t.push(t); v.vMin.push(r.vMin); v.vAvg.push(r.vAvg); v.vMax.push(r.vMax);
    v.iMin.push(r.iMin); v.iAvg.push(r.iAvg); v.iMax.push(r.iMax); v.pAvg.push(p); v.soc.push(r.soc); v.tier.push(r.tier);
  };
  const n = Math.max(16, Math.min(8000, Math.floor(buckets)));
  if (rs.length <= n) {
    for (const r of rs) push(r.t - r.dt / 2, r, r.vAvg * r.iAvg);
    return v;
  }
  const w = (t1 - t0) / n;
  let i = 0;
  for (let b = 0; b < n; b++) {
    const be = t0 + w * (b + 1);
    const acc: Rec = { t: 0, dt: 0, vAvg: 0, vMin: Infinity, vMax: -Infinity, soc: 0, iAvg: 0, iMin: Infinity, iMax: -Infinity, flags: 0, qDut: null, qUsed: 0, eDut: null, tier: 0 };
    let tw = 0, ew = 0, cnt = 0;
    while (i < rs.length && rs[i]!.t - rs[i]!.dt / 2 < be) {
      const r = rs[i]!;
      acc.vMin = Math.min(acc.vMin, r.vMin); acc.vMax = Math.max(acc.vMax, r.vMax);
      acc.iMin = Math.min(acc.iMin, r.iMin); acc.iMax = Math.max(acc.iMax, r.iMax);
      acc.vAvg += r.vAvg * r.dt; acc.iAvg += r.iAvg * r.dt; ew += r.vAvg * r.iAvg * r.dt;
      acc.soc = r.soc; acc.tier = Math.max(acc.tier, r.tier); tw += r.dt; cnt++; i++;
    }
    if (!cnt || tw <= 0) continue;
    acc.vAvg /= tw; acc.iAvg /= tw;
    push(be - w / 2, acc, ew / tw);
  }
  return v;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
export function fmtSi(v: number, unit: string): string {
  if (!Number.isFinite(v)) return "-";
  const a = Math.abs(v);
  const [scale, p] = a === 0 ? [1, ""] : a < 1e-6 ? [1e9, "n"] : a < 1e-3 ? [1e6, "\u00b5"] : a < 1 ? [1e3, "m"] : a < 1e3 ? [1, ""] : [1e-3, "k"];
  const x = v * (scale as number);
  const digits = Math.abs(x) >= 100 ? 1 : Math.abs(x) >= 10 ? 2 : 3;
  return `${x.toFixed(digits)} ${p}${unit}`;
}

export function fmtDuration(s: number): string {
  if (!Number.isFinite(s) || s < 0) return "-";
  s = Math.round(s);
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60, sec = s % 60;
  const p2 = (n: number) => String(n).padStart(2, "0");
  if (d > 0) return `${d}d ${h}h ${p2(m)}m`;
  if (h > 0) return `${h}h ${p2(m)}m`;
  if (m > 0) return `${m}m ${p2(sec)}s`;
  return `${sec}s`;
}

const TIME_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400, 172800, 604800, 1209600, 2592000];
export function timeStep(span: number, target: number): number {
  const raw = span / Math.max(1, target);
  return TIME_STEPS.find((s) => s >= raw) ?? 2592000;
}

export function fmtElapsedTick(t: number, step: number): string {
  t = Math.max(0, Math.round(t));
  const d = Math.floor(t / 86400), h = Math.floor(t / 3600) % 24, m = Math.floor(t / 60) % 60, s = t % 60;
  const p2 = (n: number) => String(n).padStart(2, "0");
  if (step >= 86400) return `${d}d`;
  if (step >= 3600) return d > 0 ? `${d}d ${p2(h)}h` : `${h}h`;
  if (step >= 60) return d > 0 ? `${d}d ${p2(h)}:${p2(m)}` : `${p2(h)}:${p2(m)}`;
  return d > 0 || h > 0 ? `${d * 24 + h}:${p2(m)}:${p2(s)}` : `${p2(m)}:${p2(s)}`;
}
