import SwiftUI

/// DAQ-mode oscilloscope canvas. Unlike the ADC-mode ScopeCanvasView, this
/// view draws ONLY pre-reduced polylines published by ScopeRenderModel — no
/// windowing/decimation happens in `body`, so re-renders stay cheap no matter
/// the sample rate. Gestures write into the model's Viewport; the ~30 Hz
/// pipeline tick picks them up.
struct DaqScopeCanvasView: View {
    @ObservedObject var model: ScopeRenderModel
    let errorMessage: String?
    let isWaitingForData: Bool
    var mergedTraces: Bool = true

    // Pinch-zoom scale committed between gestures; live pinch multiplies it.
    @State private var committedScale: CGFloat = 1.0
    /// True while ANY two-finger gesture is down — a horizontal two-finger
    /// scroll pans/unanchors even when the finger distance barely changes.
    @GestureState private var twoFingerActive = false
    @State private var touchLocation: CGPoint? = nil

    /// Dead-band so a two-finger scroll doesn't also zoom: the scale only
    /// changes once the pinch ratio moves meaningfully away from 1.
    private let pinchDeadBand: CGFloat = 0.06

    // The view normally follows the live edge of the buffer ("anchored").
    // A two-finger pan (drag concurrent with an active pinch) unanchors it;
    // it stays unanchored until the user taps the floating "Live" pill.
    @State private var followLive = true
    @State private var committedPanTranslation: CGFloat = 0

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
            let frame = model.frame

            ZStack(alignment: .topLeading) {
                if let frame {
                    if mergedTraces || frame.traces.count <= 1 {
                        mergedCanvas(frame, in: geometry.size)
                    } else {
                        laneStack(frame, in: geometry.size)
                    }
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
            // the UIKit level and unreliably both recognize on a real device.
            // Wrapping both in a single `SimultaneousGesture` and attaching
            // via one `.gesture()` call is Apple's documented pattern for a
            // concurrent pinch+drag and reliably delivers both touch streams
            // together. (Do not split this back apart — see
            // .mex/patterns/daq-hat-ios-wifi-streaming.md.)
            .gesture(
                SimultaneousGesture(
                    MagnificationGesture()
                        .updating($twoFingerActive) { _, state, _ in state = true }
                        .onChanged { value in
                            guard abs(value - 1.0) > pinchDeadBand else { return }
                            let s = clampScale(committedScale * value)
                            model.updateViewport { $0.timeScale = s }
                        }
                        .onEnded { value in
                            if abs(value - 1.0) > pinchDeadBand {
                                committedScale = clampScale(committedScale * value)
                            }
                            model.updateViewport { $0.timeScale = committedScale }
                        },
                    DragGesture(minimumDistance: 0)
                        .onChanged { value in
                            if twoFingerActive {
                                // Two fingers down: pan/unanchor, regardless
                                // of whether the distance between them is
                                // changing (horizontal scroll), not the
                                // single-finger touch cursor.
                                touchLocation = nil
                                followLive = false
                                let frame = model.frame
                                let tSpan = max(tSpanOf(frame), 0.001)
                                let liveEnd = frame?.liveEndT ?? 0
                                let vp = model.currentViewport()
                                let baseEnd = vp.anchorEndT ?? liveEnd
                                let deltaSeconds = Double(value.translation.width - committedPanTranslation)
                                    / Double(max(geometry.size.width, 1)) * tSpan
                                model.updateViewport {
                                    $0.followLive = false
                                    $0.anchorEndT = baseEnd - deltaSeconds
                                }
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
            .onAppear {
                model.updateViewport { $0.columnBudget = max(200, Int(geometry.size.width)) }
            }
            .onChange(of: geometry.size.width) { _, newWidth in
                model.updateViewport { $0.columnBudget = max(200, Int(newWidth)) }
            }
        }
    }

    private func clampScale(_ s: CGFloat) -> CGFloat {
        // Deep zoom is real fidelity now: past the reduced-envelope
        // threshold the pipeline reads the raw recent ring, so 500× on a
        // 30 s window still resolves individual samples at 32 kSps.
        max(0.2, min(500.0, s))
    }

    private func tSpanOf(_ frame: ScopeRenderFrame?) -> Double {
        guard let pts = frame?.traces.first?.points, let first = pts.first, let last = pts.last else { return 1 }
        return last.t - first.t
    }

    private var liveButton: some View {
        Button(action: {
            followLive = true
            model.updateViewport { $0.followLive = true; $0.anchorEndT = nil }
        }) {
            Label("Live", systemImage: "arrow.right.to.line")
                .font(.system(size: 11, weight: .bold))
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .glassEffect(.regular.tint(.green), in: Capsule())
        }
        .buttonStyle(.plain)
    }

    // MARK: - Merged mode (all traces on one shared axis)

    private func mergedCanvas(_ frame: ScopeRenderFrame, in size: CGSize) -> some View {
        ZStack {
            Canvas { context, canvasSize in
                let rect = CGRect(origin: .zero, size: canvasSize)
                drawGrid(context: context, rect: rect)
                for trace in frame.traces {
                    drawTrace(trace.points, in: rect, minVal: frame.mergedMin, maxVal: frame.mergedMax, context: context)
                }
            }

            VStack {
                HStack {
                    boundLabel(String(format: "%.2f V", frame.mergedMax))
                    Spacer()
                }
                Spacer()
                HStack {
                    boundLabel(String(format: "%.2f V", frame.mergedMin))
                    Spacer()
                }
            }
            .padding(8)

            if let touch = touchLocation,
               let info = closestSampleInfo(frame: frame, at: touch, in: size) {
                touchOverlay(info: info, in: size)
            }
        }
    }

    // MARK: - Separate lanes (independent autoscale + autoranged units per trace)

    private func laneStack(_ frame: ScopeRenderFrame, in size: CGSize) -> some View {
        let laneHeight = size.height / CGFloat(max(frame.traces.count, 1))
        return VStack(spacing: 2) {
            ForEach(frame.traces) { trace in
                laneView(for: trace, height: laneHeight)
            }
        }
    }

    private func laneView(for trace: RenderedTrace, height: CGFloat) -> some View {
        let maxAbs = max(abs(trace.minV), abs(trace.maxV))
        let (unitScale, unit) = ScopeColors.autoUnit(maxAbs, base: trace.unit)

        return ZStack {
            Canvas { context, canvasSize in
                let rect = CGRect(origin: .zero, size: canvasSize)
                drawGrid(context: context, rect: rect)
                drawTrace(trace.points, in: rect, minVal: trace.minV, maxVal: trace.maxV, context: context)
            }

            VStack {
                HStack {
                    laneTag(String(format: "%.2f %@", trace.maxV * unitScale, unit), color: trace.defaultColor)
                    Spacer()
                    Text(trace.label)
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(trace.defaultColor)
                }
                Spacer()
                HStack {
                    laneTag(String(format: "%.2f %@", trace.minV * unitScale, unit), color: trace.defaultColor)
                    Spacer()
                }
            }
            .padding(6)

            if let touch = touchLocation,
               let info = closestSampleInfo(points: trace.points, label: trace.label,
                                            minVal: trace.minV, maxVal: trace.maxV,
                                            at: touch, in: CGSize(width: 10_000, height: height)) {
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

    private func boundLabel(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 10, weight: .bold, design: .monospaced))
            .foregroundColor(.secondary)
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
            Text(String(format: "%.3f", info.value))
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

    private func closestSampleInfo(frame: ScopeRenderFrame, at touch: CGPoint, in size: CGSize) -> TouchInfo? {
        var closest: TouchInfo? = nil
        var minDistance: CGFloat = .infinity
        let rect = CGRect(origin: .zero, size: size)

        for trace in frame.traces {
            let points = trace.points
            guard points.count >= 2 else { continue }
            let tStart = points.first!.t
            let tSpan = points.last!.t - tStart

            for (idx, p) in points.enumerated() {
                let pt = plotPoint(idx: idx, t: p.t, v: p.v, tStart: tStart, tSpan: tSpan,
                                   count: points.count, rect: rect,
                                   minVal: frame.mergedMin, maxVal: frame.mergedMax)
                let dist = hypot(pt.x - touch.x, pt.y - touch.y)
                if dist < minDistance {
                    minDistance = dist
                    closest = TouchInfo(label: trace.label, color: p.color, value: p.v, time: p.t, x: pt.x, y: pt.y)
                }
            }
        }
        return closest
    }

    private func closestSampleInfo(points: [ScopeSeriesPoint], label: String,
                                   minVal: Double, maxVal: Double,
                                   at touch: CGPoint, in size: CGSize) -> TouchInfo? {
        guard points.count >= 2 else { return nil }
        let rect = CGRect(origin: .zero, size: size)
        let tStart = points.first!.t
        let tSpan = points.last!.t - tStart
        var closest: TouchInfo? = nil
        var minDistance: CGFloat = .infinity
        for (idx, p) in points.enumerated() {
            let pt = plotPoint(idx: idx, t: p.t, v: p.v, tStart: tStart, tSpan: tSpan,
                               count: points.count, rect: rect, minVal: minVal, maxVal: maxVal)
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

    // MARK: - Grid + trace drawing (coalesces consecutive same-color runs)

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

    private func drawTrace(_ points: [ScopeSeriesPoint], in rect: CGRect, minVal: Double, maxVal: Double, context: GraphicsContext) {
        guard points.count >= 2 else { return }
        let tStart = points.first!.t
        let tSpan = points.last!.t - tStart

        var runStart = 0
        var runColor = points[0].color
        var idx = 1
        while idx < points.count {
            if points[idx].color != runColor {
                strokeRun(points, from: runStart, to: idx, tStart: tStart, tSpan: tSpan, rect: rect, minVal: minVal, maxVal: maxVal, color: runColor, context: context)
                runStart = idx
                runColor = points[idx].color
            }
            idx += 1
        }
        strokeRun(points, from: runStart, to: points.count - 1, tStart: tStart, tSpan: tSpan, rect: rect, minVal: minVal, maxVal: maxVal, color: runColor, context: context)
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
