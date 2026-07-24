import SwiftUI

// =============================================================================
// ScopeRenderModel.swift — background render pipeline for the DAQ scope.
//
// Ticks at ~30 Hz on DaqStreamEngine.queue (the same serial queue that owns
// the sample buffers, so no locking on the hot path), reduces the visible
// window of each trace to a small min/max-envelope polyline sized for the
// display, and publishes one ready-to-draw ScopeRenderFrame to SwiftUI.
// The view never touches raw sample arrays and never runs
// sample-count-proportional work in `body`.
//
// Each trace is stored in two tiers (see DaqStreamEngine): an envelope
// `history` buffer followed in time by a raw `recent` buffer. The reducers
// here window and decimate across both segments in one pass.
// =============================================================================

/// One display-ready trace: points are already windowed + decimated to at
/// most ~2×columnBudget entries.
struct RenderedTrace: Identifiable {
    let id: String
    let label: String
    let unit: String
    let defaultColor: Color
    let points: [ScopeSeriesPoint]
    let minV: Double
    let maxV: Double
}

struct ScopeRenderFrame {
    let traces: [RenderedTrace]
    /// Shared bounds across all traces (merged-plot mode), pre-padded.
    let mergedMin: Double
    let mergedMax: Double
    let recordCount: Int
    /// Live end of the reference buffer, for the pan gesture's anchor math.
    let liveEndT: Double
}

final class ScopeRenderModel: ObservableObject, @unchecked Sendable {
    /// Everything the reducer needs from the UI. Written from the main thread
    /// via `updateViewport`, read on the pipeline queue each tick.
    struct Viewport: Equatable {
        var timeScale: CGFloat = 1.0
        var windowSeconds: Double? = nil
        var followLive = true
        var anchorEndT: Double? = nil
        var showVoltage = true
        var showCurrent = true
        var showPower = true
        /// Max envelope columns per trace; roughly the canvas pixel width.
        var columnBudget = 900
    }

    @Published private(set) var frame: ScopeRenderFrame? = nil

    private let lock = NSLock()
    private var viewport = Viewport()
    private var timer: DispatchSourceTimer?
    private weak var engine: DaqStreamEngine?
    private var lastPublishedKey: (Int, Viewport)? = nil

    func updateViewport(_ mutate: (inout Viewport) -> Void) {
        lock.lock()
        mutate(&viewport)
        lock.unlock()
    }

    func currentViewport() -> Viewport {
        lock.lock(); defer { lock.unlock() }
        return viewport
    }

    func start(engine: DaqStreamEngine) {
        stop()
        self.engine = engine
        let t = DispatchSource.makeTimerSource(queue: DaqStreamEngine.queue)
        t.schedule(deadline: .now(), repeating: .milliseconds(33))
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    // MARK: - Segment plumbing

    /// A windowed slice of one storage tier.
    private struct Segment {
        let t: [Double]
        let v: [Float]
        let src: [UInt8]
        var range: Range<Int>
        var count: Int { range.count }
    }

    // MARK: - Tick (pipeline queue)

    private func tick() {
        guard let engine else { return }
        let vp = currentViewport()

        // Skip recompute entirely when neither the data nor the view changed.
        let key = (engine.recordCount, vp)
        if let last = lastPublishedKey, last.0 == key.0, last.1 == key.1 { return }
        lastPublishedKey = key

        // Reference stream for the shared time axis: prefer voltage, fall
        // back to current (mock mode appends both symmetrically).
        let liveEnd = engine.voltage.t.last ?? engine.current.t.last ?? 0
        let end = vp.followLive ? liveEnd : (vp.anchorEndT ?? liveEnd)

        var traces: [RenderedTrace] = []

        var vSegs: [Segment] = []
        if vp.showVoltage || vp.showPower {
            vSegs = Self.windowSegments(hist: engine.voltageHist, recent: engine.voltage,
                                        end: end, windowSeconds: vp.windowSeconds,
                                        scale: vp.timeScale)
        }
        var iSegs: [Segment] = []
        if vp.showCurrent || vp.showPower {
            iSegs = Self.windowSegments(hist: engine.currentHist, recent: engine.current,
                                        end: end, windowSeconds: vp.windowSeconds,
                                        scale: vp.timeScale)
        }

        if vp.showVoltage, !vSegs.isEmpty {
            let pts = Self.decimate(vSegs, columns: vp.columnBudget, useSrc: false,
                                    color: { _ in ScopeColors.daqVoltage })
            traces.append(Self.trace(id: "v", label: "Voltage", unit: "V",
                                     color: ScopeColors.daqVoltage, points: pts))
        }
        if vp.showCurrent, !iSegs.isEmpty {
            let pts = Self.decimate(iSegs, columns: vp.columnBudget, useSrc: true,
                                    color: { ScopeColors.daqCurrentColor(forSource: $0) })
            traces.append(Self.trace(id: "i", label: "Current", unit: "A",
                                     color: ScopeColors.daqCurrentFine, points: pts))
        }
        if vp.showPower, !vSegs.isEmpty, !iSegs.isEmpty {
            // Real timestamp-nearest pairing (voltage and current are
            // independently-timestamped streams): decimate voltage to display
            // columns, then look up the nearest-in-time current sample for
            // each column point.
            let vPts = Self.decimate(vSegs, columns: vp.columnBudget, useSrc: false,
                                     color: { _ in ScopeColors.daqPower })
            var pPts = [ScopeSeriesPoint]()
            pPts.reserveCapacity(vPts.count)
            for p in vPts {
                let i = Self.nearestValue(in: iSegs, at: p.t)
                pPts.append(ScopeSeriesPoint(t: p.t, v: p.v * Double(i), color: ScopeColors.daqPower))
            }
            traces.append(Self.trace(id: "p", label: "Power", unit: "W",
                                     color: ScopeColors.daqPower, points: pPts))
        }

        var mergedMin = Double.infinity
        var mergedMax = -Double.infinity
        for tr in traces {
            mergedMin = min(mergedMin, tr.minV)
            mergedMax = max(mergedMax, tr.maxV)
        }
        if mergedMin == .infinity { mergedMin = -1; mergedMax = 1 }

        let newFrame = ScopeRenderFrame(traces: traces,
                                        mergedMin: mergedMin, mergedMax: mergedMax,
                                        recordCount: engine.recordCount,
                                        liveEndT: liveEnd)
        DispatchQueue.main.async { [weak self] in self?.frame = newFrame }
    }

    // MARK: - Reducers (pure, pipeline queue)

    /// Windows the two storage tiers to [end - window, end], applies the
    /// pinch-zoom scale as a further tail-count reduction (front-dropped from
    /// history first), and returns the in-window segments oldest-first.
    private static func windowSegments(hist: DaqChannelBuffer, recent: DaqChannelBuffer,
                                       end: Double, windowSeconds: Double?,
                                       scale: CGFloat) -> [Segment] {
        let cutoff = windowSeconds.map { end - $0 } ?? -Double.infinity

        let rHi = upperBound(recent.t, end)
        let rLo = lowerBound(recent.t, cutoff, upTo: rHi)
        let hHi = upperBound(hist.t, end)
        let hLo = lowerBound(hist.t, cutoff, upTo: hHi)

        var hRange = hLo..<hHi
        var rRange = rLo..<rHi
        let total = hRange.count + rRange.count
        guard total > 0 else { return [] }

        // Pinch-zoom tail reduction (same semantics as the pre-refactor view).
        let safeScale = Double(max(scale, 0.001))
        let maxCount = max(1, min(total, Int((Double(total) / safeScale).rounded(.down))))
        var toDrop = total - maxCount
        if toDrop > 0 {
            let dropH = min(toDrop, hRange.count)
            hRange = (hRange.lowerBound + dropH)..<hRange.upperBound
            toDrop -= dropH
            if toDrop > 0 {
                rRange = (rRange.lowerBound + min(toDrop, rRange.count))..<rRange.upperBound
            }
        }

        var segs: [Segment] = []
        if !hRange.isEmpty {
            segs.append(Segment(t: hist.t, v: hist.v, src: hist.src, range: hRange))
        }
        if !rRange.isEmpty {
            segs.append(Segment(t: recent.t, v: recent.v, src: recent.src, range: rRange))
        }
        return segs
    }

    /// First index with t[i] > value (binary search; t is monotonic per stream).
    private static func upperBound(_ t: [Double], _ value: Double) -> Int {
        var lo = 0, hi = t.count
        while lo < hi {
            let mid = (lo + hi) / 2
            if t[mid] <= value { lo = mid + 1 } else { hi = mid }
        }
        return lo
    }

    private static func lowerBound(_ t: [Double], _ value: Double, upTo limit: Int) -> Int {
        var lo = 0, hi = limit
        while lo < hi {
            let mid = (lo + hi) / 2
            if t[mid] < value { lo = mid + 1 } else { hi = mid }
        }
        return lo
    }

    /// Min/max-envelope decimation across ordered segments: bin by time into
    /// `columns` buckets, keep each bucket's (min, max) in temporal order —
    /// mirrors the desktop's ~1800-column backend pyramid, computed here on
    /// the pipeline queue.
    private static func decimate(_ segs: [Segment], columns: Int, useSrc: Bool,
                                 color: (UInt8) -> Color) -> [ScopeSeriesPoint] {
        let total = segs.reduce(0) { $0 + $1.count }
        guard total > 0, let first = segs.first, let last = segs.last else { return [] }

        func point(_ seg: Segment, _ idx: Int) -> ScopeSeriesPoint {
            let s: UInt8 = useSrc && idx < seg.src.count ? seg.src[idx] : 0
            return ScopeSeriesPoint(t: seg.t[idx], v: Double(seg.v[idx]), color: color(s))
        }

        if total <= columns * 2 {
            var out = [ScopeSeriesPoint]()
            out.reserveCapacity(total)
            for seg in segs { for idx in seg.range { out.append(point(seg, idx)) } }
            return out
        }

        let tStart = first.t[first.range.lowerBound]
        let tEnd = last.t[last.range.upperBound - 1]
        let span = max(tEnd - tStart, 0.000_001)
        var result = [ScopeSeriesPoint]()
        result.reserveCapacity(columns * 2)

        var bucket = -1
        var minPt: ScopeSeriesPoint? = nil
        var maxPt: ScopeSeriesPoint? = nil
        func flush() {
            guard let mn = minPt, let mx = maxPt else { return }
            if mn.t == mx.t && mn.v == mx.v {
                result.append(mn)
            } else if mn.t <= mx.t {
                result.append(mn); result.append(mx)
            } else {
                result.append(mx); result.append(mn)
            }
        }
        for seg in segs {
            for idx in seg.range {
                // Clamp in floating point BEFORE the Int conversion: a
                // pathological t (out-of-window, non-finite) must degrade to
                // an edge bucket, never trap Double→Int. (This trapped for
                // real when block timestamps went non-monotonic.)
                let frac = (seg.t[idx] - tStart) / span
                let clamped = frac.isFinite ? min(max(frac, 0), 1) : 0
                let b = min(Int(clamped * Double(columns)), columns - 1)
                if b != bucket {
                    flush()
                    bucket = b
                    minPt = point(seg, idx)
                    maxPt = minPt
                } else {
                    let p = point(seg, idx)
                    if let mn = minPt, p.v < mn.v { minPt = p }
                    if let mx = maxPt, p.v > mx.v { maxPt = p }
                }
            }
        }
        flush()
        return result
    }

    /// Nearest-in-time value lookup across ordered segments (binary search).
    private static func nearestValue(in segs: [Segment], at time: Double) -> Float {
        var best: Float = 0
        var bestDist = Double.infinity
        for seg in segs {
            guard !seg.range.isEmpty else { continue }
            var lo = seg.range.lowerBound, hi = seg.range.upperBound
            while lo < hi {
                let mid = (lo + hi) / 2
                if seg.t[mid] < time { lo = mid + 1 } else { hi = mid }
            }
            for idx in [lo - 1, lo] where seg.range.contains(idx) {
                let d = abs(seg.t[idx] - time)
                if d < bestDist { bestDist = d; best = seg.v[idx] }
            }
        }
        return best
    }

    private static func trace(id: String, label: String, unit: String,
                              color: Color, points: [ScopeSeriesPoint]) -> RenderedTrace {
        var minV = Double.infinity
        var maxV = -Double.infinity
        for p in points {
            if p.v < minV { minV = p.v }
            if p.v > maxV { maxV = p.v }
        }
        if minV == .infinity { minV = -1; maxV = 1 }
        let span = maxV - minV
        let pad = span > 0.001 ? span * 0.1 : 1.0
        return RenderedTrace(id: id, label: label, unit: unit, defaultColor: color,
                             points: points, minV: minV - pad, maxV: maxV + pad)
    }
}
