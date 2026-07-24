import SwiftUI
import Charts

/// Lays out rail slider+toggle blocks as a vertical stack (iPhone / compact width)
/// or a 3-column grid (iPad regular width), without touching the block contents.
struct AdaptiveRailStack<Content: View>: View {
    let sizeClass: UserInterfaceSizeClass?
    @ViewBuilder let content: Content

    var body: some View {
        if sizeClass == .regular {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 260), spacing: 12)], spacing: 12) {
                content
            }
        } else {
            VStack(spacing: 16) {
                content
            }
        }
    }
}

struct ChannelFunctionInfo: Identifiable {
    let id: Int
    let name: String
}

let availableFunctions = [
    ChannelFunctionInfo(id: 0, name: "High Impedance"),
    ChannelFunctionInfo(id: 1, name: "Voltage Output (VOUT)"),
    ChannelFunctionInfo(id: 2, name: "Current Output (IOUT)"),
    ChannelFunctionInfo(id: 3, name: "Voltage Input (VIN)"),
    ChannelFunctionInfo(id: 4, name: "Current Input (Ext Power)"),
    ChannelFunctionInfo(id: 5, name: "Current Input (Loop Power)"),
    ChannelFunctionInfo(id: 7, name: "RTD Resistance Meas"),
    ChannelFunctionInfo(id: 8, name: "Digital Input Logic"),
    ChannelFunctionInfo(id: 9, name: "Digital Input Loop")
]

struct OverviewTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var selectedChannel: ChannelState? = nil
    @State private var showingConfigSheet = false

    // Main board slider values
    @State private var vadj1Value: Double = 3.3
    @State private var vadj2Value: Double = 3.3
    @State private var vlogicValue: Double = 3.3

    // Main board dirty flags
    @State private var vadj1Dirty = false
    @State private var vadj2Dirty = false
    @State private var vlogicDirty = false

    // HAT rail slider values (rail 0=VLOGIC, 1=VADJ3, 2=VADJ4)
    @State private var hatVlogicValue: Double = 3.3
    @State private var hatVadj3Value: Double = 3.3
    @State private var hatVadj4Value: Double = 3.3

    // HAT rail dirty flags
    @State private var hatVlogicDirty = false
    @State private var hatVadj3Dirty = false
    @State private var hatVadj4Dirty = false




    @State private var quicksetupSlots: [QuickSetupSlot] = []

    // I/V Plot
    @State private var showIVPlotConfig = false

    struct QuickSetupSlot: Identifiable, Codable {
        var id: Int { index }
        let index: Int
        let occupied: Bool
        let name: String
    }

    var hatPresent: Bool {
        connectionManager.lastHatStatus?.isPresent ?? false
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                headerSection

                // AFE at top
                channelsGrid
                ivPlotSection
                suppliesCard

                presetsCard
            }
            .padding()
        }
        .background(
            LinearGradient(
                colors: [Color(red: 0.05, green: 0.08, blue: 0.16), Color(red: 0.02, green: 0.03, blue: 0.06)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()
        )
        .onAppear {
            loadQuicksetups()
            updateSlidersFromModel()
            updateHatSlidersFromModel()
        }
        .onChange(of: connectionManager.lastOverview?.idac) { _ in
            updateSlidersFromModel()
        }
        .onChange(of: connectionManager.lastHatRails) { _ in
            updateHatSlidersFromModel()
        }
        .sheet(item: $selectedChannel) { channel in
            ChannelConfigSheet(channel: channel) {
                selectedChannel = nil
            }
        }
        .sheet(isPresented: $showIVPlotConfig) {
            IVPlotConfigSheet()
                .environmentObject(connectionManager)
        }
    }

    // MARK: - Header

    var headerSection: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Bench Overview")
                    .font(.system(size: 28, weight: .bold, design: .rounded))
                if let dev = connectionManager.activeDevice {
                    Text("Connected to \(dev.hostname) (\(dev.ip))")
                        .font(.system(size: 13))
                        .foregroundColor(.secondary)
                }
            }
            Spacer()
            HStack(spacing: 12) {
                HStack(spacing: 6) {
                    Circle()
                        .fill(Color.green)
                        .frame(width: 8, height: 8)
                    Text("Live")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundColor(.black)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .glassEffect(.regular.tint(.green), in: Capsule())

                Button(action: { connectionManager.disconnect() }) {
                    Image(systemName: "power")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.black)
                        .padding(8)
                        .glassEffect(.regular.tint(.red), in: Circle())
                }
            }
        }
        .padding(.top, 10)
    }

    // MARK: - AFE Channels Grid (with sparklines)

    private var channelGridColumns: [GridItem] {
        sizeClass == .regular
            ? [GridItem(.adaptive(minimum: 260), spacing: 14)]
            : [GridItem(.flexible()), GridItem(.flexible())]
    }

    var channelsGrid: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("AFE Channels")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)
                .padding(.horizontal, 4)

            GlassEffectContainer(spacing: 12) {
                LazyVGrid(columns: channelGridColumns, spacing: 14) {
                    let channels = connectionManager.lastStatus?.channels ?? defaultChannels()
                    let workerEnabled = connectionManager.lastSelftest?.workerEnabled ?? false
                    
                    ForEach(channels) { ch in
                        let isChannelCGreyed = ch.id == 2 && workerEnabled
                        let accent = isChannelCGreyed ? Color.gray : channelAccentColor(for: ch.id)
                        
                        Button(action: { selectedChannel = ch }) {
                            VStack(alignment: .leading, spacing: 8) {
                                HStack {
                                    Text("CH \(ch.id)")
                                        .font(.system(size: 14, weight: .bold))
                                        .foregroundColor(.white)
                                    Spacer()
                                    Image(systemName: "slider.horizontal.3")
                                        .font(.system(size: 12))
                                        .foregroundColor(.secondary)
                                }

                                Text(ch.function.replacingOccurrences(of: "CH_FUNC_", with: ""))
                                    .font(.system(size: 12, weight: .medium))
                                    .foregroundColor(.cyan)
                                    .lineLimit(1)

                                Group {
                                    if isChannelCGreyed {
                                        Text("RESERVED")
                                            .font(.system(size: 18, weight: .bold, design: .monospaced))
                                    } else {
                                        Text(String(format: "%.4f", ch.adcValue))
                                            .foregroundColor(.white)
                                        + Text("V")
                                            .foregroundColor(.secondary)
                                    }
                                }
                                .font(.system(size: 18, weight: .bold, design: .monospaced))

                                // Mini sparkline histogram
                                MiniSparklineView(values: connectionManager.channelHistory[ch.id] ?? [])
                                    .frame(height: 28)

                                HStack {
                                    Text("Raw: \(ch.adcRaw)")
                                        .font(.system(size: 10, design: .monospaced))
                                        .foregroundColor(.secondary)
                                    Spacer()
                                    Text(ch.function.contains("OUT") ? "DAC: \(ch.dacCode)" : "ADC")
                                        .font(.system(size: 9, weight: .bold))
                                        .foregroundColor(.secondary)
                                        .padding(.horizontal, 4)
                                        .padding(.vertical, 2)
                                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 4, style: .continuous))
                                }
                            }
                            .padding(14)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .glassEffect(.regular.tint(accent.opacity(0.35)), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                            .overlay(
                                RoundedRectangle(cornerRadius: 16, style: .continuous)
                                    .stroke(accent.opacity(0.6), lineWidth: 1.5)
                            )
                            .opacity(isChannelCGreyed ? 0.4 : 1.0)
                            .grayscale(isChannelCGreyed ? 1.0 : 0)
                        }
                        .disabled(isChannelCGreyed)
                    }
                }
            }
        }
    }

    // MARK: - I/V Plot Section

    var ivPlotSection: some View {
        GlassEffectContainer(spacing: 12) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("I/V Characterization")
                        .font(.system(size: 16, weight: .bold))
                        .foregroundColor(.white)
                    Text("Current source sweep")
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                }
                Spacer()
                Button(action: { showIVPlotConfig = true }) {
                    Text("Configure")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 6)
                        .glassEffect(.regular.tint(.cyan), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                }
            }
        }
    }

    // MARK: - Supplies Card

    var suppliesCard: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Power Rails & Control")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)

            // Live rail status tiles — driven by ioexp.enables for on/off truth
            GlassEffectContainer(spacing: 8) {
                HStack(spacing: 12) {
                    let rails = connectionManager.lastOverview?.rails ?? []
                    let enables = connectionManager.lastOverview?.ioexp.enables

                    let railNames = ["VADJ1", "VADJ2", "3V3_ADJ"]
                    // 3V3_ADJ (VLOGIC) is always on — it has no enable control
                    let railEnabled = [enables?.vadj1 ?? false, enables?.vadj2 ?? false, true]

                    ForEach(0..<3, id: \.self) { i in
                        let measuredV = rails.first(where: { $0.rail == i })?.voltage
                        let isOn = railEnabled[i]

                        VStack(alignment: .leading, spacing: 6) {
                            Text(railNames[i])
                                .font(.system(size: 11, weight: .semibold))
                                .foregroundColor(.secondary)
                            if isOn, let v = measuredV {
                                Text(String(format: "%.3f V", v))
                                    .font(.system(size: 16, weight: .bold, design: .monospaced))
                                    .foregroundColor(.green)
                            } else if isOn {
                                Text("ON")
                                    .font(.system(size: 16, weight: .bold, design: .monospaced))
                                    .foregroundColor(.green)
                            } else {
                                Text("OFF")
                                    .font(.system(size: 16, weight: .bold, design: .monospaced))
                                    .foregroundColor(.secondary)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(10)
                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                    }
                }
            }

            Divider().background(Color.white.opacity(0.1))

            let enables = connectionManager.lastOverview?.ioexp.enables
            let pg = connectionManager.lastOverview?.ioexp.powerGood

            AdaptiveRailStack(sizeClass: sizeClass) {
                // VADJ1: slider + enable
                VStack(spacing: 8) {
                    VoltageSliderRow(
                        label: "VADJ1 Target",
                        value: $vadj1Value,
                        range: 3.0...15.0,
                        step: 0.1,
                        isDirty: $vadj1Dirty,
                        onApply: { setRailVoltage(ch: 1, val: vadj1Value); vadj1Dirty = false }
                    )
                    .padding(12)
                    .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))

                    ToggleRow(title: "Enable VADJ1 Rail", isOn: enables?.vadj1 ?? false, pg: pg?.vadj1 ?? false) { state in
                        toggleControl("vadj1", on: state)
                    }
                }

                // VADJ2: slider + enable
                VStack(spacing: 8) {
                    VoltageSliderRow(
                        label: "VADJ2 Target",
                        value: $vadj2Value,
                        range: 3.0...15.0,
                        step: 0.1,
                        isDirty: $vadj2Dirty,
                        onApply: { setRailVoltage(ch: 2, val: vadj2Value); vadj2Dirty = false }
                    )
                    .padding(12)
                    .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))

                    ToggleRow(title: "Enable VADJ2 Rail", isOn: enables?.vadj2 ?? false, pg: pg?.vadj2 ?? false) { state in
                        toggleControl("vadj2", on: state)
                    }
                }

                // VLOGIC (3V3_ADJ) — always on, no separate enable
                VoltageSliderRow(
                    label: "VLOGIC Target",
                    value: $vlogicValue,
                    range: 1.7...5.0,
                    step: 0.05,
                    isDirty: $vlogicDirty,
                    onApply: { setRailVoltage(ch: 0, val: vlogicValue); vlogicDirty = false }
                )
                .padding(12)
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            }

            // HAT Rails (only shown when HAT detected)
            if hatPresent {
                Divider().background(Color.white.opacity(0.1))

                VStack(alignment: .leading, spacing: 14) {
                    Text("HAT Rails")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(.purple)

                    // Live HAT rail status tiles
                    let hatRailNames = ["VLOGIC", "VADJ3", "VADJ4"]
                    GlassEffectContainer(spacing: 8) {
                        HStack(spacing: 12) {
                            ForEach(0..<3, id: \.self) { i in
                                let rail = connectionManager.lastHatRails.first(where: { $0.railId == i })
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(hatRailNames[i])
                                        .font(.system(size: 11, weight: .semibold))
                                        .foregroundColor(.secondary)
                                    if let r = rail, r.enabled {
                                        Text(String(format: "%.3f V", Double(r.voltageMv) / 1000.0))
                                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                                            .foregroundColor(.purple)
                                        Text(String(format: "%d mA", r.currentMa))
                                            .font(.system(size: 11, design: .monospaced))
                                            .foregroundColor(.purple.opacity(0.7))
                                    } else {
                                        Text("OFF")
                                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                                            .foregroundColor(.secondary)
                                    }
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(10)
                                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                            }
                        }
                    }

                    AdaptiveRailStack(sizeClass: sizeClass) {
                        // HAT VLOGIC: slider + enable
                        let hatVlogicRail = connectionManager.lastHatRails.first(where: { $0.railId == 0 })
                        VStack(spacing: 8) {
                            VoltageSliderRow(label: "HAT VLOGIC", value: $hatVlogicValue, range: 1.7...5.0, step: 0.05,
                                isDirty: $hatVlogicDirty,
                                onApply: { setHatRailVoltage(railId: 0, val: hatVlogicValue); hatVlogicDirty = false })
                            .padding(12)
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))

                            ToggleRow(title: "Enable HAT VLOGIC", isOn: hatVlogicRail?.enabled ?? false, pg: true) { state in
                                toggleHatRailEnable(railId: 0, on: state)
                            }
                        }

                        // HAT VADJ3: slider + enable
                        let hatVadj3Rail = connectionManager.lastHatRails.first(where: { $0.railId == 1 })
                        VStack(spacing: 8) {
                            VoltageSliderRow(label: "HAT VADJ3", value: $hatVadj3Value, range: 0.0...36.0, step: 0.1,
                                isDirty: $hatVadj3Dirty,
                                onApply: { setHatRailVoltage(railId: 1, val: hatVadj3Value); hatVadj3Dirty = false })
                            .padding(12)
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))

                            ToggleRow(title: "Enable HAT VADJ3", isOn: hatVadj3Rail?.enabled ?? false, pg: true) { state in
                                toggleHatRailEnable(railId: 1, on: state)
                            }
                        }

                        // HAT VADJ4: slider + enable
                        let hatVadj4Rail = connectionManager.lastHatRails.first(where: { $0.railId == 2 })
                        VStack(spacing: 8) {
                            VoltageSliderRow(label: "HAT VADJ4", value: $hatVadj4Value, range: 0.0...36.0, step: 0.1,
                                isDirty: $hatVadj4Dirty,
                                onApply: { setHatRailVoltage(railId: 2, val: hatVadj4Value); hatVadj4Dirty = false })
                            .padding(12)
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))

                            ToggleRow(title: "Enable HAT VADJ4", isOn: hatVadj4Rail?.enabled ?? false, pg: true) { state in
                                toggleHatRailEnable(railId: 2, on: state)
                            }
                        }
                    }
                }
            }
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }

    // MARK: - Quick Presets

    var presetsCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Quick Presets")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)

            if quicksetupSlots.isEmpty {
                Text("No presets available.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 10) {
                        ForEach(quicksetupSlots) { slot in
                            Button(action: { applyPreset(slot.index) }) {
                                HStack {
                                    Image(systemName: "play.circle.fill")
                                        .foregroundColor(.black)
                                    Text(slot.name)
                                        .font(.system(size: 14, weight: .semibold))
                                        .foregroundColor(.black)
                                }
                                .padding(.horizontal, 14)
                                .padding(.vertical, 10)
                                .glassEffect(.regular.tint(.cyan), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                            }
                        }
                    }
                }
            }
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }

    // MARK: - Helpers

    private func updateSlidersFromModel() {
        guard let idac = connectionManager.lastOverview?.idac else { return }
        for ch in idac.channels {
            switch ch.id {
            case 0: if !vlogicDirty { vlogicValue = ch.targetV }
            case 1: if !vadj1Dirty  { vadj1Value  = ch.targetV }
            case 2: if !vadj2Dirty  { vadj2Value  = ch.targetV }
            default: break
            }
        }
    }

    private func updateHatSlidersFromModel() {
        for rail in connectionManager.lastHatRails {
            let v = Double(rail.configuredVoltageMv) / 1000.0
            guard v > 0.1 else { continue }
            switch rail.railId {
            case 0: if !hatVlogicDirty { hatVlogicValue = v }
            case 1: if !hatVadj3Dirty  { hatVadj3Value  = v }
            case 2: if !hatVadj4Dirty  { hatVadj4Value  = v }
            default: break
            }
        }
    }

    private func setRailVoltage(ch: Int, val: Double) {
        Task {
            _ = try? await connectionManager.postAction(path: "/api/idac/voltage", json: ["ch": ch, "voltage": val])
        }
    }

    private func setHatRailVoltage(railId: Int, val: Double) {
        Task {
            let mv = Int((val * 1000).rounded())
            _ = try? await connectionManager.postAction(path: "/api/hat/v2/rail/voltage", json: ["railId": railId, "voltageMv": mv])
            connectionManager.fetchHatRailsQuick()
        }
    }

    private func toggleHatRailEnable(railId: Int, on: Bool) {
        connectionManager.applyOptimisticHatRailEnable(railId: railId, enabled: on)
        Task {
            _ = try? await connectionManager.postAction(path: "/api/hat/v2/rail/enable", json: ["railId": railId, "enable": on])
            connectionManager.fetchHatRailsQuick()
        }
    }

    private func toggleControl(_ ctrl: String, on: Bool) {
        let key: String
        switch ctrl {
        case "vadj1":   key = "vadj1"
        case "vadj2":   key = "vadj2"
        case "15v":     key = "analog15v"
        case "mux":     key = "mux"
        case "usb":     key = "usbHub"
        default:        key = ctrl
        }
        connectionManager.applyOptimisticEnable(key: key, value: on)
        Task {
            _ = try? await connectionManager.postAction(path: "/api/ioexp/control", json: ["control": ctrl, "on": on])
            connectionManager.fetchOverviewQuick()
        }
    }

    private func loadQuicksetups() {
        Task {
            if let slots: [QuickSetupSlot] = try? await connectionManager.getRequest(path: "/api/quicksetup") {
                DispatchQueue.main.async {
                    self.quicksetupSlots = slots.filter { $0.occupied }
                }
            }
        }
    }

    private func applyPreset(_ slot: Int) {
        Task {
            _ = try? await connectionManager.postAction(path: "/api/quicksetup/\(slot)/apply", json: [:])
        }
    }

    private func defaultChannels() -> [ChannelState] {
        return (0..<4).map { i in
            ChannelState(id: i, function: "CH_FUNC_HIGH_IMP", functionCode: 0,
                         adcRaw: 0, adcValue: 0.0, adcRange: 0, adcRate: 0, adcMux: 0,
                         dacCode: 0, dacValue: 0.0, dinState: false, dinCounter: 0,
                         doState: false, alert: 0, alertMask: 0, rtdExcitationUa: 500)
        }
    }

    private func channelAccentColor(for id: Int) -> Color {
        let colors: [Color] = [
            Color(red: 0.23, green: 0.51, blue: 0.96), // Blue
            Color(red: 0.06, green: 0.73, blue: 0.51), // Emerald
            Color(red: 0.96, green: 0.62, blue: 0.04), // Amber
            Color(red: 0.66, green: 0.33, blue: 0.97)  // Purple
        ]
        return id < colors.count ? colors[id] : .secondary
    }
}

// MARK: - Voltage Slider Row with animated confirm button

struct VoltageSliderRow: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    @Binding var isDirty: Bool
    let onApply: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(label)
                    .font(.system(size: 14, weight: .semibold))
                Spacer()
                Text(String(format: "%.2f V", value))
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                    .foregroundColor(.cyan)
            }

            Slider(value: $value, in: range, step: step) { editing in
                if editing { isDirty = true }
            }
            .tint(.cyan)

            if isDirty {
                Button(action: onApply) {
                    HStack(spacing: 6) {
                        Image(systemName: "checkmark.circle.fill")
                            .font(.system(size: 14))
                        Text("Apply \(String(format: "%.2f V", value))")
                            .font(.system(size: 13, weight: .bold))
                    }
                    .foregroundColor(.black)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
                    .glassEffect(.regular.tint(.cyan), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                }
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(.spring(response: 0.3, dampingFraction: 0.7), value: isDirty)
    }
}

// MARK: - Mini Sparkline View

struct MiniSparklineView: View {
    let values: [Double]

    var body: some View {
        GeometryReader { geo in
            if values.count < 2 {
                Rectangle()
                    .fill(Color.white.opacity(0.05))
                    .cornerRadius(2)
            } else {
                let minV = values.min() ?? 0
                let maxV = values.max() ?? 1
                let range = maxV - minV == 0 ? 1.0 : maxV - minV
                let barWidth = geo.size.width / CGFloat(values.count)

                HStack(alignment: .bottom, spacing: 1) {
                    ForEach(Array(values.enumerated()), id: \.offset) { _, v in
                        let normalized = CGFloat((v - minV) / range)
                        let barHeight = max(2, normalized * geo.size.height)
                        Rectangle()
                            .fill(Color.cyan.opacity(0.6))
                            .frame(width: max(1, barWidth - 1), height: barHeight)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
            }
        }
    }
}

// MARK: - ToggleRow

struct ToggleRow: View {
    let title: String
    let isOn: Bool
    let pg: Bool
    var action: (Bool) -> Void

    var body: some View {
        Button(action: { action(!isOn) }) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text(title)
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(.primary)
                    HStack(spacing: 4) {
                        Circle()
                            .fill(pg ? Color.green : Color.red)
                            .frame(width: 6, height: 6)
                        Text(pg ? "PGood" : "Fault/Off")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                    }
                }
                Spacer()
                Image(systemName: isOn ? "checkmark.circle.fill" : "circle")
                    .foregroundColor(isOn ? .blue : .secondary)
                    .font(.system(size: 20))
            }
            .padding(10)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        }
    }
}

// MARK: - Channel Configuration Sheet

struct ChannelConfigSheet: View {
    let channel: ChannelState
    var onDismiss: () -> Void
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var selectedFuncIndex = 0
    @State private var targetVal = ""
    @State private var voutRange = 0
    @State private var adcRange = 0
    @State private var adcRate = 0
    @State private var rtdExcitation = 500
    @State private var isSaving = false

    let adcRangeOptions = [(0, "0–12 V"), (1, "±12 V"), (5, "0–2.5 V Diag")]
    let adcRateOptions  = [(0, "10 SPS"), (1, "20 SPS"), (2, "1200 SPS"), (3, "4800 SPS")]

    var body: some View {
        NavigationStack {
            ZStack {
                Color(red: 0.03, green: 0.05, blue: 0.10).ignoresSafeArea()

                ScrollView {
                    VStack(spacing: 16) {
                        // ── Channel Info ──
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Text("CH \(channel.id)")
                                    .font(.system(size: 22, weight: .bold, design: .rounded))
                                Spacer()
                                Text(channel.function.replacingOccurrences(of: "CH_FUNC_", with: ""))
                                    .font(.system(size: 13, weight: .semibold))
                                    .foregroundColor(.cyan)
                                    .padding(.horizontal, 10)
                                    .padding(.vertical, 4)
                                    .glassEffect(.regular.tint(.cyan), in: Capsule())
                            }
                            HStack(spacing: 20) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("ADC").font(.system(size: 10)).foregroundColor(.secondary)
                                    Text(String(format: "%.4f V", channel.adcValue))
                                        .font(.system(size: 16, weight: .bold, design: .monospaced))
                                }
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("RAW").font(.system(size: 10)).foregroundColor(.secondary)
                                    Text("\(channel.adcRaw)")
                                        .font(.system(size: 16, weight: .bold, design: .monospaced))
                                }
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("DAC").font(.system(size: 10)).foregroundColor(.secondary)
                                    Text(String(format: "%.3f V", channel.dacValue))
                                        .font(.system(size: 16, weight: .bold, design: .monospaced))
                                }
                                Spacer()
                            }
                            if channel.alert != 0 {
                                HStack(spacing: 6) {
                                    Image(systemName: "exclamationmark.triangle.fill")
                                        .foregroundColor(.red)
                                    Text("Alert: 0x\(String(channel.alert, radix: 16, uppercase: true))")
                                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                                        .foregroundColor(.red)
                                }
                            }
                        }
                        .padding()
                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))

                        // ── Function Mode ──
                        VStack(alignment: .leading, spacing: 10) {
                            Text("FUNCTION MODE")
                                .font(.system(size: 10, weight: .bold))
                                .foregroundColor(.secondary)
                            Picker("Function", selection: $selectedFuncIndex) {
                                ForEach(availableFunctions) { item in
                                    Text(item.name).tag(item.id)
                                }
                            }
                            .pickerStyle(.menu)
                            .accentColor(.cyan)
                        }
                        .padding()
                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))

                        // ── VOUT Settings ──
                        if selectedFuncIndex == 1 {
                            VStack(alignment: .leading, spacing: 10) {
                                Text("VOLTAGE OUTPUT")
                                    .font(.system(size: 10, weight: .bold))
                                    .foregroundColor(.secondary)
                                HStack {
                                    Text("Target Voltage")
                                        .font(.system(size: 14, weight: .medium))
                                    Spacer()
                                    TextField("0.000", text: $targetVal)
                                        .keyboardType(.decimalPad)
                                        .font(.system(size: 14, weight: .bold, design: .monospaced))
                                        .multilineTextAlignment(.trailing)
                                        .frame(width: 100)
                                    Text("V")
                                        .font(.system(size: 14))
                                        .foregroundColor(.secondary)
                                }
                                Divider().background(Color.white.opacity(0.1))
                                Text("OUTPUT RANGE")
                                    .font(.system(size: 10, weight: .bold))
                                    .foregroundColor(.secondary)
                                Picker("Range", selection: $voutRange) {
                                    Text("0–12 V Unipolar").tag(0)
                                    Text("±12 V Bipolar").tag(1)
                                }
                                .pickerStyle(.segmented)
                            }
                            .padding()
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                        }

                        // ── IOUT Settings ──
                        if selectedFuncIndex == 2 {
                            VStack(alignment: .leading, spacing: 10) {
                                Text("CURRENT OUTPUT")
                                    .font(.system(size: 10, weight: .bold))
                                    .foregroundColor(.secondary)
                                HStack {
                                    Text("Target Current")
                                        .font(.system(size: 14, weight: .medium))
                                    Spacer()
                                    TextField("0.00", text: $targetVal)
                                        .keyboardType(.decimalPad)
                                        .font(.system(size: 14, weight: .bold, design: .monospaced))
                                        .multilineTextAlignment(.trailing)
                                        .frame(width: 100)
                                    Text("mA")
                                        .font(.system(size: 14))
                                        .foregroundColor(.secondary)
                                }
                            }
                            .padding()
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                        }

                        // ── VIN / ADC Config ──
                        if selectedFuncIndex == 3 || selectedFuncIndex == 4 || selectedFuncIndex == 5 {
                            VStack(alignment: .leading, spacing: 10) {
                                Text("ADC CONFIGURATION")
                                    .font(.system(size: 10, weight: .bold))
                                    .foregroundColor(.secondary)
                                HStack {
                                    Text("Range")
                                        .font(.system(size: 14, weight: .medium))
                                    Spacer()
                                    Picker("Range", selection: $adcRange) {
                                        ForEach(adcRangeOptions, id: \.0) { opt in
                                            Text(opt.1).tag(opt.0)
                                        }
                                    }
                                    .pickerStyle(.menu)
                                    .accentColor(.cyan)
                                }
                                HStack {
                                    Text("Sample Rate")
                                        .font(.system(size: 14, weight: .medium))
                                    Spacer()
                                    Picker("Rate", selection: $adcRate) {
                                        ForEach(adcRateOptions, id: \.0) { opt in
                                            Text(opt.1).tag(opt.0)
                                        }
                                    }
                                    .pickerStyle(.menu)
                                    .accentColor(.cyan)
                                }
                            }
                            .padding()
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                        }

                        // ── RTD Config ──
                        if selectedFuncIndex == 7 {
                            VStack(alignment: .leading, spacing: 10) {
                                Text("RTD CONFIGURATION")
                                    .font(.system(size: 10, weight: .bold))
                                    .foregroundColor(.secondary)
                                Text("Excitation Current")
                                    .font(.system(size: 14, weight: .medium))
                                Picker("Excitation", selection: $rtdExcitation) {
                                    Text("500 µA").tag(500)
                                    Text("1000 µA").tag(1000)
                                }
                                .pickerStyle(.segmented)
                            }
                            .padding()
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                        }

                        // ── Apply Button ──
                        Button(action: { saveSettings() }) {
                            HStack {
                                if isSaving {
                                    ProgressView().tint(.white)
                                } else {
                                    Image(systemName: "checkmark.circle.fill")
                                }
                                Text(isSaving ? "Applying…" : "Apply Configuration")
                                    .fontWeight(.bold)
                            }
                            .foregroundColor(.white)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 12)
                            .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                        }
                        .disabled(isSaving)
                    }
                    .padding()
                }
            }
            .navigationTitle("Configure CH \(channel.id)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { onDismiss() }
                }
            }
            .onAppear {
                selectedFuncIndex = channel.functionCode
                voutRange = channel.adcRange > 0 ? 1 : 0
                adcRange = channel.adcRange
                adcRate = channel.adcRate
                rtdExcitation = channel.rtdExcitationUa ?? 500
                if selectedFuncIndex == 1 {
                    targetVal = String(format: "%.3f", channel.dacValue)
                } else if selectedFuncIndex == 2 {
                    targetVal = String(format: "%.2f", channel.dacValue)
                }
            }
        }
        .preferredColorScheme(.dark)
    }

    private func saveSettings() {
        let funcIndex = selectedFuncIndex
        let valString = targetVal
        let chId = channel.id
        let vRange = voutRange
        let aRange = adcRange
        let aRate = adcRate
        let rtdUa = rtdExcitation
        isSaving = true

        Task {
            // 1. Set function mode
            let funcOk = try? await connectionManager.postAction(
                path: "/api/channel/\(chId)/function",
                json: ["function": funcIndex]
            )

            // 2. Sub-configurations
            if funcIndex == 1 {
                // VOUT: set range, then DAC voltage
                _ = try? await connectionManager.postAction(
                    path: "/api/channel/\(chId)/vout/range",
                    json: ["range": vRange]
                )
                if let val = Double(valString) {
                    _ = try? await connectionManager.postAction(
                        path: "/api/channel/\(chId)/dac",
                        json: ["voltage": val]
                    )
                }
            } else if funcIndex == 2 {
                // IOUT: set DAC current
                if let val = Double(valString) {
                    _ = try? await connectionManager.postAction(
                        path: "/api/channel/\(chId)/dac",
                        json: ["current_mA": val]
                    )
                }
            } else if funcIndex == 3 || funcIndex == 4 || funcIndex == 5 {
                // VIN / Current Input: set ADC config
                _ = try? await connectionManager.postAction(
                    path: "/api/channel/\(chId)/adc/config",
                    json: ["range": aRange, "rate": aRate]
                )
            } else if funcIndex == 7 {
                // RTD: set excitation
                _ = try? await connectionManager.postAction(
                    path: "/api/channel/\(chId)/rtd/config",
                    json: ["excitationUa": rtdUa]
                )
            }

            await MainActor.run {
                isSaving = false
                connectionManager.showToast(funcOk == true ? "CH \(chId) configured" : "Configuration failed", type: funcOk == true ? .success : .error)
                onDismiss()
            }
        }
    }
}

// MARK: - IV Plot Views

struct IVPoint: Identifiable {
    let id = UUID()
    let currentMa: Double
    let voltageV: Double
}

struct IVPlotConfigSheet: View {
    @Environment(\.dismiss) var dismiss
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var selectedChannel = 0
    @State private var iRangeFrom: Double = 0.0
    @State private var iRangeTo: Double = 20.0
    @State private var steps: Double = 20.0
    @State private var settleTimeMs: Double = 50.0
    @State private var showingPlot = false
    
    var body: some View {
        NavigationStack {
            Form {
                Section("Source Channel") {
                    Picker("Channel", selection: $selectedChannel) {
                        Text("CH 0").tag(0)
                        Text("CH 1").tag(1)
                        Text("CH 2").tag(2)
                        Text("CH 3").tag(3)
                    }
                    .pickerStyle(.segmented)
                }
                
                Section("Current Range (mA)") {
                    VStack(alignment: .leading) {
                        Text("From: \(String(format: "%.1f mA", iRangeFrom))")
                        Slider(value: $iRangeFrom, in: 0...25, step: 0.1)
                    }
                    VStack(alignment: .leading) {
                        Text("To: \(String(format: "%.1f mA", iRangeTo))")
                        Slider(value: $iRangeTo, in: 0...25, step: 0.1)
                    }
                }
                
                Section("Measurement Settings") {
                    VStack(alignment: .leading) {
                        Text("Steps: \(Int(steps))")
                        Slider(value: $steps, in: 5...5000, step: 1)
                    }
                    VStack(alignment: .leading) {
                        Text("Settle Time: \(Int(settleTimeMs)) ms")
                        Slider(value: $settleTimeMs, in: 10...500, step: 10)
                    }
                }
                
                Section {
                    Button(action: { showingPlot = true }) {
                        Text("Start Measurement")
                            .bold()
                            .foregroundColor(.white)
                            .frame(maxWidth: .infinity, alignment: .center)
                    }
                    .listRowBackground(Color.cyan)
                }
            }
            .navigationTitle("I/V Plot Configuration")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
            .navigationDestination(isPresented: $showingPlot) {
                IVPlotDataView(
                    channel: selectedChannel,
                    iRangeFrom: iRangeFrom,
                    iRangeTo: iRangeTo,
                    steps: Int(steps),
                    settleTimeMs: Int(settleTimeMs)
                )
            }
        }
    }
}

struct IVPlotDataView: View {
    @Environment(\.dismiss) var dismiss
    @EnvironmentObject var connectionManager: ConnectionManager
    let channel: Int
    let iRangeFrom: Double
    let iRangeTo: Double
    let steps: Int
    let settleTimeMs: Int
    
    @State private var points: [IVPoint] = []
    @State private var isMeasuring = true
    
    var body: some View {
        ZStack {
            Color(red: 0.03, green: 0.05, blue: 0.10).ignoresSafeArea()
            
            VStack(alignment: .leading, spacing: 20) {
                if points.isEmpty && isMeasuring {
                    VStack(spacing: 16) {
                        ProgressView().scaleEffect(1.5).tint(.cyan)
                        Text("Initializing I/V Sweep...")
                            .foregroundColor(.secondary)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    Text("I/V Characteristic (CH \(channel))")
                        .font(.title3)
                        .bold()
                        .foregroundColor(.white)
                        .padding(.horizontal)
                    
                    Chart {
                        ForEach(points) { pt in
                            LineMark(
                                x: .value("Current (mA)", pt.currentMa),
                                y: .value("Voltage (V)", pt.voltageV)
                            )
                            .foregroundStyle(Color.cyan)
                            .interpolationMethod(.linear)
                            
                            PointMark(
                                x: .value("Current (mA)", pt.currentMa),
                                y: .value("Voltage (V)", pt.voltageV)
                            )
                            .foregroundStyle(Color.blue)
                        }
                    }
                    .chartXAxis {
                        AxisMarks(values: .automatic) { _ in
                            AxisGridLine(stroke: StrokeStyle(lineWidth: 0.5, dash: [2, 4])).foregroundStyle(.white.opacity(0.1))
                            AxisValueLabel().foregroundStyle(.secondary)
                        }
                    }
                    .chartYAxis {
                        AxisMarks(values: .automatic) { _ in
                            AxisGridLine(stroke: StrokeStyle(lineWidth: 0.5, dash: [2, 4])).foregroundStyle(.white.opacity(0.1))
                            AxisValueLabel().foregroundStyle(.secondary)
                        }
                    }
                    .frame(height: 300)
                    .padding()
                    .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                    .padding(.horizontal)
                    
                    if isMeasuring {
                        HStack {
                            ProgressView().tint(.cyan)
                            Text("Measuring: \(points.count)/\(steps + 1) points")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                        .padding(.horizontal)
                    }
                    
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Data Points").font(.headline).padding(.horizontal)
                        ScrollView {
                            LazyVStack(spacing: 8) {
                                ForEach(points) { pt in
                                    HStack {
                                        Text(String(format: "%.2f mA", pt.currentMa))
                                            .font(.system(.subheadline, design: .monospaced))
                                        Spacer()
                                        Text(String(format: "%.4f V", pt.voltageV))
                                            .font(.system(.subheadline, design: .monospaced))
                                            .foregroundColor(.cyan)
                                    }
                                    .padding(.horizontal)
                                    .padding(.vertical, 8)
                                    .background(Color.white.opacity(0.05))
                                    .cornerRadius(8)
                                    .padding(.horizontal)
                                }
                            }
                        }
                    }
                }
            }
            .padding(.vertical)
        }
        .navigationTitle("I/V Plot")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            startMeasurement()
        }
    }
    
    private func startMeasurement() {
        points = []
        isMeasuring = true
        Task {
            // Set function to Current Output (IOUT = 2)
            _ = try? await connectionManager.postAction(
                path: "/api/channel/\(channel)/function",
                json: ["function": 2]
            )
            
            try? await Task.sleep(nanoseconds: 150_000_000) // 150ms for function setup
            
            let stepMa = steps > 0 ? (iRangeTo - iRangeFrom) / Double(steps) : 0
            
            for i in 0...steps {
                guard isMeasuring else { break }
                let current = iRangeFrom + (stepMa * Double(i))
                
                // Set DAC current
                _ = try? await connectionManager.postAction(
                    path: "/api/channel/\(channel)/dac",
                    json: ["current_mA": current]
                )
                
                // Wait for settle
                try? await Task.sleep(nanoseconds: UInt64(settleTimeMs) * 1_000_000)
                
                // Get fresh ADC reading
                if let adcResp: ChannelAdcResponse = try? await connectionManager.getRequest(path: "/api/channel/\(channel)/adc") {
                    let point = IVPoint(currentMa: current, voltageV: adcResp.adcValue)
                    await MainActor.run {
                        points.append(point)
                    }
                }
            }
            
            // Revert channel to High-Z (0) when done
            _ = try? await connectionManager.postAction(
                path: "/api/channel/\(channel)/function",
                json: ["function": 0]
            )
            
            await MainActor.run {
                isMeasuring = false
            }
        }
    }
}
