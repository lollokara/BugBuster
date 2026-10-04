import SwiftUI

/// Copy and colours shared by the pill, the header and the autorun panel.
enum ScriptStatusText {
    static func stateLabel(_ s: ScriptRunStatus) -> String {
        switch s.state {
        case .running: return "Running"
        case .stopping: return "Stopping…"
        case .error: return "Error"
        case .done:
            switch s.lastExit {
            case .stopped: return "Stopped"
            case .error: return "Error"
            default: return "Finished"
            }
        case .idle, .unknown: return "Idle"
        }
    }

    static func sourceLabel(_ source: ScriptSourceKind) -> String {
        switch source {
        case .manual: return "Manual"
        case .autorun: return "Autorun"
        case .repl: return "REPL"
        case .unknown: return "Other"
        }
    }

    static func stateColor(_ state: ScriptRunState) -> Color {
        switch state {
        case .running: return .green
        case .stopping: return .orange
        case .error: return .red
        case .done, .idle, .unknown: return .secondary
        }
    }

    static func elapsedLabel(_ seconds: TimeInterval?) -> String? {
        seconds.map(ScriptElapsed.format)
    }

    static func autorunState(_ a: ScriptAutorunStatus) -> String {
        if a.running { return "Running" }
        if a.ranThisBoot { return a.lastRunOk ? "Finished OK" : "Failed" }
        guard a.enabled else { return "Off" }
        return a.io12High ? "Runs at next boot" : "Suppressed: IO12 held low"
    }

    static func autorunRows(_ a: ScriptAutorunStatus) -> [ScriptInfoRow] {
        let script = !a.scriptName.isEmpty ? a.scriptName : (a.hasScript ? "autorun.py" : "None")
        return [ScriptInfoRow(label: "Script", value: script),
                ScriptInfoRow(label: "Ran this boot", value: a.ranThisBoot ? "Yes" : "No"),
                ScriptInfoRow(label: "State", value: autorunState(a)),
                ScriptInfoRow(label: "IO12 gate", value: a.io12High ? "High: autorun allowed" : "Low: autorun suppressed at boot")]
    }
}

struct ScriptInfoRow: Equatable {
    let label: String
    let value: String
}

/// Compact capsule shown on every tab, including Scripts, while a script is active.
struct ScriptStatusPill: View {
    let status: ScriptRunStatus
    let elapsed: TimeInterval?
    let lastLine: String?
    let onOpenLog: () -> Void
    let onStop: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            Button(action: onOpenLog) {
                HStack(spacing: 8) {
                    Circle()
                        .fill(ScriptStatusText.stateColor(status.state))
                        .frame(width: 8, height: 8)
                    VStack(alignment: .leading, spacing: 1) {
                        HStack(spacing: 6) {
                            Text(status.displayName)
                                .font(.system(size: 13, weight: .semibold, design: .monospaced))
                                .lineLimit(1)
                            if let label = ScriptStatusText.elapsedLabel(elapsed) {
                                Text(label)
                                    .font(.system(size: 11, weight: .medium).monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                        if let lastLine, !lastLine.isEmpty {
                            Text(lastLine)
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                    }
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Script \(status.displayName) \(ScriptStatusText.stateLabel(status)). Show log")

            Button(action: onStop) {
                Image(systemName: "stop.fill")
                    .font(.system(size: 12, weight: .bold))
                    .foregroundStyle(.red)
                    .padding(7)
            }
            .buttonStyle(.plain)
            .glassEffect(.regular.tint(.red), in: Circle())
            .disabled(status.state == .stopping)
            .accessibilityLabel("Stop \(status.displayName)")
        }
        .padding(.leading, 14)
        .padding(.trailing, 6)
        .padding(.vertical, 6)
        .frame(maxWidth: 420)
        .glassEffect(.regular, in: Capsule())
    }
}

/// Always-visible status strip at the top of the Scripts tab.
struct ScriptStatusHeader: View {
    let status: ScriptRunStatus?
    let elapsed: TimeInterval?
    let logVisible: Bool
    let onToggleLog: () -> Void
    let onStop: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(ScriptStatusText.stateColor(status?.state ?? .idle))
                .frame(width: 10, height: 10)
            VStack(alignment: .leading, spacing: 2) {
                Text(status.map { $0.isActive ? $0.displayName : "No script running" } ?? "Connecting…")
                    .font(.system(size: 14, weight: .semibold, design: .monospaced))
                    .lineLimit(1)
                if let status {
                    Text([ScriptStatusText.stateLabel(status),
                          status.isActive ? ScriptStatusText.sourceLabel(status.source) : nil,
                          status.isActive ? ScriptStatusText.elapsedLabel(elapsed) : nil]
                        .compactMap { $0 }.joined(separator: " · "))
                        .font(.system(size: 11).monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            Button(action: onToggleLog) {
                Label(logVisible ? "Hide log" : "Log", systemImage: "text.alignleft")
                    .font(.system(size: 13, weight: .semibold))
            }
            .buttonStyle(.bordered)
            if status?.isActive == true {
                Button(role: .destructive, action: onStop) {
                    Label("Stop", systemImage: "stop.fill")
                        .font(.system(size: 13, weight: .semibold))
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)
                .disabled(status?.state == .stopping)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

/// Autorun configuration and what happened at this boot (spec §4 "Autorun panel").
struct ScriptAutorunPanel: View {
    let autorun: ScriptAutorunStatus?
    let files: [String]
    let onEnable: (String) -> Void
    let onDisable: () -> Void
    let onRefresh: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Autorun on boot")
                    .font(.system(size: 14, weight: .semibold))
                Spacer()
                Button(action: onRefresh) { Image(systemName: "arrow.clockwise") }
                    .accessibilityLabel("Refresh autorun status")
                Menu {
                    ForEach(files, id: \.self) { name in
                        Button(name) { onEnable(name) }
                    }
                    if autorun?.enabled == true {
                        Divider()
                        Button("Disable autorun", role: .destructive, action: onDisable)
                    }
                } label: {
                    Text(autorun?.enabled == true ? "Change" : "Enable")
                        .font(.system(size: 13, weight: .semibold))
                }
                .disabled(files.isEmpty && autorun?.enabled != true)
            }
            if let autorun {
                ForEach(ScriptStatusText.autorunRows(autorun), id: \.label) { row in
                    HStack {
                        Text(row.label).foregroundStyle(.secondary)
                        Spacer()
                        Text(row.value).font(.system(size: 12, design: .monospaced))
                    }
                    .font(.system(size: 12))
                }
            } else {
                Text("Loading…").font(.system(size: 12)).foregroundStyle(.secondary)
            }
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }
}
