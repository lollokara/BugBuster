import SwiftUI

/// iPad shell: `NavigationSplitView` sidebar + detail pane, replacing the iPhone
/// floating tab bar. Reuses the exact same tab view bodies as `MainTabView` — no
/// duplication of tab content, only the navigation chrome differs.
struct iPadRootView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @State private var selectedSection: Int? = AppSections.initialSectionFromEnvironment

    /// Collapse the sidebar after this long without interaction. The scope is
    /// the primary surface on iPad and the sidebar eats a third of the width.
    static let sidebarIdleTimeout: TimeInterval = 15

    @State private var columnVisibility: NavigationSplitViewVisibility = .all
    @State private var idleTask: Task<Void, Never>?

    var body: some View {
        Group {
            if connectionManager.connectionState == .connected {
                NavigationSplitView(columnVisibility: $columnVisibility) {
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
                .onAppear { noteInteraction() }
                .onChange(of: selectedSection) { _, _ in noteInteraction() }
                .onChange(of: columnVisibility) { _, new in
                    // Reopening restarts the countdown; collapsing stops it so a
                    // cancelled task can't re-collapse an already-collapsed pane.
                    if new == .all { noteInteraction() } else { idleTask?.cancel() }
                }
                .onDisappear { idleTask?.cancel() }
            } else {
                ConnectionDashboardView()
            }
        }
        .preferredColorScheme(.dark)
    }

    /// Restart the idle countdown. Called on any interaction that should keep
    /// the sidebar open; the task is cancelled and replaced so the timeout is
    /// always measured from the LAST interaction, not the first.
    private func noteInteraction() {
        idleTask?.cancel()
        idleTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: UInt64(Self.sidebarIdleTimeout * 1_000_000_000))
            guard !Task.isCancelled else { return }
            withAnimation { columnVisibility = .detailOnly }
        }
    }
}
