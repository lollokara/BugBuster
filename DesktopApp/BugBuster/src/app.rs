use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

use crate::components::connection::ConnectionPanel;
use crate::components::io_blocked_banner::IoBlockedBanner;
use crate::tabs::{
    adc::*, board::*, daq::*, diag::*, din::*, dout::*, faults::*, gpio::*, hat::*, hv_io::*,
    idac::*, iin::*, ioexp::*, la::*, overview::*, scope::*, signal_path::*, uart::*, usbpd::*,
    vdac::*, voltages::*, wavegen::*,
};
use crate::tauri_bridge::*;

/// Which expansion HAT is currently attached. Drives tab visibility.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum HatKind {
    #[default]
    None,
    La,
    Daq,
}

/// Whether a tab should be shown for the connected device. Overview / System /
/// mainboard tabs are always available; LA and DAQ are gated on their HAT, and
/// the HAT tab only appears when some HAT is attached.
fn tab_visible(tab_id: &str, kind: HatKind) -> bool {
    match tab_id {
        "la" => kind == HatKind::La,
        "daq" => kind == HatKind::Daq,
        "hat" => kind != HatKind::None,
        _ => true,
    }
}

/// Map a tab id to its IO slot footprint. Returns an empty slice if the tab does not claim IOs.
fn tab_slots(tab_id: &str) -> &'static [u8] {
    match tab_id {
        "adc" => crate::tabs::adc::SLOTS,
        "vdac" => crate::tabs::vdac::SLOTS,
        "gpio" => crate::tabs::gpio::SLOTS,
        "din" => crate::tabs::din::SLOTS,
        "dout" => crate::tabs::dout::SLOTS,
        "scope" => crate::tabs::scope::SLOTS,
        "wavegen" => crate::tabs::wavegen::SLOTS,
        "sigpath" => crate::tabs::signal_path::SLOTS,
        "voltages" => crate::tabs::voltages::SLOTS,
        _ => &[],
    }
}

const CATEGORIES: &[(&str, &str, &[(&str, &str)])] = &[
    (
        "overview_cat",
        "Overview",
        &[
            ("overview", "Dashboard"),
            ("board", "Board Map"),
            ("voltages", "Voltages & Cal"),
            ("faults", "Faults"),
            ("diag", "Diagnostics"),
        ],
    ),
    (
        "analog_cat",
        "Analog",
        &[
            ("adc", "ADC"),
            ("vdac", "VDAC"),
            ("idac", "IDAC"),
            ("iin", "IIN"),
        ],
    ),
    (
        "digital_cat",
        "Digital",
        &[
            ("gpio", "GPIO"),
            ("din", "DIN"),
            ("dout", "DOUT"),
            ("hv_io", "HV IO"),
            ("ioexp", "IO Expander"),
        ],
    ),
    (
        "instruments_cat",
        "Instruments",
        &[
            ("scope", "Scope"),
            ("la", "Logic Analyzer"),
            ("daq", "HS DAQ"),
            ("wavegen", "WaveGen"),
            ("sigpath", "Signal Path"),
        ],
    ),
    (
        "system_cat",
        "System",
        &[("hat", "HAT"), ("usbpd", "USB PD"), ("uart", "UART")],
    ),
];

#[component]
pub fn App() -> impl IntoView {
    let (devices, set_devices) = signal(Vec::<DiscoveredDevice>::new());
    let (conn_mode, set_conn_mode) = signal("Disconnected".to_string());

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

    // Fetch GitHub releases on startup
    spawn_local(async move {
        if let Ok(rels) = fetch_github_releases().await {
            set_git_releases.set(rels);
        }
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

    let has_update = move || {
        let releases = git_releases.get();
        if releases.is_empty() {
            return false;
        }
        let esp_curr = esp32_current_version.get();
        let rp_curr = rp2040_current_version.get();
        if esp_curr.is_empty() && rp_curr.is_empty() {
            return false;
        }
        if let Some(latest) = releases.first() {
            let esp_update = if !esp_curr.is_empty() && !latest.esp32_version.is_empty() {
                is_version_newer(&esp_curr, &latest.esp32_version)
            } else {
                false
            };
            let rp_update = if !rp_curr.is_empty() && !latest.rp2040_version.is_empty() {
                is_version_newer(&rp_curr, &latest.rp2040_version)
            } else {
                false
            };
            esp_update || rp_update
        } else {
            false
        }
    };
    let (conn_addr, set_conn_addr) = signal(String::new());
    let (scanning, set_scanning) = signal(false);
    let (scan_completed, set_scan_completed) = signal(false);
    let (device_state, set_device_state) = signal(DeviceState::default());
    let (active_tab, set_active_tab) = signal("overview".to_string());
    // Which HAT is attached — drives which instrument tabs are visible.
    let (hat_kind, set_hat_kind) = signal(HatKind::None);

    // Detect the attached HAT on connect so tabs gate themselves.
    Effect::new(move |_| {
        let mode = conn_mode.get();
        if mode == "Disconnected" {
            set_hat_kind.set(HatKind::None);
            return;
        }
        // The Demo / Mock device pins itself to the DAQ HAT.
        if mode == "Mock" {
            set_hat_kind.set(HatKind::Daq);
            return;
        }
        spawn_local(async move {
            if daq_check_usb().await {
                set_hat_kind.set(HatKind::Daq);
            } else if hat_get_caps().await.is_some() {
                set_hat_kind.set(HatKind::La);
            } else {
                set_hat_kind.set(HatKind::None);
            }
        });
    });

    // If the active tab becomes hidden (e.g. HAT swapped), fall back to Overview.
    Effect::new(move |_| {
        let kind = hat_kind.get();
        let tab = active_tab.get();
        if !tab_visible(&tab, kind) {
            set_active_tab.set("overview".to_string());
        }
    });
    let active_category = move || {
        let tab = active_tab.get();
        for (cat_id, _, tabs) in CATEGORIES {
            if tabs.iter().any(|(t_id, _)| *t_id == tab) {
                return cat_id.to_string();
            }
        }
        "overview_cat".to_string()
    };
    let uart_config = RwSignal::new(UartConfigState::new());

    // Hoist scope UI state so it survives tab switches (Bug 1).
    let scope_ui_state = crate::tabs::scope::ScopeUiState::new();
    provide_context(scope_ui_state);
    // Install app-lifetime scope acquisition manager (Pass 7 — 2.2/2.3):
    // centralises the scope-data listener + start/stop Effect so they survive
    // ScopeTab unmount/remount without producing a restart storm.
    crate::tabs::scope::install_scope_lifetime_manager(scope_ui_state, device_state);

    // Hoist wavegen UI state (Bug Issue 5 — state loss on tab switch).
    provide_context(crate::tabs::wavegen::WavegenUiState::new());

    // IO ownership: None = no conflict, Some(slots) = blocked by another interface.
    let (io_blocked, set_io_blocked) = signal(Option::<Vec<u8>>::None);
    // Owner kind code of the blocking interface (0 = unknown). Updated by both
    // the tab-switch claim path and the io-owner-reject event.
    let (io_blocked_kind, set_io_blocked_kind) = signal(0u8);

    // Effect: on tab change, release old slots and claim new slots.
    // Uses a StoredValue to track the previous tab so we can release it.
    let prev_tab: StoredValue<String> = StoredValue::new(String::new());
    Effect::new(move |_| {
        let new_tab = active_tab.get();
        let old_tab = prev_tab.get_value();
        prev_tab.set_value(new_tab.clone());

        let old_slots = tab_slots(&old_tab).to_vec();
        let new_slots = tab_slots(&new_tab).to_vec();
        let tab_label = new_tab.clone();

        spawn_local(async move {
            // Release slots from the old tab.
            if !old_slots.is_empty() {
                io_release(&old_slots).await;
            }
            // Claim slots for the new tab.
            if !new_slots.is_empty() {
                let ok = io_claim(&new_slots, 5000, &tab_label).await;
                if ok {
                    set_io_blocked_kind.set(0);
                    set_io_blocked.set(None);
                } else {
                    web_sys::console::warn_1(
                        &format!("[app] io_claim blocked for tab '{}'", tab_label).into(),
                    );
                    set_io_blocked.set(Some(new_slots));
                }
            } else {
                set_io_blocked_kind.set(0);
                set_io_blocked.set(None);
            }
        });
    });

    // Toast notification system
    let (toasts, set_toasts) = signal(Vec::<(String, String, f64)>::new()); // (msg, kind, timestamp)

    // Listen for toast events from invoke_with_feedback
    spawn_local(async move {
        let closure: Closure<dyn FnMut(JsValue)> = Closure::new(move |event: JsValue| {
            let event: web_sys::CustomEvent = event.unchecked_into();
            if let Some(detail) = event.detail().dyn_ref::<js_sys::Object>() {
                let msg = js_sys::Reflect::get(detail, &"msg".into())
                    .ok()
                    .and_then(|v| v.as_string())
                    .unwrap_or_default();
                let kind = js_sys::Reflect::get(detail, &"kind".into())
                    .ok()
                    .and_then(|v| v.as_string())
                    .unwrap_or_else(|| "info".into());
                let now = js_sys::Date::now();
                // Errors persist longer (10 s) so users can actually read them;
                // other toasts auto-dismiss after 3 s as before.
                let dismiss_ms: u32 = if kind == "err" { 10_000 } else { 3_000 };
                set_toasts.update(|t| {
                    t.push((msg, kind, now));
                    // Keep max 5 toasts
                    if t.len() > 5 {
                        t.remove(0);
                    }
                });
                let set_t = set_toasts;
                spawn_local(async move {
                    let promise = js_sys::Promise::new(&mut |resolve, _| {
                        if let Some(w) = web_sys::window() {
                            w.set_timeout_with_callback_and_timeout_and_arguments_0(
                                &resolve,
                                dismiss_ms as i32,
                            )
                            .ok();
                        } else {
                            web_sys::console::warn_1(&"toast dismiss: window unavailable".into());
                        }
                    });
                    wasm_bindgen_futures::JsFuture::from(promise).await.ok();
                    set_t.update(|t| {
                        t.retain(|(_, _, ts)| js_sys::Date::now() - ts < dismiss_ms as f64);
                    });
                });
            }
        });
        if let Some(window) = web_sys::window() {
            window
                .add_event_listener_with_callback("bb-toast", closure.as_ref().unchecked_ref())
                .ok();
        }
        // INTENTIONAL: app-lifetime listener — do not cleanup
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
            if let Some(window) = web_sys::window() {
                let update_fn = js_sys::Reflect::get(&window, &"updateScanStatus".into()).unwrap();
                if update_fn.is_function() {
                    let _ = update_fn
                        .unchecked_into::<js_sys::Function>()
                        .call1(&window, &true.into());
                }
            }
        });
    };

    let disconnect = move |_: ev::MouseEvent| {
        spawn_local(async move {
            try_invoke("disconnect_device", JsValue::NULL).await;
            daq_disconnect().await;
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
        // INTENTIONAL: app-lifetime listener — do not cleanup
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
        // INTENTIONAL: app-lifetime listener — do not cleanup
        closure.forget();
    });

    // Listen for protocol version mismatch
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<serde_json::Value>>(event)
            {
                let dev_ver = evt
                    .payload
                    .get("device_version")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(0);
                let exp_ver = evt
                    .payload
                    .get("expected_version")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(0);
                let msg = format!("Protocol mismatch: device v{}, app v{}. Some features may not work. Consider updating firmware.", dev_ver, exp_ver);
                // Dispatch toast event
                if let Some(window) = web_sys::window() {
                    let detail = js_sys::Object::new();
                    let _ = js_sys::Reflect::set(&detail, &"msg".into(), &msg.into());
                    let _ = js_sys::Reflect::set(&detail, &"kind".into(), &"err".into());
                    let init = web_sys::CustomEventInit::new();
                    init.set_detail(&detail);
                    if let Ok(evt) =
                        web_sys::CustomEvent::new_with_event_init_dict("bb-toast", &init)
                    {
                        let _ = window.dispatch_event(&evt);
                    }
                }
            }
        });
        listen("version-mismatch", &closure).await;
        // INTENTIONAL: app-lifetime listener — do not cleanup
        closure.forget();
    });

    // Listen for pairing-required (HTTP connect attempt without cached token)
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<serde_json::Value>>(event)
            {
                let mac = evt
                    .payload
                    .get("mac")
                    .and_then(|v| v.as_str())
                    .unwrap_or("unknown");
                let msg = format!("Secure pairing required for device {}. Please connect via USB once to authorize this computer.", mac);
                if let Some(window) = web_sys::window() {
                    let detail = js_sys::Object::new();
                    let _ = js_sys::Reflect::set(&detail, &"msg".into(), &msg.into());
                    let _ = js_sys::Reflect::set(&detail, &"kind".into(), &"err".into());
                    let init = web_sys::CustomEventInit::new();
                    init.set_detail(&detail);
                    if let Ok(evt) =
                        web_sys::CustomEvent::new_with_event_init_dict("bb-toast", &init)
                    {
                        let _ = window.dispatch_event(&evt);
                    }
                }
            }
        });
        listen("pairing-required", &closure).await;
        // INTENTIONAL: app-lifetime listener — do not cleanup
        closure.forget();
    });

    // Listen for PCA9535 fault events (e-fuse trips, power-good changes)
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<Vec<u8>>>(event) {
                let payload = evt.payload;
                if payload.len() >= 6 {
                    let fault_type = payload[0];
                    let channel = payload[1];
                    let msg = match fault_type {
                        0 => format!("E-Fuse {} tripped — output disabled!", channel + 1),
                        1 => format!("E-Fuse {} fault cleared", channel + 1),
                        2 => {
                            let name = match channel {
                                0 => "Logic",
                                1 => "VADJ1",
                                _ => "VADJ2",
                            };
                            format!("{} power-good LOST!", name)
                        }
                        3 => {
                            let name = match channel {
                                0 => "Logic",
                                1 => "VADJ1",
                                _ => "VADJ2",
                            };
                            format!("{} power-good restored", name)
                        }
                        _ => format!("PCA fault type={} ch={}", fault_type, channel),
                    };
                    let kind = if fault_type == 0 || fault_type == 2 {
                        "err"
                    } else {
                        "ok"
                    };
                    show_toast(&msg, kind);
                }
            }
        });
        listen("pca-fault", &closure).await;
        // INTENTIONAL: app-lifetime listener — do not cleanup
        closure.forget();
    });

    // Listen for BBP_EVT_IO_OWNER_REJECT forwarded by the Tauri backend.
    // Payload: { rejected_cmd: u8, slot: u8, current_owner_kind: u8 }
    // On receipt: immediately show the IO-blocked banner for the rejected slot
    // without waiting for the next tab-mount claim attempt.
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<serde_json::Value>>(event)
            {
                let slot = evt
                    .payload
                    .get("slot")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as u8)
                    .unwrap_or(0xFF);
                let kind = evt
                    .payload
                    .get("current_owner_kind")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as u8)
                    .unwrap_or(0);
                set_io_blocked_kind.set(kind);
                // Surface the slot immediately in the banner.
                set_io_blocked.update(|blocked| {
                    if slot != 0xFF {
                        match blocked {
                            Some(ref mut slots) => {
                                if !slots.contains(&slot) {
                                    slots.push(slot);
                                }
                            }
                            None => *blocked = Some(vec![slot]),
                        }
                    }
                });
            }
        });
        listen("io-owner-reject", &closure).await;
        // INTENTIONAL: app-lifetime listener — do not cleanup
        closure.forget();
    });

    // Auto-scan
    spawn_local(async move {
        slp(2000).await;
        let result = try_invoke("discover_devices", JsValue::NULL).await;
        if let Some(devs) =
            result.and_then(|r| serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(r).ok())
        {
            set_devices.set(devs);
        }
        set_scan_completed.set(true);
        if let Some(window) = web_sys::window() {
            let update_fn = js_sys::Reflect::get(&window, &"updateScanStatus".into()).unwrap();
            if update_fn.is_function() {
                let _ = update_fn
                    .unchecked_into::<js_sys::Function>()
                    .call1(&window, &true.into());
            }
        }
    });

    view! {
        <div class="app">
            // Header
            <header class="header">
                <div class="header-left">
                    <span class="logo-text">"BugBuster"</span>
                    <span class="subtitle">"AD74416H Controller"</span>
                </div>
                <div class="header-right">
                    {move || {
                        let m = conn_mode.get();
                        if m == "Disconnected" {
                            view! {
                                <div class="status-bar">
                                    <span class="status-dot disconnected"></span>
                                    <span class="status-text">"Disconnected"</span>
                                </div>
                            }.into_any()
                        } else {
                            let badge = if m == "Usb" { "USB" } else { "HTTP" };
                            view! {
                                <div class="status-bar">
                                    <span class="status-dot connected"></span>
                                    <span class="status-badge">{badge}</span>
                                    <span class="status-text">{move || conn_addr.get()}</span>
                                    <span class="status-separator">"|"</span>
                                    <span class={move || if device_state.get().spi_ok { "spi-ok" } else { "spi-err" }}>
                                        {move || if device_state.get().spi_ok { "SPI OK" } else { "SPI ERR" }}
                                    </span>
                                    <span class="status-separator">"|"</span>
                                    <span class="temp-value">
                                        {move || format!("{:.1} °C", device_state.get().die_temperature)}
                                    </span>
                                    <span class="status-separator">"|"</span>
                                    {move || {
                                        if has_update() {
                                            view! {
                                                <button class="btn btn-xs update-glow-btn"
                                                    on:click=move |_| set_active_tab.set("diag".to_string())
                                                >
                                                    <span class="update-glow-dot"></span>
                                                    "Update Available"
                                                </button>
                                                <span class="status-separator">"|"</span>
                                            }.into_any()
                                        } else {
                                            view! { <></> }.into_any()
                                        }
                                    }}
                                    <button class="btn btn-ghost btn-xs" on:click=move |_| {
                                        spawn_local(async move {
                                            let result = try_invoke("pick_config_save_file", JsValue::NULL).await;
                                            if let Some(path) = result.and_then(|r| serde_wasm_bindgen::from_value::<Option<String>>(r).ok().flatten()) {
                                                if !path.is_empty() {
                                                    #[derive(serde::Serialize)]
                                                    struct Args { path: String }
                                                    let args = serde_wasm_bindgen::to_value(&Args { path }).unwrap();
                                                    let _ = try_invoke("export_config", args).await;
                                                }
                                            }
                                        });
                                    }>"Export"</button>
                                    <button class="btn btn-ghost btn-xs" on:click=move |_| {
                                        spawn_local(async move {
                                            let result = try_invoke("pick_config_open_file", JsValue::NULL).await;
                                            if let Some(path) = result.and_then(|r| serde_wasm_bindgen::from_value::<Option<String>>(r).ok().flatten()) {
                                                if !path.is_empty() {
                                                    #[derive(serde::Serialize)]
                                                    struct Args { path: String }
                                                    let args = serde_wasm_bindgen::to_value(&Args { path }).unwrap();
                                                    let _ = try_invoke("import_config", args).await;
                                                }
                                            }
                                        });
                                    }>"Import"</button>
                                    <span class="status-separator">"|"</span>
                                    <button class="btn btn-danger btn-xs" on:click=disconnect>"Disconnect"</button>
                                </div>
                            }.into_any()
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
                    on_mock=Callback::new(move |_| {
                        set_conn_mode.set("Mock".to_string());
                        set_conn_addr.set("Demo (synthetic DAQ)".to_string());
                        set_hat_kind.set(HatKind::Daq);
                        set_active_tab.set("daq".to_string());
                        spawn_local(async move {
                            daq_connect(true).await;
                            daq_stream_start(3, 1).await;
                        });
                    })
                />
            </Show>

            // Main content (when connected)
            <Show when=move || conn_mode.get() != "Disconnected">
                // Category bar
                <nav class="category-bar">
                    {move || {
                        let kind = hat_kind.get();
                        CATEGORIES.iter()
                            .filter(|(_, _, tabs)| tabs.iter().any(|(t, _)| tab_visible(t, kind)))
                            .map(|(cat_id, label, tabs)| {
                                let cat_id_str = cat_id.to_string();
                                let first_tab_id = tabs.iter()
                                    .find(|(t, _)| tab_visible(t, kind))
                                    .map(|(t, _)| t.to_string())
                                    .unwrap_or_default();
                                view! {
                                    <button class="category-item"
                                        class:active=move || active_category() == cat_id_str
                                        on:click=move |_| set_active_tab.set(first_tab_id.clone())
                                    >{*label}</button>
                                }
                            }).collect::<Vec<_>>()
                    }}
                </nav>

                // Tab bar
                <nav class="tab-bar">
                    {move || {
                        let cat = active_category();
                        let kind = hat_kind.get();
                        let tabs = CATEGORIES.iter()
                            .find(|(cat_id, _, _)| **cat_id == cat)
                            .map(|(_, _, t)| *t)
                            .unwrap_or(&[]);

                        tabs.iter()
                            .filter(|(id, _)| tab_visible(id, kind))
                            .map(|(id, label)| {
                                let id_str = id.to_string();
                                let id_click = id_str.clone();
                                view! {
                                    <button class="tab-item"
                                        class:active=move || active_tab.get() == id_str
                                        on:click=move |_| set_active_tab.set(id_click.clone())
                                    >{*label}</button>
                                }
                            }).collect::<Vec<_>>()
                    }}
                </nav>

                // IO blocked banner — shown when the active tab's slots are held by another interface.
                {move || {
                    if let Some(slots) = io_blocked.get() {
                        let on_claimed = Callback::new(move |_| {
                            set_io_blocked_kind.set(0);
                            set_io_blocked.set(None);
                        });
                        let kind = io_blocked_kind.get();
                        view! {
                            <IoBlockedBanner slots=slots owner_kind=kind on_claimed=on_claimed />
                        }.into_any()
                    } else {
                        view! { <></> }.into_any()
                    }
                }}

                // Tab content
                <div class="tab-container">
                    {move || match active_tab.get().as_str() {
                        "overview" => view! { <OverviewTab state=device_state /> }.into_any(),
                        "board" => view! { <BoardTab state=device_state /> }.into_any(),
                        "adc" => view! { <AdcTab state=device_state /> }.into_any(),
                        "diag" => view! { <DiagTab state=device_state /> }.into_any(),
                        "vdac" => view! { <VdacTab state=device_state /> }.into_any(),
                        "idac" => view! { <IdacTab state=device_state /> }.into_any(),
                        "iin" => view! { <IinTab state=device_state /> }.into_any(),
                        "hv_io" => view! { <HvIoTab state=device_state /> }.into_any(),
                        "faults" => view! { <FaultsTab state=device_state /> }.into_any(),
                        "gpio" => view! { <GpioTab state=device_state /> }.into_any(),
                        "din" => view! { <DinTab state=device_state /> }.into_any(),
                        "dout" => view! { <DoutTab state=device_state /> }.into_any(),
                        "uart" => view! { <UartTab uart_config=uart_config /> }.into_any(),
                        "scope" => view! { <ScopeTab state=device_state /> }.into_any(),
                        "wavegen" => view! { <WavegenTab state=device_state /> }.into_any(),
                        "sigpath" => view! { <SignalPathTab state=device_state /> }.into_any(),
                        "voltages" => view! { <VoltagesTab state=device_state /> }.into_any(),
                        "usbpd" => view! { <UsbPdTab state=device_state /> }.into_any(),
                        "ioexp" => view! { <IoExpTab state=device_state /> }.into_any(),
                        "hat" => view! { <HatTab state=device_state /> }.into_any(),
                        "la" => view! { <LaTab state=device_state /> }.into_any(),
                        "daq" => view! { <DaqTab state=device_state /> }.into_any(),
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

async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        if let Some(w) = web_sys::window() {
            w.set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32)
                .ok();
        } else {
            web_sys::console::warn_1(&"slp: window unavailable, timeout will not fire".into());
        }
    });
    wasm_bindgen_futures::JsFuture::from(p).await.ok();
}

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
