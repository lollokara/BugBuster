import SwiftUI

/// Hub address for the phone (read side) and the device's own hub settings (write side, over HTTP or BLE).
/// Pushed from the Hub screen. The device routes are POSTed (also for reads) because ConnectionManager only has postJSON and it works over BLE.
struct HubSettingsView: View {
    @ObservedObject var model: HubStatusModel
    var deviceID: String?
    @EnvironmentObject var connectionManager: ConnectionManager
    @AppStorage(HubSettings.urlKey) private var hubURL = ""
    @AppStorage(HubSettings.enabledKey) private var hubEnabled = true
    @State private var draft = ""
    @State private var addressError: String?
    @State private var deviceURL = ""
    @State private var deviceEnabled = true
    @State private var deviceStatus = ""
    @State private var message: String?
    @State private var saving = false
    @FocusState private var addressFocused: Bool

    private struct Cfg: Decodable { let ok: Bool?; let hub_url: String?; let hub_enabled: Bool?; let error: String? }
    private struct Stat: Decodable {
        struct Hub: Decodable { let ok: Bool; let url: String; let source: String; let backlog: Int; let last_push: Int }
        let hub: Hub?
    }

    var body: some View {
        Form {
            Section {
                Toggle("Use the hub", isOn: $hubEnabled)
                    .frame(minHeight: 44)
                    .onChange(of: hubEnabled) { _ in applyAndRefresh() }
                if hubEnabled {
                    TextField(HubSettings.defaultURL, text: $draft)
                        .textInputAutocapitalization(.never).autocorrectionDisabled().keyboardType(.URL)
                        .submitLabel(.done).focused($addressFocused).frame(minHeight: 44)
                        .onSubmit { commitAddress() }
                        .accessibilityLabel("Hub address")
                        .accessibilityIdentifier("hub-address")
                    if let addressError { Label(addressError, systemImage: "exclamationmark.triangle").font(.footnote).foregroundStyle(.orange) }
                }
            } header: { Text("This phone") } footer: {
                Text("Used to read devices, battery runs and logs. Empty means \(HubSettings.defaultURL). The phone must be on the same Wi-Fi as the hub.")
            }
            Section {
                TextField("Hub URL (empty = auto-discover)", text: $deviceURL)
                    .textInputAutocapitalization(.never).autocorrectionDisabled().frame(minHeight: 44)
                Toggle("Stream logs and battery runs", isOn: $deviceEnabled).frame(minHeight: 44)
                if !deviceStatus.isEmpty { Text(deviceStatus).font(.footnote).foregroundStyle(.secondary) }
                Button { Task { await save() } } label: {
                    HStack { Text("Save on BugBuster"); if saving { Spacer(); ProgressView() } }.frame(minHeight: 44)
                }
                .disabled(saving)
                if let message { Text(message).font(.footnote).foregroundStyle(.red) }
            } header: { Text("BugBuster") } footer: {
                Text("Where the board pushes its logs and runs. Needs a connection to the board.")
            }
        }
        .navigationTitle("Hub settings")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { draft = hubURL }
        .onDisappear { commitAddress() }
        .task { await load() }
    }

    private func commitAddress() {
        guard hubEnabled else { return }
        switch HubAddress.parse(draft) {
        case .empty: hubURL = ""; addressError = nil
        case .valid(let u): hubURL = u.absoluteString; draft = u.absoluteString; addressError = nil
        case .invalid(let why): addressError = why; return
        }
        applyAndRefresh()
    }

    private func applyAndRefresh() {
        model.reloadFromSettings()
        Task { await model.refresh(deviceID: deviceID) }
    }

    private func load() async {
        if let c: Cfg = await connectionManager.postJSON(Cfg.self, path: "/api/hub/config", json: [:]), c.ok == true {
            deviceURL = c.hub_url ?? ""
            deviceEnabled = c.hub_enabled ?? true
        }
        if let s: Stat = await connectionManager.postJSON(Stat.self, path: "/api/hub/status", json: [:]), let h = s.hub {
            deviceStatus = (h.ok ? "Connected" : "Not connected") + " - \(h.url) (\(h.source)), backlog \(h.backlog)"
        }
    }

    private func save() async {
        message = nil; saving = true; defer { saving = false }
        let c: Cfg? = await connectionManager.postJSON(Cfg.self, path: "/api/hub/config",
                                                       json: ["hub_url": deviceURL, "hub_enabled": deviceEnabled])
        if c?.ok != true { message = c?.error ?? "The board did not answer. Connect to it first." }
        await load()
    }
}
