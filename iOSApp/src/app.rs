use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

use crate::components::connection::ConnectionPanel;
use crate::tabs::{
    diag::*, overview::*,
};
use crate::tauri_bridge::*;

#[derive(Clone, Copy)]
pub struct OtaContext {
    pub git_releases: ReadSignal<Vec<DesktopGitRelease>>,
    pub esp32_current_version: ReadSignal<String>,
    pub rp2040_current_version: ReadSignal<String>,
    pub ota_progress: ReadSignal<OtaProgress>,
    pub ota_active: ReadSignal<bool>,
    pub ota_error: ReadSignal<Option<String>>,
    pub ota_success: ReadSignal<bool>,
    pub set_ota_active: WriteSignal<bool>,
    pub set_ota_error: WriteSignal<Option<String>>,
    pub set_ota_success: WriteSignal<bool>,
    pub set_ota_progress: WriteSignal<OtaProgress>,
}

fn parse_version(v: &str) -> Vec<u32> {
    let clean = v.trim_start_matches('v');
    clean
        .split('.')
        .map(|s| s.parse::<u32>().unwrap_or(0))
        .collect()
}

pub fn is_version_newer(current: &str, latest: &str) -> bool {
    let curr_parts = parse_version(current);
    let lat_parts = parse_version(latest);
    for i in 0..std::cmp::max(curr_parts.len(), lat_parts.len()) {
        let c = curr_parts.get(i).cloned().unwrap_or(0);
        let l = lat_parts.get(i).cloned().unwrap_or(0);
        if l > c {
            return true;
        } else if c > l {
            return false;
        }
    }
    false
}

#[component]
pub fn App() -> impl IntoView {
    let (devices, set_devices) = signal(Vec::<DiscoveredDevice>::new());
    let (conn_mode, set_conn_mode) = signal("Disconnected".to_string());
    let (conn_addr, set_conn_addr) = signal(String::new());
    let (scanning, set_scanning) = signal(false);
    let (scan_completed, set_scan_completed) = signal(false);
    let (device_state, set_device_state) = signal(DeviceState::default());
    let (active_tab, set_active_tab) = signal("overview".to_string());

    let (git_releases, set_git_releases) = signal(Vec::<DesktopGitRelease>::new());
    let (esp32_current_version, set_esp32_current_version) = signal(String::new());
    let (rp2040_current_version, set_rp2040_current_version) = signal(String::new());
    let (ota_progress, set_ota_progress) = signal(OtaProgress::default());
    let (ota_active, set_ota_active) = signal(false);
    let (ota_error, set_ota_error) = signal(Option::<String>::None);
    let (ota_success, set_ota_success) = signal(false);

    provide_context(OtaContext {
        git_releases,
        esp32_current_version,
        rp2040_current_version,
        ota_progress,
        ota_active,
        ota_error,
        ota_success,
        set_ota_active,
        set_ota_error,
        set_ota_success,
        set_ota_progress,
    });

    // Toast notification system
    let (toasts, set_toasts) = signal(Vec::<(String, String, f64)>::new());

    spawn_local(async move {
        let closure = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
            if let Ok(detail) = js_sys::Reflect::get(&event, &"detail".into()) {
                if let (Some(msg), Some(kind)) = (
                    js_sys::Reflect::get(&detail, &"msg".into()).ok().and_then(|v| v.as_string()),
                    js_sys::Reflect::get(&detail, &"kind".into()).ok().and_then(|v| v.as_string()),
                ) {
                    let timestamp = js_sys::Date::now();
                    let mut list = toasts.get_untracked();
                    list.push((msg, kind, timestamp));
                    set_toasts.set(list);

                    let set_toasts_cleanup = set_toasts.clone();
                    spawn_local(async move {
                        sleep_ms(4000).await;
                        let mut list = toasts.get_untracked();
                        list.retain(|(_, _, ts)| *ts != timestamp);
                        set_toasts_cleanup.set(list);
                    });
                }
            }
        });

        if let Some(window) = web_sys::window() {
            let _ = window.add_event_listener_with_callback(
                "bb-toast",
                closure.as_ref().unchecked_ref(),
            );
        }
        closure.forget();
    });

    // Scan for devices
    let scan = move |_: ev::MouseEvent| {
        set_scanning.set(true);
        spawn_local(async move {
            let result = try_invoke("discover_devices", JsValue::NULL).await;
            if let Some(devs) =
                result.and_then(|r| serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(r).ok())
            {
                set_devices.set(devs);
            }
            set_scanning.set(false);
            set_scan_completed.set(true);
        });
    };

    let disconnect = move |_: ev::MouseEvent| {
        spawn_local(async move {
            try_invoke("disconnect_device", JsValue::NULL).await;
        });
        set_conn_mode.set("Disconnected".to_string());
        set_conn_addr.set(String::new());
        set_device_state.set(DeviceState::default());
    };

    // Fetch GitHub releases on startup
    spawn_local(async move {
        if let Ok(rels) = fetch_github_releases().await {
            set_git_releases.set(rels);
        }
    });

    // Event listeners
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<DeviceState>>(event) {
                set_device_state.set(evt.payload);
            }
        });
        listen("device-state", &closure).await;
        closure.forget();
    });

    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<ConnectionStatus>>(event) {
                set_conn_mode.set(evt.payload.mode.clone());
                set_conn_addr.set(evt.payload.port_or_url.clone());
            }
        });
        listen("connection-status", &closure).await;
        closure.forget();
    });

    // Listen for desktop OTA progress
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<OtaProgress>>(event) {
                let p = evt.payload;
                if p.stage == "error" {
                    set_ota_active.set(false);
                    set_ota_error.set(Some(p.message.clone()));
                } else if p.stage == "done" {
                    set_ota_active.set(false);
                    set_ota_success.set(true);
                    set_ota_progress.set(p);
                } else {
                    set_ota_progress.set(p);
                }
            }
        });
        listen("desktop-ota-progress", &closure).await;
        closure.forget();
    });

    // Fetch versions and update GitHub releases list on connection
    Effect::new(move |_| {
        let mode = conn_mode.get();
        if mode != "Disconnected" {
            spawn_local(async move {
                if let Ok(rels) = fetch_github_releases().await {
                    set_git_releases.set(rels);
                }
            });
            spawn_local(async move {
                if let Some(info) = fetch_firmware_info().await {
                    set_esp32_current_version.set(info.fw_version);
                }
            });
            spawn_local(async move {
                if let Some(caps) = hat_get_caps().await {
                    let version = format!("{}.{}", caps.fw_major, caps.fw_minor);
                    set_rp2040_current_version.set(version);
                }
            });
        } else {
            set_esp32_current_version.set(String::new());
            set_rp2040_current_version.set(String::new());
        }
    });

    // Continuous background scanning loop when disconnected
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    let alive_scan = alive.clone();
    Effect::new(move |_| {
        let mode = conn_mode.get();
        if mode == "Disconnected" {
            let alive_scan = alive_scan.clone();
            spawn_local(async move {
                loop {
                    if !alive_scan.load(std::sync::atomic::Ordering::Relaxed) { break; }
                    if conn_mode.get_untracked() != "Disconnected" { break; }
                    
                    let result = try_invoke("discover_devices", JsValue::NULL).await;
                    if let Some(devs) = result.and_then(|r| serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(r).ok()) {
                        set_devices.set(devs);
                    }
                    set_scan_completed.set(true);
                    
                    sleep_ms(3000).await;
                }
            });
        }
    });

    view! {
        <div class="app ios-minified">
            // Header
            <Show when=move || conn_mode.get() != "Disconnected">
                <header class="header">
                    <div class="header-left">
                        <span class="logo-text">"BugBuster"</span>
                    </div>
                    <div class="header-right">
                        <div class="status-bar" style="display: flex; align-items: center; gap: 10px;">
                            <span class="status-text" style="font-size: 0.8rem; color: #888;">{move || conn_addr.get()}</span>
                            <button class="btn btn-danger btn-xs" on:click=disconnect>"Disconnect"</button>
                        </div>
                    </div>
                </header>
            </Show>

            // Connection panel (when disconnected)
            <Show when=move || conn_mode.get() == "Disconnected">
                <ConnectionPanel
                    devices=devices.into()
                    scanning=scanning.into()
                    scan_completed=scan_completed.into()
                    on_scan=Callback::new(scan)
                />
            </Show>

            // Main content (when connected)
            <Show when=move || conn_mode.get() != "Disconnected">
                // Tab bar (Bottom for iOS)
                <nav class="mobile-tab-bar">
                    <button class="tab-item"
                        class:active=move || active_tab.get() == "overview"
                        on:click=move |_| set_active_tab.set("overview".to_string())
                    >
                        <span class="icon">"📊"</span>
                        "Overview"
                    </button>
                    <button class="tab-item"
                        class:active=move || active_tab.get() == "diag"
                        on:click=move |_| set_active_tab.set("diag".to_string())
                    >
                        <span class="icon">"🩺"</span>
                        "Diag"
                    </button>
                </nav>

                // Tab content
                <div class="tab-container">
                    {move || match active_tab.get().as_str() {
                        "overview" => view! { <OverviewTab state=device_state /> }.into_any(),
                        "diag" => view! { <DiagTab state=device_state /> }.into_any(),
                        _ => view! { <div>"Unknown tab"</div> }.into_any(),
                    }}
                </div>
            </Show>

            // Toast notifications
            <div class="toast-container">
                {move || toasts.get().into_iter().map(|(msg, kind, _ts)| {
                    let class = match kind.as_str() {
                        "ok" => "toast toast-ok",
                        "err" => "toast toast-err",
                        _ => "toast toast-info",
                    };
                    view! { <div class=class>{msg}</div> }
                }).collect::<Vec<_>>()}
            </div>
        </div>
    }
}
