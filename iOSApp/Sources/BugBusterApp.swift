import SwiftUI

struct BugBusterApp: App {
    @StateObject private var connectionManager = ConnectionManager()
    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            MainTabView()
                .environmentObject(connectionManager)
        }
    }
}
