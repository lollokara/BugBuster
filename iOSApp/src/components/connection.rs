use crate::tauri_bridge::{log, show_toast, try_invoke, invoke, DiscoveredDevice, sleep_ms};
use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

// ---------------------------------------------------------------------------
// QR Code scan helper
// Calls the tauri-plugin-barcode-scanner `scan` command.
// Expected QR payload format:  bugbuster://pair?ip=<ip>&token=<token>
// or a bare token string for backward compat.
// ---------------------------------------------------------------------------
async fn scan_qr() -> Option<String> {
    // Request camera permission before scan on mobile
    match invoke("plugin:barcode-scanner|request_permissions", JsValue::NULL).await {
        Ok(val) => {
            log(&format!("Permission request outcome: {:?}", val));
        }
        Err(e) => {
            let err_json = js_sys::JSON::stringify(&e)
                .map(|s| s.as_string().unwrap_or_default())
                .unwrap_or_else(|_| format!("{:?}", e));
            show_toast(&format!("Permission request failed: {}", err_json), "err");
        }
    }

    #[derive(serde::Serialize)]
    struct ScanArgs {
        windowed: bool,
        formats: Vec<String>,
    }
    let args = serde_wasm_bindgen::to_value(&ScanArgs {
        windowed: false,
        formats: vec!["QR_CODE".to_string()],
    })
    .ok()?;

    match invoke("plugin:barcode-scanner|scan", args).await {
        Ok(result) => {
            // result is { content: String, format: String }
            let content: String = js_sys::Reflect::get(&result, &"content".into())
                .ok()
                .and_then(|v| v.as_string())?;
            Some(content)
        }
        Err(e) => {
            let err_json = js_sys::JSON::stringify(&e)
                .map(|s| s.as_string().unwrap_or_default())
                .unwrap_or_else(|_| format!("{:?}", e));
            show_toast(&format!("Scan failed: {}", err_json), "err");
            None
        }
    }
}

/// Parse a BugBuster QR payload and return (ip, token).
/// Supports:
///   bugbuster://pair?ip=192.168.1.100&token=ABC123
///   http://192.168.1.100/  (legacy, no token)
///   bare token string
fn parse_qr_payload(raw: &str) -> (Option<String>, Option<String>) {
    if let Some(stripped) = raw.strip_prefix("bugbuster://pair?") {
        let mut ip: Option<String> = None;
        let mut token: Option<String> = None;
        for part in stripped.split('&') {
            if let Some(v) = part.strip_prefix("ip=") {
                ip = Some(v.to_string());
            }
            if let Some(v) = part.strip_prefix("token=") {
                token = Some(v.to_string());
            }
        }
        (ip, token)
    } else if raw.starts_with("http://") || raw.starts_with("https://") {
        // Legacy: bare URL — derive IP but no token
        let ip = raw
            .trim_start_matches("http://")
            .trim_start_matches("https://")
            .split('/')
            .next()
            .map(str::to_string);
        (ip, None)
    } else {
        // Assume bare token
        (None, Some(raw.trim().to_string()))
    }
}

fn set_body_class(active: bool) {
    if let Some(window) = web_sys::window() {
        if let Some(doc) = window.document() {
            if let Some(body) = doc.body() {
                if active {
                    let _ = body.class_list().add_1("qr-active");
                } else {
                    let _ = body.class_list().remove_1("qr-active");
                }
            }
            if let Some(html) = doc.document_element() {
                if active {
                    let _ = html.class_list().add_1("qr-active");
                } else {
                    let _ = html.class_list().remove_1("qr-active");
                }
            }
        }
    }
}

#[component]
pub fn ConnectionPanel(
    devices: Signal<Vec<DiscoveredDevice>>,
    scanning: Signal<bool>,
    scan_completed: Signal<bool>,
    on_scan: Callback<ev::MouseEvent>,
) -> impl IntoView {
    let (selected_device_id, set_selected_device_id) = signal(None::<String>);
    let (token_input, set_token_input) = signal(String::new());

    // Auto-populate token when device is selected
    Effect::new(move |_| {
        if let Some(device_id) = selected_device_id.get() {
            let ip = device_id
                .trim_start_matches("http://")
                .trim_start_matches("https://")
                .to_string();
            
            spawn_local(async move {
                #[derive(serde::Serialize)]
                struct Args { mac: String }
                let args = serde_wasm_bindgen::to_value(&Args { mac: ip }).unwrap();
                if let Some(res) = try_invoke("get_device_token", args).await {
                    if let Some(token) = res.as_string() {
                        set_token_input.set(token);
                        return;
                    }
                }
                set_token_input.set(String::new());
            });
        } else {
            set_token_input.set(String::new());
        }
    });

    // QR scanner state
    let (qr_scanning, set_qr_scanning) = signal(false);

    let connect = move |device: DiscoveredDevice| {
        use serde::Serialize;
        #[derive(Serialize)]
        struct Args {
            #[serde(rename = "deviceId")]
            device_id: String,
        }

        let device_id = device.id.clone();
        // device.id is now the full URL (http://x.x.x.x)
        // save token under the IP address — get_token() has a MAC-first, IP-fallback lookup
        let ip = device.id
            .trim_start_matches("http://")
            .trim_start_matches("https://")
            .to_string();

        spawn_local(async move {
            let token = token_input.get_untracked();
            if !token.is_empty() {
                #[derive(Serialize)]
                struct TokenArgs { mac: String, token: String }
                let _ = try_invoke("save_device_token", serde_wasm_bindgen::to_value(&TokenArgs { mac: ip.clone(), token }).unwrap()).await;
            }

            log(&format!("Connecting to: {}", device_id));
            let args = serde_wasm_bindgen::to_value(&Args { device_id }).unwrap();
            if try_invoke("connect_device", args).await.is_none() {
                show_toast("Connection failed — ensure token is correct", "err");
            }
        });
    };

    // Trigger QR scan
    let do_qr_scan = move |_: ev::MouseEvent| {
        set_qr_scanning.set(true);
        set_body_class(true);
        spawn_local(async move {
            match scan_qr().await {
                Some(raw) => {
                    let (qr_ip, qr_token) = parse_qr_payload(&raw);

                    // Pre-fill token if found
                    if let Some(t) = qr_token {
                        set_token_input.set(t);
                    }

                    // Auto-select device if IP matches
                    if let Some(ip) = qr_ip {
                        let devs = devices.get_untracked();
                        // device.address is now just the IP (e.g. "192.168.3.82")
                        let matched = devs.iter().find(|d| d.address == ip || d.address.starts_with(&ip));
                        if let Some(d) = matched {
                            set_selected_device_id.set(Some(d.id.clone()));
                            show_toast("Device found — review token & connect", "ok");
                        } else {
                            show_toast("QR scanned — no matching device found on network yet", "ok");
                        }
                    } else {
                        show_toast("Token captured from QR — select a device to connect", "ok");
                    }
                }
                None => {
                    // User cancelled or permission denied — silent
                }
            }
            set_qr_scanning.set(false);
            set_body_class(false);
        });
    };

    // Cancel ongoing QR scan
    let cancel_qr = move |_: ev::MouseEvent| {
        spawn_local(async move {
            let _ = try_invoke("plugin:barcode-scanner|cancel", JsValue::NULL).await;
            set_qr_scanning.set(false);
            set_body_class(false);
        });
    };

    Effect::new(move |_| {
        spawn_local(async move {
            let mut retries = 0;
            loop {
                if let Some(window) = web_sys::window() {
                    let init_fn = js_sys::Reflect::get(&window, &"initSplash".into()).unwrap();
                    if init_fn.is_function() {
                        let _ = init_fn.unchecked_into::<js_sys::Function>().call0(&window);

                        let update_fn =
                            js_sys::Reflect::get(&window, &"updateScanStatus".into()).unwrap();
                        if update_fn.is_function() {
                            let _ = update_fn
                                .unchecked_into::<js_sys::Function>()
                                .call1(&window, &scan_completed.get().into());
                        }
                        break;
                    }
                }

                retries += 1;
                if retries > 50 {
                    web_sys::console::error_1(
                        &"ConnectionPanel: initSplash not found after 5 seconds".into(),
                    );
                    break;
                }
                sleep_ms(100).await;
            }
        });

        on_cleanup(move || {
            if let Some(window) = web_sys::window() {
                let destroy_fn =
                    js_sys::Reflect::get(&window, &"destroySplash".into()).unwrap();
                if destroy_fn.is_function() {
                    let _ = destroy_fn
                        .unchecked_into::<js_sys::Function>()
                        .call0(&window);
                }
            }
        });
    });

    Effect::new(move |_| {
        let shrink = !devices.get().is_empty() || scan_completed.get();
        if let Some(window) = web_sys::window() {
            if let Ok(update_fn) = js_sys::Reflect::get(&window, &"updateScanStatus".into()) {
                if update_fn.is_function() {
                    let _ = update_fn
                        .unchecked_into::<js_sys::Function>()
                        .call1(&window, &shrink.into());
                }
            }
        }
    });

    view! {
        // ---- QR Scanner full-screen overlay ----
        {move || if qr_scanning.get() {
            view! {
                <div class="qr-scanner-overlay">
                    <div class="qr-scanner-hud">
                        <div class="qr-viewfinder">
                            <div class="qr-corner qr-tl" />
                            <div class="qr-corner qr-tr" />
                            <div class="qr-corner qr-bl" />
                            <div class="qr-corner qr-br" />
                            <div class="qr-scan-line" />
                        </div>
                        <p class="qr-hint">"Point at the QR code on your BugBuster PCB"</p>
                        <button class="qr-cancel-btn" on:click=cancel_qr>
                            <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.5">
                                <line x1="18" y1="6" x2="6" y2="18"/>
                                <line x1="6" y1="6" x2="18" y2="18"/>
                            </svg>
                            "Cancel"
                        </button>
                    </div>
                </div>
            }.into_any()
        } else {
            view! { <></> }.into_any()
        }}

        // ---- Main connection layout ----
        <div class="connection-layout mobile-optimized" class:has-devices=move || !devices.get().is_empty()>
            <div class="model-container">
                <canvas id="board-canvas" />
            </div>

            <div class="connection-ui-side">
                <div class="connection-header-group">
                    <h1 class="logo-title">"BugBuster"</h1>
                    <p class="subtitle-desc">"WiFi Debug Suite"</p>
                </div>

                <div class="card connection-card">
                    {move || if devices.get().is_empty() {
                        view! {
                            <div class="scanning-loader-wrap">
                                <div class="scanning-glow-ring"></div>
                                <p class="scanning-status">"Looking for devices on WiFi"</p>
                            </div>
                        }.into_any()
                    } else {
                        view! {
                            <div style="display: flex; gap: 10px; margin-bottom: 20px;">
                                <button
                                    class="btn btn-primary btn-scan"
                                    style="flex: 1"
                                    on:click=move |e| on_scan.run(e)
                                    disabled=move || scanning.get()
                                >
                                    {move || if scanning.get() { "Scanning..." } else { "Refresh" }}
                                </button>
                                <button
                                    class="btn btn-qr"
                                    id="qr-scan-btn"
                                    on:click=do_qr_scan
                                    disabled=move || qr_scanning.get()
                                    title="Scan QR code on PCB"
                                >
                                    // QR icon SVG
                                    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
                                        <rect x="3" y="3" width="7" height="7" rx="1"/>
                                        <rect x="14" y="3" width="7" height="7" rx="1"/>
                                        <rect x="3" y="14" width="7" height="7" rx="1"/>
                                        <rect x="5" y="5" width="3" height="3" fill="currentColor" stroke="none"/>
                                        <rect x="16" y="5" width="3" height="3" fill="currentColor" stroke="none"/>
                                        <rect x="5" y="16" width="3" height="3" fill="currentColor" stroke="none"/>
                                        <path d="M14 14h3v3h-3z" fill="currentColor" stroke="none"/>
                                        <path d="M17 14h4M17 17v4M14 20h3" stroke-linecap="round"/>
                                    </svg>
                                    "QR"
                                </button>
                            </div>

                            <div class="device-list-container">
                                <div class="device-list">
                                    <For
                                        each=move || devices.get()
                                        key=|dev| dev.id.clone()
                                        children=move |dev: DiscoveredDevice| {
                                            let id = dev.id.clone();

                                            let id_for_class = id.clone();
                                            let id_for_click = id.clone();
                                            let id_for_view = id.clone();
                                            let id_for_connect = id.clone();
                                            let dev_for_connect = dev.clone();

                                            view! {
                                                <div class="device-item-wrap"
                                                    class:selected=move || selected_device_id.get().as_deref() == Some(&id_for_class)
                                                >
                                                    <button class="device-item" on:click=move |_| {
                                                        if selected_device_id.get().as_deref() == Some(&id_for_click) {
                                                            set_selected_device_id.set(None);
                                                        } else {
                                                            set_selected_device_id.set(Some(id_for_click.clone()));
                                                        }
                                                    }>
                                                        <span class="device-icon">"📡"</span>
                                                        <div class="device-info">
                                                            <span class="device-name">{dev.name.clone()}</span>
                                                            <span class="device-addr">{dev.address.clone()}</span>
                                                        </div>
                                                    </button>

                                                    {move || {
                                                        if selected_device_id.get().as_deref() == Some(&id_for_view) {
                                                            let d = dev_for_connect.clone();
                                                            view! {
                                                                <div class="token-entry-area fade-in">
                                                                    <div class="token-entry-header">
                                                                        <p class="token-label">"Access Token"</p>
                                                                        <button
                                                                            class="token-qr-inline-btn"
                                                                            id={format!("qr-inline-{}", id_for_connect)}
                                                                            on:click=do_qr_scan
                                                                            title="Scan token from QR"
                                                                        >
                                                                            <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
                                                                                <rect x="3" y="3" width="7" height="7" rx="1"/>
                                                                                <rect x="14" y="3" width="7" height="7" rx="1"/>
                                                                                <rect x="3" y="14" width="7" height="7" rx="1"/>
                                                                                <rect x="5" y="5" width="3" height="3" fill="currentColor" stroke="none"/>
                                                                                <rect x="16" y="5" width="3" height="3" fill="currentColor" stroke="none"/>
                                                                                <rect x="5" y="16" width="3" height="3" fill="currentColor" stroke="none"/>
                                                                            </svg>
                                                                            "Scan QR"
                                                                        </button>
                                                                    </div>
                                                                    <input type="text"
                                                                        class="token-input"
                                                                        placeholder="e.g. ABC123 (or scan QR on PCB)"
                                                                        on:input=move |e| set_token_input.set(event_target_value(&e))
                                                                        prop:value=token_input
                                                                    />
                                                                    <button class="btn btn-success" style="width: 100%; margin-top: 10px; height: 44px;"
                                                                        on:click=move |_| connect(d.clone())>
                                                                        "Authorize & Connect"
                                                                    </button>
                                                                </div>
                                                             }.into_any()
                                                        } else {
                                                            view! { <></> }.into_any()
                                                        }
                                                    }}
                                                </div>
                                            }
                                        }
                                    />
                                </div>
                            </div>
                        }.into_any()
                    }}
                </div>
            </div>
        </div>
    }
}
