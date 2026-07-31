import Foundation

/// Single source of truth for the app's top-level sections (icon + name), shared by
/// the iPhone floating tab bar (`CustomTabBar`) and the iPad sidebar (`SidebarView`)
/// so the two shells can't drift. Extend this array to add a new section (e.g. a
/// future DAQ tab) instead of touching the tab bar / sidebar directly.
enum AppSections {
    struct Section: Identifiable {
        let id: Int
        let icon: String
        let name: String
    }

    static let all: [Section] = [
        Section(id: 0, icon: "waveform.path.ecg", name: "Overview"),
        Section(id: 1, icon: "arrow.up.left.and.down.right.and.arrow.up.right.and.down.left", name: "Signal Path"),
        Section(id: 2, icon: "waveform", name: "Scope"),
        Section(id: 3, icon: "cpu", name: "Diagnostics"),
        Section(id: 4, icon: "doc.text.magnifyingglass", name: "Scripts")
    ]

    /// Debug-only: jump straight to a section on launch (`BB_INITIAL_SECTION=<id>`),
    /// for scripted screenshotting without simulator UI automation.
    static var initialSectionFromEnvironment: Int {
        #if DEBUG
        if let raw = ProcessInfo.processInfo.environment["BB_INITIAL_SECTION"], let id = Int(raw) {
            return id
        }
        #endif
        return 0
    }

    /// Debug-only: preset the Scope tab's DAQ timebase on launch (`BB_INITIAL_TIMEBASE=10s|30s|Full`).
    static var initialTimebaseFromEnvironment: String? {
        #if DEBUG
        return ProcessInfo.processInfo.environment["BB_INITIAL_TIMEBASE"]
        #else
        return nil
        #endif
    }
}
