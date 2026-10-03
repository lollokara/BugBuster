import SwiftUI
import Charts

/// DAQ HAT DUT power supply (VDUT): enable, live V / I / P, 60 s sparkline,
/// accumulated mAh / mWh since enable, voltage setpoint and current limit.
/// Backed by the real `/api/daq/vdut/status|enable|setpoint` firmware surface
/// (reachable over BLE or HTTP). `ConnectionManager.vdutPresent` reflects
/// whether the last status refresh actually got a response from that surface.
/// Shown in OverviewTab when the HAT kind is "daq", and in ScopeTab's DAQ settings.
struct VDUTControlsCard: View {
    @EnvironmentObject var connectionManager: ConnectionManager

    private struct Sample: Identifiable {
        let id = UUID()
        let t: Date
        let v: Double
        let iMa: Double
    }

    private enum Field: String, Identifiable {
        case voltage, current
        var id: String { rawValue }
    }

    static let voltageRange: ClosedRange<Double> = 1.8...19.9
    static let currentRange: ClosedRange<Double> = 0...2.6   // A
    private static let window: TimeInterval = 60

    @State private var voltageDraft: Double = 3.3
    @State private var currentDraftA: Double = 0.1
    @State private var editingSlider = false
    @State private var isApplying = false
    @State private var error: String?
    @State private var samples: [Sample] = []
    @State private var mAh: Double = 0
    @State private var mWh: Double = 0
    @State private var lastTick: Date?
    @State private var entryField: Field?
    @State private var entryText = ""

    private var vNow: Double? { connectionManager.vdutMeasuredVoltageV }
    private var iNowMa: Double? { connectionManager.vdutMeasuredCurrentMa }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header

            Toggle("Enable DUT Supply", isOn: Binding(
                get: { connectionManager.vdutEnabled },
                set: { newValue in
                    Task {
                        error = nil
                        if newValue { resetIntegration() }
                        if await connectionManager.setVdutEnable(newValue) == false {
                            error = "Enable request failed (no response or non-2xx)."
                        }
                    }
                }
            ))
            .tint(.cyan)

            readouts
            sparklines
            accumulators

            sliderRow(title: "Voltage Setpoint", valueText: String(format: "%.2f V", voltageDraft), field: .voltage) {
                Slider(value: $voltageDraft, in: Self.voltageRange, step: 0.05) { editing in
                    editingSlider = editing
                    if !editing { commitSetpoint() }
                }
            }
            sliderRow(title: "Current Limit", valueText: String(format: "%.2f A", currentDraftA), field: .current) {
                Slider(value: $currentDraftA, in: Self.currentRange, step: 0.01) { editing in
                    editingSlider = editing
                    if !editing { commitSetpoint() }
                }
            }

            if isApplying {
                HStack(spacing: 6) { ProgressView().controlSize(.small); Text("Applying…").font(.system(size: 11)).foregroundColor(.secondary) }
            }
            if connectionManager.vdutFault {
                Label("VDUT fault reported by the device.", systemImage: "exclamationmark.triangle.fill")
                    .font(.system(size: 11, weight: .semibold)).foregroundColor(.red)
            }
            if let error {
                Label(error, systemImage: "xmark.octagon.fill")
                    .font(.system(size: 11)).foregroundColor(.orange)
            }
        }
        .font(.system(size: 13, weight: .medium))
        .foregroundColor(.white)
        .padding(12)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .onAppear(perform: syncDrafts)
        .onChange(of: connectionManager.vdutVoltageSetpointV) { _, _ in if !editingSlider { syncDrafts() } }
        .onChange(of: connectionManager.vdutCurrentLimitMa) { _, _ in if !editingSlider { syncDrafts() } }
        .task { await pollLoop() }
        .alert(entryField == .voltage ? "Voltage setpoint (V)" : "Current limit (A)", isPresented: Binding(
            get: { entryField != nil }, set: { if !$0 { entryField = nil } })) {
            TextField("Value", text: $entryText).keyboardType(.decimalPad)
            Button("Set") { applyEntry() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(entryField == .voltage ? "1.8 – 19.9 V" : "0 – 2.6 A")
        }
    }

    // MARK: Pieces

    private var header: some View {
        HStack {
            Text("VDUT (DUT Supply)").font(.system(size: 13, weight: .bold))
            Spacer()
            let live = connectionManager.vdutPresent
            Text(live ? "Live" : "FW pending")
                .font(.system(size: 9, weight: .bold))
                .foregroundColor(live ? .green : .orange)
                .padding(.horizontal, 6).padding(.vertical, 2)
                .background(Capsule().stroke((live ? Color.green : .orange).opacity(0.5), lineWidth: 1))
        }
    }

    private var readouts: some View {
        let v = vNow, i = iNowMa
        let p: Double? = (v != nil && i != nil) ? v! * i! / 1000 : nil
        return HStack(spacing: 8) {
            readout("Voltage", v.map { String(format: "%.3f V", $0) }, .cyan)
            readout("Current", i.map { String(format: "%.1f mA", $0) }, .orange)
            readout("Power", p.map { String(format: "%.3f W", $0) }, .purple)
        }
    }

    private func readout(_ label: String, _ value: String?, _ color: Color) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label.uppercased()).font(.system(size: 9, weight: .semibold)).foregroundColor(.secondary)
            Text(value ?? "-").font(.system(size: 14, weight: .bold, design: .monospaced))
                .foregroundColor(value == nil ? .secondary : color)
                .lineLimit(1).minimumScaleFactor(0.6)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 10).padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(Color.white.opacity(0.05)))
    }

    private var sparklines: some View {
        let now = samples.last?.t ?? Date()
        let domain = now.addingTimeInterval(-Self.window)...now
        return HStack(spacing: 8) {
            spark("V", .cyan, domain) { $0.v }
            spark("mA", .orange, domain) { $0.iMa }
        }
        .frame(height: 64)
    }

    private func spark(_ title: String, _ color: Color, _ domain: ClosedRange<Date>,
                       _ value: @escaping (Sample) -> Double) -> some View {
        ZStack(alignment: .topLeading) {
            Chart(samples) { s in
                LineMark(x: .value("t", s.t), y: .value(title, value(s)))
                    .foregroundStyle(color)
                    .interpolationMethod(.monotone)
            }
            .chartXScale(domain: domain)
            .chartYScale(domain: .automatic(includesZero: false))
            .chartXAxis(.hidden)
            .chartYAxis {
                AxisMarks(position: .trailing, values: .automatic(desiredCount: 2)) { _ in
                    AxisValueLabel().font(.system(size: 8))
                }
            }
            Text("\(title) · 60 s").font(.system(size: 8, weight: .semibold)).foregroundColor(.secondary)
        }
        .padding(6)
        .frame(maxWidth: .infinity)
        .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(Color.white.opacity(0.04)))
    }

    private var accumulators: some View {
        HStack(spacing: 8) {
            readout("Charge", String(format: "%.3f mAh", mAh), .green)
            readout("Energy", String(format: "%.3f mWh", mWh), .green)
            Button(action: resetIntegration) {
                Label("Reset", systemImage: "arrow.counterclockwise")
                    .font(.system(size: 12, weight: .semibold))
                    .padding(.horizontal, 10).frame(minHeight: 34)
                    .background(Capsule().stroke(Color.white.opacity(0.3), lineWidth: 1))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Reset accumulated charge and energy")
        }
    }

    private func sliderRow<S: View>(title: String, valueText: String, field: Field,
                                    @ViewBuilder slider: () -> S) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(title)
                Spacer()
                Button {
                    entryText = field == .voltage ? String(format: "%.2f", voltageDraft) : String(format: "%.2f", currentDraftA)
                    entryField = field
                } label: {
                    Text(valueText).font(.system(.body, design: .monospaced))
                        .padding(.horizontal, 8).padding(.vertical, 2)
                        .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Color.white.opacity(0.08)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(title) \(valueText), tap to type a value")
            }
            slider()
        }
    }

    // MARK: Logic

    private func syncDrafts() {
        voltageDraft = min(max(connectionManager.vdutVoltageSetpointV, Self.voltageRange.lowerBound), Self.voltageRange.upperBound)
        currentDraftA = min(max(connectionManager.vdutCurrentLimitMa / 1000, Self.currentRange.lowerBound), Self.currentRange.upperBound)
    }

    private func resetIntegration() {
        mAh = 0
        mWh = 0
        lastTick = Date()
    }

    /// Polls the VDUT status ~1 Hz while the card is on screen (the background
    /// prefetch only runs every 5 s) and integrates charge/energy app-side.
    private func pollLoop() async {
        while !Task.isCancelled {
            #if DEBUG
            if connectionManager.isMockActive {
                // Simulated device has no VDUT endpoint; synthesize readings so the layout is checkable.
                let t = Date().timeIntervalSinceReferenceDate
                connectionManager.vdutPresent = true
                connectionManager.vdutEnabled = true
                connectionManager.vdutMeasuredVoltageV = connectionManager.vdutVoltageSetpointV + 0.01 * sin(t)
                connectionManager.vdutMeasuredCurrentMa = 220 + 60 * sin(t / 3)
                record()
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                continue
            }
            #endif
            if connectionManager.connectionState == .connected && !connectionManager.transportDegraded {
                await connectionManager.refreshVdutStatus()
                record()
            }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    private func record() {
        guard let v = vNow, let i = iNowMa else { return }
        let now = Date()
        samples.append(Sample(t: now, v: v, iMa: i))
        samples.removeAll { now.timeIntervalSince($0.t) > Self.window }
        if connectionManager.vdutEnabled {
            // Cap dt so a stalled link doesn't integrate one stale reading over a long gap.
            if let last = lastTick {
                let dt = min(now.timeIntervalSince(last), 5)
                mAh += i * dt / 3600
                mWh += v * i * dt / 3600
            }
            lastTick = now
        } else {
            lastTick = nil
        }
    }

    private func commitSetpoint() {
        let v = voltageDraft, ma = (currentDraftA * 1000).rounded()
        isApplying = true
        error = nil
        Task {
            let ok = await connectionManager.setVdutSetpoint(voltageV: v, currentLimitMa: ma)
            if !ok { error = "Setpoint request failed (no response or non-2xx); showing the last confirmed values."; syncDrafts() }
            isApplying = false
        }
    }

    private func applyEntry() {
        let text = entryText.replacingOccurrences(of: ",", with: ".")
        guard let x = Double(text.trimmingCharacters(in: .whitespaces)) else { error = "Not a number: \(entryText)"; return }
        switch entryField {
        case .voltage:
            guard Self.voltageRange.contains(x) else { error = "Voltage must be 1.8 – 19.9 V."; return }
            voltageDraft = x
        case .current:
            guard Self.currentRange.contains(x) else { error = "Current limit must be 0 – 2.6 A."; return }
            currentDraftA = x
        case nil: return
        }
        commitSetpoint()
    }
}
