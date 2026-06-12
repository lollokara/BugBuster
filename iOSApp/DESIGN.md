# Design

## Source of truth
- Status: Active
- Last refreshed: 2026-06-12
- Primary product surfaces: `iOSApp/Sources/Views/MainTabView.swift`, `iOSApp/Sources/Views/OverviewTab.swift`, `iOSApp/Sources/Views/SignalPathTab.swift`, `iOSApp/Sources/Views/ScopeTab.swift`, `iOSApp/Sources/Views/DiagnosticsTab.swift`, `iOSApp/Sources/Views/ScriptsTab.swift`
- Evidence reviewed: `iOSApp/Sources/Views/MainTabView.swift`, `iOSApp/Sources/Views/OverviewTab.swift`, `iOSApp/Sources/Views/SignalPathTab.swift`, `iOSApp/Sources/Views/ScopeTab.swift`, `iOSApp/Sources/Views/DiagnosticsTab.swift`, `iOSApp/Sources/Views/ScriptsTab.swift`, `iOSApp/Sources/BugBusterApp.swift`, `iOSApp/DESIGN 2.md`, `.mex/patterns/ios-glass-shell.md`, Apple HIG Materials, Liquid Glass, WWDC25 "Meet Liquid Glass", WWDC25 "Build a SwiftUI app with the new design", and Apple Design "What's new"

## Brand
- Personality: a precise bench-instrument control app wrapped in a premium dark glass shell
- Trust signals: live connection state stays visible, measured values use monospaced numerals, channel identity is deterministic, and shell chrome never steals attention from the data
- Avoid: marketing-gallery composition, decorative gradients on cards or controls, low-contrast translucent layers, hard slabs, and motion that exists only to look modern

## Product goals
- Goals: let the operator see hardware state quickly, keep navigation stable, make touch interactions safe on a phone, and preserve live status while moving between tabs
- Non-goals: mimic desktop chrome exactly, turn the app into a photo-first showcase, over-animate the shell, or bury important state behind trendy translucency
- Success signals: the shell reads as native, the tab bar floats cleanly, the Overview header never collides with the notch, and channel/state cards are scannable at a glance

## Personas and jobs
- Primary personas: hardware developers, test engineers, and lab operators
- User jobs: check live bench status, change channels and rails, inspect signal routing, run diagnostics, and launch scripts or REPL actions
- Key contexts of use: one-handed mobile control, noisy lab environments, and quick glance checks during active measurement

## Information architecture
- Primary navigation: bottom tab bar with Overview, Signal Path, Scope, Diagnostics, and Scripts
- Core routes/screens: disconnected connection dashboard, authenticated shell, Overview detail, channel configuration sheet, script browser/editor, REPL, Wi-Fi setup, OTA/update surfaces, and scope stream controls
- Content hierarchy: shell chrome first, connection state second, then live measurements and controls, then deeper tooling and maintenance flows

## Design principles
- Principle 1: keep shell chrome fixed, soft, and outside scroll content
- Principle 2: use color to identify channels and state, not as decoration
- Principle 3: prefer rounded continuous geometry and system materials over hard rectangles and custom blur stacks
- Principle 4: adopt Liquid Glass where it improves navigation or control clarity, but keep legibility above visual novelty
- Tradeoffs: use enough blur and translucency to feel current, but stop as soon as text, affordance, or touch precision starts to degrade

## Visual language
- Color: deep obsidian and navy backgrounds, cyan/blue as the primary interactive accent, and blue/emerald/amber/purple as stable channel or semantic colors; background gradients may add atmosphere, but controls and cards stay low-chroma
- Typography: use the SF system stack already in the app; titles can stay bold and rounded, body text should remain calm and compact, and voltages/codes/counters should use monospaced numerals
- Spacing/layout rhythm: generous vertical breathing room, consistent card gutters, and fixed chrome placed with `safeAreaInset` so it never touches unsafe edges
- Shape/radius/elevation: continuous rounded rectangles, capsules for status pills, circular icon buttons, regular glass for floating shell elements, and soft shadows only where a control needs separation from the background
- Motion: short spring animations only for tab switches, toggles, and active-state highlights; no continuous motion, glow pumping, or decorative parallax
- Imagery/iconography: SF Symbols with restrained weight and simple meaning; if imagery is added later, keep it functional and non-decorative

## Components
- Existing components to reuse: `CustomTabBar`, `OverviewTab` header, `GlassEffectContainer`, `glassEffect` cards, connection status pills, channel cards, `VoltageSliderRow`, `ToggleRow`, `StatusPill`, `RegisterBitIndicator`, and the `ScriptsTab` browser/editor/repl stack
- New/changed components: shell-level glass tab bar and pinned Overview header treatment, the disconnected connection dashboard, tab-specific full-bleed backgrounds, and channel-colored overview cards that stay local to their section
- Variants and states: disconnected, connecting, unauthorized, connected, live, paused, hat present, hat absent, enabled, off, fault, warning, and error
- Token/component ownership: shell chrome belongs in `MainTabView`; Overview chrome and live cards belong in `OverviewTab`; measurement formatting stays local to the tab that renders it

## Accessibility
- Target standard: legible at phone distance with Dynamic Type and Reduce Transparency in mind
- Keyboard/focus behavior: use standard SwiftUI buttons, toggles, and text fields with visible pressed states; do not rely on gesture-only controls for critical actions
- Contrast/readability: do not depend on blur to separate layers; text must stay readable over material and background color changes, and transparent layers should flatten cleanly if the system reduces transparency
- Screen-reader semantics: keep tab labels, connection state, live status, and control labels concise and ordered
- Reduced motion and sensory considerations: respect Reduce Motion, keep live indicators subtle, and avoid flashing or high-frequency shell animation

## Responsive behavior
- Supported breakpoints/devices: iPhone portrait first, with safe-area-aware behavior on notched and non-notched devices; iPad can share the same shell unless a separate tablet IA is approved later
- Layout adaptations: use `safeAreaInset` for fixed chrome, let content scroll underneath it, and allow tab-specific cards and grids to widen naturally on larger screens
- Touch/hover differences: design for thumbs and direct touch, not hover; keep targets large enough to hit without precision

## Interaction states
- Loading: show scanning, connecting, and progress states without moving or replacing the shell
- Empty: use explicit empty states inside cards or lists instead of blank screens
- Error: surface connection, auth, and transport errors clearly and keep recovery actions adjacent
- Success: highlight live/connected state with quiet glass treatments, not loud color floods
- Disabled: dim controls, but keep them readable and clearly disabled
- Offline/slow network, if applicable: preserve last-known values when safe and label them clearly

## Content voice
- Tone: direct, technical, and calm
- Terminology: use the same board, rail, channel, and diagnostic names the hardware exposes
- Microcopy rules: keep labels short, keep status text factual, and avoid exclamation marks or marketing phrasing

## Implementation constraints
- Framework/styling system: SwiftUI with `glassEffect`, `Material`, and native controls
- Design-token constraints: the dark shell palette stays low-chroma; cyan/blue accent can appear in active states; channel colors stay stable and local to the content they annotate
- Performance constraints: prefer native materials over custom blur stacks, keep background effects lightweight, and avoid expensive custom drawing in chrome
- Compatibility constraints: respect safe areas, notch geometry, and Reduce Transparency; the shell must still read correctly when materials flatten
- Test/screenshot expectations: Xcode builds should remain clean, and screenshots should show edge-to-edge background, floating tab bar, pinned Overview header, and legible control states over material

## Open questions
- [ ] Should iPad keep the current bottom-tab shell or move to a sidebar later?
- [ ] Should non-Overview tabs adopt a lighter pinned subheader pattern, or keep their current in-content section titles?
