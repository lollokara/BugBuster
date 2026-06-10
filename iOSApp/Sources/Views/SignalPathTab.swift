import SwiftUI

struct SignalPathTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var opStatus: String? = nil
    @State private var lastWriteMs: Double = 0
    
    let PRESETS: [(name: String, states: [Int])] = [
        ("All Open", [0x00, 0x00, 0x00, 0x00]),
        ("GPIO Direct", [0x51, 0x51, 0x51, 0x51]),
        ("ADC Read", [0x04, 0x04, 0x04, 0x04]),
        ("External", [0x08, 0x08, 0x08, 0x08])
    ]
    
    let C_GPIO = Color(red: 0.13, green: 0.77, blue: 0.37)     // Green
    let C_GPIO_R = Color(red: 0.92, green: 0.70, blue: 0.03)   // Yellow
    let C_ADC = Color(red: 0.23, green: 0.51, blue: 0.96)      // Blue
    let C_EXT = Color(red: 0.98, green: 0.45, blue: 0.09)      // Orange
    let C_BG = Color(red: 0.03, green: 0.05, blue: 0.10)
    let C_CHIP = Color(red: 0.05, green: 0.09, blue: 0.16)
    let C_CHIP_BD = Color(red: 0.12, green: 0.19, blue: 0.31)
    
    let ACCENTS = [
        Color(red: 0.23, green: 0.51, blue: 0.96),
        Color(red: 0.06, green: 0.73, blue: 0.51),
        Color(red: 0.96, green: 0.62, blue: 0.04),
        Color(red: 0.66, green: 0.33, blue: 0.97)
    ]
    
    let MUX_REF = ["U10", "U11", "U17", "U16"]
    let MUX_DEVICE_BY_LOGICAL = [0, 1, 3, 2]
    
    let GPIO_PAIR_LABELS = [
        ["IO3", "IO2", "IO1"],
        ["IO6", "IO5", "IO4"],
        ["IO9", "IO8", "IO7"],
        ["IO12", "IO11", "IO10"]
    ]
    
    let ADC_LABELS = ["CH A", "CH B", "CH C", "CH D"]
    let EXT_LABELS = ["EXT 1", "EXT 2", "EXT 3", "EXT 4"]
    let EFUSE_CTRL_NAMES = ["efuse1", "efuse2", "efuse3", "efuse4"]
    
    var body: some View {
        VStack(spacing: 0) {
            // Toolbar
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    Text("Presets:")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundColor(.secondary)
                    
                    ForEach(PRESETS, id: \.name) { preset in
                        Button(preset.name) {
                            applyPreset(preset.states)
                        }
                        .font(.system(size: 12, weight: .semibold))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .background(Color.white.opacity(0.05))
                        .cornerRadius(8)
                    }
                    
                    Divider()
                        .frame(height: 16)
                        .background(Color.white.opacity(0.1))
                    
                    // Controls
                    let ioexpEnables = connectionManager.lastOverview?.ioexp.enables
                    let isOeActive = connectionManager.lastStatus?.liveStatus != nil // simple simulation/read
                    
                    ControlPill(title: "LShift OE", isActive: isOeActive) {
                        toggleOe()
                    }
                    
                    ControlPill(title: "V_ADJ1", isActive: ioexpEnables?.vadj1 ?? false) {
                        togglePsu(0)
                    }
                    
                    ControlPill(title: "V_ADJ2", isActive: ioexpEnables?.vadj2 ?? false) {
                        togglePsu(1)
                    }
                    
                    ForEach(0..<4) { i in
                        let efActive = false // simple simulation or check overview snapshot
                        ControlPill(title: "EF\(i+1)", isActive: efActive) {
                            toggleEfuse(i)
                        }
                    }
                }
                .padding(.horizontal)
                .padding(.vertical, 10)
            }
            .background(Color.black.opacity(0.3))
            
            // Legend
            HStack(spacing: 12) {
                LegendItem(color: C_GPIO, text: "GPIO (direct)")
                LegendItem(color: C_GPIO_R, text: "GPIO (2kΩ)")
                LegendItem(color: C_ADC, text: "ADC Channel")
                LegendItem(color: C_EXT, text: "External")
                LegendItem(color: Color.red, text: "Power")
            }
            .padding(.vertical, 8)
            
            if let status = opStatus {
                Text(status)
                    .font(.system(size: 11))
                    .foregroundColor(.red)
                    .padding(.bottom, 4)
            }
            
            // Interactive Canvas Schematic
            GeometryReader { geo in
                let muxStates = connectionManager.lastStatus?.muxStates ?? [0, 0, 0, 0]
                let isPsuOn = [
                    connectionManager.lastOverview?.ioexp.enables.vadj1 ?? false,
                    connectionManager.lastOverview?.ioexp.enables.vadj2 ?? false
                ]
                let isEfOn = [false, false, false, false]
                let isOeOn = true // simulated/real OE
                
                Canvas { context, size in
                    render(
                        c: context,
                        w: size.width,
                        h: size.height,
                        ms: muxStates,
                        ps: isPsuOn,
                        es: isEfOn,
                        oeOn: isOeOn
                    )
                }
                .gesture(
                    DragGesture(minimumDistance: 0)
                        .onEnded { value in
                            handleTap(at: value.location, in: geo.size)
                        }
                )
            }
        }
        .background(C_BG)
        .preferredColorScheme(.dark)
    }
    
    private func handleTap(at pt: CGPoint, in size: CGSize) {
        let psuH: CGFloat = 42
        let rt = psuH + 10
        let ra = size.height - rt - 4
        let rh = ra / 4
        let hitL = size.width * 0.12
        let hitR = size.width * 0.63
        
        if pt.y < rt || pt.x < hitL || pt.x > hitR { return }
        let ch = Int((pt.y - rt) / rh)
        if ch < 0 || ch >= 4 { return }
        
        let ry = rt + CGFloat(ch) * rh
        let lh = (rh - 12) / 8.5
        let g = lh * 0.4
        var syArr = [CGFloat](repeating: 0.0, count: 8)
        for s in 0..<8 {
            let gap = s >= 6 ? g * 2.0 : (s >= 4 ? g : 0.0)
            syArr[s] = ry + 6.0 + CGFloat(s) * lh + gap
        }
        for s in 0..<8 {
            syArr[s] = 2.0 * ry + rh - syArr[s]
        }
        
        var best = -1
        var bestD = CGFloat.infinity
        for s in 0..<8 {
            let d = abs(pt.y - syArr[s])
            if d < bestD {
                bestD = d
                best = s
            }
        }
        if best < 0 || bestD > max(8.0, lh * 1.1) { return }
        
        let dev = MUX_DEVICE_BY_LOGICAL[ch]
        let currentStates = connectionManager.lastStatus?.muxStates ?? [0, 0, 0, 0]
        let on = ((currentStates[dev] >> best) & 1) != 0
        
        toggleSwitch(device: dev, sw: best, closed: !on)
    }
    
    private func toggleSwitch(device: Int, sw: Int, closed: Bool) {
        Task {
            do {
                let ok = try await connectionManager.postAction(
                    path: "/api/mux/switch",
                    json: ["device": device, "switch": sw, "closed": closed]
                )
                if !ok {
                    opStatus = "Failed to toggle switch"
                } else {
                    opStatus = nil
                }
            } catch {
                opStatus = error.localizedDescription
            }
        }
    }
    
    private func applyPreset(_ states: [Int]) {
        Task {
            do {
                let ok = try await connectionManager.postAction(
                    path: "/api/mux/all",
                    json: ["states": states]
                )
                if !ok {
                    opStatus = "Failed to apply preset"
                } else {
                    opStatus = nil
                }
            } catch {
                opStatus = error.localizedDescription
            }
        }
    }
    
    private func toggleOe() {
        Task {
            let nextState = true // Simple toggle logic or read from active
            _ = try? await connectionManager.postAction(path: "/api/ioexp/control", json: ["control": "mux", "on": nextState])
        }
    }
    
    private func togglePsu(_ index: Int) {
        let control = index == 0 ? "vadj1" : "vadj2"
        let currentEn = index == 0 ?
            (connectionManager.lastOverview?.ioexp.enables.vadj1 ?? false) :
            (connectionManager.lastOverview?.ioexp.enables.vadj2 ?? false)
        Task {
            _ = try? await connectionManager.postAction(path: "/api/ioexp/control", json: ["control": control, "on": !currentEn])
        }
    }
    
    private func toggleEfuse(_ index: Int) {
        let control = EFUSE_CTRL_NAMES[index]
        Task {
            _ = try? await connectionManager.postAction(path: "/api/ioexp/control", json: ["control": control, "on": true])
        }
    }
    
    // MARK: - Canvas Rendering Port
    private func render(
        c: GraphicsContext,
        w: CGFloat,
        h: CGFloat,
        ms: [Int],
        ps: [Bool],
        es: [Bool],
        oeOn: Bool
    ) {
        // Draw background
        c.fill(Path(CGRect(x: 0, y: 0, width: w, height: h)), with: .color(C_BG))
        
        let psuH: CGFloat = 42
        let rt = psuH + 10
        let ra = h - rt - 4
        let rh = ra / 4
        
        let gpioX = w * 0.06
        let lsL = w * 0.085
        let lsR = w * 0.115
        let muxL = w * 0.18
        let muxR = w * 0.50
        
        // PSU Bars
        drawPsuBar(c: c, r: CGRect(x: 8, y: 4, width: w * 0.48 - 12, height: psuH), name: "V_ADJ1", feeds: "→ P1, P2", on: ps[0])
        drawPsuBar(c: c, r: CGRect(x: w * 0.5 + 4, y: 4, width: w * 0.48 - 12, height: psuH), name: "V_ADJ2", feeds: "→ P3, P4", on: ps[1])
        
        // Level Shifters U13 & U15
        for pair in 0..<2 {
            let y1 = rt + CGFloat(pair * 2) * rh
            let y2 = y1 + 2.0 * rh
            let lsName = pair == 0 ? "U13" : "U15"
            let lsPad: CGFloat = 4
            
            let rect = CGRect(x: lsL - 1, y: y1 + lsPad, width: lsR - lsL + 2, height: (y2 - y1) - lsPad * 2)
            c.fill(Path(roundedRect: rect, cornerRadius: 3), with: .color(oeOn ? Color(red: 0.05, green: 0.10, blue: 0.07) : Color(red: 0.04, green: 0.06, blue: 0.11)))
            c.stroke(Path(roundedRect: rect, cornerRadius: 3), with: .color(oeOn ? Color(red: 0.10, green: 0.25, blue: 0.19) : Color(red: 0.10, green: 0.16, blue: 0.25)), lineWidth: 1.0)
            
            // Text label
            c.draw(Text(lsName).font(.system(size: 7, weight: .bold, design: .monospaced)).foregroundColor(oeOn ? .green : .secondary), at: CGPoint(x: (lsL + lsR) / 2, y: y1 + lsPad + 4))
        }
        
        // Channels Rows
        for ch in 0..<4 {
            let ry = rt + CGFloat(ch) * rh
            let dev = MUX_DEVICE_BY_LOGICAL[ch]
            let st = ms.count > dev ? ms[dev] : 0
            let accent = ACCENTS[ch]
            
            if ch > 0 {
                var linePath = Path()
                linePath.move(to: CGPoint(x: 0, y: ry))
                linePath.addLine(to: CGPoint(x: w, y: ry))
                c.stroke(linePath, with: .color(Color(white: 0.1)), lineWidth: 0.5)
            }
            
            let lh = (rh - 12.0) / 8.5
            let g = lh * 0.4
            var sy = [CGFloat](repeating: 0.0, count: 8)
            for s in 0..<8 {
                let gap = s >= 6 ? g * 2.0 : (s >= 4 ? g : 0.0)
                sy[s] = ry + 6.0 + CGFloat(s) * lh + gap
            }
            for s in 0..<8 {
                sy[s] = 2.0 * ry + rh - sy[s]
            }
            
            // MUX chip background
            let chipRect = CGRect(x: muxL, y: ry + 2, width: muxR - muxL, height: rh - 4)
            c.fill(Path(chipRect), with: .color(C_CHIP))
            c.stroke(Path(chipRect), with: .color(C_CHIP_BD), lineWidth: 1.0)
            
            // Switch labels inside MUX chip
            for s in 0..<8 {
                let closed = ((st >> s) & 1) != 0
                let syVal = sy[s]
                
                var swPath = Path()
                swPath.move(to: CGPoint(x: muxL + 10, y: syVal))
                swPath.addLine(to: CGPoint(x: muxR - 10, y: syVal))
                c.stroke(swPath, with: .color(closed ? accent : Color(white: 0.15)), lineWidth: closed ? 2.5 : 1.0)
                
                let swLabel = Text("S\(s+1)")
                    .font(.system(size: 7, weight: .bold))
                    .foregroundColor(closed ? .white : .secondary)
                c.draw(swLabel, at: CGPoint(x: muxL + 25, y: syVal))
            }
            
            // Mux chip label
            let chipLabel = Text("MUX \(dev+1) · \(MUX_REF[ch])")
                .font(.system(size: 8, weight: .semibold, design: .monospaced))
                .foregroundColor(accent)
            c.draw(chipLabel, at: CGPoint(x: (muxL + muxR) / 2, y: ry + rh - 10))
            
            // Draw schematic paths
            // IO ports
            let pairs = [[0, 1, 0], [4, 5, 1], [6, 7, 2]]
            for pair in pairs {
                let yd = sy[pair[0]]
                let yr = sy[pair[1]]
                let ym = (yd + yr) / 2
                
                let lblText = Text(GPIO_PAIR_LABELS[ch][pair[2]])
                    .font(.system(size: 8, weight: .medium, design: .monospaced))
                    .foregroundColor(.white)
                c.draw(lblText, at: CGPoint(x: gpioX, y: ym))
                
                var connectPath = Path()
                connectPath.move(to: CGPoint(x: gpioX + 10, y: ym))
                connectPath.addLine(to: CGPoint(x: lsL, y: ym))
                c.stroke(connectPath, with: .color(.secondary), lineWidth: 1.0)
            }
        }
    }
    
    private func drawPsuBar(c: GraphicsContext, r: CGRect, name: String, feeds: String, on: Bool) {
        c.fill(Path(roundedRect: r, cornerRadius: 5), with: .color(on ? Color(red: 0.15, green: 0.05, blue: 0.05) : Color(red: 0.04, green: 0.06, blue: 0.12)))
        c.stroke(Path(roundedRect: r, cornerRadius: 5), with: .color(on ? Color(red: 0.45, green: 0.12, blue: 0.12) : Color(red: 0.12, green: 0.18, blue: 0.28)), lineWidth: 1.0)
        
        let titleText = Text(name)
            .font(.system(size: 11, weight: .bold, design: .rounded))
            .foregroundColor(on ? .red : .secondary)
        c.draw(titleText, at: CGPoint(x: r.minX + 12, y: r.minY + 12), anchor: .leading)
        
        let descText = Text(feeds)
            .font(.system(size: 8, design: .monospaced))
            .foregroundColor(.secondary)
        c.draw(descText, at: CGPoint(x: r.minX + 12, y: r.minY + 26), anchor: .leading)
        
        // indicator light
        let indicatorColor = on ? Color.green : Color(white: 0.1)
        c.fill(Path(ellipseIn: CGRect(x: r.maxX - 20, y: r.midY - 4, width: 8, height: 8)), with: .color(indicatorColor))
    }
}

struct ControlPill: View {
    let title: String
    let isActive: Bool
    var action: () -> Void
    
    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 12, weight: .bold))
                .foregroundColor(isActive ? .black : .white)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(isActive ? Color.cyan : Color.white.opacity(0.1))
                .cornerRadius(12)
        }
    }
}

struct LegendItem: View {
    let color: Color
    let text: String
    
    var body: some View {
        HStack(spacing: 4) {
            Circle()
                .fill(color)
                .frame(width: 6, height: 6)
            Text(text)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(.secondary)
        }
    }
}
