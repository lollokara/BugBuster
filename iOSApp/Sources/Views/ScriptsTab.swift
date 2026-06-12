import SwiftUI
import Combine

struct ScriptFile: Identifiable {
    var id: String { name }
    let name: String
}

struct StorageInfo: Codable {
    let totalBytes: Double
    let usedBytes: Double
    let freeBytes: Double
    let scriptCount: Int
    let maxScriptBytes: Int
    let maxScripts: Int
}

struct ScriptListResponse: Codable {
    let files: [String]
}

struct ScriptsTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var files: [ScriptFile] = []
    @State private var storageInfo: StorageInfo? = nil

    // Editor States
    @State private var editingFileName: String? = nil
    @State private var editingContent: String = ""
    @State private var isEditorDirty = false

    // REPL States
    @State private var replClient: WebSocketREPL? = nil
    @State private var inputCommand: String = ""
    @State private var isShowingREPL = false
    @FocusState private var replInputFocused: Bool

    // Keyboard height (manual observation since parent ignores keyboard safe area)
    @State private var keyboardHeight: CGFloat = 0

    // Alert / Prompt States
    @State private var showingCreateAlert = false
    @State private var newFileName = ""
    @State private var errorMessage: String? = nil
    @State private var runStatusMessage: String? = nil

    // Lint States
    @State private var lintErrorMessage: String? = nil
    @State private var lintSuccess: Bool? = nil

    var body: some View {
        ZStack {
            // Background
            LinearGradient(
                colors: [Color(red: 0.05, green: 0.08, blue: 0.16), Color(red: 0.02, green: 0.03, blue: 0.06)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()

            VStack(spacing: 0) {
                if let editingName = editingFileName {
                    editorView(name: editingName)
                } else if isShowingREPL {
                    replView
                } else {
                    browserView
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { n in
            if let frame = n.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect {
                withAnimation(.easeOut(duration: 0.22)) { keyboardHeight = frame.height }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in
            withAnimation(.easeOut(duration: 0.22)) { keyboardHeight = 0 }
        }
        .onAppear { loadFiles() }
        .alert("New Script Name", isPresented: $showingCreateAlert) {
            TextField("script_name.py", text: $newFileName)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) {}
            Button("Create") { createNewScript() }
        }
    }

    // MARK: - Stored Scripts Browser

    var browserView: some View {
        VStack(spacing: 16) {
            // Header toolbar
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Stored Scripts")
                        .font(.system(size: 24, weight: .bold))
                    if let storage = storageInfo {
                        Text(String(format: "Used %.1f KB / %.1f KB",
                                    storage.usedBytes / 1024, storage.totalBytes / 1024))
                            .font(.system(size: 11))
                            .foregroundColor(.secondary)
                    }
                }
                Spacer()

                Button(action: { showingCreateAlert = true }) {
                    Image(systemName: "plus.circle.fill")
                        .font(.system(size: 24))
                        .foregroundColor(.blue)
                }
                .padding(8)
                .glassEffect(.regular.tint(.blue), in: Circle())

                Button(action: {
                    isShowingREPL = true
                    initializeREPL()
                }) {
                    Image(systemName: "terminal.fill")
                        .font(.system(size: 22))
                        .foregroundColor(.cyan)
                        .padding(.leading, 8)
                }
                .padding(8)
                .glassEffect(.regular.tint(.cyan), in: Circle())
            }
            .padding(.horizontal)
            .padding(.top, 16)

            if let error = errorMessage {
                Text(error)
                    .font(.system(size: 12))
                    .foregroundColor(.red)
                    .padding(.horizontal)
            }

            if let runMsg = runStatusMessage {
                HStack {
                    Image(systemName: runMsg.hasPrefix("Error") ? "xmark.circle.fill" : "checkmark.circle.fill")
                    Text(runMsg)
                        .font(.system(size: 12, weight: .semibold))
                }
                .foregroundColor(runMsg.hasPrefix("Error") ? .red : .green)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .glassEffect(
                    runMsg.hasPrefix("Error")
                        ? .regular.tint(.red)
                        : .regular.tint(.green),
                    in: RoundedRectangle(cornerRadius: 8, style: .continuous)
                )
                .padding(.horizontal)
                .onAppear {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                        withAnimation { runStatusMessage = nil }
                    }
                }
            }

            if files.isEmpty {
                VStack(spacing: 20) {
                    Spacer()
                    Image(systemName: "doc.text.fill")
                        .font(.system(size: 44))
                        .foregroundColor(.secondary)
                    Text("No stored scripts found.")
                        .font(.system(size: 14))
                        .foregroundColor(.secondary)
                    Button("Refresh List") { loadFiles() }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                        .buttonStyle(.plain)
                    Spacer()
                }
            } else {
                List {
                    ForEach(files) { file in
                        Button(action: { openEditor(for: file.name) }) {
                            HStack {
                                Image(systemName: "doc.text").foregroundColor(.cyan)
                                Text(file.name)
                                    .font(.system(size: 15, design: .monospaced))
                                    .foregroundColor(.primary)
                                Spacer()
                                Image(systemName: "pencil").foregroundColor(.secondary)
                            }
                            .padding(.vertical, 12)
                            .padding(.horizontal, 14)
                            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                        }
                        .listRowBackground(Color.clear)
                        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                            Button(role: .destructive) {
                                deleteFile(file.name)
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                            Button {
                                // Open REPL first so output is visible, then run
                                openREPLAndRun(file.name)
                            } label: {
                                Label("Run", systemImage: "play.fill")
                            }
                            .tint(.green)
                        }
                    }
                    
                    // Spacer row to clear the floating custom tab bar
                    Color.clear
                        .frame(height: 90)
                        .listRowBackground(Color.clear)
                }
                .listStyle(.plain)
                .refreshable { loadFiles() }
            }
        }
    }

    // MARK: - Script Editor View

    func editorView(name: String) -> some View {
        VStack(spacing: 0) {
            HStack {
                Button(action: { editingFileName = nil }) {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.left")
                        Text("Back")
                    }
                }
                Spacer()
                Text(name)
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                Spacer()
                HStack(spacing: 12) {
                    Button(action: { lintScript() }) {
                        Text("Check")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundColor(.orange)
                    }
                    Button(action: { saveScript() }) {
                        Text("Save")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundColor(isEditorDirty ? .cyan : .secondary)
                    }
                    Button(action: { saveAndRunScript() }) {
                        Image(systemName: "play.fill").foregroundColor(.green)
                    }
                }
            }
            .padding()
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))

            if let success = lintSuccess {
                HStack(alignment: .top) {
                    Image(systemName: success ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                    Text(success ? "Syntax OK" : (lintErrorMessage ?? "Syntax Error"))
                        .font(.system(size: 12, weight: .semibold))
                        .lineLimit(3)
                }
                .foregroundColor(success ? .green : .red)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .frame(maxWidth: .infinity, alignment: .leading)
                .glassEffect(
                    success ? .regular.tint(.green) : .regular.tint(.red),
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                )
            }

            SelectableCodeEditor(text: $editingContent)
                .padding(.horizontal)
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                .padding(.bottom, keyboardHeight > 0 ? max(0, keyboardHeight - 66) : 10)
                .onChange(of: editingContent) { _, newValue in
                    let sanitized = newValue
                        .replacingOccurrences(of: "\u{201C}", with: "\"")
                        .replacingOccurrences(of: "\u{201D}", with: "\"")
                        .replacingOccurrences(of: "\u{2018}", with: "'")
                        .replacingOccurrences(of: "\u{2019}", with: "'")
                        .replacingOccurrences(of: "\u{2014}", with: "--")
                        .replacingOccurrences(of: "\u{2013}", with: "-")
                    if sanitized != newValue {
                        editingContent = sanitized
                    }
                    isEditorDirty = true
                    lintSuccess = nil
                }
        }
    }

    // MARK: - Interactive REPL View

    var replView: some View {
        VStack(spacing: 0) {
            // Toolbar
            HStack {
                Button(action: {
                    replClient?.disconnect()
                    replClient = nil
                    isShowingREPL = false
                    replInputFocused = false
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.left")
                        Text("Exit REPL")
                    }
                }
                Spacer()
                Text("MicroPython Shell")
                    .font(.system(size: 14, weight: .bold))
                Spacer()
                HStack(spacing: 16) {
                    Button(action: {
                        UIPasteboard.general.string = replClient?.consoleOutput ?? ""
                    }) {
                        Image(systemName: "doc.on.doc")
                            .font(.system(size: 14))
                            .foregroundColor(.cyan)
                    }
                    .padding(8)
                    .glassEffect(.regular.tint(.cyan), in: Circle())
                    Button("Ctrl-C") { replClient?.sendControlChar("C") }
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundColor(.red)
                    Button("Ctrl-D") { replClient?.sendControlChar("D") }
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundColor(.cyan)
                }
            }
            .padding(.horizontal)
            .padding(.vertical, 12)

            // Terminal output — fills all remaining space
            if let client = replClient {
                REPLTerminalConsole(client: client)
            } else {
                Spacer()
                ProgressView().tint(.cyan)
                Spacer()
            }

            // Input bar — sits above keyboard via padding
            HStack(spacing: 10) {
                Text(">>>")
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                    .foregroundColor(.green)

                TextField("Send command…", text: $inputCommand)
                    .font(.system(size: 14, design: .monospaced))
                    .keyboardType(.asciiCapable)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 10)
                    .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                    .submitLabel(.send)
                    .focused($replInputFocused)
                    .onSubmit { sendREPLCommand() }
                    .onChange(of: inputCommand) { _, newValue in
                        let sanitized = newValue
                            .replacingOccurrences(of: "\u{201C}", with: "\"")
                            .replacingOccurrences(of: "\u{201D}", with: "\"")
                            .replacingOccurrences(of: "\u{2018}", with: "'")
                            .replacingOccurrences(of: "\u{2019}", with: "'")
                            .replacingOccurrences(of: "\u{2014}", with: "--")
                            .replacingOccurrences(of: "\u{2013}", with: "-")
                        if sanitized != newValue {
                            inputCommand = sanitized
                        }
                    }

                Button(action: { sendREPLCommand() }) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.system(size: 22))
                        .foregroundColor(inputCommand.isEmpty ? .secondary : .blue)
                }
                .disabled(inputCommand.isEmpty)
                .padding(6)
                .glassEffect(.regular.tint(.blue), in: Circle())
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            // Push above keyboard (keyboard height from notification; the shell
            // ignores the keyboard safe area, so clearance is manual here.
            // Subtract the ~66pt of tab bar chrome already below the content.)
            .padding(.bottom, keyboardHeight > 0 ? max(0, keyboardHeight - 66) : 10)
        }
            .toolbar {
                ToolbarItemGroup(placement: .keyboard) {
                    replKeyboardShortcutBar(
                        onInsert: { inputCommand += $0 },
                        onDone: { replInputFocused = false }
                )
            }
        }
        .onAppear {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                replInputFocused = true
            }
        }
    }

    @ViewBuilder
    private func replKeyboardShortcutBar(onInsert: @escaping (String) -> Void, onDone: @escaping () -> Void) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(["Tab", "#", ":", "_", "(", ")", "="], id: \.self) { key in
                    Button(action: {
                        onInsert(key == "Tab" ? "    " : key)
                    }) {
                        Text(key)
                            .font(.system(size: 13, weight: .bold, design: .monospaced))
                            .foregroundColor(.cyan)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 8)
                            .background(
                                RoundedRectangle(cornerRadius: 8, style: .continuous)
                                    .fill(Color.white.opacity(0.06))
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                                            .stroke(Color.cyan.opacity(0.28), lineWidth: 1)
                                    )
                            )
                    }
                    .buttonStyle(.plain)
                }

                Button(action: onDone) {
                    Text("Done")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 8)
                        .background(
                            RoundedRectangle(cornerRadius: 8, style: .continuous)
                                .fill(Color.blue.opacity(0.35))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                                        .stroke(Color.blue.opacity(0.45), lineWidth: 1)
                                )
                        )
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
        }
    }

    // MARK: - Helper Methods

    private func initializeREPL() {
        guard replClient == nil else { return }
        let ip = connectionManager.activeDevice?.ip ?? ""
        replClient = WebSocketREPL(host: ip, token: connectionManager.adminToken)
        replClient?.connect()
    }

    /// Open REPL, connect it, then run the script so output is visible.
    private func openREPLAndRun(_ name: String) {
        isShowingREPL = true
        initializeREPL()
        // Brief delay to let WebSocket auth complete, then run
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
            runScript(name)
        }
    }

    private func sendREPLCommand() {
        guard !inputCommand.isEmpty else { return }
        replClient?.send(inputCommand + "\r")
        inputCommand = ""
    }

    private func loadFiles() {
        Task {
            do {
                let res: ScriptListResponse = try await connectionManager.getRequest(path: "/api/scripts/files")
                let storage: StorageInfo = try await connectionManager.getRequest(path: "/api/scripts/storage")
                DispatchQueue.main.async {
                    self.files = res.files.map { ScriptFile(name: $0) }
                    self.storageInfo = storage
                    self.errorMessage = nil
                }
            } catch {
                DispatchQueue.main.async {
                    self.errorMessage = "Failed loading scripts: \(error.localizedDescription)"
                }
            }
        }
    }

    private func createNewScript() {
        let name = newFileName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return }
        let normalizedName = name.hasSuffix(".py") ? name : "\(name).py"
        Task {
            do {
                let urlStr = connectionManager.activeDevice?.ip ?? ""
                var urlComponents = URLComponents()
                urlComponents.scheme = "http"
                urlComponents.host = urlStr
                urlComponents.path = "/api/scripts/files"
                urlComponents.queryItems = [URLQueryItem(name: "name", value: normalizedName)]
                guard let url = urlComponents.url else { return }
                var request = URLRequest(url: url)
                request.httpMethod = "POST"
                request.setValue("text/plain", forHTTPHeaderField: "Content-Type")
                if !connectionManager.adminToken.isEmpty {
                    request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
                }
                request.httpBody = "# \(normalizedName)\n# Write your MicroPython code here\n".data(using: .utf8)
                let (_, response) = try await URLSession.shared.data(for: request)
                guard let httpResponse = response as? HTTPURLResponse,
                      (200...299).contains(httpResponse.statusCode) else {
                    throw URLError(.badServerResponse)
                }
                DispatchQueue.main.async {
                    newFileName = ""
                    loadFiles()
                    openEditor(for: normalizedName)
                }
            } catch {
                DispatchQueue.main.async { errorMessage = "Create failed: \(error.localizedDescription)" }
            }
        }
    }

    private func openEditor(for name: String) {
        Task {
            do {
                let urlStr = connectionManager.activeDevice?.ip ?? ""
                var urlComponents = URLComponents()
                urlComponents.scheme = "http"
                urlComponents.host = urlStr
                urlComponents.path = "/api/scripts/files/get"
                urlComponents.queryItems = [URLQueryItem(name: "name", value: name)]
                guard let url = urlComponents.url else { return }
                var request = URLRequest(url: url)
                request.httpMethod = "GET"
                if !connectionManager.adminToken.isEmpty {
                    request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
                }
                let (data, response) = try await URLSession.shared.data(for: request)
                guard let httpResponse = response as? HTTPURLResponse,
                      (200...299).contains(httpResponse.statusCode) else {
                    throw URLError(.badServerResponse)
                }
                let content = String(data: data, encoding: .utf8) ?? ""
                DispatchQueue.main.async {
                    self.editingContent = content
                    self.editingFileName = name
                    self.isEditorDirty = false
                }
            } catch {
                DispatchQueue.main.async { errorMessage = "Load file failed: \(error.localizedDescription)" }
            }
        }
    }

    private func saveScript() {
        Task { await saveScriptCore() }
    }

    @discardableResult
    private func saveScriptCore() async -> Bool {
        guard let name = editingFileName else { return false }
        guard let ip = connectionManager.activeDevice?.ip, !ip.isEmpty else { return false }
        do {
            var urlComponents = URLComponents()
            urlComponents.scheme = "http"
            urlComponents.host = ip
            urlComponents.path = "/api/scripts/files"
            urlComponents.queryItems = [URLQueryItem(name: "name", value: name)]
            guard let url = urlComponents.url else { return false }
            var request = URLRequest(url: url)
            request.httpMethod = "POST"
            request.setValue("text/plain", forHTTPHeaderField: "Content-Type")
            if !connectionManager.adminToken.isEmpty {
                request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
            }
            request.httpBody = editingContent.data(using: .utf8)
            let (_, response) = try await URLSession.shared.data(for: request)
            guard let httpResponse = response as? HTTPURLResponse,
                  (200...299).contains(httpResponse.statusCode) else {
                throw URLError(.badServerResponse)
            }
            DispatchQueue.main.async { self.isEditorDirty = false }
            return true
        } catch {
            DispatchQueue.main.async { self.errorMessage = "Save file failed: \(error.localizedDescription)" }
            return false
        }
    }

    private func runScript(_ name: String) {
        Task {
            guard let ip = connectionManager.activeDevice?.ip, !ip.isEmpty else {
                DispatchQueue.main.async { runStatusMessage = "Error: not connected" }
                return
            }
            var urlComponents = URLComponents()
            urlComponents.scheme = "http"
            urlComponents.host = ip
            urlComponents.path = "/api/scripts/run-file"
            urlComponents.queryItems = [URLQueryItem(name: "name", value: name)]
            guard let url = urlComponents.url else {
                DispatchQueue.main.async { runStatusMessage = "Error: bad URL" }
                return
            }
            var request = URLRequest(url: url)
            request.httpMethod = "POST"
            if !connectionManager.adminToken.isEmpty {
                request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
            }
            do {
                let (_, response) = try await URLSession.shared.data(for: request)
                let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                DispatchQueue.main.async {
                    if (200...299).contains(code) {
                        withAnimation { runStatusMessage = "Running \(name)…" }
                    } else {
                        withAnimation { runStatusMessage = "Error: server returned \(code)" }
                    }
                }
            } catch {
                DispatchQueue.main.async {
                    withAnimation { runStatusMessage = "Error: \(error.localizedDescription)" }
                }
            }
        }
    }

    private func saveAndRunScript() {
        guard let name = editingFileName else { return }
        Task {
            let saved = await saveScriptCore()
            if saved {
                DispatchQueue.main.async {
                    // Navigate to REPL to see output
                    editingFileName = nil
                    openREPLAndRun(name)
                }
            }
        }
    }

    private func lintScript() {
        guard let ip = connectionManager.activeDevice?.ip, !ip.isEmpty else {
            self.lintErrorMessage = "Not connected"
            self.lintSuccess = false
            return
        }

        Task {
            do {
                var urlComponents = URLComponents()
                urlComponents.scheme = "http"
                urlComponents.host = ip
                urlComponents.path = "/api/scripts/lint"
                guard let url = urlComponents.url else { return }

                var request = URLRequest(url: url)
                request.httpMethod = "POST"
                request.setValue("text/plain", forHTTPHeaderField: "Content-Type")
                if !connectionManager.adminToken.isEmpty {
                    request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
                }
                request.httpBody = editingContent.data(using: .utf8)

                let (data, response) = try await URLSession.shared.data(for: request)
                guard let httpResponse = response as? HTTPURLResponse,
                      (200...299).contains(httpResponse.statusCode) else {
                    let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                    DispatchQueue.main.async {
                        self.lintErrorMessage = "Server returned status \(code)"
                        self.lintSuccess = false
                    }
                    return
                }

                struct LintResponse: Codable {
                    let ok: Bool
                    let err: String?
                }

                let res = try JSONDecoder().decode(LintResponse.self, from: data)
                DispatchQueue.main.async {
                    self.lintSuccess = res.ok
                    self.lintErrorMessage = res.err
                }
            } catch {
                DispatchQueue.main.async {
                    self.lintErrorMessage = "Network error: \(error.localizedDescription)"
                    self.lintSuccess = false
                }
            }
        }
    }

    private func deleteFile(_ name: String) {
        Task {
            do {
                let urlStr = connectionManager.activeDevice?.ip ?? ""
                var urlComponents = URLComponents()
                urlComponents.scheme = "http"
                urlComponents.host = urlStr
                urlComponents.path = "/api/scripts/files"
                urlComponents.queryItems = [URLQueryItem(name: "name", value: name)]
                guard let url = urlComponents.url else { return }
                var request = URLRequest(url: url)
                request.httpMethod = "DELETE"
                if !connectionManager.adminToken.isEmpty {
                    request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
                }
                let (_, response) = try await URLSession.shared.data(for: request)
                guard let httpResponse = response as? HTTPURLResponse,
                      (200...299).contains(httpResponse.statusCode) else {
                    throw URLError(.badServerResponse)
                }
                DispatchQueue.main.async { loadFiles() }
            } catch {
                DispatchQueue.main.async { errorMessage = "Delete failed: \(error.localizedDescription)" }
            }
        }
    }
}

// MARK: - REPL Terminal Console

struct REPLTerminalConsole: View {
    @ObservedObject var client: WebSocketREPL
    @State private var cursorVisible = true
    let cursorTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            SelectableConsoleView(text: client.consoleOutput)
                .frame(maxWidth: .infinity, maxHeight: .infinity)

            // Blinking block cursor
            Text(cursorVisible ? "█" : " ")
                .font(.system(size: 13, design: .monospaced))
                .foregroundColor(Color(red: 0.25, green: 0.85, blue: 0.55))
                .padding(.horizontal, 12)
                .padding(.bottom, 6)
        }
        .padding()
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .onReceive(cursorTimer) { _ in
            cursorVisible.toggle()
        }
    }
}

struct SelectableConsoleView: UIViewRepresentable {
    let text: String

    func makeUIView(context: Context) -> UITextView {
        let textView = UITextView()
        textView.backgroundColor = .clear
        textView.textColor = UIColor(red: 0.78, green: 0.87, blue: 0.95, alpha: 1.0)
        textView.font = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        textView.isEditable = false
        textView.isSelectable = true
        textView.isScrollEnabled = true
        textView.showsVerticalScrollIndicator = true
        textView.showsHorizontalScrollIndicator = false
        textView.smartQuotesType = .no
        textView.smartDashesType = .no
        textView.smartInsertDeleteType = .no
        textView.autocorrectionType = .no
        textView.autocapitalizationType = .none
        return textView
    }

    func updateUIView(_ uiView: UITextView, context: Context) {
        if uiView.text != text {
            let isAtBottom = uiView.contentOffset.y >= (uiView.contentSize.height - uiView.frame.size.height - 10) || uiView.text.isEmpty
            uiView.text = text
            if isAtBottom {
                let range = NSRange(location: text.utf16.count, length: 0)
                uiView.scrollRangeToVisible(range)
            }
        }
    }
}

struct SelectableCodeEditor: UIViewRepresentable {
    @Binding var text: String

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeUIView(context: Context) -> UITextView {
        let textView = UITextView()
        textView.backgroundColor = .clear
        textView.textColor = .white
        textView.font = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        textView.isEditable = true
        textView.isSelectable = true
        textView.isScrollEnabled = true
        textView.delegate = context.coordinator
        context.coordinator.textView = textView

        // Disable smart features
        textView.smartQuotesType = .no
        textView.smartDashesType = .no
        textView.smartInsertDeleteType = .no
        textView.autocorrectionType = .no
        textView.autocapitalizationType = .none
        textView.keyboardType = .asciiCapable

        // Build horizontally scrollable accessory bar
        let scrollView = UIScrollView()
        scrollView.frame = CGRect(x: 0, y: 0, width: 1, height: 44)
        scrollView.autoresizingMask = [.flexibleWidth]
        scrollView.backgroundColor = .clear
        scrollView.showsHorizontalScrollIndicator = false
        scrollView.alwaysBounceHorizontal = true

        let stackView = UIStackView()
        stackView.axis = .horizontal
        stackView.spacing = 8
        stackView.alignment = .fill
        stackView.distribution = .fillProportionally

        let keys = ["Tab", "#", ":", "_", "(", ")", "=", "Done"]
        for key in keys {
            let button = UIButton(type: .system)
            button.configuration = UIButton.Configuration.plain()
            button.setTitle(key, for: .normal)
            button.titleLabel?.font = UIFont.systemFont(ofSize: 15, weight: key == "Done" || key == "Tab" ? .bold : .semibold)
            button.setTitleColor(.cyan, for: .normal)
            button.backgroundColor = UIColor(white: 1.0, alpha: 0.04)
            button.layer.cornerRadius = 8
            button.configuration?.contentInsets = NSDirectionalEdgeInsets(top: 8, leading: 16, bottom: 8, trailing: 16)
            
            if key == "Done" {
                button.addTarget(context.coordinator, action: #selector(Coordinator.donePressed), for: .touchUpInside)
            } else {
                button.addTarget(context.coordinator, action: #selector(Coordinator.accessoryButtonTapped(_:)), for: .touchUpInside)
            }
            stackView.addArrangedSubview(button)
        }

        stackView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.addSubview(stackView)

        NSLayoutConstraint.activate([
            stackView.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 12),
            stackView.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -12),
            stackView.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 6),
            stackView.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -6),
            stackView.heightAnchor.constraint(equalTo: scrollView.frameLayoutGuide.heightAnchor, constant: -12)
        ])

        textView.inputAccessoryView = scrollView

        return textView
    }

    func updateUIView(_ uiView: UITextView, context: Context) {
        if uiView.text != text {
            uiView.text = text
        }
    }

    class Coordinator: NSObject, UITextViewDelegate {
        var parent: SelectableCodeEditor
        weak var textView: UITextView?

        init(_ parent: SelectableCodeEditor) {
            self.parent = parent
        }

        @objc func accessoryButtonTapped(_ sender: UIButton) {
            guard textView != nil else { return }
            let title = sender.currentTitle ?? ""
            let toInsert = title == "Tab" ? "    " : title
            insertText(toInsert)
        }

        @objc func donePressed() {
            textView?.resignFirstResponder()
        }

        private func insertText(_ string: String) {
            guard let textView = textView else { return }
            let range = textView.selectedRange
            if let textRange = Range(range, in: textView.text) {
                let newText = textView.text.replacingCharacters(in: textRange, with: string)
                textView.text = newText
                parent.text = newText
                textView.selectedRange = NSRange(location: range.location + string.count, length: 0)
            }
        }

        func textViewDidChange(_ textView: UITextView) {
            parent.text = textView.text
        }

        func textView(_ textView: UITextView, shouldChangeTextIn range: NSRange, replacementText text: String) -> Bool {
            if text == "\t" {
                insertText("    ")
                return false
            }
            return true
        }
    }
}
