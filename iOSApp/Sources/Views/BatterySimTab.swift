import SwiftUI

/// Battery simulator (DAQ HAT): live run status + controls, run browser, a
/// pinch/drag-zoomable V / I / P / SOC history and window statistics.
struct BatterySimTab: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var status: BsStatus?
    @State private var runs: [BsRunSummary] = []
    @State private var history: BsHistory?
    @State private var window: ClosedRange<Double> = 0...1
    @State private var busy: String?
    @State private var message: String?
    @State private var logCurrent = true
    @State private var showNew = false
    @State private var pendingDelete: Int?
    @State private var pollTask: Task<Void, Never>?

    private var client: BattSimClient { BattSimClient(connectionManager) }

    var body: some View {
        ScrollView {
            VStack(spacing: 12) {
                liveCard
                if let message {
                    Text(message).font(.system(size: 12)).foregroundColor(.orange)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                runsCard
                chartCard
                statsCard
            }
            .padding(.horizontal, 14)
            .padding(.top, 12)
            .padding(.bottom, 110)
        }
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
        }
    }

    // MARK: Cards

    private var liveCard: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("BATTERY SIMULATOR").font(.system(size: 13, weight: .bold))
                Spacer()
                if let s = status {
                    Text(BattSim.stateNames[safe: s.state] ?? "?").font(.system(size: 9, weight: .bold))
                        .padding(.horizontal, 8).padding(.vertical, 3)
                        .background(Capsule().fill(stateColor(s.state).opacity(0.25)))
                }
            }
            if let s = status, s.state != 0 {
                HStack(spacing: 10) {
                    ZStack(alignment: .leading) {
                        Capsule().fill(Color.white.opacity(0.08))
                        GeometryReader { g in
                            Capsule().fill(Color.green.opacity(0.45)).frame(width: g.size.width * min(max(s.socPct / 100, 0), 1))
                        }
                        Text(String(format: "%.1f %%", s.socPct)).font(.system(.caption, design: .monospaced))
                            .frame(maxWidth: .infinity)
                    }
                    .frame(height: 22)
                    Text("#\(s.runId) \(BattSim.chemNames[safe: s.chem] ?? "?") \(s.cells)S \(s.capacityMah) mAh")
                        .font(.system(size: 11)).foregroundColor(.secondary).lineLimit(1)
                }
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                    readout("V", BattSim.si(s.vMeas, "V"))
                    readout("I", BattSim.si(s.iMeas, "A"))
                    readout("Elapsed", BattSim.duration(Double(s.elapsedS)))
                    readout("Remaining", s.remainingS.map { (s.provisional ? "~" : "") + BattSim.duration(Double($0)) } ?? "-")
                    readout("Target", BattSim.si(s.vTarget, "V"))
                    readout("Storage", "\(s.fsUsed / 1024)/\(s.fsTotal / 1024) KiB")
                }
            } else {
                Text(status == nil ? "Battery simulator not reachable." : "No run loaded.")
                    .font(.system(size: 12)).foregroundColor(.secondary)
            }
            HStack(spacing: 8) {
                let st = status?.state ?? 0
                controlButton("Start", "play.fill", enabled: st == 1) { await act(.start) }
                controlButton("Pause", "pause.fill", enabled: st == 2) { await act(.pause) }
                controlButton("Stop", "stop.fill", enabled: st == 1 || st == 2) { await act(.stop) }
                controlButton("Unload", "eject", enabled: st != 0) { await act(.unload) }
                Button { showNew = true } label: { Label("New", systemImage: "plus") }
                    .buttonStyle(.bordered).disabled(st == 2)
            }
            .font(.system(size: 12, weight: .medium))
        }
        .padding(12)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    private var runsCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("RUNS").font(.system(size: 13, weight: .bold))
                Spacer()
                Button { Task { await refreshRuns() } } label: { Image(systemName: "arrow.clockwise") }
            }
            if runs.isEmpty { Text("No runs stored.").font(.system(size: 12)).foregroundColor(.secondary) }
            ForEach(runs.reversed()) { r in
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 6) {
                            Text("#\(r.runId)").font(.system(size: 13, weight: .semibold))
                            Text(r.meta?.name ?? "").font(.system(size: 12)).foregroundColor(.secondary).lineLimit(1)
                            if r.active { Text("LOADED").font(.system(size: 9, weight: .bold)).foregroundColor(.green) }
                        }
                        Text(runSubtitle(r)).font(.system(size: 10)).foregroundColor(.secondary).lineLimit(1)
                    }
                    Spacer()
                    Menu {
                        Button("Open history") { Task { await open(r.runId) } }
                        Button("Load on device") { Task { await act(.load, run: r.runId) } }.disabled(r.active)
                        Button("Delete", role: .destructive) { pendingDelete = r.runId }.disabled(r.active)
                    } label: { Image(systemName: "ellipsis.circle") }
                }
                .contentShape(Rectangle())
                .onTapGesture { Task { await open(r.runId) } }
                .padding(.vertical, 4)
                .background(history?.meta.runId == r.runId ? Color.cyan.opacity(0.10) : .clear)
            }
        }
        .padding(12)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    private var chartCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(history.map { "RUN #\($0.meta.runId)" } ?? "HISTORY").font(.system(size: 13, weight: .bold))
                Spacer()
                Toggle("log I", isOn: $logCurrent).toggleStyle(.button).font(.system(size: 11))
            }
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach([("1h", 3600.0), ("6h", 21600), ("1d", 86400), ("7d", 604800), ("30d", 2592000), ("All", 0)], id: \.0) { p in
                        Button(p.0) { preset(p.1) }.buttonStyle(.bordered).font(.system(size: 11)).disabled(history == nil)
                    }
                }
            }
            ZStack {
                if let h = history {
                    BatteryHistoryChart(history: h, window: $window, logCurrent: logCurrent)
                } else {
                    Text(busy ?? "Tap a run to open its history. Pinch to zoom, drag to pan, double-tap for the whole run.")
                        .font(.system(size: 12)).foregroundColor(.secondary).multilineTextAlignment(.center).padding()
                }
                if history != nil, let busy {
                    Text(busy).font(.system(size: 10)).padding(6).background(.ultraThinMaterial, in: Capsule())
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                }
            }
            .frame(height: 440)
        }
        .padding(12)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    @ViewBuilder private var statsCard: some View {
        if let h = history, let s = BattSim.stats(h, window.lowerBound, window.upperBound) {
            VStack(alignment: .leading, spacing: 6) {
                Text("WINDOW STATISTICS").font(.system(size: 13, weight: .bold))
                statRow("Span", "\(BattSim.duration(s.duration)) from +\(BattSim.duration(s.tStart))")
                statRow("Voltage min / avg / max", "\(BattSim.si(s.vMin, "V")) / \(BattSim.si(s.vAvg, "V")) / \(BattSim.si(s.vMax, "V"))")
                statRow("Current min / avg / max", "\(BattSim.si(s.iMin, "A")) / \(BattSim.si(s.iAvg, "A")) / \(BattSim.si(s.iMax, "A"))")
                statRow("Baseline (P10) / median / P99", "\(BattSim.si(s.iP10, "A")) / \(BattSim.si(s.iMedian, "A")) / \(BattSim.si(s.iP99, "A"))")
                statRow("Power avg / peak", "\(BattSim.si(s.pAvg, "W")) / \(BattSim.si(s.pMax, "W"))")
                statRow("Charge", BattSim.si(s.chargeC / 3600, "Ah"))
                statRow(s.energyEstimated ? "Energy (est.)" : "Energy", BattSim.si(s.energyJ / 3600, "Wh"))
                statRow("SOC", String(format: "%.2f -> %.2f %% (%.3f %%/day)", s.socStart, s.socEnd, s.socRatePerDay))
                statRow("Full-battery life at avg", BattSim.duration(s.projectedLifeS))
                statRow("Remaining at avg", BattSim.duration(s.remainingAtAvgS))
                if s.gaps > 0 || s.clamped > 0 { statRow("Data quality", "\(s.gaps) gaps, \(s.clamped) clamped") }
            }
            .padding(12)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        }
    }

    // MARK: Pieces

    private func readout(_ label: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(label).font(.system(size: 9)).foregroundColor(.secondary)
            Text(value).font(.system(.caption, design: .monospaced)).lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func statRow(_ k: String, _ v: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(k).font(.system(size: 11)).foregroundColor(.secondary)
            Spacer()
            Text(v).font(.system(size: 11, design: .monospaced)).multilineTextAlignment(.trailing)
        }
    }

    private func controlButton(_ title: String, _ icon: String, enabled: Bool, _ run: @escaping () async -> Void) -> some View {
        Button { Task { await run() } } label: { Label(title, systemImage: icon).labelStyle(.iconOnly) }
            .buttonStyle(.bordered).disabled(!enabled).accessibilityLabel(title)
    }

    private func stateColor(_ s: Int) -> Color { [Color.gray, .yellow, .green, .red, .gray][safe: s] ?? .gray }

    private func runSubtitle(_ r: BsRunSummary) -> String {
        let size = r.bytes >= 1 << 20 ? String(format: "%.2f MiB", Double(r.bytes) / 1_048_576) : "\(r.bytes / 1024) KiB"
        guard let m = r.meta else { return size }
        let date = m.createdEpoch > 0
            ? Date(timeIntervalSince1970: Double(m.createdEpoch)).formatted(date: .abbreviated, time: .shortened) : "date unknown"
        return "\(BattSim.chemNames[safe: m.params.chem] ?? "?") \(m.params.cells)S \(m.params.capacityMah) mAh - \(date) - \(size)"
    }

    // MARK: Actions

    private func startPolling() {
        pollTask?.cancel()
        pollTask = Task {
            await client.setEpoch()
            await refreshRuns()
            while !Task.isCancelled {
                if let s = try? await client.status() { status = s }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    private func refreshRuns() async {
        do { runs = try await client.listRuns() } catch { message = error.localizedDescription }
    }

    private func open(_ id: Int) async {
        busy = "Opening run..."
        message = nil
        do {
            let files = try await client.syncRun(id) { name, done, total in
                Task { @MainActor in busy = "Downloading \(name) \(Int(Double(done) / Double(max(total, 1)) * 100)) %" }
            }
            let h = try BattSim.buildHistory(files)
            history = h
            window = h.bounds
        } catch { message = error.localizedDescription }
        busy = nil
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
        guard let h = history else { return }
        let b = h.bounds
        window = secs <= 0 ? b : max(b.lowerBound, b.upperBound - secs)...b.upperBound
    }
}

// MARK: - Chart

struct BatteryHistoryChart: View {
    let history: BsHistory
    @Binding var window: ClosedRange<Double>
    let logCurrent: Bool
    @State private var pinchBase: ClosedRange<Double>?
    @State private var dragBase: ClosedRange<Double>?
    @State private var hoverX: CGFloat?

    private let left: CGFloat = 54, axisH: CGFloat = 18, gap: CGFloat = 6

    var body: some View {
        GeometryReader { geo in
            let plotW = max(geo.size.width - left - 6, 10)
            let v = BattSim.view(history, window.lowerBound, window.upperBound, buckets: Int(plotW))
            ZStack(alignment: .topLeading) {
                Canvas { ctx, size in draw(ctx, size, v, plotW) }
                if let x = hoverX, let i = nearest(v, x, plotW) {
                    tooltip(v, i).offset(x: min(x + 10, geo.size.width - 170), y: 4)
                }
            }
            .contentShape(Rectangle())
            .gesture(SimultaneousGesture(
                MagnificationGesture()
                    .onChanged { scale in
                        let base = pinchBase ?? window
                        if pinchBase == nil { pinchBase = window }
                        let mid = (base.lowerBound + base.upperBound) / 2
                        let half = (base.upperBound - base.lowerBound) / 2 / max(Double(scale), 0.05)
                        setWindow(mid - half, mid + half)
                    }
                    .onEnded { _ in pinchBase = nil },
                DragGesture(minimumDistance: 4)
                    .onChanged { g in
                        let base = dragBase ?? window
                        if dragBase == nil { dragBase = window }
                        let dt = Double(g.translation.width / plotW) * (base.upperBound - base.lowerBound)
                        setWindow(base.lowerBound - dt, base.upperBound - dt)
                        hoverX = nil
                    }
                    .onEnded { _ in dragBase = nil }
            ))
            .onTapGesture(count: 2) { window = history.bounds }
            .onTapGesture { loc in hoverX = hoverX == nil ? loc.x : nil }
        }
    }

    private func setWindow(_ a: Double, _ b: Double) {
        let lim = history.bounds
        let w = min(max(b - a, 30), max(lim.upperBound - lim.lowerBound, 30))
        let s = min(max(a, lim.lowerBound), max(lim.upperBound - w, lim.lowerBound))
        window = s...(s + w)
    }

    private func nearest(_ v: BsView, _ x: CGFloat, _ plotW: CGFloat) -> Int? {
        guard !v.t.isEmpty else { return nil }
        let t = window.lowerBound + Double((x - left) / plotW) * (window.upperBound - window.lowerBound)
        return v.t.indices.min { abs(v.t[$0] - t) < abs(v.t[$1] - t) }
    }

    private func tooltip(_ v: BsView, _ i: Int) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("+\(BattSim.duration(v.t[i])) (\(["15 min", "1 min", "1 s"][v.tier[i] % 3]))").bold()
            Text("\(BattSim.si(v.vAvg[i], "V")) [\(BattSim.si(v.vMin[i], "V"))..\(BattSim.si(v.vMax[i], "V"))]").foregroundColor(.green)
            Text("\(BattSim.si(v.iAvg[i], "A")) [\(BattSim.si(v.iMin[i], "A"))..\(BattSim.si(v.iMax[i], "A"))]").foregroundColor(.orange)
            Text(BattSim.si(v.pAvg[i], "W")).foregroundColor(.purple)
            Text(String(format: "SOC %.2f %%", v.soc[i])).foregroundColor(.cyan)
        }
        .font(.system(size: 10, design: .monospaced))
        .padding(6)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private func draw(_ ctx: GraphicsContext, _ size: CGSize, _ v: BsView, _ plotW: CGFloat) {
        let t0 = window.lowerBound, t1 = window.upperBound, span = max(t1 - t0, 1e-9)
        let laneH = (size.height - axisH - gap * 3) / 4
        let x = { (t: Double) in left + CGFloat((t - t0) / span) * plotW }
        let grid = Color.white.opacity(0.10), label = Color.white.opacity(0.65)
        let step = BattSim.timeStep(span, target: max(2, Double(plotW) / 90))
        var t = (t0 / step).rounded(.up) * step
        while t <= t1 {
            var p = Path(); p.move(to: CGPoint(x: x(t), y: 0)); p.addLine(to: CGPoint(x: x(t), y: size.height - axisH))
            ctx.stroke(p, with: .color(grid), lineWidth: 0.5)
            ctx.draw(Text(BattSim.tickLabel(t, step: step)).font(.system(size: 9, design: .monospaced)).foregroundColor(label),
                     at: CGPoint(x: x(t), y: size.height - axisH / 2))
            t += step
        }
        let lanes: [(String, String, Color, [Double], [Double], [Double], Bool)] = [
            ("V", "V", .green, v.vMin, v.vAvg, v.vMax, false),
            ("I", "A", .orange, v.iMin, v.iAvg, v.iMax, logCurrent),
            ("P", "W", .purple, v.pAvg, v.pAvg, v.pAvg, false),
            ("SOC", "%", .cyan, v.soc, v.soc, v.soc, false),
        ]
        for (li, lane) in lanes.enumerated() {
            let top = CGFloat(li) * (laneH + gap)
            let rect = CGRect(x: left, y: top, width: plotW, height: laneH)
            ctx.stroke(Path(rect), with: .color(grid), lineWidth: 0.5)
            ctx.draw(Text(lane.0).font(.system(size: 9, weight: .semibold)).foregroundColor(label),
                     at: CGPoint(x: left + 12, y: top + 8))
            guard !v.t.isEmpty else { continue }
            let log = lane.6
            let f = { (y: Double) in log ? log10(max(y, 1e-10)) : y }
            // Log axis: zero / negative readings (output off, offset) would stretch it to 1e-10.
            let usable = { (y: Double) in y.isFinite && (!log || y > 0) }
            var y1 = lane.5.filter(usable).map(f).max() ?? (log ? 0 : 1)
            var y0 = lane.3.filter(usable).map(f).min() ?? (log ? y1 - 3 : 0)
            let pad = y1 - y0 < 1e-12 ? (log ? 0.5 : max(abs(y1), 1e-9) * 0.05) : (y1 - y0) * 0.06
            let dataMin = y0
            y0 -= pad; y1 += pad
            if !log && dataMin >= 0 { y0 = max(y0, 0) }
            let y = { (val: Double) in top + laneH - CGFloat(((log ? log10(max(val, pow(10, y0))) : val) - y0) / (y1 - y0)) * laneH }
            for k in 0...2 {
                let yv = y0 + (y1 - y0) * Double(k) / 2
                let real = log ? pow(10, yv) : yv
                // Edge labels sit inside their lane so they do not collide with the neighbour's.
                ctx.draw(Text(lane.1 == "%" ? String(format: "%.0f%%", real) : BattSim.si(real, lane.1))
                            .font(.system(size: 8, design: .monospaced)).foregroundColor(label),
                         at: CGPoint(x: left - 4, y: top + laneH - CGFloat(k) / 2 * laneH),
                         anchor: k == 0 ? .bottomTrailing : k == 2 ? .topTrailing : .trailing)
            }
            var inner = ctx
            inner.clip(to: Path(rect))
            let segs = BattSim.stepSegments(v.t, v.dt)
            if lane.3 != lane.4 {
                var band = Path()
                for (i, s) in segs.enumerated() {
                    let ya = y(lane.5[i]), yb = y(lane.3[i])
                    band.addRect(CGRect(x: x(s.0), y: min(ya, yb), width: max(1, x(s.1) - x(s.0)), height: max(1, abs(yb - ya))))
                }
                inner.fill(band, with: .color(lane.2.opacity(0.22)))
            }
            var line = Path()
            for (i, s) in segs.enumerated() {
                let yv = y(lane.4[i])
                if s.2 { line.addLine(to: CGPoint(x: x(s.0), y: yv)) } else { line.move(to: CGPoint(x: x(s.0), y: yv)) }
                line.addLine(to: CGPoint(x: x(s.1), y: yv))
            }
            inner.stroke(line, with: .color(lane.2), lineWidth: 1.4)
        }
        for e in history.events where e.t >= t0 && e.t <= t1 {
            var p = Path(); p.move(to: CGPoint(x: x(e.t), y: 0)); p.addLine(to: CGPoint(x: x(e.t), y: size.height - axisH))
            ctx.stroke(p, with: .color(.pink.opacity(0.7)), style: StrokeStyle(lineWidth: 0.8, dash: [3, 3]))
        }
        if let hx = hoverX {
            var p = Path(); p.move(to: CGPoint(x: hx, y: 0)); p.addLine(to: CGPoint(x: hx, y: size.height - axisH))
            ctx.stroke(p, with: .color(label), lineWidth: 0.5)
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
