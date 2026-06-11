import Foundation
import Combine

public enum REPLConnectionState: Equatable {
    case disconnected
    case connecting
    case connected
    case error(String)
}

public class WebSocketREPL: ObservableObject {
    @Published public var consoleOutput = ""
    @Published public var connectionState: REPLConnectionState = .disconnected
    
    private var webSocketTask: URLSessionWebSocketTask?
    private let session = URLSession.shared
    private let host: String
    private let token: String
    
    public init(host: String, token: String) {
        self.host = host
        self.token = token
    }
    
    public func connect() {
        guard connectionState == .disconnected else { return }
        
        var cleanHost = host.trimmingCharacters(in: .whitespacesAndNewlines)
        if cleanHost.lowercased().hasPrefix("http://") {
            cleanHost = String(cleanHost.dropFirst(7))
        } else if cleanHost.lowercased().hasPrefix("https://") {
            cleanHost = String(cleanHost.dropFirst(8))
        }
        
        let wsUrlStr = "ws://\(cleanHost)/api/scripts/repl/ws"
        guard let url = URL(string: wsUrlStr) else {
            DispatchQueue.main.async {
                self.connectionState = .error("Invalid URL")
            }
            return
        }
        
        DispatchQueue.main.async {
            self.connectionState = .connecting
        }
        
        let task = session.webSocketTask(with: url)
        self.webSocketTask = task
        task.resume()
        
        // Start the receive pump immediately — before auth — so we never miss
        // frames (including the welcome banner) that arrive before the send callback fires.
        self.readMessage()
        
        // Send the authentication token as the first message
        let authMessage = URLSessionWebSocketTask.Message.string(token)
        task.send(authMessage) { [weak self] error in
            if let error = error {
                DispatchQueue.main.async {
                    self?.connectionState = .error("Auth send failed: \(error.localizedDescription)")
                }
                self?.disconnect()
            } else {
                DispatchQueue.main.async {
                    self?.connectionState = .connected
                }
                // readMessage() is already running from above — no second call needed.
            }
        }
    }
    
    public func disconnect() {
        webSocketTask?.cancel(with: .goingAway, reason: nil)
        webSocketTask = nil
        DispatchQueue.main.async {
            self.connectionState = .disconnected
        }
    }
    
    public func send(_ text: String) {
        guard connectionState == .connected else { return }
        let message = URLSessionWebSocketTask.Message.string(text)
        webSocketTask?.send(message) { error in
            if let error = error {
                print("Send error: \(error)")
            }
        }
    }
    
    public func sendControlChar(_ char: Character) {
        // Ctrl-C is 0x03, Ctrl-D is 0x04
        if char == "C" {
            send("\u{03}")
        } else if char == "D" {
            send("\u{04}")
        }
    }
    
    private func readMessage() {
        guard let task = webSocketTask else { return }

        task.receive { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .success(let message):
                switch message {
                case .string(let text):
                    DispatchQueue.main.async {
                        self.consoleOutput += text
                        if self.consoleOutput.count > 50000 {
                            self.consoleOutput = String(self.consoleOutput.suffix(40000))
                        }
                    }
                case .data(let data):
                    if let text = String(data: data, encoding: .utf8) {
                        DispatchQueue.main.async {
                            self.consoleOutput += text
                        }
                    }
                @unknown default:
                    break
                }
                // Re-arm — keep receiving as long as we're connected or connecting
                self.readMessage()
            case .failure(let error):
                print("WebSocket read error: \(error)")
                DispatchQueue.main.async {
                    self.connectionState = .error(error.localizedDescription)
                }
                self.disconnect()
            }
        }
    }
}
