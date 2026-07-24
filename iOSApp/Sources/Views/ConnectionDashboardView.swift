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
                LinearGradient(
                    colors: [Color(red: 0.03, green: 0.05, blue: 0.10), Color(red: 0.06, green: 0.10, blue: 0.20)],
                    startPoint: .top,
                    endPoint: .bottom
                )
                .ignoresSafeArea()

                ScrollView {
                    VStack(spacing: 28) {
                        VStack(spacing: 12) {
                            Image(systemName: "bolt.shield.fill")
                                .font(.system(size: 64))
                                .foregroundStyle(
                                    LinearGradient(
                                        colors: [.cyan, .blue],
                                        startPoint: .topLeading,
                                        endPoint: .bottomTrailing
                                    )
                                )
                                .padding(.top, 48)
                                .shadow(color: .cyan.opacity(0.35), radius: 15)

                            Text("BugBuster")
                                .font(.system(size: 38, weight: .bold, design: .rounded))
                                .foregroundColor(.white)
                                .tracking(1)

                            Text("Bench Instrument Controller")
                                .font(.system(size: 15, weight: .medium))
                                .foregroundColor(.secondary)
                        }

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
                            HStack(spacing: 16) {
                                ProgressView()
                                    .tint(.cyan)
                                Text("Connecting to hardware...")
                                    .font(.system(size: 15, weight: .medium))
                                    .foregroundColor(.primary)
                            }
                            .premiumGlassCard()
                            .padding(.horizontal)
                        }

                        if connectionManager.connectionState == .unauthorized {
                            VStack(spacing: 20) {
                                VStack(spacing: 12) {
                                    Image(systemName: "lock.shield.fill")
                                        .font(.system(size: 48))
                                        .foregroundColor(.orange)
                                        .shadow(color: .orange.opacity(0.4), radius: 10)
                                        .padding(.top, 8)

                                    Text("Authentication Required")
                                        .font(.system(size: 22, weight: .bold, design: .rounded))
                                        .foregroundColor(.white)

                                    Text("Please enter the admin access token for the device at:")
                                        .font(.system(size: 14))
                                        .foregroundColor(.secondary)
                                        .multilineTextAlignment(.center)
                                        .padding(.horizontal)

                                    Text(connectionManager.activeDevice?.ip ?? "BugBuster Board")
                                        .font(.system(size: 15, weight: .semibold, design: .monospaced))
                                        .foregroundColor(.cyan)
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
                                            .foregroundColor(.cyan)
                                            .padding(14)
                                            .background(
                                                RoundedRectangle(cornerRadius: 12)
                                                    .fill(Color.cyan.opacity(0.15))
                                                    .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.cyan.opacity(0.3), lineWidth: 1))
                                            )
                                    }
                                    .buttonStyle(.plain)
                                }
                                .padding(.horizontal)

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
                                            .background(
                                                RoundedRectangle(cornerRadius: 12)
                                                    .fill(Color.white.opacity(0.08))
                                                    .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.white.opacity(0.12), lineWidth: 1))
                                            )
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
                                            .foregroundColor(.black)
                                            .frame(maxWidth: .infinity)
                                            .padding()
                                            .background(
                                                RoundedRectangle(cornerRadius: 12)
                                                    .fill(
                                                        LinearGradient(
                                                            colors: [Color.cyan, Color.blue],
                                                            startPoint: .top,
                                                            endPoint: .bottom
                                                        )
                                                    )
                                                    .shadow(color: .cyan.opacity(0.3), radius: 6, x: 0, y: 2)
                                            )
                                    }
                                    .buttonStyle(.plain)
                                }
                                .padding(.horizontal)
                                .padding(.bottom, 12)
                            }
                            .premiumGlassCard()
                            .padding(.horizontal)
                        } else {
                            VStack(spacing: 24) {
                                VStack(alignment: .leading, spacing: 14) {
                                    HStack {
                                        Text("Discovered Devices")
                                            .font(.system(size: 18, weight: .bold))
                                            .foregroundColor(.white)
                                        Spacer()
                                        if connectionManager.isSearching {
                                            ProgressView()
                                                .tint(.cyan)
                                        } else {
                                            Button(action: {
                                                connectionManager.startDiscovery()
                                            }) {
                                                Text("Scan")
                                                    .font(.system(size: 13, weight: .semibold))
                                                    .foregroundColor(.cyan)
                                                    .padding(.horizontal, 12)
                                                    .padding(.vertical, 6)
                                                    .background(Capsule().stroke(Color.cyan.opacity(0.4), lineWidth: 1))
                                            }
                                            .buttonStyle(.plain)
                                        }
                                    }

                                    if connectionManager.discoveredDevices.isEmpty {
                                        HStack(spacing: 12) {
                                            Image(systemName: "wifi.router.fill")
                                                .font(.system(size: 26))
                                                .foregroundColor(.secondary)
                                            VStack(alignment: .leading, spacing: 4) {
                                                Text("Searching Bonjour...")
                                                    .font(.system(size: 14, weight: .semibold))
                                                    .foregroundColor(.primary)
                                                Text("Ensure hardware is powered & on same Wi-Fi.")
                                                    .font(.system(size: 12))
                                                    .foregroundColor(.secondary)
                                            }
                                        }
                                        .padding()
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                        .background(RoundedRectangle(cornerRadius: 14).fill(Color.white.opacity(0.04)))
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
                                                        VStack(alignment: .leading, spacing: 4) {
                                                            Text(device.hostname)
                                                                .font(.system(size: 15, weight: .bold))
                                                                .foregroundColor(.white)
                                                            Text(device.ip)
                                                                .font(.system(size: 12, design: .monospaced))
                                                                .foregroundColor(.cyan)
                                                        }
                                                        Spacer()
                                                        Image(systemName: "chevron.right")
                                                            .font(.system(size: 14, weight: .bold))
                                                            .foregroundColor(.secondary)
                                                    }
                                                    .padding()
                                                    .background(
                                                        RoundedRectangle(cornerRadius: 14)
                                                            .fill(Color.white.opacity(0.05))
                                                            .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.white.opacity(0.08), lineWidth: 1))
                                                    )
                                                }
                                                .buttonStyle(.plain)
                                            }
                                        }
                                    }
                                }
                                .padding()
                                .background(
                                    RoundedRectangle(cornerRadius: 18)
                                        .fill(Color.white.opacity(0.03))
                                        .overlay(RoundedRectangle(cornerRadius: 18).stroke(Color.white.opacity(0.06), lineWidth: 1))
                                )

                                BLEDevicesCard(manualToken: $manualToken, errorMessage: $errorMessage)
                            }
                            .premiumGlassCard()
                            .padding(.horizontal)
                        }

                        Button(action: {
                            showingScanSheet = true
                        }) {
                            HStack(spacing: 12) {
                                Image(systemName: "qrcode.viewfinder")
                                    .font(.system(size: 22, weight: .semibold))
                                Text("Scan Device QR Code")
                                    .font(.system(size: 16, weight: .bold))
                            }
                            .foregroundColor(.cyan)
                            .frame(maxWidth: .infinity)
                            .padding()
                            .background(
                                RoundedRectangle(cornerRadius: 14)
                                    .fill(Color.cyan.opacity(0.12))
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 14)
                                            .stroke(Color.cyan.opacity(0.35), lineWidth: 1.5)
                                    )
                            )
                            .padding(.horizontal)
                        }
                        .buttonStyle(.plain)

                        #if DEBUG
                        Button(action: {
                            connectionManager.connectMock()
                        }) {
                            HStack(spacing: 12) {
                                Image(systemName: "wand.and.stars")
                                    .font(.system(size: 20, weight: .semibold))
                                Text("Use Mock Device (Debug)")
                                    .font(.system(size: 15, weight: .bold))
                            }
                            .foregroundColor(.purple)
                            .frame(maxWidth: .infinity)
                            .padding()
                            .background(
                                RoundedRectangle(cornerRadius: 14)
                                    .fill(Color.purple.opacity(0.12))
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 14)
                                            .stroke(Color.purple.opacity(0.35), lineWidth: 1.5)
                                    )
                            )
                            .padding(.horizontal)
                        }
                        .buttonStyle(.plain)
                        #endif

                        Spacer(minLength: 0)
                            .padding(.bottom, 48)
                    }
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
            manualToken = code
            Task {
                let ip = connectionManager.activeDevice?.ip ?? manualIp
                let success = await connectionManager.connect(ip: ip, token: code)
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
