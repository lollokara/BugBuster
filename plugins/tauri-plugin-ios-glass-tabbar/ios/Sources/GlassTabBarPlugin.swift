import Tauri
import UIKit
import WebKit

// A tab item described by the web layer.
struct GlassTabItem: Decodable {
  let key: String
  let title: String
  let sfSymbol: String
}

class SetItemsArgs: Decodable {
  let items: [GlassTabItem]
  let selectedIndex: Int?
}

class SetActiveTabArgs: Decodable {
  let index: Int
}

class SetHiddenArgs: Decodable {
  let hidden: Bool
}

class SetBadgeArgs: Decodable {
  let index: Int
  let value: String?
}

// Native iOS 26 Liquid Glass tab bar for a Tauri (WKWebView) app.
//
// A standard UITabBar adopts Liquid Glass automatically when built with the
// iOS 26 SDK — something CSS cannot reproduce in WKWebView (WebKit has no
// SVG-displacement backdrop-filter). Tauri/tao owns the UIWindow, so we inject
// the bar into the key window and bridge it:
//   web  -> native : setItems / setActiveTab / setHidden / setBadge commands
//   native -> web  : the `tabSelected` plugin event ({ key, index })
class GlassTabBarPlugin: Plugin, UITabBarDelegate {
  private var tabBar: UITabBar?
  private var keys: [String] = []

  @objc public func setItems(_ invoke: Invoke) throws {
    let args = try invoke.parseArgs(SetItemsArgs.self)
    DispatchQueue.main.async {
      guard let window = self.keyWindow() else {
        invoke.reject("no key window")
        return
      }
      let bar = self.tabBar ?? self.makeTabBar(in: window)
      self.keys = args.items.map { $0.key }
      bar.items = args.items.enumerated().map { (i, it) in
        UITabBarItem(title: it.title, image: UIImage(systemName: it.sfSymbol), tag: i)
      }
      let sel = args.selectedIndex ?? 0
      if sel >= 0, sel < (bar.items?.count ?? 0) { bar.selectedItem = bar.items?[sel] }
      invoke.resolve()
    }
  }

  @objc public func setActiveTab(_ invoke: Invoke) throws {
    let args = try invoke.parseArgs(SetActiveTabArgs.self)
    DispatchQueue.main.async {
      if let items = self.tabBar?.items, args.index >= 0, args.index < items.count {
        self.tabBar?.selectedItem = items[args.index]
      }
      invoke.resolve()
    }
  }

  @objc public func setHidden(_ invoke: Invoke) throws {
    let args = try invoke.parseArgs(SetHiddenArgs.self)
    DispatchQueue.main.async {
      self.tabBar?.isHidden = args.hidden
      invoke.resolve()
    }
  }

  @objc public func setBadge(_ invoke: Invoke) throws {
    let args = try invoke.parseArgs(SetBadgeArgs.self)
    DispatchQueue.main.async {
      if let items = self.tabBar?.items, args.index >= 0, args.index < items.count {
        items[args.index].badgeValue = args.value
      }
      invoke.resolve()
    }
  }

  // MARK: - Internals

  private func makeTabBar(in window: UIWindow) -> UITabBar {
    let bar = UITabBar()
    bar.delegate = self
    bar.translatesAutoresizingMaskIntoConstraints = false
    // Default appearance on the iOS 26 SDK == Liquid Glass; keep it.
    window.addSubview(bar)
    NSLayoutConstraint.activate([
      bar.leadingAnchor.constraint(equalTo: window.leadingAnchor),
      bar.trailingAnchor.constraint(equalTo: window.trailingAnchor),
      bar.bottomAnchor.constraint(equalTo: window.bottomAnchor),
    ])
    self.tabBar = bar
    return bar
  }

  private func keyWindow() -> UIWindow? {
    for scene in UIApplication.shared.connectedScenes {
      guard let ws = scene as? UIWindowScene else { continue }
      if let key = ws.windows.first(where: { $0.isKeyWindow }) { return key }
      if let first = ws.windows.first { return first }
    }
    return nil
  }

  func tabBar(_ tabBar: UITabBar, didSelect item: UITabBarItem) {
    let i = item.tag
    print("[GlassTabBar] didSelect item: \(i), keys count: \(keys.count)")
    guard i >= 0, i < keys.count else { 
      print("[GlassTabBar] index out of bounds or keys empty")
      return 
    }
    let key = keys[i]
    print("[GlassTabBar] triggering tabSelected with key: \(key)")
    trigger("tabSelected", data: ["key": key, "index": i])
  }
}

@_cdecl("init_plugin_ios_glass_tabbar")
func initPlugin() -> Plugin {
  return GlassTabBarPlugin()
}
