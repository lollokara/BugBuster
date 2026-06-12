import SwiftUI
import Network
import AVFoundation

// MARK: - Custom Glassmorphic Tab Bar
struct CustomTabBar: View {
    @Binding var selectedTab: Int
    let tabs: [(icon: String, name: String)]
    @Namespace private var animationNamespace

    var body: some View {
        HStack(spacing: 6) {
            ForEach(0..<tabs.count, id: \.self) { index in
                let isActive = selectedTab == index
                Button {
                    withAnimation(.spring(response: 0.30, dampingFraction: 0.76, blendDuration: 0)) {
                        selectedTab = index
                    }
                } label: {
                    VStack(spacing: 4) {
                        Image(systemName: tabs[index].icon)
                            .font(.system(size: 18, weight: .semibold))
                            .foregroundStyle(isActive ? .primary : .secondary)
                            .scaleEffect(isActive ? 1.08 : 1.0)
                        
                        Text(tabs[index].name)
                            .font(.system(size: 9, weight: isActive ? .semibold : .regular))
                            .foregroundColor(isActive ? .primary : .secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 10)
                    .background(
                        ZStack {
                            if isActive {
                                RoundedRectangle(cornerRadius: 18, style: .continuous)
                                    .fill(
                                        LinearGradient(
                                            colors: [Color.white.opacity(0.22), Color.cyan.opacity(0.12)],
                                            startPoint: .topLeading,
                                            endPoint: .bottomTrailing
                                        )
                                    )
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 18, style: .continuous)
                                            .stroke(Color.white.opacity(0.16), lineWidth: 1)
                                    )
                                    .shadow(color: Color.black.opacity(0.18), radius: 10, x: 0, y: 4)
                                    .matchedGeometryEffect(id: "activeTabHighlight", in: animationNamespace)
                            }
                        }
                    )
                }
                .buttonStyle(.plain)
            }
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 34, style: .continuous)
                .fill(Color(red: 0.05, green: 0.08, blue: 0.16).opacity(0.30))
                .background(.ultraThinMaterial)
                .overlay(
                    RoundedRectangle(cornerRadius: 34, style: .continuous)
                        .stroke(
                            LinearGradient(
                                colors: [Color.white.opacity(0.20), Color.white.opacity(0.06)],
                                startPoint: .top,
                                endPoint: .bottom
                            ),
                            lineWidth: 1
                        )
                )
                .shadow(color: Color.black.opacity(0.34), radius: 18, x: 0, y: 10)
        )
        .frame(maxWidth: .infinity)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 34, style: .continuous))
    }
}


struct MainTabView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var selectedTab = 0
    @State private var showingScanSheet = false
    @State private var manualIp = ""
    @State private var manualToken = ""
    @State private var errorMessage: String? = nil
    
    let tabs = [
        (icon: "waveform.path.ecg", name: "Overview"),
        (icon: "arrow.up.left.and.down.right.and.arrow.up.right.and.down.left", name: "Signal Path"),
        (icon: "waveform", name: "Scope"),
        (icon: "cpu", name: "Diagnostics"),
        (icon: "doc.text.magnifyingglass", name: "Scripts")
    ]
    
    var body: some View {
        Group {
            if connectionManager.connectionState == .connected {
                ZStack {
                    connectedBackground

                    Group {
                        switch selectedTab {
                        case 0:  OverviewTab()
                        case 1:  SignalPathTab()
                        case 2:  ScopeTab()
                        case 3:  DiagnosticsTab()
                        case 4:  ScriptsTab()
                        default: OverviewTab()
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .safeAreaInset(edge: .bottom, spacing: 0) {
                    CustomTabBar(
                        selectedTab: $selectedTab,
                        tabs: tabs
                    )
                    .padding(.horizontal, 16)
                    .padding(.top, 10)
                    .padding(.bottom, 12)
                }
            } else {
                connectionDashboard
            }
        }
        .preferredColorScheme(.dark)
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

    private var connectedBackground: some View {
        ZStack {
            LinearGradient(
                colors: [
                    Color(red: 0.02, green: 0.04, blue: 0.09),
                    Color(red: 0.05, green: 0.08, blue: 0.16),
                    Color(red: 0.03, green: 0.05, blue: 0.11)
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )

            RadialGradient(
                colors: [Color.cyan.opacity(0.14), .clear],
                center: .topLeading,
                startRadius: 24,
                endRadius: 420
            )
            .blendMode(.screen)
            .blur(radius: 18)

            RadialGradient(
                colors: [Color.blue.opacity(0.12), .clear],
                center: .topTrailing,
                startRadius: 28,
                endRadius: 480
            )
            .blendMode(.screen)
            .blur(radius: 20)

            RadialGradient(
                colors: [Color.purple.opacity(0.08), .clear],
                center: .bottom,
                startRadius: 40,
                endRadius: 520
            )
            .blendMode(.screen)
            .blur(radius: 24)
        }
        .ignoresSafeArea()
    }
    
    var connectionDashboard: some View {
        NavigationStack {
            ZStack {
                // Premium background gradient (Deep Obsidian/Midnight Blue to Dark Navy)
                LinearGradient(
                    colors: [Color(red: 0.03, green: 0.05, blue: 0.10), Color(red: 0.06, green: 0.10, blue: 0.20)],
                    startPoint: .top,
                    endPoint: .bottom
                )
                .ignoresSafeArea()
                
                ScrollView {
                    VStack(spacing: 28) {
                        // Title Section
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
                        
                        // Connection State indicator
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
                            // Unauthorized / Token Request UI
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
                                            let ip = connectionManager.activeDevice?.ip ?? manualIp
                                            let success = await connectionManager.connect(ip: ip, token: manualToken)
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
                            // Normal Dashboard UI (Scan + Connect)
                            VStack(spacing: 24) {
                                // Discovered Devices Section
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
                                
                                // Manual Entry Form
                                VStack(alignment: .leading, spacing: 14) {
                                    Text("Manual Connection")
                                        .font(.system(size: 18, weight: .bold))
                                        .foregroundColor(.white)
                                    
                                    VStack(spacing: 16) {
                                        TextField("IP Address or Hostname", text: $manualIp)
                                            .keyboardType(.numbersAndPunctuation)
                                            .autocorrectionDisabled()
                                            .textInputAutocapitalization(.never)
                                            .premiumGlassInput()
                                        
                                        SecureField("Admin Access Token", text: $manualToken)
                                            .autocorrectionDisabled()
                                            .textInputAutocapitalization(.never)
                                            .premiumGlassInput()
                                        
                                        Button(action: {
                                            guard !manualIp.isEmpty else {
                                                errorMessage = "Please enter an IP address"
                                                return
                                            }
                                            Task {
                                                let success = await connectionManager.connect(ip: manualIp, token: manualToken)
                                                if !success {
                                                    errorMessage = "Failed to connect. Verify IP & Token."
                                                }
                                            }
                                        }) {
                                            Text("Connect")
                                                .font(.system(size: 16, weight: .bold))
                                                .foregroundColor(.black)
                                                .frame(maxWidth: .infinity)
                                                .padding()
                                                .background(
                                                    LinearGradient(
                                                        colors: [Color.cyan, Color.blue],
                                                        startPoint: .top,
                                                        endPoint: .bottom
                                                    )
                                                )
                                                .cornerRadius(12)
                                                .shadow(color: .cyan.opacity(0.3), radius: 6, x: 0, y: 2)
                                        }
                                        .buttonStyle(.plain)
                                    }
                                }
                                .padding()
                                .background(
                                    RoundedRectangle(cornerRadius: 18)
                                        .fill(Color.white.opacity(0.03))
                                        .overlay(RoundedRectangle(cornerRadius: 18).stroke(Color.white.opacity(0.06), lineWidth: 1))
                                )
                            }
                            .premiumGlassCard()
                            .padding(.horizontal)
                        }
                        
                        // QR Scanner Button
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
    }
    
    private func parseScannedCode(_ code: String) {
        // Handle plain token scanning when in unauthorized state
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
        
        // Handle full device URL: bugbuster://<ip>?token=<token>
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

// Camera QR Code Scanner Subview
struct QRScannerView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    var scannedCode: (String) -> Void
    @Environment(\.dismiss) var dismiss
    
    var body: some View {
        NavigationStack {
            ZStack {
                Color.black.ignoresSafeArea()
                
                #if targetEnvironment(simulator)
                VStack(spacing: 20) {
                    Image(systemName: "camera.fill")
                        .font(.system(size: 50))
                        .foregroundColor(.secondary)
                    Text("Camera not available on simulator.")
                        .font(.headline)
                    Button("Simulate QR Code Scan") {
                        if connectionManager.connectionState == .unauthorized {
                            scannedCode("admin_secret_token")
                        } else {
                            scannedCode("bugbuster://bugbuster-s3.local?token=admin_secret_token")
                        }
                    }
                    .buttonStyle(.borderedProminent)
                }
                .foregroundColor(.white)
                #else
                ScannerViewControllerWrapper(scannedCode: scannedCode)
                    .ignoresSafeArea()
                #endif
            }
            .navigationTitle("Scan QR Code")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        dismiss()
                    }
                }
            }
        }
    }
}

#if !targetEnvironment(simulator)
struct ScannerViewControllerWrapper: UIViewControllerRepresentable {
    var scannedCode: (String) -> Void
    
    func makeUIViewController(context: Context) -> ScannerViewController {
        let controller = ScannerViewController()
        controller.delegate = context.coordinator
        return controller
    }
    
    func updateUIViewController(_ uiViewController: ScannerViewController, context: Context) {}
    
    func makeCoordinator() -> Coordinator {
        Coordinator(scannedCode: scannedCode)
    }
    
    class Coordinator: NSObject, AVCaptureMetadataOutputObjectsDelegate {
        var scannedCode: (String) -> Void
        
        init(scannedCode: @escaping (String) -> Void) {
            self.scannedCode = scannedCode
        }
        
        func metadataOutput(_ output: AVCaptureMetadataOutput, didOutput metadataObjects: [AVMetadataObject], from connection: AVCaptureConnection) {
            if let metadataObject = metadataObjects.first {
                guard let readableObject = metadataObject as? AVMetadataMachineReadableCodeObject else { return }
                guard let stringValue = readableObject.stringValue else { return }
                AudioServicesPlaySystemSound(SystemSoundID(kSystemSoundID_Vibrate))
                scannedCode(stringValue)
            }
        }
    }
}

class ScannerViewController: UIViewController {
    var captureSession: AVCaptureSession!
    var previewLayer: AVCaptureVideoPreviewLayer!
    weak var delegate: AVCaptureMetadataOutputObjectsDelegate?
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        view.backgroundColor = UIColor.black
        captureSession = AVCaptureSession()
        
        guard let videoCaptureDevice = AVCaptureDevice.default(for: .video) else { return }
        let videoInput: AVCaptureDeviceInput
        
        do {
            videoInput = try AVCaptureDeviceInput(device: videoCaptureDevice)
        } catch {
            return
        }
        
        if (captureSession.canAddInput(videoInput)) {
            captureSession.addInput(videoInput)
        } else {
            return
        }
        
        let metadataOutput = AVCaptureMetadataOutput()
        
        if (captureSession.canAddOutput(metadataOutput)) {
            captureSession.addOutput(metadataOutput)
            
            metadataOutput.setMetadataObjectsDelegate(delegate, queue: DispatchQueue.main)
            metadataOutput.metadataObjectTypes = [.qr]
        } else {
            return
        }
        
        previewLayer = AVCaptureVideoPreviewLayer(session: captureSession)
        previewLayer.frame = view.layer.bounds
        previewLayer.videoGravity = .resizeAspectFill
        view.layer.addSublayer(previewLayer)
        
        DispatchQueue.global(qos: .userInitiated).async {
            self.captureSession.startRunning()
        }
    }
    
    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        
        if (captureSession?.isRunning == true) {
            captureSession.stopRunning()
        }
    }
    
    override var prefersStatusBarHidden: Bool {
        return true
    }
    
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        return .portrait
    }
}
#endif

// MARK: - Premium Glass UI Helpers
extension View {
    func premiumGlassCard(cornerRadius: CGFloat = 20, shadowRadius: CGFloat = 12) -> some View {
        self
            .padding()
            .background(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .fill(Color(red: 0.04, green: 0.06, blue: 0.12).opacity(0.45))
                    .background(.ultraThinMaterial)
                    .overlay(
                        RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                            .stroke(
                                LinearGradient(
                                    colors: [Color.white.opacity(0.12), Color.white.opacity(0.02)],
                                    startPoint: .top,
                                    endPoint: .bottom
                                ),
                                lineWidth: 1
                            )
                    )
            )
            .shadow(color: Color.black.opacity(0.4), radius: shadowRadius, x: 0, y: 6)
    }
    
    func premiumGlassInput() -> some View {
        self
            .padding()
            .background(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(Color.black.opacity(0.3))
                    .overlay(
                        RoundedRectangle(cornerRadius: 12, style: .continuous)
                            .stroke(Color.white.opacity(0.08), lineWidth: 1)
                    )
            )
            .foregroundColor(.white)
    }
}
