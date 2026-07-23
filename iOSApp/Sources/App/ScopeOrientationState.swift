import UIKit
import Combine

/// Tracks live physical device orientation for the Scope tab only. Other tabs
/// stay portrait-locked via OrientationLock and never read this.
final class ScopeOrientationState: ObservableObject {
    static let shared = ScopeOrientationState()

    @Published var isLandscape = false

    private init() {
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(orientationChanged),
            name: UIDevice.orientationDidChangeNotification,
            object: nil
        )
    }

    @objc private func orientationChanged() {
        switch UIDevice.current.orientation {
        case .landscapeLeft, .landscapeRight:
            isLandscape = true
        case .portrait, .portraitUpsideDown:
            isLandscape = false
        default:
            break // ignore faceUp/faceDown/unknown, keep last known state
        }
    }

    func beginTracking() {
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
    }

    func endTracking() {
        UIDevice.current.endGeneratingDeviceOrientationNotifications()
        isLandscape = false
    }
}
