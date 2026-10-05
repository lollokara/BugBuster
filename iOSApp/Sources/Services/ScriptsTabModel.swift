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
    struct Entry: Equatable {
        let echo: String
        var output: [String] = []
    }

    @Published private(set) var entries: [Entry] = []
    @Published private(set) var error: String?
    @Published private(set) var isSending = false

    private let manager: ScriptRunManager
    private var logObserver: AnyCancellable?
    private var lastSeenLineId: Int = 0

    init(manager: ScriptRunManager) {
        self.manager = manager
        self.lastSeenLineId = manager.log.lines.last?.id ?? 0
        logObserver = manager.log.$lines.sink { [weak self] lines in
            self?.processLogLines(lines)
        }
    }

    private func processLogLines(_ lines: [ScriptLogLine]) {
        if lines.isEmpty {
            lastSeenLineId = 0
            return
        }
        if let lastId = lines.last?.id, lastId < lastSeenLineId {
            lastSeenLineId = 0
        }
        let newLines = lines.filter { $0.id > lastSeenLineId }
        guard !newLines.isEmpty else { return }
        lastSeenLineId = newLines.last?.id ?? lastSeenLineId

        let mpyLines = newLines.filter { !$0.isMarker && $0.source == "mpy" }
        if !mpyLines.isEmpty && !entries.isEmpty {
            entries[entries.count - 1].output.append(contentsOf: mpyLines.map(\.text))
        }
    }

    func submit(_ src: String) async {
        guard let client = manager.client, !src.isEmpty else { return }
        processLogLines(manager.log.lines)
        isSending = true
        defer { isSending = false }
        entries.append(Entry(echo: src, output: []))
        lastSeenLineId = manager.log.lines.last?.id ?? 0
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
        for entry in entries {
            let parts = entry.echo.split(separator: "\n", omittingEmptySubsequences: false)
            for (i, part) in parts.enumerated() { out.append((i == 0 ? ">>> " : "... ") + part) }
            out.append(contentsOf: entry.output)
        }
        return out.joined(separator: "\n")
    }
}
