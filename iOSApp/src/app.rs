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
    let (toasts, _set_toasts) = signal(Vec::<(String, String, f64)>::new());

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

    // Auto-scan on startup
    spawn_local(async move {
        let result = try_invoke("discover_devices", JsValue::NULL).await;
        if let Some(devs) =
            result.and_then(|r| serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(r).ok())
        {
            set_devices.set(devs);
        }
        set_scan_completed.set(true);
    });

    view! {
        <div class="app ios-minified">
            // Header
            <header class="header">
                <div class="header-left">
                    <span class="logo-text">"BugBuster"</span>
                </div>
                <div class="header-right">
                    {move || {
                        let m = conn_mode.get();
                        if m != "Disconnected" {
                            view! {
                                <div class="status-bar" style="display: flex; align-items: center; gap: 10px;">
                                    <span class="status-text" style="font-size: 0.8rem; color: #888;">{move || conn_addr.get()}</span>
                                    <button class="btn btn-danger btn-xs" on:click=disconnect>"Disconnect"</button>
                                </div>
                            }.into_any()
                        } else {
                            view! { <></> }.into_any()
                        }
                    }}
                </div>
            </header>

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
