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
    
    // Sliders local states
    @State private var vadj1Value: Double = 3.3
    @State private var vadj2Value: Double = 3.3
    @State private var quicksetupSlots: [QuickSetupSlot] = []
    
    struct QuickSetupSlot: Identifiable, Codable {
        var id: Int { index }
        let index: Int
        let occupied: Bool
        let name: String
    }
    
    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                headerSection
                
                suppliesCard
                
                channelsGrid
                
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
        .sheet(item: $selectedChannel) { channel in
            ChannelConfigSheet(channel: channel) {
                selectedChannel = nil
            }
            .environmentObject(connectionManager)
        }
    }
    
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
            
            // Connected pill
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
                
                Button(action: {
                    connectionManager.disconnect()
                }) {
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
    
    var suppliesCard: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Power Rails & Control")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)
            
            // Live measurements
            HStack(spacing: 12) {
                let rails = connectionManager.lastOverview?.rails ?? []
                ForEach(0..<3) { i in
                    let railName = i == 0 ? "VADJ1" : (i == 1 ? "VADJ2" : "3V3_ADJ")
                    let volts = rails.first(where: { $0.rail == i })?.voltage ?? -1.0
                    
                    VStack(alignment: .leading, spacing: 6) {
                        Text(railName)
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundColor(.secondary)
                        Text(volts >= 0 ? String(format: "%.3f V", volts) : "OFF")
                            .font(.system(size: 16, weight: .bold, design: .monospaced))
                            .foregroundColor(volts >= 0 ? .green : .secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10)
                    .background(Color.white.opacity(0.03))
                    .cornerRadius(10)
                }
            }
            
            Divider()
                .background(Color.white.opacity(0.1))
            
            // Adjustments
            VStack(spacing: 14) {
                // VADJ1
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text("VADJ1 Target")
                            .font(.system(size: 14, weight: .semibold))
                        Spacer()
                        Text(String(format: "%.2f V", vadj1Value))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                            .foregroundColor(.cyan)
                    }
                    Slider(value: $vadj1Value, in: 0.8...5.0, step: 0.05) { editing in
                        if !editing {
                            setRailVoltage(ch: 1, val: vadj1Value)
                        }
                    }
                    .tint(.cyan)
                }
                
                // VADJ2
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text("VADJ2 Target")
                            .font(.system(size: 14, weight: .semibold))
                        Spacer()
                        Text(String(format: "%.2f V", vadj2Value))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                            .foregroundColor(.cyan)
                    }
                    Slider(value: $vadj2Value, in: 0.8...5.0, step: 0.05) { editing in
                        if !editing {
                            setRailVoltage(ch: 2, val: vadj2Value)
                        }
                    }
                    .tint(.cyan)
                }
            }
            
            Divider()
                .background(Color.white.opacity(0.1))
            
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
    
    var channelsGrid: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("AFE Channels")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)
                .padding(.horizontal, 4)
            
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 14) {
                let channels = connectionManager.lastStatus?.channels ?? defaultChannels()
                ForEach(channels) { ch in
                    Button(action: {
                        selectedChannel = ch
                    }) {
                        VStack(alignment: .leading, spacing: 10) {
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
                            Button(action: {
                                applyPreset(slot.index)
                            }) {
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
    
    private func updateSlidersFromModel() {
        if let idac = connectionManager.lastOverview?.idac {
            if let ch1 = idac.channels.first(where: { $0.id == 1 }) {
                vadj1Value = ch1.targetV
            }
            if let ch2 = idac.channels.first(where: { $0.id == 2 }) {
                vadj2Value = ch2.targetV
            }
        }
    }
    
    private func setRailVoltage(ch: Int, val: Double) {
        Task {
            _ = try? await connectionManager.postAction(path: "/api/idac/voltage", json: ["ch": ch, "voltage": val])
        }
    }
    
    private func toggleControl(_ ctrl: String, on: Bool) {
        Task {
            _ = try? await connectionManager.postAction(path: "/api/ioexp/control", json: ["control": ctrl, "on": on])
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
            ChannelState(
                id: i,
                function: "CH_FUNC_HIGH_IMP",
                functionCode: 0,
                adcRaw: 0,
                adcValue: 0.0,
                adcRange: 0,
                adcRate: 0,
                adcMux: 0,
                dacCode: 0,
                dacValue: 0.0,
                dinState: false,
                dinCounter: 0,
                doState: false,
                channelAlert: 0,
                channelAlertMask: 0,
                rtdExcitationUa: 500.0
            )
        }
    }
}

struct ToggleRow: View {
    let title: String
    let isOn: Bool
    let pg: Bool
    var action: (Bool) -> Void
    
    var body: some View {
        Button(action: {
            action(!isOn)
        }) {
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

// Channel Configuration Sheet
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
                    Button(action: {
                        saveSettings()
                    }) {
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
                    Button("Close") {
                        onDismiss()
                    }
                }
            }
            .onAppear {
                selectedFuncIndex = channel.functionCode
                if selectedFuncIndex == 1 {
                    targetVal = String(format: "%.2f", channel.dacValue)
                } else if selectedFuncIndex == 2 || selectedFuncIndex == 10 {
                    targetVal = String(format: "%.2f", channel.dacValue)
                }
            }
        }
    }
    
    private func saveSettings() {
        Task {
            // Apply function mode
            _ = try? await connectionManager.postAction(
                path: "/api/channel/\(channel.id)/function",
                json: ["function": selectedFuncIndex]
            )
            
            // If VOUT / IOUT, set target dac value
            if (selectedFuncIndex == 1 || selectedFuncIndex == 2 || selectedFuncIndex == 10),
               let val = Double(targetVal) {
                let key = selectedFuncIndex == 1 ? "voltage" : "current_mA"
                _ = try? await connectionManager.postAction(
                    path: "/api/channel/\(channel.id)/dac",
                    json: [key: val]
                )
            }
            
            DispatchQueue.main.async {
                onDismiss()
            }
        }
    }
}
