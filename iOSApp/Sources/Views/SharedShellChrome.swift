import SwiftUI

/// Deep obsidian gradient background used behind the connected-state shell.
/// Shared by `MainTabView` (iPhone) and `iPadRootView` (iPad) so the two shells
/// can't visually drift.
struct ConnectedBackgroundView: View {
    var body: some View {
        ZStack {
            LinearGradient(
                colors: [
                    Color(red: 0.02, green: 0.04, blue: 0.09),
                    Color(red: 0.05, green: 0.08, blue: 0.16),
                    Color(red: 0.03, green: 0.05, blue: 0.11)
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )

            RadialGradient(
                colors: [Color.cyan.opacity(0.14), .clear],
                center: .topLeading,
                startRadius: 24,
                endRadius: 420
            )
            .blendMode(.screen)
            .blur(radius: 18)

            RadialGradient(
                colors: [Color.blue.opacity(0.12), .clear],
                center: .topTrailing,
                startRadius: 28,
                endRadius: 480
            )
            .blendMode(.screen)
            .blur(radius: 20)

            RadialGradient(
                colors: [Color.purple.opacity(0.08), .clear],
                center: .bottom,
                startRadius: 40,
                endRadius: 520
            )
            .blendMode(.screen)
            .blur(radius: 24)
        }
        .ignoresSafeArea()
    }
}

/// Floating glass toast shown for connection-manager status messages. Shared by
/// both shells.
struct ToastOverlayView: View {
    @EnvironmentObject var connectionManager: ConnectionManager

    var body: some View {
        if let toast = connectionManager.toastMessage {
            VStack {
                HStack(spacing: 8) {
                    Image(systemName: toast.type == .success ? "checkmark.circle.fill" : toast.type == .error ? "xmark.circle.fill" : "info.circle.fill")
                        .foregroundColor(toast.type == .success ? .green : toast.type == .error ? .red : .blue)
                    Text(toast.text)
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundColor(.white)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .glassEffect(.regular, in: Capsule())
                .shadow(color: .black.opacity(0.3), radius: 10)
                .padding(.top, 60)
                Spacer()
            }
            .transition(.move(edge: .top).combined(with: .opacity))
            .animation(.spring(response: 0.3), value: connectionManager.toastMessage)
            .allowsHitTesting(false)
        }
    }
}
