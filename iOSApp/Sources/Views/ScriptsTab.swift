import SwiftUI
import UIKit

/// Scripts tab (spec §4): status header, stored scripts with Run / Run in background,
/// the Runestone editor (docs, lint on save), the REPL (read-only while a file
/// script runs) and the autorun panel. Run state and logs live in ScriptRunManager,
/// so they survive leaving this tab.
struct ScriptsTab: View {
    @EnvironmentObject var scripts: ScriptRunManager

    var body: some View {
        ScriptsTabContent(manager: scripts)
    }
}

private struct ScriptsTabContent: View {
    @ObservedObject var manager: ScriptRunManager
    @StateObject private var model: ScriptsTabModel
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var showingREPL = false
    @State private var showingCreate = false
    @State private var newFileName = ""
    @State private var keyboardHeight: CGFloat = 0
    @State private var isWide = false

    init(manager: ScriptRunManager) {
        self.manager = manager
        _model = StateObject(wrappedValue: ScriptsTabModel(manager: manager))
    }

    var body: some View {
        VStack(spacing: 10) {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                ScriptStatusHeader(
                    status: manager.status,
                    elapsed: manager.status.flatMap { ScriptElapsed.seconds($0, now: context.date, anchor: manager.uptimeAnchor) },
                    logVisible: manager.consoleVisible,
                    onToggleLog: { manager.consoleVisible.toggle() },
                    onStop: { Task { await manager.stop() } })
            }
            .padding(.horizontal)
            .padding(.top, 12)

            // Side by side only when the pane really has room: an iPad in portrait with the sidebar
            // and the log column open leaves ~480 pt, so it falls back to browser-or-detail.
            // Measure width in background to avoid trapping ScrollViews in GeometryReader (preserves safeAreaInsets).
            Group {
                if sizeClass == .regular && isWide {
                    HStack(spacing: 0) {
                        browser.frame(width: 340)
                        Divider().opacity(0.3)
                        detail.frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                } else if model.openFile != nil || showingREPL {
                    detail
                } else {
                    browser
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(
                GeometryReader { geo in
                    Color.clear.preference(key: ScriptsTabWidthPreferenceKey.self, value: geo.size.width)
                }
            )
            .onPreferenceChange(ScriptsTabWidthPreferenceKey.self) { width in
                isWide = width >= 760
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // The shells ignore the keyboard safe area; clear it by hand (minus ~66 pt of tab bar chrome).
        .padding(.bottom, keyboardHeight > 0 ? max(0, keyboardHeight - 66) : 0)
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { note in
            if let frame = note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect {
                withAnimation(.easeOut(duration: 0.22)) { keyboardHeight = frame.height }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in
            withAnimation(.easeOut(duration: 0.22)) { keyboardHeight = 0 }
        }
        .onAppear { manager.scriptsTabVisible = true }
        .onDisappear { manager.scriptsTabVisible = false }
        .task(id: manager.isConnected) {
            guard manager.isConnected else { return }
            await model.loadFiles()
            await manager.refreshAutorun()
            #if DEBUG
            if model.openFile == nil, let name = ProcessInfo.processInfo.environment["BB_SCRIPTS_OPEN"] {
                await model.open(name)
            }
            #endif
        }
        .alert("New script", isPresented: $showingCreate) {
            TextField("script_name.py", text: $newFileName)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) { newFileName = "" }
            Button("Create") {
                let raw = newFileName
                newFileName = ""
                showingREPL = false
                Task { _ = await model.create(rawName: raw) }
            }
        } message: {
            Text("1–32 characters: letters, digits, _ . - (\".py\" is added)")
        }
        .alert(manager.replacePrompt?.message ?? "",
               isPresented: Binding(get: { manager.replacePrompt != nil }, set: { _ in }),
               presenting: manager.replacePrompt) { _ in
            Button("Stop and run", role: .destructive) { Task { await manager.confirmReplace() } }
            Button("Cancel", role: .cancel) { manager.cancelReplace() }
        } message: { _ in
            Text("Only one script runs at a time. The running script is asked to stop (3 s), then the VM resets.")
        }
        .alert("Scripts", isPresented: Binding(get: { model.errorMessage != nil }, set: { if !$0 { model.errorMessage = nil } })) {
            Button("OK", role: .cancel) { model.errorMessage = nil }
        } message: {
            Text(model.errorMessage ?? "")
        }
    }

    private var browser: some View {
        ScriptsBrowserView(
            model: model,
            autorun: manager.autorun,
            onOpen: { name in
                showingREPL = false
                Task { await model.open(name) }
            },
            onRun: { name, background in Task { await model.run(name, background: background) } },
            onNew: { showingCreate = true },
            onREPL: {
                model.close()
                showingREPL = true
            },
            onEnableAutorun: { name in Task { await manager.setAutorun(enabled: true, name: name) } },
            onDisableAutorun: { Task { await manager.setAutorun(enabled: false, name: nil) } },
            onRefreshAutorun: { Task { await manager.refreshAutorun() } })
    }

    @ViewBuilder
    private var detail: some View {
        if showingREPL {
            ScriptREPLView(status: manager.status,
                           onClose: { showingREPL = false },
                           onStop: { Task { await manager.stop() } })
        } else if let name = model.openFile {
            ScriptEditorScreen(name: name, model: model,
                               onBack: { model.close() },
                               onRun: { background in Task { await model.run(name, background: background) } })
        } else {
            ContentUnavailableView("Select a script", systemImage: "doc.text",
                                   description: Text("Open a stored script to edit it, or start the REPL."))
        }
    }
}

private struct ScriptsTabWidthPreferenceKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = nextValue()
    }
}

