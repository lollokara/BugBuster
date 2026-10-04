import SwiftUI

/// iPad shell: `NavigationSplitView` sidebar + detail pane, replacing the iPhone
/// floating tab bar. Reuses the exact same tab view bodies as `MainTabView` — no
/// duplication of tab content, only the navigation chrome differs.
struct iPadRootView: View {
    @EnvironmentObject var connectionManager: ConnectionManager
    @EnvironmentObject var scripts: ScriptRunManager
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
                    GeometryReader { geo in
                        let logWidth = ScriptLogColumnLayout.columnWidth(for: geo.size.width)
                        ZStack {
                            ConnectedBackgroundView()

                            HStack(spacing: 0) {
                                Group {
                                    switch selectedSection ?? 0 {
                                    case 0:  OverviewTab()
                                    case 1:  SignalPathTab()
                                    case 2:  ScopeTab()
                                    case 3:  DiagnosticsTab()
                                    case 4:  ScriptsTab()
                                    case 5:  BatterySimTab()
                                    default: OverviewTab()
                                    }
                                }
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                                // Outside the section switch so it survives section changes (spec §4).
                                // Uses safeAreaInset so scroll views reserve space instead of being covered (Findings #22, #24).
                                .safeAreaInset(edge: .bottom, spacing: 0) {
                                    ScriptPillHost(onScriptsTab: (selectedSection ?? 0) == 4)
                                        .padding(.horizontal, 24)
                                        .padding(.bottom, 16)
                                }
                                .frame(minWidth: 0, maxWidth: .infinity)
                                .clipped()

                                if scripts.consoleVisible {
                                    Divider().opacity(0.3)
                                    ScriptLogPanel()
                                        .frame(width: logWidth)
                                        .transition(.move(edge: .trailing))
                                }
                            }
                            .animation(.snappy, value: scripts.consoleVisible)

                            ToastOverlayView()
                        }
                        .onAppear {
                            checkSidebarCollapseForConsole(detailWidth: geo.size.width)
                        }
                        .onChange(of: geo.size.width) { _, width in
                            checkSidebarCollapseForConsole(detailWidth: width)
                        }
                    }
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar {
                        ToolbarItem(placement: .topBarTrailing) { ScriptLogToggleButton() }
                    }
                    // ScriptsTab's REPL manages its own keyboard clearance
                    // (keyboardWillShow/Hide tracking); without this the
                    // system also squeezes content, double-avoiding the
                    // keyboard. See .mex/patterns/ios-glass-shell.md.
                    .ignoresSafeArea(.keyboard)
                }
                .navigationSplitViewStyle(.balanced)
                .onAppear { noteInteraction() }
                .onChange(of: selectedSection) { _, _ in noteInteraction() }
                .onChange(of: connectionManager.lastHatStatus?.isDaqHat ?? false) { _, daq in
                    if !daq && selectedSection == 5 { selectedSection = 0 }
                }
                .onChange(of: columnVisibility) { _, new in
                    // Reopening restarts the countdown; collapsing stops it so a
                    // cancelled task can't re-collapse an already-collapsed pane.
                    if new == .all { noteInteraction() } else { idleTask?.cancel() }
                }
                .onChange(of: scripts.consoleVisible) { _, visible in
                    if visible {
                        withAnimation(.snappy) { columnVisibility = .detailOnly }
                    }
                }
                .onDisappear { idleTask?.cancel() }
            } else {
                ConnectionDashboardView()
            }
        }
        .preferredColorScheme(.dark)
    }

    /// Collapse sidebar if detail pane width is constrained with the log console open (Finding #25).
    private func checkSidebarCollapseForConsole(detailWidth: CGFloat) {
        if scripts.consoleVisible && detailWidth < 750 && columnVisibility != .detailOnly {
            withAnimation(.snappy) {
                columnVisibility = .detailOnly
            }
        }
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
