import SwiftUI
import Combine

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

struct ScopeTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @ObservedObject private var streamManager = ScopeStreamManager.shared
    
    // Zoom timebase gesture states
    @State private var timeScale: CGFloat = 1.0
    @GestureState private var gestureScale: CGFloat = 1.0
    
    // Settings configuration
    @State private var activeChannels = [true, true, false, false]
    @State private var showingSettings = false
    @State private var channelConfigs: [ADCChannelConfig] = (0..<4).map {
        ADCChannelConfig(channel: $0, mux: 0, range: 0, rate: 1)
    }
    
    @State private var touchLocation: CGPoint? = nil
    
    struct TouchInfo {
        let channelIndex: Int
        let voltage: Double
        let time: Double
        let x: CGFloat
        let y: CGFloat
    }
    
    let ACCENTS = [
        Color(red: 0.23, green: 0.51, blue: 0.96), // Blue
        Color(red: 0.06, green: 0.73, blue: 0.51), // Emerald
        Color(red: 0.96, green: 0.62, blue: 0.04), // Amber
        Color(red: 0.66, green: 0.33, blue: 0.97)  // Purple
    ]
    
    var activeScale: CGFloat {
        max(0.2, min(5.0, timeScale * gestureScale))
    }
    
    var body: some View {
        VStack(spacing: 0) {
            // Header toolbar
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Oscilloscope")
                        .font(.system(size: 24, weight: .bold))
                        .foregroundColor(.white)
                    
                    HStack(spacing: 4) {
                        Circle()
                            .fill(streamManager.isStreaming ? Color.green : Color.secondary)
                            .frame(width: 6, height: 6)
                        Text(streamManager.isStreaming ? "Streaming (\(streamManager.totalSamplesReceived) samples)" : "Stopped")
                            .font(.system(size: 10, weight: .bold))
                            .foregroundColor(.secondary)
                    }
                }
                Spacer()
                
                // Play / Pause + Settings buttons wrapped in GlassEffectContainer
                GlassEffectContainer(spacing: 8) {
                    // Play / Pause button
                    Button(action: {
                        if streamManager.isStreaming {
                            streamManager.stopStream()
                        } else {
                            streamManager.startStream(
                                ip: connectionManager.activeDevice?.ip ?? "",
                                token: connectionManager.adminToken
                            )
                        }
                    }) {
                        Image(systemName: streamManager.isStreaming ? "pause.fill" : "play.fill")
                            .font(.system(size: 16, weight: .bold))
                            .foregroundColor(streamManager.isStreaming ? .orange : .green)
                            .padding(10)
                            .glassEffect(.regular, in: Circle())
                    }
                    .padding(.trailing, 6)
                    
                    Button(action: {
                        showingSettings = true
                    }) {
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
            
            // Scope screen graph
            GeometryReader { geometry in
                ZStack {
                    ScopeGridView()
                    
                    let bounds = calculateVisibleBounds(
                        samples: streamManager.sampleBuffer,
                        activeChannels: activeChannels,
                        timeScale: activeScale
                    )
                    
                    // Draw waveforms
                    ForEach(0..<4) { ch in
                        if activeChannels[ch] {
                            ScopeLineShape(
                                samples: streamManager.sampleBuffer,
                                channelIndex: ch,
                                timeScale: activeScale,
                                minVal: bounds.min,
                                maxVal: bounds.max
                            )
                            .stroke(ACCENTS[ch], lineWidth: 2)
                        }
                    }
                    
                    // Dynamic labels overlay (Y axis bounds)
                    VStack {
                        HStack {
                            Text(String(format: "%.2f V", bounds.max))
                                .font(.system(size: 10, weight: .bold, design: .monospaced))
                                .foregroundColor(.secondary)
                                .padding(4)
                                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 4, style: .continuous))
                            Spacer()
                        }
                        Spacer()
                        HStack {
                            Text(String(format: "%.2f V", bounds.min))
                                .font(.system(size: 10, weight: .bold, design: .monospaced))
                                .foregroundColor(.secondary)
                                .padding(4)
                                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 4, style: .continuous))
                            Spacer()
                        }
                    }
                    .padding(8)
                    
                    // Touch cursor and tooltip bubble
                    if let touch = touchLocation {
                        let info = findClosestSampleInfo(
                            at: touch,
                            in: geometry.size,
                            samples: streamManager.sampleBuffer,
                            activeChannels: activeChannels,
                            timeScale: activeScale,
                            minVal: bounds.min,
                            maxVal: bounds.max
                        )
                        
                        if let info = info {
                            // Vertical cursor line
                            Path { path in
                                path.move(to: CGPoint(x: info.x, y: 0))
                                path.addLine(to: CGPoint(x: info.x, y: geometry.size.height))
                            }
                            .stroke(Color.white.opacity(0.4), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
                            
                            // Highlight point on the closest channel
                            Circle()
                                .fill(ACCENTS[info.channelIndex])
                                .frame(width: 8, height: 8)
                                .position(x: info.x, y: info.y)
                                .shadow(color: ACCENTS[info.channelIndex], radius: 4)
                            
                            // Tooltip bubble
                            VStack(alignment: .leading, spacing: 4) {
                                Text("CH\(info.channelIndex + 1)")
                                    .font(.system(size: 11, weight: .bold))
                                    .foregroundColor(ACCENTS[info.channelIndex])
                                Text(String(format: "%.3f V", info.voltage))
                                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                                    .foregroundColor(.white)
                                Text(String(format: "T: %.3fs", info.time))
                                    .font(.system(size: 10, weight: .medium))
                                    .foregroundColor(.secondary)
                            }
                            .padding(8)
                            .glassEffect(.regular.tint(ACCENTS[info.channelIndex]), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                            // Position bubble near the touch/point, keeping it within bounds
                            .position(
                                x: max(60, min(geometry.size.width - 60, info.x + (info.x > geometry.size.width / 2 ? -70 : 70))),
                                y: max(40, min(geometry.size.height - 40, info.y + (info.y > geometry.size.height / 2 ? -50 : 50)))
                            )
                        }
                    }
                    
                    // Error & Empty state overlays
                    if let error = streamManager.lastError {
                        VStack(spacing: 12) {
                            Image(systemName: "exclamationmark.triangle.fill")
                                .font(.system(size: 32))
                                .foregroundColor(.orange)
                            Text("Connection Error")
                                .font(.headline)
                                .foregroundColor(.white)
                            Text(error)
                                .font(.system(size: 12))
                                .foregroundColor(.secondary)
                                .multilineTextAlignment(.center)
                                .padding(.horizontal, 16)
                            
                            Button(action: {
                                streamManager.startStream(
                                    ip: connectionManager.activeDevice?.ip ?? "",
                                    token: connectionManager.adminToken
                                )
                            }) {
                                Text("Retry")
                                    .font(.system(size: 13, weight: .bold))
                                    .foregroundColor(.cyan)
                                    .padding(.horizontal, 20)
                                    .padding(.vertical, 8)
                                    .glassEffect(.regular.tint(.cyan), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                            }
                        }
                        .padding()
                        .frame(maxWidth: 280)
                        .glassEffect(.regular.tint(.red), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                    } else if streamManager.isStreaming && streamManager.sampleBuffer.isEmpty {
                        VStack(spacing: 8) {
                            ProgressView()
                                .tint(.cyan)
                            Text("Waiting for waveform data...")
                                .font(.system(size: 12))
                                .foregroundColor(.secondary)
                        }
                        .padding()
                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
                    }
                }
                .contentShape(Rectangle())
                .simultaneousGesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { value in
                            if gestureScale == 1.0 {
                                touchLocation = value.location
                            } else {
                                touchLocation = nil
                            }
                        }
                        .onEnded { _ in
                            touchLocation = nil
                        }
                )
                .simultaneousGesture(
                    MagnificationGesture()
                        .updating($gestureScale) { value, state, _ in
                            state = value
                        }
                        .onEnded { value in
                            timeScale = max(0.2, min(5.0, timeScale * value))
                        }
                )
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .padding()
            
            // Channel legend selector
            GlassEffectContainer(spacing: 8) {
                HStack(spacing: 12) {
                    ForEach(0..<4) { ch in
                        Button(action: {
                            withAnimation {
                                activeChannels[ch].toggle()
                            }
                        }) {
                            HStack(spacing: 6) {
                                Circle()
                                    .fill(ACCENTS[ch])
                                    .frame(width: 8, height: 8)
                                Text("CH\(ch + 1)")
                                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                                    .foregroundColor(activeChannels[ch] ? .white : .secondary)
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 8)
                            .glassEffect(
                                activeChannels[ch]
                                    ? .regular.tint(ACCENTS[ch])
                                    : .regular,
                                in: RoundedRectangle(cornerRadius: 8, style: .continuous)
                            )
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            .padding(.bottom, 20)
        }
        .sheet(isPresented: $showingSettings) {
            ScopeSettingsView(
                configs: $channelConfigs,
                activeChannels: $activeChannels,
                onApply: { ch, config in
                    applyChannelConfig(ch: ch, config: config)
                }
            )
        }
    }
    
    private func findClosestSampleInfo(
        at touch: CGPoint,
        in size: CGSize,
        samples: [[Double]],
        activeChannels: [Bool],
        timeScale: CGFloat,
        minVal: Double,
        maxVal: Double
    ) -> TouchInfo? {
        guard !samples.isEmpty else { return nil }
        
        let maxDisplayCount = max(1, min(samples.count, Int(Double(samples.count) / Double(timeScale))))
        let startIndex = max(0, samples.count - maxDisplayCount)
        let displaySamples = Array(samples[startIndex..<samples.count])
        
        guard !displaySamples.isEmpty else { return nil }
        
        let tStart = displaySamples.first?[0] ?? 0
        let tEnd = displaySamples.last?[0] ?? tStart
        let tSpan = tEnd - tStart
        
        var closestSampleIndex = 0
        var minDistanceX: CGFloat = .infinity
        var closestX: CGFloat = 0
        
        for (idx, sample) in displaySamples.enumerated() {
            let t = sample[0]
            let x = tSpan > 0 
                ? CGFloat((t - tStart) / tSpan) * size.width
                : CGFloat(idx) / CGFloat(displaySamples.count - 1) * size.width
            
            let dist = abs(x - touch.x)
            if dist < minDistanceX {
                minDistanceX = dist
                closestSampleIndex = idx
                closestX = x
            }
        }
        
        let sample = displaySamples[closestSampleIndex]
        let t = sample[0]
        
        var closestChannel: Int? = nil
        var minDistanceY: CGFloat = .infinity
        var closestY: CGFloat = 0
        var closestVoltage: Double = 0
        
        let valSpan = maxVal - minVal
        let valSpanSafe = valSpan > 0.001 ? valSpan : 1.0
        
        for ch in 0..<4 {
            if activeChannels[ch] && sample.count > ch + 1 {
                let val = sample[ch + 1]
                let y = size.height - CGFloat((val - minVal) / valSpanSafe) * size.height
                let dist = abs(y - touch.y)
                if dist < minDistanceY {
                    minDistanceY = dist
                    closestChannel = ch
                    closestY = y
                    closestVoltage = val
                }
            }
        }
        
        if let ch = closestChannel {
            return TouchInfo(
                channelIndex: ch,
                voltage: closestVoltage,
                time: t,
                x: closestX,
                y: closestY
            )
        }
        
        return nil
    }
    
    private func calculateVisibleBounds(samples: [[Double]], activeChannels: [Bool], timeScale: CGFloat) -> (min: Double, max: Double) {
        guard !samples.isEmpty else { return (-1.0, 1.0) }
        
        let maxDisplayCount = max(1, min(samples.count, Int(Double(samples.count) / Double(timeScale))))
        let startIndex = max(0, samples.count - maxDisplayCount)
        let displaySamples = Array(samples[startIndex..<samples.count])
        
        var minVal = Double.infinity
        var maxVal = -Double.infinity
        
        for sample in displaySamples {
            for ch in 0..<4 {
                if activeChannels[ch] && sample.count > ch + 1 {
                    let val = sample[ch + 1]
                    if val < minVal { minVal = val }
                    if val > maxVal { maxVal = val }
                }
            }
        }
        
        if minVal == Double.infinity || maxVal == -Double.infinity {
            return (-1.0, 1.0)
        }
        
        let span = maxVal - minVal
        let padding = span > 0.001 ? span * 0.1 : 1.0
        return (minVal - padding, maxVal + padding)
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

struct ScopeGridView: View {
    var body: some View {
        GeometryReader { geometry in
            Path { path in
                let hSpacing = geometry.size.height / 8
                for i in 1..<8 {
                    let y = CGFloat(i) * hSpacing
                    path.move(to: CGPoint(x: 0, y: y))
                    path.addLine(to: CGPoint(x: geometry.size.width, y: y))
                }
                
                let wSpacing = geometry.size.width / 10
                for i in 1..<10 {
                    let x = CGFloat(i) * wSpacing
                    path.move(to: CGPoint(x: x, y: 0))
                    path.addLine(to: CGPoint(x: x, y: geometry.size.height))
                }
            }
            .stroke(Color.white.opacity(0.04), lineWidth: 1)
        }
    }
}

struct ScopeLineShape: Shape {
    let samples: [[Double]]
    let channelIndex: Int
    let timeScale: CGFloat
    let minVal: Double
    let maxVal: Double

    func path(in rect: CGRect) -> Path {
        var path = Path()
        guard samples.count >= 2 else { return path }

        let maxDisplayCount = max(1, min(samples.count, Int(Double(samples.count) / Double(timeScale))))
        let startIndex = max(0, samples.count - maxDisplayCount)
        let displaySamples = Array(samples[startIndex..<samples.count])

        guard let firstPoint = displaySamples.first else { return path }
        let tStart = firstPoint[0]
        let tEnd = displaySamples.last?[0] ?? tStart
        let tSpan = tEnd - tStart

        let valSpan = maxVal - minVal
        let valSpanSafe = valSpan > 0.001 ? valSpan : 1.0

        for (idx, sample) in displaySamples.enumerated() {
            guard sample.count > channelIndex + 1 else { continue }
            let t = sample[0]
            let val = sample[channelIndex + 1]

            let x = tSpan > 0 
                ? CGFloat((t - tStart) / tSpan) * rect.width
                : CGFloat(idx) / CGFloat(displaySamples.count - 1) * rect.width

            let y = rect.height - CGFloat((val - minVal) / valSpanSafe) * rect.height

            let point = CGPoint(x: x, y: y)
            if idx == 0 {
                path.move(to: point)
            } else {
                path.addLine(to: point)
            }
        }
        return path
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
        let colors = [
            Color(red: 0.23, green: 0.51, blue: 0.96),
            Color(red: 0.06, green: 0.73, blue: 0.51),
            Color(red: 0.96, green: 0.62, blue: 0.04),
            Color(red: 0.66, green: 0.33, blue: 0.97)
        ]
        return colors[ch % 4]
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
    @Binding var configs: [ADCChannelConfig]
    @Binding var activeChannels: [Bool]
    let onApply: (Int, ADCChannelConfig) -> Void
    
    var body: some View {
        NavigationStack {
            ZStack {
                Color(red: 0.03, green: 0.05, blue: 0.10)
                    .ignoresSafeArea()
                
                ScrollView {
                    GlassEffectContainer(spacing: 8) {
                        VStack(spacing: 20) {
                            ForEach(0..<4) { ch in
                                ChannelConfigRowView(
                                    ch: ch,
                                    config: $configs[ch],
                                    isActive: $activeChannels[ch],
                                    onApply: onApply
                                )
                            }
                        }
                        .padding()
                    }
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
}
