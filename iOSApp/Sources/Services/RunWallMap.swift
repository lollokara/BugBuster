import Foundation

// =============================================================================
// RunWallMap.swift - run time (simulated seconds, only advances while ACTIVE) <-> unix time.
// Mirrors hub_wallmap_* in Firmware/ESP32/src/hub/hub_runs.c so the app and the S3 agree on
// every sample's timestamp. START events (code 2) carry the wall epoch in `b`.
// =============================================================================

struct RunWallMap: Equatable {
    struct Seg: Equatable { var t: Double; var wall: Double }
    var created: Double
    var segs: [Seg] = []

    init(created: Double, events: [(t: Double, code: Int, b: Int)]) {
        self.created = created
        segs = events.filter { $0.code == 2 && $0.b != 0 }.map { Seg(t: $0.t, wall: Double($0.b)) }
    }

    /// Unix time of an interval END at run time t (last START strictly before it applies).
    func unix(_ t: Double) -> Double {
        guard let s = segs.last(where: { $0.t < t }) else { return created + t }
        return s.wall + (t - s.t)
    }

    /// Run time of a unix time inside an ACTIVE segment.
    func runTime(_ unix: Double) -> Double {
        guard let s = segs.last(where: { $0.wall <= unix }) else { return max(unix - created, 0) }
        return s.t + (unix - s.wall)
    }

    /// Clock source and uncertainty in milliseconds for an interval ending at run time t.
    func source(_ t: Double) -> (src: String, uncMs: Int) {
        if let s = segs.last(where: { $0.t < t }) {
            return ("P4_EPOCH", 200 + Int((t - s.t) * 0.05))
        }
        if created > 0 {
            return ("P4_EPOCH", 200 + Int(t * 0.05))
        }
        return ("EST", 3_600_000)
    }
}
