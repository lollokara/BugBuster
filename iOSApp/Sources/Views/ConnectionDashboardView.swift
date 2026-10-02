import SwiftUI
import AVFoundation

/// Disconnected/scanning/auth UI shown before a device is connected.
/// Shared by both the iPhone shell (`MainTabView`) and the iPad shell (`iPadRootView`)
/// so this UI is never duplicated between idioms.
struct ConnectionDashboardView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var showingScanSheet = false
    @State private var manualIp = ""
    @State private var manualToken = ""
    @State private var errorMessage: String? = nil

    var body: some View {
        NavigationStack {
            ZStack {
                Color(red: 0.075, green: 0.08, blue: 0.09)
                    .ignoresSafeArea()

                ScrollView {
                    VStack(spacing: 24) {
                        HStack(alignment: .center, spacing: 14) {
                            Image(systemName: "bolt.horizontal.fill")
                                .font(.system(size: 24, weight: .medium))
                                .foregroundColor(Color(red: 0.36, green: 0.84, blue: 0.74))
                                .frame(width: 52, height: 52)
                                .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))

                            VStack(alignment: .leading, spacing: 3) {
                                Text("BugBuster")
                                    .font(.system(size: 27, weight: .semibold, design: .rounded))
                                    .foregroundColor(.white)
                                Text("CONNECT TO INSTRUMENT")
                                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                                    .foregroundColor(.white.opacity(0.52))
                            }
                            Spacer(minLength: 0)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 24)
                        .padding(.top, 28)

                        if let error = errorMessage {
                            Text(error)
                                .font(.system(size: 14, weight: .medium))
                                .foregroundColor(.red)
                                .frame(maxWidth: .infinity)
                                .padding(14)
                                .background(
                                    RoundedRectangle(cornerRadius: 12)
                                        .fill(Color.red.opacity(0.15))
                                        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.red.opacity(0.3), lineWidth: 1))
                                )
                                .padding(.horizontal)
                        }

                        if connectionManager.connectionState == .connecting {
                            HStack(spacing: 12) {
                                ProgressView()
                                    .tint(Color(red: 0.36, green: 0.84, blue: 0.74))
                                Text("Connecting to instrument")
                                    .font(.system(size: 14, weight: .medium))
                                    .foregroundColor(.white)
                                Spacer()
                            }
                            .padding(16)
                            .background(Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
                            .padding(.horizontal)
                        }

                        if connectionManager.connectionState == .unauthorized {
                            VStack(alignment: .leading, spacing: 18) {
                                HStack(spacing: 12) {
                                    Image(systemName: "lock.shield")
                                        .font(.system(size: 19))
                                        .foregroundColor(Color(red: 1, green: 0.72, blue: 0.42))
                                        .frame(width: 38, height: 38)
                                        .background(Color.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
                                    VStack(alignment: .leading, spacing: 3) {
                                        Text("Admin access")
                                            .font(.system(size: 17, weight: .semibold))
                                            .foregroundColor(.white)
                                        Text(connectionManager.activeDevice?.hostname ?? "BugBuster")
                                            .font(.system(size: 12, design: .monospaced))
                                            .foregroundColor(.white.opacity(0.56))
                                            .lineLimit(1)
                                    }
                                }

                                HStack(spacing: 10) {
                                    SecureField("Admin Access Token", text: $manualToken)
                                        .autocorrectionDisabled()
                                        .textInputAutocapitalization(.never)
                                        .premiumGlassInput()

                                    Button(action: {
                                        showingScanSheet = true
                                    }) {
                                        Image(systemName: "qrcode.viewfinder")
                                            .font(.system(size: 20, weight: .semibold))
                                            .foregroundColor(Color(red: 0.36, green: 0.84, blue: 0.74))
                                            .frame(width: 48, height: 48)
                                            .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
                                    }
                                    .buttonStyle(.plain)
                                    .accessibilityLabel("Scan access token QR code")
                                }

                                HStack(spacing: 14) {
                                    Button(action: {
                                        connectionManager.disconnect()
                                        errorMessage = nil
                                    }) {
                                        Text("Cancel")
                                            .font(.system(size: 15, weight: .semibold))
                                            .foregroundColor(.white)
                                            .frame(maxWidth: .infinity)
                                            .padding()
                                            .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
                                    }
                                    .buttonStyle(.plain)

                                    Button(action: {
                                        Task {
                                            let success: Bool
                                            if connectionManager.transport == .ble, let dev = connectionManager.activeDevice {
                                                success = await connectionManager.connectBLE(dev, token: manualToken)
                                            } else {
                                                let ip = connectionManager.activeDevice?.ip ?? manualIp
                                                success = await connectionManager.connect(ip: ip, token: manualToken)
                                            }
                                            if !success {
                                                errorMessage = "Authentication failed. Invalid token."
                                            } else {
                                                errorMessage = nil
                                            }
                                        }
                                    }) {
                                        Text("Authenticate")
                                            .font(.system(size: 15, weight: .bold))
                                            .foregroundColor(Color(red: 0.075, green: 0.08, blue: 0.09))
                                            .frame(maxWidth: .infinity)
                                            .padding()
                                            .background(Color(red: 0.36, green: 0.84, blue: 0.74), in: RoundedRectangle(cornerRadius: 8))
                                    }
                                    .buttonStyle(.plain)
                                    .disabled(manualToken.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                                }
                            }
                            .padding(20)
                            .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 8))
                            .padding(.horizontal)
                        } else {
                            VStack(spacing: 16) {
                                VStack(alignment: .leading, spacing: 14) {
                                    HStack {
                                        Label("Wi-Fi", systemImage: "wifi")
                                            .font(.system(size: 16, weight: .semibold))
                                            .foregroundColor(.white)
                                        Spacer()
                                        if connectionManager.isSearching {
                                            ProgressView()
                                                .tint(Color(red: 0.36, green: 0.84, blue: 0.74))
                                        } else {
                                            Button(action: {
                                                connectionManager.startDiscovery()
                                            }) {
                                                Image(systemName: "arrow.clockwise")
                                                    .font(.system(size: 15, weight: .semibold))
                                                    .foregroundColor(Color(red: 0.36, green: 0.84, blue: 0.74))
                                                    .frame(width: 32, height: 32)
                                            }
                                            .buttonStyle(.plain)
                                            .accessibilityLabel("Scan Wi-Fi devices")
                                        }
                                    }

                                    if connectionManager.discoveredDevices.isEmpty {
                                        HStack(spacing: 12) {
                                            Image(systemName: "wifi.router.fill")
                                                .font(.system(size: 26))
                                                .foregroundColor(.secondary)
                                            VStack(alignment: .leading, spacing: 4) {
                                                Text(connectionManager.isSearching ? "Searching nearby..." : "No Wi-Fi devices found")
                                                    .font(.system(size: 14, weight: .semibold))
                                                    .foregroundColor(.white)
                                                Text("Check that your instrument is on this network.")
                                                    .font(.system(size: 12))
                                                    .foregroundColor(.white.opacity(0.56))
                                            }
                                        }
                                        .padding()
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                        .background(Color.white.opacity(0.04), in: RoundedRectangle(cornerRadius: 8))
                                    } else {
                                        VStack(spacing: 10) {
                                            ForEach(connectionManager.discoveredDevices) { device in
                                                Button(action: {
                                                    Task {
                                                        var tokenToUse = manualToken
                                                        if tokenToUse.isEmpty {
                                                            let normMac = device.mac.uppercased().trimmingCharacters(in: .whitespacesAndNewlines)
                                                            if let savedToken = connectionManager.savedTokens[normMac] {
                                                                tokenToUse = savedToken
                                                            } else if let savedToken = UserDefaults.standard.string(forKey: "bugbuster_token") {
                                                                tokenToUse = savedToken
                                                            }
                                                        }
                                                        let success = await connectionManager.connect(ip: device.ip, token: tokenToUse)
                                                        if !success {
                                                            if connectionManager.connectionState == .unauthorized {
                                                                errorMessage = "Token Required: Enter admin access token."
                                                            } else {
                                                                errorMessage = "Failed to connect to \(device.hostname). Invalid token or device offline."
                                                            }
                                                        } else {
                                                            errorMessage = nil
                                                        }
                                                    }
                                                }) {
                                                    HStack {
                                                        Image(systemName: "cpu")
                                                            .font(.system(size: 18))
                                                            .foregroundColor(Color(red: 0.36, green: 0.84, blue: 0.74))
                                                            .frame(width: 34, height: 34)
                                                            .background(Color.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 6))
                                                        VStack(alignment: .leading, spacing: 4) {
                                                            Text(device.hostname)
                                                                .font(.system(size: 15, weight: .bold))
                                                                .foregroundColor(.white)
                                                            Text(device.ip)
                                                                .font(.system(size: 12, design: .monospaced))
                                                                .foregroundColor(.white.opacity(0.55))
                                                        }
                                                        Spacer()
                                                        Image(systemName: "chevron.right")
                                                            .font(.system(size: 14, weight: .bold))
                                                            .foregroundColor(.secondary)
                                                    }
                                                    .padding()
                                                    .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 8))
                                                }
                                                .buttonStyle(.plain)
                                            }
                                        }
                                    }
                                }
                                .padding()
                                .background(Color.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 8))

                                BLEDevicesCard(manualToken: $manualToken, errorMessage: $errorMessage)
                            }
                            .padding(.horizontal)
                        }

                        if connectionManager.connectionState != .unauthorized {
                            VStack(alignment: .leading, spacing: 12) {
                                Label("Connect manually", systemImage: "point.3.connected.trianglepath.dotted")
                                    .font(.system(size: 16, weight: .semibold))
                                    .foregroundColor(.white)

                                TextField("IP address or hostname", text: $manualIp)
                                    .textInputAutocapitalization(.never)
                                    .autocorrectionDisabled()
                                    .keyboardType(.URL)
                                    .premiumGlassInput()
                                SecureField("Admin access token", text: $manualToken)
                                    .textInputAutocapitalization(.never)
                                    .autocorrectionDisabled()
                                    .premiumGlassInput()

                                Button {
                                    Task {
                                        let success = await connectionManager.connect(ip: manualIp, token: manualToken)
                                        if !success {
                                            errorMessage = connectionManager.connectionState == .unauthorized
                                                ? "Admin token required for this instrument."
                                                : "Could not connect to \(manualIp)."
                                        } else {
                                            errorMessage = nil
                                        }
                                    }
                                } label: {
                                    HStack {
                                        Text("Connect by IP")
                                        Spacer()
                                        Image(systemName: "arrow.right")
                                    }
                                    .font(.system(size: 15, weight: .semibold))
                                    .foregroundColor(Color(red: 0.075, green: 0.08, blue: 0.09))
                                    .padding(14)
                                    .background(Color(red: 0.36, green: 0.84, blue: 0.74), in: RoundedRectangle(cornerRadius: 8))
                                }
                                .buttonStyle(.plain)
                                .disabled(manualIp.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || connectionManager.connectionState == .connecting)
                            }
                            .padding(20)
                            .background(Color.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 8))
                            .padding(.horizontal)
                        }

                        Button(action: {
                            showingScanSheet = true
                        }) {
                            HStack(spacing: 12) {
                                Image(systemName: "qrcode.viewfinder")
                                    .font(.system(size: 19, weight: .medium))
                                Text("Scan device QR code")
                                    .font(.system(size: 15, weight: .semibold))
                                Spacer()
                                Image(systemName: "arrow.up.right")
                                    .font(.system(size: 12, weight: .semibold))
                            }
                            .foregroundColor(Color(red: 0.36, green: 0.84, blue: 0.74))
                            .padding(16)
                            .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 8))
                            .padding(.horizontal)
                        }
                        .buttonStyle(.plain)

                        #if DEBUG
                        Button(action: {
                            connectionManager.connectMock()
                        }) {
                            HStack(spacing: 12) {
                                Image(systemName: "cpu")
                                    .font(.system(size: 17, weight: .medium))
                                Text("Demo instrument")
                                    .font(.system(size: 14, weight: .medium))
                            }
                            .foregroundColor(.white.opacity(0.55))
                            .frame(maxWidth: .infinity)
                            .padding(12)
                            .padding(.horizontal)
                        }
                        .buttonStyle(.plain)
                        #endif

                        Spacer(minLength: 0)
                            .padding(.bottom, 48)
                    }
                    .frame(maxWidth: 560)
                    .frame(maxWidth: .infinity)
                }
            }
            .navigationTitle("Connect")
            .navigationBarHidden(true)
            .sheet(isPresented: $showingScanSheet) {
                QRScannerView(scannedCode: { code in
                    showingScanSheet = false
                    parseScannedCode(code)
                })
                .environmentObject(connectionManager)
            }
        }
        .alert("Bluetooth Pairing", isPresented: Binding(
            get: { connectionManager.blePairingPasskey != nil },
            set: { _ in }
        )) {
            Button("Cancel", role: .cancel) { connectionManager.respondToBLEPairing(allow: false) }
            Button("Pair") { connectionManager.respondToBLEPairing(allow: true) }
        } message: {
            Text("Enter code \(connectionManager.blePairingPasskey ?? "") when iOS asks to pair with BugBuster.")
        }
        .onAppear {
            if let savedIp = UserDefaults.standard.string(forKey: "bugbuster_ip") {
                manualIp = savedIp
            }
            let lastMac = UserDefaults.standard.string(forKey: "bugbuster_last_mac") ?? ""
            let normLastMac = lastMac.uppercased().trimmingCharacters(in: .whitespacesAndNewlines)
            if !normLastMac.isEmpty, let savedToken = connectionManager.savedTokens[normLastMac] {
                manualToken = savedToken
            } else if let savedToken = UserDefaults.standard.string(forKey: "bugbuster_token") {
                manualToken = savedToken
            }
        }
    }

    private func parseScannedCode(_ code: String) {
        if connectionManager.connectionState == .unauthorized {
            let token: String
            if let url = URL(string: code), url.scheme == "bugbuster" {
                token = URLComponents(url: url, resolvingAgainstBaseURL: false)?
                    .queryItems?.first(where: { $0.name == "token" })?.value ?? ""
            } else {
                token = code.trimmingCharacters(in: .whitespacesAndNewlines)
            }
            guard !token.isEmpty else {
                errorMessage = "QR code has no admin access token."
                return
            }
            manualToken = token
            Task {
                let success: Bool
                if connectionManager.transport == .ble, let device = connectionManager.activeDevice {
                    success = await connectionManager.connectBLE(device, token: token)
                } else {
                    let ip = connectionManager.activeDevice?.ip ?? manualIp
                    success = await connectionManager.connect(ip: ip, token: token)
                }
                if !success {
                    errorMessage = "Authentication failed. Invalid token scanned."
                } else {
                    errorMessage = nil
                }
            }
            return
        }

        guard let url = URL(string: code), url.scheme == "bugbuster" else {
            errorMessage = "Invalid QR code format"
            return
        }

        let ip = url.host ?? ""
        var token = ""
        if let components = URLComponents(url: url, resolvingAgainstBaseURL: false) {
            token = components.queryItems?.first(where: { $0.name == "token" })?.value ?? ""
        }

        manualIp = ip
        manualToken = token

        Task {
            let success = await connectionManager.connect(ip: ip, token: token)
            if !success {
                errorMessage = "Failed to connect using QR credentials"
            }
        }
    }
}
