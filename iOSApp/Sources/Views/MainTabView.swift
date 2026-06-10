import SwiftUI
import Network
import AVFoundation

struct CustomTabBar: View {
    @Binding var selectedTab: Int
    let tabs: [(icon: String, name: String)]
    @Namespace private var tabAnimation

    var body: some View {
        VStack(spacing: 0) {
            // Hair-line separator so glass edge is visible against content
            Rectangle()
                .fill(Color.white.opacity(0.10))
                .frame(height: 0.5)

            HStack(spacing: 0) {
                ForEach(0..<tabs.count, id: \.self) { index in
                    let isActive = selectedTab == index
                    Button {
                        withAnimation(.spring(response: 0.38, dampingFraction: 0.72)) {
                            selectedTab = index
                        }
                    } label: {
                        VStack(spacing: 4) {
                            ZStack {
                                // Sliding blue capsule behind the active icon
                                if isActive {
                                    Capsule()
                                        .fill(
                                            LinearGradient(
                                                colors: [Color.blue.opacity(0.30), Color.cyan.opacity(0.14)],
                                                startPoint: .topLeading,
                                                endPoint: .bottomTrailing
                                            )
                                        )
                                        .matchedGeometryEffect(id: "tab_bg", in: tabAnimation)
                                        .frame(width: 54, height: 32)
                                }

                                Image(systemName: tabs[index].icon)
                                    .font(.system(size: 19, weight: isActive ? .bold : .medium))
                                    .foregroundStyle(
                                        isActive
                                            ? LinearGradient(colors: [.blue, .cyan], startPoint: .top, endPoint: .bottom)
                                            : LinearGradient(colors: [Color(white: 0.50), Color(white: 0.50)], startPoint: .top, endPoint: .bottom)
                                    )
                                    .shadow(color: isActive ? Color.blue.opacity(0.65) : .clear, radius: 7)
                                    .scaleEffect(isActive ? 1.08 : 1.0)
                                    .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isActive)
                            }
                            .frame(width: 54, height: 32)

                            Text(tabs[index].name)
                                .font(.system(size: 10, weight: isActive ? .semibold : .regular))
                                .foregroundColor(isActive ? .blue : Color(white: 0.42))
                                .animation(.easeInOut(duration: 0.18), value: isActive)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.top, 10)
                        .padding(.bottom, 6)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        // Glass background that EXTENDS below the safe area so it covers the
        // home indicator — no black bar, no separate spacer needed.
        .background {
            Rectangle()
                .fill(.ultraThinMaterial)
                .ignoresSafeArea(edges: .bottom)
        }
        // Force the entire bar (including its background) into dark appearance
        // so the material renders as dark frosted glass, not white.
        .environment(\.colorScheme, .dark)
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
        (icon: "cpu", name: "Diagnostics"),
        (icon: "doc.text.magnifyingglass", name: "Scripts")
    ]
    
    var body: some View {
        Group {
            if connectionManager.connectionState == .connected {
                ZStack {
                    Color(red: 0.03, green: 0.05, blue: 0.10)
                        .ignoresSafeArea()

                    Group {
                        switch selectedTab {
                        case 0:  OverviewTab()
                        case 1:  SignalPathTab()
                        case 2:  DiagnosticsTab()
                        case 3:  ScriptsTab()
                        default: OverviewTab()
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                // safeAreaInset is the correct approach: content is pushed up by the
                // tab bar height AND the material blurs the content behind it.
                .safeAreaInset(edge: .bottom, spacing: 0) {
                    CustomTabBar(selectedTab: $selectedTab, tabs: tabs)
                }
                .ignoresSafeArea(.keyboard, edges: .bottom)
            } else {
                connectionDashboard
            }
        }
        .preferredColorScheme(.dark)
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
                                .background(Color.red.opacity(0.1))
                                .cornerRadius(12)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 12)
                                        .stroke(Color.red.opacity(0.3), lineWidth: 1)
                                )
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
                            .background(Color.black.opacity(0.2))
                            .cornerRadius(16)
                        }
                        
                        if connectionManager.connectionState == .unauthorized {
                            // Unauthorized / Token Request UI
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
                                        .background(Color.white.opacity(0.05))
                                        .cornerRadius(12)
                                        .overlay(
                                            RoundedRectangle(cornerRadius: 12)
                                                .stroke(Color.white.opacity(0.1), lineWidth: 1)
                                        )
                                    
                                    Button(action: {
                                        showingScanSheet = true
                                    }) {
                                        Image(systemName: "qrcode.viewfinder")
                                            .font(.system(size: 20))
                                            .foregroundColor(.cyan)
                                            .padding()
                                            .background(Color.cyan.opacity(0.1))
                                            .cornerRadius(12)
                                            .overlay(
                                                RoundedRectangle(cornerRadius: 12)
                                                    .stroke(Color.cyan.opacity(0.3), lineWidth: 1)
                                            )
                                    }
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
                                    .background(Color.white.opacity(0.05))
                                    .cornerRadius(12)
                                    
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
                                    .foregroundColor(.white)
                                    .frame(maxWidth: .infinity)
                                    .padding()
                                    .background(
                                        LinearGradient(
                                            colors: [.blue, .cyan],
                                            startPoint: .leading,
                                            endPoint: .trailing
                                        )
                                    )
                                    .cornerRadius(12)
                                }
                                .padding(.horizontal)
                                .padding(.bottom, 16)
                            }
                            .background(.ultraThinMaterial)
                            .cornerRadius(20)
                            .overlay(
                                RoundedRectangle(cornerRadius: 20)
                                    .stroke(Color.orange.opacity(0.3), lineWidth: 1)
                            )
                            .padding(.horizontal)
                        } else {
                            // Normal Dashboard UI (Scan + Connect)
                            
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
                                    .background(.ultraThinMaterial)
                                    .cornerRadius(16)
                                    .padding(.horizontal)
                                } else {
                                    ForEach(connectionManager.discoveredDevices) { device in
                                        Button(action: {
                                            Task {
                                                var tokenToUse = manualToken
                                                if tokenToUse.isEmpty {
                                                    if let savedIp = UserDefaults.standard.string(forKey: "bugbuster_ip"),
                                                       savedIp == device.ip,
                                                       let savedToken = UserDefaults.standard.string(forKey: "bugbuster_token") {
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
                                            .background(.ultraThinMaterial)
                                            .cornerRadius(16)
                                            .overlay(
                                                RoundedRectangle(cornerRadius: 16)
                                                    .stroke(Color.white.opacity(0.08), lineWidth: 1)
                                            )
                                        }
                                        .padding(.horizontal)
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
                                        .background(Color.white.opacity(0.05))
                                        .cornerRadius(12)
                                        .overlay(
                                            RoundedRectangle(cornerRadius: 12)
                                                .stroke(Color.white.opacity(0.1), lineWidth: 1)
                                        )
                                    
                                    SecureField("Admin Access Token", text: $manualToken)
                                        .autocorrectionDisabled()
                                        .textInputAutocapitalization(.never)
                                        .padding()
                                        .background(Color.white.opacity(0.05))
                                        .cornerRadius(12)
                                        .overlay(
                                            RoundedRectangle(cornerRadius: 12)
                                                .stroke(Color.white.opacity(0.1), lineWidth: 1)
                                        )
                                    
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
                                            .foregroundColor(.white)
                                            .frame(maxWidth: .infinity)
                                            .padding()
                                            .background(
                                                LinearGradient(
                                                    colors: [.blue, .cyan],
                                                    startPoint: .leading,
                                                    endPoint: .trailing
                                                )
                                            )
                                            .cornerRadius(12)
                                    }
                                }
                                .padding()
                                .background(.ultraThinMaterial)
                                .cornerRadius(16)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 16)
                                        .stroke(Color.white.opacity(0.05), lineWidth: 1)
                                )
                                .padding(.horizontal)
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
                            .background(Color.cyan.opacity(0.1))
                            .cornerRadius(12)
                            .overlay(
                                RoundedRectangle(cornerRadius: 12)
                                    .stroke(Color.cyan.opacity(0.3), lineWidth: 1)
                            )
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
