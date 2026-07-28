import SwiftUI

/// DAQ HAT DUT power supply controls (enable + voltage setpoint + current
/// limit), backed by the real `/api/daq/vdut/status|enable|setpoint` firmware
/// surface (reachable over BLE or HTTP). `ConnectionManager.vdutPresent`
/// reflects whether the last status refresh actually got a response from
/// that surface, driving the badge below.
/// Distinct from the mainboard/HAT power rail cards in `OverviewTab`.
struct VDUTControlsCard: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var voltageDraft: Double = 3.3
    @State private var currentLimitDraft: Double = 100
    @State private var isApplying = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("VDUT (DUT Supply)")
                    .font(.system(size: 13, weight: .bold))
                Spacer()
                if connectionManager.vdutPresent {
                    Text("Live")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.green)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(Capsule().stroke(Color.green.opacity(0.5), lineWidth: 1))
                } else {
                    Text("FW pending")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.orange)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(Capsule().stroke(Color.orange.opacity(0.5), lineWidth: 1))
                }
            }

            Toggle("Enable DUT Supply", isOn: Binding(
                get: { connectionManager.vdutEnabled },
                set: { newValue in
                    Task { _ = await connectionManager.setVdutEnable(newValue) }
                }
            ))
            .tint(.cyan)

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("Voltage Setpoint")
                    Spacer()
                    Text(String(format: "%.2f V", voltageDraft))
                        .font(.system(.body, design: .monospaced))
                }
                Slider(value: $voltageDraft, in: 0...12, step: 0.05)
            }

            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("Current Limit")
                    Spacer()
                    Text(String(format: "%.0f mA", currentLimitDraft))
                        .font(.system(.body, design: .monospaced))
                }
                Slider(value: $currentLimitDraft, in: 0...2000, step: 10)
            }

            Button(action: applySetpoint) {
                HStack {
                    if isApplying { ProgressView().tint(.black) }
                    Text("Apply Setpoint")
                        .font(.system(size: 13, weight: .bold))
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
                .foregroundColor(.black)
                .background(Capsule().fill(Color.cyan))
            }
            .buttonStyle(.plain)

            if let mv = connectionManager.vdutMeasuredVoltageV, let mc = connectionManager.vdutMeasuredCurrentMa {
                HStack {
                    Text(String(format: "Measured: %.3f V, %.1f mA", mv, mc))
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.secondary)
                    Spacer()
                }
            } else {
                Text(connectionManager.vdutPresent
                     ? "No measurement yet."
                     : "No measurement yet — device did not respond to the VDUT status request.")
                    .font(.system(size: 11))
                    .foregroundColor(.secondary)
            }
        }
        .font(.system(size: 13, weight: .medium))
        .foregroundColor(.white)
        .padding(12)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .onAppear {
            // Drafts are seeded from ConnectionManager's published state, which
            // startVdutPrefetch() keeps fresh in the background — no fetch is
            // kicked off here, so the menu opens with real values, not stale
            // ones that snap a moment later.
            voltageDraft = connectionManager.vdutVoltageSetpointV
            currentLimitDraft = connectionManager.vdutCurrentLimitMa
        }
    }

    private func applySetpoint() {
        isApplying = true
        Task {
            _ = await connectionManager.setVdutSetpoint(voltageV: voltageDraft, currentLimitMa: currentLimitDraft)
            isApplying = false
        }
    }
}
