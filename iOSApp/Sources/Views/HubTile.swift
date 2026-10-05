import SwiftUI

extension HubReachability {
    /// Green = reachable, amber = configured but silent, neutral = off.
    var tint: Color {
        switch self {
        case .online: return .green
        case .offline: return .orange
        case .checking, .notConfigured: return .secondary
        }
    }
}

/// The Battery toolbar's Hub tile: tinted by hub state, with a status dot, a caption and a VoiceOver value.
struct HubTile: View {
    @ObservedObject var model: HubStatusModel
    var action: () -> Void

    var body: some View {
        let r = model.reachability
        let filled = r == .online
        Button(action: action) {
            VStack(spacing: 2) {
                ZStack(alignment: .topTrailing) {
                    Image(systemName: r == .offline ? "network.slash" : "network").font(.system(size: 15, weight: .semibold))
                    Circle().fill(r == .notConfigured ? Color.secondary.opacity(0.5) : r.tint)
                        .frame(width: 7, height: 7).offset(x: 6, y: -3)
                        .overlay(Circle().stroke(Color.black.opacity(0.4), lineWidth: 1).offset(x: 6, y: -3))
                }
                Text(r.caption).font(.system(size: 10, weight: .semibold)).lineLimit(1).minimumScaleFactor(0.7)
            }
            .foregroundStyle(filled ? Color.black.opacity(0.85) : r == .notConfigured || r == .checking ? Color.secondary : r.tint)
            .frame(minWidth: 56, maxWidth: .infinity, minHeight: 44, maxHeight: 44)
            .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(filled ? Color.green.opacity(0.9) : r.tint.opacity(0.14)))
            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(filled ? Color.clear : r.tint.opacity(0.5), lineWidth: 1))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("ESPFleet hub")
        .accessibilityValue(r.accessibilityValue)
        .accessibilityHint("Opens the hub screen")
        .accessibilityIdentifier("hub-tile")
        .animation(.easeOut(duration: 0.2), value: r)
    }
}
