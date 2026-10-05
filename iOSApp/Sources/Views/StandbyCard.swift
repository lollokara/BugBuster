import SwiftUI

/// Auto-standby timeout, state and Wake / Sleep controls (Diagnostics & Config).
struct StandbyCard: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @ObservedObject var standby: StandbyPresence

    @State private var status: StandbyStatus?
    @State private var busy = false
    @State private var note: String?
    @State private var bleUnsupported = false
    @State private var confirmSleep = false

    private var unsupported: Bool { bleUnsupported || standby.capability == .unsupported }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Standby")
                .font(.system(size: 18, weight: .bold))
                .foregroundColor(.blue)

            if unsupported {
                Text("This firmware has no system standby.")
                    .font(.system(size: 13))
                    .foregroundColor(.secondary)
            } else {
                row("STATE", status?.stateLabel ?? "--")
                row("TIMER", status?.idleLine ?? "--")

                VStack(alignment: .leading, spacing: 6) {
                    Text("AUTO STANDBY")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundColor(.secondary)
                    HStack(spacing: 8) {
                        ForEach(StandbyTimeout.allCases) { option in
                            Button(option.label) { setTimeout(option) }
                                .font(.system(size: 13, weight: .medium))
                                .padding(.horizontal, 12)
                                .padding(.vertical, 8)
                                .glassEffect(
                                    status?.timeoutSeconds == option.rawValue ? .regular.tint(.blue) : .regular,
                                    in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                                .disabled(busy)
                                .accessibilityIdentifier("standby_timeout_\(option.rawValue)")
                        }
                    }
                }

                HStack(spacing: 10) {
                    Button("Wake") { wake() }
                        .disabled(busy)
                    Button("Sleep now") { confirmSleep = true }
                        .disabled(busy)
                }
                .font(.system(size: 13, weight: .medium))

                if standby.released {
                    Text("This app released its connection and will not reconnect until you tap Wake.")
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                }
                if let note {
                    Text(note)
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)
                }
            }
        }
        .cardStyle()
        .task { await pollWhileVisible() }
        .confirmationDialog("Enter standby now?", isPresented: $confirmSleep, titleVisibility: .visible) {
            Button("Enter Standby", role: .destructive) { sleep() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("VADJ1/VADJ2, VDUT and the analog rails switch off and all MUX routes open. Outputs stay off after wake.")
        }
    }

    private func row(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.secondary)
            Spacer()
            Text(value)
                .font(.system(size: 13, weight: .bold, design: .monospaced))
        }
    }

    private func pollWhileVisible() async {
        while !Task.isCancelled && !unsupported {
            await refresh()
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
    }

    private func refresh() async {
        do {
            let s: StandbyStatus = try await connectionManager.getRequest(path: "/api/standby/status")
            status = s
        } catch ConnectionAPIError.bleRejected(_, let message) where message == "unknown path" {
            bleUnsupported = true
        } catch {
            // Transient (or an HTTP 404 the presence session reports as unsupported): keep the last status.
        }
    }

    private func apply(_ path: String, _ json: [String: Any], refused: String) {
        busy = true
        note = nil
        Task {
            let s: StandbyStatus? = await connectionManager.postJSON(StandbyStatus.self, path: path, json: json)
            await MainActor.run {
                busy = false
                if let s { status = s } else { note = refused }
            }
        }
    }

    private func setTimeout(_ option: StandbyTimeout) {
        apply("/api/standby/policy", ["timeoutSeconds": option.rawValue],
              refused: "The device did not accept the timeout (pair this app first).")
    }

    private func wake() {
        standby.attach()
        apply("/api/standby/wake", [:], refused: "Wake request failed.")
    }

    private func sleep() {
        standby.detach()
        apply("/api/standby/sleep", [:],
              refused: "The device did not enter standby: another client or running work still owns it.")
    }
}
