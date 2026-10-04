import SwiftUI
import UIKit

/// Spec §2/§4: while a file script holds the single slot the REPL only watches
/// (the firmware refuses input with 409); the WebSocket keeps streaming output.
enum ScriptREPLGate {
    static func inputEnabled(status: ScriptRunStatus?, transport: TransportKind) -> Bool {
        !(status?.holdsFileSlot ?? false)
    }

    static func banner(status: ScriptRunStatus?, transport: TransportKind) -> String? {
        if let status, status.holdsFileSlot {
            return "'\(status.fileSlotName)' is running: the REPL is read-only until it stops."
        }
        if transport == .ble { return "Limited REPL over Bluetooth: no tab completion, output arrives via the log." }
        return nil
    }
}

struct ScriptREPLView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @EnvironmentObject var scripts: ScriptRunManager
    let status: ScriptRunStatus?
    let onClose: () -> Void
    let onStop: () -> Void

    @State private var client: WebSocketREPL?
    @State private var input = ""
    @FocusState private var focused: Bool

    private var inputEnabled: Bool { ScriptREPLGate.inputEnabled(status: status, transport: connectionManager.transport) }

    var body: some View {
        if connectionManager.transport == .ble {
            ScriptBLEREPLView(manager: scripts, status: status, onClose: onClose, onStop: onStop)
        } else {
            wifiBody
        }
    }

    private var wifiBody: some View {
        VStack(spacing: 8) {
            HStack(spacing: 14) {
                Button {
                    client?.disconnect()
                    client = nil
                    onClose()
                } label: {
                    Label("Exit REPL", systemImage: "chevron.left")
                        .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                }
                .accessibilityIdentifier("repl_exit_button")
                Spacer()
                Text("MicroPython shell").font(.subheadline.weight(.bold))
                Spacer()
                Button { UIPasteboard.general.string = client?.consoleOutput ?? "" } label: {
                    Image(systemName: "doc.on.doc")
                        .frame(minWidth: ScriptViewMetrics.minTouchTarget, minHeight: ScriptViewMetrics.minTouchTarget)
                        .contentShape(Rectangle())
                }
                .accessibilityLabel("Copy REPL output")
                .accessibilityIdentifier("repl_copy_button")
                Button("Ctrl-C") { client?.sendControlChar("C") }
                    .disabled(!inputEnabled)
                    .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                    .accessibilityIdentifier("repl_ctrl_c_button")
                Button("Ctrl-D") { client?.sendControlChar("D") }
                    .disabled(!inputEnabled)
                    .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                    .accessibilityIdentifier("repl_ctrl_d_button")
            }
            .font(.subheadline.monospaced().weight(.semibold))
            .padding(.horizontal)
            .padding(.top, 8)

            if let banner = ScriptREPLGate.banner(status: status, transport: connectionManager.transport) {
                HStack(spacing: 8) {
                    Image(systemName: "lock.fill")
                        .accessibilityHidden(true)
                    Text(banner).font(.caption.weight(.semibold))
                    Spacer()
                    if status?.holdsFileSlot == true {
                        Button("Stop", role: .destructive, action: onStop)
                            .font(.caption.weight(.bold))
                            .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                            .accessibilityIdentifier("repl_stop_button")
                    }
                }
                .accessibilityElement(children: .contain)
                .accessibilityIdentifier("repl_banner")
                .padding(10)
                .glassEffect(.regular.tint(.orange), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                .padding(.horizontal)
            }

            if let client {
                REPLTerminalConsole(client: client)
                    .padding(.horizontal)
                    .accessibilityIdentifier("repl_terminal_console")
            } else {
                Spacer()
                ProgressView().tint(.cyan)
                Spacer()
            }

            HStack(spacing: 10) {
                Text(">>>")
                    .font(.subheadline.monospaced().weight(.bold))
                    .foregroundColor(.green)
                TextField(inputEnabled ? "Send command…" : "Read-only", text: $input)
                    .font(.subheadline.monospaced())
                    .keyboardType(.asciiCapable)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .submitLabel(.send)
                    .focused($focused)
                    .disabled(!inputEnabled)
                    .onSubmit(send)
                    .accessibilityIdentifier("repl_input_field")
                Button(action: send) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.title2)
                        .frame(width: ScriptViewMetrics.minTouchTarget, height: ScriptViewMetrics.minTouchTarget)
                        .contentShape(Rectangle())
                }
                .disabled(!inputEnabled || input.isEmpty)
                .accessibilityLabel("Send")
                .accessibilityIdentifier("repl_send_button")
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            .padding(.horizontal)
            .padding(.bottom, 10)
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("repl_view")
        .onAppear(perform: connect)
        .onDisappear {
            client?.disconnect()
            client = nil
        }
        .onChange(of: input) { _, value in
            let clean = ScriptTextSanitizer.sanitize(value)
            if clean != value { input = clean }
        }
    }

    private func connect() {
        guard client == nil, connectionManager.transport == .wifi,
              let ip = connectionManager.activeDevice?.ip, !ip.isEmpty else { return }
        let repl = WebSocketREPL(host: ip, token: connectionManager.adminToken)
        client = repl
        repl.connect()
    }

    private func send() {
        guard inputEnabled, !input.isEmpty else { return }
        client?.send(input + "\r")
        input = ""
    }
}

/// BLE mini-REPL: input goes through `eval`, output comes back through the log ring.
private struct ScriptBLEREPLView: View {
    @ObservedObject var manager: ScriptRunManager
    let status: ScriptRunStatus?
    let onClose: () -> Void
    let onStop: () -> Void

    @StateObject private var repl: ScriptBLERepl
    @State private var input = ""

    init(manager: ScriptRunManager, status: ScriptRunStatus?, onClose: @escaping () -> Void, onStop: @escaping () -> Void) {
        self.manager = manager
        self.status = status
        self.onClose = onClose
        self.onStop = onStop
        _repl = StateObject(wrappedValue: ScriptBLERepl(manager: manager))
    }

    private var inputEnabled: Bool { ScriptREPLGate.inputEnabled(status: status, transport: .ble) && !repl.isSending }

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 14) {
                Button(action: onClose) {
                    Label("Exit REPL", systemImage: "chevron.left")
                        .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                }
                Spacer()
                Text("MicroPython (Bluetooth)").font(.subheadline.weight(.bold))
                Spacer()
                Button { UIPasteboard.general.string = repl.transcript() } label: {
                    Image(systemName: "doc.on.doc")
                        .frame(minWidth: ScriptViewMetrics.minTouchTarget, minHeight: ScriptViewMetrics.minTouchTarget)
                        .contentShape(Rectangle())
                }
                .accessibilityLabel("Copy REPL output")
            }
            .font(.subheadline.monospaced().weight(.semibold))
            .padding(.horizontal)
            .padding(.top, 8)

            if let banner = ScriptREPLGate.banner(status: status, transport: .ble) {
                HStack(spacing: 8) {
                    Image(systemName: status?.holdsFileSlot == true ? "lock.fill" : "info.circle.fill")
                        .accessibilityHidden(true)
                    Text(banner).font(.caption.weight(.semibold))
                    Spacer()
                    if status?.holdsFileSlot == true {
                        Button("Stop", role: .destructive, action: onStop)
                            .font(.caption.weight(.bold))
                            .frame(minHeight: ScriptViewMetrics.minTouchTarget)
                    }
                }
                .padding(10)
                .glassEffect(.regular.tint(.orange), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                .padding(.horizontal)
            }
            if let error = repl.error {
                Text(error).font(.caption.weight(.semibold)).foregroundStyle(.red).padding(.horizontal)
            }

            SelectableConsoleView(text: repl.transcript())
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding()
                .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                .padding(.horizontal)

            HStack(spacing: 10) {
                Text(">>>")
                    .font(.subheadline.monospaced().weight(.bold))
                    .foregroundColor(.green)
                TextField(inputEnabled ? "Send command…" : "Read-only", text: $input)
                    .font(.subheadline.monospaced())
                    .keyboardType(.asciiCapable)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .submitLabel(.send)
                    .disabled(!inputEnabled)
                    .onSubmit(send)
                Button(action: send) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.title2)
                        .frame(width: ScriptViewMetrics.minTouchTarget, height: ScriptViewMetrics.minTouchTarget)
                        .contentShape(Rectangle())
                }
                .disabled(!inputEnabled || input.isEmpty)
                .accessibilityLabel("Send")
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            .padding(.horizontal)
            .padding(.bottom, 10)
        }
        .onAppear { manager.replVisible = true }
        .onDisappear { manager.replVisible = false }
        .onChange(of: input) { _, value in
            let clean = ScriptTextSanitizer.sanitize(value)
            if clean != value { input = clean }
        }
    }

    private func send() {
        guard inputEnabled, !input.isEmpty else { return }
        let src = input
        input = ""
        Task { await repl.submit(src) }
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
                .font(.caption.monospaced())
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
        let font = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        textView.font = UIFontMetrics(forTextStyle: .subheadline).scaledFont(for: font)
        textView.adjustsFontForContentSizeCategory = true
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
