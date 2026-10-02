use crate::components::icons::Icon;
use crate::components::ui::{Callout, Readout, Switch};
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

const SLOT_COLORS: [&str; 4] = ["var(--ch-a)", "var(--ch-b)", "var(--ch-c)", "var(--ch-d)"];

#[derive(Clone, Copy, PartialEq, Eq)]
enum Pane {
    Health,
    Channels,
    Firmware,
    Wifi,
    App,
}

const PANES: &[(Pane, &str, &str)] = &[
    (Pane::Health, "Health", "stethoscope"),
    (Pane::Channels, "Channels", "activity"),
    (Pane::Firmware, "Firmware", "cpu"),
    (Pane::Wifi, "WiFi", "wifi"),
    (Pane::App, "App Update", "download"),
];

fn flag_tone(color: &str) -> &'static str {
    match color {
        "rose" => "red",
        "amber" => "orange",
        "blue" => "blue",
        _ => "green",
    }
}

fn active_count(v: u16) -> usize {
    (0..16).filter(|b| (v >> b) & 1 != 0).count()
}

/// (tone, label) for the die temperature.
fn temp_status(t: f32) -> (&'static str, &'static str) {
    if t > 100.0 {
        ("red", "Hot")
    } else if t > 70.0 {
        ("orange", "Warm")
    } else {
        ("green", "Normal")
    }
}

/// Nominal display range per diagnostic source, used only for the bar gauge.
fn diag_range(source: u8) -> (f32, f32) {
    match source {
        0 => (0.0, 0.5),
        1 => (0.0, 125.0),
        2 => (4.5, 5.5),
        3 => (4.5, 5.5),
        4 => (1.6, 2.0),
        5 => (0.0, 33.0),
        6 => (4.5, 5.5),
        7 => (-24.0, 0.0),
        8 => (0.0, 30.0),
        9 => (0.0, 30.0),
        10 => (0.0, 5.0),
        11 => (0.0, 5.0),
        _ => (0.0, 5.0),
    }
}

/// Register bit grid with a status badge. Set bits are marked by text, not colour alone.
#[component]
fn FlagPanel(
    title: &'static str,
    icon: &'static str,
    bits: &'static [(usize, &'static str, &'static str)],
    #[prop(into)] status: Signal<u16>,
    #[prop(into)] reserved: Signal<bool>,
    reserved_name: &'static str,
) -> impl IntoView {
    view! {
        <div class="group dg-flags">
            <div class="group-header">
                <span class="group-title"><Icon name=icon size=16 />{title}</span>
                <div class="group-actions">
                    <span class="dg-mono dg-reg" title="Register value">{move || format!("0x{:04X}", status.get())}</span>
                    <span class={move || if active_count(status.get()) > 0 { "badge tone-red" } else { "badge tone-green" }}>
                        {move || if active_count(status.get()) > 0 {
                            view! { <Icon name="triangle-alert" size=12 /> }.into_any()
                        } else {
                            view! { <Icon name="circle-check" size=12 /> }.into_any()
                        }}
                        {move || {
                            let n = active_count(status.get());
                            if n == 0 { "All clear".to_string() } else { format!("{} active", n) }
                        }}
                    </span>
                </div>
            </div>
            <div class="dg-flag-grid">
                {bits.iter().map(|(bit, name, color)| {
                    let bit = *bit;
                    let tone = flag_tone(color);
                    let is_res = !reserved_name.is_empty() && *name == reserved_name;
                    let on = move || (status.get() >> bit) & 1 != 0;
                    let mon = move || !on() && is_res && reserved.get();
                    view! {
                        <div
                            class=move || format!("dg-flag tone-{}{}{}", tone,
                                if on() { " on" } else { "" },
                                if mon() { " mon" } else { "" })
                            title=move || if mon() { "Reserved for supply monitoring".to_string() } else { String::new() }
                        >
                            <span class="dg-flag-bit dg-mono">{format!("{:02}", bit)}</span>
                            <span class="dg-flag-name">{*name}</span>
                            <span class="dg-flag-state">
                                {move || if on() { "SET" } else if mon() { "MON" } else { "clear" }}
                            </span>
                        </div>
                    }
                }).collect::<Vec<_>>()}
            </div>
        </div>
    }
}

#[component]
pub fn DiagTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let pane = RwSignal::new(Pane::Health);
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

    let on_worker_toggle = move || {
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

    let alert_total = move || {
        let s = state.get();
        active_count(s.alert_status) + active_count(s.supply_alert_status)
    };
    let update_available = move || update_info.get().map(|i| i.available).unwrap_or(false);

    view! {
        <div class="view dg">
            <div class="dg-shell">
                // ============ SECTION LIST ============
                <nav class="dg-nav" aria-label="Diagnostics sections">
                    {PANES.iter().map(|(p, label, icon)| {
                        let p = *p;
                        view! {
                            <button type="button" class="dg-nav-item"
                                class:active=move || pane.get() == p
                                aria-current=move || if pane.get() == p { "page" } else { "false" }
                                on:click=move |_| pane.set(p)
                            >
                                <Icon name=*icon size=15 />
                                <span class="dg-nav-label">{*label}</span>
                                {match p {
                                    Pane::Health => view! {
                                        <span class="badge tone-red dg-nav-badge"
                                            style:display={move || if alert_total() > 0 { "inline-flex" } else { "none" }}
                                            title="Active alert bits"
                                        >{move || alert_total().to_string()}</span>
                                    }.into_any(),
                                    Pane::App => view! {
                                        <span class="badge tone-blue dg-nav-badge"
                                            style:display=move || if update_available() { "inline-flex" } else { "none" }
                                            title="A desktop update is available"
                                        >"New"</span>
                                    }.into_any(),
                                    _ => view! { <></> }.into_any(),
                                }}
                            </button>
                        }
                    }).collect::<Vec<_>>()}
                </nav>

                <div class="dg-content">
                    // ============ HEALTH ============
                    <section class="dg-pane" class:dg-hidden=move || pane.get() != Pane::Health>
                        <p class="dg-desc">"AD74416H die temperature, alert registers and the periodic supply selftest."</p>
                        <div class="dg-health">
                            <div class="dg-side">
                                <div class="group dg-temp">
                                    <div class="group-header">
                                        <span class="group-title"><Icon name="thermometer" size=16 />"Die temperature"</span>
                                        <span class=move || format!("badge tone-{}", temp_status(state.get().die_temperature).0)>
                                            {move || temp_status(state.get().die_temperature).1}
                                        </span>
                                    </div>
                                    <Readout
                                        label="AD74416H"
                                        value=Signal::derive(move || format!("{:.1}", state.get().die_temperature))
                                        unit="°C"
                                        size="lg"
                                        tone=Signal::derive(move || temp_status(state.get().die_temperature).0)
                                    />
                                    <div class=move || format!("dg-thermo tone-{}", temp_status(state.get().die_temperature).0)
                                        role="progressbar" aria-label="Die temperature" aria-valuemin="0" aria-valuemax="125"
                                        aria-valuenow=move || format!("{:.0}", state.get().die_temperature)>
                                        <span style=move || {
                                            let pct = ((state.get().die_temperature / 125.0) * 100.0).clamp(2.0, 100.0);
                                            format!("width: {:.1}%", pct)
                                        }></span>
                                    </div>
                                    <div class="dg-scale"><span>"0"</span><span>"70 warm"</span><span>"100 hot"</span><span>"125 °C"</span></div>
                                </div>

                                <div class="group">
                                    <div class="group-header">
                                        <span class="group-title"><Icon name="activity" size=16 />"Selftest worker"</span>
                                        <span class=move || if supply_monitor_active.get() { "badge tone-orange" } else { "badge" }>
                                            {move || if supply_monitor_active.get() { "CH-D reserved" } else { "Inactive" }}
                                        </span>
                                    </div>
                                    <div class="row dg-worker">
                                        <span>
                                            <span class="row-label">"Supply-rail selftest"</span>
                                            <span class="row-hint">"Runs periodically. While active, channel D is reserved for internal measurements."</span>
                                        </span>
                                        <Switch
                                            checked=Signal::derive(move || worker_enabled.get())
                                            disabled=Signal::derive(move || worker_toggling.get())
                                            aria_label="Selftest worker"
                                            on_change=Callback::new(move |_: bool| on_worker_toggle())
                                        />
                                    </div>
                                </div>
                            </div>

                            <div class="dg-flags-col">
                                <FlagPanel title="Alert status" icon="triangle-alert" bits=ALERT_BITS
                                    status=Signal::derive(move || state.get().alert_status)
                                    reserved=supply_monitor_active reserved_name="CH_D" />
                                <FlagPanel title="Supply status" icon="zap" bits=SUPPLY_BITS
                                    status=Signal::derive(move || state.get().supply_alert_status)
                                    reserved=Signal::derive(|| false) reserved_name="" />
                            </div>
                        </div>
                    </section>

                    // ============ CHANNELS ============
                    <section class="dg-pane" class:dg-hidden=move || pane.get() != Pane::Channels>
                        <p class="dg-desc">"Internal diagnostic ADC channels. Choose what each slot measures: die temperature, supply rails or sense voltages."</p>
                        <div class="dg-slots">
                            {(0..4usize).map(|i| {
                                let slot = i as u8;
                                let src = move || state.with(|s| s.diag[i].source);
                                view! {
                                    <div class="group dg-slot" style=format!("--slot: {}", SLOT_COLORS[i])>
                                        <div class="group-header">
                                            <span class="group-title"><i class="dg-slot-dot"></i>{format!("Slot {}", i)}</span>
                                            <select class="dropdown dropdown-sm dg-src"
                                                aria-label=format!("Diagnostic source for slot {}", i)
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
                                                    let code = *code;
                                                    view! { <option value=code.to_string() selected=move || src() == code>{*name}</option> }
                                                }).collect::<Vec<_>>()}
                                            </select>
                                        </div>
                                        <div class="big-value dg-val">
                                            {move || format!("{:.3}", state.with(|s| s.diag[i].value))}
                                            <span class="unit">{move || if src() == 1 { "°C" } else { "V" }}</span>
                                        </div>
                                        <div class="progress dg-bar" role="presentation">
                                            <span style=move || {
                                                let (lo, hi) = diag_range(src());
                                                let v = state.with(|s| s.diag[i].value);
                                                let pct = if hi > lo { ((v - lo) / (hi - lo) * 100.0).clamp(0.0, 100.0) } else { 0.0 };
                                                format!("width: {:.1}%", pct)
                                            }></span>
                                        </div>
                                        <dl class="kv dg-kv">
                                            <dt>"Source"</dt>
                                            <dd>{move || DIAG_SOURCE_OPTIONS.iter().find(|(c, _)| *c == src()).map(|(_, n)| *n).unwrap_or("?")}</dd>
                                            <dt>"Raw code"</dt>
                                            <dd class="dg-mono">{move || format!("0x{:04X}", state.with(|s| s.diag[i].raw_code))}</dd>
                                            <dt>"Gauge range"</dt>
                                            <dd class="dg-mono">{move || { let (lo, hi) = diag_range(src()); format!("{} to {}", lo, hi) }}</dd>
                                        </dl>
                                    </div>
                                }
                            }).collect::<Vec<_>>()}
                        </div>
                    </section>

                    // ============ FIRMWARE ============
                    <section class="dg-pane" class:dg-hidden=move || pane.get() != Pane::Firmware>
                        <FirmwareSection />
                    </section>

                    // ============ WIFI ============
                    <section class="dg-pane" class:dg-hidden=move || pane.get() != Pane::Wifi>
                        <WifiSection />
                    </section>

                    // ============ APP UPDATE ============
                    <section class="dg-pane" class:dg-hidden=move || pane.get() != Pane::App>
                        <p class="dg-desc">"Check for a newer BugBuster desktop build, or install a specific release."</p>
                        <div class="group dg-app">
                            <div class="group-header">
                                <span class="group-title"><Icon name="download" size=16 />"Desktop app"</span>
                                <span class="badge dg-mono">{move || {
                                    update_info.get()
                                        .map(|i| format!("v{}", i.current_version))
                                        .unwrap_or_else(|| "v-".to_string())
                                }}</span>
                            </div>

                            {move || {
                                if update_checking.get() {
                                    view! {
                                        <div class="dg-status"><span class="spinner"></span>"Checking for updates..."</div>
                                    }.into_any()
                                } else if let Some(info) = update_info.get() {
                                    if info.available {
                                        let notes_preview = if info.notes.chars().count() > 200 {
                                            format!("{}…", info.notes.chars().take(200).collect::<String>())
                                        } else { info.notes.clone() };
                                        view! {
                                            <Callout tone="blue">
                                                <strong>{format!("v{} available", info.version)}</strong>
                                                {(!notes_preview.is_empty()).then(|| view! { <pre class="dg-notes">{notes_preview}</pre> })}
                                                <div class="dg-callout-actions">
                                                    <button class="btn btn-sm btn-primary"
                                                        disabled=move || update_installing.get() || update_checking.get()
                                                        on:click=on_install
                                                    >{move || if update_installing.get() { "Installing..." } else { "Install & Restart" }}</button>
                                                </div>
                                            </Callout>
                                        }.into_any()
                                    } else {
                                        view! {
                                            <div class="dg-status tone-text-green">
                                                <Icon name="circle-check" size=15 />"Up to date"
                                            </div>
                                        }.into_any()
                                    }
                                } else { view! { <></> }.into_any() }
                            }}

                            {move || update_error.get().map(|err| view! {
                                <Callout tone="red"><span class="dg-mono">{err}</span></Callout>
                            })}

                            <div class="dg-actions">
                                <button class="btn btn-sm"
                                    disabled=move || update_checking.get() || update_installing.get()
                                    on:click=on_check
                                ><Icon name="refresh-cw" size=14 />"Check for updates"</button>
                                <button class="btn btn-sm"
                                    disabled=move || releases_loading.get()
                                    on:click=on_load_releases
                                ><Icon name="list" size=14 />{move || if releases_loading.get() { "Loading..." } else { "Browse releases" }}</button>
                            </div>

                            {move || {
                                let rlist = releases.get();
                                if rlist.is_empty() { return view! { <></> }.into_any(); }
                                view! {
                                    <div class="dg-release">
                                        <h4 class="dg-sub">"Install a specific version"</h4>
                                        <div class="dg-release-row">
                                            <select class="dropdown dg-wide" aria-label="Release to install"
                                                on:change=move |e| {
                                                    let val = event_target_value(&e);
                                                    selected_release.set(val.parse::<usize>().ok());
                                                }
                                            >
                                                <option value="">"Choose a release"</option>
                                                {rlist.iter().enumerate().map(|(i, r)| {
                                                    let label = if r.prerelease {
                                                        format!("{} (nightly)", r.version)
                                                    } else {
                                                        let date = r.published_at.get(..10).unwrap_or(&r.published_at);
                                                        format!("{} - {}", r.version, date)
                                                    };
                                                    let size_kb = r.installer_size / 1024;
                                                    let label = format!("{} [{} KB]", label, size_kb);
                                                    view! { <option value=i.to_string()>{label}</option> }
                                                }).collect::<Vec<_>>()}
                                            </select>
                                            <button class="btn btn-sm btn-primary"
                                                disabled=move || selected_release.get().is_none() || installing_url.get().is_some()
                                                title=move || if selected_release.get().is_none() { "Choose a release first" } else { "" }
                                                on:click=on_install_selected
                                            >
                                                {move || if installing_url.get().is_some() { "Downloading & installing..." } else { "Install selected" }}
                                            </button>
                                        </div>
                                    </div>
                                }.into_any()
                            }}
                        </div>
                    </section>
                </div>
            </div>

            // Startup popup, shown when a desktop update is available
            {move || if show_update_popup.get() {
                let info = update_info.get();
                view! {
                    <div class="scrim dg-scrim">
                        <div class="dg-dialog" role="dialog" aria-modal="true" aria-labelledby="dg-upd-title">
                            <span class="badge tone-blue">"Update available"</span>
                            <div class="dg-dialog-title" id="dg-upd-title">
                                {info.as_ref().map(|i| format!("v{}", i.version)).unwrap_or_default()}
                            </div>
                            <div class="dg-dialog-sub">
                                {info.as_ref().map(|i| {
                                    if i.is_nightly {
                                        format!("A newer nightly build is available (you have v{})", i.current_version)
                                    } else {
                                        format!("A new stable release is available (you have v{})", i.current_version)
                                    }
                                }).unwrap_or_default()}
                            </div>
                            <div class="dg-dialog-actions">
                                <button class="btn btn-sm btn-ghost"
                                    on:click=move |_| { show_update_popup.set(false); }
                                >"Later"</button>
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
                            </div>
                        </div>
                    </div>
                }.into_any()
            } else {
                view! { <></> }.into_any()
            }}
        </div>
    }
}

/// Shared progress block for any in-flight OTA (git-release, SPIFFS, or DAQ HAT
/// push) — reads the same `ota_progress`/`ota_active` signals fed by the single
/// "desktop-ota-progress" event stream, so no OTA flow needs its own copy.
#[component]
fn OtaProgressBar(ota_progress: ReadSignal<OtaProgress>) -> impl IntoView {
    view! {
        <div class="dg-ota" role="status">
            <div class="dg-ota-head">
                <span class="dg-ota-stage">{move || ota_progress.get().stage}</span>
                <span class="dg-mono">{move || format!("{:.1}%", ota_progress.get().percent)}</span>
            </div>
            <div class="progress"><span style=move || format!("width: {}%", ota_progress.get().percent)></span></div>
            <div class="dg-ota-msg">{move || ota_progress.get().message}</div>
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

    // Push a locally built P4/C6 image to the DAQ HAT over HTTP.
    let push_daq = move |target: &'static str, title: &'static str, msg: &'static str| {
        set_ota_active.set(true);
        set_ota_success.set(false);
        set_ota_error.set(None);
        set_ota_progress.set(OtaProgress {
            stage: "starting".to_string(),
            percent: 0.0,
            message: msg.to_string(),
        });
        leptos::task::spawn_local(async move {
            let args = serde_wasm_bindgen::to_value(
                &serde_json::json!({
                    "title": title,
                    "filters": [{"name": "Firmware", "extensions": ["bin"]}]
                })
            ).unwrap();
            let result = try_invoke("plugin:dialog|open", args).await;
            let path: Option<String> = result.and_then(|r| serde_wasm_bindgen::from_value(r).ok().flatten());
            let Some(path) = path else {
                set_ota_active.set(false);
                return;
            };
            if let Err(e) = upload_daq_image(target, &path).await {
                set_ota_active.set(false);
                set_ota_error.set(Some(e));
            }
        });
    };

    view! {
        <div class="dg-stack">
            <p class="dg-desc">"Installed firmware, release updates over the network, and manual image uploads."</p>

            {move || ota_success.get().then(|| view! {
                <Callout tone="green">
                    <strong>"Update successful"</strong>
                    <div>"Firmware updated successfully. The device is now rebooting."</div>
                </Callout>
            })}
            {move || ota_error.get().map(|err| view! {
                <Callout tone="red">
                    <strong>"Update failed"</strong>
                    <div class="dg-mono">{err}</div>
                </Callout>
            })}

            <div class="dg-cols">
                // Firmware info
                <div class="group">
                    <div class="group-header">
                        <span class="group-title"><Icon name="cpu" size=16 />"Firmware info"</span>
                        <span class="badge dg-mono" title="BBP protocol version">{move || format!("PROTO v{}", fw.get().proto_version)}</span>
                    </div>
                    <dl class="kv dg-kv">
                        <dt>"ESP32 mainboard"</dt>
                        <dd class="dg-mono dg-ver">{move || {
                            let v = fw.get().fw_version.clone();
                            if v.is_empty() || v == "0.0.0" { "Fetching...".to_string() } else { format!("v{}", v) }
                        }}</dd>
                        <dt>"RP2040 HAT"</dt>
                        <dd class="dg-mono dg-ver">{move || {
                            let v = rp2040_current.get();
                            if v.is_empty() { "Fetching...".to_string() } else { format!("v{}", v) }
                        }}</dd>
                        <dt>"Built"</dt>
                        <dd class="dg-mono">{move || {
                            let d = fw.get().build_date.clone();
                            if d.is_empty() { "-".to_string() } else { d }
                        }}</dd>
                        <dt>"ESP-IDF"</dt>
                        <dd class="dg-mono">{move || {
                            let v = fw.get().idf_version.clone();
                            if v.is_empty() { "-".to_string() } else { v }
                        }}</dd>
                        <dt>"Partition"</dt>
                        <dd class="dg-mono">{move || {
                            let p = fw.get().partition.clone();
                            let n = fw.get().next_partition.clone();
                            if p.is_empty() { "-".to_string() } else { format!("{} (next: {})", p, n) }
                        }}</dd>
                    </dl>
                </div>

                // Git release update
                <div class="group">
                    <div class="group-header">
                        <span class="group-title"><Icon name="cloud-download" size=16 />"Firmware update (GitHub)"</span>
                    </div>
                    {move || {
                        let releases = git_releases.get();
                        if releases.is_empty() {
                            view! {
                                <div class="dg-status"><span class="spinner"></span>"Querying GitHub releases..."</div>
                            }.into_any()
                        } else {
                            let esp_releases: Vec<_> = releases.iter().filter(|r| !r.esp32_url.is_empty()).cloned().collect();
                            let rp_releases: Vec<_> = releases.iter().filter(|r| !r.rp2040_url.is_empty()).cloned().collect();
                            view! {
                                <div class="rows">
                                    <div class="row dg-dev">
                                        <label class="dg-check">
                                            <input type="checkbox"
                                                prop:checked=move || update_esp32.get()
                                                on:change=move |ev| update_esp32.set(event_target_checked(&ev))
                                            />
                                            <span>
                                                <span class="row-label">"ESP32 mainboard"</span>
                                                <span class="row-hint dg-mono">{move || {
                                                    let v = esp32_current.get();
                                                    if v.is_empty() { "installed: unknown".to_string() } else { format!("installed: v{}", v) }
                                                }}</span>
                                            </span>
                                        </label>
                                        {if esp_releases.is_empty() {
                                            view! { <span class="subtle text-footnote">"No releases available"</span> }.into_any()
                                        } else {
                                            view! {
                                                <select class="dropdown dg-wide dg-mono" aria-label="ESP32 release"
                                                    on:change=move |ev| selected_esp32_tag.set(event_target_value(&ev))
                                                >
                                                    {esp_releases.into_iter().map(|r| {
                                                        let tag = r.tag.clone();
                                                        let sel_tag = tag.clone();
                                                        let label = if r.esp32_version.is_empty() {
                                                            tag.clone()
                                                        } else {
                                                            format!("{} (v{})", tag, r.esp32_version)
                                                        };
                                                        view! { <option value=tag selected=move || selected_esp32_tag.get() == sel_tag>{label}</option> }
                                                    }).collect::<Vec<_>>()}
                                                </select>
                                            }.into_any()
                                        }}
                                    </div>
                                    <div class="row dg-dev">
                                        <label class="dg-check">
                                            <input type="checkbox"
                                                prop:checked=move || update_rp2040.get()
                                                on:change=move |ev| update_rp2040.set(event_target_checked(&ev))
                                            />
                                            <span>
                                                <span class="row-label">"RP2040 HAT"</span>
                                                <span class="row-hint dg-mono">{move || {
                                                    let v = rp2040_current.get();
                                                    if v.is_empty() { "installed: unknown".to_string() } else { format!("installed: v{}", v) }
                                                }}</span>
                                            </span>
                                        </label>
                                        {if rp_releases.is_empty() {
                                            view! { <span class="subtle text-footnote">"No releases available"</span> }.into_any()
                                        } else {
                                            view! {
                                                <select class="dropdown dg-wide dg-mono" aria-label="RP2040 release"
                                                    on:change=move |ev| selected_rp2040_tag.set(event_target_value(&ev))
                                                >
                                                    {rp_releases.into_iter().map(|r| {
                                                        let tag = r.tag.clone();
                                                        let sel_tag = tag.clone();
                                                        let label = if r.rp2040_version.is_empty() {
                                                            tag.clone()
                                                        } else {
                                                            format!("{} (v{})", tag, r.rp2040_version)
                                                        };
                                                        view! { <option value=tag selected=move || selected_rp2040_tag.get() == sel_tag>{label}</option> }
                                                    }).collect::<Vec<_>>()}
                                                </select>
                                            }.into_any()
                                        }}
                                    </div>
                                </div>
                            }.into_any()
                        }
                    }}
                    <button class="btn btn-sm btn-primary btn-block"
                        disabled=move || ota_active.get() || (!update_esp32.get() && !update_rp2040.get()) || (update_esp32.get() && selected_esp32_release().is_none()) || (update_rp2040.get() && selected_rp2040_release().is_none())
                        title=move || if !update_esp32.get() && !update_rp2040.get() { "Tick at least one device" } else { "" }
                        on:click=on_start_git_ota
                    >
                        {move || if ota_active.get() { "Update in progress..." } else { "Perform update" }}
                    </button>
                    {move || ota_active.get().then(|| view! { <OtaProgressBar ota_progress=ota_progress /> })}
                </div>
            </div>

            // SPIFFS
            <div class="group">
                <div class="group-header">
                    <span class="group-title"><Icon name="hard-drive" size=16 />"SPIFFS update (GitHub)"</span>
                </div>
                <p class="dg-hint">"Updates the ESP32 SPIFFS partition used by the web UI. The image comes from the ESP32 release assets and is applied over WiFi or USB."</p>
                {move || {
                    let releases = git_releases.get();
                    let spiffs_releases: Vec<_> = releases.iter().filter(|r| !r.spiffs_url.is_empty()).cloned().collect();
                    if spiffs_releases.is_empty() {
                        view! {
                            <div class="dg-status subtle">"No SPIFFS images available in the current release set."</div>
                        }.into_any()
                    } else {
                        view! {
                            <div class="dg-release-row">
                                <select class="dropdown dg-wide dg-mono" aria-label="SPIFFS release"
                                    on:change=move |ev| selected_spiffs_tag.set(event_target_value(&ev))
                                >
                                    {spiffs_releases.into_iter().map(|r| {
                                        let tag = r.tag.clone();
                                        let sel_tag = tag.clone();
                                        let label = if r.spiffs_version.is_empty() {
                                            tag.clone()
                                        } else {
                                            format!("{} (v{})", tag, r.spiffs_version)
                                        };
                                        view! { <option value=tag selected=move || selected_spiffs_tag.get() == sel_tag>{label}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                                <button class="btn btn-sm btn-primary"
                                    disabled=move || ota_active.get() || selected_spiffs_release().is_none()
                                    on:click=on_start_spiffs_ota
                                >
                                    {move || if ota_active.get() { "Updating..." } else { "Update SPIFFS" }}
                                </button>
                            </div>
                        }.into_any()
                    }
                }}
                {move || ota_active.get().then(|| view! { <OtaProgressBar ota_progress=ota_progress /> })}
            </div>

            // DAQ HAT
            <div class="group">
                <div class="group-header">
                    <span class="group-title"><Icon name="upload" size=16 />"DAQ HAT (P4 / C6) update"</span>
                </div>
                <p class="dg-hint">"Push a locally built P4 or C6 image straight to the DAQ HAT over HTTP. Progress is the device's own byte count from the upload stream, not an estimate. The HTTP response commits before the device knows the outcome, so only the final status line is authoritative."</p>
                <div class="dg-actions">
                    <button class="btn btn-sm"
                        disabled=move || ota_active.get()
                        on:click=move |_| push_daq("p4", "Select P4 Firmware Image", "Selecting P4 image...")
                    >
                        <Icon name="file-up" size=14 />
                        {move || if ota_active.get() { "Uploading..." } else { "Push P4 image" }}
                    </button>
                    <button class="btn btn-sm"
                        disabled=move || ota_active.get()
                        title="The C6 needs a merged image that starts at flash offset 0"
                        on:click=move |_| push_daq("c6", "Select C6 Merged Firmware Image", "Selecting C6 image...")
                    >
                        <Icon name="file-up" size=14 />
                        {move || if ota_active.get() { "Uploading..." } else { "Push C6 image (merged)" }}
                    </button>
                </div>
                {move || ota_active.get().then(|| view! { <OtaProgressBar ota_progress=ota_progress /> })}
            </div>

            // Manual upload fallback
            <div class="group dg-manual">
                <button class="dg-disclose" type="button"
                    aria-expanded=move || if show_manual.get() { "true" } else { "false" }
                    on:click=move |_| show_manual.set(!show_manual.get())
                >
                    <Icon name="chevron-right" size=14 class="dg-chev" />
                    <span>"Advanced: manual file upload (fallback)"</span>
                </button>
                <Show when=move || show_manual.get()>
                    <div class="dg-manual-body">
                        <p class="dg-hint">"Directly upload a compiled firmware.bin (ESP32) over WiFi or USB. Choose this only for custom development builds."</p>
                        <div class="dg-actions">
                            <button class="btn btn-sm"
                                disabled=move || manual_uploading.get()
                                on:click=move |_| {
                                    manual_uploading.set(true);
                                    manual_status.set("Selecting file...".to_string());
                                    leptos::task::spawn_local(async move {
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
                                <Icon name="folder-open" size=14 />
                                {move || if manual_uploading.get() { "Uploading..." } else { "Select & upload local .bin" }}
                            </button>
                        </div>
                        <div class=move || if manual_status.get().starts_with("Error") { "dg-msg tone-text-red" } else { "dg-msg tone-text-green" }>
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
    // DESK-9: SoftAP password change.
    let ap_pass = RwSignal::new(String::new());
    let ap_status = RwSignal::new(String::new());
    let set_ap_password = move |_| {
        let pass = ap_pass.get_untracked();
        if !(8..=63).contains(&pass.len()) {
            ap_status.set("AP password must be 8-63 characters".to_string());
            return;
        }
        ap_status.set("Applying...".to_string());
        leptos::task::spawn_local(async move {
            #[derive(serde::Serialize)]
            struct Args { password: String }
            let args = serde_wasm_bindgen::to_value(&Args { password: pass }).unwrap();
            #[derive(serde::Deserialize)]
            struct Res { applied: bool, persisted: bool }
            let res: Option<Res> = try_invoke("wifi_set_ap_password", args)
                .await
                .and_then(|r| serde_wasm_bindgen::from_value(r).ok());
            ap_status.set(match res {
                Some(Res { applied: true, persisted: true }) => "AP password changed".to_string(),
                Some(Res { applied: true, persisted: false }) =>
                    "Applied, but not saved: the old password returns after a reboot".to_string(),
                _ => "Failed to change the AP password".to_string(),
            });
            ap_pass.set(String::new());
        });
    };
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
        <div class="dg-stack">
            <p class="dg-desc">"Mainboard WiFi: its own access point, the network it joins as a station, and saved credentials."</p>
            <div class="dg-cols">
                // AP Mode card
                <div class="group">
                    <div class="group-header">
                        <span class="group-title"><Icon name="radio" size=16 />"Access point"</span>
                    </div>
                    <dl class="kv dg-kv">
                        <dt>"SSID"</dt><dd>{move || wifi.get().ap_ssid.clone()}</dd>
                        <dt>"IP"</dt><dd class="dg-mono">{move || wifi.get().ap_ip.clone()}</dd>
                        <dt>"MAC"</dt><dd class="dg-mono">{move || wifi.get().ap_mac.clone()}</dd>
                    </dl>
                </div>

                // STA Mode card
                <div class="group">
                    <div class="group-header">
                        <span class="group-title"><Icon name="wifi" size=16 />"Station"</span>
                        <span class=move || if wifi.get().connected { "badge tone-green" } else { "badge tone-red" }>
                            {move || if wifi.get().connected {
                                view! { <Icon name="check" size=12 /> }.into_any()
                            } else {
                                view! { <Icon name="x" size=12 /> }.into_any()
                            }}
                            {move || if wifi.get().connected { "Connected" } else { "Disconnected" }}
                        </span>
                    </div>
                    <dl class="kv dg-kv">
                        <dt>"SSID"</dt><dd>{move || wifi.get().sta_ssid.clone()}</dd>
                        <dt>"IP"</dt><dd class="dg-mono">{move || wifi.get().sta_ip.clone()}</dd>
                        <dt>"RSSI"</dt>
                        <dd class="dg-rssi">
                            <span class="dg-mono">{move || format!("{} dBm", wifi.get().rssi)}</span>
                            <div class="progress dg-rssi-bar" role="presentation">
                                <span style=move || {
                                    let rssi = wifi.get().rssi;
                                    let pct = (((rssi + 100) as f32 / 60.0) * 100.0).clamp(0.0, 100.0);
                                    format!("width: {}%", pct)
                                }></span>
                            </div>
                        </dd>
                    </dl>
                </div>
            </div>

            // Connect form
            <div class="group">
                <div class="group-header">
                    <span class="group-title"><Icon name="plug" size=16 />"Connect to a network"</span>
                </div>
                <div class="dg-form">
                    <button class="btn btn-sm"
                        disabled=move || scanning.get()
                        on:click=do_scan
                    ><Icon name="search" size=14 />{move || if scanning.get() { "Scanning..." } else { "Scan" }}</button>
                    <select class="input dg-grow" aria-label="Network"
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
                    <input type="password" class="input dg-grow" placeholder="Password" aria-label="Network password"
                        prop:value=move || connect_pass.get()
                        on:input=move |e| connect_pass.set(event_target_value(&e))
                    />
                    <button class="btn btn-sm btn-primary"
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
                <div class="dg-form-foot">
                    <div class=move || {
                        let s = connect_status.get();
                        if s == "No networks found" || s.starts_with("Failed") {
                            "dg-msg tone-text-red"
                        } else if s.starts_with("Found") || s.starts_with("Connected to") || s == "Credentials cleared" {
                            "dg-msg tone-text-green"
                        } else {
                            "dg-msg"
                        }
                    } role="status">
                        {move || connect_status.get()}
                    </div>
                    <button class="btn btn-sm btn-tinted tone-red" on:click=do_forget
                        title="Erase the WiFi credentials stored on the mainboard">
                        <Icon name="trash-2" size=14 />"Clear saved credentials"
                    </button>
                </div>
            </div>

            <div class="group">
                <div class="group-header">
                    <span class="group-title"><Icon name="lock" size=16 />"Access point password"</span>
                    <span class="group-subtitle">"WPA2, 8-63 characters"</span>
                </div>
                <div class="dg-form">
                    <input type="password" class="input dg-grow" placeholder="New AP password (8-63)" aria-label="New access point password"
                        prop:value=move || ap_pass.get()
                        on:input=move |e| ap_pass.set(event_target_value(&e))
                    />
                    <button class="btn btn-sm" on:click=set_ap_password>"Set AP password"</button>
                </div>
                <div class="dg-msg" role="status">{move || ap_status.get()}</div>
            </div>
        </div>
    }
}
