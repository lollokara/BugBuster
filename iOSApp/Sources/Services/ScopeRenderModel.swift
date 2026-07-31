import SwiftUI

// =============================================================================
// ScopeRenderModel.swift — background render pipeline for the DAQ scope.
//
// Ticks at 20 Hz on DaqStreamEngine.queue (the same serial queue that owns
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
    /// Resolves a point's compact `source` discriminator to a real `Color`.
    /// Points no longer carry a `Color` themselves (see `ScopeSeriesPoint`);
    /// the canvas calls this ONCE per contiguous same-source run, not once
    /// per point. Voltage/power traces ignore the argument (constant color);
    /// the current trace maps 0/1/2 to fine/coarse/blend.
    let colorForSource: (UInt8) -> Color
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
    /// Whether the view is still anchored to the live edge. Published here so
    /// the "Live" affordance can be rendered by ScopeTab as a SIBLING of the
    /// legend panel rather than inside the canvas: ScopeTab draws the legend
    /// as an overlay ON TOP of the canvas, so anything the canvas positions in
    /// a corner can be silently covered by it — which is exactly how the pill
    /// ended up unclickable twice (first under the lane label, then under the
    /// diagnostic legend).
    let followLive: Bool
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
        var showPower = false
        /// Max envelope columns per trace; roughly the canvas pixel width.
        var columnBudget = 900
    }

    @Published private(set) var frame: ScopeRenderFrame? = nil

    private let lock = NSLock()
    private var viewport = Viewport()
    private var timer: DispatchSourceTimer?
    private weak var engine: DaqStreamEngine?
    private var lastPublishedKey: (Int, Viewport)? = nil

    // =========================================================================
    // TEMP-INSTRUMENTATION — bench diagnostics for two open bugs:
    //   M1: 10s/30s timebases render nothing while Full renders fine.
    //   M2: render perf degrades ("sloppy") after ~5-6k frames.
    // Remove this whole block (grep TEMP-INSTRUMENTATION across the repo) once
    // both measurements have been captured at the bench. #if DEBUG only —
    // no production behaviour change, no per-sample hot-path work, throttled
    // to ~1 Hz. See instrumentation-report.md for how to read the output.
    // =========================================================================
    #if DEBUG
    /// Compact, human-readable rolling summary for the on-screen readout.
    /// Updated at most once/sec so it's stable enough to photograph.
    @Published private(set) var dbgLine: String = ""

    private var dbgLastLogAt: CFAbsoluteTime = 0
    private var dbgTickCalls = 0
    private var dbgRecomputes = 0
    private var dbgTickDurMaxMs: Double = 0
    private var dbgTickDurSumMs: Double = 0
    private var dbgTickDurCount = 0
    private var dbgPointsMax = 0
    private var dbgPointsSumForMean = 0
    private var dbgPointsCount = 0
    #endif

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
        t.schedule(deadline: .now(), repeating: .milliseconds(50))
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
    ///
    /// `step` sub-samples the slice while iterating. The RECENT buffers have
    /// real precomputed tiers (raw -> 8:1 -> 64:1) so they always step by 1,
    /// but the HISTORY buffer has no tiers: it is a single ~200k-entry
    /// envelope that `decimate` used to walk in full on every 20 Hz tick,
    /// which measured as the dominant cost of the whole render pipeline
    /// (bench: bufV hist=196608 vs the 7696-entry `reduced` tier actually
    /// chosen for the recent window). Stepping bounds that walk to the display
    /// budget.
    ///
    /// History is stored as consecutive (min, max) PAIRS, so `step` is forced
    /// even and both members of a sampled pair are kept -- sub-sampling whole
    /// pairs preserves the peak-envelope shape, whereas an odd stride would
    /// alternately drop the min or the max and visibly bias the trace.
    private struct Segment {
        let t: [Double]
        let v: [Float]
        let src: [UInt8]
        var range: Range<Int>
        var step: Int = 1
        var count: Int { (range.count + step - 1) / step }

        /// Indices to visit, honouring `step` while keeping (min,max) pairs
        /// together.
        func indices() -> StrideTo<Int> {
            stride(from: range.lowerBound, to: range.upperBound, by: step)
        }
    }

    // MARK: - Tick (pipeline queue)

    private func tick() {
        guard let engine else { return }
        #if DEBUG
        let dbgStart = DispatchTime.now()  // TEMP-INSTRUMENTATION
        #endif
        let vp = currentViewport()

        // Skip recompute entirely when neither the data nor the view changed.
        let key = (engine.recordCount, vp)
        if let last = lastPublishedKey, last.0 == key.0, last.1 == key.1 {
            #if DEBUG
            // TEMP-INSTRUMENTATION: still record the skip so M2's "recompute
            // vs early-return" ratio is accurate; end is two O(1) reads.
            let liveEnd = engine.voltage.t.last ?? engine.current.t.last ?? 0
            let end = vp.followLive ? liveEnd : (vp.anchorEndT ?? liveEnd)
            dbgTick(recomputed: false, start: dbgStart, engine: engine, vp: vp,
                    end: end, vSegs: [], iSegs: [], traces: [])
            #endif
            return
        }
        lastPublishedKey = key

        // Reference stream for the shared time axis: prefer voltage, fall
        // back to current (mock mode appends both symmetrically).
        let liveEnd = engine.voltage.t.last ?? engine.current.t.last ?? 0
        let end = vp.followLive ? liveEnd : (vp.anchorEndT ?? liveEnd)

        var traces: [RenderedTrace] = []

        var vSegs: [Segment] = []
        if vp.showVoltage || vp.showPower {
            vSegs = Self.windowSegments(hist: engine.voltageHist,
                                        tiers: [engine.voltage, engine.voltageMid, engine.voltageReduced],
                                        end: end, windowSeconds: vp.windowSeconds,
                                        scale: vp.timeScale, columns: vp.columnBudget)
        }
        var iSegs: [Segment] = []
        if vp.showCurrent || vp.showPower {
            iSegs = Self.windowSegments(hist: engine.currentHist,
                                        tiers: [engine.current, engine.currentMid, engine.currentReduced],
                                        end: end, windowSeconds: vp.windowSeconds,
                                        scale: vp.timeScale, columns: vp.columnBudget)
        }

        // Voltage polyline is shared by the Voltage trace and the Power
        // pairing — decimate it once.
        var vPts: [ScopeSeriesPoint] = []
        if !vSegs.isEmpty {
            vPts = Self.decimate(vSegs, columns: vp.columnBudget, useSrc: false)
        }
        if vp.showVoltage, !vPts.isEmpty {
            traces.append(Self.trace(id: "v", label: "Voltage", unit: "V",
                                     colorForSource: { _ in ScopeColors.daqVoltage }, points: vPts))
        }
        if vp.showCurrent, !iSegs.isEmpty {
            let pts = Self.decimate(iSegs, columns: vp.columnBudget, useSrc: true)
            traces.append(Self.trace(id: "i", label: "Current", unit: "A",
                                     colorForSource: { ScopeColors.daqCurrentColor(forSource: $0) }, points: pts))
        }
        if vp.showPower, !vPts.isEmpty, !iSegs.isEmpty {
            // Real timestamp-nearest pairing (voltage and current are
            // independently-timestamped streams): for each voltage column
            // point, look up the nearest-in-time current sample.
            var pPts = [ScopeSeriesPoint]()
            pPts.reserveCapacity(vPts.count)
            for p in vPts {
                let i = Self.nearestValue(in: iSegs, at: p.t)
                pPts.append(ScopeSeriesPoint(t: p.t, v: p.v * Double(i)))
            }
            traces.append(Self.trace(id: "p", label: "Power", unit: "W",
                                     colorForSource: { _ in ScopeColors.daqPower }, points: pPts))
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
                                        liveEndT: liveEnd,
                                        followLive: vp.followLive)
        #if DEBUG
        // TEMP-INSTRUMENTATION
        dbgTick(recomputed: true, start: dbgStart, engine: engine, vp: vp,
                end: end, vSegs: vSegs, iSegs: iSegs, traces: traces)
        #endif
        DispatchQueue.main.async { [weak self] in self?.frame = newFrame }
    }

    #if DEBUG
    // =========================================================================
    // TEMP-INSTRUMENTATION — see block comment near the stored properties.
    // Pure bookkeeping + throttled print/publish; does not touch buffers or
    // recompute anything the real pipeline didn't already compute.
    // =========================================================================
    private func dbgTick(recomputed: Bool, start: DispatchTime, engine: DaqStreamEngine,
                         vp: Viewport, end: Double,
                         vSegs: [Segment], iSegs: [Segment], traces: [RenderedTrace]) {
        let durMs = Double(DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds) / 1_000_000.0
        dbgTickCalls += 1
        if recomputed { dbgRecomputes += 1 }
        dbgTickDurSumMs += durMs
        dbgTickDurCount += 1
        if durMs > dbgTickDurMaxMs { dbgTickDurMaxMs = durMs }

        if recomputed {
            let pointsPublished = traces.reduce(0) { $0 + $1.points.count }
            dbgPointsSumForMean += pointsPublished
            dbgPointsCount += 1
            if pointsPublished > dbgPointsMax { dbgPointsMax = pointsPublished }
        }

        // Throttle to ~1 Hz: this is the ONLY place that does string
        // formatting / printing / publishing, so the hot per-tick path above
        // stays cheap regardless of log rate.
        let now = CFAbsoluteTimeGetCurrent()
        guard now - dbgLastLogAt >= 1.0 else { return }
        dbgLastLogAt = now

        let meanTickMs = dbgTickDurCount > 0 ? dbgTickDurSumMs / Double(dbgTickDurCount) : 0
        let meanPts = dbgPointsCount > 0 ? dbgPointsSumForMean / dbgPointsCount : 0
        let skipPct = dbgTickCalls > 0
            ? Double(dbgTickCalls - dbgRecomputes) / Double(dbgTickCalls) * 100 : 0

        // --- Measurement 1: 10s/30s blank-render evidence ---
        let vLast = engine.voltage.t.last ?? .nan
        let iLast = engine.current.t.last ?? .nan
        let skew = (vLast.isFinite && iLast.isFinite) ? abs(vLast - iLast) : .nan
        let cutoff = vp.windowSeconds.map { end - $0 }
        let vSegPts = vSegs.reduce(0) { $0 + $1.count }
        let iSegPts = iSegs.reduce(0) { $0 + $1.count }
        let winLabel = vp.windowSeconds.map { String(format: "%.0fs", $0) } ?? "full"

        let m1 = String(format: "[TEMP-INSTRUMENTATION] M1 win=%@ end=%.3f cutoff=%.3f skew=%.4f " +
                        "vLast=%.3f iLast=%.3f | vHist=[%.3f,%.3f] vRaw=[%.3f,%.3f] " +
                        "iHist=[%.3f,%.3f] iRaw=[%.3f,%.3f] | vSegPts=%d iSegPts=%d",
                        winLabel, end, cutoff ?? .nan, skew, vLast, iLast,
                        engine.voltageHist.t.first ?? .nan, engine.voltageHist.t.last ?? .nan,
                        engine.voltage.t.first ?? .nan, engine.voltage.t.last ?? .nan,
                        engine.currentHist.t.first ?? .nan, engine.currentHist.t.last ?? .nan,
                        engine.current.t.first ?? .nan, engine.current.t.last ?? .nan,
                        vSegPts, iSegPts)

        // --- Measurement 2: render-perf-degradation evidence ---
        let m2 = String(format: "[TEMP-INSTRUMENTATION] M2 tickMs(max/mean)=%.2f/%.2f " +
                        "pts(max/mean)=%d/%d recompute=%d/%d skip=%.0f%% " +
                        "bufV(raw/mid/reduced/hist)=%d/%d/%d/%d bufI(raw/mid/reduced/hist)=%d/%d/%d/%d",
                        dbgTickDurMaxMs, meanTickMs, dbgPointsMax, meanPts,
                        dbgRecomputes, dbgTickCalls, skipPct,
                        engine.voltage.count, engine.voltageMid.count,
                        engine.voltageReduced.count, engine.voltageHist.count,
                        engine.current.count, engine.currentMid.count,
                        engine.currentReduced.count, engine.currentHist.count)

        print(m1)
        print(m2)

        let short = String(format: "M1 skew=%.3f cut=%.2f segV/I=%d/%d | M2 tick %.1f/%.1fms pts=%d skip=%.0f%%",
                           skew, cutoff ?? .nan, vSegPts, iSegPts,
                           dbgTickDurMaxMs, meanTickMs, dbgPointsMax, skipPct)
        DispatchQueue.main.async { [weak self] in self?.dbgLine = short }

        // Reset the ~1s rolling window.
        dbgTickDurMaxMs = 0; dbgTickDurSumMs = 0; dbgTickDurCount = 0
        dbgPointsMax = 0; dbgPointsSumForMean = 0; dbgPointsCount = 0
        dbgTickCalls = 0; dbgRecomputes = 0
    }
    #endif

    // MARK: - Reducers (pure, pipeline queue)

    /// Windows the storage tiers to [end - window, end], applies the
    /// pinch-zoom scale as a further tail-count reduction (front-dropped from
    /// history first), and returns the in-window segments oldest-first.
    /// `tiers` is ordered finest-first (raw, 8:1, 64:1): the finest tier
    /// whose POST-zoom visible count fits the display budget is used, so
    /// resolution steps down progressively as the visible span grows and
    /// deep zooms always read raw samples at full fidelity.
    private static func windowSegments(hist: DaqChannelBuffer,
                                       tiers: [DaqChannelBuffer],
                                       end: Double, windowSeconds: Double?,
                                       scale: CGFloat, columns: Int) -> [Segment] {
        let cutoff = windowSeconds.map { end - $0 } ?? -Double.infinity
        let budget = Double(columns * 4)
        let zoom = Double(max(scale, 0.001))

        var recent = tiers.first ?? DaqChannelBuffer()
        var rHi = upperBound(recent.t, end)
        var rLo = lowerBound(recent.t, cutoff, upTo: rHi)
        for tier in tiers.dropFirst() {
            if Double(rHi - rLo) / zoom <= budget { break }
            guard !tier.t.isEmpty else { continue }
            recent = tier
            rHi = upperBound(recent.t, end)
            rLo = lowerBound(recent.t, cutoff, upTo: rHi)
        }
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
            // History has no precomputed tiers, so bound its walk here (see
            // Segment.step). Round the stride to an even number so whole
            // (min,max) pairs are sampled together.
            var hStep = 1
            if Double(hRange.count) > budget {
                hStep = Int((Double(hRange.count) / budget).rounded(.up))
                if hStep > 1 && hStep % 2 != 0 { hStep += 1 }
            }
            segs.append(Segment(t: hist.t, v: hist.v, src: hist.src,
                                range: hRange, step: hStep))
        }
        if !rRange.isEmpty {
            segs.append(Segment(t: recent.t, v: recent.v, src: recent.src,
                                range: rRange))
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
    private static func decimate(_ segs: [Segment], columns: Int, useSrc: Bool) -> [ScopeSeriesPoint] {
        let total = segs.reduce(0) { $0 + $1.count }
        guard total > 0, let first = segs.first, let last = segs.last else { return [] }

        func point(_ seg: Segment, _ idx: Int) -> ScopeSeriesPoint {
            let s: UInt8 = useSrc && idx < seg.src.count ? seg.src[idx] : 0
            return ScopeSeriesPoint(t: seg.t[idx], v: Double(seg.v[idx]), source: s)
        }

        if total <= columns * 2 {
            var out = [ScopeSeriesPoint]()
            out.reserveCapacity(total)
            for seg in segs { for idx in seg.indices() { out.append(point(seg, idx)) } }
            return out
        }

        let tStart = first.t[first.range.lowerBound]
        let tEnd = last.t[last.range.upperBound - 1]
        let span = max(tEnd - tStart, 0.000_001)
        var result = [ScopeSeriesPoint]()
        result.reserveCapacity(columns * 2)

        // Scalar bucket tracking: materializing a ScopeSeriesPoint (with a
        // heap-backed SwiftUI Color) per RAW sample burned an entire CPU core
        // at real rates. Points are only built at bucket flush now, and even
        // then carry just the `source` discriminator — the canvas resolves
        // that to a real Color once per contiguous run when it draws, not
        // once per point (see ScopeSeriesPoint's doc comment).
        var bucket = -1
        var mnT = 0.0, mxT = 0.0
        var mnV: Float = 0, mxV: Float = 0
        var mnS: UInt8 = 0, mxS: UInt8 = 0
        func flush() {
            guard bucket >= 0 else { return }
            if mnT == mxT && mnV == mxV {
                result.append(ScopeSeriesPoint(t: mnT, v: Double(mnV), source: mnS))
            } else if mnT <= mxT {
                result.append(ScopeSeriesPoint(t: mnT, v: Double(mnV), source: mnS))
                result.append(ScopeSeriesPoint(t: mxT, v: Double(mxV), source: mxS))
            } else {
                result.append(ScopeSeriesPoint(t: mxT, v: Double(mxV), source: mxS))
                result.append(ScopeSeriesPoint(t: mnT, v: Double(mnV), source: mnS))
            }
        }
        for seg in segs {
            let hasSrc = useSrc && !seg.src.isEmpty
            for idx in seg.indices() {
                // Clamp in floating point BEFORE the Int conversion: a
                // pathological t (out-of-window, non-finite) must degrade to
                // an edge bucket, never trap Double→Int. (This trapped for
                // real when block timestamps went non-monotonic.)
                let frac = (seg.t[idx] - tStart) / span
                let clamped = frac.isFinite ? min(max(frac, 0), 1) : 0
                let b = min(Int(clamped * Double(columns)), columns - 1)
                let t = seg.t[idx]
                let v = seg.v[idx]
                let s: UInt8 = hasSrc && idx < seg.src.count ? seg.src[idx] : 0
                if b != bucket {
                    flush()
                    bucket = b
                    mnT = t; mxT = t; mnV = v; mxV = v; mnS = s; mxS = s
                } else {
                    if v < mnV { mnV = v; mnT = t; mnS = s }
                    if v > mxV { mxV = v; mxT = t; mxS = s }
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
                              colorForSource: @escaping (UInt8) -> Color,
                              points: [ScopeSeriesPoint]) -> RenderedTrace {
        // Outlier-robust bounds. Taking the raw min/max here let a SINGLE
        // dropout or spike define the whole lane, collapsing the real signal
        // into a sliver; see ScopeAxis for the full rationale and the
        // magnitude-relative padding this used to do inline.
        let (minV, maxV) = ScopeAxis.bounds(points, by: \.v)
        return RenderedTrace(id: id, label: label, unit: unit, defaultColor: colorForSource(0),
                             colorForSource: colorForSource,
                             points: points, minV: minV, maxV: maxV)
    }
}
