import SwiftUI

/// Battery simulator (DAQ HAT): live status + controls, run browser, a zoomable
/// V / I / P / SOC history with a scrub cursor and navigator, and window statistics.
struct BatterySimTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var status: BsStatus?
    @State private var runs: [BsRunSummary] = []
    @State private var cached: [BsCachedRun] = []
    @State private var history: BsHistory?
    @State private var openFromDevice = false
    @State private var window: ClosedRange<Double> = 0...1
    @State private var busy: String?
    @State private var message: String?
    @State private var logCurrent = true
    @State private var wallClock = false
    @State private var follow = true
    @State private var showNew = false
    @State private var pendingDelete: Int?
    @State private var pollTask: Task<Void, Never>?
    @State private var liveTail: [BsRec] = []
    @State private var syncing = false
    @State private var scrubbing = false

    private var wide: Bool { sizeClass == .regular }

    /// Synced history plus status-poll points newer than the last stored record.
    private var displayed: BsHistory? {
        guard var h = history else { return nil }
        let last = h.recs.last?.t ?? -1
        h.recs += liveTail.filter { $0.t > last }
        return h
    }
    private var client: BattSimClient { BattSimClient(connectionManager) }
    private static let presets: [(String, Double)] = [("1h", 3600), ("6h", 21600), ("1d", 86400), ("7d", 604800), ("30d", 2_592_000), ("All", 0)]

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                liveCard
                if let message { errorBanner(message) }
                chartCard
                if wide {
                    HStack(alignment: .top, spacing: 14) {
                        statsCard.frame(maxWidth: .infinity)
                        runsCard.frame(width: 360)
                    }
                } else {
                    statsCard
                    runsCard
                }
            }
            .padding(.horizontal, wide ? 18 : 12)
            .padding(.top, 12)
            .padding(.bottom, 110)
        }
        .scrollDisabled(scrubbing)
        .onAppear { startPolling() }
        .onDisappear { pollTask?.cancel() }
        .sheet(isPresented: $showNew) {
            NewBatteryRunSheet { cfg in await createRun(cfg) }
        }
        .confirmationDialog("Delete run from the device?", isPresented: Binding(
            get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }), titleVisibility: .visible) {
            Button("Delete run #\(pendingDelete ?? 0)", role: .destructive) {
                if let id = pendingDelete { Task { await act(.delete, run: id) } }
            }
        } message: { Text("The copy on this device (if any) is kept.") }
    }

    // MARK: Live status

    private var liveCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: "battery.75percent").foregroundColor(.green)
                Text("Battery simulator").font(.system(size: 16, weight: .bold))
                if let s = status { stateBadge(s.state) }
                Spacer(minLength: 6)
                if let s = status, s.state != 0 {
                    Text("#\(s.runId)  \(BattSim.chemNames[safe: s.chem] ?? "?") \(s.cells)S \(s.capacityMah) mAh")
                        .font(.system(size: 12)).foregroundColor(.secondary).lineLimit(1)
                }
            }
            if let s = status, s.state != 0 {
                socBar(s.socPct)
                LazyVGrid(columns: [GridItem(.adaptive(minimum: wide ? 128 : 96), spacing: 8)], spacing: 8) {
                    readout("Voltage", BattSim.si(s.vMeas, "V"))
                    readout("Current", BattSim.si(s.iMeas, "A"))
                    readout("Target", BattSim.si(s.vTarget, "V"))
                    readout("Elapsed", BattSim.duration(Double(s.elapsedS)))
                    readout("Remaining", s.remainingS.map { (s.provisional ? "~" : "") + BattSim.duration(Double($0)) } ?? "-")
                    if let e = s.eDutJ { readout("Energy", BattSim.si(e / 3600, "Wh")) }
                    readout("Storage", "\(s.fsUsed / 1024) / \(s.fsTotal / 1024) KiB",
                            warn: s.fsTotal > 0 && s.fsTotal - s.fsUsed < 2_600_000)
                }
            } else {
                Text(status == nil ? "Battery simulator not reachable." : "No run loaded. Create a new run or load one from the list.")
                    .font(.system(size: 12)).foregroundColor(.secondary)
            }
            controls
        }
        .bsCard()
    }

    private var controls: some View {
        let st = status?.state ?? 0
        return HStack(spacing: 8) {
            actionButton("Start", "play.fill", .green, prominent: true, enabled: st == 1) { await act(.start) }
            actionButton("Pause", "pause.fill", .yellow, enabled: st == 2) { await act(.pause) }
            actionButton("Stop", "stop.fill", .red, enabled: st == 1 || st == 2) { await act(.stop) }
            actionButton("Unload", "eject.fill", .gray, enabled: st != 0) { await act(.unload) }
            if wide { Spacer() }
            actionButton("New run", "plus", .cyan, prominent: true, enabled: st != 2) { showNew = true }
        }
    }

    // MARK: Chart

    private var chartCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            toolbar
            ZStack {
                if let h = displayed {
                    BatteryHistoryChart(history: h, window: $window, scrubbing: $scrubbing, logCurrent: logCurrent,
                                        wallEpoch: wallClock && h.meta.createdEpoch > 0 ? Double(h.meta.createdEpoch) : nil,
                                        compact: !wide)
                } else {
                    VStack(spacing: 8) {
                        Image(systemName: "chart.xyaxis.line").font(.system(size: 28)).foregroundColor(.secondary)
                        Text(busy ?? "Pick a run to open its history.").font(.system(size: 13)).foregroundColor(.secondary)
                    }
                }
                if history != nil, let busy {
                    Text(busy).font(.system(size: 11, weight: .medium)).padding(.horizontal, 10).padding(.vertical, 5)
                        .background(.ultraThinMaterial, in: Capsule())
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                        .allowsHitTesting(false)
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: wide ? 520 : 380)
            if let h = displayed {
                BsNavigator(history: h, window: $window, scrubbing: $scrubbing, logCurrent: logCurrent).frame(height: wide ? 46 : 40)
                Text("Drag on the chart to read values. Pinch to zoom, drag the strip to pan, double-tap for the whole run.")
                    .font(.system(size: 10)).foregroundColor(.secondary)
            }
        }
        .bsCard()
    }

    @ViewBuilder private var toolbar: some View {
        if wide {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 10) {
                    runPicker.frame(maxWidth: 380)
                    pointsInfo
                    Spacer()
                    zoomButtons
                    exportMenu
                }
                HStack(spacing: 10) {
                    presetBar
                    Spacer()
                    toggles
                }
            }
        } else {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) { runPicker; exportMenu }
                presetBar
                HStack(spacing: 6) { toggles; Spacer(minLength: 4); zoomButtons }
            }
        }
    }

    private var runPicker: some View {
        Menu {
            Section("On device") {
                if runs.isEmpty { Text("No runs stored") }
                ForEach(runs.reversed()) { r in
                    Button { Task { await open(r.runId) } } label: {
                        Label("#\(r.runId) \(r.meta?.name ?? "")\(r.active ? "  (loaded)" : "")",
                              systemImage: isOpen(r) ? "checkmark" : "externaldrive")
                    }
                }
            }
            if !cached.isEmpty {
                Section("Saved on this device") {
                    ForEach(cached) { c in
                        Button { openCached(c) } label: {
                            Label("#\(c.meta.runId) \(c.meta.name) - \(dateText(c.meta.createdEpoch))", systemImage: "internaldrive")
                        }
                    }
                }
            }
        } label: {
            HStack(spacing: 8) {
                Image(systemName: "folder").foregroundColor(.cyan)
                Text(history.map { "Run #\($0.meta.runId)  \($0.meta.name)" } ?? "Open a run...")
                    .font(.system(size: 13, weight: .semibold)).foregroundColor(.primary).lineLimit(1)
                Spacer(minLength: 4)
                Image(systemName: "chevron.up.chevron.down").font(.system(size: 10, weight: .semibold)).foregroundColor(.secondary)
            }
            .padding(.horizontal, 12)
            .frame(maxWidth: .infinity, minHeight: wide ? 38 : 34)
            .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.07)))
            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(Color.white.opacity(0.14), lineWidth: 1))
        }
    }

    @ViewBuilder private var pointsInfo: some View {
        if let h = history {
            Text("\(h.recs.count) points\(h.meta.version < 2 ? " - energy estimated (format 1)" : "")")
                .font(.system(size: 11)).foregroundColor(.secondary).lineLimit(1)
        }
    }

    private var presetBar: some View {
        HStack(spacing: 2) {
            ForEach(Self.presets, id: \.0) { p in
                let sel = presetSelected(p.1)
                let usable = history != nil && (p.1 <= 0 || p.1 < fullSpan)
                Button { preset(p.1) } label: {
                    Text(p.0).font(.system(size: 12, weight: .semibold))
                        .frame(minWidth: 40, maxWidth: wide ? nil : .infinity, minHeight: wide ? 32 : 30)
                        .padding(.horizontal, wide ? 6 : 0)
                        .foregroundColor(sel ? .black : usable ? .primary : Color.white.opacity(0.3))
                        .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(sel ? Color.cyan : .clear))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!usable)
            }
        }
        .padding(3)
        .background(RoundedRectangle(cornerRadius: 11, style: .continuous).fill(Color.white.opacity(0.06)))
        .overlay(RoundedRectangle(cornerRadius: 11, style: .continuous).stroke(Color.white.opacity(0.10), lineWidth: 1))
        .animation(.easeOut(duration: 0.15), value: window)
    }

    private var toggles: some View {
        HStack(spacing: 6) {
            chip("Log I", "waveform.path.ecg", $logCurrent)
            chip("Clock", "clock", $wallClock)
            chip("Follow", "arrow.right.to.line", $follow)
        }
    }

    private var zoomButtons: some View {
        HStack(spacing: 6) {
            Button { zoom(2) } label: { Image(systemName: "minus.magnifyingglass") }
                .buttonStyle(BsButtonStyle(tint: .white, compact: !wide)).accessibilityLabel("Zoom out")
            Button { zoom(0.5) } label: { Image(systemName: "plus.magnifyingglass") }
                .buttonStyle(BsButtonStyle(tint: .white, compact: !wide)).accessibilityLabel("Zoom in")
        }
        .disabled(history == nil)
    }

    private var exportMenu: some View {
        Menu {
            if let h = history {
                ShareLink(item: BsExportFile(history: h, csv: true), preview: SharePreview("Run #\(h.meta.runId) CSV")) {
                    Label("CSV (all records)", systemImage: "tablecells")
                }
                ShareLink(item: BsExportFile(history: h, csv: false), preview: SharePreview("Run #\(h.meta.runId) JSON")) {
                    Label("JSON (meta, events, records)", systemImage: "curlybraces")
                }
            }
        } label: {
            HStack(spacing: 6) {
                Image(systemName: "square.and.arrow.up")
                if wide { Text("Export") }
            }
                .font(.system(size: 13, weight: .semibold))
                .foregroundColor(history == nil ? Color.white.opacity(0.3) : .cyan)
                .padding(.horizontal, 12)
                .frame(minHeight: wide ? 38 : 34)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.cyan.opacity(history == nil ? 0.03 : 0.14)))
                .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(Color.cyan.opacity(history == nil ? 0.1 : 0.55), lineWidth: 1))
        }
        .disabled(history == nil)
    }

    // MARK: Statistics

    private var statsCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text("Window statistics").font(.system(size: 15, weight: .bold))
                Spacer()
                if let h = displayed, let s = BattSim.stats(h, window.lowerBound, window.upperBound) {
                    Text("\(BattSim.duration(s.duration)) from +\(BattSim.duration(s.tStart)) - \(s.points) points")
                        .font(.system(size: 11)).foregroundColor(.secondary).lineLimit(1)
                }
            }
            if let h = displayed, let s = BattSim.stats(h, window.lowerBound, window.upperBound) {
                let cols = wide ? [GridItem(.adaptive(minimum: 180), spacing: 10)]
                    : [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)]
                let duty = s.iMax > 0 ? String(format: "%.2f %%", s.iAvg / s.iMax * 100) : "-"
                let evs = h.events.filter { window.contains($0.t) }
                LazyVGrid(columns: cols, alignment: .leading, spacing: wide ? 10 : 8) {
                    tile("Voltage", .green, [("min", BattSim.si(s.vMin, "V")), ("avg", BattSim.si(s.vAvg, "V")), ("max", BattSim.si(s.vMax, "V"))])
                    tile("Current", .orange, [("min", BattSim.si(s.iMin, "A")), ("avg", BattSim.si(s.iAvg, "A")), ("max", BattSim.si(s.iMax, "A"))])
                    tile("Current profile", .orange, [("baseline P10", BattSim.si(s.iP10, "A")), ("median", BattSim.si(s.iMedian, "A")), ("busy P99", BattSim.si(s.iP99, "A"))])
                    tile("Power", .purple, [("avg", BattSim.si(s.pAvg, "W")), ("peak", BattSim.si(s.pMax, "W")), ("duty avg/peak", duty)])
                    tile("Consumed", .purple, [("charge", BattSim.si(s.chargeC / 3600, "Ah")),
                                               (s.energyEstimated ? "energy (est.)" : "energy", BattSim.si(s.energyJ / 3600, "Wh"))])
                    tile("State of charge", .cyan, [("start", String(format: "%.2f %%", s.socStart)), ("end", String(format: "%.2f %%", s.socEnd)),
                                                    ("rate", String(format: "%.3f %%/day", s.socRatePerDay))])
                    tile("Projection at avg", .cyan, [("full battery", BattSim.duration(s.projectedLifeS)), ("remaining", BattSim.duration(s.remainingAtAvgS))])
                    if s.gaps > 0 || s.clamped > 0 {
                        tile("Data quality", .gray, [("gaps", "\(s.gaps)"), ("clamped", "\(s.clamped)")])
                    }
                    if !evs.isEmpty {
                        tile("Events in window", .pink, evs.prefix(4).map { ($0.name, "+" + BattSim.duration($0.t)) })
                    }
                }
            } else {
                Text("Open a run to see statistics for the visible window.").font(.system(size: 12)).foregroundColor(.secondary)
            }
        }
        .bsCard()
    }

    // MARK: Runs

    private var runsCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Runs on device").font(.system(size: 15, weight: .bold))
                Spacer()
                Button { Task { await refreshRuns() } } label: { Image(systemName: "arrow.clockwise") }
                    .buttonStyle(BsButtonStyle(tint: .white, compact: true)).accessibilityLabel("Refresh")
            }
            if runs.isEmpty { Text("No runs stored.").font(.system(size: 12)).foregroundColor(.secondary) }
            ForEach(runs.reversed()) { r in runRow(r) }
        }
        .bsCard()
    }

    private func runRow(_ r: BsRunSummary) -> some View {
        let sel = isOpen(r)
        return HStack(spacing: 10) {
            RoundedRectangle(cornerRadius: 2).fill(sel ? Color.cyan : Color.clear).frame(width: 3)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text("#\(r.runId)").font(.system(size: 13, weight: .bold, design: .monospaced))
                    Text(r.meta?.name ?? "").font(.system(size: 13)).lineLimit(1)
                    if r.active {
                        Text("LOADED").font(.system(size: 9, weight: .bold)).foregroundColor(.green)
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(Capsule().fill(Color.green.opacity(0.15)))
                    }
                }
                Text(runSubtitle(r)).font(.system(size: 11)).foregroundColor(.secondary).lineLimit(1)
            }
            Spacer(minLength: 4)
            Button { Task { await act(.load, run: r.runId) } } label: { Text("Load") }
                .buttonStyle(BsButtonStyle(tint: .cyan, compact: true)).disabled(r.active)
            Button { pendingDelete = r.runId } label: { Image(systemName: "trash") }
                .buttonStyle(BsButtonStyle(tint: .red, compact: true)).disabled(r.active).accessibilityLabel("Delete")
        }
        .padding(.vertical, 8).padding(.trailing, 8)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(sel ? Color.cyan.opacity(0.12) : Color.white.opacity(0.03)))
        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(sel ? Color.cyan.opacity(0.75) : Color.white.opacity(0.06), lineWidth: sel ? 1.5 : 1))
        .contentShape(Rectangle())
        .onTapGesture { Task { await open(r.runId) } }
        .animation(.easeOut(duration: 0.15), value: sel)
    }

    // MARK: Pieces

    private func stateBadge(_ st: Int) -> some View {
        let c = stateColor(st)
        return Text(BattSim.stateNames[safe: st] ?? "?").font(.system(size: 11, weight: .bold)).foregroundColor(c)
            .padding(.horizontal, 9).padding(.vertical, 3)
            .background(Capsule().fill(c.opacity(0.18)))
            .overlay(Capsule().stroke(c.opacity(0.5), lineWidth: 1))
    }

    private func socBar(_ soc: Double) -> some View {
        let f = min(max(soc / 100, 0), 1)
        let c: Color = soc > 50 ? .green : soc > 20 ? .yellow : .red
        return ZStack(alignment: .leading) {
            RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.white.opacity(0.07))
            GeometryReader { g in
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(LinearGradient(colors: [c.opacity(0.35), c.opacity(0.7)], startPoint: .leading, endPoint: .trailing))
                    .frame(width: g.size.width * f)
            }
            Text(String(format: "SOC  %.1f %%", soc)).font(.system(size: 12, weight: .semibold, design: .monospaced))
                .frame(maxWidth: .infinity)
        }
        .frame(height: wide ? 26 : 22)
    }

    private func readout(_ label: String, _ value: String, warn: Bool = false) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label.uppercased()).font(.system(size: 9, weight: .semibold)).foregroundColor(.secondary)
            Text(value).font(.system(size: wide ? 16 : 13, weight: .semibold, design: .monospaced))
                .foregroundColor(warn ? .orange : .primary).lineLimit(1).minimumScaleFactor(0.6)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 10).padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(Color.white.opacity(0.05)))
    }

    private func tile(_ title: String, _ color: Color, _ rows: [(String, String)]) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 6) {
                RoundedRectangle(cornerRadius: 1.5).fill(color).frame(width: 3, height: 12)
                Text(title.uppercased()).font(.system(size: 10, weight: .bold)).foregroundColor(.secondary).lineLimit(1)
            }
            ForEach(rows.indices, id: \.self) { i in
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(rows[i].0).font(.system(size: 11)).foregroundColor(.secondary).lineLimit(1)
                    Spacer(minLength: 4)
                    Text(rows[i].1).font(.system(size: wide ? 13 : 12, weight: .semibold, design: .monospaced))
                        .lineLimit(1).minimumScaleFactor(0.7)
                }
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.white.opacity(0.04)))
        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(color.opacity(0.25), lineWidth: 1))
    }

    private func chip(_ title: String, _ icon: String, _ on: Binding<Bool>) -> some View {
        Button { on.wrappedValue.toggle() } label: {
            Label(title, systemImage: on.wrappedValue ? "checkmark" : icon)
        }
        .buttonStyle(BsButtonStyle(tint: .cyan, on: on.wrappedValue, compact: !wide))
        .accessibilityAddTraits(on.wrappedValue ? .isSelected : [])
    }

    private func actionButton(_ title: String, _ icon: String, _ tint: Color, prominent: Bool = false,
                              enabled: Bool, _ run: @escaping () async -> Void) -> some View {
        Button { Task { await run() } } label: {
            if wide {
                Label(title, systemImage: icon).frame(minWidth: 92)
            } else {
                VStack(spacing: 3) {
                    Image(systemName: icon).font(.system(size: 14, weight: .semibold))
                    Text(title).font(.system(size: 10, weight: .semibold)).lineLimit(1)
                }
                .frame(maxWidth: .infinity, minHeight: 40)
            }
        }
        .buttonStyle(BsButtonStyle(tint: tint, on: prominent && enabled, compact: !wide))
        .disabled(!enabled)
        .accessibilityLabel(title)
    }

    private func errorBanner(_ m: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundColor(.orange)
            Text(m).font(.system(size: 12))
            Spacer()
            Button { message = nil } label: { Image(systemName: "xmark") }.buttonStyle(.plain).foregroundColor(.secondary)
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.orange.opacity(0.12)))
        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(Color.orange.opacity(0.4), lineWidth: 1))
    }

    private func stateColor(_ s: Int) -> Color { [Color.gray, .yellow, .green, .red, .gray][safe: s] ?? .gray }

    private func dateText(_ epoch: Int) -> String {
        epoch > 0 ? Date(timeIntervalSince1970: Double(epoch)).formatted(date: .abbreviated, time: .shortened) : "date unknown"
    }

    private func runSubtitle(_ r: BsRunSummary) -> String {
        let size = r.bytes >= 1 << 20 ? String(format: "%.2f MiB", Double(r.bytes) / 1_048_576) : "\(r.bytes / 1024) KiB"
        guard let m = r.meta else { return size }
        return "\(BattSim.chemNames[safe: m.params.chem] ?? "?") \(m.params.cells)S \(m.params.capacityMah) mAh - \(dateText(m.createdEpoch)) - \(size)"
    }

    private func isOpen(_ r: BsRunSummary) -> Bool {
        guard openFromDevice, let h = history else { return false }
        return h.meta.runId == r.runId && (r.meta.map { $0.createdEpoch == h.meta.createdEpoch } ?? true)
    }

    private var fullSpan: Double { displayed.map { $0.bounds.upperBound - $0.bounds.lowerBound } ?? 0 }

    private func presetSelected(_ secs: Double) -> Bool {
        guard let h = displayed else { return false }
        let b = h.bounds
        if secs <= 0 { return abs(window.lowerBound - b.lowerBound) < 1 && abs(window.upperBound - b.upperBound) < 1 }
        return secs < fullSpan && abs(window.upperBound - b.upperBound) < 1
            && abs((window.upperBound - window.lowerBound) - secs) < 1
    }

    // MARK: Actions

    private func startPolling() {
        pollTask?.cancel()
        cached = BattSim.cachedRuns()
        pollTask = Task {
            await client.setEpoch()
            await refreshRuns()
            var tick = 0
            while !Task.isCancelled {
                if let s = try? await client.status() {
                    status = s
                    if openFromDevice, s.state == 2, history?.meta.runId == s.runId {
                        appendLive(s)
                        // m1 gains a record per minute; only appended bytes are fetched.
                        if tick % 30 == 29, busy == nil, !syncing { await open(s.runId, quiet: true) }
                    }
                }
                tick += 1
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    private func refreshRuns() async {
        do { runs = try await client.listRuns() } catch { message = error.localizedDescription }
    }

    private func open(_ id: Int, quiet: Bool = false) async {
        let prev = displayed, prevWindow = window
        if quiet { syncing = true } else { busy = "Opening run #\(id)..."; message = nil }
        do {
            let files = try await client.syncRun(id) { name, done, total in
                if !quiet { Task { @MainActor in busy = "Downloading \(name) \(Int(Double(done) / Double(max(total, 1)) * 100)) %" } }
            }
            apply(try BattSim.buildHistory(files), previous: openFromDevice ? prev : nil, previousWindow: prevWindow)
            openFromDevice = true
            if !quiet { cached = BattSim.cachedRuns() }
        } catch { if !quiet { message = error.localizedDescription } }
        busy = nil
        syncing = false
    }

    private func appendLive(_ s: BsStatus) {
        guard let h = history else { return }
        let t = Double(s.elapsedS)
        let lastT = liveTail.last?.t ?? h.recs.last?.t ?? 0
        guard t > lastT else { return }
        let before = displayed?.bounds
        let pinned = follow && before.map { abs(window.upperBound - $0.upperBound) < 1 } ?? false
        // Gaps longer than this break the trace instead of being bridged.
        let maxDt: Double = liveTail.isEmpty ? 120 : 10
        liveTail.append(BsRec(t: t, dt: min(t - lastT, maxDt), vAvg: s.vMeas, vMin: s.vMeas, vMax: s.vMeas, soc: s.socPct,
                              iAvg: s.iMeas, iMin: s.iMeas, iMax: s.iMeas, flags: 0, qDut: nil, qUsed: 0,
                              eDut: s.eDutJ ?? h.recs.last?.eDut, tier: 2))
        if pinned, let d = displayed {
            let span = window.upperBound - window.lowerBound
            window = d.clampWindow(d.bounds.upperBound - span, d.bounds.upperBound)
        }
    }

    private func openCached(_ c: BsCachedRun) {
        do {
            apply(try BattSim.buildHistory(BattSim.loadCached(c.dir)), previous: nil, previousWindow: window)
            openFromDevice = false
        } catch { message = error.localizedDescription }
    }

    /// Keeps the window when the same run is re-synced; slides it to the new end when Follow is on and it was pinned there.
    private func apply(_ h: BsHistory, previous: BsHistory?, previousWindow: ClosedRange<Double>) {
        let same = previous.map { $0.meta.runId == h.meta.runId && $0.meta.createdEpoch == h.meta.createdEpoch } ?? false
        history = h
        let lastT = h.recs.last?.t ?? 0
        if same { liveTail.removeAll { $0.t <= lastT } } else { liveTail = [] }
        guard same, let previous else { window = h.bounds; return }
        let span = previousWindow.upperBound - previousWindow.lowerBound
        if follow && abs(previousWindow.upperBound - previous.bounds.upperBound) < 1 {
            window = h.clampWindow(h.bounds.upperBound - span, h.bounds.upperBound)
        } else {
            window = h.clampWindow(previousWindow.lowerBound, previousWindow.upperBound)
        }
    }

    private func act(_ a: BattSim.Action, run: Int? = nil) async {
        message = nil
        do {
            try await client.action(a, run: run)
            try? await Task.sleep(nanoseconds: 400_000_000)
            if let s = try? await client.status() {
                status = s
                if s.lastError != 0 { message = BattSim.errorText[safe: s.lastError] ?? "error \(s.lastError)" }
            }
            if [.newRun, .delete, .stop, .unload, .load].contains(a) { await refreshRuns() }
        } catch { message = error.localizedDescription }
    }

    private func createRun(_ c: NewBatteryRunSheet.Config) async {
        let cl = client
        do {
            try await cl.cfgSet(0x0801, .enumV, c.chem)
            try await cl.cfgSet(0x0802, .u8, c.cells)
            try await cl.cfgSet(0x0803, .u32, c.capacityMah)
            try await cl.action(.defaults)
            try await cl.cfgSet(0x0804, .u16, Int((c.startSoc * 10).rounded()))
            try await cl.cfgSet(0x0808, .bool, c.selfDischarge ? 1 : 0)
            try await cl.cfgSet(0x080A, .bool, c.externalLoad ? 1 : 0)
            try await cl.cfgSet(0x080B, .u32, c.externalLoadUa)
            try await cl.cfgSetString(0x080F, c.name)
            await act(.newRun)
        } catch { message = "New run: \(error.localizedDescription)" }
    }

    private func preset(_ secs: Double) {
        guard let h = displayed else { return }
        let b = h.bounds
        window = secs <= 0 ? b : h.clampWindow(b.upperBound - secs, b.upperBound)
    }

    private func zoom(_ factor: Double) {
        guard let h = displayed else { return }
        let mid = (window.lowerBound + window.upperBound) / 2, half = (window.upperBound - window.lowerBound) / 2 * factor
        window = h.clampWindow(mid - half, mid + half)
    }
}

extension BsHistory {
    func clampWindow(_ a: Double, _ b: Double) -> ClosedRange<Double> {
        let lim = bounds
        let w = min(max(b - a, 30), max(lim.upperBound - lim.lowerBound, 30))
        let s = min(max(a, lim.lowerBound), max(lim.upperBound - w, lim.lowerBound))
        return s...(s + w)
    }
}

private extension View {
    func bsCard() -> some View {
        padding(14).glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

/// Filled when `on`, tinted outline otherwise, greyed when disabled.
struct BsButtonStyle: ButtonStyle {
    var tint: Color = .cyan
    var on = false
    var compact = false
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        let fg: Color = !enabled ? Color.white.opacity(0.28) : on ? Color.black.opacity(0.85) : tint
        let fill: Color = !enabled ? Color.white.opacity(0.04)
            : on ? tint.opacity(configuration.isPressed ? 0.65 : 0.9) : tint.opacity(configuration.isPressed ? 0.3 : 0.14)
        let stroke: Color = !enabled ? Color.white.opacity(0.08) : on ? Color.clear : tint.opacity(0.5)
        return configuration.label
            .font(.system(size: compact ? 12 : 13, weight: .semibold))
            .foregroundStyle(fg)
            .padding(.horizontal, compact ? 10 : 14)
            .frame(minHeight: compact ? 34 : 38)
            .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(fill))
            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(stroke, lineWidth: 1))
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
            .contentShape(Rectangle())
    }
}

// MARK: - Chart

struct BatteryHistoryChart: View {
    let history: BsHistory
    @Binding var window: ClosedRange<Double>
    @Binding var scrubbing: Bool
    let logCurrent: Bool
    let wallEpoch: Double?
    let compact: Bool
    @State private var pinchBase: ClosedRange<Double>?
    @State private var cursorX: CGFloat?
    @State private var horizontal: Bool?

    private var left: CGFloat { compact ? 50 : 62 }
    private let axisH: CGFloat = 20, gap: CGFloat = 8

    var body: some View {
        GeometryReader { geo in
            let plotW = max(geo.size.width - left - 8, 10)
            let v = BattSim.view(history, window.lowerBound, window.upperBound, buckets: Int(plotW))
            let idx = cursorX.flatMap { nearest(v, $0, plotW) }
            ZStack(alignment: .topLeading) {
                Canvas { ctx, size in draw(ctx, size, v, plotW, idx) }
                if let cx = cursorX, let i = idx {
                    let bw: CGFloat = compact ? 196 : 236
                    let bx = cx + 18 + bw < geo.size.width ? cx + 18 : cx - 18 - bw
                    CursorBubble(v: v, i: i, title: timeTitle(v.t[i]), compact: compact)
                        .frame(width: bw)
                        .offset(x: max(0, bx), y: 6)
                        .allowsHitTesting(false)
                }
            }
            .contentShape(Rectangle())
            // Simultaneous + axis lock: vertical swipes still scroll the page, horizontal ones scrub.
            .simultaneousGesture(DragGesture(minimumDistance: 6)
                .onChanged { g in
                    guard pinchBase == nil else { return }
                    if horizontal == nil {
                        horizontal = abs(g.translation.width) > abs(g.translation.height)
                        scrubbing = horizontal == true
                    }
                    guard horizontal == true else { return }
                    cursorX = min(max(g.location.x, left), left + plotW)
                }
                .onEnded { _ in horizontal = nil; scrubbing = false })
            .simultaneousGesture(MagnifyGesture()
                .onChanged { m in
                    let base = pinchBase ?? window
                    if pinchBase == nil { pinchBase = window }
                    cursorX = nil
                    let span = base.upperBound - base.lowerBound
                    let ax = min(max(Double((m.startAnchor.x * geo.size.width - left) / plotW), 0), 1)
                    let at = base.lowerBound + ax * span
                    let ns = span / max(Double(m.magnification), 0.05)
                    window = history.clampWindow(at - ax * ns, at + (1 - ax) * ns)
                }
                .onEnded { _ in pinchBase = nil })
            .onTapGesture(count: 2) { window = history.bounds; cursorX = nil }
            .onTapGesture { loc in
                if let cx = cursorX, abs(cx - loc.x) < 24 { cursorX = nil } else { cursorX = min(max(loc.x, left), left + plotW) }
            }
        }
    }

    private func timeTitle(_ t: Double) -> String {
        wallEpoch.map { BattSim.wallFull($0 + t) } ?? "+" + BattSim.duration(t)
    }

    private func nearest(_ v: BsView, _ x: CGFloat, _ plotW: CGFloat) -> Int? {
        guard !v.t.isEmpty else { return nil }
        let t = window.lowerBound + Double((x - left) / plotW) * (window.upperBound - window.lowerBound)
        var lo = 0, hi = v.t.count
        while lo < hi { let m = (lo + hi) / 2; if v.t[m] < t { lo = m + 1 } else { hi = m } }
        if lo >= v.t.count { return v.t.count - 1 }
        return lo > 0 && t - v.t[lo - 1] < v.t[lo] - t ? lo - 1 : lo
    }

    private func draw(_ ctx: GraphicsContext, _ size: CGSize, _ v: BsView, _ plotW: CGFloat, _ idx: Int?) {
        let t0 = window.lowerBound, t1 = window.upperBound, span = max(t1 - t0, 1e-9)
        let plotH = size.height - axisH
        let laneH = (plotH - gap * 3) / 4
        let x = { (t: Double) in left + CGFloat((t - t0) / span) * plotW }
        let grid = Color.white.opacity(0.09), label = Color.white.opacity(0.65)
        let tickFont = Font.system(size: compact ? 9 : 10, design: .monospaced)
        let step = BattSim.timeStep(span, target: max(2, Double(plotW) / (wallEpoch == nil ? 90 : 110)))
        func tick(_ tt: Double, _ text: String) {
            var p = Path(); p.move(to: CGPoint(x: x(tt), y: 0)); p.addLine(to: CGPoint(x: x(tt), y: plotH))
            ctx.stroke(p, with: .color(grid), lineWidth: 0.5)
            ctx.draw(Text(text).font(tickFont).foregroundColor(label), at: CGPoint(x: x(tt), y: plotH + axisH / 2))
        }
        if let ep = wallEpoch {
            let tz = Double(TimeZone.current.secondsFromGMT(for: Date(timeIntervalSince1970: ep + t0)))
            var u = ((ep + t0 + tz) / step).rounded(.up) * step - tz
            while u <= ep + t1 { tick(u - ep, BattSim.wallLabel(u, step: step)); u += step }
        } else {
            var t = (t0 / step).rounded(.up) * step
            while t <= t1 { tick(t, BattSim.tickLabel(t, step: step)); t += step }
        }
        let names = compact ? ["V", "I", "P", "SOC"] : ["Voltage", "Current", "Power", "SOC"]
        let lanes: [(String, Color, [Double], [Double], [Double], Bool)] = [
            ("V", .green, v.vMin, v.vAvg, v.vMax, false),
            ("A", .orange, v.iMin, v.iAvg, v.iMax, logCurrent),
            ("W", .purple, v.pAvg, v.pAvg, v.pAvg, false),
            ("%", .cyan, v.soc, v.soc, v.soc, false),
        ]
        let segs = BattSim.stepSegments(v.t, v.dt)
        for (li, lane) in lanes.enumerated() {
            let top = CGFloat(li) * (laneH + gap)
            let rect = CGRect(x: left, y: top, width: plotW, height: laneH)
            ctx.fill(Path(roundedRect: rect, cornerRadius: 4), with: .color(Color.white.opacity(0.02)))
            ctx.stroke(Path(roundedRect: rect, cornerRadius: 4), with: .color(grid), lineWidth: 0.5)
            guard !v.t.isEmpty else { continue }
            let log = lane.5
            let f = { (y: Double) in log ? log10(max(y, 1e-10)) : y }
            // Log axis: zero / negative readings (output off, offset) would stretch it to 1e-10.
            let usable = { (y: Double) in y.isFinite && (!log || y > 0) }
            var y1 = lane.4.filter(usable).map(f).max() ?? (log ? 0 : 1)
            var y0 = lane.2.filter(usable).map(f).min() ?? (log ? y1 - 3 : 0)
            let pad = y1 - y0 < 1e-12 ? (log ? 0.5 : max(abs(y1), 1e-9) * 0.05) : (y1 - y0) * 0.06
            let dataMin = y0
            y0 -= pad; y1 += pad
            if !log && dataMin >= 0 { y0 = max(y0, 0) }
            let y = { (val: Double) in top + laneH - CGFloat(((log ? log10(max(val, pow(10, y0))) : val) - y0) / (y1 - y0)) * laneH }
            for k in 0...2 {
                let yv = y0 + (y1 - y0) * Double(k) / 2
                let real = log ? pow(10, yv) : yv
                let gy = top + laneH - CGFloat(k) / 2 * laneH
                if k == 1 {
                    var g = Path(); g.move(to: CGPoint(x: left, y: gy)); g.addLine(to: CGPoint(x: left + plotW, y: gy))
                    ctx.stroke(g, with: .color(grid), style: StrokeStyle(lineWidth: 0.5, dash: [2, 4]))
                }
                // Edge labels sit inside their lane so they do not collide with the neighbour's.
                ctx.draw(Text(lane.0 == "%" ? String(format: "%.0f%%", real) : BattSim.si(real, lane.0))
                            .font(.system(size: compact ? 8 : 9, design: .monospaced)).foregroundColor(label),
                         at: CGPoint(x: left - 5, y: gy),
                         anchor: k == 0 ? .bottomTrailing : k == 2 ? .topTrailing : .trailing)
            }
            var inner = ctx
            inner.clip(to: Path(rect))
            if lane.2 != lane.3 {
                var band = Path()
                for (i, s) in segs.enumerated() {
                    let ya = y(lane.4[i]), yb = y(lane.2[i])
                    band.addRect(CGRect(x: x(s.0), y: min(ya, yb), width: max(1, x(s.1) - x(s.0)), height: max(1, abs(yb - ya))))
                }
                inner.fill(band, with: .color(lane.1.opacity(0.22)))
            }
            var line = Path()
            for (i, s) in segs.enumerated() {
                let yv = y(lane.3[i])
                if s.2 { line.addLine(to: CGPoint(x: x(s.0), y: yv)) } else { line.move(to: CGPoint(x: x(s.0), y: yv)) }
                line.addLine(to: CGPoint(x: x(s.1), y: yv))
            }
            inner.stroke(line, with: .color(lane.1), lineWidth: compact ? 1.4 : 1.6)
            let title = inner.resolve(Text(names[li]).font(.system(size: compact ? 9 : 10, weight: .bold)).foregroundColor(lane.1))
            let ts = title.measure(in: CGSize(width: 200, height: 30))
            let tr = CGRect(x: left + 6, y: top + 5, width: ts.width + 10, height: ts.height + 4)
            inner.fill(Path(roundedRect: tr, cornerRadius: 5), with: .color(Color.black.opacity(0.55)))
            inner.draw(title, at: CGPoint(x: tr.midX, y: tr.midY))
            if let i = idx, let cx = cursorX, i < lane.3.count {
                let cy = y(lane.3[i])
                let dot = CGRect(x: cx - 4.5, y: cy - 4.5, width: 9, height: 9)
                inner.fill(Path(ellipseIn: dot), with: .color(lane.1))
                inner.stroke(Path(ellipseIn: dot), with: .color(.white), lineWidth: 1.5)
            }
        }
        var lastLabel = -CGFloat.infinity
        for e in history.events where e.t >= t0 && e.t <= t1 {
            let ex = x(e.t)
            var p = Path(); p.move(to: CGPoint(x: ex, y: 0)); p.addLine(to: CGPoint(x: ex, y: plotH))
            ctx.stroke(p, with: .color(.pink.opacity(0.6)), style: StrokeStyle(lineWidth: 0.8, dash: [3, 3]))
            if ex - lastLabel > (compact ? 48 : 64) && ex < left + plotW - 30 {
                ctx.draw(Text(e.name).font(.system(size: 9, weight: .semibold)).foregroundColor(.pink),
                         at: CGPoint(x: ex + 3, y: laneH + 2), anchor: .bottomLeading)
                lastLabel = ex
            }
        }
        if let cx = cursorX {
            var p = Path(); p.move(to: CGPoint(x: cx, y: 0)); p.addLine(to: CGPoint(x: cx, y: plotH))
            ctx.stroke(p, with: .color(.white.opacity(0.8)), lineWidth: 1)
            if let i = idx {
                let txt = ctx.resolve(Text(timeTitle(v.t[i])).font(.system(size: 9, weight: .bold, design: .monospaced)).foregroundColor(.black))
                let s = txt.measure(in: CGSize(width: 300, height: 30))
                let w = s.width + 12
                let r = CGRect(x: min(max(cx - w / 2, left), left + plotW - w), y: plotH + 2, width: w, height: axisH - 4)
                ctx.fill(Path(roundedRect: r, cornerRadius: 5), with: .color(.white))
                ctx.draw(txt, at: CGPoint(x: r.midX, y: r.midY))
            }
        }
    }
}

private struct CursorBubble: View {
    let v: BsView, i: Int, title: String, compact: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 6) {
                Text(title).font(.system(size: compact ? 11 : 12, weight: .bold, design: .monospaced)).lineLimit(1)
                Spacer(minLength: 4)
                Text(["15 min", "1 min", "1 s"][v.tier[i] % 3]).font(.system(size: 9, weight: .semibold))
                    .padding(.horizontal, 6).padding(.vertical, 2)
                    .background(Capsule().fill(Color.white.opacity(0.12)))
            }
            row(.green, "V", BattSim.si(v.vAvg[i], "V"), "\(BattSim.si(v.vMin[i], "V")) .. \(BattSim.si(v.vMax[i], "V"))")
            row(.orange, "I", BattSim.si(v.iAvg[i], "A"), "\(BattSim.si(v.iMin[i], "A")) .. \(BattSim.si(v.iMax[i], "A"))")
            row(.purple, "P", BattSim.si(v.pAvg[i], "W"), nil)
            row(.cyan, "SOC", String(format: "%.2f %%", v.soc[i]), nil)
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(.ultraThinMaterial))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(Color.white.opacity(0.18), lineWidth: 1))
        .shadow(color: .black.opacity(0.4), radius: 10, y: 4)
    }

    private func row(_ c: Color, _ k: String, _ val: String, _ range: String?) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: 6) {
                Circle().fill(c).frame(width: 8, height: 8)
                Text(k).font(.system(size: 10, weight: .semibold)).foregroundColor(.secondary).frame(width: 28, alignment: .leading)
                Text(val).font(.system(size: compact ? 12 : 13, weight: .semibold, design: .monospaced))
            }
            if let range {
                Text(range).font(.system(size: 9, design: .monospaced)).foregroundColor(.secondary).padding(.leading, 42)
            }
        }
    }
}

/// Whole-run overview; the highlighted box is the visible window. Drag it to pan, tap elsewhere to jump.
private struct BsNavigator: View {
    let history: BsHistory
    @Binding var window: ClosedRange<Double>
    @Binding var scrubbing: Bool
    let logCurrent: Bool
    @State private var start: (ClosedRange<Double>, Double)?
    @State private var horizontal: Bool?

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let b = history.bounds, full = max(b.upperBound - b.lowerBound, 1e-9)
            let v = BattSim.view(history, b.lowerBound, b.upperBound, buckets: max(Int(w / 2), 16))
            Canvas { ctx, size in
                let h = size.height
                let x = { (t: Double) in CGFloat((t - b.lowerBound) / full) * w }
                ctx.fill(Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 8), with: .color(Color.white.opacity(0.04)))
                let vals = v.iAvg.map { logCurrent ? log10(max($0, 1e-9)) : $0 }
                let lo = vals.min() ?? 0, hi = max(vals.max() ?? 1, lo + 1e-12)
                var area = Path()
                area.move(to: CGPoint(x: 0, y: h))
                var lastX: CGFloat = 0
                for (k, s) in BattSim.stepSegments(v.t, v.dt).enumerated() {
                    let yv = h - 3 - CGFloat((vals[k] - lo) / (hi - lo)) * (h - 8)
                    area.addLine(to: CGPoint(x: x(s.0), y: yv)); area.addLine(to: CGPoint(x: x(s.1), y: yv))
                    lastX = x(s.1)
                }
                area.addLine(to: CGPoint(x: lastX, y: h)); area.closeSubpath()
                ctx.fill(area, with: .color(Color.orange.opacity(0.35)))
                let r = CGRect(x: x(window.lowerBound), y: 1, width: max(8, x(window.upperBound) - x(window.lowerBound)), height: h - 2)
                ctx.fill(Path(CGRect(x: 0, y: 0, width: r.minX, height: h)), with: .color(Color.black.opacity(0.35)))
                ctx.fill(Path(CGRect(x: r.maxX, y: 0, width: max(0, w - r.maxX), height: h)), with: .color(Color.black.opacity(0.35)))
                ctx.fill(Path(roundedRect: r, cornerRadius: 6), with: .color(Color.cyan.opacity(0.14)))
                ctx.stroke(Path(roundedRect: r, cornerRadius: 6), with: .color(.cyan), lineWidth: 1.5)
                for hx in [r.minX, r.maxX] {
                    ctx.fill(Path(roundedRect: CGRect(x: hx - 2, y: h / 2 - 8, width: 4, height: 16), cornerRadius: 2), with: .color(.cyan))
                }
            }
            .contentShape(Rectangle())
            .simultaneousGesture(DragGesture(minimumDistance: 4)
                .onChanged { g in
                    if horizontal == nil {
                        horizontal = abs(g.translation.width) > abs(g.translation.height)
                        scrubbing = horizontal == true
                    }
                    guard horizontal == true else { return }
                    let toT = { (px: CGFloat) in b.lowerBound + Double(px / w) * full }
                    if start == nil { start = (window, toT(g.startLocation.x)) }
                    guard let st = start else { return }
                    let base = st.0, span = base.upperBound - base.lowerBound
                    let a = base.contains(st.1) ? base.lowerBound + toT(g.location.x) - st.1 : toT(g.location.x) - span / 2
                    window = history.clampWindow(a, a + span)
                }
                .onEnded { _ in start = nil; horizontal = nil; scrubbing = false })
            .onTapGesture { loc in
                let span = window.upperBound - window.lowerBound
                let t = b.lowerBound + Double(loc.x / w) * full
                window = history.clampWindow(t - span / 2, t + span / 2)
            }
        }
    }
}

// MARK: - New run sheet

struct NewBatteryRunSheet: View {
    struct Config { var name = "", chem = 0, cells = 1, capacityMah = 2000, startSoc = 100.0
                    var selfDischarge = false, externalLoad = false, externalLoadUa = 0 }
    let onCreate: (Config) async -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var c = Config()
    @State private var working = false

    var body: some View {
        NavigationStack {
            Form {
                Section(footer: Text("Created paused with the output off. Start needs a 9 V / 3 A USB-PD contract.")) {
                    TextField("Name", text: $c.name)
                    Picker("Chemistry", selection: $c.chem) {
                        ForEach(BattSim.chemNames.indices, id: \.self) { Text(BattSim.chemNames[$0]).tag($0) }
                    }
                    Stepper("Series cells: \(c.cells)", value: $c.cells, in: 1...14)
                    HStack { Text("Capacity (mAh)"); TextField("mAh", value: $c.capacityMah, format: .number).multilineTextAlignment(.trailing).keyboardType(.numberPad) }
                    HStack { Text("Start SOC (%)"); TextField("%", value: $c.startSoc, format: .number).multilineTextAlignment(.trailing).keyboardType(.decimalPad) }
                    Toggle("Self-discharge", isOn: $c.selfDischarge)
                    Toggle("Virtual external load", isOn: $c.externalLoad)
                    if c.externalLoad {
                        HStack { Text("External load (uA)"); TextField("uA", value: $c.externalLoadUa, format: .number).multilineTextAlignment(.trailing).keyboardType(.numberPad) }
                    }
                }
            }
            .navigationTitle("New battery run")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button(working ? "Creating..." : "Create") {
                        working = true
                        Task { await onCreate(c); working = false; dismiss() }
                    }.disabled(working)
                }
            }
        }
    }
}

private extension Array {
    subscript(safe i: Int) -> Element? { indices.contains(i) ? self[i] : nil }
}
