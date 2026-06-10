import SwiftUI

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
    @State private var terminalText: String = ""
    
    // Alert / Prompt States
    @State private var showingCreateAlert = false
    @State private var newFileName = ""
    @State private var errorMessage: String? = nil
    
    var body: some View {
        NavigationStack {
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
            .navigationTitle("Python Scripts")
            .navigationBarHidden(true)
            .onAppear {
                loadFiles()
            }
            .alert("New Script Name", isPresented: $showingCreateAlert) {
                TextField("script_name.py", text: $newFileName)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Cancel", role: .cancel) {}
                Button("Create") {
                    createNewScript()
                }
            }
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
                        Text(String(format: "Used %.1f KB / %.1f KB", storage.usedBytes / 1024, storage.totalBytes / 1024))
                            .font(.system(size: 11))
                            .foregroundColor(.secondary)
                    }
                }
                Spacer()
                
                Button(action: {
                    showingCreateAlert = true
                }) {
                    Image(systemName: "plus.circle.fill")
                        .font(.system(size: 24))
                        .foregroundColor(.blue)
                }
                
                Button(action: {
                    isShowingREPL = true
                    initializeREPL()
                }) {
                    Image(systemName: "terminal.fill")
                        .font(.system(size: 22))
                        .foregroundColor(.cyan)
                        .padding(.leading, 8)
                }
            }
            .padding(.horizontal)
            .padding(.top, 16)
            
            if let error = errorMessage {
                Text(error)
                    .font(.system(size: 12))
                    .foregroundColor(.red)
                    .padding(.horizontal)
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
                    Button("Refresh List") {
                        loadFiles()
                    }
                    .buttonStyle(.bordered)
                    Spacer()
                }
            } else {
                List {
                    ForEach(files) { file in
                        Button(action: {
                            openEditor(for: file.name)
                        }) {
                            HStack {
                                Image(systemName: "doc.text")
                                    .foregroundColor(.cyan)
                                Text(file.name)
                                    .font(.system(size: 15, design: .monospaced))
                                    .foregroundColor(.primary)
                                Spacer()
                                Image(systemName: "pencil")
                                    .foregroundColor(.secondary)
                            }
                        }
                        .listRowBackground(Color.white.opacity(0.02))
                        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                            Button(role: .destructive) {
                                deleteFile(file.name)
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                            
                            Button {
                                runScript(file.name)
                            } label: {
                                Label("Run", systemImage: "play.fill")
                            }
                            .tint(.green)
                        }
                    }
                }
                .listStyle(.plain)
                .refreshable {
                    loadFiles()
                }
            }
        }
    }
    
    // MARK: - Script Editor View
    func editorView(name: String) -> some View {
        VStack(spacing: 0) {
            // Editor toolbar
            HStack {
                Button(action: {
                    if isEditorDirty {
                        // Confirm discard changes
                        editingFileName = nil
                    } else {
                        editingFileName = nil
                    }
                }) {
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
                    Button(action: {
                        saveScript()
                    }) {
                        Text("Save")
                            .font(.system(size: 14, weight: .bold))
                            .foregroundColor(isEditorDirty ? .cyan : .secondary)
                    }
                    
                    Button(action: {
                        saveAndRunScript()
                    }) {
                        Image(systemName: "play.fill")
                            .foregroundColor(.green)
                    }
                }
            }
            .padding()
            .background(Color.black.opacity(0.3))
            
            TextEditor(text: $editingContent)
                .font(.system(size: 13, design: .monospaced))
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .scrollContentBackground(.hidden)
                .background(Color(red: 0.02, green: 0.03, blue: 0.05))
                .onChange(of: editingContent) { _ in
                    isEditorDirty = true
                }
        }
    }
    
    // MARK: - Interactive REPL View
    var replView: some View {
        VStack(spacing: 0) {
            // REPL Toolbar
            HStack {
                Button(action: {
                    replClient?.disconnect()
                    isShowingREPL = false
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
                    Button("Ctrl-C") {
                        replClient?.sendControlChar("C")
                    }
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(.red)
                    
                    Button("Ctrl-D") {
                        replClient?.sendControlChar("D")
                    }
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(.cyan)
                }
            }
            .padding()
            .background(Color.black.opacity(0.3))
            
            // Console Terminal Output
            if let client = replClient {
                REPLTerminalConsole(client: client)
            } else {
                Spacer()
                ProgressView()
                Spacer()
            }
            
            // Interactive Input Row
            HStack(spacing: 8) {
                Text(">>>")
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                    .foregroundColor(.green)
                
                TextField("Send command", text: $inputCommand, onCommit: {
                    sendREPLCommand()
                })
                .font(.system(size: 14, design: .monospaced))
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .submitLabel(.send)
                
                Button(action: {
                    sendREPLCommand()
                }) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.system(size: 20))
                        .foregroundColor(.blue)
                }
            }
            .padding()
            .background(Color.black.opacity(0.4))
        }
    }
    
    // MARK: - Helper Methods
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
                guard let httpResponse = response as? HTTPURLResponse, (200...299).contains(httpResponse.statusCode) else {
                    throw URLError(.badServerResponse)
                }
                
                DispatchQueue.main.async {
                    newFileName = ""
                    loadFiles()
                    openEditor(for: normalizedName)
                }
            } catch {
                DispatchQueue.main.async {
                    errorMessage = "Create failed: \(error.localizedDescription)"
                }
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
                guard let httpResponse = response as? HTTPURLResponse, (200...299).contains(httpResponse.statusCode) else {
                    throw URLError(.badServerResponse)
                }
                
                let content = String(data: data, encoding: .utf8) ?? ""
                
                DispatchQueue.main.async {
                    self.editingContent = content
                    self.editingFileName = name
                    self.isEditorDirty = false
                }
            } catch {
                DispatchQueue.main.async {
                    errorMessage = "Load file failed: \(error.localizedDescription)"
                }
            }
        }
    }
    
    private func saveScript() {
        guard let name = editingFileName else { return }
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
                request.httpMethod = "POST"
                request.setValue("text/plain", forHTTPHeaderField: "Content-Type")
                if !connectionManager.adminToken.isEmpty {
                    request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
                }
                request.httpBody = editingContent.data(using: .utf8)
                
                let (_, response) = try await URLSession.shared.data(for: request)
                guard let httpResponse = response as? HTTPURLResponse, (200...299).contains(httpResponse.statusCode) else {
                    throw URLError(.badServerResponse)
                }
                
                DispatchQueue.main.async {
                    isEditorDirty = false
                }
            } catch {
                DispatchQueue.main.async {
                    errorMessage = "Save file failed: \(error.localizedDescription)"
                }
            }
        }
    }
    
    private func runScript(_ name: String) {
        Task {
            let urlStr = connectionManager.activeDevice?.ip ?? ""
            var urlComponents = URLComponents()
            urlComponents.scheme = "http"
            urlComponents.host = urlStr
            urlComponents.path = "/api/scripts/run-file"
            urlComponents.queryItems = [URLQueryItem(name: "name", value: name)]
            
            guard let url = urlComponents.url else { return }
            var request = URLRequest(url: url)
            request.httpMethod = "POST"
            if !connectionManager.adminToken.isEmpty {
                request.setValue(connectionManager.adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
            }
            
            _ = try? await URLSession.shared.data(for: request)
        }
    }
    
    private func saveAndRunScript() {
        saveScript()
        if let name = editingFileName {
            runScript(name)
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
                guard let httpResponse = response as? HTTPURLResponse, (200...299).contains(httpResponse.statusCode) else {
                    throw URLError(.badServerResponse)
                }
                
                DispatchQueue.main.async {
                    loadFiles()
                }
            } catch {
                DispatchQueue.main.async {
                    errorMessage = "Delete failed: \(error.localizedDescription)"
                }
            }
        }
    }
    
    private func initializeREPL() {
        let ip = connectionManager.activeDevice?.ip ?? ""
        replClient = WebSocketREPL(host: ip, token: connectionManager.adminToken)
        replClient?.connect()
    }
    
    private func sendREPLCommand() {
        guard !inputCommand.isEmpty else { return }
        replClient?.send(inputCommand + "\r\n")
        inputCommand = ""
    }
}

struct REPLTerminalConsole: View {
    @ObservedObject var client: WebSocketREPL
    
    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 4) {
                    Text(client.consoleOutput)
                        .font(.system(size: 13, design: .monospaced))
                        .foregroundColor(Color(red: 0.8, green: 0.85, blue: 0.9))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .id("bottom")
                }
                .padding()
            }
            .background(Color(red: 0.01, green: 0.02, blue: 0.04))
            .onChange(of: client.consoleOutput) { _ in
                proxy.scrollTo("bottom", anchor: .bottom)
            }
        }
    }
}
