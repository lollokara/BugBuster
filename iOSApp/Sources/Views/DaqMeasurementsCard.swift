import SwiftUI

/// Live DAQ HAT energy/charge accumulators and per-signal statistics (STATS +
/// ENERGY records, 10 Hz), plus the CSV export affordance — the only export
/// of any kind in the app today. Mirrors the house style established by
/// `VDUTControlsCard`: `.ultraThinMaterial`/glass background, continuous
/// rounded rectangle, monospaced numerics so digits don't jitter the layout
/// at 10 Hz.
struct DaqMeasurementsCard: View {
    @ObservedObject var daqStream: DaqWifiStreamManager

    @State private var exportURL: URL?
    @State private var isExporting = false
    @State private var exportError: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Measurements")
                    .font(.system(size: 13, weight: .bold))
                Spacer()
                exportControl
            }

            energySection

            Divider().background(Color.white.opacity(0.1))

            statsSection

            if let exportError {
                Text(exportError)
                    .font(.system(size: 11))
                    .foregroundColor(.orange)
            }
        }
        .font(.system(size: 13, weight: .medium))
        .foregroundColor(.white)
        .padding(12)
        .background(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(.ultraThinMaterial)
        )
    }

    // MARK: - Export control

    @ViewBuilder
    private var exportControl: some View {
        if let exportURL {
            ShareLink(item: exportURL) {
                Label("Share CSV", systemImage: "square.and.arrow.up")
                    .font(.system(size: 11, weight: .bold))
            }
            .onDisappear { self.exportURL = nil }
        } else {
            Button(action: startExport) {
                HStack(spacing: 4) {
                    if isExporting {
                        ProgressView().controlSize(.mini)
                    } else {
                        Image(systemName: "square.and.arrow.up")
                    }
                    Text("Export CSV")
                }
                .font(.system(size: 11, weight: .bold))
            }
            .buttonStyle(.plain)
            .foregroundColor(.cyan)
            .disabled(isExporting)
        }
    }

    private func startExport() {
        exportError = nil
        isExporting = true
        Task {
            do {
                let url = try await DaqExportManager.export(
                    engine: daqStream.engine,
                    deviceOdrHz: daqStream.lastStatus?.deviceOdrHz
                )
                exportURL = url
            } catch {
                exportError = error.localizedDescription
            }
            isExporting = false
        }
    }

    // MARK: - Energy / charge

    private var energySection: some View {
        Group {
            if let e = daqStream.lastEnergy {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        metric(label: "Energy", value: String(format: "%.3f mWh", e.energyMwh))
                        Spacer()
                        metric(label: "", value: String(format: "%.3f J", e.energyJ))
                    }
                    HStack {
                        metric(label: "Charge", value: String(format: "%.3f mAh", e.chargeMah))
                        Spacer()
                        metric(label: "", value: String(format: "%.3f C", e.chargeC))
                    }
                    HStack {
                        metric(label: "Elapsed", value: formatElapsed(e.elapsedS))
                        Spacer()
                    }
                    HStack(spacing: 16) {
                        lastValue(label: "I", value: Double(e.lastI), unit: "A", color: ScopeColors.daqCurrentFine)
                        lastValue(label: "V", value: Double(e.lastV), unit: "V", color: ScopeColors.daqVoltage)
                        lastValue(label: "P", value: Double(e.lastP), unit: "W", color: ScopeColors.daqPower)
                    }
                }
            } else {
                Text("Waiting for ENERGY frames…")
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
            }
        }
    }

    private func metric(label: String, value: String) -> some View {
        HStack(spacing: 4) {
            if !label.isEmpty {
                Text(label)
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
            }
            Text(value)
                .font(.system(size: 13, weight: .semibold, design: .monospaced))
                .monospacedDigit()
        }
    }

    private func lastValue(label: String, value: Double, unit: String, color: Color) -> some View {
        let (scale, u) = ScopeColors.autoUnit(abs(value), base: unit)
        return HStack(spacing: 4) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(label)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.secondary)
            Text(String(format: "%.3f %@", value * scale, u))
                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                .monospacedDigit()
        }
    }

    private func formatElapsed(_ seconds: Double) -> String {
        let total = Int(seconds.rounded())
        let h = total / 3600
        let m = (total % 3600) / 60
        let s = total % 60
        return String(format: "%02d:%02d:%02d", h, m, s)
    }

    // MARK: - Statistics table

    private var statsSection: some View {
        Group {
            if let stats = daqStream.lastStats {
                VStack(alignment: .leading, spacing: 6) {
                    statsHeaderRow
                    statsRow(label: "I", unit: "A", color: ScopeColors.daqCurrentFine, block: stats.i)
                    statsRow(label: "V", unit: "V", color: ScopeColors.daqVoltage, block: stats.v)
                    statsRow(label: "P", unit: "W", color: ScopeColors.daqPower, block: stats.p)
                }
            } else {
                Text("Waiting for STATS frames…")
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
            }
        }
    }

    private var statsHeaderRow: some View {
        HStack {
            Text("").frame(width: 28, alignment: .leading)
            ForEach(["min", "mean", "max", "rms", "std"], id: \.self) { col in
                Text(col)
                    .font(.system(size: 9, weight: .bold))
                    .foregroundColor(.secondary)
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
        }
    }

    private func statsRow(label: String, unit: String, color: Color, block: DaqStatBlock) -> some View {
        // One shared scale per row, chosen from the row's largest magnitude,
        // so min/mean/max/rms/std within a row read on a consistent unit
        // rather than each cell auto-ranging independently.
        let refMag = [block.min, block.max, block.rms].map { abs(Double($0)) }.max() ?? 0
        let (scale, u) = ScopeColors.autoUnit(refMag, base: unit)
        return HStack {
            HStack(spacing: 4) {
                Circle().fill(color).frame(width: 6, height: 6)
                Text(label)
                    .font(.system(size: 11, weight: .bold))
            }
            .frame(width: 28, alignment: .leading)

            let values = cells(block, scale: scale, unit: u)
            ForEach(Array(values.enumerated()), id: \.offset) { _, cell in
                Text(cell)
                    .font(.system(size: 11, design: .monospaced))
                    .monospacedDigit()
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
        }
    }

    private func cells(_ block: DaqStatBlock, scale: Double, unit: String) -> [String] {
        [block.min, block.mean, block.max, block.rms, block.std].map {
            String(format: "%.3f%@", Double($0) * scale, unit)
        }
    }
}
