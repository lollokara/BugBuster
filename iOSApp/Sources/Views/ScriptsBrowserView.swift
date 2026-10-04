import SwiftUI

/// Tap runs in the foreground (log opens); the menu also offers Run in background.
struct ScriptRunMenu<Content: View>: View {
    let onRun: (_ background: Bool) -> Void
    @ViewBuilder let label: () -> Content

    var body: some View {
        Menu {
            Button { onRun(false) } label: { Label("Run", systemImage: "play.fill") }
            Button { onRun(true) } label: { Label("Run in background", systemImage: "play.circle") }
        } label: {
            label()
        } primaryAction: {
            onRun(false)
        }
    }
}

struct ScriptsBrowserView: View {
    @ObservedObject var model: ScriptsTabModel
    let autorun: ScriptAutorunStatus?
    let onOpen: (String) -> Void
    let onRun: (_ name: String, _ background: Bool) -> Void
    let onNew: () -> Void
    let onREPL: () -> Void
    let onEnableAutorun: (String) -> Void
    let onDisableAutorun: () -> Void
    let onRefreshAutorun: () -> Void

    var body: some View {
        List {
            Section {
                HStack(spacing: 14) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Stored scripts").font(.title2.weight(.bold))
                        if let s = model.storage {
                            Text(String(format: "Used %.1f KB of %.1f KB", s.usedBytes / 1024, s.totalBytes / 1024))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                    Spacer()
                    Button(action: onNew) {
                        Image(systemName: "plus.circle.fill")
                            .font(.title2)
                            .frame(minWidth: ScriptViewMetrics.minTouchTarget, minHeight: ScriptViewMetrics.minTouchTarget)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(.blue)
                    .accessibilityLabel("New script")

                    Button(action: onREPL) {
                        Image(systemName: "terminal.fill")
                            .font(.title3)
                            .frame(minWidth: ScriptViewMetrics.minTouchTarget, minHeight: ScriptViewMetrics.minTouchTarget)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(.cyan)
                    .accessibilityLabel("Open REPL")
                }
            }
            .listRowBackground(Color.clear)

            Section {
                ScriptAutorunPanel(autorun: autorun, files: model.files, onEnable: onEnableAutorun,
                                   onDisable: onDisableAutorun, onRefresh: onRefreshAutorun)
            }
            .listRowBackground(Color.clear)

            Section("Scripts") {
                if model.isLoading && model.files.isEmpty {
                    ProgressView().tint(.cyan).frame(maxWidth: .infinity)
                } else if model.files.isEmpty {
                    Text("No stored scripts").foregroundStyle(.secondary)
                }
                ForEach(model.files, id: \.self) { name in
                    HStack {
                        Button { onOpen(name) } label: {
                            Label(name, systemImage: "doc.text")
                                .font(.body.monospaced())
                                .frame(maxWidth: .infinity, minHeight: ScriptViewMetrics.minTouchTarget, alignment: .leading)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        ScriptRunMenu(onRun: { onRun(name, $0) }) {
                            Image(systemName: "play.fill")
                                .foregroundStyle(.green)
                                .frame(width: ScriptViewMetrics.minTouchTarget, height: ScriptViewMetrics.minTouchTarget)
                                .contentShape(Rectangle())
                        }
                        .accessibilityLabel("Run \(name)")
                    }
                    .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                        Button(role: .destructive) {
                            Task { await model.delete(name) }
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                    }
                }
            }
            .listRowBackground(Color.clear)
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .refreshable { await model.loadFiles() }
    }
}

