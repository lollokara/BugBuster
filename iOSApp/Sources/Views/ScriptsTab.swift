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
    @State private var keyboardOverlap: CGFloat = 0
    @State private var keyboardTop: CGFloat?
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
        // The shells ignore the keyboard safe area, so lift the content by hand until its bottom
        // edge sits exactly on the keyboard's top (the key bar is part of that frame). Closed
        // loop on the measured bottom, so it holds whatever else the system adds (safe areas,
        // tab-bar chrome, iPad sidebar, orientation) and settles without a dead gap.
        .padding(.bottom, keyboardOverlap)
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillChangeFrameNotification)) { note in
            guard let frame = note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect else { return }
            let hidden = frame.minY >= UIScreen.main.bounds.height - 0.5
            // No animation: measuring a half-animated layout would feed wrong numbers back in.
            keyboardTop = hidden ? nil : ScriptsKeyboard.topEdge(of: frame)
            if hidden {
                keyboardOverlap = 0
            } else {
                // Start unlifted; the editor measures itself and the lift grows to fit (never past it).
                settleAboveKeyboard(pass: 0)
            }
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

    /// Trim the lift so the editor's bottom edge ends flush above the keyboard / key bar. The
    /// editor view is measured live after the keyboard animation (no animation on the padding, so
    /// a half-animated layout is never measured). The editor moves a different distance per point
    /// of lift depending on the shell (about two on iPhone), so the step size is learned from the
    /// previous measurement (secant method) and the loop stops after a few corrections.
    private func settleAboveKeyboard(pass: Int, previous: (overlap: CGFloat, bottom: CGFloat)? = nil) {
        DispatchQueue.main.asyncAfter(deadline: .now() + (pass == 0 ? 0.3 : 0.15)) {
            guard let top = keyboardTop, let textView = model.editor.textView, textView.window != nil else { return }
            let bottom = textView.convert(textView.bounds, to: nil).maxY
            let delta = ScriptsKeyboard.editorGap(editorBottom: bottom, keyboardTop: top)
            guard abs(delta) > 1.5, pass < 7 else { return }
            let slope = ScriptsKeyboard.slope(previous: previous, overlap: keyboardOverlap, bottom: bottom)
            let before = keyboardOverlap
            keyboardOverlap = max(0, before - delta / slope)
            settleAboveKeyboard(pass: pass + 1, previous: (before, bottom))
        }
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


enum ScriptsKeyboard {
    /// Space left between the editor text view and the keyboard top beyond the chrome around it
    /// (4 pt glass inset + 8 pt screen padding); negative when it overlaps.
    static let editorChrome: CGFloat = 12

    /// How many points the editor bottom moves per point of lift: measured between two samples,
    /// else 2 (what the iPhone shells do). Clamped so a noisy sample cannot blow the step up.
    static func slope(previous: (overlap: CGFloat, bottom: CGFloat)?, overlap: CGFloat, bottom: CGFloat) -> CGFloat {
        guard let previous, abs(overlap - previous.overlap) > 1 else { return 2 }
        return min(4, max(0.5, abs((bottom - previous.bottom) / (overlap - previous.overlap))))
    }

    static func editorGap(editorBottom: CGFloat, keyboardTop: CGFloat) -> CGFloat {
        keyboardTop - (editorBottom + editorChrome)
    }

    /// Top edge of a keyboard frame (screen coordinates) in the key window's coordinates.
    @MainActor static func topEdge(of screenFrame: CGRect) -> CGFloat {
        let window = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows).first { $0.isKeyWindow }
        guard let window else { return screenFrame.minY }
        return window.convert(screenFrame, from: window.screen.coordinateSpace).minY
    }
}
