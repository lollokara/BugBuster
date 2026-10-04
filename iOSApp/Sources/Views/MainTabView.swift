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
            // .fill(.ultraThinMaterial) keeps the material inside the pill;
            // .background(.ultraThinMaterial) on a Shape fills its rectangular
            // bounding box and renders a visible slab behind the bar.
            RoundedRectangle(cornerRadius: 34, style: .continuous)
                .fill(.ultraThinMaterial)
                .overlay(
                    RoundedRectangle(cornerRadius: 34, style: .continuous)
                        .fill(Color(red: 0.05, green: 0.08, blue: 0.16).opacity(0.55))
                )
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
    }
}


struct MainTabView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @EnvironmentObject var scripts: ScriptRunManager
    @Environment(\.horizontalSizeClass) private var sizeClass
    @ObservedObject private var scopeOrientation = ScopeOrientationState.shared
    @State private var selectedTab = AppSections.initialSectionFromEnvironment

    var tabs: [(icon: String, name: String)] {
        AppSections.visible(daqHat: connectionManager.lastHatStatus?.isDaqHat ?? false).map { (icon: $0.icon, name: $0.name) }
    }

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
                        case 5:  BatterySimTab()
                        default: OverviewTab()
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .onChange(of: connectionManager.lastHatStatus?.isDaqHat ?? false) { _, daq in
                        if !daq && selectedTab == 5 { selectedTab = 0 }
                    }

                    // Toast overlay
                    if let toast = connectionManager.toastMessage {
                        VStack {
                            HStack(spacing: 8) {
                                Image(systemName: toast.type == .success ? "checkmark.circle.fill" : toast.type == .error ? "xmark.circle.fill" : "info.circle.fill")
                                    .foregroundColor(toast.type == .success ? .green : toast.type == .error ? .red : .blue)
                                Text(toast.text)
                                    .font(.system(size: 13, weight: .semibold))
                                    .foregroundColor(.white)
                            }
                            .padding(.horizontal, 16)
                            .padding(.vertical, 10)
                            .glassEffect(.regular, in: Capsule())
                            .shadow(color: .black.opacity(0.3), radius: 10)
                            .padding(.top, 60)
                            Spacer()
                        }
                        .transition(.move(edge: .top).combined(with: .opacity))
                        .animation(.spring(response: 0.3), value: connectionManager.toastMessage)
                        .allowsHitTesting(false)
                    }
                }
                .safeAreaInset(edge: .bottom, spacing: 0) {
                    Group {
                        // Scope tab (index 2) owns its own landscape mode; hide the shared
                        // tab bar while it's rotated so the scope can use the full screen.
                        if !(selectedTab == 2 && scopeOrientation.isLandscape) {
                            VStack(spacing: 8) {
                                // Running-script pill: outside the tab switch, on every tab.
                                ScriptPillHost(onScriptsTab: selectedTab == 4)
                                    .padding(.horizontal, 16)
                                CustomTabBar(
                                    selectedTab: $selectedTab,
                                    tabs: tabs
                                )
                                .padding(.horizontal, sizeClass == .regular ? 80 : 16)
                                .padding(.top, 6)
                                // Sit closer to the home indicator: shrink the measured
                                // inset so the pill shifts down into the bottom safe area.
                                .padding(.bottom, -10)
                            }
                            .animation(.snappy, value: scripts.status?.isActive)
                        }
                    }
                }
                // Tabs (Scripts editor/REPL) manage keyboard clearance themselves;
                // without this the system also squeezes content and the tab bar
                // rides up above the keyboard.
                .ignoresSafeArea(.keyboard)
                .onChange(of: selectedTab) { _ in
                    UIImpactFeedbackGenerator(style: .light).impactOccurred()
                }
                .sheet(isPresented: $scripts.consoleVisible) {
                    ScriptLogPanel()
                        .environmentObject(scripts)
                        .presentationDetents([.medium, .large])
                        .presentationDragIndicator(.visible)
                        .presentationBackground(.ultraThinMaterial)
                }
            } else {
                ConnectionDashboardView()
            }
        }
        .preferredColorScheme(.dark)
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

// MARK: - Bluetooth (BLE) Devices Card

/// Discovery + connect UI for BugBuster peripherals over Bluetooth Low Energy.
/// Mirrors the Wi-Fi "Discovered Devices" card; uses the same admin token.
struct BLEDevicesCard: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @Binding var manualToken: String
    @Binding var errorMessage: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label("Bluetooth", systemImage: "wave.3.right")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundColor(.white)
                Spacer()
                if connectionManager.bleScanning {
                    ProgressView().tint(Color(red: 0.36, green: 0.84, blue: 0.74))
                } else {
                    Button(action: { connectionManager.startBLEScan() }) {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundColor(Color(red: 0.36, green: 0.84, blue: 0.74))
                            .frame(width: 32, height: 32)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Scan Bluetooth devices")
                }
            }

            if !connectionManager.bleAvailable {
                infoRow(icon: "antenna.radiowaves.left.and.right.slash",
                        title: "Bluetooth unavailable",
                        subtitle: "Enable Bluetooth and grant permission to scan.")
            } else if connectionManager.bleDevices.isEmpty {
                infoRow(icon: "dot.radiowaves.left.and.right",
                        title: connectionManager.bleScanning ? "Searching nearby..." : "No Bluetooth devices found",
                        subtitle: "Check that your instrument is powered on and nearby.")
            } else {
                VStack(spacing: 10) {
                    ForEach(connectionManager.bleDevices) { device in
                        Button(action: { connectBLE(device) }) {
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
                                    Text("Bluetooth LE")
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
        .onAppear { connectionManager.startBLEScan() }
    }

    private func infoRow(icon: String, title: String, subtitle: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 26))
                .foregroundColor(.white.opacity(0.55))
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.white)
                Text(subtitle)
                    .font(.system(size: 12))
                    .foregroundColor(.white.opacity(0.56))
            }
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.white.opacity(0.04), in: RoundedRectangle(cornerRadius: 8))
    }

    private func connectBLE(_ device: DiscoveredDevice) {
        Task {
            let token = manualToken
            let success = await connectionManager.connectBLE(device, token: token)
            if !success {
                if connectionManager.connectionState == .unauthorized {
                    errorMessage = "Token Required: Enter admin access token."
                } else {
                    errorMessage = "Failed to connect to \(device.hostname) over Bluetooth."
                }
            } else {
                errorMessage = nil
            }
        }
    }
}

// MARK: - Premium Glass UI Helpers
extension View {
    func premiumGlassCard(cornerRadius: CGFloat = 20, shadowRadius: CGFloat = 12) -> some View {
        self
            .padding()
            .background(
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .fill(.ultraThinMaterial)
                    .overlay(
                        RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                            .fill(Color(red: 0.04, green: 0.06, blue: 0.12).opacity(0.45))
                    )
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
