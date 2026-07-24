import SwiftUI

struct BugBusterApp: App {
    @StateObject private var connectionManager = ConnectionManager()
    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            AdaptiveRootView()
                .environmentObject(connectionManager)
                .onAppear {
                    #if DEBUG
                    if ProcessInfo.processInfo.environment["BB_MOCK_MODE"] == "1" {
                        connectionManager.connectMock()
                    }
                    #endif
                }
        }
    }
}
