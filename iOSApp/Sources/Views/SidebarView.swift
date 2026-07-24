import SwiftUI

/// iPad sidebar for `iPadRootView`. Reads `AppSections.all` (shared with the
/// iPhone `CustomTabBar`) so the two shells can't drift on icon/label.
///
/// Uses a native `List(.sidebar)` rather than a custom glass background: iPad
/// sidebars already render with the system material, so layering another
/// `.ultraThinMaterial` on top would violate the "one glass layer per chrome
/// element" rule (see `.mex/patterns/ios-glass-shell.md`).
struct SidebarView: View {
    @Binding var selection: Int?

    var body: some View {
        List(selection: $selection) {
            ForEach(AppSections.all) { section in
                Label(section.name, systemImage: section.icon)
                    .tag(section.id)
            }
        }
        .listStyle(.sidebar)
        .navigationTitle("BugBuster")
    }
}
