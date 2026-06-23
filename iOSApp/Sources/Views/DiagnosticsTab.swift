import SwiftUI
import Charts

struct WifiNetworkScanItem: Identifiable, Codable {
    var id: String { ssid }
    let ssid: String
    let rssi: Int
    let auth: Int
}

struct OtaUpdateStatus: Codable {
    let status: String
    let progress: Int?
    let version: String?
}

struct GitRelease: Identifiable {
    let id: String
    let tag: String
    let esp32Version: String
    let esp32Available: Bool
    let hatAvailable: Bool
    let spiffsAvailable: Bool
    let publishedAt: String
}

struct DiagnosticsTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var isScanningWifi = false
    @State private var wifiNetworks: [WifiNetworkScanItem] = []
    @State private var selectedSSID = ""
    @State private var wifiPassword = ""
    @State private var showWifiSheet = false
    @State private var wifiConnectStatus: String? = nil

    // OTA States
    @State private var gitReleases: [GitRelease] = []
    @State private var isFetchingReleases = false
    @State private var selectedEsp32Tag = ""
    @State private var selectedHatTag = ""
    @State private var updateEsp32 = true
    @State private var updateHat = false
    @State private var isApplyingOta = false
    @State private var otaApplyStatus = ""

    // USB PD
    @State private var isNegotiatingPd = false

    // Calibration
    @State private var selectedCalSupply = 1
    @State private var showingCalData = false
    @State private var loadedCalPoints: [CalibrationPoint] = []
    @State private var loadedHatCal: HatCalibration? = nil
    @State private var isLoadingCalData = false
    // Mainboard IDAC channels are tagged 0-2; HAT rails use a 100+railId offset
    // so a single picker can drive both calibration backends.
    static let hatRailTagOffset = 100
    var calSupplies: [(id: Int, name: String)] {
        var opts: [(id: Int, name: String)] = [
            (id: 0, name: "LevelShift (IDAC0)"),
            (id: 1, name: "V_ADJ1 (IDAC1)"),
            (id: 2, name: "V_ADJ2 (IDAC2)")
        ]
        if hatPresent {
            opts.append((id: Self.hatRailTagOffset + 1, name: "HAT VADJ3"))
            opts.append((id: Self.hatRailTagOffset + 2, name: "HAT VADJ4"))
        }
        return opts
    }

    // SPIFFS
    @State private var spiffsStorage: StorageInfoDiag? = nil
    @State private var isLoadingSpiffs = false

    // Auto-Calibration
    @State private var calChannel = 0
    @State private var isCalibrating = false
    @State private var calResult: String? = nil

    // Cached supply voltages from selftest worker
    @State private var cachedSupplies: SelftestSupplyCached? = nil

    // AD74416H Internal Diagnostics
    @State private var internalSupplies: [InternalSupplyEntry]? = nil
    @State private var isLoadingInternalSupplies = false

    var hatPresent: Bool { connectionManager.lastHatStatus?.isPresent ?? false }

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                headerSection
                selfTestCard
                internalSuppliesCard
                registersCard
                usbPdCard
                calibrationCard
                wifiCard
                otaCard
                spiffsCard
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
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showWifiSheet) { wifiConnectionSheet }
        .sheet(isPresented: $showingCalData) {
            CalibrationDataView(supplyName: calSupplies.first(where: { $0.id == selectedCalSupply })?.name ?? "Unknown", points: loadedCalPoints, hatCal: loadedHatCal)
        }
        .onAppear {
            fetchGitReleases()
            loadSpiffsStorage()
            fetchCachedSupplies()
            fetchInternalSupplies()
            connectionManager.fetchDeviceVersion()
            connectionManager.fetchWifiStatus()
        }
    }

    // MARK: - Header

    var headerSection: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Diagnostics & Config")
                    .font(.system(size: 28, weight: .bold, design: .rounded))
                Text("Monitor hardware alerts and system states")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
                if let ver = connectionManager.lastDeviceVersion {
                    HStack(spacing: 8) {
                        if let esp = ver.esp32 {
                            Text("ESP32: \(esp)").font(.system(size: 11, weight: .bold, design: .monospaced)).foregroundColor(.cyan)
                        }
                        if let hat = ver.hat {
                            Text("HAT: \(hat)").font(.system(size: 11, weight: .bold, design: .monospaced)).foregroundColor(.purple)
                        }
                    }
                }
            }
            Spacer()
        }
        .padding(.top, 10)
    }

    // MARK: - Self-Test Card

    var selfTestCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Self-Test & Calibration")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                Button(action: { fetchCachedSupplies() }) {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 14))
                }
                .padding(8)
                .glassEffect(.regular, in: Circle())
            }

            let selftest = connectionManager.lastSelftest

            // Worker + Supply Monitor controls
            HStack {
                Text("Worker Thread Monitor")
                    .font(.system(size: 14, weight: .medium))
                Spacer()
                Toggle("", isOn: Binding(
                    get: { selftest?.workerEnabled ?? false },
                    set: { toggleWorkerState($0) }
                ))
                .toggleStyle(SwitchToggleStyle(tint: .cyan))
                .labelsHidden()
            }

            HStack(spacing: 8) {
                StatusPill(title: selftest?.supplyMonitorActive == true ? "Monitor Active" : "Monitor Idle",
                           ok: selftest?.supplyMonitorActive ?? false)
                if selftest?.supplyMonitorActive == true {
                    Text("CH D reserved for supply monitoring")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                }
                Spacer()
            }

            Divider().background(Color.white.opacity(0.1))

            // ── Boot Self-Test Results ──
            VStack(alignment: .leading, spacing: 8) {
                Text("BOOT SELF-TEST")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.secondary)

                HStack(spacing: 12) {
                    StatusPill(title: "Ran", ok: selftest?.boot.ran ?? false)
                    StatusPill(title: selftest?.boot.passed == true ? "Passed" : "Failed",
                               ok: selftest?.boot.passed ?? false)
                }

                HStack(spacing: 12) {
                    VStack(alignment: .leading) {
                        Text("VADJ1").font(.system(size: 10)).foregroundColor(.secondary)
                        Text(String(format: "%.3f V", selftest?.boot.vadj1V ?? 0.0))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)

                    VStack(alignment: .leading) {
                        Text("VADJ2").font(.system(size: 10)).foregroundColor(.secondary)
                        Text(String(format: "%.3f V", selftest?.boot.vadj2V ?? 0.0))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)

                    VStack(alignment: .leading) {
                        Text("VLOGIC").font(.system(size: 10)).foregroundColor(.secondary)
                        Text(String(format: "%.3f V", selftest?.boot.vlogicV ?? 0.0))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .padding(.top, 4)
            }

            Divider().background(Color.white.opacity(0.1))

            // ── Live Supply Monitor Readings ──
            VStack(alignment: .leading, spacing: 8) {
                Text("SUPPLY MONITOR (LIVE)")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.secondary)

                if let supplies = cachedSupplies, supplies.available {
                    HStack(spacing: 12) {
                        ForEach(supplies.rails) { rail in
                            VStack(alignment: .leading, spacing: 2) {
                                Text(rail.name)
                                    .font(.system(size: 10))
                                    .foregroundColor(.secondary)
                                Text(String(format: "%.3f V", rail.voltageV))
                                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                                    .foregroundColor(rail.voltageV > 0.5 ? .green : .secondary)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                } else {
                    Text("Enable the worker thread to start supply monitoring.")
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                        .italic()
                }
            }

            Divider().background(Color.white.opacity(0.1))

            // ── Calibration Engine ──
            VStack(alignment: .leading, spacing: 8) {
                Text("CALIBRATION ENGINE")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.secondary)

                let cal = selftest?.calibration

                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Status").font(.system(size: 10)).foregroundColor(.secondary)
                        Text(cal?.statusText ?? "Unknown")
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                            .foregroundColor(cal?.status == 2 ? .green : (cal?.status == 3 ? .red : .primary))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)

                    VStack(alignment: .leading, spacing: 2) {
                        Text("Channel").font(.system(size: 10)).foregroundColor(.secondary)
                        Text("CH \(cal?.channel ?? 0)")
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)

                    VStack(alignment: .leading, spacing: 2) {
                        Text("Points").font(.system(size: 10)).foregroundColor(.secondary)
                        Text("\(cal?.points ?? 0)")
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)

                    VStack(alignment: .leading, spacing: 2) {
                        Text("Error").font(.system(size: 10)).foregroundColor(.secondary)
                        Text(String(format: "%.2f mV", cal?.errorMv ?? 0.0))
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                            .foregroundColor((cal?.errorMv ?? 0) > 10 ? .red : .primary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }

                if let cal = cal, cal.status == 1 {
                    HStack(spacing: 8) {
                        ProgressView().tint(.cyan)
                        Text(String(format: "Calibrating… Last: %.4f V", cal.lastVoltageV))
                            .font(.system(size: 12))
                            .foregroundColor(.secondary)
                    }
                }

                Divider().background(Color.white.opacity(0.06))

                // Auto-calibrate trigger
                HStack {
                    Text("Channel")
                        .font(.system(size: 13, weight: .medium))
                    Picker("", selection: $calChannel) {
                        ForEach(calSupplies, id: \.id) { supply in
                            Text(supply.name).tag(supply.id)
                        }
                    }
                    .pickerStyle(.menu)
                    .accentColor(.cyan)
                    Spacer()
                }

                Button(action: { startAutoCalibrate() }) {
                    HStack {
                        if isCalibrating {
                            ProgressView().tint(.white)
                        } else {
                            Image(systemName: "wand.and.stars")
                        }
                        Text(isCalibrating ? "Calibrating…" : "Start Auto-Calibration")
                            .fontWeight(.bold)
                    }
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 10)
                    .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                }
                .disabled(isCalibrating)

                if let result = calResult {
                    Text(result)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(result.contains("Error") || result.contains("blocked") ? .red : .green)
                }
            }

            Divider().background(Color.white.opacity(0.1))

            Button(action: { resetAllChannels() }) {
                HStack {
                    Image(systemName: "arrow.counterclockwise.circle.fill")
                    Text("Reset All Channels to High-Z")
                        .fontWeight(.bold)
                }
                .foregroundColor(.white)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
                .glassEffect(.regular.tint(.red.opacity(0.6)), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            }
        }
        .cardStyle()
    }

    // MARK: - Internal Supplies Card

    var internalSuppliesCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("AD74416H Internal Diagnostics")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                if isLoadingInternalSupplies {
                    ProgressView().tint(.blue).scaleEffect(0.8)
                } else {
                    Button(action: { fetchInternalSupplies() }) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 14))
                    }
                    .padding(8)
                    .glassEffect(.regular, in: Circle())
                }
            }

            if let supplies = internalSupplies {
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
                    ForEach(supplies) { entry in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(entry.name)
                                .font(.system(size: 9, weight: .bold))
                                .foregroundColor(.secondary)
                            Text(String(format: "%.3f %@", entry.value, entry.unit))
                                .font(.system(size: 13, weight: .bold, design: .monospaced))
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                    }
                }
            } else {
                Text("Tap refresh to read internal diagnostics.")
                    .font(.system(size: 12))
                    .foregroundColor(.secondary)
                    .italic()
            }
        }
        .cardStyle()
    }

    // MARK: - Hardware Alarm Registers Card

    var registersCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Hardware Alarm Registers")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)

            let status = connectionManager.lastStatus

            VStack(alignment: .leading, spacing: 8) {
                Text("ALERT_STATUS (0x3F)")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.secondary)

                let val = status?.alertStatus ?? 0
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    RegisterBitIndicator(label: "RESET",      active: (val & (1 << 0)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "SUPPLY ERR", active: (val & (1 << 2)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "SPI ERR",    active: (val & (1 << 3)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "TEMP ALRT",  active: (val & (1 << 4)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "ADC ERR",    active: (val & (1 << 5)) != 0, normalIsOff: true)
                }
            }

            Divider().background(Color.white.opacity(0.1))

            VStack(alignment: .leading, spacing: 8) {
                Text("LIVE_STATUS (0x40)")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.secondary)

                let val = status?.liveStatus ?? 0
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    RegisterBitIndicator(label: "SUPPLY PG", active: (val & (1 << 0)) != 0, normalIsOff: false)
                    RegisterBitIndicator(label: "ADC BUSY",  active: (val & (1 << 1)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "DATA RDY",  active: (val & (1 << 2)) != 0, normalIsOff: false)
                    RegisterBitIndicator(label: "TEMP HIGH", active: (val & (1 << 3)) != 0, normalIsOff: true)
                }
            }
        }
        .cardStyle()
    }

    // MARK: - USB PD Card

    var usbPdCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("USB Power Delivery (HUSB238)")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                if isNegotiatingPd {
                    ProgressView().tint(.blue)
                } else {
                    Button(action: { connectionManager.fetchUsbPdQuick() }) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 14))
                    }
                    .padding(8)
                    .glassEffect(.regular, in: Circle())
                }
            }

            let pd = connectionManager.lastUsbPd

            if let pd = pd, pd.present {
                VStack(alignment: .leading, spacing: 12) {
                    // Status Overview
                    HStack(spacing: 12) {
                        StatusPill(title: pd.attached ? "Attached" : "No Source", ok: pd.attached)
                        StatusPill(title: pd.cc, ok: true)
                    }

                    HStack(spacing: 12) {
                        VStack(alignment: .leading) {
                            Text("VOLTAGE").font(.system(size: 10)).foregroundColor(.secondary)
                            Text(String(format: "%.1f V", pd.voltageV))
                                .font(.system(size: 16, weight: .bold, design: .monospaced))
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)

                        VStack(alignment: .leading) {
                            Text("CURRENT").font(.system(size: 10)).foregroundColor(.secondary)
                            Text(String(format: "%.2f A", pd.currentA))
                                .font(.system(size: 16, weight: .bold, design: .monospaced))
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)

                        VStack(alignment: .leading) {
                            Text("POWER").font(.system(size: 10)).foregroundColor(.secondary)
                            Text(String(format: "%.1f W", pd.powerW))
                                .font(.system(size: 16, weight: .bold, design: .monospaced))
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .padding(.vertical, 4)

                    Divider().background(Color.white.opacity(0.1))

                    Text("AVAILABLE PROFILES")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.secondary)

                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack(spacing: 10) {
                            ForEach(pd.sourcePdos) { pdo in
                                let isSelected = pdo.voltage == "\(Int(pd.voltageV))V"
                                Button(action: { selectUsbPdVoltage(pdo.voltage) }) {
                                    VStack(alignment: .center, spacing: 4) {
                                        Text(pdo.voltage)
                                            .font(.system(size: 14, weight: .bold))
                                            .foregroundColor(isSelected ? .black : .blue)
                                        Text(String(format: "%.1fA", pdo.maxCurrentA))
                                            .font(.system(size: 10))
                                            .foregroundColor(isSelected ? .black.opacity(0.7) : .secondary)
                                    }
                                    .frame(width: 60, height: 50)
                                    .glassEffect(
                                        pdo.detected ? (isSelected ? .regular.tint(.blue) : .regular) : .regular,
                                        in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                                    )
                                    .opacity(pdo.detected ? 1.0 : 0.4)
                                }
                                .disabled(!pdo.detected || isNegotiatingPd)
                            }
                        }
                    }
                }
            } else {
                Text("HUSB238 controller not detected or status unavailable.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            }
        }
        .cardStyle()
    }

    // MARK: - Calibration Card

    var calibrationCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Supply Calibration")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                if isLoadingCalData {
                    ProgressView().tint(.blue)
                }
            }

            VStack(alignment: .leading, spacing: 12) {
                Text("SELECT SUPPLY")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.secondary)

                HStack {
                    Text("Supply")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(.secondary)
                    Spacer()
                    Picker("Supply", selection: $selectedCalSupply) {
                        ForEach(calSupplies, id: \.id) { supply in
                            Text(supply.name).tag(supply.id)
                        }
                    }
                    .pickerStyle(.menu)
                    .accentColor(.cyan)
                }

                Button(action: { fetchCalibrationData() }) {
                    HStack {
                        Image(systemName: "chart.xyaxis.line")
                        Text("View Calibration Data")
                            .fontWeight(.bold)
                    }
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 10)
                    .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                }
                .disabled(isLoadingCalData)
            }
        }
        .cardStyle()
    }

    // MARK: - WiFi Card

    var wifiCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Wi-Fi Configuration")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                if isScanningWifi {
                    ProgressView().tint(.blue)
                } else {
                    Button("Scan Networks") { scanWifi() }
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(.black)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                }
            }

            if let ws = connectionManager.lastWifiStatus {
                VStack(alignment: .leading, spacing: 8) {
                    Text("CURRENT CONNECTION")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.secondary)
                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("SSID").font(.system(size: 9)).foregroundColor(.secondary)
                            Text(ws.connected ? ws.staSSID : "Not connected")
                                .font(.system(size: 13, weight: .bold, design: .monospaced))
                                .foregroundColor(ws.connected ? .green : .secondary)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("IP").font(.system(size: 9)).foregroundColor(.secondary)
                            Text(ws.staIP.isEmpty ? "\u{2014}" : ws.staIP)
                                .font(.system(size: 13, weight: .bold, design: .monospaced))
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Signal").font(.system(size: 9)).foregroundColor(.secondary)
                            Text("\(ws.rssi) dBm")
                                .font(.system(size: 13, weight: .bold, design: .monospaced))
                                .foregroundColor(ws.rssi > -60 ? .green : (ws.rssi > -75 ? .yellow : .red))
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                Divider().background(Color.white.opacity(0.1))
            }
            if wifiNetworks.isEmpty {
                Text("No scanned networks yet. Tap Scan Networks.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            } else {
                VStack(spacing: 8) {
                    ForEach(wifiNetworks) { net in
                        Button(action: {
                            selectedSSID = net.ssid
                            wifiPassword = ""
                            showWifiSheet = true
                        }) {
                            HStack {
                                Image(systemName: net.auth != 0 ? "lock.fill" : "lock.open.fill")
                                    .foregroundColor(.secondary)
                                Text(net.ssid)
                                    .font(.system(size: 14, weight: .medium))
                                    .foregroundColor(.white)
                                Spacer()
                                Text("\(net.rssi) dBm")
                                    .font(.system(size: 12, design: .monospaced))
                                    .foregroundColor(.secondary)
                            }
                            .padding(.vertical, 8)
                            .padding(.horizontal, 10)
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                        }
                    }
                }
            }
        }
        .cardStyle()
    }

    // MARK: - OTA Version Picker Card

    var otaCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Firmware OTA Update")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                if isFetchingReleases {
                    ProgressView().tint(.blue).scaleEffect(0.8)
                } else {
                    Button(action: fetchGitReleases) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 14))
                    }
                    .padding(8)
                    .glassEffect(.regular, in: Circle())
                }
            }

            // Current version info
            if let fw = connectionManager.activeDevice?.firmware, !fw.isEmpty {
                HStack {
                    Text("Installed ESP32")
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                    Spacer()
                    Text(fw)
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundColor(.cyan)
                }
            }

            if gitReleases.isEmpty && !isFetchingReleases {
                Text("Could not fetch releases. Check network and retry.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            } else if !gitReleases.isEmpty {

                Divider().background(Color.white.opacity(0.1))

                // ESP32 picker
                let esp32Releases = gitReleases.filter { $0.esp32Available }
                if !esp32Releases.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Toggle(isOn: $updateEsp32) {
                                Text("Update ESP32")
                                    .font(.system(size: 14, weight: .semibold))
                            }
                            .toggleStyle(SwitchToggleStyle(tint: .cyan))
                        }

                        if updateEsp32 {
                            Picker("ESP32 Version", selection: $selectedEsp32Tag) {
                                ForEach(esp32Releases) { r in
                                    Text("\(r.tag) (\(r.esp32Version.isEmpty ? "?" : r.esp32Version))")
                                        .tag(r.tag)
                                }
                            }
                            .pickerStyle(.menu)
                            .accentColor(.cyan)
                        }
                    }
                }

                // HAT picker (only when HAT detected)
                if hatPresent {
                    let hatReleases = gitReleases.filter { $0.hatAvailable }
                    if !hatReleases.isEmpty {
                        VStack(alignment: .leading, spacing: 6) {
                            Toggle(isOn: $updateHat) {
                                Text("Update HAT (RP2040)")
                                    .font(.system(size: 14, weight: .semibold))
                            }
                            .toggleStyle(SwitchToggleStyle(tint: .purple))

                            if updateHat {
                                Picker("HAT Version", selection: $selectedHatTag) {
                                    ForEach(hatReleases) { r in
                                        Text(r.tag).tag(r.tag)
                                    }
                                }
                                .pickerStyle(.menu)
                                .accentColor(.purple)
                            }
                        }
                    }
                }

                Divider().background(Color.white.opacity(0.1))

                // Status / progress
                if !otaApplyStatus.isEmpty {
                    Text(otaApplyStatus)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundColor(otaApplyStatus.contains("Error") ? .red : .green)
                        .padding(.bottom, 4)
                }

                if isApplyingOta {
                    HStack {
                        ProgressView()
                            .tint(.cyan)
                        Text("Update in progress… device will reboot")
                            .font(.system(size: 12))
                            .foregroundColor(.secondary)
                    }
                } else {
                    Button(action: applyOtaUpdate) {
                        HStack {
                            Image(systemName: "arrow.down.circle.fill")
                            Text("Apply Selected Firmware")
                                .fontWeight(.bold)
                        }
                        .foregroundColor(.black)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                        .glassEffect(
                            (updateEsp32 || updateHat) ? .regular.tint(.blue) : .regular,
                            in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                        )
                    }
                    .disabled(!updateEsp32 && !updateHat)
                }

                Divider().background(Color.white.opacity(0.1))

                Button(action: { performRollback() }) {
                    HStack {
                        Image(systemName: "arrow.uturn.backward.circle.fill")
                        Text("Rollback to Previous Firmware")
                            .fontWeight(.bold)
                    }
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 10)
                    .glassEffect(.regular.tint(.orange), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                }
            }
        }
        .cardStyle()
    }

    // MARK: - SPIFFS Storage Card

    var spiffsCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("SPIFFS Storage")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                if isLoadingSpiffs {
                    ProgressView().tint(.blue).scaleEffect(0.8)
                } else {
                    Button(action: loadSpiffsStorage) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 14))
                    }
                    .padding(8)
                    .glassEffect(.regular, in: Circle())
                }
            }

            if let s = spiffsStorage {
                let usedKB = s.usedBytes / 1024
                let totalKB = s.totalBytes / 1024
                let freeKB = s.freeBytes / 1024
                let usedFraction = totalKB > 0 ? usedKB / totalKB : 0.0

                VStack(alignment: .leading, spacing: 8) {
                    // Usage bar
                    GeometryReader { geo in
                        ZStack(alignment: .leading) {
                            RoundedRectangle(cornerRadius: 4)
                                .fill(Color.white.opacity(0.07))
                                .frame(height: 8)
                            RoundedRectangle(cornerRadius: 4)
                                .fill(usedFraction > 0.85 ? Color.red : Color.cyan)
                                .frame(width: geo.size.width * CGFloat(min(usedFraction, 1.0)), height: 8)
                        }
                    }
                    .frame(height: 8)

                    HStack {
                        Text(String(format: "Used %.1f KB", usedKB))
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundColor(.cyan)
                        Spacer()
                        Text(String(format: "Free %.1f KB", freeKB))
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundColor(.secondary)
                        Spacer()
                        Text(String(format: "Total %.1f KB", totalKB))
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundColor(.secondary)
                    }

                    HStack {
                        Text("Scripts stored")
                            .font(.system(size: 13))
                            .foregroundColor(.secondary)
                        Spacer()
                        Text("\(s.scriptCount) / \(s.maxScripts)")
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                    }
                }

                Divider().background(Color.white.opacity(0.1))

                Text("SPIFFS OTA (full image flash) requires the desktop app.")
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
                    .italic()

            } else if !isLoadingSpiffs {
                Text("Storage info unavailable.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            }
        }
        .cardStyle()
    }

    // MARK: - WiFi sheet

    var wifiConnectionSheet: some View {
        NavigationStack {
            ZStack {
                Color(red: 0.03, green: 0.05, blue: 0.10)
                    .ignoresSafeArea()

                ScrollView {
                    GlassEffectContainer(spacing: 8) {
                        VStack(alignment: .leading, spacing: 16) {
                            VStack(alignment: .leading, spacing: 10) {
                                Text("Network parameters")
                                    .font(.system(size: 13, weight: .semibold))
                                    .foregroundColor(.secondary)

                                LabeledContent("Network SSID", value: selectedSSID)
                                SecureField("WPA2 Network Password", text: $wifiPassword)
                                    .padding()
                                    .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                            }

                    Button("Confirm & Connect") { connectToWifi() }
                        .bold()
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.vertical, 10)
                        .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                        .foregroundColor(.black)

                            if let status = wifiConnectStatus {
                                VStack(alignment: .leading, spacing: 8) {
                                    Text("Status")
                                        .font(.system(size: 13, weight: .semibold))
                                        .foregroundColor(.secondary)
                                    Text(status)
                                        .foregroundColor(.blue)
                                }
                                .padding()
                                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                            }
                        }
                        .padding()
                    }
                }
            }
            .navigationTitle("Connect Wi-Fi")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { showWifiSheet = false }
                }
            }
        }
    }

    // MARK: - Actions

    private func toggleWorkerState(_ enabled: Bool) {
        Task {
            _ = try? await connectionManager.postAction(path: "/api/selftest/worker", json: ["enabled": enabled])
            // Refresh cached supplies after toggling — the monitor may have new data
            if enabled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                fetchCachedSupplies()
            }
        }
    }

    private func startAutoCalibrate() {
        isCalibrating = true
        calResult = nil
        let isHatRail = calChannel >= Self.hatRailTagOffset
        let railId = calChannel - Self.hatRailTagOffset
        Task {
            do {
                let ok: Bool
                if isHatRail {
                    ok = await connectionManager.startHatCalibration(rail: railId)
                } else {
                    ok = try await connectionManager.postAction(
                        path: "/api/selftest/calibrate",
                        json: ["channel": calChannel]
                    )
                }
                DispatchQueue.main.async {
                    if ok {
                        if isHatRail {
                            self.calResult = "HAT calibration started on rail \(railId)"
                        } else {
                            self.calResult = "Calibration started on channel \(self.calChannel)"
                        }
                    } else {
                        self.calResult = "Error: calibration blocked (busy or interlock)"
                    }
                    self.isCalibrating = false
                }
            } catch {
                DispatchQueue.main.async {
                    self.calResult = "Error: \(error.localizedDescription)"
                    self.isCalibrating = false
                }
            }
        }
    }

    private func fetchCachedSupplies() {
        Task {
            if let supplies: SelftestSupplyCached = try? await connectionManager.getRequest(path: "/api/selftest/supplies/cached") {
                DispatchQueue.main.async {
                    self.cachedSupplies = supplies
                }
            }
        }
    }

    private func fetchCalibrationData() {
        isLoadingCalData = true
        let isHatRail = selectedCalSupply >= Self.hatRailTagOffset
        let railId = selectedCalSupply - Self.hatRailTagOffset
        Task {
            if isHatRail {
                // HAT rail: the RP2040 stores the cal points. Feed them into the
                // same chart/analysis path used by the mainboard supplies so the
                // linearity graph + metrics render identically.
                let hat = await connectionManager.fetchHatCalibration(rail: railId)
                DispatchQueue.main.async {
                    self.loadedCalPoints = hat?.points ?? []
                    self.loadedHatCal = hat
                    self.isLoadingCalData = false
                    self.showingCalData = true
                }
            } else {
                let pts = await connectionManager.fetchCalibrationPoints(ch: selectedCalSupply)
                let hat = await connectionManager.fetchHatCalibration()
                DispatchQueue.main.async {
                    self.loadedCalPoints = pts
                    self.loadedHatCal = hat
                    self.isLoadingCalData = false
                    self.showingCalData = true
                }
            }
        }
    }

    private func selectUsbPdVoltage(_ voltageStr: String) {
        // Extract number from "5V", "9V", etc.
        let v = Int(voltageStr.replacingOccurrences(of: "V", with: "")) ?? 0
        guard v > 0 else { return }
        
        isNegotiatingPd = true
        Task {
            let success = await connectionManager.setUsbPdVoltage(v)
            DispatchQueue.main.async {
                self.isNegotiatingPd = false
            }
        }
    }

    private func scanWifi() {
        isScanningWifi = true
        Task {
            do {
                let res: WifiScanResponse = try await connectionManager.getRequest(path: "/api/wifi/scan")
                DispatchQueue.main.async {
                    self.wifiNetworks = res.networks.map { WifiNetworkScanItem(ssid: $0.ssid, rssi: $0.rssi, auth: $0.auth) }
                    self.isScanningWifi = false
                }
            } catch {
                DispatchQueue.main.async { self.isScanningWifi = false }
            }
        }
    }

    private func connectToWifi() {
        wifiConnectStatus = "Sending parameters to BugBuster..."
        Task {
            do {
                let success = try await connectionManager.postAction(
                    path: "/api/wifi/connect",
                    json: ["ssid": selectedSSID, "password": wifiPassword]
                )
                DispatchQueue.main.async {
                    if success {
                        wifiConnectStatus = "Connection command sent! Check ESP IP."
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { showWifiSheet = false }
                    } else {
                        wifiConnectStatus = "Error writing credentials."
                    }
                }
            } catch {
                DispatchQueue.main.async { wifiConnectStatus = error.localizedDescription }
            }
        }
    }

    private func fetchGitReleases() {
        isFetchingReleases = true
        Task {
            do {
                guard let url = URL(string: "https://api.github.com/repos/lollokara/BugBuster/releases?per_page=10") else { return }
                var req = URLRequest(url: url)
                req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
                req.setValue("BugBuster-iOS", forHTTPHeaderField: "User-Agent")

                let (data, _) = try await URLSession.shared.data(for: req)
                guard let rawArray = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
                    DispatchQueue.main.async { self.isFetchingReleases = false }
                    return
                }

                var releases: [GitRelease] = []
                for item in rawArray {
                    guard let tag = item["tag_name"] as? String else { continue }
                    let assets = item["assets"] as? [[String: Any]] ?? []
                    let publishedAt = item["published_at"] as? String ?? ""

                    var esp32Available = false
                    var hatAvailable = false
                    var spiffsAvailable = false
                    var esp32Version = ""

                    for asset in assets {
                        guard let name = asset["name"] as? String else { continue }
                        let lower = name.lowercased()
                        if lower.contains("esp32") && lower.hasSuffix(".bin") && !lower.contains("spiffs") {
                            esp32Available = true
                            // Extract version from name like bugbuster-esp32-3.4.0.bin
                            let parts = name.components(separatedBy: "-")
                            if parts.count >= 3 {
                                esp32Version = parts.last?.replacingOccurrences(of: ".bin", with: "") ?? ""
                            }
                        }
                        if lower.contains("rp2040") || lower.contains("hat") {
                            hatAvailable = true
                        }
                        if lower.contains("spiffs") && lower.hasSuffix(".bin") {
                            spiffsAvailable = true
                        }
                    }

                    if esp32Available || hatAvailable || spiffsAvailable {
                        releases.append(GitRelease(
                            id: tag,
                            tag: tag,
                            esp32Version: esp32Version,
                            esp32Available: esp32Available,
                            hatAvailable: hatAvailable,
                            spiffsAvailable: spiffsAvailable,
                            publishedAt: publishedAt
                        ))
                    }
                }

                DispatchQueue.main.async {
                    self.gitReleases = releases
                    self.isFetchingReleases = false
                    // Auto-select the newest available
                    if self.selectedEsp32Tag.isEmpty, let first = releases.first(where: { $0.esp32Available }) {
                        self.selectedEsp32Tag = first.tag
                    }
                    if self.selectedHatTag.isEmpty, let first = releases.first(where: { $0.hatAvailable }) {
                        self.selectedHatTag = first.tag
                    }
                }
            } catch {
                DispatchQueue.main.async { self.isFetchingReleases = false }
            }
        }
    }

    private func applyOtaUpdate() {
        guard updateEsp32 || updateHat else { return }
        isApplyingOta = true
        otaApplyStatus = "Triggering OTA on device…"

        Task {
            do {
                // First run check so firmware latches the latest URLs
                _ = try? await connectionManager.getRequest(path: "/api/update/check") as OtaUpdateStatus
                let ok = try await connectionManager.postAction(
                    path: "/api/update/apply",
                    json: ["esp32": updateEsp32, "rp2040": updateHat]
                )
                DispatchQueue.main.async {
                    if ok {
                        self.otaApplyStatus = "Update started — device will reboot when done."
                        // Poll status for 60 seconds
                        self.startOtaPolling()
                    } else {
                        self.otaApplyStatus = "Error: device rejected the request."
                        self.isApplyingOta = false
                    }
                }
            } catch {
                DispatchQueue.main.async {
                    self.otaApplyStatus = "Error: \(error.localizedDescription)"
                    self.isApplyingOta = false
                }
            }
        }
    }

    private func startOtaPolling() {
        var attempts = 0
        // Use a repeating async task instead of Timer to avoid Sendable issues
        Task {
            while attempts < 20 {
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                attempts += 1
                if let status: OtaUpdateStatus = try? await connectionManager.getRequest(path: "/api/update/status") {
                    let pct = status.progress.map { " (\($0)%)" } ?? ""
                    DispatchQueue.main.async {
                        self.otaApplyStatus = "Status: \(status.status)\(pct)"
                    }
                    if status.status == "idle" || status.status == "done" {
                        break
                    }
                }
            }
            DispatchQueue.main.async { self.isApplyingOta = false }
        }
    }

    private func loadSpiffsStorage() {
        isLoadingSpiffs = true
        Task {
            // Retry a few times: the first load can race a device that is
            // still busy (e.g. right after connect or a heavy stream).
            for attempt in 0..<3 {
                if let s: StorageInfoDiag = try? await connectionManager.getRequest(path: "/api/scripts/storage") {
                    DispatchQueue.main.async {
                        self.spiffsStorage = s
                        self.isLoadingSpiffs = false
                    }
                    return
                }
                if attempt < 2 {
                    try? await Task.sleep(nanoseconds: 3_000_000_000)
                }
            }
            DispatchQueue.main.async { self.isLoadingSpiffs = false }
        }
    }

    private func resetAllChannels() {
        Task {
            let ok = await connectionManager.resetDevice()
            connectionManager.showToast(ok ? "All channels reset to High-Z" : "Reset failed", type: ok ? .success : .error)
        }
    }

    private func performRollback() {
        Task {
            let ok = await connectionManager.otaRollback()
            connectionManager.showToast(ok ? "Rollback initiated \u{2014} device will reboot" : "Rollback failed", type: ok ? .success : .error)
        }
    }

    private func fetchInternalSupplies() {
        isLoadingInternalSupplies = true
        Task {
            if let resp: InternalSuppliesResponse = try? await connectionManager.getRequest(path: "/api/selftest/supplies") {
                DispatchQueue.main.async {
                    self.internalSupplies = resp.supplies
                    self.isLoadingInternalSupplies = false
                }
            } else {
                DispatchQueue.main.async { self.isLoadingInternalSupplies = false }
            }
        }
    }
}

// MARK: - Calibration Data View

struct CalibrationAnalysis {
    let slope: Double
    let intercept: Double
    let rSquared: Double
    let maxDevMv: Double
    let minDevMv: Double
    let rmseMv: Double
    
    init(points: [CalibrationPoint]) {
        guard points.count >= 2 else {
            slope = 0; intercept = 0; rSquared = 0; maxDevMv = 0; minDevMv = 0; rmseMv = 0
            return
        }
        
        let n = Double(points.count)
        let sumX = points.reduce(0.0) { $0 + Double($1.dacCode) }
        let sumY = points.reduce(0.0) { $0 + $1.measuredV }
        let sumXY = points.reduce(0.0) { $0 + Double($1.dacCode) * $1.measuredV }
        let sumX2 = points.reduce(0.0) { $0 + Double($1.dacCode) * Double($1.dacCode) }
        let sumY2 = points.reduce(0.0) { $0 + $1.measuredV * $1.measuredV }
        
        let denominator = (n * sumX2 - sumX * sumX)
        if abs(denominator) < 1e-9 {
            slope = 0; intercept = 0; rSquared = 0; maxDevMv = 0; minDevMv = 0; rmseMv = 0
            return
        }
        
        slope = (n * sumXY - sumX * sumY) / denominator
        intercept = (sumY - slope * sumX) / n
        
        // R^2
        let numRSq = (n * sumXY - sumX * sumY) * (n * sumXY - sumX * sumY)
        let denRSq = (n * sumX2 - sumX * sumX) * (n * sumY2 - sumY * sumY)
        rSquared = denRSq > 0 ? numRSq / denRSq : 1.0
        
        // Deviations (residuals)
        var maxD = -Double.infinity
        var minD = Double.infinity
        var sumSqErr = 0.0
        
        for pt in points {
            let predicted = slope * Double(pt.dacCode) + intercept
            let dev = (pt.measuredV - predicted) * 1000.0 // Convert to mV
            maxD = max(maxD, dev)
            minD = min(minD, dev)
            sumSqErr += (dev * dev)
        }
        
        maxDevMv = maxD
        minDevMv = minD
        rmseMv = sqrt(sumSqErr / n)
    }
}

struct CalibrationDataView: View {
    @Environment(\.dismiss) var dismiss
    let supplyName: String
    let points: [CalibrationPoint]
    var hatCal: HatCalibration? = nil

    var analysis: CalibrationAnalysis { CalibrationAnalysis(points: points) }

    private func hatRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(.system(size: 13)).foregroundColor(.secondary)
            Spacer()
            Text(value).font(.system(size: 13, design: .monospaced)).foregroundColor(.cyan)
        }
    }

    @ViewBuilder private var hatCalSection: some View {
        if let h = hatCal, h.hatPresent {
            VStack(alignment: .leading, spacing: 8) {
                Text("RP2040 HAT Calibration")
                    .font(.headline)
                    .foregroundColor(.white)
                hatRow("State", h.state.map(String.init) ?? "—")
                hatRow("Rail", h.railId.map(String.init) ?? "—")
                hatRow("Progress", h.progress.map { "\($0)%" } ?? "—")
                hatRow("Last code", h.code.map(String.init) ?? "—")
                hatRow("Measured", h.measuredMv.map { String(format: "%.3f V", Double($0) / 1000.0) } ?? "—")
                hatRow("Max error", h.maxErrorMv.map { "\($0) mV" } ?? "—")
                hatRow("Max gap", h.maxGapMv.map { "\($0) mV" } ?? "—")
                hatRow("Validation", h.validationFlags.map { String(format: "0x%04X", $0) } ?? "—")
                if let count = h.pointsCount {
                    hatRow("Stored points", "\(count)\(h.pointsValid == true ? " · valid" : "")")
                }
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 14).fill(Color.white.opacity(0.05)))
            .padding()
        }
    }

    var body: some View {
        NavigationStack {
            ZStack {
                Color(red: 0.03, green: 0.05, blue: 0.10)
                    .ignoresSafeArea()

                VStack(spacing: 0) {
                    hatCalSection
                    if points.isEmpty {
                        if hatCal?.hatPresent == true {
                            // HAT rail view: hatCalSection already shows the stored
                            // points / live status, so no mainboard placeholder.
                            Spacer()
                        } else {
                        VStack(spacing: 12) {
                            Image(systemName: "chart.bar.xaxis")
                                .font(.system(size: 50))
                                .foregroundColor(.secondary)
                            Text("No calibration data found")
                                .font(.headline)
                                .foregroundColor(.secondary)
                            Text("Run an auto-calibration sweep first.")
                                .font(.subheadline)
                                .foregroundColor(.secondary.opacity(0.8))
                                .multilineTextAlignment(.center)
                                .padding(.horizontal, 40)
                        }
                        .frame(maxHeight: .infinity)
                        }
                    } else {
                        ScrollView {
                            VStack(alignment: .leading, spacing: 20) {
                                Text("Linearity Curve")
                                    .font(.title3)
                                    .bold()
                                    .foregroundColor(.white)

                                Chart {
                                    ForEach(points) { pt in
                                        LineMark(
                                            x: .value("DAC Code", pt.dacCode),
                                            y: .value("Voltage", pt.measuredV)
                                        )
                                        .foregroundStyle(Color.cyan)
                                        .interpolationMethod(.linear)

                                        PointMark(
                                            x: .value("DAC Code", pt.dacCode),
                                            y: .value("Voltage", pt.measuredV)
                                        )
                                        .foregroundStyle(Color.blue)
                                    }
                                    
                                    // Trend Line (Fitting Rect)
                                    if points.count >= 2 {
                                        let firstX = Double(points.first?.dacCode ?? -127)
                                        let lastX  = Double(points.last?.dacCode ?? 127)
                                        
                                        LineMark(
                                            x: .value("DAC Code", firstX),
                                            y: .value("Voltage", analysis.slope * firstX + analysis.intercept)
                                        )
                                        .foregroundStyle(Color.red.opacity(0.5))
                                        .lineStyle(StrokeStyle(lineWidth: 2, dash: [5, 5]))
                                        
                                        LineMark(
                                            x: .value("DAC Code", lastX),
                                            y: .value("Voltage", analysis.slope * lastX + analysis.intercept)
                                        )
                                        .foregroundStyle(Color.red.opacity(0.5))
                                        .lineStyle(StrokeStyle(lineWidth: 2, dash: [5, 5]))
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
                                .frame(height: 240)
                                .padding()
                                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))

                                // Math Analysis Section
                                VStack(alignment: .leading, spacing: 12) {
                                    Text("FITTING ANALYSIS")
                                        .font(.system(size: 10, weight: .bold))
                                        .foregroundColor(.secondary)
                                    
                                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 12) {
                                        AnalysisPill(title: "Slope (G)", value: String(format: "%.4f V/LSB", analysis.slope))
                                        AnalysisPill(title: "Offset", value: String(format: "%.3f V", analysis.intercept))
                                        AnalysisPill(title: "Max Dev", value: String(format: "%.2f mV", analysis.maxDevMv))
                                        AnalysisPill(title: "Min Dev", value: String(format: "%.2f mV", analysis.minDevMv))
                                        AnalysisPill(title: "RMSE", value: String(format: "%.3f mV", analysis.rmseMv))
                                        AnalysisPill(title: "Linearity (R²)", value: String(format: "%.5f", analysis.rSquared))
                                    }
                                }
                                .padding()
                                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))

                                VStack(alignment: .leading, spacing: 8) {
                                    Text("Data Points").font(.headline)
                                    LazyVStack(spacing: 8) {
                                        ForEach(points) { pt in
                                            let predicted = analysis.slope * Double(pt.dacCode) + analysis.intercept
                                            let dev = (pt.measuredV - predicted) * 1000.0
                                            
                                            HStack {
                                                VStack(alignment: .leading, spacing: 2) {
                                                    Text("Code \(pt.dacCode)")
                                                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                                                    Text(String(format: "%.1f mV dev", dev))
                                                        .font(.system(size: 10))
                                                        .foregroundColor(abs(dev) > 10 ? .red : .secondary)
                                                }
                                                Spacer()
                                                Text(String(format: "%.4f V", pt.measuredV))
                                                    .font(.system(size: 14, weight: .medium, design: .monospaced))
                                                    .foregroundColor(.cyan)
                                            }
                                            .padding(.horizontal)
                                            .padding(.vertical, 8)
                                            .background(Color.white.opacity(0.05))
                                            .cornerRadius(8)
                                        }
                                    }
                                }
                            }
                            .padding()
                        }
                    }
                }
            }
            .navigationTitle(supplyName)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                        .fontWeight(.bold)
                        .foregroundColor(.cyan)
                }
            }
        }
        .preferredColorScheme(.dark)
    }
}

struct AnalysisPill: View {
    let title: String
    let value: String
    
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.system(size: 9, weight: .bold))
                .foregroundColor(.secondary)
                .textCase(.uppercase)
            Text(value)
                .font(.system(size: 13, weight: .semibold, design: .monospaced))
                .foregroundColor(.white)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(Color.white.opacity(0.05))
        .cornerRadius(10)
    }
}

// MARK: - StorageInfoDiag (local copy to avoid cross-file dependency on ScriptsTab's StorageInfo)

struct StorageInfoDiag: Codable {
    let totalBytes: Double
    let usedBytes: Double
    let freeBytes: Double
    let scriptCount: Int
    let maxScriptBytes: Int
    let maxScripts: Int
}

// MARK: - Card style modifier

private extension View {
    func cardStyle() -> some View {
        self
            .padding()
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }
}

// MARK: - Supporting views

struct RegisterBitIndicator: View {
    let label: String
    let active: Bool
    let normalIsOff: Bool

    var body: some View {
        HStack(spacing: 6) {
            let matchesNormal = (active == !normalIsOff)
            Circle()
                .fill(matchesNormal ? Color.green : Color.red)
                .frame(width: 8, height: 8)
                .shadow(color: (matchesNormal ? Color.green : Color.red).opacity(0.3), radius: 4)
            Text(label)
                .font(.system(size: 11, weight: .semibold, design: .rounded))
                .foregroundColor(.primary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, minHeight: 36)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }
}

struct StatusPill: View {
    let title: String
    let ok: Bool

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(ok ? Color.green : Color.secondary)
                .frame(width: 6, height: 6)
            Text(title)
                .font(.system(size: 11, weight: .bold))
                .foregroundColor(ok ? .black : .secondary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .glassEffect(ok ? .regular.tint(.green) : .regular, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }
}
