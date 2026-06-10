use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

#[derive(serde::Deserialize, Clone, Debug)]
struct AppUpdateInfo {
    available: bool,
    version: String,
    current_version: String,
    notes: String,
    is_nightly: bool,
}

#[derive(serde::Deserialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
#[allow(dead_code)]
struct DesktopReleaseEntry {
    tag: String,
    name: String,
    version: String,
    prerelease: bool,
    published_at: String,
    installer_url: String,
    installer_size: u64,
}

// ALERT_STATUS register (0x3F)
const ALERT_BITS: &[(usize, &str, &str)] = &[
    (0, "RESET", "amber"),
    (2, "SUPPLY_ERR", "rose"),
    (3, "SPI_ERR", "rose"),
    (4, "TEMP_ALERT", "amber"),
    (5, "ADC_ERR", "rose"),
    (8, "CH_A", "blue"),
    (9, "CH_B", "blue"),
    (10, "CH_C", "blue"),
    (11, "CH_D", "blue"),
    (12, "HART_A", "amber"),
    (13, "HART_B", "amber"),
    (14, "HART_C", "amber"),
    (15, "HART_D", "amber"),
];

// SUPPLY_ALERT_STATUS register (0x57)
const SUPPLY_BITS: &[(usize, &str, &str)] = &[
    (0, "CAL_MEM", "amber"),
    (1, "AVSS", "rose"),
    (2, "DVCC", "rose"),
    (3, "AVCC", "rose"),
    (4, "DO_VDD", "rose"),
    (5, "AVDD_LO", "rose"),
    (6, "AVDD_HI", "rose"),
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
    let update_checking = RwSignal::new(false);
    let update_info: RwSignal<Option<AppUpdateInfo>> = RwSignal::new(None);
    let update_error: RwSignal<Option<String>> = RwSignal::new(None);
    let update_installing = RwSignal::new(false);
    let show_update_popup = RwSignal::new(false);
    let releases_loading = RwSignal::new(false);
    let releases: RwSignal<Vec<DesktopReleaseEntry>> = RwSignal::new(Vec::new());
    let selected_release: RwSignal<Option<usize>> = RwSignal::new(None);
    let installing_url: RwSignal<Option<String>> = RwSignal::new(None);
    let worker_enabled = RwSignal::new(false);
    let supply_monitor_active = RwSignal::new(false);
    let worker_toggling = RwSignal::new(false);

    // Auto-check for updates on mount
    {
        leptos::task::spawn_local(async move {
            update_checking.set(true);
            match invoke("check_app_update", wasm_bindgen::JsValue::NULL).await {
                Ok(val) => {
                    match serde_wasm_bindgen::from_value::<AppUpdateInfo>(val) {
                        Ok(info) => {
                            if info.available {
                                show_update_popup.set(true);
                            }
                            update_info.set(Some(info));
                            update_error.set(None);
                        }
                        Err(e) => { update_error.set(Some(e.to_string())); }
                    }
                }
                Err(e) => { update_error.set(Some(e.as_string().unwrap_or_default())); }
            }
            update_checking.set(false);
        });
    }

    // Fetch selftest worker state on mount
    {
        leptos::task::spawn_local(async move {
            if let Some(st) = fetch_selftest_status().await {
                worker_enabled.set(st.worker_enabled);
                supply_monitor_active.set(st.supply_monitor_active);
            }
        });
    }

    let on_worker_toggle = move |_| {
        let new_val = !worker_enabled.get();
        worker_toggling.set(true);
        leptos::task::spawn_local(async move {
            if let Some(confirmed) = fetch_selftest_worker_set(new_val).await {
                worker_enabled.set(confirmed);
                if let Some(st) = fetch_selftest_status().await {
                    supply_monitor_active.set(st.supply_monitor_active);
                }
            }
            worker_toggling.set(false);
        });
    };

    let on_check = move |_| {
        update_checking.set(true);
        update_info.set(None);
        update_error.set(None);
        leptos::task::spawn_local(async move {
            match invoke("check_app_update", wasm_bindgen::JsValue::NULL).await {
                Ok(val) => {
                    match serde_wasm_bindgen::from_value::<AppUpdateInfo>(val) {
                        Ok(info) => { update_info.set(Some(info)); update_error.set(None); }
                        Err(e) => { update_error.set(Some(e.to_string())); }
                    }
                }
                Err(e) => { update_error.set(Some(e.as_string().unwrap_or_default())); }
            }
            update_checking.set(false);
        });
    };

    let on_install = move |_| {
        update_installing.set(true);
        leptos::task::spawn_local(async move {
            match invoke("apply_app_update", wasm_bindgen::JsValue::NULL).await {
                Ok(_) => {} // app will restart itself
                Err(e) => {
                    update_error.set(Some(e.as_string().unwrap_or_default()));
                    update_installing.set(false);
                }
            }
        });
    };

    let on_load_releases = move |_| {
        releases_loading.set(true);
        releases.set(Vec::new());
        selected_release.set(None);
        leptos::task::spawn_local(async move {
            match invoke("list_desktop_releases", wasm_bindgen::JsValue::NULL).await {
                Ok(val) => {
                    match serde_wasm_bindgen::from_value::<Vec<DesktopReleaseEntry>>(val) {
                        Ok(list) => { releases.set(list); }
                        Err(e) => { update_error.set(Some(e.to_string())); }
                    }
                }
                Err(e) => { update_error.set(Some(e.as_string().unwrap_or_default())); }
            }
            releases_loading.set(false);
        });
    };

    let on_install_selected = move |_| {
        if let Some(idx) = selected_release.get() {
            let rlist = releases.get();
            if let Some(entry) = rlist.get(idx) {
                let url = entry.installer_url.clone();
                installing_url.set(Some(url.clone()));
                leptos::task::spawn_local(async move {
                    #[derive(serde::Serialize)]
                    struct Args { url: String }
                    let args = serde_wasm_bindgen::to_value(&Args { url }).unwrap();
                    match invoke("install_desktop_version", args).await {
                        Ok(_) => {}
                        Err(e) => {
                            update_error.set(Some(e.as_string().unwrap_or_default()));
                            installing_url.set(None);
                        }
                    }
                });
            }
        }
    };

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
                            let is_ch_d = *name == "CH_D";
                            let lc = led_color(color);
                            let accent = match *color { "rose" => "#ef4444", "amber" => "#f59e0b", "blue" => "#3b82f6", _ => "#10b981" };
                            view! {
                                <div class="alert-cell"
                                    style=move || if (state.get().alert_status >> bit) & 1 != 0 {
                                        format!("border-color: {}; background: {}0a; box-shadow: inset 0 0 20px {}08, 0 0 12px {}15", accent, accent, accent, accent)
                                    } else if is_ch_d && supply_monitor_active.get() {
                                        "border-color: #f59e0b; background: #f59e0b0a; opacity: 0.75;".to_string()
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
                                    <span
                                        title="Reserved for supply monitoring"
                                        style=move || if is_ch_d && supply_monitor_active.get() {
                                            "display:block;font-size:0.5rem;color:#f59e0b;line-height:1.2;"
                                        } else { "display:none;" }
                                    >"⚡MON"</span>
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

            // Selftest Worker toggle
            <div class="card" style="margin-bottom:0.75rem;">
                <div class="card-header">
                    <span class="card-title">"Selftest Worker"</span>
                    <span style=move || if supply_monitor_active.get() {
                        "font-size:0.7rem;color:#f59e0b;padding:2px 6px;border:1px solid #f59e0b44;border-radius:4px;"
                    } else {
                        "font-size:0.7rem;color:#6b7280;padding:2px 6px;border:1px solid #ffffff11;border-radius:4px;"
                    }>
                        {move || if supply_monitor_active.get() { "CH-D RESERVED" } else { "Inactive" }}
                    </span>
                </div>
                <div class="card-body" style="display:flex;align-items:center;gap:1rem;">
                    <span style="font-size:0.8rem;color:#9ca3af;">
                        "Periodic supply-rail selftest. When active, CH-D is reserved for internal measurements."
                    </span>
                    <button
                        class=move || if worker_enabled.get() { "btn btn-sm btn-primary" } else { "btn btn-sm" }
                        disabled=move || worker_toggling.get()
                        on:click=on_worker_toggle
                    >
                        {move || if worker_toggling.get() { "…" } else if worker_enabled.get() { "Disable" } else { "Enable" }}
                    </button>
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
                                                8  => (0.0, 30.0),      // LVIN (input supply)
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

            // Startup popup — fixed overlay, shown when update is available
            {move || if show_update_popup.get() {
                let info = update_info.get();
                view! {
                    <div style="position: fixed; inset: 0; z-index: 9999; background: rgba(0,0,0,0.6); display: flex; align-items: center; justify-content: center; backdrop-filter: blur(4px)">
                        <div style="background: var(--surface-2, #1a1a2e); border: 1px solid rgba(59,130,246,0.4); border-radius: 12px; padding: 28px 32px; max-width: 420px; width: 90%; box-shadow: 0 0 40px rgba(59,130,246,0.15)">
                            <div style="font-size: 13px; font-weight: 700; color: var(--blue); font-family: 'JetBrains Mono', monospace; margin-bottom: 8px">
                                "UPDATE AVAILABLE"
                            </div>
                            <div style="font-size: 22px; font-weight: 700; color: var(--text-primary, #e2e8f0); margin-bottom: 6px">
                                {info.as_ref().map(|i| format!("v{}", i.version)).unwrap_or_default()}
                            </div>
                            <div style="font-size: 11px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 20px">
                                {info.as_ref().map(|i| {
                                    if i.is_nightly {
                                        format!("A newer nightly build is available (you have v{})", i.current_version)
                                    } else {
                                        format!("A new stable release is available (you have v{})", i.current_version)
                                    }
                                }).unwrap_or_default()}
                            </div>
                            <div style="display: flex; gap: 10px">
                                <button class="btn btn-sm btn-primary"
                                    disabled=move || update_installing.get()
                                    on:click=move |_| {
                                        show_update_popup.set(false);
                                        update_installing.set(true);
                                        leptos::task::spawn_local(async move {
                                            match invoke("apply_app_update", wasm_bindgen::JsValue::NULL).await {
                                                Ok(_) => {}
                                                Err(e) => {
                                                    update_error.set(Some(e.as_string().unwrap_or_default()));
                                                    update_installing.set(false);
                                                }
                                            }
                                        });
                                    }
                                >
                                    {move || if update_installing.get() { "Installing..." } else { "Install & Restart" }}
                                </button>
                                <button class="btn btn-sm btn-ghost"
                                    on:click=move |_| { show_update_popup.set(false); }
                                >"Later"</button>
                            </div>
                        </div>
                    </div>
                }.into_any()
            } else {
                view! { <></> }.into_any()
            }}

            // App Updates section
            <h3 class="section-title">"App Updates"</h3>
            <div class="alert-panel">
                <div class="alert-panel-header">
                    <div class="alert-panel-title">"APP UPDATE"</div>
                    <span class="alert-panel-reg">{move || {
                        update_info.get()
                            .map(|i| format!("v{}", i.current_version))
                            .unwrap_or_else(|| "—".to_string())
                    }}</span>
                </div>
                <div class="alert-panel-scanline supply-scanline"></div>
                <div style="padding: 12px 16px; display: flex; flex-direction: column; gap: 10px">
                    // Status row
                    {move || {
                        if update_checking.get() {
                            view! {
                                <div style="font-size: 11px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">"Checking for updates..."</div>
                            }.into_any()
                        } else if let Some(info) = update_info.get() {
                            if info.available {
                                let notes_preview = if info.notes.len() > 200 { format!("{}…", &info.notes[..200]) } else { info.notes.clone() };
                                view! {
                                    <div style="display: flex; flex-direction: column; gap: 8px; padding: 10px; background: rgba(59,130,246,0.07); border: 1px solid rgba(59,130,246,0.2); border-radius: 6px">
                                        <span style="color: var(--blue); font-size: 11px; font-weight: 700; font-family: 'JetBrains Mono', monospace">
                                            {format!("v{} available", info.version)}
                                        </span>
                                        {if !notes_preview.is_empty() {
                                            view! { <div style="font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; line-height: 1.5; white-space: pre-wrap">{notes_preview}</div> }.into_any()
                                        } else { view! { <></> }.into_any() }}
                                        <button class="btn btn-sm btn-primary" style="align-self: flex-start"
                                            disabled=move || update_installing.get() || update_checking.get()
                                            on:click=on_install
                                        >{move || if update_installing.get() { "Installing..." } else { "Install & Restart" }}</button>
                                    </div>
                                }.into_any()
                            } else {
                                view! {
                                    <div style="display: flex; align-items: center; gap: 8px; font-size: 11px; font-family: 'JetBrains Mono', monospace">
                                        <span style="color: var(--green); font-weight: 700">"✓"</span>
                                        <span style="color: var(--green)">"Up to date"</span>
                                    </div>
                                }.into_any()
                            }
                        } else { view! { <></> }.into_any() }
                    }}

                    // Error
                    {move || update_error.get().map(|err| view! {
                        <div style="padding: 8px 10px; background: rgba(239,68,68,0.1); border: 1px solid rgba(239,68,68,0.3); border-radius: 6px; color: var(--rose); font-size: 10px; font-family: 'JetBrains Mono', monospace">{err}</div>
                    })}

                    // Action buttons row
                    <div style="display: flex; gap: 8px; flex-wrap: wrap">
                        <button class="btn btn-sm btn-ghost"
                            disabled=move || update_checking.get() || update_installing.get()
                            on:click=on_check
                        >"Check for Updates"</button>
                        <button class="btn btn-sm btn-ghost"
                            disabled=move || releases_loading.get()
                            on:click=on_load_releases
                        >{move || if releases_loading.get() { "Loading..." } else { "Browse Releases" }}</button>
                    </div>

                    // Release version picker (shown when releases list is loaded)
                    {move || {
                        let rlist = releases.get();
                        if rlist.is_empty() { return view! { <></> }.into_any(); }
                        view! {
                            <div style="display: flex; flex-direction: column; gap: 8px; padding: 10px; background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.08); border-radius: 6px">
                                <div style="font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; text-transform: uppercase; letter-spacing: 0.05em">"Select version to install:"</div>
                                <select class="dropdown dropdown-sm"
                                    on:change=move |e| {
                                        let val = event_target_value(&e);
                                        selected_release.set(val.parse::<usize>().ok());
                                    }
                                >
                                    <option value="">"— choose a release —"</option>
                                    {rlist.iter().enumerate().map(|(i, r)| {
                                        let label = if r.prerelease {
                                            format!("{} (nightly)", r.version)
                                        } else {
                                            format!("{} — {}", r.version, &r.published_at[..10])
                                        };
                                        let size_kb = r.installer_size / 1024;
                                        let label = format!("{} [{} KB]", label, size_kb);
                                        view! { <option value=i.to_string()>{label}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                                <button class="btn btn-sm btn-primary" style="align-self: flex-start"
                                    disabled=move || selected_release.get().is_none() || installing_url.get().is_some()
                                    on:click=on_install_selected
                                >
                                    {move || if installing_url.get().is_some() { "Downloading & Installing..." } else { "Install Selected" }}
                                </button>
                            </div>
                        }.into_any()
                    }}
                </div>
            </div>
        </div>
    }
}

#[component]
fn FirmwareSection() -> impl IntoView {
    let fw = RwSignal::new(FirmwareInfo::default());
    let ota_ctx = use_context::<crate::app::OtaContext>().expect("OtaContext must be provided");

    let git_releases = ota_ctx.git_releases;
    let esp32_current = ota_ctx.esp32_current_version;
    let rp2040_current = ota_ctx.rp2040_current_version;
    let ota_progress = ota_ctx.ota_progress;
    let ota_active = ota_ctx.ota_active;
    let ota_error = ota_ctx.ota_error;
    let ota_success = ota_ctx.ota_success;

    let set_ota_active = ota_ctx.set_ota_active;
    let set_ota_error = ota_ctx.set_ota_error;
    let set_ota_success = ota_ctx.set_ota_success;
    let set_ota_progress = ota_ctx.set_ota_progress;

    // Per-device version selection: each device independently picks from available releases
    let selected_esp32_tag = RwSignal::new(String::new());
    let selected_rp2040_tag = RwSignal::new(String::new());
    let selected_spiffs_tag = RwSignal::new(String::new());
    let update_esp32 = RwSignal::new(true);
    let update_rp2040 = RwSignal::new(true);

    // Collapsible manual upload foldout state
    let show_manual = RwSignal::new(false);

    // Local manual upload OTA state
    let manual_status = RwSignal::new(String::new());
    let manual_uploading = RwSignal::new(false);

    // Alive flag for the subcomponent scope
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    // Fetch firmware info
    let alive_fw = alive.clone();
    leptos::task::spawn_local(async move {
        if let Some(info) = fetch_firmware_info().await {
            if alive_fw.load(std::sync::atomic::Ordering::Relaxed) {
                fw.set(info);
            }
        }
    });

    // Auto-init each device to the newest release that has a valid binary for it
    Effect::new(move |_| {
        let releases = git_releases.get();
        if selected_esp32_tag.get_untracked().is_empty() {
            if let Some(r) = releases.iter().find(|r| !r.esp32_url.is_empty()) {
                selected_esp32_tag.set(r.tag.clone());
            }
        }
        if selected_rp2040_tag.get_untracked().is_empty() {
            if let Some(r) = releases.iter().find(|r| !r.rp2040_url.is_empty()) {
                selected_rp2040_tag.set(r.tag.clone());
            }
        }
        if selected_spiffs_tag.get_untracked().is_empty() {
            if let Some(r) = releases.iter().find(|r| !r.spiffs_url.is_empty()) {
                selected_spiffs_tag.set(r.tag.clone());
            }
        }
    });

    let selected_esp32_release = move || {
        let tag = selected_esp32_tag.get();
        git_releases
            .get()
            .into_iter()
            .find(|r| r.tag == tag && !r.esp32_url.is_empty())
    };

    let selected_rp2040_release = move || {
        let tag = selected_rp2040_tag.get();
        git_releases
            .get()
            .into_iter()
            .find(|r| r.tag == tag && !r.rp2040_url.is_empty())
    };

    let selected_spiffs_release = move || {
        let tag = selected_spiffs_tag.get();
        git_releases
            .get()
            .into_iter()
            .find(|r| r.tag == tag && !r.spiffs_url.is_empty())
    };

    // Auto-set checkboxes: enable if the selected version is newer than what's installed
    Effect::new(move |_| {
        let esp_curr = esp32_current.get();
        if let Some(r) = selected_esp32_release() {
            update_esp32.set(
                !esp_curr.is_empty()
                    && !r.esp32_version.is_empty()
                    && crate::app::is_version_newer(&esp_curr, &r.esp32_version),
            );
        }
    });

    Effect::new(move |_| {
        let rp_curr = rp2040_current.get();
        if let Some(r) = selected_rp2040_release() {
            update_rp2040.set(
                !rp_curr.is_empty()
                    && !r.rp2040_version.is_empty()
                    && crate::app::is_version_newer(&rp_curr, &r.rp2040_version),
            );
        }
    });

    let on_start_git_ota = move |_| {
        let up_esp = update_esp32.get();
        let up_rp = update_rp2040.get();
        let esp_release = if up_esp {
            selected_esp32_release()
        } else {
            None
        };
        let rp_release = if up_rp {
            selected_rp2040_release()
        } else {
            None
        };

        if esp_release.is_none() && rp_release.is_none() {
            return;
        }

        set_ota_active.set(true);
        set_ota_success.set(false);
        set_ota_error.set(None);
        set_ota_progress.set(OtaProgress {
            stage: "starting".to_string(),
            percent: 0.0,
            message: "Initializing desktop update sequence...".to_string(),
        });

        let (rp_url, rp_size, rp_sha) = rp_release
            .map(|r| (r.rp2040_url, r.rp2040_size, r.rp2040_sha256))
            .unwrap_or_default();
        let (esp_url, esp_size, esp_sha) = esp_release
            .map(|r| (r.esp32_url, r.esp32_size, r.esp32_sha256))
            .unwrap_or_default();

        leptos::task::spawn_local(async move {
            if let Err(e) = start_desktop_ota(
                up_rp, up_esp, rp_url, rp_size, rp_sha, esp_url, esp_size, esp_sha,
            )
            .await
            {
                set_ota_active.set(false);
                set_ota_error.set(Some(e));
            }
        });
    };

    let on_start_spiffs_ota = move |_| {
        let release = selected_spiffs_release();
        let Some(release) = release else {
            return;
        };

        set_ota_active.set(true);
        set_ota_success.set(false);
        set_ota_error.set(None);
        set_ota_progress.set(OtaProgress {
            stage: "starting".to_string(),
            percent: 0.0,
            message: "Initializing SPIFFS update sequence...".to_string(),
        });

        leptos::task::spawn_local(async move {
            if let Err(e) = start_desktop_spiffs_ota(
                release.spiffs_url,
                release.spiffs_size,
                release.spiffs_sha256,
            )
            .await
            {
                set_ota_active.set(false);
                set_ota_error.set(Some(e));
            }
        });
    };

    view! {
        <div>
            <div class="channel-grid" style="grid-template-columns: 1fr 1fr">
                // Firmware Info card
                <div class="alert-panel">
                    <div class="alert-panel-header">
                        <div class="alert-panel-title">"FIRMWARE INFO"</div>
                        <span class="alert-panel-reg">{move || format!("PROTO v{}", fw.get().proto_version)}</span>
                    </div>
                    <div class="alert-panel-scanline"></div>
                    <div style="padding: 12px 16px; display: grid; grid-template-columns: auto 1fr; gap: 6px 16px; font-size: 11px; font-family: 'JetBrains Mono', monospace">
                        <span style="color: var(--text-muted)">"ESP32 Version:"</span>
                        <span style="color: var(--green); font-weight: 700">{move || {
                            let v = fw.get().fw_version.clone();
                            if v.is_empty() || v == "0.0.0" { "Fetching...".to_string() } else { format!("v{}", v) }
                        }}</span>
                        <span style="color: var(--text-muted)">"RP2040 Version:"</span>
                        <span style="color: var(--green); font-weight: 700">{move || {
                            let v = rp2040_current.get();
                            if v.is_empty() { "Fetching...".to_string() } else { format!("v{}", v) }
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

                // OTA Update card (Git-based)
                <div class="alert-panel">
                    <div class="alert-panel-header">
                        <div class="alert-panel-title">"FIRMWARE UPDATE (GIT)"</div>
                    </div>
                    <div class="alert-panel-scanline supply-scanline"></div>
                    <div style="padding: 12px 16px; display: flex; flex-direction: column; gap: 12px">
                        // Per-device version selection
                        {move || {
                            let releases = git_releases.get();
                            if releases.is_empty() {
                                view! {
                                    <div style="font-size: 11px; color: var(--text-dim); padding: 6px; background: rgba(0,0,0,0.2); border-radius: 4px">
                                        "Querying GitHub releases..."
                                    </div>
                                }.into_any()
                            } else {
                                let esp_releases: Vec<_> = releases.iter().filter(|r| !r.esp32_url.is_empty()).cloned().collect();
                                let rp_releases: Vec<_> = releases.iter().filter(|r| !r.rp2040_url.is_empty()).cloned().collect();
                                view! {
                                    <div style="display: flex; flex-direction: column; gap: 8px; padding: 8px; background: rgba(0,0,0,0.15); border-radius: 6px">
                                        // ESP32 row
                                        <div style="display: flex; align-items: center; gap: 8px">
                                            <label style="display: flex; align-items: center; gap: 6px; font-size: 11px; cursor: pointer; color: var(--text); white-space: nowrap; min-width: 120px">
                                                <input type="checkbox"
                                                    prop:checked=move || update_esp32.get()
                                                    on:change=move |ev| update_esp32.set(event_target_checked(&ev))
                                                    style="accent-color: var(--blue)"
                                                />
                                                "ESP32 Mainboard"
                                            </label>
                                            {if esp_releases.is_empty() {
                                                view! { <span style="font-size: 10px; color: var(--text-dim); flex: 1">"No releases available"</span> }.into_any()
                                            } else {
                                                view! {
                                                    <select class="dropdown"
                                                        style="flex: 1; height: 28px; background: rgba(12,20,38,0.7); border: 1px solid var(--border-bright); color: var(--text); border-radius: 6px; padding: 2px 6px; font-family: 'JetBrains Mono', monospace; font-size: 10px; outline: none"
                                                        prop:value=move || selected_esp32_tag.get()
                                                        on:change=move |ev| selected_esp32_tag.set(event_target_value(&ev))
                                                    >
                                                        {esp_releases.into_iter().map(|r| {
                                                            let tag = r.tag.clone();
                                                            let label = if r.esp32_version.is_empty() {
                                                                tag.clone()
                                                            } else {
                                                                format!("{} (v{})", tag, r.esp32_version)
                                                            };
                                                            view! { <option value=tag>{label}</option> }
                                                        }).collect::<Vec<_>>()}
                                                    </select>
                                                }.into_any()
                                            }}
                                        </div>
                                        // RP2040 row
                                        <div style="display: flex; align-items: center; gap: 8px">
                                            <label style="display: flex; align-items: center; gap: 6px; font-size: 11px; cursor: pointer; color: var(--text); white-space: nowrap; min-width: 120px">
                                                <input type="checkbox"
                                                    prop:checked=move || update_rp2040.get()
                                                    on:change=move |ev| update_rp2040.set(event_target_checked(&ev))
                                                    style="accent-color: var(--blue)"
                                                />
                                                "RP2040 HAT"
                                            </label>
                                            {if rp_releases.is_empty() {
                                                view! { <span style="font-size: 10px; color: var(--text-dim); flex: 1">"No releases available"</span> }.into_any()
                                            } else {
                                                view! {
                                                    <select class="dropdown"
                                                        style="flex: 1; height: 28px; background: rgba(12,20,38,0.7); border: 1px solid var(--border-bright); color: var(--text); border-radius: 6px; padding: 2px 6px; font-family: 'JetBrains Mono', monospace; font-size: 10px; outline: none"
                                                        prop:value=move || selected_rp2040_tag.get()
                                                        on:change=move |ev| selected_rp2040_tag.set(event_target_value(&ev))
                                                    >
                                                        {rp_releases.into_iter().map(|r| {
                                                            let tag = r.tag.clone();
                                                            let label = if r.rp2040_version.is_empty() {
                                                                tag.clone()
                                                            } else {
                                                                format!("{} (v{})", tag, r.rp2040_version)
                                                            };
                                                            view! { <option value=tag>{label}</option> }
                                                        }).collect::<Vec<_>>()}
                                                    </select>
                                                }.into_any()
                                            }}
                                        </div>
                                    </div>
                                }.into_any()
                            }
                        }}

                        // Trigger/Cancel button
                        <div style="display: flex; gap: 8px; align-items: center">
                            <button class="btn btn-sm btn-primary"
                                disabled=move || ota_active.get() || (!update_esp32.get() && !update_rp2040.get()) || (update_esp32.get() && selected_esp32_release().is_none()) || (update_rp2040.get() && selected_rp2040_release().is_none())
                                on:click=on_start_git_ota
                                style="flex: 1"
                            >
                                {move || if ota_active.get() { "Update in progress..." } else { "Perform Update" }}
                            </button>
                        </div>

                        // Active progress bar and status message
                        {move || {
                            if ota_active.get() {
                                let prog = ota_progress.get();
                                view! {
                                    <div style="display: flex; flex-direction: column; gap: 6px; padding: 10px; background: rgba(16,185,129,0.05); border: 1px solid rgba(16,185,129,0.15); border-radius: 6px">
                                        <div style="display: flex; justify-content: space-between; font-size: 10px; font-family: 'JetBrains Mono', monospace">
                                            <span style="color: var(--text-muted); text-transform: uppercase">{prog.stage}</span>
                                            <span style="color: var(--green); font-weight: 700">{format!("{:.1}%", prog.percent)}</span>
                                        </div>
                                        <div style="width: 100%; height: 6px; background: rgba(0,0,0,0.3); border-radius: 3px; overflow: hidden">
                                            <div style:width=move || format!("{}%", ota_progress.get().percent)
                                                style="height: 100%; background: linear-gradient(90deg, #10b981, #34d399); transition: width 0.3s ease; box-shadow: 0 0 8px rgba(16,185,129,0.5)"
                                            ></div>
                                        </div>
                                        <div style="font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; line-height: 1.4">
                                            {prog.message}
                                        </div>
                                    </div>
                                }.into_any()
                            } else {
                                view! { <></> }.into_any()
                            }
                        }}

                        // Success state
                        {move || {
                            if ota_success.get() {
                                view! {
                                    <div style="padding: 10px 12px; background: rgba(16,185,129,0.1); border: 1px solid rgba(16,185,129,0.3); border-radius: 6px; color: var(--green); font-size: 11px; font-family: 'JetBrains Mono', monospace; display: flex; flex-direction: column; gap: 4px">
                                        <b style="font-weight: 700">"✓ UPDATE SUCCESSFUL"</b>
                                        <span>"Firmware updated successfully. The device is now rebooting."</span>
                                    </div>
                                }.into_any()
                            } else {
                                view! { <></> }.into_any()
                            }
                        }}

                        // Error state
                        {move || {
                            if let Some(err) = ota_error.get() {
                                view! {
                                    <div style="padding: 10px 12px; background: rgba(239,68,68,0.1); border: 1px solid rgba(239,68,68,0.3); border-radius: 6px; color: var(--rose); font-size: 11px; font-family: 'JetBrains Mono', monospace; display: flex; flex-direction: column; gap: 4px">
                                        <b style="font-weight: 700">"✗ UPDATE FAILED"</b>
                                        <span>{err}</span>
                                    </div>
                                }.into_any()
                            } else {
                                view! { <></> }.into_any()
                            }
                        }}
                    </div>
                </div>
            </div>

            <div class="alert-panel" style="margin-top: 16px">
                <div class="alert-panel-header">
                    <div class="alert-panel-title">"SPIFFS UPDATE (GIT)"</div>
                </div>
                <div class="alert-panel-scanline supply-scanline"></div>
                <div style="padding: 12px 16px; display: flex; flex-direction: column; gap: 12px">
                    <div style="font-size: 10px; color: var(--text-dim); line-height: 1.6; font-family: 'JetBrains Mono', monospace">
                        "Updates the ESP32 SPIFFS partition used by the web UI. The image comes from the ESP32 release assets and is applied over WiFi or USB."
                    </div>
                    {move || {
                        let releases = git_releases.get();
                        let spiffs_releases: Vec<_> = releases.iter().filter(|r| !r.spiffs_url.is_empty()).cloned().collect();
                        if spiffs_releases.is_empty() {
                            view! {
                                <div style="font-size: 11px; color: var(--text-dim); padding: 6px; background: rgba(0,0,0,0.2); border-radius: 4px">
                                    "No SPIFFS images available in the current release set."
                                </div>
                            }.into_any()
                        } else {
                            view! {
                                <div style="display: flex; align-items: center; gap: 8px">
                                    <label style="display: flex; align-items: center; gap: 6px; font-size: 11px; cursor: pointer; color: var(--text); white-space: nowrap; min-width: 120px">
                                        <span>"SPIFFS"</span>
                                    </label>
                                    <select class="dropdown"
                                        style="flex: 1; height: 28px; background: rgba(12,20,38,0.7); border: 1px solid var(--border-bright); color: var(--text); border-radius: 6px; padding: 2px 6px; font-family: 'JetBrains Mono', monospace; font-size: 10px; outline: none"
                                        prop:value=move || selected_spiffs_tag.get()
                                        on:change=move |ev| selected_spiffs_tag.set(event_target_value(&ev))
                                    >
                                        {spiffs_releases.into_iter().map(|r| {
                                            let tag = r.tag.clone();
                                            let label = if r.spiffs_version.is_empty() {
                                                tag.clone()
                                            } else {
                                                format!("{} (v{})", tag, r.spiffs_version)
                                            };
                                            view! { <option value=tag>{label}</option> }
                                        }).collect::<Vec<_>>()}
                                    </select>
                                    <button class="btn btn-sm btn-primary"
                                        disabled=move || ota_active.get() || selected_spiffs_release().is_none()
                                        on:click=on_start_spiffs_ota
                                        style="white-space: nowrap"
                                    >
                                        {move || if ota_active.get() { "Updating..." } else { "Update SPIFFS" }}
                                    </button>
                                </div>
                            }.into_any()
                        }
                    }}
                    {move || {
                        if ota_active.get() {
                            let prog = ota_progress.get();
                            view! {
                                <div style="display: flex; flex-direction: column; gap: 6px; padding: 10px; background: rgba(16,185,129,0.05); border: 1px solid rgba(16,185,129,0.15); border-radius: 6px">
                                    <div style="display: flex; justify-content: space-between; font-size: 10px; font-family: 'JetBrains Mono', monospace">
                                        <span style="color: var(--text-muted); text-transform: uppercase">{prog.stage}</span>
                                        <span style="color: var(--green); font-weight: 700">{format!("{:.1}%", prog.percent)}</span>
                                    </div>
                                    <div style="width: 100%; height: 6px; background: rgba(0,0,0,0.3); border-radius: 3px; overflow: hidden">
                                        <div style:width=move || format!("{}%", ota_progress.get().percent)
                                            style="height: 100%; background: linear-gradient(90deg, #10b981, #34d399); transition: width 0.3s ease; box-shadow: 0 0 8px rgba(16,185,129,0.5)"
                                        ></div>
                                    </div>
                                    <div style="font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; line-height: 1.4">
                                        {prog.message}
                                    </div>
                                </div>
                            }.into_any()
                        } else {
                            view! { <></> }.into_any()
                        }
                    }}
                </div>
            </div>

            // Collapsible Manual upload fallback
            <div style="margin-top: 16px; border: 1px solid var(--border); border-radius: var(--radius); background: var(--glass); overflow: hidden">
                <button class="btn btn-ghost"
                    style="width: 100%; text-align: left; padding: 10px 16px; display: flex; justify-content: space-between; align-items: center; border-radius: 0; font-size: 10px; font-weight: 600; color: var(--text-dim); text-transform: uppercase; font-family: 'JetBrains Mono', monospace; border: none; background: transparent; cursor: pointer"
                    on:click=move |_| show_manual.set(!show_manual.get())
                >
                    <span>"Advanced: Manual File Upload (Fallback)"</span>
                    <span>{move || if show_manual.get() { "▲" } else { "▼" }}</span>
                </button>

                <Show when=move || show_manual.get()>
                    <div style="padding: 16px; border-top: 1px solid var(--border); display: flex; flex-direction: column; gap: 10px">
                        <div style="font-size: 10px; color: var(--text-dim); line-height: 1.6; font-family: 'JetBrains Mono', monospace">
                            "Directly upload a compiled firmware.bin (ESP32) over WiFi or USB. Choose this only for custom development builds."
                        </div>
                        <div style="display: flex; gap: 8px; align-items: center">
                            <button class="btn btn-sm btn-ghost"
                                disabled=move || manual_uploading.get()
                                on:click=move |_| {
                                    manual_uploading.set(true);
                                    manual_status.set("Selecting file...".to_string());
                                    leptos::task::spawn_local(async move {
                                        #[derive(serde::Deserialize)]
                                        struct DialogResult { path: Option<String> }

                                        let args = serde_wasm_bindgen::to_value(
                                            &serde_json::json!({
                                                "title": "Select Firmware Binary",
                                                "filters": [{"name": "Firmware", "extensions": ["bin"]}]
                                            })
                                        ).unwrap();
                                        let result = try_invoke("plugin:dialog|open", args).await;
                                        let path: Option<String> = result.and_then(|r| serde_wasm_bindgen::from_value(r).ok().flatten());

                                        if let Some(p) = path {
                                            manual_status.set(format!("Uploading {}...", p.split('/').next_back().unwrap_or(&p)));
                                            match upload_firmware(&p).await {
                                                Ok(msg) => manual_status.set(msg),
                                                Err(e) => manual_status.set(format!("Error: {}", e)),
                                            }
                                        } else {
                                            manual_status.set(String::new());
                                        }
                                        manual_uploading.set(false);
                                    });
                                }
                            >
                                {move || if manual_uploading.get() { "Uploading..." } else { "Select & Upload Local .bin" }}
                            </button>
                        </div>
                        <div style="font-size: 10px; font-family: 'JetBrains Mono', monospace; min-height: 16px"
                            style:color=move || if manual_status.get().starts_with("Error") { "var(--rose)" } else { "var(--green)" }
                        >
                            {move || manual_status.get()}
                        </div>
                    </div>
                </Show>
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

    // Alive flag for the subcomponent scope
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    // Poll WiFi status every 2 seconds
    let alive_poll = alive.clone();
    let poll = move || {
        let alive = alive_poll.clone();
        leptos::task::spawn_local(async move {
            if let Some(ws) = fetch_wifi_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    wifi.set(ws);
                }
            }
        });
    };
    poll();
    let handle =
        leptos::prelude::set_interval_with_handle(poll, std::time::Duration::from_secs(2)).ok();
    on_cleanup(move || {
        if let Some(h) = handle {
            h.clear();
        }
    });

    let alive_scan = alive.clone();
    let do_scan = move |_| {
        let alive = alive_scan.clone();
        scanning.set(true);
        connect_status.set("Scanning...".to_string());
        leptos::task::spawn_local(async move {
            let results = fetch_wifi_scan().await;
            if !alive.load(std::sync::atomic::Ordering::Relaxed) {
                return;
            }
            let count = results.len();
            if count > 0 && connect_ssid.get().is_empty() {
                connect_ssid.set(results[0].ssid.clone());
            }
            scan_results.set(results);
            scanning.set(false);
            if count == 0 {
                connect_status.set("No networks found".to_string());
            } else {
                connect_status.set(format!(
                    "Found {} network{}",
                    count,
                    if count == 1 { "" } else { "s" }
                ));
            }
        });
    };

    let alive_forget = alive.clone();
    let do_forget = move |_| {
        let alive = alive_forget.clone();
        connect_status.set("Clearing credentials...".to_string());
        leptos::task::spawn_local(async move {
            let ok = crate::tauri_bridge::wifi_forget().await;
            if !alive.load(std::sync::atomic::Ordering::Relaxed) {
                return;
            }
            connect_status.set(if ok {
                "Credentials cleared".to_string()
            } else {
                "Failed to clear credentials".to_string()
            });
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
                                let result = try_invoke("wifi_connect", args).await;
                                let ok: bool = result.and_then(|r| serde_wasm_bindgen::from_value(r).ok()).unwrap_or(false);
                                if ok {
                                    connect_status.set(format!("Connected to {}", ssid));
                                } else {
                                    connect_status.set(format!("Failed to connect to {}", ssid));
                                }
                            });
                        }
                    >"Connect"</button>
                </div>
                <div style="display: flex; align-items: center; justify-content: space-between; margin-top: 0.5rem; gap: 0.5rem">
                    <div class="text-xs" style=move || {
                        let s = connect_status.get();
                        let color = if s == "No networks found" || s.starts_with("Failed") {
                            "color: var(--rose)"
                        } else if s.starts_with("Found") || s.starts_with("Connected to") || s == "Credentials cleared" {
                            "color: var(--green)"
                        } else {
                            "color: var(--text-dim)"
                        };
                        color.to_string()
                    }>
                        {move || connect_status.get()}
                    </div>
                    <button class="btn btn-sm"
                        style="white-space: nowrap; background: rgba(239,68,68,0.15); border-color: rgba(239,68,68,0.3); color: var(--rose); font-size: 0.7rem"
                        on:click=do_forget
                    >"Clear Saved Credentials"</button>
                </div>
            </div>
        </div>
    }
}
