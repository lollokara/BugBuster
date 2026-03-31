use leptos::prelude::*;
use serde::Serialize;
use wasm_bindgen::JsValue;
use crate::tauri_bridge::*;

// ALERT_STATUS register (0x3F)
const ALERT_BITS: &[(usize, &str, &str)] = &[
    (0, "RESET", "amber"), (2, "SUPPLY_ERR", "rose"), (3, "SPI_ERR", "rose"),
    (4, "TEMP_ALERT", "amber"), (5, "ADC_ERR", "rose"),
    (8, "CH_A", "blue"), (9, "CH_B", "blue"), (10, "CH_C", "blue"), (11, "CH_D", "blue"),
    (12, "HART_A", "amber"), (13, "HART_B", "amber"), (14, "HART_C", "amber"), (15, "HART_D", "amber"),
];

// SUPPLY_ALERT_STATUS register (0x57)
const SUPPLY_BITS: &[(usize, &str, &str)] = &[
    (0, "CAL_MEM", "amber"), (1, "AVSS", "rose"), (2, "DVCC", "rose"),
    (3, "AVCC", "rose"), (4, "DO_VDD", "rose"), (5, "AVDD_LO", "rose"), (6, "AVDD_HI", "rose"),
];

fn led_color(name: &str) -> &'static str {
    match name {
        "rose" => "var(--rose)",
        "amber" => "var(--amber)",
        "blue" => "var(--blue)",
        _ => "var(--green)",
    }
}

#[component]
pub fn DiagTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            <div class="tab-desc">"Internal diagnostic ADC channels. Select what to measure per slot: die temperature, supply voltages (DVCC, AVCC, AVDD), or sense voltages. Useful for verifying power rail health."</div>
            // Top row: Temp + Status LEDs
            <div class="diag-top">
                // Temperature
                <div class="card diag-temp-card">
                    <div class="temp-gauge-wrap">
                        <div class="temp-value-hero">
                            {move || format!("{:.1}", state.get().die_temperature)}
                            <span class="temp-unit">"°C"</span>
                        </div>
                        <div class="temp-thermometer">
                            <div class="temp-thermo-fill" style=move || {
                                let t = state.get().die_temperature;
                                let pct = ((t / 125.0) * 100.0).clamp(2.0, 100.0);
                                let color = if t > 100.0 { "#ef4444" } else if t > 70.0 { "#f59e0b" } else { "#10b981" };
                                format!("height: {:.1}%; background: {}", pct, color)
                            }></div>
                        </div>
                        <div class="temp-label" style=move || {
                            let t = state.get().die_temperature;
                            let color = if t > 100.0 { "var(--rose)" } else if t > 70.0 { "var(--amber)" } else { "var(--green)" };
                            format!("color: {}", color)
                        }>
                            {move || {
                                let t = state.get().die_temperature;
                                if t > 100.0 { "HOT" } else if t > 70.0 { "Warm" } else { "Normal" }
                            }}
                        </div>
                    </div>
                </div>

                // Alert Status — futuristic panel
                <div class="alert-panel">
                    <div class="alert-panel-header">
                        <div class="alert-panel-title">
                            <span class="alert-panel-icon">"△"</span>
                            " ALERT STATUS"
                        </div>
                        <span class="alert-panel-reg">{move || format!("REG 0x{:04X}", state.get().alert_status)}</span>
                    </div>
                    <div class="alert-panel-scanline"></div>
                    <div class="alert-grid">
                        {ALERT_BITS.iter().map(|(bit, name, color)| {
                            let bit = *bit;
                            let lc = led_color(color);
                            let accent = match *color { "rose" => "#ef4444", "amber" => "#f59e0b", "blue" => "#3b82f6", _ => "#10b981" };
                            view! {
                                <div class="alert-cell"
                                    style=move || if (state.get().alert_status >> bit) & 1 != 0 {
                                        format!("border-color: {}; background: {}0a; box-shadow: inset 0 0 20px {}08, 0 0 12px {}15", accent, accent, accent, accent)
                                    } else { String::new() }
                                >
                                    <div class="alert-cell-dot"
                                        style=move || if (state.get().alert_status >> bit) & 1 != 0 {
                                            format!("background: {}; box-shadow: 0 0 6px {}, 0 0 12px {}66", lc, lc, lc)
                                        } else { String::new() }
                                    ></div>
                                    <span class="alert-cell-label"
                                        style=move || if (state.get().alert_status >> bit) & 1 != 0 {
                                            format!("color: {}", accent)
                                        } else { String::new() }
                                    >{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                    <div class="alert-panel-footer">
                        <span class="alert-panel-count" style=move || {
                            let count = (0..16).filter(|b| (state.get().alert_status >> b) & 1 != 0).count();
                            if count > 0 { "color: #ef4444".to_string() } else { "color: #10b981".to_string() }
                        }>{move || {
                            let count = (0..16).filter(|b| (state.get().alert_status >> b) & 1 != 0).count();
                            if count == 0 { "ALL CLEAR".to_string() } else { format!("{} ACTIVE", count) }
                        }}</span>
                    </div>
                </div>

                // Supply Status — futuristic panel
                <div class="alert-panel supply-panel">
                    <div class="alert-panel-header">
                        <div class="alert-panel-title">
                            <span class="alert-panel-icon">"⚡"</span>
                            " SUPPLY STATUS"
                        </div>
                        <span class="alert-panel-reg">{move || format!("REG 0x{:04X}", state.get().supply_alert_status)}</span>
                    </div>
                    <div class="alert-panel-scanline supply-scanline"></div>
                    <div class="alert-grid">
                        {SUPPLY_BITS.iter().map(|(bit, name, color)| {
                            let bit = *bit;
                            let lc = led_color(color);
                            let accent = match *color { "rose" => "#ef4444", "amber" => "#f59e0b", "blue" => "#3b82f6", _ => "#10b981" };
                            view! {
                                <div class="alert-cell"
                                    style=move || if (state.get().supply_alert_status >> bit) & 1 != 0 {
                                        format!("border-color: {}; background: {}0a; box-shadow: inset 0 0 20px {}08, 0 0 12px {}15", accent, accent, accent, accent)
                                    } else { String::new() }
                                >
                                    <div class="alert-cell-dot"
                                        style=move || if (state.get().supply_alert_status >> bit) & 1 != 0 {
                                            format!("background: {}; box-shadow: 0 0 6px {}, 0 0 12px {}66", lc, lc, lc)
                                        } else { String::new() }
                                    ></div>
                                    <span class="alert-cell-label"
                                        style=move || if (state.get().supply_alert_status >> bit) & 1 != 0 {
                                            format!("color: {}", accent)
                                        } else { String::new() }
                                    >{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                    <div class="alert-panel-footer">
                        <span class="alert-panel-count" style=move || {
                            let count = (0..16).filter(|b| (state.get().supply_alert_status >> b) & 1 != 0).count();
                            if count > 0 { "color: #ef4444".to_string() } else { "color: #10b981".to_string() }
                        }>{move || {
                            let count = (0..16).filter(|b| (state.get().supply_alert_status >> b) & 1 != 0).count();
                            if count == 0 { "ALL CLEAR".to_string() } else { format!("{} ACTIVE", count) }
                        }}</span>
                    </div>
                </div>
            </div>

            // Diagnostic slots with source dropdowns
            <h3 class="section-title">"Diagnostic Channels"</h3>
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    ds.diag.into_iter().enumerate().map(|(i, d)| {
                        let slot = i as u8;
                        let source_name = DIAG_SOURCE_OPTIONS.iter()
                            .find(|(c, _)| *c == d.source)
                            .map(|(_, n)| *n).unwrap_or("?");
                        let unit = if d.source == 1 { "°C" } else { "V" };
                        let color = CH_COLORS[i];

                        view! {
                            <div class="card">
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("Slot {}", i)}
                                    </div>
                                    <select class="dropdown dropdown-sm"
                                        prop:value=d.source.to_string()
                                        on:change=move |e| {
                                            let src: u8 = event_target_value(&e).parse().unwrap_or(0);
                                            #[derive(Serialize)]
                                            struct Args { slot: u8, source: u8 }
                                            let args = serde_wasm_bindgen::to_value(&Args { slot, source: src }).unwrap();
                                            let src_name = DIAG_SOURCE_OPTIONS.iter()
                                                .find(|(c, _)| *c == src)
                                                .map(|(_, n)| *n).unwrap_or("?");
                                            let label = format!("Set Diag {} to {}", slot, src_name);
                                            invoke_with_feedback("set_diag_config", args, &label);
                                        }
                                    >
                                        {DIAG_SOURCE_OPTIONS.iter().map(|(code, name)| {
                                            view! { <option value=code.to_string()>{*name}</option> }
                                        }).collect::<Vec<_>>()}
                                    </select>
                                </div>
                                <div class="card-body">
                                    <div class="big-value">{format!("{:.3}", d.value)}<span class="unit">{unit}</span></div>
                                    <div class="card-details">
                                        <span>"Source: "{source_name}</span>
                                        <span>"Raw: 0x"{format!("{:04X}", d.raw_code)}</span>
                                    </div>
                                    <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                        <div class="bar-fill-dynamic" style={
                                            // Nominal ranges per diagnostic source
                                            let (lo, hi) = match d.source {
                                                0  => (0.0, 0.5),       // AGND
                                                1  => (0.0, 125.0),     // Temperature (°C)
                                                2  => (4.5, 5.5),       // DVCC (5V nom)
                                                3  => (4.5, 5.5),       // AVCC (5V nom)
                                                4  => (1.6, 2.0),       // LDO1V8 (1.8V nom)
                                                5  => (0.0, 33.0),      // AVDD_HI (up to ~33V)
                                                6  => (4.5, 5.5),       // ALDO5V (5V nom)
                                                7  => (-24.0, 0.0),     // AVSS (negative)
                                                8  => (2.3, 2.7),       // REFOUT (2.5V nom)
                                                9  => (0.0, 30.0),      // DO_VDD
                                                10 => (0.0, 5.0),       // AGND_SENSE
                                                11 => (0.0, 5.0),       // AVDD_LO
                                                _  => (0.0, 5.0),
                                            };
                                            let pct = if hi > lo {
                                                ((d.value - lo) / (hi - lo) * 100.0).clamp(0.0, 100.0)
                                            } else { 0.0 };
                                            format!("width: {:.1}%", pct)
                                        }></div>
                                    </div>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>

            // Firmware section
            <h3 class="section-title">"Firmware"</h3>
            <FirmwareSection />

            // WiFi section
            <h3 class="section-title">"WiFi"</h3>
            <WifiSection />
        </div>
    }
}

#[component]
fn FirmwareSection() -> impl IntoView {
    let fw = RwSignal::new(FirmwareInfo::default());
    let ota_status = RwSignal::new(String::new());
    let uploading = RwSignal::new(false);

    // Fetch firmware info
    leptos::task::spawn_local(async move {
        if let Some(info) = fetch_firmware_info().await {
            fw.set(info);
        }
    });

    view! {
        <div class="channel-grid" style="grid-template-columns: 1fr 1fr">
            // Firmware Info card
            <div class="alert-panel">
                <div class="alert-panel-header">
                    <div class="alert-panel-title">"FIRMWARE INFO"</div>
                    <span class="alert-panel-reg">{move || format!("PROTO v{}", fw.get().proto_version)}</span>
                </div>
                <div class="alert-panel-scanline"></div>
                <div style="padding: 12px 16px; display: grid; grid-template-columns: auto 1fr; gap: 6px 16px; font-size: 11px; font-family: 'JetBrains Mono', monospace">
                    <span style="color: var(--text-muted)">"Version:"</span>
                    <span style="color: var(--green); font-weight: 700">{move || {
                        let v = fw.get().fw_version.clone();
                        if v.is_empty() || v == "0.0.0" { "Fetching...".to_string() } else { format!("v{}", v) }
                    }}</span>
                    <span style="color: var(--text-muted)">"Built:"</span>
                    <span style="color: var(--text-dim)">{move || {
                        let d = fw.get().build_date.clone();
                        if d.is_empty() { "—".to_string() } else { d }
                    }}</span>
                    <span style="color: var(--text-muted)">"ESP-IDF:"</span>
                    <span style="color: var(--text-dim)">{move || {
                        let v = fw.get().idf_version.clone();
                        if v.is_empty() { "—".to_string() } else { v }
                    }}</span>
                    <span style="color: var(--text-muted)">"Partition:"</span>
                    <span style="color: var(--text-dim)">{move || {
                        let p = fw.get().partition.clone();
                        let n = fw.get().next_partition.clone();
                        if p.is_empty() { "—".to_string() } else { format!("{} (next: {})", p, n) }
                    }}</span>
                </div>
            </div>

            // OTA Update card
            <div class="alert-panel">
                <div class="alert-panel-header">
                    <div class="alert-panel-title">"OTA UPDATE"</div>
                </div>
                <div class="alert-panel-scanline supply-scanline"></div>
                <div style="padding: 12px 16px">
                    <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 10px; line-height: 1.6; font-family: 'JetBrains Mono', monospace">
                        "Upload a firmware.bin to flash the device over WiFi. The device reboots automatically after a successful update. NVS data is preserved."
                    </div>
                    <div style="display: flex; gap: 8px; align-items: center">
                        <button class="btn btn-sm btn-primary"
                            disabled=move || uploading.get()
                            on:click=move |_| {
                                uploading.set(true);
                                ota_status.set("Selecting file...".to_string());
                                leptos::task::spawn_local(async move {
                                    // Use Tauri file dialog
                                    #[derive(serde::Deserialize)]
                                    struct DialogResult { path: Option<String> }

                                    let args = serde_wasm_bindgen::to_value(
                                        &serde_json::json!({
                                            "title": "Select Firmware Binary",
                                            "filters": [{"name": "Firmware", "extensions": ["bin"]}]
                                        })
                                    ).unwrap();
                                    let result = invoke("plugin:dialog|open", args).await;
                                    let path: Option<String> = serde_wasm_bindgen::from_value(result).ok().flatten();

                                    if let Some(p) = path {
                                        ota_status.set(format!("Uploading {}...", p.split('/').last().unwrap_or(&p)));
                                        match upload_firmware(&p).await {
                                            Ok(msg) => ota_status.set(msg),
                                            Err(e) => ota_status.set(format!("Error: {}", e)),
                                        }
                                    } else {
                                        ota_status.set(String::new());
                                    }
                                    uploading.set(false);
                                });
                            }
                        >{move || if uploading.get() { "Uploading..." } else { "Select & Upload .bin" }}</button>
                    </div>
                    <div style="margin-top: 8px; font-size: 10px; font-family: 'JetBrains Mono', monospace; min-height: 16px"
                        style:color=move || if ota_status.get().starts_with("Error") { "var(--rose)" } else { "var(--green)" }
                    >
                        {move || ota_status.get()}
                    </div>
                </div>
            </div>
        </div>
    }
}

#[component]
fn WifiSection() -> impl IntoView {
    let wifi = RwSignal::new(WifiState::default());
    let connect_ssid = RwSignal::new(String::new());
    let connect_pass = RwSignal::new(String::new());
    let connect_status = RwSignal::new(String::new());
    let scan_results: RwSignal<Vec<WifiNetwork>> = RwSignal::new(Vec::new());
    let scanning = RwSignal::new(false);

    // Poll WiFi status every 2 seconds
    let poll = move || {
        leptos::task::spawn_local(async move {
            if let Some(ws) = fetch_wifi_status().await {
                wifi.set(ws);
            }
        });
    };
    poll();
    let _interval = leptos::prelude::set_interval_with_handle(
        move || poll(),
        std::time::Duration::from_secs(2),
    );

    let do_scan = move |_| {
        scanning.set(true);
        connect_status.set("Scanning...".to_string());
        leptos::task::spawn_local(async move {
            let results = fetch_wifi_scan().await;
            let count = results.len();
            if count > 0 && connect_ssid.get().is_empty() {
                connect_ssid.set(results[0].ssid.clone());
            }
            scan_results.set(results);
            scanning.set(false);
            connect_status.set(format!("Found {} networks", count));
        });
    };

    view! {
        <div class="channel-grid" style="grid-template-columns: 1fr 1fr">
            // AP Mode card
            <div class="card">
                <div class="card-header"><span>"Access Point"</span></div>
                <div class="card-body">
                    <div class="card-details" style="gap: 0.5rem">
                        <div>"SSID: "<strong>{move || wifi.get().ap_ssid.clone()}</strong></div>
                        <div>"IP: "<span class="mono">{move || wifi.get().ap_ip.clone()}</span></div>
                        <div>"MAC: "<span class="mono">{move || wifi.get().ap_mac.clone()}</span></div>
                    </div>
                </div>
            </div>

            // STA Mode card
            <div class="card">
                <div class="card-header">
                    <span>"Station"</span>
                    <div class="led"
                        class:led-on=move || wifi.get().connected
                        style=move || if wifi.get().connected {
                            "background: var(--green); box-shadow: 0 0 8px var(--green)".to_string()
                        } else {
                            String::new()
                        }
                    ></div>
                </div>
                <div class="card-body">
                    <div class="card-details" style="gap: 0.5rem">
                        <div>"Status: "
                            <strong style=move || if wifi.get().connected {
                                "color: var(--green)"
                            } else { "color: var(--rose)" }>
                                {move || if wifi.get().connected { "Connected" } else { "Disconnected" }}
                            </strong>
                        </div>
                        <div>"SSID: "<strong>{move || wifi.get().sta_ssid.clone()}</strong></div>
                        <div>"IP: "<span class="mono">{move || wifi.get().sta_ip.clone()}</span></div>
                        <div style="display: flex; align-items: center; gap: 0.5rem">
                            "RSSI: "<span class="mono">{move || format!("{} dBm", wifi.get().rssi)}</span>
                            <div class="bar-gauge" style="flex: 1; --bar-color: var(--blue)">
                                <div class="bar-fill-dynamic" style=move || {
                                    let rssi = wifi.get().rssi;
                                    let pct = (((rssi + 100) as f32 / 60.0) * 100.0).clamp(0.0, 100.0);
                                    format!("width: {}%", pct)
                                }></div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        // Connect form
        <div class="card" style="margin-top: 0.5rem">
            <div class="card-header"><span>"Connect to Network"</span></div>
            <div class="card-body">
                <div style="display: flex; gap: 0.5rem; align-items: center; flex-wrap: wrap">
                    <button class="btn btn-sm btn-primary"
                        disabled=move || scanning.get()
                        on:click=do_scan
                        style="white-space: nowrap"
                    >{move || if scanning.get() { "Scanning..." } else { "Scan" }}</button>
                    <select class="input"
                        style="flex: 1; min-width: 160px; max-width: none"
                        prop:value=move || connect_ssid.get()
                        on:change=move |e| connect_ssid.set(event_target_value(&e))
                    >
                        <option value="" disabled=true selected=move || connect_ssid.get().is_empty()>"Select network..."</option>
                        {move || {
                            scan_results.get().into_iter().map(|n| {
                                let label = format!("{} ({} dBm)", n.ssid, n.rssi);
                                let ssid = n.ssid.clone();
                                view! { <option value=ssid>{label}</option> }
                            }).collect::<Vec<_>>()
                        }}
                    </select>
                    <input type="password" class="input" placeholder="Password"
                        style="flex: 1; min-width: 120px"
                        prop:value=move || connect_pass.get()
                        on:input=move |e| connect_pass.set(event_target_value(&e))
                    />
                    <button class="btn btn-sm" style="background: rgba(16,185,129,0.7); border-color: rgba(16,185,129,0.3)"
                        on:click=move |_| {
                            let ssid = connect_ssid.get();
                            let pass = connect_pass.get();
                            if ssid.is_empty() {
                                connect_status.set("Select a network first".to_string());
                                return;
                            }
                            connect_status.set("Connecting...".to_string());
                            leptos::task::spawn_local(async move {
                                #[derive(serde::Serialize)]
                                struct Args { ssid: String, password: String }
                                let args = serde_wasm_bindgen::to_value(
                                    &Args { ssid: ssid.clone(), password: pass }
                                ).unwrap();
                                let result = invoke("wifi_connect", args).await;
                                let ok: bool = serde_wasm_bindgen::from_value(result).unwrap_or(false);
                                if ok {
                                    connect_status.set(format!("Connected to {}", ssid));
                                } else {
                                    connect_status.set(format!("Failed to connect to {}", ssid));
                                }
                            });
                        }
                    >"Connect"</button>
                </div>
                <div class="text-xs" style="margin-top: 0.5rem; color: var(--text-dim)">
                    {move || connect_status.get()}
                </div>
            </div>
        </div>
    }
}
