import SwiftUI
import Combine
import UIKit

struct ADCChannelConfig: Identifiable, Equatable {
    var id: Int { channel }
    let channel: Int
    var mux: Int
    var range: Int
    var rate: Int
}

class ScopeStreamManager: NSObject, ObservableObject, URLSessionDataDelegate {
    static let shared = ScopeStreamManager()
    
    @Published var sampleBuffer: [[Double]] = []
    @Published var isStreaming = false
    @Published var lastError: String? = nil
    @Published var totalSamplesReceived = 0
    
    private var session: URLSession?
    private var task: URLSessionDataTask?
    private var bufferString = ""
    
    func startStream(ip: String, token: String) {
        stopStream()
        
        guard !ip.isEmpty else {
            self.lastError = "No active device IP address"
            return
        }
        
        let urlStr = "http://\(ip)/api/scope/stream"
        guard let url = URL(string: urlStr) else {
            self.lastError = "Invalid URL"
            return
        }
        
        DispatchQueue.main.async {
            self.lastError = nil
            self.isStreaming = true
            self.totalSamplesReceived = 0
        }
        
        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        request.timeoutInterval = 600 // Long-lived SSE stream
        if !token.isEmpty {
            request.setValue(token, forHTTPHeaderField: "X-BugBuster-Admin-Token")
        }
        
        let configuration = URLSessionConfiguration.default
        configuration.timeoutIntervalForRequest = 600
        configuration.timeoutIntervalForResource = 600
        
        session = URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
        task = session?.dataTask(with: request)
        task?.resume()
    }
    
    func stopStream() {
        task?.cancel()
        task = nil
        session?.invalidateAndCancel()
        session = nil
        bufferString = ""
        DispatchQueue.main.async {
            self.isStreaming = false
        }
    }
    
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse, completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        // Essential to allow URLSession to deliver streaming chunk data
        completionHandler(.allow)
    }
    
    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        DispatchQueue.main.async {
            self.isStreaming = false
            if let error = error {
                let nsError = error as NSError
                // Do not surface standard cancellation as a connection error
                if nsError.domain == NSURLErrorDomain && nsError.code == NSURLErrorCancelled {
                    // Ignored
                } else {
                    self.lastError = error.localizedDescription
                }
            }
        }
    }
    
    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard let chunkStr = String(data: data, encoding: .utf8) else { return }
        bufferString += chunkStr
        
        let events = bufferString.components(separatedBy: "\n\n")
        if let last = events.last {
            bufferString = last
        }
        
        for event in events.dropLast() {
            let lines = event.components(separatedBy: "\n")
            for line in lines {
                if line.hasPrefix("data: ") {
                    let jsonStr = line.dropFirst(6)
                    if let jsonData = jsonStr.data(using: .utf8) {
                        do {
                            struct SSEPayload: Codable {
                                let seq: Int
                                let samples: [[Double]]
                            }
                            let payload = try JSONDecoder().decode(SSEPayload.self, from: jsonData)
                            DispatchQueue.main.async {
                                if !payload.samples.isEmpty {
                                    self.sampleBuffer.append(contentsOf: payload.samples)
                                    self.totalSamplesReceived += payload.samples.count
                                    if self.sampleBuffer.count > 500 {
                                        self.sampleBuffer.removeFirst(self.sampleBuffer.count - 500)
                                    }
                                }
                            }
                        } catch {
                            // Ignore malformed json
                        }
                    }
                }
            }
        }
    }
}

enum ScopeMode: Hashable {
    case adc, daq
}

enum DaqTimebase: String, CaseIterable, Identifiable {
    case tenSeconds = "10s"
    case thirtySeconds = "30s"
    case full = "Full"
    var id: String { rawValue }
    var seconds: Double? {
        switch self {
        case .tenSeconds: return 10
        case .thirtySeconds: return 30
        case .full: return nil
        }
    }
}

struct ScopeTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @ObservedObject private var adcStream = ScopeStreamManager.shared
    @ObservedObject private var daqStream = DaqWifiStreamManager.shared
    /// Background render pipeline for DAQ mode: reduces the engine's sample
    /// buffers to display-ready polylines at ~30 Hz off the main thread.
    @StateObject private var daqRenderModel = ScopeRenderModel()
    @ObservedObject private var orientation = ScopeOrientationState.shared
    @Environment(\.horizontalSizeClass) private var sizeClass

    @State private var mode: ScopeMode = .adc
    @State private var lastAutoDetectedDaq: Bool? = nil

    // Zoom timebase (shared across modes via ScopeCanvasView)
    @State private var timeScale: CGFloat = 1.0
    // DAQ display window: nil = full buffered span
    @State private var daqTimebase: DaqTimebase = .full

    // ADC settings
    @State private var activeChannels = [true, true, false, false]
    @State private var channelConfigs: [ADCChannelConfig] = (0..<4).map {
        ADCChannelConfig(channel: $0, mux: 0, range: 0, rate: 1)
    }

    // DAQ settings
    @State private var sampleRateIndex = 1 // default: 10 kSps
    @State private var showVoltage = true
    @State private var showCurrent = true
    @State private var showPower = false
    @State private var autoscale = true
    /// Off (default) = each active trace gets its own stacked lane with
    /// independent autoscale/autoranged units; on = all traces share one plot
    /// (legacy behavior, only readable when the traces share a scale).
    @State private var mergedTraces: Bool = {
        #if DEBUG
        if ProcessInfo.processInfo.environment["BB_MERGED_TRACES"] == "1" { return true }
        #endif
        return false
    }()

    @State private var showingSettings = false

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            VStack(spacing: 0) {
                header

                if mode == .daq, let progress = daqProvisioningOverlay {
                    Spacer()
                    daqProvisioningCard(progress)
                    Spacer()
                } else if sizeClass == .regular {
                    // iPad: full-bleed canvas regardless of physical orientation,
                    // legend/cursor readout floats as a glass panel instead of
                    // consuming a full-width row.
                    activeCanvas
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .overlay(alignment: .bottomTrailing) {
                        Group {
                            if mode == .adc {
                                channelLegend
                            } else {
                                daqLegendRow
                            }
                        }
                        // channelLegend's flexible-column grid and daqLegendRow's
                        // Spacer are both greedy about the width SwiftUI offers —
                        // fine for the old full-width row, wrong for this floating
                        // corner panel. fixedSize collapses them to content width.
                        .fixedSize()
                        .padding(12)
                        .background(
                            RoundedRectangle(cornerRadius: 16, style: .continuous)
                                .fill(.ultraThinMaterial)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                                        .fill(Color(red: 0.05, green: 0.08, blue: 0.16).opacity(0.55))
                                )
                        )
                        .padding(16)
                    }
                } else if orientation.isLandscape {
                    activeCanvas
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)

                    if mode == .adc {
                        channelLegend
                            .padding(.bottom, 8)
                    } else {
                        daqLegendRow
                            .padding(.horizontal)
                            .padding(.bottom, 8)
                    }
                } else {
                    RotatePromptView()
                }
            }
        }
        .sheet(isPresented: $showingSettings) {
            ScopeSettingsView(
                mode: $mode,
                configs: $channelConfigs,
                activeChannels: $activeChannels,
                onApplyADC: applyChannelConfig,
                sampleRateIndex: $sampleRateIndex,
                showVoltage: $showVoltage,
                showCurrent: $showCurrent,
                showPower: $showPower,
                autoscale: $autoscale,
                mergedTraces: $mergedTraces,
                onDaqRateChange: { rate in
                    daqStream.sendSetRate(currentSps: rate, voltageSps: rate)
                }
            )
        }
        .onAppear {
            setupOrientation()
            syncModeToDevice()
            daqRenderModel.updateViewport {
                $0.windowSeconds = daqTimebase.seconds
                $0.showVoltage = showVoltage
                $0.showCurrent = showCurrent
                $0.showPower = showPower
            }
            daqRenderModel.start(engine: daqStream.engine)
        }
        .onDisappear {
            daqRenderModel.stop()
            teardownOrientation()
            stopActiveStream()
        }
        .onChange(of: connectionManager.lastHatStatus?.isDaqHat) { _, _ in
            syncModeToDevice()
        }
        .onChange(of: daqTimebase) { _, tb in
            daqRenderModel.updateViewport { $0.windowSeconds = tb.seconds }
        }
        .onChange(of: showVoltage) { _, v in daqRenderModel.updateViewport { $0.showVoltage = v } }
        .onChange(of: showCurrent) { _, v in daqRenderModel.updateViewport { $0.showCurrent = v } }
        .onChange(of: showPower) { _, v in daqRenderModel.updateViewport { $0.showPower = v } }
    }

    // MARK: - Header

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(mode == .adc ? "Oscilloscope" : "DAQ Scope")
                    .font(.system(size: 24, weight: .bold))
                    .foregroundColor(.white)
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
                    .layoutPriority(1)

                HStack(spacing: 4) {
                    Circle()
                        .fill(isCurrentlyStreaming ? Color.green : Color.secondary)
                        .frame(width: 6, height: 6)
                    Text(streamStatusText)
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                }
            }
            .layoutPriority(1)
            Spacer(minLength: 8)

            Picker("Mode", selection: $mode) {
                Text("ADC").tag(ScopeMode.adc)
                Text("DAQ").tag(ScopeMode.daq)
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 140)
            .padding(.trailing, 8)

            if mode == .daq {
                Picker("Timebase", selection: $daqTimebase) {
                    ForEach(DaqTimebase.allCases) { tb in
                        Text(tb.rawValue).tag(tb)
                    }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 170)
                .padding(.trailing, 8)
            }

            GlassEffectContainer(spacing: 8) {
                HStack(spacing: 6) {
                    if mode == .daq {
                        Button(action: { daqStream.engine.resetBuffers() }) {
                            Image(systemName: "trash")
                                .font(.system(size: 14, weight: .bold))
                                .foregroundColor(.secondary)
                                .padding(10)
                                .glassEffect(.regular, in: Circle())
                        }
                    }

                    Button(action: togglePlayPause) {
                        Image(systemName: isCurrentlyStreaming ? "pause.fill" : "play.fill")
                            .font(.system(size: 16, weight: .bold))
                            .foregroundColor(isCurrentlyStreaming ? .orange : .green)
                            .padding(10)
                            .glassEffect(.regular, in: Circle())
                    }

                    Button(action: { showingSettings = true }) {
                        Image(systemName: "gearshape.fill")
                            .font(.system(size: 16))
                            .foregroundColor(.cyan)
                            .padding(10)
                            .glassEffect(.regular, in: Circle())
                    }
                }
            }
        }
        .padding(.horizontal)
        .padding(.top, orientation.isLandscape ? 8 : 16)
        .padding(.bottom, orientation.isLandscape ? 4 : 0)
    }

    private var streamStatusText: String {
        switch mode {
        case .adc:
            return adcStream.isStreaming ? "Streaming (\(adcStream.totalSamplesReceived) samples)" : "Stopped"
        case .daq:
            return daqStream.isStreaming ? "Streaming (\(daqRenderModel.frame?.recordCount ?? 0) frames)" : "Stopped"
        }
    }

    // MARK: - Legends

    private var channelLegend: some View {
        GlassEffectContainer(spacing: 8) {
            let columns = Array(repeating: GridItem(.flexible(), spacing: 10), count: 4)
            let workerEnabled = connectionManager.lastSelftest?.workerEnabled ?? false

            LazyVGrid(columns: columns, spacing: 10) {
                ForEach(0..<4) { ch in
                    let isChannelCGreyed = ch == 2 && workerEnabled
                    let accent = isChannelCGreyed ? Color.gray : ScopeColors.accents[ch]

                    Button(action: {
                        withAnimation { activeChannels[ch].toggle() }
                    }) {
                        HStack(spacing: 6) {
                            Circle().fill(accent).frame(width: 8, height: 8)
                            Text("CH\(ch + 1)")
                                .font(.system(size: 11, weight: .bold, design: .monospaced))
                                .foregroundColor(activeChannels[ch] ? .white : .secondary)
                        }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .glassEffect(
                            activeChannels[ch] ? .regular.tint(accent) : .regular,
                            in: RoundedRectangle(cornerRadius: 8, style: .continuous)
                        )
                        .opacity(isChannelCGreyed ? 0.4 : 1.0)
                        .grayscale(isChannelCGreyed ? 1.0 : 0)
                    }
                    .buttonStyle(.plain)
                    .disabled(isChannelCGreyed)
                }
            }
        }
        .padding(.horizontal)
    }

    private var daqLegendRow: some View {
        HStack(spacing: 16) {
            if showVoltage {
                legendDot(color: ScopeColors.daqVoltage, label: "Voltage (V)")
            }
            if showCurrent {
                legendDot(color: ScopeColors.daqCurrentFine, label: "Current (A)")
            }
            if showPower {
                legendDot(color: ScopeColors.daqPower, label: "Power (W)")
            }
            Spacer()
            Text(daqDiagnosticLine)
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(.secondary)
        }
    }

    /// Voltage-loss diagnostic readout: device TX vs phone RX per record
    /// type, plus voltage-ADC health. "V:ERR" means the ADC itself is down,
    /// "tx 120/0" with "rx 118/0" means voltage was never produced, and
    /// "tx 120/118 rx 118/4" means the wire is eating voltage frames.
    private var daqDiagnosticLine: String {
        let rx = daqStream.rxCounts
        guard let s = daqStream.lastStatus else {
            return "rx \(rx.i)/\(rx.v)"
        }
        var parts: [String] = []
        if let ti = s.waveIFrames, let tv = s.waveVFrames {
            parts.append("tx \(ti)/\(tv)")
        }
        parts.append("rx \(rx.i)/\(rx.v)")
        if let di = s.waveIDrops, let dv = s.waveVDrops, di + dv > 0 {
            parts.append("drop \(di)/\(dv)")
        }
        if let ok = s.voltAdcOK {
            parts.append(ok ? "V:ok" : "V:ERR")
        }
        return parts.joined(separator: "  ")
    }

    // MARK: - DAQ WiFi provisioning progress screen

    /// Shown in place of the canvas while the play button's BLE->hotspot->
    /// socket flow is in flight, driven by real P4 bring-up stage reports
    /// (`ProvisioningStage`), not a synthetic timer.
    private var daqProvisioningOverlay: (label: String, fraction: Double)? {
        switch daqStream.provisioningState {
        case .requestingStart:
            return ("Requesting hotspot from device…", 0.05)
        case .waitingForCredentials(let stage):
            return (stage.label, stage.fraction)
        case .joiningWifi:
            return ("Joining DAQ HAT WiFi network…", 0.97)
        default:
            return nil
        }
    }

    private func daqProvisioningCard(_ progress: (label: String, fraction: Double)) -> some View {
        VStack(spacing: 16) {
            ProgressView(value: progress.fraction)
                .progressViewStyle(.linear)
                .tint(.cyan)
                .frame(width: 200)
            Text(progress.label)
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(.white)
            Text("This can take up to ~15s while the DAQ HAT brings up its hotspot.")
                .font(.system(size: 11))
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 24)
        }
        .padding(24)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .frame(maxWidth: .infinity)
    }

    private func legendDot(color: Color, label: String) -> some View {
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(label)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.secondary)
        }
    }

    // MARK: - Mode-derived state for the canvases

    /// ADC keeps the legacy synchronous ScopeCanvasView (small 500-row
    /// buffer); DAQ uses the pipeline-fed DaqScopeCanvasView.
    @ViewBuilder
    private var activeCanvas: some View {
        switch mode {
        case .adc:
            ScopeCanvasView(
                series: ScopeSampleSeries.fromADC(sampleBuffer: adcStream.sampleBuffer, activeChannels: activeChannels),
                timeScale: $timeScale,
                errorMessage: currentErrorMessage,
                isWaitingForData: currentIsWaiting,
                onRetry: currentRetryAction,
                windowSeconds: nil,
                mergedTraces: mergedTraces
            )
        case .daq:
            DaqScopeCanvasView(
                model: daqRenderModel,
                errorMessage: currentErrorMessage,
                isWaitingForData: currentIsWaiting,
                mergedTraces: mergedTraces
            )
        }
    }

    private var currentErrorMessage: String? {
        switch mode {
        case .adc:
            return adcStream.lastError
        case .daq:
            if case .failed(let msg) = daqStream.provisioningState { return msg }
            return daqStream.lastError
        }
    }

    private var currentIsWaiting: Bool {
        switch mode {
        case .adc:
            return adcStream.isStreaming && adcStream.sampleBuffer.isEmpty
        case .daq:
            return daqStream.isStreaming && (daqRenderModel.frame?.traces.isEmpty ?? true)
        }
    }

    private var currentRetryAction: (() -> Void)? {
        switch mode {
        case .adc:
            return {
                adcStream.startStream(ip: connectionManager.activeDevice?.ip ?? "", token: connectionManager.adminToken)
            }
        case .daq:
            return nil
        }
    }

    private var isCurrentlyStreaming: Bool {
        mode == .adc ? adcStream.isStreaming : daqStream.isStreaming
    }

    // MARK: - Actions

    private func togglePlayPause() {
        switch mode {
        case .adc:
            if adcStream.isStreaming {
                adcStream.stopStream()
            } else {
                adcStream.startStream(ip: connectionManager.activeDevice?.ip ?? "", token: connectionManager.adminToken)
            }
        case .daq:
            if daqStream.isStreaming {
                // Pause: stop sampling but keep the hotspot/socket connected
                // so resume is instant (see pauseStream() doc comment).
                daqStream.pauseStream()
            } else if daqStream.isConnected {
                Task { await daqStream.resumeStream(ble: connectionManager.ble) }
            } else {
                Task { await daqStream.startFullStreamFlow(ble: connectionManager.ble) }
            }
        }
    }

    private func syncModeToDevice() {
        let isDaq = connectionManager.lastHatStatus?.isDaqHat ?? false
        if lastAutoDetectedDaq != isDaq {
            lastAutoDetectedDaq = isDaq
            mode = isDaq ? .daq : .adc
        }
    }

    private func setupOrientation() {
        ScopeOrientationState.shared.beginTracking()
        // On iPad the mask is already permissive app-wide (OrientationLock's
        // idiom-aware default); forcing it here would fight the split-view shell.
        guard UIDevice.current.userInterfaceIdiom == .phone else { return }
        OrientationLock.shared.mask = [.portrait, .landscapeLeft, .landscapeRight]
        if let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene {
            windowScene.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        }
    }

    private func teardownOrientation() {
        defer { ScopeOrientationState.shared.endTracking() }
        guard UIDevice.current.userInterfaceIdiom == .phone else { return }
        OrientationLock.shared.mask = .portrait
        if let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene {
            windowScene.requestGeometryUpdate(.iOS(interfaceOrientations: .portrait)) { _ in }
        }
    }

    private func stopActiveStream() {
        adcStream.stopStream()
        Task { await daqStream.requestStreamStop(ble: connectionManager.ble) }
    }

    private func applyChannelConfig(ch: Int, config: ADCChannelConfig) {
        guard let ip = connectionManager.activeDevice?.ip, !ip.isEmpty else { return }
        Task {
            do {
                let urlStr = "http://\(ip)/api/channel/\(ch)/adc/config"
                guard let url = URL(string: urlStr) else { return }
                var request = URLRequest(url: url)
                request.httpMethod = "POST"
                request.setValue("application/json", forHTTPHeaderField: "Content-Type")
                if !connectionManager.adminToken.isEmpty {
                    request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
                }
                let body: [String: Int] = [
                    "mux": config.mux,
                    "range": config.range,
                    "rate": config.rate
                ]
                request.httpBody = try? JSONSerialization.data(withJSONObject: body)

                let (_, response) = try await URLSession.shared.data(for: request)
                let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                if (200...299).contains(code) {
                    // Success, stream auto-adapts
                }
            } catch {
                // Ignore transient api errors
            }
        }
    }
}

struct ChannelConfigRowView: View {
    let ch: Int
    @Binding var config: ADCChannelConfig
    @Binding var isActive: Bool
    let onApply: (Int, ADCChannelConfig) -> Void

    let muxOptions = [
        (value: 0, name: "LF -> AGND"),
        (value: 1, name: "HF -> LF (diff)"),
        (value: 2, name: "VSENSE- -> AGND"),
        (value: 3, name: "LF -> VSENSE-"),
        (value: 4, name: "AGND -> AGND")
    ]

    let rangeOptions = [
        (value: 0, name: "0 V to +12 V"),
        (value: 1, name: "-12 V to +12 V"),
        (value: 2, name: "-312.5 mV to +312.5 mV"),
        (value: 3, name: "-0.3125 V to 0 V"),
        (value: 4, name: "0 V to +0.3125 V"),
        (value: 5, name: "0 V to +0.625 V"),
        (value: 6, name: "-104 mV to +104 mV"),
        (value: 7, name: "-2.5 V to +2.5 V")
    ]

    let rateOptions = [
        (value: 0, name: "10 SPS (HR)"),
        (value: 1, name: "20 SPS"),
        (value: 3, name: "20 SPS (HR)"),
        (value: 4, name: "200 SPS (HR v1)"),
        (value: 6, name: "200 SPS (HR)"),
        (value: 8, name: "1.2 kSPS"),
        (value: 9, name: "1.2 kSPS (HR)"),
        (value: 12, name: "4.8 kSPS"),
        (value: 13, name: "9.6 kSPS")
    ]

    private var accentColor: Color {
        ScopeColors.accents[ch % 4]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text("CHANNEL \(ch + 1)")
                    .font(.system(size: 14, weight: .black))
                    .foregroundColor(accentColor)
                Spacer()
                Toggle("", isOn: $isActive)
                    .labelsHidden()
                    .tint(accentColor)
            }

            if isActive {
                VStack(spacing: 12) {
                    HStack {
                        Text("MUX Input")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundColor(.secondary)
                        Spacer()
                        Picker("", selection: Binding(
                            get: { config.mux },
                            set: { newVal in
                                config.mux = newVal
                                onApply(ch, config)
                            }
                        )) {
                            ForEach(muxOptions, id: \.value) { opt in
                                Text(opt.name).tag(opt.value)
                            }
                        }
                        .pickerStyle(.menu)
                    }

                    Divider().background(Color.white.opacity(0.06))

                    HStack {
                        Text("Voltage Range")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundColor(.secondary)
                        Spacer()
                        Picker("", selection: Binding(
                            get: { config.range },
                            set: { newVal in
                                config.range = newVal
                                onApply(ch, config)
                            }
                        )) {
                            ForEach(rangeOptions, id: \.value) { opt in
                                Text(opt.name).tag(opt.value)
                            }
                        }
                        .pickerStyle(.menu)
                    }

                    Divider().background(Color.white.opacity(0.06))

                    HStack {
                        Text("Sample Rate")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundColor(.secondary)
                        Spacer()
                        Picker("", selection: Binding(
                            get: { config.rate },
                            set: { newVal in
                                config.rate = newVal
                                onApply(ch, config)
                            }
                        )) {
                            ForEach(rateOptions, id: \.value) { opt in
                                Text(opt.name).tag(opt.value)
                            }
                        }
                        .pickerStyle(.menu)
                    }
                }
                .padding(.top, 4)
            }
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }
}

struct ScopeSettingsView: View {
    @Environment(\.dismiss) var dismiss
    @Binding var mode: ScopeMode
    @Binding var configs: [ADCChannelConfig]
    @Binding var activeChannels: [Bool]
    let onApplyADC: (Int, ADCChannelConfig) -> Void

    @Binding var sampleRateIndex: Int
    @Binding var showVoltage: Bool
    @Binding var showCurrent: Bool
    @Binding var showPower: Bool
    @Binding var autoscale: Bool
    @Binding var mergedTraces: Bool
    let onDaqRateChange: (UInt32) -> Void

    // Effective stream rates: the ADC hardware always runs at its max ODR;
    // these map onto device-side stream decimation (P4 CTRL_MSG_SET_RATE).
    // 32k = decimation 1 at the FINE ADC's 32 kSps ODR.
    private let sampleRateOptions: [(label: String, sps: UInt32)] = [
        ("1024", 1_024),
        ("4096", 4_096),
        ("16k", 16_384),
        ("32k", 32_768),
    ]

    var body: some View {
        NavigationStack {
            ZStack {
                Color(red: 0.03, green: 0.05, blue: 0.10)
                    .ignoresSafeArea()

                ScrollView {
                    VStack(spacing: 20) {
                        Picker("Mode", selection: $mode) {
                            Text("ADC").tag(ScopeMode.adc)
                            Text("DAQ").tag(ScopeMode.daq)
                        }
                        .pickerStyle(.segmented)

                        if mode == .adc {
                            adcSection
                        } else {
                            daqSection
                            VDUTControlsCard()
                        }
                    }
                    .padding()
                }
            }
            .navigationTitle("Scope Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        dismiss()
                    }
                    .font(.system(size: 15, weight: .bold))
                    .foregroundColor(.cyan)
                }
            }
            .preferredColorScheme(.dark)
        }
    }

    private var adcSection: some View {
        GlassEffectContainer(spacing: 8) {
            VStack(spacing: 20) {
                ForEach(0..<4) { ch in
                    ChannelConfigRowView(
                        ch: ch,
                        config: $configs[ch],
                        isActive: $activeChannels[ch],
                        onApply: onApplyADC
                    )
                }

                VStack(alignment: .leading, spacing: 8) {
                    Text("ADC CONFIGURATION")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.secondary)

                    ForEach(0..<4, id: \.self) { ch in
                        if activeChannels[ch] {
                            HStack {
                                Text("CH \(ch)")
                                    .font(.system(size: 13, weight: .bold))
                                    .frame(width: 40)
                                Picker("Rate", selection: scopeRateBinding(for: ch)) {
                                    Text("10 SPS").tag(0)
                                    Text("20 SPS").tag(1)
                                    Text("1.2k").tag(2)
                                    Text("4.8k").tag(3)
                                }
                                .pickerStyle(.menu)
                                Picker("Range", selection: scopeRangeBinding(for: ch)) {
                                    Text("0-12V").tag(0)
                                    Text("\u{00B1}12V").tag(1)
                                }
                                .pickerStyle(.menu)
                            }
                        }
                    }
                }
                .padding()
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            }
        }
    }

    private var daqSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("SAMPLE RATE")
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.secondary)
            Picker("Sample Rate", selection: $sampleRateIndex) {
                ForEach(0..<sampleRateOptions.count, id: \.self) { idx in
                    Text(sampleRateOptions[idx].label).tag(idx)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: sampleRateIndex) { _, newValue in
                onDaqRateChange(sampleRateOptions[newValue].sps)
            }

            Divider().background(Color.secondary)

            Toggle("Voltage", isOn: $showVoltage).tint(ScopeColors.daqVoltage)
            Toggle("Current", isOn: $showCurrent).tint(ScopeColors.daqCurrentFine)
            Toggle("Power", isOn: $showPower).tint(ScopeColors.daqPower)

            Divider().background(Color.secondary)

            Toggle("Autoscale", isOn: $autoscale).tint(.cyan)

            Divider().background(Color.secondary)

            Toggle("Merged Traces", isOn: $mergedTraces).tint(.cyan)
            Text(mergedTraces
                 ? "All active traces share one plot."
                 : "Each active trace gets its own lane with independent autoscale and autoranged units (V/mV/µV, A/mA/µA, etc).")
                .font(.system(size: 11))
                .foregroundColor(.secondary)
        }
        .font(.system(size: 13, weight: .medium))
        .foregroundColor(.white)
        .padding(12)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    private func scopeRateBinding(for ch: Int) -> Binding<Int> {
        let rateMap = [0, 1, 8, 12]
        return Binding(
            get: { rateMap.firstIndex(of: configs[ch].rate) ?? 1 },
            set: { newIdx in
                configs[ch].rate = rateMap[newIdx]
                onApplyADC(ch, configs[ch])
            }
        )
    }

    private func scopeRangeBinding(for ch: Int) -> Binding<Int> {
        return Binding(
            get: { min(configs[ch].range, 1) },
            set: { newVal in
                configs[ch].range = newVal
                onApplyADC(ch, configs[ch])
            }
        )
    }
}
