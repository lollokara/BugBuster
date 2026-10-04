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
                        Text("Stored scripts").font(.system(size: 22, weight: .bold))
                        if let s = model.storage {
                            Text(String(format: "Used %.1f KB of %.1f KB", s.usedBytes / 1024, s.totalBytes / 1024))
                                .font(.system(size: 11))
                                .foregroundStyle(.secondary)
                        }
                    }
                    Spacer()
                    Button(action: onNew) { Image(systemName: "plus.circle.fill").font(.system(size: 24)) }
                        .buttonStyle(.plain)
                        .foregroundStyle(.blue)
                        .accessibilityLabel("New script")
                    Button(action: onREPL) { Image(systemName: "terminal.fill").font(.system(size: 20)) }
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
                                .font(.system(size: 15, design: .monospaced))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        ScriptRunMenu(onRun: { onRun(name, $0) }) {
                            Image(systemName: "play.fill")
                                .foregroundStyle(.green)
                                .padding(8)
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

            // Clear the floating iPhone tab bar.
            Color.clear.frame(height: 90).listRowBackground(Color.clear)
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .refreshable { await model.loadFiles() }
    }
}
