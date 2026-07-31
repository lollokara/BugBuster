import UIKit

final class OrientationLock {
    static let shared = OrientationLock()
    var mask: UIInterfaceOrientationMask
    private init() {
        mask = UIDevice.current.userInterfaceIdiom == .pad ? .allButUpsideDown : .portrait
    }
}
