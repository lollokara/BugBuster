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

    @GestureState private var gestureScale: CGFloat = 1.0
    @State private var touchLocation: CGPoint? = nil

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
            let (minVal, maxVal) = bounds(for: series.channels, scale: activeScale)

            ZStack {
                Canvas { context, size in
                    let rect = CGRect(origin: .zero, size: size)
                    drawGrid(context: context, rect: rect)
                    for channel in series.channels {
                        drawChannel(channel, in: rect, scale: activeScale, minVal: minVal, maxVal: maxVal, context: context)
                    }
                }

                VStack {
                    HStack {
                        boundLabel(String(format: "%.2f V", maxVal))
                        Spacer()
                    }
                    Spacer()
                    HStack {
                        boundLabel(String(format: "%.2f V", minVal))
                        Spacer()
                    }
                }
                .padding(8)

                if let touch = touchLocation,
                   let info = closestSampleInfo(at: touch, in: geometry.size, scale: activeScale, minVal: minVal, maxVal: maxVal) {
                    touchOverlay(info: info, in: geometry.size)
                }

                if let errorMessage {
                    errorOverlay(errorMessage)
                } else if isWaitingForData {
                    waitingOverlay
                }
            }
            .contentShape(Rectangle())
            .simultaneousGesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        touchLocation = gestureScale == 1.0 ? value.location : nil
                    }
                    .onEnded { _ in touchLocation = nil }
            )
            .simultaneousGesture(
                MagnificationGesture()
                    .updating($gestureScale) { value, state, _ in state = value }
                    .onEnded { value in timeScale = max(0.2, min(5.0, timeScale * value)) }
            )
        }
    }

    // MARK: - Bound labels

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

    private func closestSampleInfo(at touch: CGPoint, in size: CGSize, scale: CGFloat, minVal: Double, maxVal: Double) -> TouchInfo? {
        var closest: TouchInfo? = nil
        var minDistance: CGFloat = .infinity
        let rect = CGRect(origin: .zero, size: size)

        for channel in series.channels {
            let displayPoints = windowedPoints(channel.points, scale: scale)
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

    private func drawChannel(_ channel: ScopeSeriesChannel, in rect: CGRect, scale: CGFloat, minVal: Double, maxVal: Double, context: GraphicsContext) {
        let displayPoints = windowedPoints(channel.points, scale: scale)
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

    // MARK: - Windowing / bounds

    private func windowedPoints(_ points: [ScopeSeriesPoint], scale: CGFloat) -> [ScopeSeriesPoint] {
        guard !points.isEmpty else { return [] }
        let maxCount = max(1, min(points.count, Int(Double(points.count) / Double(scale))))
        let start = max(0, points.count - maxCount)
        return Array(points[start...])
    }

    private func bounds(for channels: [ScopeSeriesChannel], scale: CGFloat) -> (min: Double, max: Double) {
        var minVal = Double.infinity
        var maxVal = -Double.infinity
        for channel in channels {
            for p in windowedPoints(channel.points, scale: scale) {
                if p.v < minVal { minVal = p.v }
                if p.v > maxVal { maxVal = p.v }
            }
        }
        if minVal == .infinity || maxVal == -.infinity { return (-1.0, 1.0) }
        let span = maxVal - minVal
        let padding = span > 0.001 ? span * 0.1 : 1.0
        return (minVal - padding, maxVal + padding)
    }
}
