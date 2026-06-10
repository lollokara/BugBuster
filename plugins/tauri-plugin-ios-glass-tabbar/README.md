# tauri-plugin-ios-glass-tabbar

[![crates.io](https://img.shields.io/crates/v/tauri-plugin-ios-glass-tabbar.svg)](https://crates.io/crates/tauri-plugin-ios-glass-tabbar)
[![npm](https://img.shields.io/npm/v/tauri-plugin-ios-glass-tabbar-api.svg)](https://www.npmjs.com/package/tauri-plugin-ios-glass-tabbar-api)
[![Test](https://github.com/tuanle96/tauri-plugin-ios-glass-tabbar/actions/workflows/test.yml/badge.svg)](https://github.com/tuanle96/tauri-plugin-ios-glass-tabbar/actions/workflows/test.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A **native iOS 26 "Liquid Glass" `UITabBar`** for [Tauri](https://tauri.app) apps, bridged to your
web-side navigation.

## Why a native tab bar?

You **cannot** render real Liquid Glass with CSS inside a Tauri app. Tauri runs your UI in a
`WKWebView` (WebKit), and the web technique for Liquid Glass — an SVG `feDisplacementMap` used as
`backdrop-filter: url(#…)` — is **Chromium-only** (WebKit doesn't support SVG filters in
`backdrop-filter`; [bug #245510](https://bugs.webkit.org/show_bug.cgi?id=245510)). In the webview you
can only get a flat frosted blur, not real refractive glass.

Real Liquid Glass only comes from **native UIKit**: a standard `UITabBar` adopts Liquid Glass
automatically when built with the iOS 26 SDK. This plugin injects that native bar into your app's
window and bridges it to your web nav:

- **web → native**: `setItems` / `setActiveTab` / `setHidden` / `setBadge`
- **native → web**: the `tabSelected` event (`{ key, index }`)

On older iOS it falls back to the standard `UITabBar` look; on non-iOS platforms every call is a
no-op, so cross-platform code stays simple.

> Heads-up: this plugin injects native UI chrome into the window — a little unusual for a Tauri
> plugin. Keep an HTML tab bar as a fallback and hide it once `setItems` resolves.

## Install

**Rust** (`src-tauri/Cargo.toml`):

```toml
[dependencies]
tauri-plugin-ios-glass-tabbar = "0.1"
```

**JavaScript**:

```sh
npm install tauri-plugin-ios-glass-tabbar-api
# or: pnpm add / yarn add / bun add
```

Register it in `src-tauri/src/lib.rs`:

```rust
tauri::Builder::default()
    .plugin(tauri_plugin_ios_glass_tabbar::init())
    // …
```

Allow the commands in `src-tauri/capabilities/default.json`:

```json
{ "permissions": ["ios-glass-tabbar:default"] }
```

## Usage

```ts
import {
  setItems, setActiveTab, setHidden, setBadge, onTabSelected,
} from 'tauri-plugin-ios-glass-tabbar-api'

// 1. Install the bar (SF Symbol names for the icons).
await setItems([
  { key: 'chats',    title: 'Messages', sfSymbol: 'message' },
  { key: 'contacts', title: 'Contacts', sfSymbol: 'person.2' },
  { key: 'settings', title: 'Settings', sfSymbol: 'gearshape' },
], 0)

// 2. React to native taps -> drive your router.
await onTabSelected(({ key }) => router.go(key))

// 3. Mirror web state back to the native bar.
await setActiveTab(1)          // when nav changes programmatically
await setHidden(true)          // hide on a full-screen pushed view
await setBadge(0, '3')         // or setBadge(0, null) to clear
```

The native bar overlays the webview (translucent glass → content scrolls under it), so reserve
bottom padding in your web content and hide the bar on pushed full-screen screens.

## How it works

Tauri/tao creates the `UIWindow` + `WKWebView` from Rust; there is no Swift view controller. The
plugin's iOS Swift (`GlassTabBarPlugin`) finds the key window at runtime, adds a `UITabBar` pinned to
the bottom (default iOS 26 appearance = Liquid Glass), and uses Tauri's plugin `trigger()` to emit
`tabSelected` back to the web layer. Commands set the items, selection, visibility, and badges.

## Requirements

- iOS 13+ to run; **iOS 26 SDK (Xcode 26+)** for the Liquid Glass material.
- Tauri v2 with the **iOS plugin Swift layer wired up** — i.e. your app actually compiles and
  links Tauri plugins' Swift (the standard setup; how `addPluginListener` / `invoke('plugin:…')`
  reach native code). This plugin is a normal Tauri `Plugin`, so it links the same way as any other.

## Not linking? (apps without the Tauri Swift plugin layer)

A few apps run a **minimal iOS setup** that only uses plugins' *Rust* side and never compiles any
plugin Swift (no SwiftPM `packages:` in the xcodegen project, no generated `Package.swift`). There,
this plugin won't link — `register_ios_plugin(init_plugin_ios_glass_tabbar)` resolves to a Swift
`@_cdecl` symbol that was never built, so the app fails at link time.

You don't need the plugin in that case — you can inject the same native `UITabBar` with a
**self-contained `.mm` that does *not* `import Tauri`**: drop an Objective-C++ file into your app
target's sources, find the key window + `WKWebView` at runtime (e.g. on
`UIApplicationDidBecomeActiveNotification`), add a `UITabBar` pinned to the bottom, and bridge it
yourself — native→web via `webView.evaluateJavaScript("window.__setTab('…')")`, web→native via a
`WKScriptMessageHandler`. It's more boilerplate and no type-safety, but it has zero Tauri-Swift
dependency. The plugin's [`GlassTabBarPlugin.swift`](ios/Sources/GlassTabBarPlugin.swift) is a good
reference for the window/tab-bar wiring; just swap the Tauri `Invoke`/`trigger` calls for the raw
JS bridge above.

## License

MIT © Lê Anh Tuấn
