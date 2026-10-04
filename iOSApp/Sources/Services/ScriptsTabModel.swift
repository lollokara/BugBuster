import Combine
import Foundation

/// Scripts tab logic: file list, open, save with lint-on-save (spec §4), create,
/// delete, run. Run state, logs and the replace prompt belong to ScriptRunManager.
@MainActor
final class ScriptsTabModel: ObservableObject {
    enum LintBanner: Equatable {
        case ok
        case failed(String)
        /// Saved, but no syntax check (old BLE firmware, interpreter busy, or lint itself failed).
        case unavailable(String)
    }

    @Published private(set) var files: [String] = []
    @Published private(set) var storage: ScriptStorageInfo?
    @Published private(set) var isLoading = false
    @Published private(set) var openFile: String?
    @Published private(set) var lint: LintBanner?
    @Published private(set) var uploadProgress: Double?
    @Published var errorMessage: String?

    /// Firmware lint reply while a script holds the interpreter.
    static let lintBusyMessage = "Interpreter is busy running a script"

    let editor: ScriptEditorModel
    private let manager: ScriptRunManager

    init(manager: ScriptRunManager, editor: ScriptEditorModel? = nil) {
        self.manager = manager
        self.editor = editor ?? ScriptEditorModel()
    }

    func loadFiles() async {
        guard let client = manager.client else { return }
        isLoading = true
        defer { isLoading = false }
        do {
            files = try await client.files().sorted { $0.lowercased() < $1.lowercased() }
            storage = try? await client.storage()
            errorMessage = nil
        } catch {
            errorMessage = "Failed loading scripts: \(error.localizedDescription)"
        }
    }

    func create(rawName: String) async -> Bool {
        guard let name = ScriptName.normalized(rawName) else {
            errorMessage = ScriptsClientError.invalidName(rawName.trimmingCharacters(in: .whitespaces)).localizedDescription
            return false
        }
        guard !files.contains(name) else {
            errorMessage = "'\(name)' already exists"
            return false
        }
        guard let client = manager.client else { return false }
        let template = "# \(name)\n# Write your MicroPython code here\n"
        do {
            try await client.writeFile(name, text: template)
            await loadFiles()
            editor.load(template)
            openFile = name
            lint = nil
            return true
        } catch {
            errorMessage = "Create failed: \(error.localizedDescription)"
            return false
        }
    }

    func open(_ name: String) async {
        guard let client = manager.client else { return }
        do {
            let text = try await client.readFile(name)
            editor.load(text)
            openFile = name
            lint = nil
            errorMessage = nil
        } catch {
            errorMessage = "Load failed: \(error.localizedDescription)"
        }
    }

    func close() {
        openFile = nil
        lint = nil
        editor.dismissCompletion()
    }

    /// Save, then lint (spec §4 "lint on save"). A lint failure never blocks the save.
    @discardableResult
    func save() async -> Bool {
        guard let name = openFile, let client = manager.client else { return false }
        let text = ScriptTextSanitizer.sanitize(editor.text)
        uploadProgress = 0
        defer { uploadProgress = nil }
        do {
            try await client.writeFile(name, text: text) { progress in
                Task { @MainActor [weak self] in
                    if self?.uploadProgress != nil { self?.uploadProgress = progress }
                }
            }
            if editor.text == text { editor.markSaved() }      // typing during the upload stays dirty
            errorMessage = nil
        } catch {
            errorMessage = "Save failed: \(error.localizedDescription)"
            return false
        }
        do {
            // Over BLE the stored file is linted on the device; the source never crosses the tunnel.
            let result = try await client.lint(text, name: name)
            if result.ok {
                lint = .ok
            } else if result.message == Self.lintBusyMessage {
                lint = .unavailable("Saved. Syntax check skipped while a script runs.")
            } else {
                lint = .failed(result.message ?? "Syntax error")
            }
        } catch ScriptsClientError.needsWiFi {
            lint = .unavailable("Saved. Syntax check needs Wi-Fi.")
        } catch {
            lint = .unavailable("Saved. Syntax check failed: \(error.localizedDescription)")
        }
        return true
    }

    func delete(_ name: String) async {
        guard let client = manager.client else { return }
        do {
            try await client.deleteFile(name)
            if openFile == name { close() }
            await loadFiles()
        } catch {
            errorMessage = "Delete failed: \(error.localizedDescription)"
        }
    }

    /// The device runs the stored file, so an unsaved open file is saved first.
    func run(_ name: String, background: Bool) async {
        if name == openFile && editor.isDirty {
            guard await save() else { return }
        }
        await manager.run(name, background: background)
    }
}

/// Mini-REPL over BLE: each submission goes through `eval`; the echoed input and the
/// `mpy` log lines that arrive after it form the transcript (output comes via the log).
@MainActor
final class ScriptBLERepl: ObservableObject {
    @Published private(set) var entries: [(echo: String, afterLineId: Int)] = []
    @Published private(set) var error: String?
    @Published private(set) var isSending = false

    private let manager: ScriptRunManager
    private var logObserver: AnyCancellable?

    init(manager: ScriptRunManager) {
        self.manager = manager
        // Re-render when log lines arrive.
        logObserver = manager.log.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
    }

    func submit(_ src: String) async {
        guard let client = manager.client, !src.isEmpty else { return }
        isSending = true
        defer { isSending = false }
        let anchor = manager.log.lines.last?.id ?? 0
        entries.append((src, anchor))
        do {
            _ = try await client.eval(src, persist: true)
            error = nil
        } catch ScriptsClientError.tooLarge {
            error = "Too long for one Bluetooth request (512 bytes). Shorten it, or use Wi-Fi."
        } catch {
            self.error = error.localizedDescription
        }
    }

    func transcript() -> String {
        var out: [String] = []
        let lines = manager.log.lines.filter { !$0.isMarker && $0.source == "mpy" }
        for (index, entry) in entries.enumerated() {
            let parts = entry.echo.split(separator: "\n", omittingEmptySubsequences: false)
            for (i, part) in parts.enumerated() { out.append((i == 0 ? ">>> " : "... ") + part) }
            let upper = index + 1 < entries.count ? entries[index + 1].afterLineId : Int.max
            out += lines.filter { $0.id > entry.afterLineId && $0.id <= upper }.map(\.text)
        }
        return out.joined(separator: "\n")
    }
}
