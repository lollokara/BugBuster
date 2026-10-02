// Battery-sim history decoder: v2 exact current/energy, v1 estimate, tier merge, decimation.
import { describe, expect, it } from "vitest";
import { buildHistory, fmtElapsedTick, parseStatus, stats, timeStep, view } from "./history";

function meta(version: number): Uint8Array {
  const b = new Uint8Array(68);
  const d = new DataView(b.buffer);
  d.setUint32(0, 0x4e525342, true);
  d.setUint16(4, version, true);
  d.setUint16(6, 7, true);
  d.setUint32(44, 2000, true); // params.capacity_mah
  return b;
}

function recV2(t: number, dt: number, qNc: number, eUj: number): Uint8Array {
  const b = new Uint8Array(48);
  const d = new DataView(b.buffer);
  d.setUint32(0, t, true);
  d.setUint16(4, 3700, true); d.setUint16(6, 3695, true); d.setUint16(8, 3705, true);
  d.setUint16(10, 9000, true);
  d.setInt32(12, 9_000_000, true); d.setInt32(16, 11_000_000, true);
  d.setUint16(22, dt, true);
  d.setBigInt64(24, BigInt(qNc), true); d.setBigInt64(32, BigInt(qNc), true);
  d.setBigInt64(40, BigInt(eUj), true);
  return b;
}

const concat = (parts: Uint8Array[]) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
};

describe("battsim history", () => {
  it("derives exact current and energy from v2 cumulative integrals", () => {
    const m1 = concat(Array.from({ length: 100 }, (_, k) => recV2(60 * (k + 1), 60, 600_000_000 * (k + 1), 2_220_000 * (k + 1))));
    const h = buildHistory({ "meta.bin": meta(2), "m0000.bin": m1 });
    expect(h.recs).toHaveLength(100);
    for (const r of h.recs) expect(r.iAvg).toBeCloseTo(0.01, 9);
    const s = stats(h)!;
    expect(s.energyJ).toBeCloseTo(222, 6);
    expect(s.chargeC).toBeCloseTo(60, 6);
    expect(s.energyEstimated).toBe(false);
    expect(stats(h, 600, 1200)!.points).toBe(10);
  });

  it("decimates to a min/max envelope", () => {
    const m1 = concat(Array.from({ length: 2000 }, (_, k) => recV2(60 * (k + 1), 60, 600_000_000 * (k + 1), 0)));
    const h = buildHistory({ "meta.bin": meta(2), "m0000.bin": m1 });
    const v = view(h, 0, 120000, 100);
    expect(v.t).toHaveLength(100);
    expect(v.iMax[0]).toBeCloseTo(0.011, 9);
  });

  it("prefers the 1-min tier over the 15-min tier where both exist", () => {
    const q15 = concat([1, 2, 3, 4].map((k) => recV2(900 * k, 900, k, k)));
    const m1 = concat(Array.from({ length: 15 }, (_, k) => recV2(2700 + 60 * (k + 1), 60, k, k)));
    const h = buildHistory({ "meta.bin": meta(2), "q15.bin": q15, "m0000.bin": m1 });
    expect(h.recs.map((r) => r.tier)).toEqual([0, 0, 0, ...Array(15).fill(1)]);
  });

  it("decodes the status block", () => {
    const b = new Uint8Array(88);
    const d = new DataView(b.buffer);
    b[1] = 2; b[2] = 0x40;
    d.setUint16(12, 8123, true);
    d.setUint32(20, 7200, true);
    const s = parseStatus(b);
    expect(s.state).toBe(2);
    expect(s.remainingS).toBe(7200);
    expect(s.socPct).toBeCloseTo(81.23, 6);
  });

  it("labels the time axis by tick step", () => {
    expect(timeStep(30 * 86400, 10)).toBe(604800);
    expect(fmtElapsedTick(3 * 86400, 86400)).toBe("3d");
    expect(fmtElapsedTick(90061, 3600)).toBe("1d 01h");
    expect(fmtElapsedTick(330, 60)).toBe("00:05");
  });
});
