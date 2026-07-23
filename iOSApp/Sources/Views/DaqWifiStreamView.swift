import SwiftUI
import UIKit

// =============================================================================
// DaqWifiStreamView.swift — phone-side UI for the DAQ HAT WiFi streaming path
// (P4 softAP over ESP-Hosted/C6, TCP port 5566, usb_proto.h v2 frames). See
// DaqWifiStreamManager for the socket/protocol implementation and
// .mex/patterns/daq-hat-ios-wifi-streaming.md for the overall feature design.
//
// This landscape-only, full-screen scope view is entered from ScopeTab's
// header toolbar (when the connected HAT reports kind == "daq") and drives
// the entire BLE-based provisioning + WiFi auto-join + TCP-stream flow via
// DaqWifiStreamManager.startFullStreamFlow(ble:).
// =============================================================================

struct DaqStreamView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @ObservedObject private var stream = DaqWifiStreamManager.shared
    @Environment(\.dismiss) var dismiss

    @State private var settingsExpanded = false
    @State private var showVoltage = true
    @State private var showCurrent = true
    @State private var showPower = true
    @State private var autoscale = true
    @State private var sampleRateIndex = 1   // default: 10 kSps

    // MARK: - Trace colors (must match ScopeTab.ACCENTS / desktop app exactly)

    private let colorCurrentFine   = Color(red: 0.23, green: 0.51, blue: 0.96)  // blue
    private let colorCurrentCoarse = Color(red: 0.96, green: 0.62, blue: 0.12)  // amber (distinct from Power)
    private let colorCurrentBlend  = Color(red: 0.66, green: 0.33, blue: 0.97)  // purple
    private let colorVoltage       = Color(red: 0.06, green: 0.73, blue: 0.51)  // emerald
    private let colorPower         = Color(red: 0.96, green: 0.62, blue: 0.04)  // amber

    // MARK: - Sample rate options

    private let sampleRateOptions: [(label: String, sps: UInt32)] = [
        ("1 kSps", 1_000),
        ("10 kSps", 10_000),
        ("50 kSps", 50_000),
        ("250 kSps", 250_000),
    ]

    // MARK: - Fixed autoscale-off ranges

    private let fixedVoltageRange: ClosedRange<Double> = 0.0...30.0
    private let fixedCurrentRange: ClosedRange<Double> = -6.0...6.0

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            Canvas { context, size in
                drawTraces(context: context, size: size)
            }
            .ignoresSafeArea()

            VStack {
                HStack(alignment: .top) {
                    playPauseControl
                    Spacer()
                    settingsBubble
                }
                .padding(.top, 12)
                .padding(.horizontal, 16)
                Spacer()
                legendRow
                    .padding(.bottom, 12)
                    .padding(.horizontal, 16)
            }
        }
        .statusBarHidden(true)
        .onAppear {
            OrientationLock.shared.mask = .landscape
            if let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene {
                windowScene.requestGeometryUpdate(.iOS(interfaceOrientations: .landscapeRight)) { _ in }
            }
        }
        .onDisappear {
            Task { await stream.requestStreamStop(ble: connectionManager.ble) }
            OrientationLock.shared.mask = .portrait
            if let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene {
                windowScene.requestGeometryUpdate(.iOS(interfaceOrientations: .portrait)) { _ in }
            }
        }
    }

    // MARK: - Play / pause control (top-left)

    private var isBusyProvisioning: Bool {
        switch stream.provisioningState {
        case .requestingStart, .waitingForCredentials:
            return true
        default:
            return false
        }
    }

    private var playPauseControl: some View {
        VStack(alignment: .leading, spacing: 8) {
            GlassEffectContainer(spacing: 8) {
                HStack(spacing: 8) {
                    Button(action: togglePlayPause) {
                        ZStack {
                            Image(systemName: stream.isStreaming ? "pause.fill" : "play.fill")
                                .font(.system(size: 16, weight: .bold))
                                .foregroundColor(stream.isStreaming ? .orange : .green)
                                .opacity(isBusyProvisioning ? 0 : 1)
                            if isBusyProvisioning {
                                ProgressView()
                                    .tint(.cyan)
                            }
                        }
                        .padding(10)
                        .glassEffect(.regular, in: Circle())
                    }
                    .disabled(isBusyProvisioning)

                    Button(action: { dismiss() }) {
                        Image(systemName: "xmark")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundColor(.secondary)
                            .padding(10)
                            .glassEffect(.regular, in: Circle())
                    }
                }
            }

            if case .failed(let msg) = stream.provisioningState {
                Text(msg)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(.red)
                    .padding(6)
                    .glassEffect(.regular.tint(.red), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .frame(maxWidth: 260, alignment: .leading)
            } else if let error = stream.lastError {
                Text(error)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(.red)
                    .padding(6)
                    .glassEffect(.regular.tint(.red), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                    .frame(maxWidth: 260, alignment: .leading)
            }
        }
    }

    private func togglePlayPause() {
        if stream.isStreaming || stream.isConnected {
            Task { await stream.requestStreamStop(ble: connectionManager.ble) }
        } else {
            Task { await stream.startFullStreamFlow(ble: connectionManager.ble) }
        }
    }

    // MARK: - Settings bubble (top-right)

    private var settingsBubble: some View {
        VStack(alignment: .trailing, spacing: 8) {
            Button(action: {
                withAnimation(.easeInOut) { settingsExpanded.toggle() }
            }) {
                HStack(spacing: 6) {
                    Image(systemName: "slider.horizontal.3")
                    if !settingsExpanded {
                        Text("Settings")
                            .font(.system(size: 12, weight: .bold))
                    }
                }
                .font(.system(size: 14, weight: .bold))
                .foregroundColor(.cyan)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            }

            if settingsExpanded {
                VStack(alignment: .leading, spacing: 12) {
                    Text("SAMPLE RATE")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.secondary)
                    Picker("Sample Rate", selection: $sampleRateIndex) {
                        ForEach(0..<sampleRateOptions.count, id: \.self) { idx in
                            Text(sampleRateOptions[idx].label).tag(idx)
                        }
                    }
                    .pickerStyle(.segmented)
                    .onChange(of: sampleRateIndex) { _, newValue in
                        let rate = sampleRateOptions[newValue].sps
                        stream.sendSetRate(currentSps: rate, voltageSps: rate)
                    }

                    Divider().background(Color.secondary)

                    Toggle("Voltage", isOn: $showVoltage)
                        .tint(colorVoltage)
                    Toggle("Current", isOn: $showCurrent)
                        .tint(colorCurrentFine)
                    Toggle("Power", isOn: $showPower)
                        .tint(colorPower)

                    Divider().background(Color.secondary)

                    Toggle("Autoscale", isOn: $autoscale)
                        .tint(.cyan)
                }
                .font(.system(size: 13, weight: .medium))
                .foregroundColor(.white)
                .padding(12)
                .frame(width: 220)
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                .transition(.opacity.combined(with: .move(edge: .top)))
            }
        }
        .animation(.easeInOut, value: settingsExpanded)
    }

    // MARK: - Legend

    private var legendRow: some View {
        HStack(spacing: 16) {
            if showVoltage {
                legendDot(color: colorVoltage, label: "Voltage (V)")
            }
            if showCurrent {
                legendDot(color: colorCurrentFine, label: "Current — Fine")
                legendDot(color: colorCurrentCoarse, label: "Coarse")
                legendDot(color: colorCurrentBlend, label: "Blend")
            }
            Spacer()
            Text("\(stream.totalRecordsReceived) frames")
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(.secondary)
        }
    }

    private func legendDot(color: Color, label: String) -> some View {
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(label)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.secondary)
        }
    }

    // MARK: - Canvas trace rendering

    private func colorForSource(_ source: UInt8) -> Color {
        switch source {
        case 1: return colorCurrentCoarse
        case 2: return colorCurrentBlend
        default: return colorCurrentFine
        }
    }

    private func drawTraces(context: GraphicsContext, size: CGSize) {
        let margin: CGFloat = 8
        let plotRect = CGRect(x: margin, y: margin, width: size.width - margin * 2, height: size.height - margin * 2)
        guard plotRect.width > 0, plotRect.height > 0 else { return }

        if showVoltage {
            let samples = stream.voltageSamples
            if samples.count >= 2 {
                let (minV, maxV) = autoscale
                    ? liveRange(samples.map { Double($0.value) })
                    : (fixedVoltageRange.lowerBound, fixedVoltageRange.upperBound)
                let path = tracePath(samples: samples.map(\.value), in: plotRect, minVal: minV, maxVal: maxV)
                context.stroke(path, with: .color(colorVoltage), lineWidth: 2)
            }
        }

        if showCurrent {
            let samples = stream.currentSamples
            let sources = stream.currentSampleSources
            if samples.count >= 2 {
                let (minV, maxV) = autoscale
                    ? liveRange(samples.map { Double($0.value) })
                    : (fixedCurrentRange.lowerBound, fixedCurrentRange.upperBound)
                drawColoredCurrentTrace(samples: samples, sources: sources, in: plotRect, minVal: minV, maxVal: maxV, context: context)
            }
        }

        // Power: reserved. No per-sample power data is available from the wire
        // protocol (only separately-timestamped current and voltage streams,
        // which are not sample-aligned), so the toggle above is currently
        // inert -- no fabricated trace is drawn here.
    }

    private func liveRange(_ values: [Double]) -> (Double, Double) {
        guard let minV = values.min(), let maxV = values.max() else { return (-1, 1) }
        if maxV - minV < 0.001 { return (minV - 0.5, maxV + 0.5) }
        let pad = (maxV - minV) * 0.1
        return (minV - pad, maxV + pad)
    }

    private func point(index: Int, count: Int, value: Float, in rect: CGRect, minVal: Double, maxVal: Double) -> CGPoint {
        let span = max(maxVal - minVal, 0.001)
        let x = rect.minX + CGFloat(index) / CGFloat(max(count - 1, 1)) * rect.width
        let y = rect.maxY - CGFloat((Double(value) - minVal) / span) * rect.height
        return CGPoint(x: x, y: y)
    }

    private func tracePath(samples: [Float], in rect: CGRect, minVal: Double, maxVal: Double) -> Path {
        var path = Path()
        guard samples.count >= 2 else { return path }
        for (idx, v) in samples.enumerated() {
            let pt = point(index: idx, count: samples.count, value: v, in: rect, minVal: minVal, maxVal: maxVal)
            if idx == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
        }
        return path
    }

    /// Draws the current trace with per-segment coloring by source (FINE/COARSE/BLEND),
    /// coalescing consecutive same-color runs into a single stroked sub-path each.
    private func drawColoredCurrentTrace(
        samples: [(t: Double, value: Float)],
        sources: [UInt8],
        in rect: CGRect,
        minVal: Double,
        maxVal: Double,
        context: GraphicsContext
    ) {
        let count = samples.count
        guard count >= 2 else { return }

        func sourceAt(_ idx: Int) -> UInt8 {
            idx < sources.count ? sources[idx] : 0
        }

        var runStart = 0
        var runColor = colorForSource(sourceAt(0))

        var idx = 1
        while idx < count {
            let thisColor = colorForSource(sourceAt(idx))
            if thisColor != runColor {
                strokeRun(samples: samples, from: runStart, to: idx, count: count, rect: rect, minVal: minVal, maxVal: maxVal, color: runColor, context: context)
                runStart = idx
                runColor = thisColor
            }
            idx += 1
        }
        strokeRun(samples: samples, from: runStart, to: count - 1, count: count, rect: rect, minVal: minVal, maxVal: maxVal, color: runColor, context: context)
    }

    private func strokeRun(
        samples: [(t: Double, value: Float)],
        from: Int,
        to: Int,
        count: Int,
        rect: CGRect,
        minVal: Double,
        maxVal: Double,
        color: Color,
        context: GraphicsContext
    ) {
        guard to > from else { return }
        var path = Path()
        for idx in from...to {
            let pt = point(index: idx, count: count, value: samples[idx].value, in: rect, minVal: minVal, maxVal: maxVal)
            if idx == from { path.move(to: pt) } else { path.addLine(to: pt) }
        }
        context.stroke(path, with: .color(color), lineWidth: 2)
    }
}
