import SwiftUI

struct BugBusterApp: App {
    @StateObject private var connectionManager = ConnectionManager()
    @StateObject private var scripts = ScriptRunManager()
    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            AdaptiveRootView()
                .environmentObject(connectionManager)
                .environmentObject(scripts)
                .onAppear {
                    // Attach before the mock connects so the manager sees the connected transition.
                    scripts.attach(connectionManager)
                    #if DEBUG
                    if ProcessInfo.processInfo.environment["BB_MOCK_MODE"] == "1" {
                        connectionManager.connectMock()
                    }
                    #endif
                }
        }
        .onChange(of: scenePhase) { _, phase in
            scripts.setAppActive(phase == .active)
        }
    }
}
