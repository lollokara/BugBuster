import CoreTransferable
import Foundation
import UniformTypeIdentifiers

// Battery simulator (DAQ HAT P4) host access: wire decoders, run-file mirror,
// merged history, decimated views and window statistics.
// Mirrors python/bugbuster/battsim.py (reference decoder) and transports over
// /api/daq/bs, /api/daq/bs/read and /api/daq/config (HTTP or the BLE tunnel).

struct BsParams: Equatable {
    var chem = 0, cells = 1, capacityMah = 0, cutoffMvCell = 0, rintUohmCell = 0, extLoadUa = 0
    var sdEnable = false, extEnable = false, dither = false
    var startSocPct = 100.0, peukert = 1.0, sdPctMonth = 0.0
}

struct BsStatus: Equatable {
    var state = 0, flags = 0, lastError = 0, runId = 0, chem = 0, cells = 0, capacityMah = 0
    var socPct = 0.0, elapsedS = 0, remainingS: Int?, provisional = false
    var vMeas = 0.0, iMeas = 0.0, vTarget = 0.0, iAvg = 0.0, fsTotal = 0, fsUsed = 0
    var eDutJ: Double?
}

struct BsMeta: Equatable {
    var runId = 0, version = 1, createdEpoch = 0
    var name = ""
    var params = BsParams()
}

struct BsEvent: Equatable, Identifiable {
    let id = UUID()
    var t: Double, code: Int, a: Int, b: Int
    var name: String { BattSim.eventNames[code] ?? "event" }
}

struct BsRec {
    var t: Double, dt: Double, vAvg: Double, vMin: Double, vMax: Double, soc: Double
    var iAvg: Double, iMin: Double, iMax: Double, flags: Int
    var qDut: Double?, qUsed: Double, eDut: Double?, tier: Int
}

struct BsRunSummary: Identifiable, Equatable {
    var id: Int { runId }
    var runId: Int, active: Bool, meta: BsMeta?, bytes: Int
}

struct BsHistory {
    var meta: BsMeta
    var events: [BsEvent]
    var recs: [BsRec]
    var bounds: ClosedRange<Double> {
        guard let f = recs.first, let l = recs.last else { return 0...1 }
        return (f.t - f.dt)...max(l.t, f.t - f.dt + 60)
    }
}

struct BsView {
    var t: [Double] = [], dt: [Double] = [], vMin: [Double] = [], vAvg: [Double] = [], vMax: [Double] = []
    var iMin: [Double] = [], iAvg: [Double] = [], iMax: [Double] = [], pAvg: [Double] = []
    var soc: [Double] = [], tier: [Int] = []
}

struct BsStats {
    var points = 0, tStart = 0.0, duration = 0.0
    var vMin = 0.0, vMax = 0.0, vAvg = 0.0, iMin = 0.0, iMax = 0.0, iAvg = 0.0
    var iP10 = 0.0, iMedian = 0.0, iP99 = 0.0, pAvg = 0.0, pMax = 0.0
    var chargeC = 0.0, energyJ = 0.0, energyEstimated = false
    var socStart = 0.0, socEnd = 0.0, socRatePerDay = 0.0, projectedLifeS = 0.0, remainingAtAvgS = 0.0
    var gaps = 0, clamped = 0
}

enum BattSimError: LocalizedError {
    case transport(String), decode(String)
    var errorDescription: String? {
        switch self { case .transport(let s), .decode(let s): return s }
    }
}

// MARK: - Little-endian reader

private struct LE {
    let d: Data
    init(_ d: Data) { self.d = Data(d) }   // rebase to index 0
    var count: Int { d.count }
    func u8(_ o: Int) -> Int { Int(d[o]) }
    func u16(_ o: Int) -> Int { Int(d[o]) | Int(d[o + 1]) << 8 }
    func u32(_ o: Int) -> Int { u16(o) | u16(o + 2) << 16 }
    func i32(_ o: Int) -> Int { Int(Int32(bitPattern: UInt32(u32(o)))) }
    func i64(_ o: Int) -> Int64 {
        var v: UInt64 = 0
        for k in 0..<8 { v |= UInt64(d[o + k]) << (8 * UInt64(k)) }
        return Int64(bitPattern: v)
    }
    func f32(_ o: Int) -> Double { Double(Float(bitPattern: UInt32(u32(o)))) }
}

enum BattSim {
    static let stateNames = ["No run", "Paused", "Active", "Depleted", "Stopped"]
    static let chemNames = ["LiPo", "LiFePO4", "NiMH", "Lead-acid"]
    static let errorText = ["", "battlog partition missing", "no run loaded", "busy", "invalid parameters",
                            "not allowed in this state", "USB-PD contract below 9 V / 3 A",
                            "acquisition not running", "flash I/O error", "run not found", "run depleted"]
    static let eventNames: [Int: String] = [1: "created", 2: "start", 3: "pause", 4: "stop", 5: "depleted",
                                            6: "reboot", 7: "param", 8: "stall", 9: "output off",
                                            10: "PD lost", 11: "store error", 12: "reopen"]
    enum Action: Int { case defaults = 14, newRun = 7, start = 8, pause = 9, stop = 10, unload = 11, load = 12, delete = 13, reopen = 15 }

    // MARK: Decoders

    static func parseParams(_ d: Data) -> BsParams {
        let b = LE(d)
        guard b.count >= 28 else { return BsParams() }
        return BsParams(chem: b.u8(0), cells: b.u8(1), capacityMah: b.u32(8), cutoffMvCell: b.u16(14),
                        rintUohmCell: b.u32(16), extLoadUa: b.u32(24), sdEnable: b.u8(2) != 0,
                        extEnable: b.u8(3) != 0, dither: b.u8(4) != 0,
                        startSocPct: Double(b.u16(12)) / 10, peukert: Double(b.u16(20)) / 1000,
                        sdPctMonth: Double(b.u16(22)) / 100)
    }

    static func parseStatus(_ d: Data) throws -> BsStatus {
        let b = LE(d)
        guard b.count >= 88 else { throw BattSimError.decode("short battsim status (\(b.count) B)") }
        let flags = b.u8(2)
        return BsStatus(state: b.u8(1), flags: flags, lastError: b.u8(3), runId: b.u16(4), chem: b.u8(6),
                        cells: b.u8(7), capacityMah: b.u32(8), socPct: Double(b.u16(12)) / 100,
                        elapsedS: b.u32(16), remainingS: flags & 0x40 != 0 ? b.u32(20) : nil,
                        provisional: flags & 0x01 != 0, vMeas: b.f32(28), iMeas: b.f32(32),
                        vTarget: b.f32(24), iAvg: b.f32(36), fsTotal: b.u32(80), fsUsed: b.u32(84),
                        eDutJ: b.count >= 96 ? Double(b.i64(88)) * 1e-6 : nil)
    }

    static func parseMeta(_ d: Data) throws -> BsMeta {
        let b = LE(d)
        guard b.count >= 68, b.u32(0) == 0x4E52_5342 else { throw BattSimError.decode("bad run meta") }
        let nameBytes = b.d.subdata(in: 12..<36).prefix { $0 != 0 }
        return BsMeta(runId: b.u16(6), version: b.u16(4), createdEpoch: b.u32(8),
                      name: String(decoding: nameBytes, as: UTF8.self), params: parseParams(b.d.subdata(in: 36..<64)))
    }

    static func parseEvents(_ d: Data) -> [BsEvent] {
        let b = LE(d)
        return stride(from: 0, to: b.count - 15, by: 16).map {
            BsEvent(t: Double(b.u32($0)), code: b.u16($0 + 4), a: b.i32($0 + 8), b: b.i32($0 + 12))
        }
    }

    static func parseRecords(_ d: Data, version: Int, nominalDt: Double, tier: Int) -> [BsRec] {
        let b = LE(d)
        var out: [BsRec] = []
        if version == 1 {
            var prev: Double?
            for o in stride(from: 0, to: b.count - 31, by: 32) {
                let t = Double(b.u32(o))
                let dt = (prev.map { t > $0 ? t - $0 : nil } ?? nil) ?? min(t, nominalDt)
                out.append(BsRec(t: t, dt: dt, vAvg: Double(b.u16(o + 4)) / 1e3, vMin: Double(b.u16(o + 6)) / 1e3,
                                 vMax: Double(b.u16(o + 8)) / 1e3, soc: Double(b.u16(o + 10)) / 100,
                                 iAvg: Double(b.i32(o + 12)) * 1e-9, iMin: Double(b.i32(o + 16)) * 1e-9,
                                 iMax: Double(b.i32(o + 20)) * 1e-9, flags: 0, qDut: nil,
                                 qUsed: Double(b.i64(o + 24)) * 1e-9, eDut: nil, tier: tier))
                prev = t
            }
        } else if version == 2 {
            for o in stride(from: 0, to: b.count - 47, by: 48) {
                out.append(BsRec(t: Double(b.u32(o)), dt: Double(b.u16(o + 22)), vAvg: Double(b.u16(o + 4)) / 1e3,
                                 vMin: Double(b.u16(o + 6)) / 1e3, vMax: Double(b.u16(o + 8)) / 1e3,
                                 soc: Double(b.u16(o + 10)) / 100, iAvg: 0, iMin: Double(b.i32(o + 12)) * 1e-9,
                                 iMax: Double(b.i32(o + 16)) * 1e-9, flags: b.u16(o + 20),
                                 qDut: Double(b.i64(o + 24)) * 1e-9, qUsed: Double(b.i64(o + 32)) * 1e-9,
                                 eDut: Double(b.i64(o + 40)) * 1e-6, tier: tier))
            }
        }
        return out
    }

    static func buildHistory(_ files: [String: Data]) throws -> BsHistory {
        let meta = try parseMeta(files["meta.bin"] ?? Data())
        var q15Dt = 900.0
        if let ck = files["ckpt.bin"], ck.count >= 352, LE(ck).u32(0) == 0x4B43_5342 {
            q15Dt = Double(max(LE(ck).u32(336), 1))
        }
        let q15 = parseRecords(files["q15.bin"] ?? Data(), version: meta.version, nominalDt: q15Dt, tier: 0)
        var m1: [BsRec] = []
        for name in files.keys.filter({ $0.range(of: #"^m\d{4}\.bin$"#, options: .regularExpression) != nil }).sorted() {
            m1 += parseRecords(files[name]!, version: meta.version, nominalDt: 60, tier: 1)
        }
        let s1 = parseRecords(files["s1.bin"] ?? Data(), version: meta.version, nominalDt: 1, tier: 2)
        // s1 is only dumped at pause/stop, so while a run is live it can sit inside m1:
        // keep coarser records on both sides of each finer tier.
        func outside(_ recs: [BsRec], _ fine: [BsRec]) -> [BsRec] {
            guard let a = fine.first, let b = fine.last else { return recs }
            return recs.filter { $0.t <= a.t - a.dt || $0.t - $0.dt >= b.t }
        }
        var recs = (outside(outside(q15, m1), s1) + outside(m1, s1) + s1).sorted { $0.t < $1.t }
        if meta.version >= 2 {
            var prev: BsRec?
            for i in recs.indices {
                guard let q = recs[i].qDut, recs[i].dt > 0 else { prev = recs[i]; continue }
                let start = recs[i].t - recs[i].dt
                if let p = prev, let pq = p.qDut, abs(p.t - start) <= 1 { recs[i].iAvg = (q - pq) / recs[i].dt }
                else if start <= 0 { recs[i].iAvg = q / recs[i].dt }
                else { recs[i].iAvg = (recs[i].iMin + recs[i].iMax) / 2 }
                prev = recs[i]
            }
        }
        return BsHistory(meta: meta, events: parseEvents(files["ev.bin"] ?? Data()), recs: recs)
    }

    // MARK: Views and statistics

    static func range(_ h: BsHistory, _ t0: Double, _ t1: Double) -> Range<Int> {
        func lb(_ pred: (BsRec) -> Bool) -> Int {
            var lo = 0, hi = h.recs.count
            while lo < hi { let m = (lo + hi) / 2; if pred(h.recs[m]) { lo = m + 1 } else { hi = m } }
            return lo
        }
        let lo = lb { $0.t <= t0 }
        return lo..<max(lo, lb { $0.t - $0.dt < t1 })
    }

    static func view(_ h: BsHistory, _ t0: Double, _ t1: Double, buckets: Int) -> BsView {
        let rs = h.recs[range(h, t0, t1)]
        var v = BsView()
        func push(_ t: Double, _ r: BsRec, _ p: Double, _ dt: Double) {
            v.t.append(t); v.dt.append(dt); v.vMin.append(r.vMin); v.vAvg.append(r.vAvg); v.vMax.append(r.vMax)
            v.iMin.append(r.iMin); v.iAvg.append(r.iAvg); v.iMax.append(r.iMax); v.pAvg.append(p)
            v.soc.append(r.soc); v.tier.append(r.tier)
        }
        let n = min(max(buckets, 16), 4000)
        if rs.count <= n {
            for r in rs { push(r.t - r.dt / 2, r, r.vAvg * r.iAvg, r.dt) }
            return v
        }
        let w = (t1 - t0) / Double(n)
        var i = rs.startIndex
        for b in 0..<n {
            let be = t0 + w * Double(b + 1)
            var acc = BsRec(t: 0, dt: 0, vAvg: 0, vMin: .infinity, vMax: -.infinity, soc: 0, iAvg: 0,
                            iMin: .infinity, iMax: -.infinity, flags: 0, qDut: nil, qUsed: 0, eDut: nil, tier: 0)
            var tw = 0.0, ew = 0.0, cnt = 0, lo = Double.infinity, hi = -Double.infinity
            while i < rs.endIndex && rs[i].t - rs[i].dt / 2 < be {
                let r = rs[i]
                lo = min(lo, r.t - r.dt); hi = max(hi, r.t)
                acc.vMin = min(acc.vMin, r.vMin); acc.vMax = max(acc.vMax, r.vMax)
                acc.iMin = min(acc.iMin, r.iMin); acc.iMax = max(acc.iMax, r.iMax)
                acc.vAvg += r.vAvg * r.dt; acc.iAvg += r.iAvg * r.dt; ew += r.vAvg * r.iAvg * r.dt
                acc.soc = r.soc; acc.tier = max(acc.tier, r.tier); tw += r.dt; cnt += 1; i += 1
            }
            guard cnt > 0, tw > 0 else { continue }
            acc.vAvg /= tw; acc.iAvg /= tw
            // Span the records actually covered, so sparse data stays continuous instead of w-wide dots.
            push((lo + hi) / 2, acc, ew / tw, hi - lo)
        }
        return v
    }

    static func stats(_ h: BsHistory, _ t0: Double, _ t1: Double) -> BsStats? {
        let r = range(h, t0, t1)
        let rs = h.recs[r]
        guard let first = rs.first, let last = rs.last else { return nil }
        let tw = max(rs.reduce(0) { $0 + $1.dt }, 1e-9)
        let charge = rs.reduce(0) { $0 + $1.iAvg * $1.dt }
        let exact = h.meta.version >= 2
        let energy = exact
            ? (last.eDut ?? 0) - (r.lowerBound > 0 ? h.recs[r.lowerBound - 1].eDut ?? 0 : 0)
            : rs.reduce(0) { $0 + $1.vAvg * $1.iAvg * $1.dt }
        let sorted = rs.map(\.iAvg).sorted()
        func pct(_ p: Double) -> Double {
            let k = Double(sorted.count - 1) * p
            let lo = Int(k.rounded(.down)), hi = Int(k.rounded(.up))
            return sorted[lo] + (sorted[hi] - sorted[lo]) * (k - Double(lo))
        }
        let span = last.t - (first.t - first.dt)
        let iMean = charge / tw
        let capC = Double(h.meta.params.capacityMah) * 3.6
        var s = BsStats()
        s.points = rs.count; s.tStart = first.t - first.dt; s.duration = span
        s.vMin = rs.map(\.vMin).min() ?? 0; s.vMax = rs.map(\.vMax).max() ?? 0
        s.vAvg = rs.reduce(0) { $0 + $1.vAvg * $1.dt } / tw
        s.iMin = rs.map(\.iMin).min() ?? 0; s.iMax = rs.map(\.iMax).max() ?? 0; s.iAvg = iMean
        s.iP10 = pct(0.10); s.iMedian = pct(0.5); s.iP99 = pct(0.99)
        s.pAvg = energy / tw; s.pMax = rs.map { $0.vMax * $0.iMax }.max() ?? 0
        s.chargeC = charge; s.energyJ = energy; s.energyEstimated = !exact
        s.socStart = first.soc; s.socEnd = last.soc
        s.socRatePerDay = span > 0 ? (first.soc - last.soc) / span * 86400 : 0
        s.projectedLifeS = iMean > 0 ? capC / iMean : .infinity
        s.remainingAtAvgS = iMean > 0 ? last.soc / 100 * capC / iMean : .infinity
        s.gaps = rs.filter { $0.flags & 1 != 0 }.count; s.clamped = rs.filter { $0.flags & 2 != 0 }.count
        return s
    }

    // MARK: Formatting

    /// (start, end, joinsPrevious) per point; a hole longer than half an interval breaks the trace.
    static func stepSegments(_ t: [Double], _ dt: [Double]) -> [(Double, Double, Bool)] {
        var prevEnd = -Double.infinity
        return t.indices.map { i in
            let w = i < dt.count && dt[i] > 0 ? dt[i] : 60
            let a = t[i] - w / 2, b = t[i] + w / 2
            let joined = abs(a - prevEnd) <= w * 0.5
            prevEnd = b
            return (a, b, joined)
        }
    }

    static func si(_ v: Double, _ unit: String) -> String {
        guard v.isFinite else { return "-" }
        let a = abs(v)
        let (scale, p): (Double, String) = a == 0 ? (1, "") : a < 1e-6 ? (1e9, "n") : a < 1e-3 ? (1e6, "\u{b5}")
            : a < 1 ? (1e3, "m") : a < 1e3 ? (1, "") : (1e-3, "k")
        let x = v * scale
        return String(format: abs(x) >= 100 ? "%.1f" : abs(x) >= 10 ? "%.2f" : "%.3f", x) + " \(p)\(unit)"
    }

    static func duration(_ s: Double) -> String {
        guard s.isFinite, s >= 0 else { return "-" }
        let t = Int(s.rounded())
        let d = t / 86400, h = t / 3600 % 24, m = t / 60 % 60, sec = t % 60
        if d > 0 { return "\(d)d \(h)h \(String(format: "%02d", m))m" }
        if h > 0 { return "\(h)h \(String(format: "%02d", m))m" }
        if m > 0 { return "\(m)m \(String(format: "%02d", sec))s" }
        return "\(sec)s"
    }

    static let timeSteps: [Double] = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800,
                                      21600, 43200, 86400, 172800, 604800, 1209600, 2592000]
    static func timeStep(_ span: Double, target: Double) -> Double {
        let raw = span / max(1, target)
        return timeSteps.first { $0 >= raw } ?? 2592000
    }

    static func tickLabel(_ t: Double, step: Double) -> String {
        let s = Int(max(0, t).rounded())
        let d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60, sec = s % 60
        let p2 = { (n: Int) in String(format: "%02d", n) }
        if step >= 86400 { return "\(d)d" }
        if step >= 3600 { return d > 0 ? "\(d)d \(p2(h))h" : "\(h)h" }
        if step >= 60 { return d > 0 ? "\(d)d \(p2(h)):\(p2(m))" : "\(p2(h)):\(p2(m))" }
        return d > 0 || h > 0 ? "\(d * 24 + h):\(p2(m)):\(p2(sec))" : "\(p2(m)):\(p2(sec))"
    }

    /// Wall-clock tick label; dates appear on day boundaries or when ticks are a day apart.
    static func wallLabel(_ unix: Double, step: Double) -> String {
        let d = Date(timeIntervalSince1970: unix)
        let cal = Calendar.current
        if step >= 86400 || (cal.component(.hour, from: d) == 0 && cal.component(.minute, from: d) == 0 && step >= 60) {
            return d.formatted(.dateTime.day().month(.abbreviated))
        }
        return step < 60 ? d.formatted(.dateTime.hour(.twoDigits(amPM: .omitted)).minute().second())
            : d.formatted(.dateTime.hour(.twoDigits(amPM: .omitted)).minute())
    }

    static func wallFull(_ unix: Double) -> String {
        Date(timeIntervalSince1970: unix).formatted(.dateTime.day().month(.abbreviated).hour(.twoDigits(amPM: .omitted)).minute().second())
    }

    // MARK: Export

    static func csv(_ h: BsHistory) -> String {
        var s = "t_s,unix_s,dt_s,v_avg_V,v_min_V,v_max_V,i_avg_A,i_min_A,i_max_A,p_avg_W,soc_pct,tier,flags,q_used_C,e_dut_J\n"
        let ep = Double(h.meta.createdEpoch)
        for r in h.recs {
            s += "\(Int(r.t)),\(ep > 0 ? String(Int(ep + r.t)) : ""),\(Int(r.dt)),\(r.vAvg),\(r.vMin),\(r.vMax),"
            s += "\(r.iAvg),\(r.iMin),\(r.iMax),\(r.vAvg * r.iAvg),\(r.soc),\(r.tier),\(r.flags),\(r.qUsed),\(r.eDut.map { "\($0)" } ?? "")\n"
        }
        return s
    }

    static func json(_ h: BsHistory) -> Data {
        let m = h.meta, p = m.params
        let doc: [String: Any] = [
            "run_id": m.runId, "version": m.version, "name": m.name, "created_epoch": m.createdEpoch,
            "params": ["chem": chemNames.indices.contains(p.chem) ? chemNames[p.chem] : "\(p.chem)", "cells": p.cells, "capacity_mah": p.capacityMah,
                       "start_soc_pct": p.startSocPct, "self_discharge": p.sdEnable, "external_load_ua": p.extEnable ? p.extLoadUa : 0],
            "events": h.events.map { ["t_s": $0.t, "event": $0.name, "a": $0.a, "b": $0.b] },
            "records": h.recs.map { r -> [String: Any] in
                ["t_s": r.t, "dt_s": r.dt, "v_avg": r.vAvg, "v_min": r.vMin, "v_max": r.vMax, "i_avg": r.iAvg,
                 "i_min": r.iMin, "i_max": r.iMax, "soc_pct": r.soc, "tier": r.tier, "flags": r.flags,
                 "q_used_c": r.qUsed, "e_dut_j": r.eDut ?? NSNull()]
            },
        ]
        return (try? JSONSerialization.data(withJSONObject: doc, options: [.prettyPrinted, .sortedKeys])) ?? Data()
    }

    // MARK: Local mirror

    static var cacheRoot: URL? {
        try? FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            .appendingPathComponent("battsim", isDirectory: true)
    }

    static func cachedRuns() -> [BsCachedRun] {
        guard let root = cacheRoot,
              let dirs = try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil) else { return [] }
        return dirs.compactMap { d in
            guard let raw = try? Data(contentsOf: d.appendingPathComponent("meta.bin")), let meta = try? parseMeta(raw) else { return nil }
            let files = (try? FileManager.default.contentsOfDirectory(at: d, includingPropertiesForKeys: [.fileSizeKey])) ?? []
            let bytes = files.reduce(0) { $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
            return BsCachedRun(dir: d, meta: meta, bytes: bytes)
        }
        .sorted { ($0.meta.createdEpoch, $0.meta.runId) > ($1.meta.createdEpoch, $1.meta.runId) }
    }

    static func loadCached(_ dir: URL) -> [String: Data] {
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        var out: [String: Data] = [:]
        for f in files where f.pathExtension == "bin" { out[f.lastPathComponent] = try? Data(contentsOf: f) }
        return out
    }
}

struct BsCachedRun: Identifiable {
    var id: String { dir.path }
    let dir: URL, meta: BsMeta, bytes: Int
}

struct BsExportFile: Transferable {
    let history: BsHistory, csv: Bool
    var fileName: String { "battsim-run\(history.meta.runId).\(csv ? "csv" : "json")" }

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .data) { e in
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(e.fileName)
            try (e.csv ? Data(BattSim.csv(e.history).utf8) : BattSim.json(e.history)).write(to: url, options: .atomic)
            return SentTransferredFile(url)
        }
    }
}

// MARK: - Device client

private struct B64Reply: Decodable { let ok: Bool?; let n: Int?; let data: String?; let error: String? }

@MainActor
final class BattSimClient {
    private let cm: ConnectionManager
    init(_ cm: ConnectionManager) { self.cm = cm }

    private static func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02x", $0) }.joined() }
    private static func le16(_ v: Int) -> [UInt8] { [UInt8(v & 0xFF), UInt8(v >> 8 & 0xFF)] }
    private static func le32(_ v: Int) -> [UInt8] { le16(v & 0xFFFF) + le16(v >> 16 & 0xFFFF) }

    private func post(_ path: String, _ json: [String: Any]) async throws -> Data {
        guard let r: B64Reply = await cm.postJSON(B64Reply.self, path: path, json: json) else {
            throw BattSimError.transport("no reply from \(path)")
        }
        if r.ok == false || r.error != nil { throw BattSimError.transport(r.error ?? "\(path) failed") }
        return Data(base64Encoded: r.data ?? "") ?? Data()
    }

    private func bs(_ op: Int, _ args: [UInt8] = []) async throws -> Data {
        try await post("/api/daq/bs", ["op": op, "args": Self.hex(args)])
    }

    func status() async throws -> BsStatus { try BattSim.parseStatus(try await bs(0)) }
    func setEpoch() async { _ = try? await bs(5, Self.le32(Int(Date().timeIntervalSince1970))) }

    func readRange(run: Int, file: Int, offset: Int, size: Int, progress: ((Int) -> Void)? = nil) async throws -> Data {
        var out = Data()
        // BLE tunnel replies are notify-framed; smaller chunks keep each one well inside the 6 s request timeout.
        let chunk = cm.transport == .ble ? 1024 : 2048
        while out.count < size {
            let want = min(size - out.count, chunk)
            let c = try await post("/api/daq/bs/read", ["run": run, "file": file, "off": offset + out.count, "len": want])
            out.append(c)
            progress?(out.count)
            if c.isEmpty || c.count < want { break }
        }
        return out
    }

    static func fileName(_ id: Int) -> String {
        id >= 0x100 ? String(format: "m%04d.bin", id - 0x100)
            : (["meta.bin", "ckpt.bin", "ev.bin", "q15.bin", "s1.bin"].indices.contains(id)
               ? ["meta.bin", "ckpt.bin", "ev.bin", "q15.bin", "s1.bin"][id] : "f\(id).bin")
    }

    func runDir(_ run: Int) async throws -> [(id: Int, size: Int, name: String)] {
        var files: [(id: Int, size: Int, name: String)] = []
        while true {
            let raw = LE(try await bs(2, Self.le16(run) + [UInt8(files.count & 0xFF)]))
            guard raw.count >= 4 else { throw BattSimError.decode("short run dir") }
            let n = (raw.count - 4) / 6
            for i in 0..<n {
                let id = raw.u16(4 + 6 * i)
                files.append((id, raw.u32(6 + 6 * i), Self.fileName(id)))
            }
            if n == 0 || files.count >= raw.u8(2) { return files }
        }
    }

    func listRuns() async throws -> [BsRunSummary] {
        var ids: [Int] = []
        var active = 0
        while true {
            let raw = LE(try await bs(1, Self.le16(ids.count)))
            guard raw.count >= 4 else { throw BattSimError.decode("short run list") }
            active = raw.u16(2)
            let page = (raw.count - 4) / 2
            for i in 0..<page { ids.append(raw.u16(4 + 2 * i)) }
            if page == 0 || ids.count >= raw.u16(0) { break }
        }
        var out: [BsRunSummary] = []
        for id in ids {
            let meta = try? BattSim.parseMeta(try await readRange(run: id, file: 0, offset: 0, size: 68))
            let bytes = (try? await runDir(id))?.reduce(0) { $0 + $1.size } ?? 0
            out.append(BsRunSummary(runId: id, active: id == active, meta: meta, bytes: bytes))
        }
        return out
    }

    /// Mirror a run into Application Support (only appended bytes are fetched).
    func syncRun(_ run: Int, progress: ((String, Int, Int) -> Void)? = nil) async throws -> [String: Data] {
        let meta = try BattSim.parseMeta(try await readRange(run: run, file: 0, offset: 0, size: 68))
        let root = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                               appropriateFor: nil, create: true)
            .appendingPathComponent("battsim/r\(run)-\(meta.createdEpoch)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let remote = try await runDir(run)
        let total = remote.reduce(0) { $0 + $1.size }
        var base = 0
        var out: [String: Data] = [:]
        for f in remote {
            let url = root.appendingPathComponent(f.name)
            var local = (f.id >= 2 && f.id != 4) ? ((try? Data(contentsOf: url)) ?? Data()) : Data()
            if local.count > f.size { local = Data() }
            else if !local.isEmpty {
                let head = try await readRange(run: run, file: f.id, offset: 0, size: min(local.count, 236))
                if head != local.prefix(head.count) { local = Data() }
            }
            if local.count < f.size {
                let start = local.count, b0 = base
                local.append(try await readRange(run: run, file: f.id, offset: start, size: f.size - start) { n in
                    progress?(f.name, b0 + start + n, total)
                })
                try? local.write(to: url)
            }
            out[f.name] = local
            base += f.size
        }
        return out
    }

    // Settings registry (TLV: key u16, type u8, len u8, value).
    enum CfgType: UInt8 { case bool = 1, u8 = 2, u16 = 4, u32 = 6, enumV = 9, str = 10 }

    func cfgSet(_ key: Int, _ type: CfgType, _ value: Int) async throws {
        let v: [UInt8] = type == .u16 ? Self.le16(value) : type == .u32 ? Self.le32(value) : [UInt8(value & 0xFF)]
        _ = try await post("/api/daq/config", ["op": 1, "args": Self.hex(Self.le16(key) + [type.rawValue, UInt8(v.count)] + v)])
    }

    func cfgSetString(_ key: Int, _ s: String) async throws {
        let v = Array(s.utf8.prefix(23))
        _ = try await post("/api/daq/config", ["op": 1, "args": Self.hex(Self.le16(key) + [CfgType.str.rawValue, UInt8(v.count)] + v)])
    }

    func action(_ a: BattSim.Action, run: Int? = nil) async throws {
        if let run { try await cfgSet(0x080E, .u16, run) }
        _ = try await post("/api/daq/config", ["op": 4, "args": Self.hex([UInt8(a.rawValue)])])
    }
}
