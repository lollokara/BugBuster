import SwiftUI

struct BugBusterApp: App {
    @StateObject private var connectionManager = ConnectionManager()
    
    var body: some Scene {
        WindowGroup {
            MainTabView()
                .environmentObject(connectionManager)
        }
    }
}
