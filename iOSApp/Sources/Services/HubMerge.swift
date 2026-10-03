import Foundation

// =============================================================================
// HubMerge.swift - hub-first battery data (spec 2026-10-03 section 6, iOS).
// Foundation only so it runs on the host toolchain: tests/unit/test_hub_merge.py.
//
// Conventions: every point covers (t - dt, t], t = unix seconds at the interval END,
// the same as the device records and the hub's sample rows.
// =============================================================================

enum DataSource: String { case hub = "Hub", device = "Device" }

struct SeriesPoint: Equatable {
    var t: Double
    var dt: Double
    var vAvg: Double, vMin: Double, vMax: Double
    var iAvg: Double, iMin: Double, iMax: Double
    var soc: Double
    var res: Int                 // source resolution in seconds (1, 60, 900)
    var source: DataSource
}

enum HubError: Error, Equatable {
    case decode(String)
    case http(Int)
    case unreachable
}

struct HubRunKey: Equatable {
    let mac: String              // 12 lowercase hex
    let runId: Int
    let createdEpoch: Int
}

enum HubRunUid {
    /// `<mac12>-<run_id>-<start epoch>`; nil while the run has no valid creation time (device clock unset).
    static func make(mac: String, runId: Int, createdEpoch: Int) -> String? {
        let lower = mac.lowercased()
        guard lower.allSatisfy({ $0.isHexDigit || $0 == ":" || $0 == "-" }) else { return nil }
        let hex = lower.filter { $0.isHexDigit }
        guard hex.count == 12, (0...65535).contains(runId), createdEpoch > 0 else { return nil }
        return "\(hex)-\(runId)-\(createdEpoch)"
    }

    static func parse(_ uid: String) -> HubRunKey? {
        let p = uid.split(separator: "-", omittingEmptySubsequences: false)
        guard p.count == 3, p[0].count == 12,
              p[0].allSatisfy({ $0.isHexDigit && !$0.isUppercase }),
              let r = Int(p[1]), let e = Int(p[2]), (0...65535).contains(r), e > 0 else { return nil }
        return HubRunKey(mac: String(p[0]), runId: r, createdEpoch: e)
    }
}

/// `GET /api/v1/runs/{uid}/samples` decoded. A hole marker (`null` v_avg) is dropped.
struct HubSeries: Equatable {
    var bucket: Int
    var points: [SeriesPoint]

    static func decode(_ data: Data) throws -> HubSeries {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let t = o["t"] as? [Any], let bucket = (o["bucket"] as? NSNumber)?.intValue else {
            throw HubError.decode("run samples")
        }
        func col(_ key: String) -> [Double?] {
            (o[key] as? [Any] ?? []).map { ($0 as? NSNumber)?.doubleValue }
        }
        func at(_ a: [Double?], _ i: Int) -> Double? { i < a.count ? a[i] : nil }
        let vMin = col("v_min"), vAvg = col("v_avg"), vMax = col("v_max")
        let iMin = col("i_min"), iAvg = col("i_avg"), iMax = col("i_max"), soc = col("soc"), res = col("res")
        var pts: [SeriesPoint] = []
        for (k, raw) in t.enumerated() {
            guard let start = (raw as? NSNumber)?.doubleValue, let v = at(vAvg, k) else { continue }
            let r = Int(at(res, k) ?? 1)
            let span = Double(max(bucket, r))
            pts.append(SeriesPoint(
                t: start + span, dt: span,
                vAvg: v, vMin: at(vMin, k) ?? v, vMax: at(vMax, k) ?? v,
                iAvg: at(iAvg, k) ?? 0, iMin: at(iMin, k) ?? at(iAvg, k) ?? 0, iMax: at(iMax, k) ?? at(iAvg, k) ?? 0,
                soc: at(soc, k) ?? 0, res: r, source: .hub))
        }
        return HubSeries(bucket: bucket, points: pts)
    }
}

/// `GET /api/v1/runs/{uid}/coverage`.
struct HubCoverage: Equatable {
    struct Range: Equatable { var from: Double; var to: Double; var res: Int }
    var ranges: [Range]

    static func decode(_ data: Data) throws -> HubCoverage {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let rs = o["ranges"] as? [[String: Any]] else { throw HubError.decode("coverage") }
        return HubCoverage(ranges: rs.compactMap {
            guard let f = ($0["from"] as? NSNumber)?.doubleValue, let t = ($0["to"] as? NSNumber)?.doubleValue,
                  let r = ($0["res"] as? NSNumber)?.intValue else { return nil }
            return Range(from: f, to: t, res: r)
        })
    }
}

enum HubMerge {
    typealias Span = (from: Double, to: Double)

    /// Union of the (t - dt, t] intervals; intervals closer than `tol` seconds are joined.
    static func union(_ pts: [SeriesPoint], tol: Double = 0.5) -> [Span] {
        let iv = pts.map { (from: $0.t - $0.dt, to: $0.t) }.sorted { $0.from < $1.from }
        var out: [Span] = []
        for s in iv {
            if let last = out.last, s.from <= last.to + tol { out[out.count - 1].to = max(last.to, s.to) }
            else { out.append(s) }
        }
        return out
    }

    /// The parts of (a, b] not inside `cover` (sorted, disjoint).
    static func subtract(_ a: Double, _ b: Double, _ cover: [Span]) -> [Span] {
        var cur = a
        var out: [Span] = []
        for c in cover where c.to > cur && c.from < b {
            if c.from > cur { out.append((cur, min(c.from, b))) }
            cur = max(cur, c.to)
            if cur >= b { break }
        }
        if cur < b { out.append((cur, b)) }
        return out
    }

    /// Ranges inside (from, to] the hub does not cover. Holes narrower than 2.5 x the expected
    /// cadence are measurement jitter, not gaps.
    static func gaps(hub: [SeriesPoint], from: Double, to: Double, expected: Double) -> [Span] {
        subtract(from, to, union(hub)).filter { $0.to - $0.from >= 2.5 * expected }
    }

    /// Hub points win; each device point is cut down to the part the hub does not cover, so there is
    /// no overlap, no duplicate timestamp and no hole where the device has data. Sorted by t.
    static func merge(hub: [SeriesPoint], device: [SeriesPoint]) -> [SeriesPoint] {
        if hub.isEmpty { return device.sorted { $0.t < $1.t } }
        let cover = union(hub)
        var out = hub
        for d in device {
            for s in subtract(d.t - d.dt, d.t, cover) where s.to - s.from >= 0.5 {
                var p = d
                p.t = s.to
                p.dt = s.to - s.from
                out.append(p)
            }
        }
        return out.sorted { $0.t < $1.t }
    }

    static func resLabel(_ seconds: Int) -> String {
        seconds < 60 ? "\(seconds) s" : (seconds < 3600 ? "\(seconds / 60) min" : "\(seconds / 3600) h")
    }

    /// "Hub 1 s", "Device 1 min" or "Hub 1 s + Device 15 min": each source with its dominant resolution.
    static func indicator(_ pts: [SeriesPoint]) -> String {
        var parts: [String] = []
        for src in [DataSource.hub, DataSource.device] {
            var weight: [Int: Double] = [:]
            for p in pts where p.source == src { weight[p.res, default: 0] += p.dt }
            if let best = weight.max(by: { $0.value < $1.value }) { parts.append("\(src.rawValue) \(resLabel(best.key))") }
        }
        return parts.joined(separator: " + ")
    }
}
