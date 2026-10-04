import Foundation

// Hub-first data for the Battery view (spec 2026-10-03 section 6). Device records are in run time,
// hub points in unix time: RunWallMap converts, HubMerge merges. Never throws: a hub failure means
// "device only", which is the normal state while the phone sits on the DAQ hotspot.
enum BattSimHub {
    static func res(forTier tier: Int) -> Int { [900, 60, 1][tier % 3] }
    static func tier(forRes res: Int) -> Int { res <= 1 ? 2 : (res <= 60 ? 1 : 0) }

    static func wallMap(_ h: BsHistory) -> RunWallMap {
        RunWallMap(created: Double(h.meta.createdEpoch), events: h.events.map { ($0.t, $0.code, $0.b) })
    }

    static func points(_ recs: [BsRec], map: RunWallMap) -> [SeriesPoint] {
        recs.map {
            let (src, unc) = map.source($0.t)
            return SeriesPoint(t: map.unix($0.t), dt: $0.dt, vAvg: $0.vAvg, vMin: $0.vMin, vMax: $0.vMax,
                               iAvg: $0.iAvg, iMin: $0.iMin, iMax: $0.iMax, soc: $0.soc,
                               res: res(forTier: $0.tier), source: .device,
                               clkSrc: src, clkUncMs: unc)
        }
    }

    /// Back to run time. A device point keeps its stored charge/energy when it still ends where the
    /// original record did; a hub point (or a device point cut at its end) carries none.
    static func recs(_ pts: [SeriesPoint], device: [BsRec], map: RunWallMap) -> [BsRec] {
        pts.map { p in
            let t = map.runTime(p.t)
            if p.source == .device, let d = device.first(where: { abs($0.t - t) < 0.5 }) {
                var r = d
                r.dt = min(r.dt, p.dt)
                return r
            }
            return BsRec(t: t, dt: p.dt, vAvg: p.vAvg, vMin: p.vMin, vMax: p.vMax, soc: p.soc,
                         iAvg: p.iAvg, iMin: p.iMin, iMax: p.iMax, flags: 0, qDut: nil, qUsed: 0, eDut: nil,
                         tier: tier(forRes: p.res))
        }
    }

    /// `window` is in run time. Returns the (possibly merged) history and the source indicator.
    @MainActor
    static func merged(_ h: BsHistory, mac: String, window: ClosedRange<Double>, hub: HubClient) async -> (BsHistory, String) {
        let map = wallMap(h)
        let device = points(h.recs, map: map)
        let deviceOnly = (h, HubMerge.indicator(device))
        guard let uid = HubRunUid.make(mac: mac, runId: h.meta.runId, createdEpoch: h.meta.createdEpoch),
              !hub.isKnownUnreachable else { return deviceOnly }
        do {
            guard let ext = try await hub.extent(uid: uid) else { return deviceOnly }
            let from = max(map.unix(window.lowerBound), ext.first - 1)
            let to = min(map.unix(window.upperBound), ext.last + 1)
            guard to > from else { return deviceOnly }
            let bucket = max(1, Int(((to - from) / 1500).rounded(.up)))       // ~1500 points: what the chart can show
            let series = try await hub.series(uid: uid, from: from, to: to, bucket: bucket)
            let merged = HubMerge.merge(hub: series.points, device: device)
            var out = h
            out.recs = recs(merged, device: h.recs, map: map)
            return (out, HubMerge.indicator(merged))
        } catch {
            return deviceOnly                                                    // unreachable / 5xx / decode: silently device-only
        }
    }
}
