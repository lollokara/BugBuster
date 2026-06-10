import SwiftUI

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

    // SPIFFS
    @State private var spiffsStorage: StorageInfoDiag? = nil
    @State private var isLoadingSpiffs = false

    var hatPresent: Bool { connectionManager.lastHatStatus?.isPresent ?? false }

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                headerSection
                selfTestCard
                registersCard
                wifiCard
                otaCard
                spiffsCard
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
        .sheet(isPresented: $showWifiSheet) { wifiConnectionSheet }
        .onAppear {
            fetchGitReleases()
            loadSpiffsStorage()
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
            }
            Spacer()
        }
        .padding(.top, 10)
    }

    // MARK: - Self-Test Card

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

            Divider().background(Color.white.opacity(0.1))

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

            VStack(alignment: .leading, spacing: 6) {
                Text("CALIBRATION ENGINE")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.secondary)

                HStack {
                    Text("Calibration Status Code").font(.system(size: 13))
                    Spacer()
                    Text("\(connectionManager.lastSelftest?.calibration.status ?? 0)")
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                }
                HStack {
                    Text("Collected Points").font(.system(size: 13))
                    Spacer()
                    Text("\(connectionManager.lastSelftest?.calibration.points ?? 0)")
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                }
                HStack {
                    Text("Error Value").font(.system(size: 13))
                    Spacer()
                    Text(String(format: "%.2f mV", connectionManager.lastSelftest?.calibration.errorMv ?? 0.0))
                        .font(.system(size: 13, weight: .bold, design: .monospaced))
                }
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
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
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
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    RegisterBitIndicator(label: "SUPPLY PG", active: (val & (1 << 0)) != 0, normalIsOff: false)
                    RegisterBitIndicator(label: "ADC BUSY",  active: (val & (1 << 1)) != 0, normalIsOff: true)
                    RegisterBitIndicator(label: "DATA RDY",  active: (val & (1 << 2)) != 0, normalIsOff: false)
                    RegisterBitIndicator(label: "TEMP HIGH", active: (val & (1 << 3)) != 0, normalIsOff: true)
                }
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
                        .foregroundColor(.white)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                        .background(
                            (updateEsp32 || updateHat) ? Color.blue : Color.gray.opacity(0.3)
                        )
                        .cornerRadius(12)
                    }
                    .disabled(!updateEsp32 && !updateHat)
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
            Form {
                Section("Network parameters") {
                    LabeledContent("Network SSID", value: selectedSSID)
                    SecureField("WPA2 Network Password", text: $wifiPassword)
                }
                Section {
                    Button("Confirm & Connect") { connectToWifi() }
                        .bold()
                        .frame(maxWidth: .infinity, alignment: .center)
                }
                if let status = wifiConnectStatus {
                    Section("Status") {
                        Text(status).foregroundColor(.blue)
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
            if let s: StorageInfoDiag = try? await connectionManager.getRequest(path: "/api/scripts/storage") {
                DispatchQueue.main.async {
                    self.spiffsStorage = s
                    self.isLoadingSpiffs = false
                }
            } else {
                DispatchQueue.main.async { self.isLoadingSpiffs = false }
            }
        }
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
            .background(.ultraThinMaterial)
            .cornerRadius(20)
            .overlay(
                RoundedRectangle(cornerRadius: 20)
                    .stroke(Color.white.opacity(0.05), lineWidth: 1)
            )
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
