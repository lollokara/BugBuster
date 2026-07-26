import SwiftUI

/// Shared oscilloscope canvas for both ADC and DAQ modes: grid, per-channel
/// traces (color-coalesced runs so DAQ's per-sample source coloring works),
/// pinch-zoom timebase, and a drag-to-inspect touch cursor/tooltip.
struct ScopeCanvasView: View {
    let series: ScopeSampleSeries
    @Binding var timeScale: CGFloat
    let errorMessage: String?
    let isWaitingForData: Bool
    let onRetry: (() -> Void)?
    /// Absolute timebase window in seconds (e.g. "last 10s"/"last 30s"), applied
    /// before the pinch-zoom scale. `nil` shows the full buffered span.
    var windowSeconds: Double? = nil
    /// On: all active traces share one plot/axis (legacy behavior). Off: each
    /// trace gets its own stacked lane with independent autoscale + autoranged
    /// units (mixing V and A on one axis is otherwise misleading).
    var mergedTraces: Bool = true

    @GestureState private var gestureScale: CGFloat = 1.0
    @State private var touchLocation: CGPoint? = nil

    // MARK: - Anchor / pan state
    //
    // The view normally follows the live edge of the buffer ("anchored").
    // A two-finger pan (detected as drag translation concurrent with an
    // active pinch) unanchors it and scrubs `anchorEndT` backward/forward;
    // it stays unanchored until the user taps the floating "Live" pill.
    @State private var followLive = true
    @State private var anchorEndT: Double? = nil
    @GestureState private var panTranslation: CGFloat = 0
    @State private var committedPanTranslation: CGFloat = 0

    private var activeScale: CGFloat {
        max(0.2, min(5.0, timeScale * gestureScale))
    }

    private struct TouchInfo {
        let label: String
        let color: Color
        let value: Double
        let time: Double
        let x: CGFloat
        let y: CGFloat
    }

    var body: some View {
        GeometryReader { geometry in
            let refPoints = ScopeGeom.windowedPoints(
                series.channels.first?.points ?? [],
                scale: activeScale,
                windowSeconds: windowSeconds,
                endOverride: followLive ? nil : anchorEndT
            )
            let tSpanForPan = max(refPoints.last.map { $0.t - (refPoints.first?.t ?? $0.t) } ?? 1, 0.001)

            ZStack(alignment: .topLeading) {
                if mergedTraces || series.channels.count <= 1 {
                    mergedCanvas(in: geometry.size)
                } else {
                    laneStack(in: geometry.size)
                }

                if !followLive {
                    liveButton
                        .padding(8)
                }

                if let errorMessage {
                    errorOverlay(errorMessage)
                } else if isWaitingForData {
                    waitingOverlay
                }
            }
            .contentShape(Rectangle())
            // A pinch (2-touch UIPinchGestureRecognizer) and a pan (1-touch
            // UIPanGestureRecognizer) attached as two separate
            // `.simultaneousGesture` modifiers compete for touch ownership at
            // the UIKit level and unreliably both recognize on a real device
            // (this combo was only ever exercised against synthetic mock
            // events before, never real multitouch). Wrapping both in a
            // single `SimultaneousGesture` and attaching via one `.gesture()`
            // call is Apple's documented pattern for a concurrent pinch+drag
            // and reliably delivers both touch streams together.
            .gesture(
                SimultaneousGesture(
                    MagnificationGesture()
                        .updating($gestureScale) { value, state, _ in state = value }
                        .onEnded { value in timeScale = max(0.2, min(5.0, timeScale * value)) },
                    DragGesture(minimumDistance: 0)
                        .updating($panTranslation) { value, state, _ in
                            if gestureScale != 1.0 { state = value.translation.width }
                        }
                        .onChanged { value in
                            if gestureScale != 1.0 {
                                // Concurrent with an active pinch: treat as a
                                // 2-finger pan, not the single-finger touch cursor.
                                touchLocation = nil
                                followLive = false
                                let liveEnd = series.channels.first?.points.last?.t ?? 0
                                let baseEnd = anchorEndT ?? liveEnd
                                let deltaSeconds = Double(value.translation.width - committedPanTranslation) / Double(max(geometry.size.width, 1)) * tSpanForPan
                                anchorEndT = baseEnd - deltaSeconds
                                committedPanTranslation = value.translation.width
                            } else {
                                touchLocation = value.location
                            }
                        }
                        .onEnded { _ in
                            touchLocation = nil
                            committedPanTranslation = 0
                        }
                )
            )
        }
    }

    private var liveButton: some View {
        Button(action: { followLive = true; anchorEndT = nil }) {
            Label("Live", systemImage: "arrow.right.to.line")
                .font(.system(size: 11, weight: .bold))
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .glassEffect(.regular.tint(.green), in: Capsule())
        }
        .buttonStyle(.plain)
    }

    // MARK: - Merged mode (legacy: all traces on one shared axis)

    private func mergedCanvas(in size: CGSize) -> some View {
        let endOverride = followLive ? nil : anchorEndT
        let columnBudget = max(200, Int(size.width))
        let (minVal, maxVal) = ScopeGeom.bounds(for: series.channels, scale: activeScale, windowSeconds: windowSeconds, endOverride: endOverride, columnBudget: columnBudget)

        return ZStack {
            Canvas { context, canvasSize in
                let rect = CGRect(origin: .zero, size: canvasSize)
                drawGrid(context: context, rect: rect)
                for channel in series.channels {
                    let pts = ScopeGeom.displayPoints(for: channel.points, scale: activeScale, windowSeconds: windowSeconds, endOverride: endOverride, columnBudget: columnBudget)
                    drawChannel(pts, in: rect, minVal: minVal, maxVal: maxVal, context: context)
                }
            }

            VStack {
                HStack {
                    boundLabel(String(format: "%.2f V", maxVal), color: .secondary)
                    Spacer()
                }
                Spacer()
                HStack {
                    boundLabel(String(format: "%.2f V", minVal), color: .secondary)
                    Spacer()
                }
            }
            .padding(8)

            if let touch = touchLocation,
               let info = closestSampleInfo(at: touch, in: size, endOverride: endOverride, minVal: minVal, maxVal: maxVal) {
                touchOverlay(info: info, in: size)
            }
        }
    }

    // MARK: - Separate lanes (independent autoscale + autoranged units per trace)

    private func laneStack(in size: CGSize) -> some View {
        let endOverride = followLive ? nil : anchorEndT
        let laneHeight = size.height / CGFloat(max(series.channels.count, 1))
        return VStack(spacing: 2) {
            ForEach(Array(series.channels.enumerated()), id: \.offset) { _, channel in
                laneView(for: channel, height: laneHeight, endOverride: endOverride)
            }
        }
    }

    private func laneView(for channel: ScopeSeriesChannel, height: CGFloat, endOverride: Double?) -> some View {
        let columnBudget = 900
        let pts = ScopeGeom.displayPoints(for: channel.points, scale: activeScale, windowSeconds: windowSeconds, endOverride: endOverride, columnBudget: columnBudget)
        let (rawMin, rawMax) = ScopeGeom.bounds(forPoints: pts)
        let maxAbs = max(abs(rawMin), abs(rawMax))
        let (unitScale, unit) = ScopeColors.autoUnit(maxAbs, base: channel.unit)

        return ZStack {
            Canvas { context, canvasSize in
                let rect = CGRect(origin: .zero, size: canvasSize)
                drawGrid(context: context, rect: rect)
                drawChannel(pts, in: rect, minVal: rawMin, maxVal: rawMax, context: context)
            }

            VStack {
                HStack {
                    laneTag(String(format: "%.2f %@", rawMax * unitScale, unit), color: channel.defaultColor)
                    Spacer()
                    Text(channel.label)
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(channel.defaultColor)
                }
                Spacer()
                HStack {
                    laneTag(String(format: "%.2f %@", rawMin * unitScale, unit), color: channel.defaultColor)
                    Spacer()
                }
            }
            .padding(6)

            if let touch = touchLocation,
               let info = closestSampleInfo(at: touch, in: CGSize(width: 10_000, height: height), points: pts, label: channel.label, minVal: rawMin, maxVal: rawMax) {
                // Only draw the cursor in the lane the touch actually falls in;
                // caller clips via laneView's own frame, so any x is valid here.
                touchOverlay(info: info, in: CGSize(width: 10_000, height: height))
            }
        }
        .frame(height: height)
        .background(Color.black.opacity(0.001)) // hit-test the full lane rect
    }

    private func laneTag(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.system(size: 10, weight: .bold, design: .monospaced))
            .foregroundColor(.white)
            .padding(4)
            .glassEffect(.regular.tint(color.opacity(0.5)), in: RoundedRectangle(cornerRadius: 4, style: .continuous))
            .shadow(color: color, radius: 3)
    }

    // MARK: - Bound labels

    private func boundLabel(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.system(size: 10, weight: .bold, design: .monospaced))
            .foregroundColor(color)
            .padding(4)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 4, style: .continuous))
    }

    // MARK: - Touch cursor + tooltip

    @ViewBuilder
    private func touchOverlay(info: TouchInfo, in size: CGSize) -> some View {
        Path { path in
            path.move(to: CGPoint(x: info.x, y: 0))
            path.addLine(to: CGPoint(x: info.x, y: size.height))
        }
        .stroke(Color.white.opacity(0.4), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))

        Circle()
            .fill(info.color)
            .frame(width: 8, height: 8)
            .position(x: info.x, y: info.y)
            .shadow(color: info.color, radius: 4)

        VStack(alignment: .leading, spacing: 4) {
            Text(info.label)
                .font(.system(size: 11, weight: .bold))
                .foregroundColor(info.color)
            Text(String(format: "%.3f V", info.value))
                .font(.system(size: 14, weight: .bold, design: .monospaced))
                .foregroundColor(.white)
            Text(String(format: "T: %.3fs", info.time))
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.secondary)
        }
        .padding(8)
        .glassEffect(.regular.tint(info.color), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .position(
            x: min(max(info.x + (info.x > size.width / 2 ? -70 : 70), 60), size.width - 60),
            y: min(max(info.y + (info.y > size.height / 2 ? -50 : 50), 30), size.height - 30)
        )
    }

    private func closestSampleInfo(at touch: CGPoint, in size: CGSize, endOverride: Double?, minVal: Double, maxVal: Double) -> TouchInfo? {
        var closest: TouchInfo? = nil
        var minDistance: CGFloat = .infinity
        let rect = CGRect(origin: .zero, size: size)
        let columnBudget = max(200, Int(size.width))

        for channel in series.channels {
            let displayPoints = ScopeGeom.displayPoints(for: channel.points, scale: activeScale, windowSeconds: windowSeconds, endOverride: endOverride, columnBudget: columnBudget)
            guard displayPoints.count >= 2 else { continue }
            let tStart = displayPoints.first!.t
            let tSpan = displayPoints.last!.t - tStart

            for (idx, p) in displayPoints.enumerated() {
                let pt = plotPoint(idx: idx, t: p.t, v: p.v, tStart: tStart, tSpan: tSpan, count: displayPoints.count, rect: rect, minVal: minVal, maxVal: maxVal)
                let dist = hypot(pt.x - touch.x, pt.y - touch.y)
                if dist < minDistance {
                    minDistance = dist
                    closest = TouchInfo(label: channel.label, color: p.color, value: p.v, time: p.t, x: pt.x, y: pt.y)
                }
            }
        }
        return closest
    }

    private func closestSampleInfo(at touch: CGPoint, in size: CGSize, points: [ScopeSeriesPoint], label: String, minVal: Double, maxVal: Double) -> TouchInfo? {
        guard points.count >= 2 else { return nil }
        let rect = CGRect(origin: .zero, size: size)
        let tStart = points.first!.t
        let tSpan = points.last!.t - tStart
        var closest: TouchInfo? = nil
        var minDistance: CGFloat = .infinity
        for (idx, p) in points.enumerated() {
            let pt = plotPoint(idx: idx, t: p.t, v: p.v, tStart: tStart, tSpan: tSpan, count: points.count, rect: rect, minVal: minVal, maxVal: maxVal)
            let dist = abs(pt.x - touch.x)
            if dist < minDistance {
                minDistance = dist
                closest = TouchInfo(label: label, color: p.color, value: p.v, time: p.t, x: pt.x, y: pt.y)
            }
        }
        return closest
    }

    // MARK: - Error / waiting overlays

    private func errorOverlay(_ message: String) -> some View {
        VStack(spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 32))
                .foregroundColor(.orange)
            Text("Connection Error")
                .font(.headline)
                .foregroundColor(.white)
            Text(message)
                .font(.system(size: 12))
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 16)
            if let onRetry {
                Button(action: onRetry) {
                    Text("Retry")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundColor(.cyan)
                        .padding(.horizontal, 20)
                        .padding(.vertical, 8)
                        .glassEffect(.regular.tint(.cyan), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                }
            }
        }
        .padding()
        .frame(maxWidth: 280)
        .glassEffect(.regular.tint(.red), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }

    private var waitingOverlay: some View {
        VStack(spacing: 8) {
            ProgressView().tint(.cyan)
            Text("Waiting for waveform data...")
                .font(.system(size: 12))
                .foregroundColor(.secondary)
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }

    // MARK: - Grid

    private func drawGrid(context: GraphicsContext, rect: CGRect) {
        var path = Path()
        let hSpacing = rect.height / 8
        for i in 1..<8 {
            let y = CGFloat(i) * hSpacing
            path.move(to: CGPoint(x: rect.minX, y: y))
            path.addLine(to: CGPoint(x: rect.maxX, y: y))
        }
        let wSpacing = rect.width / 10
        for i in 1..<10 {
            let x = CGFloat(i) * wSpacing
            path.move(to: CGPoint(x: x, y: rect.minY))
            path.addLine(to: CGPoint(x: x, y: rect.maxY))
        }
        context.stroke(path, with: .color(Color.white.opacity(0.06)), lineWidth: 1)
    }

    // MARK: - Trace drawing (coalesces consecutive same-color point runs)

    private func drawChannel(_ displayPoints: [ScopeSeriesPoint], in rect: CGRect, minVal: Double, maxVal: Double, context: GraphicsContext) {
        guard displayPoints.count >= 2 else { return }
        let tStart = displayPoints.first!.t
        let tSpan = displayPoints.last!.t - tStart

        var runStart = 0
        var runColor = displayPoints[0].color
        var idx = 1
        while idx < displayPoints.count {
            if displayPoints[idx].color != runColor {
                strokeRun(displayPoints, from: runStart, to: idx, tStart: tStart, tSpan: tSpan, rect: rect, minVal: minVal, maxVal: maxVal, color: runColor, context: context)
                runStart = idx
                runColor = displayPoints[idx].color
            }
            idx += 1
        }
        strokeRun(displayPoints, from: runStart, to: displayPoints.count - 1, tStart: tStart, tSpan: tSpan, rect: rect, minVal: minVal, maxVal: maxVal, color: runColor, context: context)
    }

    private func strokeRun(_ points: [ScopeSeriesPoint], from: Int, to: Int, tStart: Double, tSpan: Double, rect: CGRect, minVal: Double, maxVal: Double, color: Color, context: GraphicsContext) {
        guard to > from else { return }
        var path = Path()
        for i in from...to {
            let pt = plotPoint(idx: i, t: points[i].t, v: points[i].v, tStart: tStart, tSpan: tSpan, count: points.count, rect: rect, minVal: minVal, maxVal: maxVal)
            if i == from { path.move(to: pt) } else { path.addLine(to: pt) }
        }
        context.stroke(path, with: .color(color), lineWidth: 2)
    }

    private func plotPoint(idx: Int, t: Double, v: Double, tStart: Double, tSpan: Double, count: Int, rect: CGRect, minVal: Double, maxVal: Double) -> CGPoint {
        let x = tSpan > 0
            ? CGFloat((t - tStart) / tSpan) * rect.width
            : CGFloat(idx) / CGFloat(max(count - 1, 1)) * rect.width
        let span = max(maxVal - minVal, 0.001)
        let y = rect.height - CGFloat((v - minVal) / span) * rect.height
        return CGPoint(x: rect.minX + x, y: rect.minY + y)
    }
}

// MARK: - Windowing / decimation / bounds (shared geometry helpers)

enum ScopeGeom {
    /// Slices to the absolute-seconds window (if any) ending at `endOverride`
    /// (or the buffer's live end when following), then applies the pinch-zoom
    /// `scale` as a further tail-count reduction.
    static func windowedPoints(_ points: [ScopeSeriesPoint], scale: CGFloat, windowSeconds: Double?, endOverride: Double?) -> [ScopeSeriesPoint] {
        guard !points.isEmpty else { return [] }
        var points = points
        let refEnd = endOverride ?? points.last!.t
        if let windowSeconds {
            let cutoff = refEnd - windowSeconds
            points = points.filter { $0.t >= cutoff && $0.t <= refEnd }
        } else if endOverride != nil {
            points = points.filter { $0.t <= refEnd }
        }
        guard !points.isEmpty else { return [] }
        let maxCount = max(1, min(points.count, Int(Double(points.count) / Double(scale))))
        let start = max(0, points.count - maxCount)
        return Array(points[start...])
    }

    /// Min/max envelope decimation: once a window has more raw samples than
    /// `targetColumns` display pixels can usefully show, bin by time into
    /// `targetColumns` buckets and keep each bucket's (min, max) pair in
    /// temporal order — mirrors the desktop app's ~1800-column backend
    /// pyramid (`DesktopApp/BugBuster/src/tabs/daq.rs:daq_get_view`), just
    /// computed client-side since iOS has no equivalent backend view API.
    static func decimate(_ points: [ScopeSeriesPoint], targetColumns: Int) -> [ScopeSeriesPoint] {
        guard targetColumns > 0, points.count > targetColumns * 2 else { return points }
        let tStart = points.first!.t
        let tEnd = points.last!.t
        let span = max(tEnd - tStart, 0.000_001)
        var buckets = [[ScopeSeriesPoint]](repeating: [], count: targetColumns)
        for p in points {
            var idx = Int((p.t - tStart) / span * Double(targetColumns))
            idx = min(max(idx, 0), targetColumns - 1)
            buckets[idx].append(p)
        }
        var result: [ScopeSeriesPoint] = []
        result.reserveCapacity(targetColumns * 2)
        for bucket in buckets {
            guard !bucket.isEmpty else { continue }
            if bucket.count == 1 {
                result.append(bucket[0])
                continue
            }
            var minP = bucket[0]
            var maxP = bucket[0]
            for p in bucket {
                if p.v < minP.v { minP = p }
                if p.v > maxP.v { maxP = p }
            }
            if minP.t <= maxP.t {
                result.append(minP); result.append(maxP)
            } else {
                result.append(maxP); result.append(minP)
            }
        }
        return result
    }

    static func displayPoints(for points: [ScopeSeriesPoint], scale: CGFloat, windowSeconds: Double?, endOverride: Double?, columnBudget: Int) -> [ScopeSeriesPoint] {
        decimate(windowedPoints(points, scale: scale, windowSeconds: windowSeconds, endOverride: endOverride), targetColumns: columnBudget)
    }

    static func bounds(for channels: [ScopeSeriesChannel], scale: CGFloat, windowSeconds: Double?, endOverride: Double?, columnBudget: Int) -> (min: Double, max: Double) {
        var minVal = Double.infinity
        var maxVal = -Double.infinity
        for channel in channels {
            for p in displayPoints(for: channel.points, scale: scale, windowSeconds: windowSeconds, endOverride: endOverride, columnBudget: columnBudget) {
                if p.v < minVal { minVal = p.v }
                if p.v > maxVal { maxVal = p.v }
            }
        }
        if minVal == .infinity || maxVal == -.infinity { return (-1.0, 1.0) }
        let span = maxVal - minVal
        let padding = span > 0.001 ? span * 0.1 : 1.0
        return (minVal - padding, maxVal + padding)
    }

    static func bounds(forPoints points: [ScopeSeriesPoint]) -> (min: Double, max: Double) {
        var minVal = Double.infinity
        var maxVal = -Double.infinity
        for p in points {
            if p.v < minVal { minVal = p.v }
            if p.v > maxVal { maxVal = p.v }
        }
        if minVal == .infinity || maxVal == -.infinity { return (-1.0, 1.0) }
        let span = maxVal - minVal
        let padding = span > 0.001 ? span * 0.1 : 1.0
        return (minVal - padding, maxVal + padding)
    }
}
