import SwiftUI

/// A battery run held on the connected board, for matching against the hub's copy.
struct HubLocalRun: Equatable, Identifiable {
    var id: Int { runId }
    var runId: Int
    var createdEpoch: Int
    var bytes: Int
    var name: String
}

private extension View {
    func hubCard() -> some View {
        padding(14).frame(maxWidth: .infinity, alignment: .leading)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

/// Hub screen: reachability, devices, runs and logs held by the ESPFleet hub, plus settings.
struct HubView: View {
    @ObservedObject var model: HubStatusModel
    let mac: String
    var localRuns: [HubLocalRun]
    /// Opens a run of the connected board in the chart (hub data merges in); nil when nothing can be opened.
    var onOpenRun: ((Int) -> Void)?
    @EnvironmentObject var connectionManager: ConnectionManager
    @Environment(\.dismiss) private var dismiss
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var showSettings = false
    @State private var showOtherDevices = false
    #if DEBUG
    @State private var debugRun: HubRunInfo?      // BB_BS_HUB=run opens the newest run for screenshots
    #endif

    private var deviceID: String? { mac.isEmpty ? nil : HubDevice.normalizedMac(mac) }
    private var firstLoad: Bool { model.reachability == .online && !model.loadedOnce }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    headerCard
                    switch model.reachability {
                    case .notConfigured: notConfiguredCard
                    case .offline: offlineCard
                    case .checking, .online: content
                    }
                }
                .frame(maxWidth: 760)
                .padding(16)
                .frame(maxWidth: .infinity)
            }
            .refreshable { await model.refresh(deviceID: deviceID) }
            .navigationTitle("ESPFleet hub")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button { showSettings = true } label: { Image(systemName: "gearshape") }
                        .frame(minWidth: 44, minHeight: 44)
                        .accessibilityLabel("Hub settings")
                        .accessibilityIdentifier("hub-settings")
                }
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() }.frame(minHeight: 44) }
            }
            .navigationDestination(isPresented: $showSettings) {
                HubSettingsView(model: model, deviceID: deviceID).environmentObject(connectionManager)
            }
            .task {
                await model.refresh(deviceID: deviceID)
                #if DEBUG
                if ProcessInfo.processInfo.environment["BB_BS_HUB"] == "run" { debugRun = model.runs.first }
                #endif
            }
            #if DEBUG
            .navigationDestination(item: $debugRun) { r in
                HubRunDetailView(model: model, run: r, local: localRun(for: r), onOpen: nil)
            }
            #endif
        }
        .presentationDetents([.large])
    }

    // MARK: Header

    private var headerCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Circle().fill(model.reachability == .notConfigured ? Color.secondary.opacity(0.5) : model.reachability.tint)
                    .frame(width: 12, height: 12)
                VStack(alignment: .leading, spacing: 2) {
                    Text(stateTitle).font(.headline)
                    Text(model.url?.absoluteString ?? "Hub turned off").font(.subheadline.monospaced())
                        .foregroundStyle(.secondary).lineLimit(1).minimumScaleFactor(0.7)
                }
                Spacer(minLength: 0)
                if model.reachability == .checking || model.loading { ProgressView().controlSize(.small) }
            }
            .accessibilityElement(children: .combine)
            .accessibilityValue(model.reachability.accessibilityValue)
            Divider().opacity(0.4)
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 96), alignment: .leading)], alignment: .leading, spacing: 8) {
                fact("Version", model.health?.version.map { "v\($0)" } ?? "—")
                fact("Latency", model.latency.map(HubFormat.latency) ?? "—")
                fact("Last sync", model.lastSync.map { $0.formatted(.relative(presentation: .named)) } ?? "—")
            }
        }
        .hubCard()
    }

    private var stateTitle: String {
        switch model.reachability {
        case .online: return "Hub online"
        case .offline: return "Hub offline"
        case .checking: return "Checking hub…"
        case .notConfigured: return "No hub"
        }
    }

    private func fact(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.subheadline.weight(.medium)).monospacedDigit().lineLimit(1).minimumScaleFactor(0.8)
        }
        .accessibilityElement(children: .combine)
    }

    // MARK: Problem states

    private var notConfiguredCard: some View {
        problemCard(icon: "network.slash", title: "The hub is switched off",
                    text: "BugBuster keeps its own battery history either way. Turn the hub on to see devices, stored runs and logs, and to fill gaps in the chart with its higher-resolution data.",
                    button: "Open settings") { showSettings = true }
    }

    private var offlineCard: some View {
        let f = model.failure ?? .unreachable
        return VStack(alignment: .leading, spacing: 10) {
            problemCard(icon: "wifi.exclamationmark", title: f.message, text: f.fix, button: "Check again") {
                Task { await model.refresh(deviceID: deviceID) }
            }
            Button { showSettings = true } label: {
                Label("Edit hub address", systemImage: "pencil").frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(BsButtonStyle(tint: .white))
        }
    }

    private func problemCard(icon: String, title: String, text: String, button: String, action: @escaping () -> Void) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(title, systemImage: icon).font(.subheadline.weight(.semibold)).foregroundStyle(.orange)
            Text(text).font(.subheadline).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Button(action: action) { Text(button).frame(maxWidth: .infinity, minHeight: 44) }
                .buttonStyle(BsButtonStyle(tint: .cyan))
        }
        .hubCard()
    }

    // MARK: Content

    @ViewBuilder private var content: some View {
        if let f = model.dataFailure {
            problemCard(icon: "exclamationmark.triangle", title: f.message, text: f.fix, button: "Try again") {
                Task { await model.refresh(deviceID: deviceID) }
            }
        }
        if sizeClass == .regular {
            HStack(alignment: .top, spacing: 14) {
                VStack(spacing: 14) { boardCard; devicesCard; logsCard }.frame(maxWidth: .infinity)
                VStack(spacing: 14) { runsCard }.frame(maxWidth: .infinity)
            }
        } else {
            boardCard; runsCard; devicesCard; logsCard
        }
    }

    private func sectionTitle(_ t: String, count: Int? = nil) -> some View {
        HStack {
            Text(t).font(.headline).accessibilityAddTraits(.isHeader)
            if let count { Text("\(count)").font(.subheadline).foregroundStyle(.secondary) }
            Spacer(minLength: 0)
        }
    }

    private var boardCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionTitle("This BugBuster")
            if firstLoad { skeletonRows(1) }
            else if let d = model.device(forMac: mac) { deviceRow(d, highlighted: true) }
            else {
                Text(mac.isEmpty ? "Connect to a BugBuster to see how the hub knows it."
                                 : "The hub has not seen this board yet. It registers on its own once the board is on the same Wi-Fi as the hub.")
                    .font(.subheadline).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .hubCard()
    }

    private var devicesCard: some View {
        let fleet = model.devices.filter { $0.isBugBuster && !$0.matches(mac: mac) }
        let others = model.devices.filter { !$0.isBugBuster }
        return VStack(alignment: .leading, spacing: 10) {
            sectionTitle("Devices", count: model.devices.isEmpty ? nil : model.devices.count)
            if firstLoad { skeletonRows(2) }
            else if model.devices.isEmpty { Text("No devices yet.").font(.subheadline).foregroundStyle(.secondary) }
            else {
                ForEach(fleet) { deviceRow($0, highlighted: false) }
                if fleet.isEmpty { Text("No other BugBuster boards.").font(.subheadline).foregroundStyle(.secondary) }
                if !others.isEmpty {
                    DisclosureGroup(isExpanded: $showOtherDevices) {
                        VStack(spacing: 8) { ForEach(others) { deviceRow($0, highlighted: false) } }.padding(.top, 6)
                    } label: {
                        Text("\(others.count) other devices on the network").font(.subheadline)
                            .frame(minHeight: 44, alignment: .leading)
                    }
                }
            }
        }
        .hubCard()
    }

    private func deviceRow(_ d: HubDevice, highlighted: Bool) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Circle().fill(d.isOnline ? Color.green : Color.secondary.opacity(0.6)).frame(width: 9, height: 9).padding(.top, 6)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(d.name).font(.subheadline.weight(.semibold)).lineLimit(1)
                    if highlighted {
                        Text("Connected").font(.caption2.weight(.bold)).foregroundStyle(.black)
                            .padding(.horizontal, 6).padding(.vertical, 2).background(Capsule().fill(Color.cyan))
                    }
                }
                Text(deviceDetail(d)).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                if d.isBugBuster { Text(d.id).font(.caption2.monospaced()).foregroundStyle(.tertiary) }
            }
            Spacer(minLength: 0)
        }
        .padding(highlighted ? 10 : 0)
        .background(highlighted ? RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.cyan.opacity(0.12)) : nil)
        .overlay(highlighted ? RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(Color.cyan.opacity(0.55), lineWidth: 1) : nil)
        .frame(minHeight: 44)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(d.name), \(d.isOnline ? "online" : d.status)\(highlighted ? ", this BugBuster" : ""), \(deviceDetail(d))")
    }

    private func deviceDetail(_ d: HubDevice) -> String {
        var parts = [d.isOnline ? "Online" : d.status.capitalized]
        if !d.isOnline, let t = d.lastSeen {
            parts[0] += ", seen \(Date(timeIntervalSince1970: t).formatted(.relative(presentation: .named)))"
        }
        if let fw = d.firmware { parts.append(fw) }
        if let ip = d.ip { parts.append(ip) }
        return parts.joined(separator: " · ")
    }

    private var runsCard: some View {
        let mine = model.runs(forMac: mac), other = model.otherRuns(forMac: mac)
        return VStack(alignment: .leading, spacing: 10) {
            sectionTitle("Data on the hub", count: model.runs.isEmpty ? nil : model.runs.count)
            if firstLoad { skeletonRows(3) }
            else if model.runs.isEmpty {
                Text("The hub holds no battery runs yet. Runs appear here as soon as the board pushes them.")
                    .font(.subheadline).foregroundStyle(.secondary)
            } else {
                if !mine.isEmpty { Text("This BugBuster").font(.caption.weight(.semibold)).foregroundStyle(.secondary); ForEach(mine) { runRow($0) } }
                if !other.isEmpty { Text("Other devices").font(.caption.weight(.semibold)).foregroundStyle(.secondary).padding(.top, mine.isEmpty ? 0 : 4); ForEach(other) { runRow($0) } }
            }
        }
        .hubCard()
    }

    private func localRun(for r: HubRunInfo) -> HubLocalRun? {
        guard let key = HubRunUid.parse(r.uid), key.mac == HubDevice.normalizedMac(mac) else { return nil }
        return localRuns.first { $0.runId == key.runId && $0.createdEpoch == key.createdEpoch }
    }

    private func runRow(_ r: HubRunInfo) -> some View {
        NavigationLink {
            HubRunDetailView(model: model, run: r, local: localRun(for: r), onOpen: onOpenRun.map { open in
                { id in dismiss(); open(id) }
            })
        } label: {
            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Circle().fill(r.state == "running" ? Color.green : Color.secondary.opacity(0.6)).frame(width: 8, height: 8)
                        Text(r.name.isEmpty ? "Run #\(r.runID)" : r.name).font(.subheadline.weight(.semibold)).lineLimit(1)
                        if localRun(for: r) != nil { Image(systemName: "externaldrive.fill").font(.caption2).foregroundStyle(.cyan).accessibilityLabel("Also on this device") }
                    }
                    Text(runSummary(r)).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right").font(.caption.weight(.semibold)).foregroundStyle(.tertiary)
            }
            .frame(minHeight: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(r.name.isEmpty ? "Run \(r.runID)" : r.name), \(runSummary(r))")
        .accessibilityHint("Shows what the hub holds for this run")
    }

    private func runSummary(_ r: HubRunInfo) -> String {
        var p = [Date(timeIntervalSince1970: r.startedAt).formatted(date: .abbreviated, time: .shortened)]
        if let d = r.duration { p.append(HubFormat.duration(d)) }
        if let n = r.points { p.append("\(HubFormat.points(n)) pts") }
        return p.joined(separator: " · ")
    }

    private var logsCard: some View {
        let s = HubLogSummary(model.logs)
        return VStack(alignment: .leading, spacing: 8) {
            sectionTitle("Recent logs")
            if firstLoad { skeletonRows(2) }
            else if model.logs.isEmpty {
                Text(mac.isEmpty ? "Connect to a BugBuster to see its logs." : "No logs from this board on the hub.").font(.subheadline).foregroundStyle(.secondary)
            } else {
                HStack(spacing: 8) {
                    logPill("\(s.errors) errors", .red); logPill("\(s.warnings) warnings", .orange); logPill("\(s.other) info", .secondary)
                }
                .accessibilityElement(children: .combine)
                Text("Latest \(s.total) entries from this board").font(.caption).foregroundStyle(.secondary)
                ForEach(model.logs.prefix(5)) { l in
                    HStack(alignment: .top, spacing: 6) {
                        Text(l.level).font(.caption.monospaced().weight(.bold)).foregroundStyle(levelColor(l.level)).frame(width: 14)
                        Text(l.msg).font(.caption).lineLimit(2)
                    }
                    .accessibilityElement(children: .combine)
                }
            }
        }
        .hubCard()
    }

    private func logPill(_ t: String, _ c: Color) -> some View {
        Text(t).font(.caption.weight(.semibold)).foregroundStyle(c)
            .padding(.horizontal, 8).padding(.vertical, 4).background(Capsule().fill(c.opacity(0.15)))
    }

    private func levelColor(_ l: String) -> Color { l == "E" ? .red : l == "W" ? .orange : .secondary }

    private func skeletonRows(_ n: Int) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(0..<n, id: \.self) { _ in
                VStack(alignment: .leading, spacing: 4) {
                    Text("Placeholder device name").font(.subheadline.weight(.semibold))
                    Text("Online · firmware 1.2.3 · 192.168.0.1").font(.caption)
                }
            }
        }
        .redacted(reason: .placeholder)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Loading")
    }
}
