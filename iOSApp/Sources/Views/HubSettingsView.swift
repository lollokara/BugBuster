import SwiftUI

/// Hub URL for the phone (read side) and the device's own hub settings (write side, over HTTP or BLE).
/// The device routes are POSTed (also for reads) because ConnectionManager only has postJSON and it works over BLE.
struct HubSettingsView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @AppStorage(HubSettings.urlKey) private var hubURL = ""
    @State private var deviceURL = ""
    @State private var deviceEnabled = true
    @State private var deviceStatus = ""
    @State private var message: String?
    @Environment(\.dismiss) private var dismiss

    private struct Cfg: Decodable { let ok: Bool?; let hub_url: String?; let hub_enabled: Bool?; let error: String? }
    private struct Stat: Decodable {
        struct Hub: Decodable { let ok: Bool; let url: String; let source: String; let backlog: Int; let last_push: Int }
        let hub: Hub?
    }

    var body: some View {
        NavigationView {
            Form {
                Section("Phone") {
                    TextField(HubSettings.defaultURL, text: $hubURL).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Text("Used to read battery runs. Empty = \(HubSettings.defaultURL).").font(.caption).foregroundColor(.secondary)
                }
                Section("BugBuster") {
                    TextField("Hub URL (empty = auto-discover)", text: $deviceURL).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Toggle("Stream logs and battery runs", isOn: $deviceEnabled)
                    if !deviceStatus.isEmpty { Text(deviceStatus).font(.caption).foregroundColor(.secondary) }
                    Button("Save on device") { Task { await save() } }
                }
                if let message { Text(message).font(.caption).foregroundColor(.red) }
            }
            .navigationTitle("ESPFleet hub")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .task { await load() }
        }
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
        message = nil
        let c: Cfg? = await connectionManager.postJSON(Cfg.self, path: "/api/hub/config",
                                                       json: ["hub_url": deviceURL, "hub_enabled": deviceEnabled])
        if c?.ok != true { message = c?.error ?? "The device did not answer" }
        await load()
    }
}
