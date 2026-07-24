import SwiftUI
import UIKit

/// Root view: routes to the iPhone floating-tab-bar shell or the iPad sidebar shell.
/// Branches on device idiom (not `horizontalSizeClass`) because iPad Split View /
/// Slide Order can put an iPad into a compact width, and `NavigationSplitView`
/// already handles that narrow-width case on its own (sidebar auto-collapses) —
/// branching on size class here would fight that and incorrectly fall back to the
/// iPhone shell on an iPad in Split View.
struct AdaptiveRootView: View {
    var body: some View {
        if UIDevice.current.userInterfaceIdiom == .pad {
            iPadRootView()
        } else {
            MainTabView()
        }
    }
}
