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

struct ScopeTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @ObservedObject private var adcStream = ScopeStreamManager.shared
    @ObservedObject private var daqStream = DaqWifiStreamManager.shared
    @ObservedObject private var orientation = ScopeOrientationState.shared

    @State private var mode: ScopeMode = .adc
    @State private var lastAutoDetectedDaq: Bool? = nil

    // Zoom timebase (shared across modes via ScopeCanvasView)
    @State private var timeScale: CGFloat = 1.0

    // ADC settings
    @State private var activeChannels = [true, true, false, false]
    @State private var channelConfigs: [ADCChannelConfig] = (0..<4).map {
        ADCChannelConfig(channel: $0, mux: 0, range: 0, rate: 1)
    }

    // DAQ settings
    @State private var sampleRateIndex = 1 // default: 10 kSps
    @State private var showVoltage = true
    @State private var showCurrent = true
    @State private var showPower = true
    @State private var autoscale = true

    @State private var showingSettings = false

    var body: some View {
        VStack(spacing: 0) {
            header

            if orientation.isLandscape {
                ScopeCanvasView(
                    series: currentSeries,
                    timeScale: $timeScale,
                    errorMessage: currentErrorMessage,
                    isWaitingForData: currentIsWaiting,
                    onRetry: currentRetryAction
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                .padding()

                if mode == .adc {
                    channelLegend
                        .padding(.bottom, 20)
                } else {
                    daqLegendRow
                        .padding(.horizontal)
                        .padding(.bottom, 20)
                }
            } else {
                RotatePromptView()
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
                onDaqRateChange: { rate in
                    daqStream.sendSetRate(currentSps: rate, voltageSps: rate)
                }
            )
        }
        .onAppear {
            setupOrientation()
            syncModeToDevice()
        }
        .onDisappear {
            teardownOrientation()
            stopActiveStream()
        }
        .onChange(of: connectionManager.lastHatStatus?.isDaqHat) { _, _ in
            syncModeToDevice()
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(mode == .adc ? "Oscilloscope" : "DAQ Scope")
                    .font(.system(size: 24, weight: .bold))
                    .foregroundColor(.white)

                HStack(spacing: 4) {
                    Circle()
                        .fill(isCurrentlyStreaming ? Color.green : Color.secondary)
                        .frame(width: 6, height: 6)
                    Text(streamStatusText)
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.secondary)
                }
            }
            Spacer()

            Picker("Mode", selection: $mode) {
                Text("ADC").tag(ScopeMode.adc)
                Text("DAQ").tag(ScopeMode.daq)
            }
            .pickerStyle(.segmented)
            .frame(width: 140)
            .padding(.trailing, 8)

            GlassEffectContainer(spacing: 8) {
                Button(action: togglePlayPause) {
                    Image(systemName: isCurrentlyStreaming ? "pause.fill" : "play.fill")
                        .font(.system(size: 16, weight: .bold))
                        .foregroundColor(isCurrentlyStreaming ? .orange : .green)
                        .padding(10)
                        .glassEffect(.regular, in: Circle())
                }
                .padding(.trailing, 6)

                Button(action: { showingSettings = true }) {
                    Image(systemName: "gearshape.fill")
                        .font(.system(size: 16))
                        .foregroundColor(.cyan)
                        .padding(10)
                        .glassEffect(.regular, in: Circle())
                }
            }
        }
        .padding(.horizontal)
        .padding(.top, 16)
    }

    private var streamStatusText: String {
        switch mode {
        case .adc:
            return adcStream.isStreaming ? "Streaming (\(adcStream.totalSamplesReceived) samples)" : "Stopped"
        case .daq:
            return daqStream.isStreaming ? "Streaming (\(daqStream.totalRecordsReceived) frames)" : "Stopped"
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
            Spacer()
            Text("\(daqStream.totalRecordsReceived) frames")
                .font(.system(size: 10, weight: .medium, design: .monospaced))
                .foregroundColor(.secondary)
        }
    }

    private func legendDot(color: Color, label: String) -> some View {
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(label)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.secondary)
        }
    }

    // MARK: - Mode-derived state for ScopeCanvasView

    private var currentSeries: ScopeSampleSeries {
        switch mode {
        case .adc:
            return ScopeSampleSeries.fromADC(sampleBuffer: adcStream.sampleBuffer, activeChannels: activeChannels)
        case .daq:
            return ScopeSampleSeries.fromDAQ(
                voltageSamples: daqStream.voltageSamples,
                currentSamples: daqStream.currentSamples,
                currentSampleSources: daqStream.currentSampleSources,
                showVoltage: showVoltage,
                showCurrent: showCurrent
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
            return daqStream.isStreaming && daqStream.voltageSamples.isEmpty && daqStream.currentSamples.isEmpty
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
            if daqStream.isStreaming || daqStream.isConnected {
                Task { await daqStream.requestStreamStop(ble: connectionManager.ble) }
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
        OrientationLock.shared.mask = [.portrait, .landscapeLeft, .landscapeRight]
    }

    private func teardownOrientation() {
        OrientationLock.shared.mask = .portrait
        if let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene {
            windowScene.requestGeometryUpdate(.iOS(interfaceOrientations: .portrait)) { _ in }
        }
        ScopeOrientationState.shared.endTracking()
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
    let onDaqRateChange: (UInt32) -> Void

    private let sampleRateOptions: [(label: String, sps: UInt32)] = [
        ("1 kSps", 1_000),
        ("10 kSps", 10_000),
        ("50 kSps", 50_000),
        ("250 kSps", 250_000),
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
            Toggle("Power", isOn: $showPower).tint(ScopeColors.daqCurrentCoarse)

            Divider().background(Color.secondary)

            Toggle("Autoscale", isOn: $autoscale).tint(.cyan)
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
