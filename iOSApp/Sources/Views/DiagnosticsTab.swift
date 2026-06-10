import SwiftUI

struct WifiNetworkScanItem: Identifiable, Codable {
    var id: String { ssid }
    let ssid: String
    let rssi: Int
    let auth: Int
}

struct UpdateStatus: Codable {
    let status: String
    let progress: Int?
    let version: String?
}

struct DiagnosticsTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var isScanningWifi = false
    @State private var wifiNetworks: [WifiNetworkScanItem] = []
    
    @State private var selectedSSID = ""
    @State private var wifiPassword = ""
    @State private var showWifiSheet = false
    @State private var wifiConnectStatus: String? = nil
    
    // OTA Update States
    @State private var isCheckingUpdate = false
    @State private var otaAvailable = false
    @State private var otaVersion = ""
    @State private var otaStatus = ""
    
    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                headerSection
                
                selfTestCard
                
                registersCard
                
                wifiCard
                
                otaCard
                
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
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showWifiSheet) {
            wifiConnectionSheet
        }
    }
    
    var headerSection: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Diagnostics & Config")
                    .font(.system(size: 28, weight: .bold, design: .rounded))
                Text("Monitor hardware alerts and system states")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            }
            Spacer()
        }
        .padding(.top, 10)
    }
    
    var selfTestCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Self-Test & Calibrations")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)
            
            let selftest = connectionManager.lastSelftest
            
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
            
            Divider()
                .background(Color.white.opacity(0.1))
            
            // Boot Results
            VStack(alignment: .leading, spacing: 8) {
                Text("BOOT STATE")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.secondary)
                
                HStack(spacing: 12) {
                    StatusPill(title: "Ran", ok: selftest?.boot.ran ?? false)
                    StatusPill(title: "Passed", ok: selftest?.boot.passed ?? false)
                }
                
                HStack(spacing: 12) {
                    VStack(alignment: .leading) {
                        Text("VADJ1")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Text(String(format: "%.3f V", selftest?.boot.vadj1V ?? 0))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    
                    VStack(alignment: .leading) {
                        Text("VADJ2")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Text(String(format: "%.3f V", selftest?.boot.vadj2V ?? 0))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    
                    VStack(alignment: .leading) {
                        Text("VLOGIC")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Text(String(format: "%.3f V", selftest?.boot.vlogicV ?? 0))
                            .font(.system(size: 14, weight: .bold, design: .monospaced))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .padding(.top, 4)
            }
            
            Divider()
                .background(Color.white.opacity(0.1))
            
            // Calibration Status
            VStack(alignment: .leading, spacing: 6) {
                Text("CALIBRATION ENGINE")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.secondary)
                
                HStack {
                    Text("Calibration Status Code")
                        .font(.system(size: 13))
                    Spacer()
                    Text("\(selftest?.calibration.status ?? 0)")
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                }
                HStack {
                    Text("Collected Points")
                        .font(.system(size: 13))
                    Spacer()
                    Text("\(selftest?.calibration.points ?? 0)")
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                }
                HStack {
                    Text("Error Value")
                        .font(.system(size: 13))
                    Spacer()
                    Text(String(format: "%.2f mV", selftest?.calibration.errorMv ?? 0))
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
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
    
    var registersCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Hardware Alarm Registers")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)
            
            let status = connectionManager.lastStatus
            
            // ALERT_STATUS (0x3F)
            VStack(alignment: .leading, spacing: 8) {
                Text("ALERT_STATUS (0x3F)")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.secondary)
                
                let val = status?.alertStatus ?? 0
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    RegisterBitIndicator(label: "RESET", active: (val & (1 << 0)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "SUPPLY ERR", active: (val & (1 << 2)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "SPI ERR", active: (val & (1 << 3)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "TEMP ALRT", active: (val & (1 << 4)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "ADC ERR", active: (val & (1 << 5)) != 0, normalIsOff: true)
                }
            }
            
            Divider()
                .background(Color.white.opacity(0.1))
            
            // LIVE_STATUS (0x40)
            VStack(alignment: .leading, spacing: 8) {
                Text("LIVE_STATUS (0x40)")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.secondary)
                
                let val = status?.liveStatus ?? 0
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    RegisterBitIndicator(label: "SUPPLY PG", active: (val & (1 << 0)) != 0, normalIsOff: false)
                    RegisterBitIndicator(label: "ADC BUSY", active: (val & (1 << 1)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "DATA RDY", active: (val & (1 << 2)) != 0, normalIsOff: false)
                    RegisterBitIndicator(label: "TEMP HIGH", active: (val & (1 << 3)) != 0, normalIsOff: true)
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
    
    var wifiCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("Wi-Fi Configuration")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                if isScanningWifi {
                    ProgressView()
                        .tint(.blue)
                } else {
                    Button(action: {
                        scanWifi()
                    }) {
                        Text("Scan Networks")
                            .font(.system(size: 13, weight: .medium))
                    }
                }
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
                            .background(Color.white.opacity(0.03))
                            .cornerRadius(8)
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
    
    var otaCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("OTA Update Management")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)
            
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Firmware OTA Update")
                        .font(.system(size: 14, weight: .medium))
                    Text(otaStatus.isEmpty ? "Check for firmware revisions over Wi-Fi." : otaStatus)
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                }
                Spacer()
                
                if isCheckingUpdate {
                    ProgressView()
                } else {
                    Button(action: {
                        checkOtaUpdate()
                    }) {
                        Text("Check")
                            .font(.system(size: 14, weight: .semibold))
                            .padding(.horizontal, 16)
                            .padding(.vertical, 8)
                            .background(Color.blue.opacity(0.1))
                            .cornerRadius(10)
                    }
                }
            }
            
            if otaAvailable {
                HStack {
                    Text("Revision \(otaVersion) ready to apply.")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundColor(.green)
                    Spacer()
                    Button("Upgrade") {
                        applyOtaUpdate()
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.green)
                }
                .padding(.top, 4)
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
    
    var wifiConnectionSheet: some View {
        NavigationStack {
            Form {
                Section("Network parameters") {
                    LabeledContent("Network SSID", value: selectedSSID)
                    SecureField("WPA2 Network Password", text: $wifiPassword)
                }
                
                Section {
                    Button("Confirm & Connect") {
                        connectToWifi()
                    }
                    .bold()
                    .frame(maxWidth: .infinity, alignment: .center)
                }
                
                if let status = wifiConnectStatus {
                    Section("Status") {
                        Text(status)
                            .foregroundColor(.blue)
                    }
                }
            }
            .navigationTitle("Connect Wi-Fi")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        showWifiSheet = false
                    }
                }
            }
        }
    }
    
    private func toggleWorkerState(_ enabled: Bool) {
        Task {
            _ = try? await connectionManager.postAction(path: "/api/selftest/worker", json: ["enabled": enabled])
        }
    }
    
    private func scanWifi() {
        isScanningWifi = true
        Task {
            do {
                let res: WifiScanResponse = try await connectionManager.getRequest(path: "/api/wifi/scan")
                DispatchQueue.main.async {
                    self.wifiNetworks = res.networks.map { net in
                        WifiNetworkScanItem(ssid: net.ssid, rssi: net.rssi, auth: net.auth)
                    }
                    self.isScanningWifi = false
                }
            } catch {
                print("Failed scanning WiFi: \(error)")
                DispatchQueue.main.async {
                    self.isScanningWifi = false
                }
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
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                            showWifiSheet = false
                        }
                    } else {
                        wifiConnectStatus = "Error writing credentials."
                    }
                }
            } catch {
                DispatchQueue.main.async {
                    wifiConnectStatus = error.localizedDescription
                }
            }
        }
    }
    
    private func checkOtaUpdate() {
        isCheckingUpdate = true
        Task {
            do {
                let updateInfo: UpdateStatus = try await connectionManager.getRequest(path: "/api/update/check")
                DispatchQueue.main.async {
                    self.otaAvailable = updateInfo.status == "available"
                    self.otaVersion = updateInfo.version ?? ""
                    self.otaStatus = self.otaAvailable ? "Updates found: \(self.otaVersion)" : "Firmware is up-to-date."
                    self.isCheckingUpdate = false
                }
            } catch {
                DispatchQueue.main.async {
                    self.otaStatus = "Failed checking updates."
                    self.isCheckingUpdate = false
                }
            }
        }
    }
    
    private func applyOtaUpdate() {
        Task {
            _ = try? await connectionManager.postAction(path: "/api/update/apply", json: [:])
            DispatchQueue.main.async {
                self.otaStatus = "OTA Upgrade in progress... Device will reboot."
                self.otaAvailable = false
            }
        }
    }
}

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
            Spacer()
        }
        .padding(8)
        .background(Color.white.opacity(0.04))
        .cornerRadius(8)
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
                .foregroundColor(ok ? .green : .secondary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .background(ok ? Color.green.opacity(0.1) : Color.white.opacity(0.05))
        .cornerRadius(10)
    }
}
