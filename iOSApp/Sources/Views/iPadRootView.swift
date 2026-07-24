import SwiftUI

/// iPad shell: `NavigationSplitView` sidebar + detail pane, replacing the iPhone
/// floating tab bar. Reuses the exact same tab view bodies as `MainTabView` — no
/// duplication of tab content, only the navigation chrome differs.
struct iPadRootView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var selectedSection: Int? = AppSections.initialSectionFromEnvironment

    var body: some View {
        Group {
            if connectionManager.connectionState == .connected {
                NavigationSplitView {
                    SidebarView(selection: $selectedSection)
                } detail: {
                    ZStack {
                        ConnectedBackgroundView()

                        Group {
                            switch selectedSection ?? 0 {
                            case 0:  OverviewTab()
                            case 1:  SignalPathTab()
                            case 2:  ScopeTab()
                            case 3:  DiagnosticsTab()
                            case 4:  ScriptsTab()
                            default: OverviewTab()
                            }
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                        ToastOverlayView()
                    }
                    .navigationBarTitleDisplayMode(.inline)
                    // ScriptsTab's REPL manages its own keyboard clearance
                    // (keyboardWillShow/Hide tracking); without this the
                    // system also squeezes content, double-avoiding the
                    // keyboard. See .mex/patterns/ios-glass-shell.md.
                    .ignoresSafeArea(.keyboard)
                }
                .navigationSplitViewStyle(.balanced)
            } else {
                ConnectionDashboardView()
            }
        }
        .preferredColorScheme(.dark)
    }
}
