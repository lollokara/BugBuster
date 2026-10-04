import Foundation
import Combine

/// App-wide Scripts state shared by every tab and both shells (spec §4): the run
/// status behind the pill and the header, the device log copy behind the console,
/// the replace prompt and autorun. It polls on a cadence set by what is on screen
/// (`ScriptPollPolicy`), only while the app is active and a device is connected.
/// Created once in `BugBusterApp` and injected as an environment object, so the
/// pill and the iPad log column outlive tab switches.
@MainActor
final class ScriptRunManager: ObservableObject {
    struct ReplacePrompt: Identifiable, Equatable {
        let running: String
        let requested: String
        let background: Bool

        var id: String { "\(running)->\(requested)" }
        /// Spec copy, verbatim.
        var message: String { "Stop '\(running)' and run '\(requested)'?" }
    }

    typealias Sleeper = (TimeInterval) async throws -> Void

    /// Pages read per drain before yielding (8 × 4 KB = 32 KB: the whole ring twice over).
    static let maxPagesPerDrain = 8

    @Published private(set) var status: ScriptRunStatus?
    @Published private(set) var autorun: ScriptAutorunStatus?
    @Published private(set) var lastError: String?
    @Published private(set) var uptimeAnchor: UptimeAnchor?
    @Published var replacePrompt: ReplacePrompt?
    @Published var consoleVisible = false {
        didSet { if consoleVisible != oldValue { restartPolling() } }
    }
    @Published var scriptsTabVisible = false {
        didSet { if scriptsTabVisible != oldValue { restartPolling() } }
    }
    /// The BLE mini-REPL is on screen: its output arrives through the log, so logs
    /// poll at the console cadence.
    @Published var replVisible = false {
        didSet { if replVisible != oldValue { restartPolling() } }
    }

    let log: ScriptLogStore
    private(set) var client: ScriptsClient?
    private(set) var transport: TransportKind = .wifi

    private let sleeper: Sleeper
    private let clock: () -> Date
    private let autoStart: Bool
    private var loop: Task<Void, Never>?
    private var nextStatusAt = Date.distantPast
    private var nextLogsAt = Date.distantPast
    private var appActive = true
    private var deviceKey: String?
    private var cancellables = Set<AnyCancellable>()
    private weak var connection: ConnectionManager?

    init(log: ScriptLogStore? = nil,
         sleeper: @escaping Sleeper = { try await Task.sleep(nanoseconds: UInt64($0 * 1_000_000_000)) },
         clock: @escaping () -> Date = Date.init,
         autoStart: Bool = true) {
        self.log = log ?? ScriptLogStore()
        self.sleeper = sleeper
        self.clock = clock
        self.autoStart = autoStart
        #if DEBUG
        if ProcessInfo.processInfo.environment["BB_SCRIPTS_CONSOLE"] == "1" { consoleVisible = true }
        #endif
    }

    var policy: ScriptPollPolicy {
        .make(consoleVisible: consoleVisible || replVisible, scriptsTabVisible: scriptsTabVisible, active: status?.isActive ?? false)
    }

    /// The REPL may only watch while a file script holds the single slot (spec §2/§4).
    var isREPLReadOnly: Bool { status?.holdsFileSlot ?? false }

    var isConnected: Bool { client != nil }

    // MARK: - Connection

    private struct LinkKey: Equatable {
        let connected: Bool
        let transport: TransportKind
        let device: String?
    }

    /// Follow the connection: a client per connected device, nothing while disconnected.
    func attach(_ cm: ConnectionManager) {
        connection = cm
        cancellables.removeAll()
        Publishers.CombineLatest3(cm.$connectionState, cm.$transport, cm.$activeDevice)
            .map { LinkKey(connected: $0 == .connected && $2 != nil, transport: $1, device: $2?.id) }
            .removeDuplicates()
            .receive(on: DispatchQueue.main)
            .sink { [weak self, weak cm] key in
                MainActor.assumeIsolated {
                    guard let self, let cm else { return }
                    guard key.connected, let device = key.device else { self.disconnect(); return }
                    self.connect(client: ScriptsClient(wire: Self.makeWire(cm)), deviceKey: device, transport: key.transport)
                }
            }
            .store(in: &cancellables)
        cm.$lastStatus
            .compactMap { $0?.uptimeMs }
            .receive(on: DispatchQueue.main)
            .sink { [weak self] ms in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.uptimeAnchor = UptimeAnchor(uptimeMs: ms, at: self.clock())
                }
            }
            .store(in: &cancellables)
    }

    private static func makeWire(_ cm: ConnectionManager) -> ScriptsWire {
        #if DEBUG
        if cm.isMockActive { return MockScriptsWire() }
        #endif
        return ConnectionScriptsWire(cm)
    }

    /// Start talking to a device. Another device starts a clean log; the same one
    /// (e.g. after a drop) keeps its cursor and lines.
    func connect(client: ScriptsClient, deviceKey: String, transport: TransportKind) {
        if self.deviceKey != deviceKey {
            log.reset()
            status = nil
            autorun = nil
        }
        self.deviceKey = deviceKey
        self.client = client
        self.transport = transport
        lastError = nil
        restartPolling()
    }

    func disconnect() {
        loop?.cancel()
        loop = nil
        client = nil
        status = nil
        replacePrompt = nil
    }

    func setAppActive(_ active: Bool) {
        guard active != appActive else { return }
        appActive = active
        restartPolling()
    }

    // MARK: - Polling

    /// Cancel and restart the loop so a visibility change takes effect now.
    private func restartPolling() {
        loop?.cancel()
        loop = nil
        nextStatusAt = .distantPast
        nextLogsAt = .distantPast
        guard autoStart, appActive, client != nil else { return }
        loop = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                let wait = await self.tick()
                do { try await self.sleeper(wait) } catch { return }
            }
        }
    }

    func waitForPollingLoop() async {
        await loop?.value
    }

    /// One scheduling step: poll what is due, return the delay to the next step.
    @discardableResult
    func tick() async -> TimeInterval {
        guard client != nil else { return policy.tick }
        let now = clock()
        if now >= nextStatusAt {
            await refreshStatus()
            nextStatusAt = now.addingTimeInterval(policy.status)
        }
        if let every = policy.logs, now >= nextLogsAt {
            await drainLogs()
            nextLogsAt = now.addingTimeInterval(every)
        }
        return policy.tick
    }

    func refreshStatus() async {
        guard let client else { return }
        do {
            let fresh = try await client.status()
            let wasActive = status?.isActive ?? false
            status = fresh
            lastError = nil
            // The run just ended: fetch its last lines (a traceback) even if nothing polls logs.
            if wasActive && !fresh.isActive { await drainLogs() }
        } catch {
            lastError = error.localizedDescription
        }
    }

    /// Read pages until one comes back short (caught up), at most `maxPagesPerDrain`.
    func drainLogs() async {
        guard let client else { return }
        for _ in 0..<Self.maxPagesPerDrain {
            do {
                let page = try await client.logs(since: log.cursor)
                log.ingest(page)
                if page.restarted { continue }              // re-read the new ring from 0 now
                if page.data.count < ScriptsClient.logPageBytes { return }
            } catch {
                lastError = error.localizedDescription
                return
            }
        }
    }

    // MARK: - Run control

    func run(_ name: String, background: Bool) async {
        guard let client else { return }
        do {
            let outcome = try await client.runFile(name, background: background, replace: false)
            await handle(outcome, requested: name, background: background)
        } catch {
            report(error)
        }
    }

    /// The user confirmed "Stop '<x>' and run '<y>'?": same request with replace=1.
    func confirmReplace() async {
        guard let prompt = replacePrompt, let client else { return }
        replacePrompt = nil
        do {
            let outcome = try await client.runFile(prompt.requested, background: prompt.background, replace: true)
            await handle(outcome, requested: prompt.requested, background: prompt.background)
        } catch {
            report(error)
        }
    }

    func cancelReplace() {
        replacePrompt = nil
    }

    func stop() async {
        guard let client else { return }
        do {
            try await client.stop()
            await refreshStatus()
        } catch {
            report(error)
        }
    }

    private func handle(_ outcome: ScriptRunOutcome, requested: String, background: Bool) async {
        switch outcome {
        case .started(_, let name, _):
            log.appendMarker(background ? "Started '\(name)' in background" : "Started '\(name)'")
            if !background { consoleVisible = true }
            await refreshStatus()
        case .busy(let running, _):
            // Name the holder from the 409 body: the polled status may be up to 5 s old.
            let holder = running.isEmpty ? (status?.displayName ?? "a script") : running
            replacePrompt = ReplacePrompt(running: holder, requested: requested, background: background)
        }
    }

    // MARK: - Autorun

    func refreshAutorun() async {
        guard let client else { return }
        do {
            autorun = try await client.autorunStatus()
        } catch {
            report(error)
        }
    }

    func setAutorun(enabled: Bool, name: String?) async {
        guard let client else { return }
        do {
            try await client.setAutorun(enabled: enabled, name: name)
            await refreshAutorun()
        } catch {
            report(error)
        }
    }

    private func report(_ error: Error) {
        lastError = error.localizedDescription
        connection?.showToast(error.localizedDescription, type: .error)
    }
}
