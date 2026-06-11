import SwiftUI
import Network
import AVFoundation

// MARK: - Liquid Glass tab bar (iOS 26+)
// Uses the real GlassEffectContainer + .glassEffect() APIs so the active-tab
// bubble genuinely merges with the outer pill — exactly like Apple Music.
struct CustomTabBar: View {
    @Binding var selectedTab: Int
    let tabs: [(icon: String, name: String)]
    let safeAreaBottom: CGFloat
    @Namespace private var glassNS

    var body: some View {
        GlassEffectContainer(spacing: 10) {
            HStack(spacing: 0) {
                ForEach(0..<tabs.count, id: \.self) { index in
                    tabButton(index: index)
                }
            }
            .padding(.horizontal, 6)
            .padding(.top, 4)
            // Bottom padding absorbs home-indicator inset exactly — no extra.
            .padding(.bottom, max(safeAreaBottom - 6, 4))
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 30, style: .continuous))
        }
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private func tabButton(index: Int) -> some View {
        let isActive = selectedTab == index
        Button {
            withAnimation(.spring(response: 0.38, dampingFraction: 0.80)) {
                selectedTab = index
            }
        } label: {
            VStack(spacing: 2) {
                Image(systemName: tabs[index].icon)
                    .font(.system(size: 16, weight: isActive ? .semibold : .regular))
                    .scaleEffect(isActive ? 1.05 : 1.0)
                    .animation(.spring(response: 0.3, dampingFraction: 0.65), value: isActive)

                Text(tabs[index].name)
                    .font(.system(size: 9, weight: isActive ? .semibold : .regular))
            }
            .foregroundStyle(isActive ? AnyShapeStyle(.primary) : AnyShapeStyle(.secondary))
            .frame(maxWidth: .infinity)
            .padding(.vertical, 5)
            .glassEffect(
                isActive ? Glass.regular : Glass.clear,
                in: RoundedRectangle(cornerRadius: 18, style: .continuous)
            )
            .glassEffectID(isActive ? "active" : nil, in: glassNS)
        }
        .buttonStyle(.plain)
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
                GeometryReader { geometry in
                    ZStack(alignment: .bottom) {
                        Color(red: 0.03, green: 0.05, blue: 0.10)
                            .ignoresSafeArea()

                        // Tab content
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
                        // Spacer = pill visible content height (6 top + 7+18+2+13+7 item + some slack).
                        // The pill's own internal bottom padding absorbs safeAreaBottom.
                        .safeAreaInset(edge: .bottom, spacing: 0) {
                            Color.clear.frame(height: 54)
                        }

                        // Floating Liquid Glass pill — flush at the physical bottom
                        CustomTabBar(
                            selectedTab: $selectedTab,
                            tabs: tabs,
                            safeAreaBottom: geometry.safeAreaInsets.bottom
                        )
                        .ignoresSafeArea(edges: .bottom)
                    }
                    .ignoresSafeArea(.keyboard, edges: .bottom)
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
            if let savedToken = UserDefaults.standard.string(forKey: "bugbuster_token") {
                manualToken = savedToken
            }
        }
    }
    
    var connectionDashboard: some View {
        NavigationStack {
            ZStack {
                // Premium background gradient
                LinearGradient(
                    colors: [Color(red: 0.05, green: 0.08, blue: 0.16), Color(red: 0.02, green: 0.03, blue: 0.06)],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                )
                .ignoresSafeArea()
                
                ScrollView {
                    VStack(spacing: 24) {
                        // Title Section
                        VStack(spacing: 8) {
                            Image(systemName: "bolt.shield.fill")
                                .font(.system(size: 60))
                                .foregroundStyle(
                                    LinearGradient(
                                        colors: [.blue, .cyan],
                                        startPoint: .top,
                                        endPoint: .bottom
                                    )
                                )
                                .padding(.top, 40)
                                .shadow(color: .blue.opacity(0.3), radius: 15)
                            
                            Text("BugBuster")
                                .font(.system(size: 36, weight: .bold, design: .rounded))
                                .tracking(1)
                            
                            Text("Bench Instrument Controller")
                                .font(.system(size: 16, weight: .medium))
                                .foregroundColor(.secondary)
                        }
                        
                        if let error = errorMessage {
                            Text(error)
                                .font(.system(size: 14))
                                .foregroundColor(.red)
                                .padding(12)
                                .frame(maxWidth: .infinity)
                                .glassEffect(.regular.tint(.red), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                                .padding(.horizontal)
                        }
                        
                        // Connection State indicator
                        if connectionManager.connectionState == .connecting {
                            VStack(spacing: 12) {
                                ProgressView()
                                    .tint(.blue)
                                Text("Connecting to hardware...")
                                    .font(.system(size: 14, weight: .medium))
                                    .foregroundColor(.secondary)
                            }
                            .padding()
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                        }
                        
                        if connectionManager.connectionState == .unauthorized {
                            // Unauthorized / Token Request UI
                            GlassEffectContainer(spacing: 12) {
                                VStack(spacing: 16) {
                                    Image(systemName: "lock.shield.fill")
                                        .font(.system(size: 44))
                                        .foregroundStyle(
                                            LinearGradient(
                                                colors: [.orange, .orange],
                                                startPoint: .top,
                                                endPoint: .bottom
                                            )
                                        )
                                        .shadow(color: .orange.opacity(0.3), radius: 10)
                                        .padding(.top, 16)
                                    
                                    Text("Token Required")
                                        .font(.system(size: 20, weight: .bold, design: .rounded))
                                    
                                    Text("Please enter the admin access token for the device at:\n\(connectionManager.activeDevice?.ip ?? "BugBuster Board")")
                                        .font(.system(size: 14))
                                        .foregroundColor(.secondary)
                                        .multilineTextAlignment(.center)
                                        .padding(.horizontal)
                                    
                                    HStack(spacing: 8) {
                                        SecureField("Admin Access Token", text: $manualToken)
                                            .autocorrectionDisabled()
                                            .textInputAutocapitalization(.never)
                                            .padding()
                                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                                        
                                        Button(action: {
                                            showingScanSheet = true
                                        }) {
                                            Image(systemName: "qrcode.viewfinder")
                                                .font(.system(size: 20))
                                                .foregroundColor(.cyan)
                                                .padding()
                                                .glassEffect(.regular.tint(.cyan), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                                        }
                                        .buttonStyle(.plain)
                                    }
                                    .padding(.horizontal)
                                    
                                    HStack(spacing: 16) {
                                        Button("Cancel") {
                                            connectionManager.disconnect()
                                            errorMessage = nil
                                        }
                                        .font(.system(size: 15, weight: .semibold))
                                        .foregroundColor(.secondary)
                                        .frame(maxWidth: .infinity)
                                        .padding()
                                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                                        
                                        Button("Authenticate") {
                                            Task {
                                                let ip = connectionManager.activeDevice?.ip ?? manualIp
                                                let success = await connectionManager.connect(ip: ip, token: manualToken)
                                                if !success {
                                                    errorMessage = "Authentication failed. Invalid token."
                                                } else {
                                                    errorMessage = nil
                                                }
                                            }
                                        }
                                        .font(.system(size: 15, weight: .bold))
                                        .foregroundColor(.black)
                                        .frame(maxWidth: .infinity)
                                        .padding()
                                        .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                                    }
                .padding(.horizontal)
                                    .padding(.bottom, 16)
                                }
                                .glassEffect(.regular.tint(.orange), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
                            }
                            .padding(.horizontal)
                        } else {
                            // Normal Dashboard UI (Scan + Connect)
                            GlassEffectContainer(spacing: 16) {
                                // Discovered Devices Section
                                VStack(alignment: .leading, spacing: 12) {
                                    HStack {
                                        Text("Discovered Devices")
                                            .font(.system(size: 18, weight: .semibold))
                                        Spacer()
                                        if connectionManager.isSearching {
                                            ProgressView()
                                                .tint(.blue)
                                        } else {
                                            Button("Scan") {
                                                connectionManager.startDiscovery()
                                            }
                                            .font(.system(size: 14, weight: .medium))
                                            .foregroundColor(.black)
                                            .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                                        }
                                    }
                                    .padding(.horizontal)
                                    
                                    if connectionManager.discoveredDevices.isEmpty {
                                        HStack(spacing: 12) {
                                            Image(systemName: "wifi")
                                                .font(.system(size: 24))
                                                .foregroundColor(.secondary)
                                            VStack(alignment: .leading) {
                                                Text("Searching Bonjour...")
                                                    .font(.system(size: 14, weight: .medium))
                                                Text("Ensure hardware is powered & on same Wi-Fi.")
                                                    .font(.system(size: 12))
                                                    .foregroundColor(.secondary)
                                            }
                                        }
                                        .padding()
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                                        .padding(.horizontal)
                                    } else {
                                        ForEach(connectionManager.discoveredDevices) { device in
                                            Button(action: {
                                                Task {
                                                    var tokenToUse = manualToken
                                                    if tokenToUse.isEmpty {
                                                        if let savedToken = UserDefaults.standard.string(forKey: "bugbuster_token") {
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
                                                            .font(.system(size: 16, weight: .semibold))
                                                            .foregroundColor(.primary)
                                                        Text(device.ip)
                                                            .font(.system(size: 13, design: .monospaced))
                                                            .foregroundColor(.secondary)
                                                    }
                                                    Spacer()
                                                    Image(systemName: "chevron.right")
                                                        .foregroundColor(.secondary)
                                                }
                                                .padding()
                                                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                                            }
                                            .padding(.horizontal)
                                            .buttonStyle(.plain)
                                        }
                                    }
                                }
                                
                                // Manual Entry Form
                                VStack(alignment: .leading, spacing: 12) {
                                    Text("Manual Connection")
                                        .font(.system(size: 18, weight: .semibold))
                                        .padding(.horizontal)
                                    
                                    VStack(spacing: 16) {
                                        TextField("IP Address or Hostname", text: $manualIp)
                                            .keyboardType(.numbersAndPunctuation)
                                            .autocorrectionDisabled()
                                            .textInputAutocapitalization(.never)
                                            .padding()
                                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                                        
                                        SecureField("Admin Access Token", text: $manualToken)
                                            .autocorrectionDisabled()
                                            .textInputAutocapitalization(.never)
                                            .padding()
                                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                                        
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
                                                .glassEffect(.regular.tint(.blue), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                                        }
                                        .buttonStyle(.plain)
                                    }
                                    .padding()
                                    .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                                    .padding(.horizontal)
                                }
                            }
                        }
                        
                        // QR Scanner Button
                        Button(action: {
                            showingScanSheet = true
                        }) {
                            HStack {
                                Image(systemName: "qrcode.viewfinder")
                                    .font(.system(size: 20))
                                Text("Scan Device QR Code")
                                    .font(.system(size: 16, weight: .semibold))
                            }
                            .foregroundColor(.cyan)
                            .frame(maxWidth: .infinity)
                            .padding()
                            .glassEffect(.regular.tint(.cyan), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                            .foregroundColor(.black)
                            .padding(.horizontal)
                        }
                        .padding(.bottom, 40)
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
