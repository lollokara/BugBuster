import SwiftUI

struct SignalPathTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var opStatus: String? = nil
    
    // Constants
    let PRESETS: [(name: String, states: [Int])] = [
        ("All Open", [0x00, 0x00, 0x00, 0x00]),
        ("GPIO Direct", [0x51, 0x51, 0x51, 0x51]),
        ("ADC Read", [0x04, 0x04, 0x04, 0x04]),
        ("External", [0x08, 0x08, 0x08, 0x08])
    ]
    
    let ACCENTS = [
        Color(red: 0.23, green: 0.51, blue: 0.96), // Blue
        Color(red: 0.06, green: 0.73, blue: 0.51), // Emerald
        Color(red: 0.96, green: 0.62, blue: 0.04), // Amber
        Color(red: 0.66, green: 0.33, blue: 0.97)  // Purple
    ]
    
    let MUX_REF = ["U10", "U11", "U17", "U16"]
    let MUX_DEVICE_BY_LOGICAL = [0, 1, 2, 3]
    
    let GPIO_PAIR_LABELS = [
        ["IO1", "IO2", "IO3"],
        ["IO4", "IO5", "IO6"],
        ["IO7", "IO8", "IO9"],
        ["IO10", "IO11", "IO12"]
    ]
    
    let EFUSE_CTRL_NAMES = ["efuse1", "efuse2", "efuse3", "efuse4"]

    var body: some View {
        ZStack(alignment: .top) {
            Color(red: 0.03, green: 0.05, blue: 0.10)
                .ignoresSafeArea()

            VStack(spacing: 0) {
                if let status = opStatus {
                    Text(status)
                        .font(.system(size: 11, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.vertical, 8)
                        .frame(maxWidth: .infinity)
                        .glassEffect(.regular.tint(.red), in: Rectangle())
                        .transition(.move(edge: .top))
                        .onAppear {
                            DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                                withAnimation { opStatus = nil }
                            }
                        }
                }

                ScrollView {
                    VStack(spacing: 0) {
                        GlassEffectContainer(spacing: 16) {
                            VStack(spacing: 16) {
                                let workerEnabled = connectionManager.lastSelftest?.workerEnabled ?? false

                                ForEach(0..<4) { ch in
                                    let isChannelCGreyed = ch == 2 && workerEnabled
                                    let accent = isChannelCGreyed ? Color.gray : ACCENTS[ch]

                                    let muxDev = MUX_DEVICE_BY_LOGICAL[ch]
                                    let muxState = connectionManager.lastStatus?.muxStates.count ?? 0 > muxDev
                                        ? connectionManager.lastStatus!.muxStates[muxDev] : 0

                                let isPsuOn = ch < 2
                                    ? (connectionManager.lastOverview?.ioexp.enables.vadj1 ?? false)
                                    : (connectionManager.lastOverview?.ioexp.enables.vadj2 ?? false)

                                let efuses = connectionManager.lastOverview?.ioexp.efuses
                                let isEfOn   = efuses?.first(where: { $0.id == ch + 1 })?.enabled ?? false
                                let isEfFault = efuses?.first(where: { $0.id == ch + 1 })?.fault ?? false

                                let isOeActive = connectionManager.lastOverview?.ioexp.enables.mux ?? false
                                let vAdjLabel  = ch < 2 ? "V_ADJ1" : "V_ADJ2"
                                let vAdjIndex  = ch < 2 ? 0 : 1

                                    BlockTile(
                                        blockIndex: ch,
                                        muxDevice: muxDev,
                                        muxRef: MUX_REF[ch],
                                        muxState: muxState,
                                        ioLabels: GPIO_PAIR_LABELS[ch],
                                        accentColor: accent,
                                        isPsuActive: isPsuOn,
                                        isEfuseActive: isEfOn,
                                        isEfuseFault: isEfFault,
                                        isOeActive: isOeActive,
                                        vAdjLabel: vAdjLabel,
                                        onApplyMuxStates: { states in applyPreset(states) },
                                        onToggleOe:     { toggleOe() },
                                        onToggleVAdj:   { togglePsu(vAdjIndex) },
                                        onToggleEfuse:  { toggleEfuse(ch) }
                                    )
                                    .opacity(isChannelCGreyed ? 0.4 : 1.0)
                                    .grayscale(isChannelCGreyed ? 1.0 : 0)
                                    .disabled(isChannelCGreyed)
                                }
                            }
                        }
                    }
                    .padding()

                    GPIOControlCard()
                        .padding(.horizontal)
                        .padding(.bottom)
                }
            }
        }
        .preferredColorScheme(.dark)
    }

    // MARK: - Actions (with optimistic local state updates)

    private func applyPreset(_ states: [Int]) {
        Task {
            do {
                let ok = try await connectionManager.postAction(
                    path: "/api/mux/all",
                    json: ["states": states]
                )
                if !ok { opStatus = "Failed to apply MUX state" }
            } catch {
                opStatus = error.localizedDescription
            }
        }
    }

    private func toggleOe() {
        let current = connectionManager.lastOverview?.ioexp.enables.mux ?? false
        let next    = !current
        // Optimistic: update UI immediately
        connectionManager.applyOptimisticEnable(key: "mux", value: next)
        Task {
            _ = try? await connectionManager.postAction(
                path: "/api/ioexp/control",
                json: ["control": "mux", "on": next]
            )
            // Refresh once to sync any server-side side-effects
            connectionManager.fetchOverviewQuick()
        }
    }

    private func togglePsu(_ index: Int) {
        let control   = index == 0 ? "vadj1" : "vadj2"
        let currentEn = index == 0
            ? (connectionManager.lastOverview?.ioexp.enables.vadj1 ?? false)
            : (connectionManager.lastOverview?.ioexp.enables.vadj2 ?? false)
        let next = !currentEn
        connectionManager.applyOptimisticEnable(key: control, value: next)
        Task {
            _ = try? await connectionManager.postAction(
                path: "/api/ioexp/control",
                json: ["control": control, "on": next]
            )
            connectionManager.fetchOverviewQuick()
        }
    }

    private func toggleEfuse(_ index: Int) {
        let control   = EFUSE_CTRL_NAMES[index]
        let efuseId   = index + 1
        let currentEn = connectionManager.lastOverview?.ioexp.efuses?
            .first(where: { $0.id == efuseId })?.enabled ?? false
        let next = !currentEn
        connectionManager.applyOptimisticEfuse(id: efuseId, enabled: next)
        Task {
            _ = try? await connectionManager.postAction(
                path: "/api/ioexp/control",
                json: ["control": control, "on": next]
            )
            connectionManager.fetchOverviewQuick()
        }
    }
}

// MARK: - BlockTile

struct BlockTile: View {
    let blockIndex: Int
    let muxDevice: Int
    let muxRef: String
    let muxState: Int
    let ioLabels: [String]
    let accentColor: Color
    let isPsuActive: Bool
    let isEfuseActive: Bool
    let isEfuseFault: Bool
    let isOeActive: Bool
    let vAdjLabel: String
    let onApplyMuxStates: ([Int]) -> Void
    let onToggleOe: () -> Void
    let onToggleVAdj: () -> Void
    let onToggleEfuse: () -> Void

    @EnvironmentObject var connectionManager: ConnectionManager

    var body: some View {
        let isGlowActive = isPsuActive && isEfuseActive

        VStack(alignment: .leading, spacing: 12) {
            // ── Header ────────────────────────────────────────────────────
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("BLOCK \(blockIndex + 1)")
                        .font(.system(size: 12, weight: .black))
                        .foregroundColor(accentColor)
                    Text("MUX \(muxDevice + 1) · \(muxRef)")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(.secondary)
                }
                Spacer()
                HStack(spacing: 6) {
                    if (muxState & 0x04) != 0 {
                        BadgeView(text: "ADC", color: .blue)
                    }
                    if isOeActive && (muxState & 0xF3) != 0 {
                        BadgeView(text: "HAT LA", color: .green)
                    }
                    Circle()
                        .fill(isGlowActive ? Color.green : Color.red.opacity(0.3))
                        .frame(width: 8, height: 8)
                        .shadow(color: isGlowActive ? .green : .clear, radius: 4)
                }
            }

            Divider().background(Color.white.opacity(0.1))

            // ── IO Rows ───────────────────────────────────────────────────
            VStack(spacing: 12) {
                IORow(label: ioLabels[0],
                      mode: currentMode(for: 0),
                      options: [.highZ, .direct, .resistor, .adc, .external],
                      onSelect: { selectMode($0, for: 0) })
                IORow(label: ioLabels[1],
                      mode: currentMode(for: 1),
                      options: [.highZ, .direct, .resistor],
                      onSelect: { selectMode($0, for: 1) })
                IORow(label: ioLabels[2],
                      mode: currentMode(for: 2),
                      options: [.highZ, .direct, .resistor],
                      onSelect: { selectMode($0, for: 2) })
            }

            // ── Power & eFuse Controls ────────────────────────────────────
            Divider().background(Color.white.opacity(0.06))

            HStack(spacing: 10) {
                ControlPill(title: "LS OE",    isActive: isOeActive,  action: onToggleOe)
                ControlPill(title: vAdjLabel,  isActive: isPsuActive, action: onToggleVAdj)
                Spacer()
                EFuseInlineRow(
                    index: blockIndex,
                    enabled: isEfuseActive,
                    fault: isEfuseFault,
                    onToggle: onToggleEfuse
                )
            }
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .shadow(color: isGlowActive ? accentColor.opacity(0.4) : .clear,
                radius: isGlowActive ? 14 : 0)
        .animation(.easeInOut(duration: 0.3), value: isGlowActive)
    }

    private func currentMode(for ioIndex: Int) -> IOMode {
        if ioIndex == 0 {
            if (muxState & 0x01) != 0 { return .direct }
            if (muxState & 0x02) != 0 { return .resistor }
            if (muxState & 0x04) != 0 { return .adc }
            if (muxState & 0x08) != 0 { return .external }
        } else if ioIndex == 1 {
            if (muxState & 0x10) != 0 { return .direct }
            if (muxState & 0x20) != 0 { return .resistor }
        } else if ioIndex == 2 {
            if (muxState & 0x40) != 0 { return .direct }
            if (muxState & 0x80) != 0 { return .resistor }
        }
        return .highZ
    }

    private func selectMode(_ mode: IOMode, for ioIndex: Int) {
        guard var currentStates = connectionManager.lastStatus?.muxStates,
              currentStates.count > muxDevice else { return }
        var state = currentStates[muxDevice]
        if ioIndex == 0 {
            state &= ~0x0F
            switch mode {
            case .direct:   state |= 0x01
            case .resistor: state |= 0x02
            case .adc:      state |= 0x04
            case .external: state |= 0x08
            case .highZ:    break
            }
        } else if ioIndex == 1 {
            state &= ~0x30
            switch mode {
            case .direct:   state |= 0x10
            case .resistor: state |= 0x20
            default: break
            }
        } else if ioIndex == 2 {
            state &= ~0xC0
            switch mode {
            case .direct:   state |= 0x40
            case .resistor: state |= 0x80
            default: break
            }
        }
        currentStates[muxDevice] = state
        onApplyMuxStates(currentStates)
    }
}

// MARK: - EFuseInlineRow

struct EFuseInlineRow: View {
    let index: Int
    let enabled: Bool
    let fault: Bool
    let onToggle: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(fault ? Color.red : (enabled ? Color.orange : Color.gray.opacity(0.4)))
                .frame(width: 7, height: 7)
                .shadow(color: fault ? .red : (enabled ? .orange : .clear), radius: 5)

            Text("EF\(index + 1)")
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(.white.opacity(0.8))

            Text(fault ? "FAULT" : (enabled ? "ON" : "OFF"))
                .font(.system(size: 10, weight: .semibold))
                .foregroundColor(fault ? .red : (enabled ? .orange : .secondary))

            Button(action: onToggle) {
                Text(enabled ? "Disable" : "Enable")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundColor(enabled ? .red : .cyan)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .glassEffect(
                        enabled ? .regular.tint(.red) : .regular.tint(.cyan),
                        in: RoundedRectangle(cornerRadius: 7, style: .continuous)
                    )
            }
        }
    }
}

// MARK: - IOMode

enum IOMode: String, CaseIterable {
    case highZ    = "High-Z"
    case direct   = "Direct"
    case resistor = "2kΩ Resistor"
    case adc      = "ADC Channel"
    case external = "External"
}

// MARK: - IORow

struct IORow: View {
    let label: String
    let mode: IOMode
    let options: [IOMode]
    let onSelect: (IOMode) -> Void

    var body: some View {
        HStack {
            Text(label)
                .font(.system(size: 14, weight: .bold, design: .monospaced))
                .foregroundColor(.white)
            Spacer()
            Menu {
                ForEach(options, id: \.self) { option in
                    Button {
                        onSelect(option)
                    } label: {
                        HStack {
                            Text(option.rawValue)
                            if option == mode { Image(systemName: "checkmark") }
                        }
                    }
                }
            } label: {
                HStack(spacing: 6) {
                    Text(mode.rawValue)
                        .font(.system(size: 13, weight: .semibold))
                    Image(systemName: "chevron.down")
                        .font(.system(size: 10, weight: .bold))
                }
                .foregroundColor(modeColor)
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            }
        }
    }

    private var modeColor: Color {
        switch mode {
        case .highZ:    return .secondary
        case .direct:   return Color(red: 0.13, green: 0.77, blue: 0.37)
        case .resistor: return Color(red: 0.92, green: 0.70, blue: 0.03)
        case .adc:      return Color(red: 0.23, green: 0.51, blue: 0.96)
        case .external: return Color(red: 0.98, green: 0.45, blue: 0.09)
        }
    }
}

// MARK: - BadgeView

struct BadgeView: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .font(.system(size: 9, weight: .black))
            .foregroundColor(.black)
            .padding(.horizontal, 6)
            .padding(.vertical, 3)
            .glassEffect(.regular.tint(color), in: RoundedRectangle(cornerRadius: 4, style: .continuous))
    }
}

// MARK: - ControlPill

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
                .glassEffect(
                    isActive ? .regular.tint(.cyan) : .regular,
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                )
        }
    }
}

// MARK: - GPIO Control Section

struct GPIOControlCard: View {
    @EnvironmentObject var connectionManager: ConnectionManager

    let modeNames = ["Disabled", "Input", "Output", "In+PD", "Out+OD"]
    let columns = [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("GPIO Direct Control")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundColor(.blue)
                Spacer()
                Button(action: { connectionManager.fetchGpios() }) {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 14))
                }
                .padding(8)
                .glassEffect(.regular, in: Circle())
            }

            if connectionManager.lastGpios.isEmpty {
                Text("Loading GPIO states…")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            } else {
                LazyVGrid(columns: columns, spacing: 10) {
                    ForEach(connectionManager.lastGpios) { gpio in
                        GPIOPinTile(gpio: gpio)
                    }
                }
            }
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .onAppear { connectionManager.fetchGpios() }
    }
}

struct GPIOPinTile: View {
    let gpio: GPIOPin
    @EnvironmentObject var connectionManager: ConnectionManager

    let modeNames = ["Disabled", "Input", "Output", "In+PD", "Out+OD"]

    var isOutput: Bool { gpio.mode == 2 || gpio.mode == 4 }
    var isInput: Bool { gpio.mode == 1 || gpio.mode == 3 }

    var stateColor: Color {
        if gpio.mode == 0 { return .secondary }
        if isOutput { return gpio.output ? .green : .red.opacity(0.6) }
        if isInput { return gpio.input ? .cyan : .secondary }
        return .secondary
    }

    var body: some View {
        VStack(spacing: 6) {
            HStack {
                Text("IO \(gpio.id)")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(.white)
                Spacer()
                Circle()
                    .fill(stateColor)
                    .frame(width: 8, height: 8)
                    .shadow(color: stateColor.opacity(0.5), radius: 3)
            }

            Menu {
                ForEach(0..<5) { mode in
                    Button(modeNames[mode]) {
                        Task { _ = await connectionManager.configureGpio(pin: gpio.id, mode: mode) }
                    }
                }
            } label: {
                Text(gpio.mode < modeNames.count ? modeNames[gpio.mode] : "?")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundColor(.cyan)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 4)
                    .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
            }

            if isOutput {
                Button(action: {
                    Task { _ = await connectionManager.setGpioOutput(pin: gpio.id, value: !gpio.output) }
                }) {
                    Text(gpio.output ? "HIGH" : "LOW")
                        .font(.system(size: 10, weight: .black))
                        .foregroundColor(gpio.output ? .black : .white)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 4)
                        .glassEffect(
                            gpio.output ? .regular.tint(.green) : .regular,
                            in: RoundedRectangle(cornerRadius: 6, style: .continuous)
                        )
                }
            } else if isInput {
                Text(gpio.input ? "HIGH" : "LOW")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(gpio.input ? .cyan : .secondary)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 4)
            }
        }
        .padding(8)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }
}
