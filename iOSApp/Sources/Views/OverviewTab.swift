import SwiftUI

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

    // Per-channel ADC history for sparklines (last 60 samples)
    @State private var channelHistory: [Int: [Double]] = [:]


    @State private var quicksetupSlots: [QuickSetupSlot] = []

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

                suppliesCard

                presetsCard

                Spacer(minLength: 100)
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
        }
        .onChange(of: connectionManager.lastOverview?.idac) { _ in
            updateSlidersFromModel()
        }
        .onChange(of: connectionManager.lastHatRails) { _ in
            updateHatSlidersFromModel()
        }
        .onChange(of: connectionManager.lastStatus) { newStatus in
            guard let channels = newStatus?.channels else { return }
            for ch in channels {
                var history = channelHistory[ch.id] ?? []
                history.append(ch.adcValue)
                if history.count > 60 { history.removeFirst(history.count - 60) }
                channelHistory[ch.id] = history
            }
        }
        .sheet(item: $selectedChannel) { channel in
            ChannelConfigSheet(channel: channel) {
                selectedChannel = nil
            }
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
                        .foregroundColor(.green)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(Color.green.opacity(0.1))
                .cornerRadius(12)

                Button(action: { connectionManager.disconnect() }) {
                    Image(systemName: "power")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundColor(.red)
                        .padding(8)
                        .background(Color.red.opacity(0.1))
                        .clipShape(Circle())
                }
            }
        }
        .padding(.top, 10)
    }

    // MARK: - AFE Channels Grid (with sparklines)

    var channelsGrid: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("AFE Channels")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)
                .padding(.horizontal, 4)

            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 14) {
                let channels = connectionManager.lastStatus?.channels ?? defaultChannels()
                ForEach(channels) { ch in
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

                            Text(String(format: "%.4f", ch.adcValue))
                                .font(.system(size: 18, weight: .bold, design: .monospaced))
                                .foregroundColor(.white)

                            // Mini sparkline histogram
                            MiniSparklineView(values: channelHistory[ch.id] ?? [])
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
                                    .background(Color.white.opacity(0.05))
                                    .cornerRadius(4)
                            }
                        }
                        .padding(14)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(.ultraThinMaterial)
                        .cornerRadius(16)
                        .overlay(
                            RoundedRectangle(cornerRadius: 16)
                                .stroke(Color.white.opacity(0.06), lineWidth: 1)
                        )
                    }
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
            HStack(spacing: 12) {
                let rails = connectionManager.lastOverview?.rails ?? []
                let enables = connectionManager.lastOverview?.ioexp.enables

                let railNames = ["VADJ1", "VADJ2", "3V3_ADJ"]
                let railEnabled = [enables?.vadj1 ?? false, enables?.vadj2 ?? false, enables?.analog15v ?? false]

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
                    .background(Color.white.opacity(0.03))
                    .cornerRadius(10)
                }
            }

            Divider().background(Color.white.opacity(0.1))

            // Voltage adjustments
            VStack(spacing: 18) {
                // VADJ1
                VoltageSliderRow(
                    label: "VADJ1 Target",
                    value: $vadj1Value,
                    range: 3.0...15.0,
                    step: 0.1,
                    isDirty: $vadj1Dirty,
                    onApply: { setRailVoltage(ch: 1, val: vadj1Value); vadj1Dirty = false }
                )

                // VADJ2
                VoltageSliderRow(
                    label: "VADJ2 Target",
                    value: $vadj2Value,
                    range: 3.0...15.0,
                    step: 0.1,
                    isDirty: $vadj2Dirty,
                    onApply: { setRailVoltage(ch: 2, val: vadj2Value); vadj2Dirty = false }
                )

                // VLOGIC (3V3_ADJ)
                VoltageSliderRow(
                    label: "VLOGIC Target",
                    value: $vlogicValue,
                    range: 1.7...5.0,
                    step: 0.05,
                    isDirty: $vlogicDirty,
                    onApply: { setRailVoltage(ch: 0, val: vlogicValue); vlogicDirty = false }
                )
            }

            // HAT Rails (only shown when HAT detected)
            if hatPresent {
                Divider().background(Color.white.opacity(0.1))

                VStack(alignment: .leading, spacing: 14) {
                    Text("HAT Rails")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundColor(.purple)

                    // Measured voltage + current tiles
                    let hatRailNames = ["VLOGIC", "VADJ3", "VADJ4"]
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
                            .background(Color.purple.opacity(0.05))
                            .cornerRadius(10)
                        }
                    }

                    // Enable toggles
                    HStack(spacing: 12) {
                        ForEach(0..<3, id: \.self) { i in
                            let rail = connectionManager.lastHatRails.first(where: { $0.railId == i })
                            ToggleRow(title: "Enable \(hatRailNames[i])", isOn: rail?.enabled ?? false, pg: true) { state in
                                toggleHatRailEnable(railId: i, on: state)
                            }
                        }
                    }

                    // Voltage sliders
                    VoltageSliderRow(label: "HAT VLOGIC", value: $hatVlogicValue, range: 1.7...5.0, step: 0.05,
                        isDirty: $hatVlogicDirty,
                        onApply: { setHatRailVoltage(railId: 0, val: hatVlogicValue); hatVlogicDirty = false })
                    VoltageSliderRow(label: "HAT VADJ3", value: $hatVadj3Value, range: 0.0...36.0, step: 0.1,
                        isDirty: $hatVadj3Dirty,
                        onApply: { setHatRailVoltage(railId: 1, val: hatVadj3Value); hatVadj3Dirty = false })
                    VoltageSliderRow(label: "HAT VADJ4", value: $hatVadj4Value, range: 0.0...36.0, step: 0.1,
                        isDirty: $hatVadj4Dirty,
                        onApply: { setHatRailVoltage(railId: 2, val: hatVadj4Value); hatVadj4Dirty = false })
                }
            }

            Divider().background(Color.white.opacity(0.1))

            // PCA Enables
            let enables = connectionManager.lastOverview?.ioexp.enables
            let pg = connectionManager.lastOverview?.ioexp.powerGood

            VStack(spacing: 12) {
                HStack {
                    ToggleRow(title: "Enable VADJ1 Rail", isOn: enables?.vadj1 ?? false, pg: pg?.vadj1 ?? false) { state in
                        toggleControl("vadj1", on: state)
                    }
                    ToggleRow(title: "Enable VADJ2 Rail", isOn: enables?.vadj2 ?? false, pg: pg?.vadj2 ?? false) { state in
                        toggleControl("vadj2", on: state)
                    }
                }
                HStack {
                    ToggleRow(title: "Enable Analog 15V", isOn: enables?.analog15v ?? false, pg: pg?.logic ?? false) { state in
                        toggleControl("15v", on: state)
                    }
                    ToggleRow(title: "Enable Signal Mux", isOn: enables?.mux ?? false, pg: true) { state in
                        toggleControl("mux", on: state)
                    }
                }
                ToggleRow(title: "Enable USB Hub Controller", isOn: enables?.usbHub ?? false, pg: true) { state in
                    toggleControl("usb", on: state)
                }
            }
        }
        .padding()
        .background(.ultraThinMaterial)
        .cornerRadius(20)
        .overlay(
            RoundedRectangle(cornerRadius: 20)
                .stroke(Color.white.opacity(0.05), lineWidth: 1)
        )
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
                                        .foregroundColor(.cyan)
                                    Text(slot.name)
                                        .font(.system(size: 14, weight: .semibold))
                                        .foregroundColor(.white)
                                }
                                .padding(.horizontal, 14)
                                .padding(.vertical, 10)
                                .background(Color.cyan.opacity(0.1))
                                .cornerRadius(12)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 12)
                                        .stroke(Color.cyan.opacity(0.2), lineWidth: 1)
                                )
                            }
                        }
                    }
                }
            }
        }
        .padding()
        .background(.ultraThinMaterial)
        .cornerRadius(20)
        .overlay(
            RoundedRectangle(cornerRadius: 20)
                .stroke(Color.white.opacity(0.05), lineWidth: 1)
        )
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
            let v = Double(rail.voltageMv) / 1000.0
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
                         doState: false, channelAlert: 0, channelAlertMask: 0, rtdExcitationUa: 500.0)
        }
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
                    .background(Color.cyan)
                    .cornerRadius(10)
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
            .background(Color.white.opacity(0.04))
            .cornerRadius(12)
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

    var body: some View {
        NavigationStack {
            Form {
                Section("Channel Information") {
                    LabeledContent("Channel ID", value: "\(channel.id)")
                    LabeledContent("Current Function", value: channel.function.replacingOccurrences(of: "CH_FUNC_", with: ""))
                    LabeledContent("ADC Value", value: String(format: "%.4f", channel.adcValue))
                }

                Section("Function Mode Setup") {
                    Picker("Function Mode", selection: $selectedFuncIndex) {
                        ForEach(availableFunctions) { item in
                            Text(item.name).tag(item.id)
                        }
                    }
                    .pickerStyle(.menu)
                }

                if selectedFuncIndex == 1 || selectedFuncIndex == 2 || selectedFuncIndex == 10 {
                    Section("DAC Output Settings") {
                        TextField(selectedFuncIndex == 1 ? "Target Voltage (V)" : "Target Current (mA)", text: $targetVal)
                            .keyboardType(.decimalPad)
                    }
                }

                Section {
                    Button(action: { saveSettings() }) {
                        Text("Apply Configurations")
                            .bold()
                            .foregroundColor(.white)
                            .frame(maxWidth: .infinity, alignment: .center)
                    }
                    .listRowBackground(Color.blue)
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
                if selectedFuncIndex == 1 || selectedFuncIndex == 2 || selectedFuncIndex == 10 {
                    targetVal = String(format: "%.2f", channel.dacValue)
                }
            }
        }
    }

    private func saveSettings() {
        Task {
            _ = try? await connectionManager.postAction(
                path: "/api/channel/\(channel.id)/function",
                json: ["function": selectedFuncIndex]
            )
            if (selectedFuncIndex == 1 || selectedFuncIndex == 2 || selectedFuncIndex == 10),
               let val = Double(targetVal) {
                let key = selectedFuncIndex == 1 ? "voltage" : "current_mA"
                _ = try? await connectionManager.postAction(
                    path: "/api/channel/\(channel.id)/dac",
                    json: [key: val]
                )
            }
            DispatchQueue.main.async { onDismiss() }
        }
    }
}
