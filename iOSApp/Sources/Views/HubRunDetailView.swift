import SwiftUI

/// One run held by the hub: its time span, how much of it the hub covers (by resolution) and how it relates to the board's copy.
struct HubRunDetailView: View {
    @ObservedObject var model: HubStatusModel
    @State var run: HubRunInfo
    let local: HubLocalRun?
    var onOpen: ((Int) -> Void)?
    @State private var coverage: HubCoverage?
    @State private var loading = true
    @State private var failure: HubFailure?

    private var stats: HubCoverageStats? {
        coverage.flatMap { HubCoverageStats.compute($0, from: run.startedAt, to: run.lastTS ?? run.lastSampleAt) }
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                summaryCard
                coverageCard
                deviceCard
            }
            .frame(maxWidth: 760).padding(16).frame(maxWidth: .infinity)
        }
        .navigationTitle(run.name.isEmpty ? "Run #\(run.runID)" : run.name)
        .navigationBarTitleDisplayMode(.inline)
        .refreshable { await load() }
        .task { await load() }
    }

    private func load() async {
        guard let api = model.api else { return }
        loading = true; defer { loading = false }
        do {
            async let r = api.run(uid: run.uid)
            async let c = api.coverage(uid: run.uid)
            let (rr, cc) = try await (r, c)
            run = rr; coverage = cc; failure = nil
        } catch {
            failure = HubFailure(error)
        }
    }

    private func card<C: View>(_ title: String, @ViewBuilder _ body: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title).font(.headline).accessibilityAddTraits(.isHeader)
            body()
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }

    private func row(_ k: String, _ v: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(k).font(.subheadline).foregroundStyle(.secondary)
            Spacer(minLength: 12)
            Text(v).font(.subheadline.weight(.medium)).monospacedDigit().multilineTextAlignment(.trailing)
        }
        .frame(minHeight: 28)
        .accessibilityElement(children: .combine)
    }

    private var summaryCard: some View {
        card("Run") {
            row("Started", Date(timeIntervalSince1970: run.startedAt).formatted(date: .abbreviated, time: .shortened))
            row("State", run.state.isEmpty ? "—" : run.state.capitalized)
            if let l = run.lastTS ?? run.lastSampleAt { row("Last sample", Date(timeIntervalSince1970: l).formatted(date: .abbreviated, time: .shortened)) }
            if let d = run.duration { row("Duration", HubFormat.duration(d)) }
            if let n = run.points { row("Samples on hub", "\(n)") } else if loading { row("Samples on hub", "…").redacted(reason: .placeholder) }
            if let c = run.chem, let n = run.cells, let mah = run.capacityMah { row("Battery", "\(c) \(n)S · \(mah) mAh") }
        }
    }

    private var coverageCard: some View {
        card("Hub coverage") {
            if let failure {
                Text("\(failure.message) \(failure.fix)").font(.subheadline).foregroundStyle(.orange)
            } else if let s = stats, let cov = coverage {
                HubCoverageBar(coverage: cov, start: s.spanStart, end: s.spanEnd)
                    .frame(height: 28)
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("Coverage \(Int((s.fraction * 100).rounded())) percent, \(s.gaps.count) gaps")
                HStack(spacing: 14) {
                    legend("1 s", .green); legend("1 min", .cyan); legend("15 min", .purple)
                }
                row("Covered", "\(Int((s.fraction * 100).rounded())) % of \(HubFormat.duration(s.span))")
                row("Gaps", s.gaps.isEmpty ? "None" : "\(s.gaps.count) · longest \(HubFormat.duration(s.gaps.map { $0.to - $0.from }.max() ?? 0))")
            } else if loading {
                RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.1)).frame(height: 28)
                    .redacted(reason: .placeholder).accessibilityLabel("Loading coverage")
            } else {
                Text("The hub holds no samples for this run yet.").font(.subheadline).foregroundStyle(.secondary)
            }
        }
    }

    private func legend(_ t: String, _ c: Color) -> some View {
        HStack(spacing: 5) {
            RoundedRectangle(cornerRadius: 2).fill(c).frame(width: 12, height: 8)
            Text(t).font(.caption).foregroundStyle(.secondary)
        }
        .accessibilityHidden(true)
    }

    private var deviceCard: some View {
        card("On the BugBuster") {
            if let l = local {
                row("Stored on device", "Run #\(l.runId) · \(ByteCountFormatter.string(fromByteCount: Int64(l.bytes), countStyle: .file))")
                Text("Opening it in the chart uses the device file and fills any gaps with the hub's higher-resolution samples.")
                    .font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                if let onOpen {
                    Button { onOpen(l.runId) } label: {
                        Label("Open in chart", systemImage: "chart.xyaxis.line").frame(maxWidth: .infinity, minHeight: 44)
                    }
                    .buttonStyle(BsButtonStyle(tint: .cyan, on: true))
                    .accessibilityIdentifier("hub-open-in-chart")
                }
            } else {
                Text("This run is not on the connected board, so there is no device copy to chart. The hub keeps it for export and reference.")
                    .font(.subheadline).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

/// Timeline of the hub's ranges over the run span, coloured by resolution; uncovered time stays dark.
struct HubCoverageBar: View {
    let coverage: HubCoverage
    let start: Double
    let end: Double

    static func color(_ res: Int) -> Color { res <= 1 ? .green : res < 600 ? .cyan : .purple }

    var body: some View {
        Canvas { ctx, size in
            let span = max(end - start, 1)
            ctx.fill(Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 6), with: .color(.white.opacity(0.08)))
            // Coarse first so fine resolution paints on top where ranges overlap.
            for r in coverage.ranges.sorted(by: { $0.res > $1.res }) {
                let a = max(r.from, start), b = min(r.to, end)
                guard b >= a else { continue }
                let x = (a - start) / span * size.width
                let w = max(2, (b - a) / span * size.width)
                ctx.fill(Path(CGRect(x: x, y: 0, width: w, height: size.height)), with: .color(Self.color(r.res).opacity(0.85)))
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).stroke(Color.white.opacity(0.12)))
    }
}
