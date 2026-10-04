import SwiftUI

/// Pill visibility: shown on every tab, Scripts included, while a script is active
/// (user decision 4; the Scripts tab keeps its full header as well).
enum ScriptPillRule {
    /// `onScriptsTab` is kept for call-site stability; it no longer hides the pill.
    static func isVisible(status: ScriptRunStatus?, onScriptsTab: Bool) -> Bool {
        status?.isActive ?? false
    }
}

enum ScriptLogColumnLayout {
    static let width: CGFloat = 380
}

/// The pill bound to the shared manager. Elapsed time and the last log line refresh once a second.
struct ScriptPillHost: View {
    @EnvironmentObject var scripts: ScriptRunManager
    let onScriptsTab: Bool

    var body: some View {
        if ScriptPillRule.isVisible(status: scripts.status, onScriptsTab: onScriptsTab), let status = scripts.status {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                ScriptStatusPill(
                    status: status,
                    elapsed: ScriptElapsed.seconds(status, now: context.date, anchor: scripts.uptimeAnchor),
                    lastLine: scripts.log.lastLine?.text,
                    onOpenLog: { withAnimation(.snappy) { scripts.consoleVisible = true } },
                    onStop: { Task { await scripts.stop() } })
            }
            .transition(.move(edge: .bottom).combined(with: .opacity))
        }
    }
}

/// The console bound to the shared manager: iPad trailing column, iPhone sheet.
struct ScriptLogPanel: View {
    @EnvironmentObject var scripts: ScriptRunManager

    var body: some View {
        ScriptLogConsoleView(
            store: scripts.log,
            subtitle: scripts.status.map { s in
                s.isActive ? "\(s.displayName) · \(ScriptStatusText.stateLabel(s))" : ScriptStatusText.stateLabel(s)
            },
            onClose: { withAnimation(.snappy) { scripts.consoleVisible = false } })
    }
}

struct ScriptLogToggleButton: View {
    @EnvironmentObject var scripts: ScriptRunManager

    var body: some View {
        Button {
            withAnimation(.snappy) { scripts.consoleVisible.toggle() }
        } label: {
            Image(systemName: scripts.consoleVisible ? "sidebar.trailing" : "text.alignleft")
        }
        .accessibilityLabel(scripts.consoleVisible ? "Hide script log" : "Show script log")
    }
}
