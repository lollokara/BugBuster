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

    // MARK: - Tick (pipeline queue)

    private func tick() {
        guard let engine else { return }
        let vp = currentViewport()

        // Skip recompute entirely when neither the data nor the view changed.
        let key = (engine.recordCount, vp)
        if let last = lastPublishedKey, last.0 == key.0, last.1 == key.1 { return }
        lastPublishedKey = key

        let voltage = engine.voltage
        let current = engine.current

        // Reference stream for the shared time axis: prefer voltage, fall
        // back to current (mock mode appends both symmetrically).
        let refT = !voltage.t.isEmpty ? voltage.t : current.t
        let liveEnd = refT.last ?? 0
        let end = vp.followLive ? liveEnd : (vp.anchorEndT ?? liveEnd)

        var traces: [RenderedTrace] = []

        var vWindow: Range<Int> = 0..<0
        if vp.showVoltage || vp.showPower {
            vWindow = Self.windowRange(voltage.t, end: end, windowSeconds: vp.windowSeconds, scale: vp.timeScale)
        }
        var iWindow: Range<Int> = 0..<0
        if vp.showCurrent || vp.showPower {
            iWindow = Self.windowRange(current.t, end: end, windowSeconds: vp.windowSeconds, scale: vp.timeScale)
        }

        if vp.showVoltage, !vWindow.isEmpty {
            let pts = Self.decimate(t: voltage.t, v: voltage.v, src: nil, range: vWindow,
                                    columns: vp.columnBudget,
                                    color: { _ in ScopeColors.daqVoltage })
            traces.append(Self.trace(id: "v", label: "Voltage", unit: "V",
                                     color: ScopeColors.daqVoltage, points: pts))
        }
        if vp.showCurrent, !iWindow.isEmpty {
            let pts = Self.decimate(t: current.t, v: current.v, src: current.src, range: iWindow,
                                    columns: vp.columnBudget,
                                    color: { ScopeColors.daqCurrentColor(forSource: $0) })
            traces.append(Self.trace(id: "i", label: "Current", unit: "A",
                                     color: ScopeColors.daqCurrentFine, points: pts))
        }
        if vp.showPower, !vWindow.isEmpty, !iWindow.isEmpty {
            // Real timestamp-nearest pairing (voltage and current are
            // independently-timestamped streams): decimate voltage to display
            // columns, then look up the nearest-in-time current sample for
            // each column point. Replaces the old naive index-pairing, which
            // silently misaligned once the two buffers halved at different
            // times.
            let vPts = Self.decimate(t: voltage.t, v: voltage.v, src: nil, range: vWindow,
                                     columns: vp.columnBudget,
                                     color: { _ in ScopeColors.daqPower })
            var pPts = [ScopeSeriesPoint]()
            pPts.reserveCapacity(vPts.count)
            for p in vPts {
                let i = Self.nearestValue(t: current.t, v: current.v, range: iWindow, at: p.t)
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

    /// Index range of `t` inside [end - window, end], with the pinch-zoom
    /// scale applied as a further tail-count reduction (same semantics as the
    /// old ScopeGeom.windowedPoints).
    static func windowRange(_ t: [Double], end: Double, windowSeconds: Double?, scale: CGFloat) -> Range<Int> {
        guard !t.isEmpty else { return 0..<0 }
        let hi = upperBound(t, end)
        let lo: Int
        if let windowSeconds {
            lo = lowerBound(t, end - windowSeconds, upTo: hi)
        } else {
            lo = 0
        }
        guard hi > lo else { return 0..<0 }
        let count = hi - lo
        let maxCount = max(1, min(count, Int(Double(count) / Double(max(scale, 0.001)))))
        return (hi - maxCount)..<hi
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

    /// Min/max-envelope decimation over an SoA slice: bin by time into
    /// `columns` buckets, keep each bucket's (min, max) in temporal order —
    /// mirrors the desktop's ~1800-column backend pyramid, computed here on
    /// the pipeline queue.
    static func decimate(t: [Double], v: [Float], src: [UInt8]?, range: Range<Int>,
                         columns: Int, color: (UInt8) -> Color) -> [ScopeSeriesPoint] {
        let count = range.count
        guard count > 0 else { return [] }
        func point(_ idx: Int) -> ScopeSeriesPoint {
            let s: UInt8 = src.map { idx < $0.count ? $0[idx] : 0 } ?? 0
            return ScopeSeriesPoint(t: t[idx], v: Double(v[idx]), color: color(s))
        }
        if count <= columns * 2 {
            return range.map(point)
        }
        let tStart = t[range.lowerBound]
        let tEnd = t[range.upperBound - 1]
        let span = max(tEnd - tStart, 0.000_001)
        var result = [ScopeSeriesPoint]()
        result.reserveCapacity(columns * 2)
        var minIdx = -1, maxIdx = -1, bucket = -1
        func flush() {
            guard bucket >= 0, minIdx >= 0 else { return }
            if minIdx == maxIdx {
                result.append(point(minIdx))
            } else if minIdx < maxIdx {
                result.append(point(minIdx)); result.append(point(maxIdx))
            } else {
                result.append(point(maxIdx)); result.append(point(minIdx))
            }
        }
        for idx in range {
            var b = Int((t[idx] - tStart) / span * Double(columns))
            b = min(max(b, 0), columns - 1)
            if b != bucket {
                flush()
                bucket = b
                minIdx = idx; maxIdx = idx
            } else {
                if v[idx] < v[minIdx] { minIdx = idx }
                if v[idx] > v[maxIdx] { maxIdx = idx }
            }
        }
        flush()
        return result
    }

    /// Nearest-in-time value lookup inside `range` (binary search).
    static func nearestValue(t: [Double], v: [Float], range: Range<Int>, at time: Double) -> Float {
        guard !range.isEmpty else { return 0 }
        var lo = range.lowerBound, hi = range.upperBound
        while lo < hi {
            let mid = (lo + hi) / 2
            if t[mid] < time { lo = mid + 1 } else { hi = mid }
        }
        if lo >= range.upperBound { return v[range.upperBound - 1] }
        if lo == range.lowerBound { return v[lo] }
        return (time - t[lo - 1]) <= (t[lo] - time) ? v[lo - 1] : v[lo]
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
