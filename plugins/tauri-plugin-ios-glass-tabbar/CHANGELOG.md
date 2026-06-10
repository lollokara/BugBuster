# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-06-09

Initial release.

### Added

- Native iOS 26 Liquid Glass `UITabBar` injected into the Tauri window.
- Web → native commands: `setItems`, `setActiveTab`, `setHidden`, `setBadge`.
- Native → web event: `tabSelected` (`{ key, index }`).
- No-op implementations off iOS so cross-platform apps build unchanged.
- JavaScript/TypeScript API package `tauri-plugin-ios-glass-tabbar-api`.

[0.1.0]: https://github.com/tuanle96/tauri-plugin-ios-glass-tabbar/releases/tag/v0.1.0
