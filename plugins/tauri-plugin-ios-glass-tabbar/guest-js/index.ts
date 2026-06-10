import { invoke, addPluginListener, type PluginListener } from '@tauri-apps/api/core'

export interface TabItem {
  /** Id echoed back on the `tabSelected` event. */
  key: string
  title: string
  /** SF Symbol name, e.g. "message", "person.2", "gearshape". */
  sfSymbol: string
}

export interface TabSelectedEvent {
  key: string
  index: number
}

/**
 * Install / replace the native tab bar items and select one. iOS only — a no-op
 * on every other platform, so cross-platform call sites stay simple.
 */
export async function setItems(items: TabItem[], selectedIndex = 0): Promise<void> {
  await invoke('plugin:ios-glass-tabbar|set_items', { payload: { items, selectedIndex } })
}

/** Programmatically select a tab (e.g. when web navigation changes the tab). */
export async function setActiveTab(index: number): Promise<void> {
  await invoke('plugin:ios-glass-tabbar|set_active_tab', { payload: { index } })
}

/** Hide/show the bar — hide it on full-screen pushed views so it doesn't overlap. */
export async function setHidden(hidden: boolean): Promise<void> {
  await invoke('plugin:ios-glass-tabbar|set_hidden', { payload: { hidden } })
}

/** Set (or clear, with `null`) the badge on a tab. */
export async function setBadge(index: number, value: string | null): Promise<void> {
  await invoke('plugin:ios-glass-tabbar|set_badge', { payload: { index, value } })
}

/** Subscribe to native tab taps. Returns a listener you can `.unregister()`. */
export async function onTabSelected(
  handler: (event: TabSelectedEvent) => void,
): Promise<PluginListener> {
  return addPluginListener('ios-glass-tabbar', 'tabSelected', handler)
}
