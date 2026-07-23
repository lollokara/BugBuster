import SwiftUI

/// Shown in the Scope tab while the device is in portrait orientation,
/// prompting the user to rotate to landscape to see the live scope.
struct RotatePromptView: View {
    @State private var rotationAngle: Double = 0

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "arrow.triangle.2.circlepath")
                .font(.system(size: 40, weight: .bold))
                .foregroundColor(.cyan)
                .rotationEffect(.degrees(rotationAngle))
                .onAppear {
                    withAnimation(.easeInOut(duration: 1.2).repeatForever(autoreverses: true)) {
                        rotationAngle = 25
                    }
                }

            Image(systemName: "iphone.landscape")
                .font(.system(size: 44))
                .foregroundColor(.white)

            Text("Rotate your phone to view the scope")
                .font(.system(size: 14, weight: .semibold))
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
