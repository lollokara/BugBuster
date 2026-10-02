use std::collections::BTreeMap;

use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::{Deserialize, Serialize};
use wasm_bindgen::JsValue;

use crate::components::icons::Icon;
use crate::components::ui::{Callout, Switch};
use crate::tauri_bridge::*;

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq)]
#[allow(clippy::upper_case_acronyms)]
pub enum PinMode {
    NC,
    GPIO,
    GPI,
    GPO,
    Analog,
}

impl PinMode {
    pub fn to_str(&self) -> &'static str {
        match self {
            PinMode::NC => "NC",
            PinMode::GPIO => "GPIO",
            PinMode::GPI => "GPI",
            PinMode::GPO => "GPO",
            PinMode::Analog => "ADC",
        }
    }

    #[allow(dead_code)]
    pub fn to_badge(&self) -> &'static str {
        match self {
            PinMode::NC => "·",
            PinMode::GPIO => "GPIO",
            PinMode::GPI => "GPI",
            PinMode::GPO => "GPO",
            PinMode::Analog => "ADC",
        }
    }

    /// True when the pin mode is a digital IO variant (drive-strength applies).
    pub fn is_digital(&self) -> bool {
        matches!(self, PinMode::GPIO | PinMode::GPI | PinMode::GPO)
    }

    /// Map PinMode to the nested MCP schema (type, direction) pair.
    /// Defaults to ("GPIO", "IN") for ambiguous variants.
    pub fn to_type_direction(&self) -> (&'static str, &'static str) {
        match self {
            PinMode::NC => ("NC", "NONE"),
            PinMode::GPIO => ("GPIO", "INOUT"),
            PinMode::GPI => ("GPIO", "IN"),
            PinMode::GPO => ("GPIO", "OUT"),
            PinMode::Analog => ("ANALOG", "INOUT"),
        }
    }
}

/// Drive strength selector for digital IO. `Weak2k` inserts a 2 kΩ series
/// resistor for protected drive into unknown loads.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq)]
pub enum DriveStrength {
    Standard,
    Weak2k,
}

impl DriveStrength {
    pub fn to_str(&self) -> &'static str {
        match self {
            DriveStrength::Standard => "Standard",
            DriveStrength::Weak2k => "Weak (2k)",
        }
    }
    pub fn to_schema(&self) -> &'static str {
        match self {
            DriveStrength::Standard => "standard",
            DriveStrength::Weak2k => "weak_2k",
        }
    }
    pub fn to_u8(&self) -> u8 {
        match self {
            DriveStrength::Standard => 0,
            DriveStrength::Weak2k => 1,
        }
    }
}

// -----------------------------------------------------------------------------
// Nested MCP-schema-aligned export types (Bug 5)
// -----------------------------------------------------------------------------

#[derive(Serialize)]
struct VLockedF32 {
    value: f32,
    locked: bool,
}

#[derive(Serialize)]
struct PinEntry {
    name: String,
    #[serde(rename = "type")]
    ty: String,
    direction: String,
    drive: String,
}

#[derive(Serialize)]
struct EfuseEntry {
    sw_limit_ma: u16,
    enabled: bool,
}

#[derive(Serialize)]
struct EfusesExport {
    vadj1_a: EfuseEntry,
    vadj1_b: EfuseEntry,
    vadj2_a: EfuseEntry,
    vadj2_b: EfuseEntry,
}

#[derive(Serialize)]
struct BoardProfileExport {
    name: String,
    description: String,
    vlogic: VLockedF32,
    vadj1: VLockedF32,
    vadj2: VLockedF32,
    pins: BTreeMap<String, PinEntry>,
    efuses: EfusesExport,
}

impl From<&BoardConfig> for BoardProfileExport {
    fn from(c: &BoardConfig) -> Self {
        let mut pins = BTreeMap::new();
        for i in 0..12 {
            let (ty, dir) = c.pins[i].to_type_direction();
            pins.insert(
                (i + 1).to_string(),
                PinEntry {
                    name: c.pin_names[i].clone(),
                    ty: ty.to_string(),
                    direction: dir.to_string(),
                    drive: c.pin_drive[i].to_schema().to_string(),
                },
            );
        }
        let e = |i: usize| EfuseEntry {
            sw_limit_ma: c.efuses[i].sw_limit_ma,
            enabled: c.efuses[i].sw_limit_enabled,
        };
        BoardProfileExport {
            name: c.name.clone(),
            description: c.description.clone(),
            vlogic: VLockedF32 {
                value: c.vlogic,
                locked: c.vlogic_locked,
            },
            vadj1: VLockedF32 {
                value: c.vadj1,
                locked: c.vadj1_locked,
            },
            vadj2: VLockedF32 {
                value: c.vadj2,
                locked: c.vadj2_locked,
            },
            pins,
            efuses: EfusesExport {
                vadj1_a: e(0),
                vadj1_b: e(1),
                vadj2_a: e(2),
                vadj2_b: e(3),
            },
        }
    }
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq)]
pub struct EfuseConfig {
    pub sw_limit_ma: u16,
    pub sw_limit_enabled: bool,
}

impl Default for EfuseConfig {
    fn default() -> Self {
        Self {
            sw_limit_ma: 500,
            sw_limit_enabled: false,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BoardConfig {
    pub name: String,
    pub description: String,
    pub vlogic: f32,
    pub vlogic_locked: bool,
    pub vadj1: f32,
    pub vadj1_locked: bool,
    pub vadj2: f32,
    pub vadj2_locked: bool,
    pub pins: Vec<PinMode>,     // 12 pins
    pub pin_names: Vec<String>, // 12 names
    #[serde(default = "default_pin_drive")]
    pub pin_drive: Vec<DriveStrength>, // 12 drive strengths (digital only)
    #[serde(default = "default_efuses")]
    pub efuses: [EfuseConfig; 4], // index 0..3 = VADJ1-A, VADJ1-B, VADJ2-A, VADJ2-B
}

fn default_pin_drive() -> Vec<DriveStrength> {
    vec![DriveStrength::Standard; 12]
}

fn default_efuses() -> [EfuseConfig; 4] {
    [EfuseConfig::default(); 4]
}

impl Default for BoardConfig {
    fn default() -> Self {
        Self {
            name: "New Board".to_string(),
            description: "Custom DUT profile".to_string(),
            vlogic: 3.3,
            vlogic_locked: true,
            vadj1: 3.3,
            vadj1_locked: false,
            vadj2: 5.0,
            vadj2_locked: true,
            pins: vec![PinMode::NC; 12],
            pin_names: (1..=12).map(|i| format!("Port {}", i)).collect(),
            pin_drive: default_pin_drive(),
            efuses: default_efuses(),
        }
    }
}

// -----------------------------------------------------------------------------
// Board map helpers
// -----------------------------------------------------------------------------

/// Pins 1-6 sit on VADJ1, 7-12 on VADJ2.
fn pin_domain(i: usize) -> (&'static str, &'static str) {
    if i < 6 {
        ("VADJ1", "vadj1")
    } else {
        ("VADJ2", "vadj2")
    }
}

/// Each VADJ has two e-fuses covering three pins each:
/// IO1-3 -> 0 (VADJ1-A), IO4-6 -> 1 (VADJ1-B), IO7-9 -> 2 (VADJ2-A), IO10-12 -> 3 (VADJ2-B).
fn pin_efuse(i: usize) -> usize {
    i / 3
}

fn efuse_label(e: usize) -> &'static str {
    match e {
        0 => "VADJ1-A",
        1 => "VADJ1-B",
        2 => "VADJ2-A",
        _ => "VADJ2-B",
    }
}

/// Only IO3/IO6/IO9/IO12 are wired to the AD74416H analog channels A-D.
fn is_analog_capable(i: usize) -> bool {
    matches!(i, 2 | 5 | 8 | 11)
}

fn io_range(e: usize) -> String {
    format!("IO{}\u{2013}{}", e * 3 + 1, e * 3 + 3)
}

/// (function, value, unit) read back from the AD74416H channel behind an analog-capable IO.
/// Digital IOs have no live readback in the device state.
fn live_io(ds: &DeviceState, i: usize) -> Option<(&'static str, String, &'static str)> {
    if !is_analog_capable(i) {
        return None;
    }
    let ch = ds.channels.get(i / 3)?;
    Some(match ch.function {
        0 => ("High impedance", "Hi-Z".to_string(), ""),
        1 => ("Voltage out", format!("{:.3}", ch.dac_value), "V"),
        2 | 10 => ("Current out", format!("{:.3}", ch.dac_value), "mA"),
        3 => ("Voltage in", format!("{:.4}", ch.adc_value), "V"),
        4 | 11 => ("Current in", format!("{:.3}", ch.adc_value), "mA"),
        5 | 12 => ("Current in (loop)", format!("{:.3}", ch.adc_value), "mA"),
        7 => ("Resistance", format!("{:.1}", ch.adc_value), "\u{3a9}"),
        8 | 9 => (
            "Digital in",
            if ch.din_state { "High" } else { "Low" }.to_string(),
            "",
        ),
        _ => ("Unknown", "\u{2014}".to_string(), ""),
    })
}

#[derive(Clone, Debug, Default, Deserialize)]
struct OwnerRow {
    #[serde(default)]
    kind: u8,
}

/// Owner of an IO: its own slot, or the analog channel slot (12..15) behind it.
fn owner_name(owners: RwSignal<Vec<OwnerRow>>, i: usize) -> Option<&'static str> {
    let kind = owners.with(|o| {
        let direct = o.get(i).map(|s| s.kind).unwrap_or(0);
        if direct != 0 || !is_analog_capable(i) {
            direct
        } else {
            o.get(12 + i / 3).map(|s| s.kind).unwrap_or(0)
        }
    });
    match kind {
        0 => None,
        1 => Some("USB"),
        2 => Some("HTTP"),
        3 => Some("Script"),
        4 => Some("CLI"),
        5 => Some("Internal"),
        _ => Some("Other"),
    }
}

/// Push e-fuse config. The firmware does not enforce a SW limit, so only the
/// enable toggle surfaces that to the user.
fn push_efuse(efuse: u8, sw_limit_ma: u16, enabled: bool) {
    #[derive(Serialize)]
    struct Args {
        efuse: u8,
        sw_limit_ma: u16,
        enabled: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        efuse,
        sw_limit_ma,
        enabled,
    })
    .unwrap();
    spawn_local(async move {
        if try_invoke("set_efuse_config", args).await.is_none() && enabled {
            show_toast(
                "SW current limit is not enforced by the firmware (saved to profile only)",
                "err",
            );
        }
    });
}

/// Push drive strength to the backend (stub command until firmware wiring lands).
fn push_drive(pin: u8, drive: DriveStrength) {
    #[derive(Serialize)]
    struct Args {
        pin: u8,
        drive: u8,
    }
    let payload = Args {
        pin,
        drive: drive.to_u8(),
    };
    let args = serde_wasm_bindgen::to_value(&payload).unwrap();
    spawn_local(async move {
        let _ = try_invoke("set_pin_drive_strength", args).await;
    });
}

// -----------------------------------------------------------------------------
// Components
// -----------------------------------------------------------------------------

/// One supply rail of the board profile: voltage input plus AI lockout switch.
#[component]
fn RailCell(
    label: &'static str,
    cls: &'static str,
    lock_label: &'static str,
    dflt: f32,
    config: ReadSignal<BoardConfig>,
    set_config: WriteSignal<BoardConfig>,
    get_v: fn(&BoardConfig) -> f32,
    set_v: fn(&mut BoardConfig, f32),
    get_lock: fn(&BoardConfig) -> bool,
    set_lock: fn(&mut BoardConfig, bool),
) -> impl IntoView {
    view! {
        <div class=format!("bm-rail {cls}")>
            <div class="bm-rail-top">
                <span class="bm-rail-dot" aria-hidden="true"></span>
                <span class="bm-rail-name">{label}</span>
                <span class="bm-rail-sub">"Profile setpoint"</span>
            </div>
            <div class="bm-rail-ctl">
                <input type="number" step="0.1" class="bm-rail-input"
                    aria-label=format!("{label} voltage (V)")
                    prop:value=move || format!("{:.1}", config.with(get_v))
                    on:change=move |e| {
                        let v = event_target_value(&e).parse().unwrap_or(dflt);
                        set_config.update(|c| set_v(c, v));
                    } />
                <span class="bm-unit">"V"</span>
                <span class="spacer"></span>
                <label class="bm-lock"
                    title="When locked, AI assistants cannot modify this supply voltage.">
                    <Switch
                        checked=Signal::derive(move || config.with(get_lock))
                        aria_label=lock_label
                        on_change=Callback::new(move |v: bool| set_config.update(|c| set_lock(c, v)))
                    />
                    <span>"AI lockout"</span>
                </label>
            </div>
        </div>
    }
}

/// A pad on the board diagram: name, mode, owner and live value of one IO.
#[component]
fn PinTile(
    i: usize,
    config: ReadSignal<BoardConfig>,
    selected: RwSignal<usize>,
    state: ReadSignal<DeviceState>,
    owners: RwSignal<Vec<OwnerRow>>,
) -> impl IntoView {
    let analog = is_analog_capable(i);
    let mode = move || config.with(|c| c.pins[i]);
    let live = move || state.with(|ds| live_io(ds, i));
    view! {
        <button type="button" class="bm-pin"
            class:sel=move || selected.get() == i
            aria-pressed=move || if selected.get() == i { "true" } else { "false" }
            aria-label=move || format!("IO{} {}", i + 1, config.with(|c| c.pin_names[i].clone()))
            on:click=move |_| selected.set(i)
        >
            <span class="bm-pad" aria-hidden="true">{i + 1}</span>
            <span class="bm-pin-id">
                <span class="bm-pin-name">{move || config.with(|c| c.pin_names[i].clone())}</span>
                <span class="bm-pin-sub">
                    {format!("IO{:02}", i + 1)}
                    {analog.then(|| view! { <span class="bm-cap" title="Analog capable (AD74416H channel)">"ADC"</span> })}
                    {move || owner_name(owners, i).map(|k| view! {
                        <span class="bm-own" title=format!("Owned by {k}")>
                            <Icon name="lock" size=11 />{k}
                        </span>
                    })}
                </span>
            </span>
            <span class="bm-pin-mode"
                class:set=move || mode() != PinMode::NC
                class:weak=move || config.with(|c| c.pin_drive[i] == DriveStrength::Weak2k)
                title=move || if config.with(|c| c.pin_drive[i] == DriveStrength::Weak2k) { "Weak drive (2k series)" } else { "" }
            >{move || mode().to_str()}</span>
            <span class="bm-pin-val">
                {move || match live() {
                    Some((_, v, u)) => view! { {v}<span class="bm-unit">{u}</span> }.into_any(),
                    None => view! { <span class="bm-dash">"\u{2014}"</span> }.into_any(),
                }}
            </span>
        </button>
    }
}

/// Selection inspector: live state, profile configuration and properties of one IO.
#[component]
fn PinInspector(
    config: ReadSignal<BoardConfig>,
    set_config: WriteSignal<BoardConfig>,
    selected: RwSignal<usize>,
    state: ReadSignal<DeviceState>,
    owners: RwSignal<Vec<OwnerRow>>,
) -> impl IntoView {
    let mode = move || config.with(|c| c.pins[selected.get()]);
    let drive = move || config.with(|c| c.pin_drive[selected.get()]);
    let analog = move || is_analog_capable(selected.get());
    let digital = move || mode().is_digital();
    let dom_v = move || {
        let i = selected.get();
        config.with(|c| if i < 6 { c.vadj1 } else { c.vadj2 })
    };
    let live = move || state.with(|ds| live_io(ds, selected.get()));

    view! {
        <div class="bm-insp-head">
            <span class="bm-insp-io">{move || format!("IO{:02}", selected.get() + 1)}</span>
            <span class="badge"
                class:tone-blue={move || selected.get() < 6}
                class:tone-purple={move || selected.get() >= 6}
            >{move || pin_domain(selected.get()).0}</span>
            <span class="spacer"></span>
            <span class="bm-insp-v">{move || format!("{:.1}", dom_v())}<span class="bm-unit">"V"</span></span>
        </div>

        <section class="bm-insp-sec">
            <h4>"Live"</h4>
            <div class="bm-live">
                <span class="bm-live-val">
                    {move || live().map(|l| l.1).unwrap_or_else(|| "\u{2014}".to_string())}
                </span>
                <span class="bm-live-unit">{move || live().map(|l| l.2).unwrap_or("")}</span>
            </div>
            <dl class="kv">
                <dt>"Function"</dt>
                <dd>{move || live().map(|l| l.0).unwrap_or("Digital IO")}</dd>
                <dt>"Channel"</dt>
                <dd>{move || if analog() { format!("CH {}", CH_NAMES[selected.get() / 3]) } else { "\u{2014}".to_string() }}</dd>
                <dt>"Owner"</dt>
                <dd>
                    {move || {
                        let known = owners.with(|o| !o.is_empty());
                        match owner_name(owners, selected.get()) {
                            Some(k) => view! { <span class="bm-owner"><span class="dot tone-orange"></span>{k}</span> }.into_any(),
                            None if known => view! { <span class="bm-owner"><span class="dot"></span>"Free"</span> }.into_any(),
                            None => view! { <span class="subtle">"Unknown"</span> }.into_any(),
                        }
                    }}
                </dd>
            </dl>
            <Show when=move || !analog()>
                <p class="bm-note">"Digital IO levels are not reported in the device state. Use the digital IO tabs for a live level."</p>
            </Show>
        </section>

        <section class="bm-insp-sec">
            <h4>"Profile"</h4>
            <div class="row">
                <label class="row-label" for="bm-pin-name">"Name"</label>
                <input id="bm-pin-name" type="text" class="bm-field"
                    prop:value=move || config.with(|c| c.pin_names[selected.get()].clone())
                    on:input=move |e| {
                        let i = selected.get_untracked();
                        set_config.update(|c| c.pin_names[i] = event_target_value(&e));
                    } />
            </div>
            <div class="row">
                <label class="row-label" for="bm-pin-mode">"Mode"</label>
                <select id="bm-pin-mode" class="dropdown bm-field"
                    on:change=move |e| {
                        let new_mode = match event_target_value(&e).as_str() {
                            "GPIO" => PinMode::GPIO,
                            "GPI" => PinMode::GPI,
                            "GPO" => PinMode::GPO,
                            "Analog" => PinMode::Analog,
                            _ => PinMode::NC,
                        };
                        let i = selected.get_untracked();
                        set_config.update(|c| c.pins[i] = new_mode);
                    }
                >
                    <option value="NC" prop:selected=move || mode() == PinMode::NC>"NC \u{2014} Not connected"</option>
                    <option value="GPIO" prop:selected=move || mode() == PinMode::GPIO>"GPIO \u{2014} Digital bidir"</option>
                    <option value="GPI" prop:selected=move || mode() == PinMode::GPI>"GPI \u{2014} Digital input"</option>
                    <option value="GPO" prop:selected=move || mode() == PinMode::GPO>"GPO \u{2014} Digital output"</option>
                    {move || analog().then(|| view! {
                        <option value="Analog" prop:selected=move || mode() == PinMode::Analog>"ADC \u{2014} Analog input"</option>
                    })}
                </select>
            </div>
            <div class="row" class:bm-row-off=move || !digital()>
                <label class="row-label" for="bm-pin-drive"
                    title="2k series resistor limits peak current for protected driving into unknown loads">"Drive"</label>
                <select id="bm-pin-drive" class="dropdown bm-field"
                    prop:disabled=move || !digital()
                    title=move || if digital() { "" } else { "Drive strength applies to digital modes only" }
                    on:change=move |e| {
                        let d = match event_target_value(&e).as_str() {
                            "weak_2k" => DriveStrength::Weak2k,
                            _ => DriveStrength::Standard,
                        };
                        let i = selected.get_untracked();
                        set_config.update(|c| c.pin_drive[i] = d);
                        push_drive(i as u8, d);
                    }
                >
                    <option value="standard" prop:selected=move || drive() == DriveStrength::Standard>"Standard"</option>
                    <option value="weak_2k" prop:selected=move || drive() == DriveStrength::Weak2k>"Weak (2k series)"</option>
                </select>
            </div>
        </section>

        <section class="bm-insp-sec">
            <h4>"Properties"</h4>
            <dl class="kv">
                <dt>"Power domain"</dt>
                <dd>{move || pin_domain(selected.get()).0}</dd>
                <dt>"Rail voltage"</dt>
                <dd>{move || format!("{:.1} V", dom_v())}</dd>
                <dt>"eFuse"</dt>
                <dd>{move || efuse_label(pin_efuse(selected.get()))}</dd>
                <dt>"Analog capable"</dt>
                <dd>{move || if analog() { "Yes" } else { "No" }}</dd>
                <dt>"Drive limit"</dt>
                <dd>{move || if digital() { drive().to_str() } else { "\u{2014}" }}</dd>
            </dl>
        </section>

        <Show when=move || matches!(mode(), PinMode::Analog) && !analog()>
            <div class="bm-insp-warn">
                <Callout tone="orange">"Analog function not available on this pin. Only IO3/IO6/IO9/IO12 support analog."</Callout>
            </div>
        </Show>
    }
}

/// One e-fuse block: live current, software limit and enable.
#[component]
fn FuseCard(
    e: usize,
    config: ReadSignal<BoardConfig>,
    set_config: WriteSignal<BoardConfig>,
    imon: RwSignal<EfuseImonStatus>,
) -> impl IntoView {
    let dom = if e < 2 { "bm-d-vadj1" } else { "bm-d-vadj2" };
    let monitored = move || imon.with(|s| s.efuse as usize == e + 1);
    view! {
        <div class=format!("bm-fuse group {dom}")>
            <div class="bm-fuse-head">
                <span class="bm-fuse-dot" aria-hidden="true"></span>
                <span class="bm-fuse-name">{format!("eFuse {}", efuse_label(e))}</span>
                <span class="subtle bm-fuse-io">{io_range(e)}</span>
            </div>
            <div class="bm-fuse-cur"
                class:tone-text-red=move || imon.with(|s| s.efuse as usize == e + 1 && s.valid && s.saturated)
                title="Only the e-fuse selected in the current monitor reports a live current."
            >
                {move || if monitored() {
                    let (text, _) = imon.with(|s| efuse_imon_display(s));
                    text
                } else {
                    "\u{2014}".to_string()
                }}
            </div>
            <div class="row">
                <label class="row-label" for=format!("bm-fuse-lim-{e}")
                    title="Software current limit (100 - 1200 mA). Saved to the board profile; not enforced by the firmware.">"SW limit"</label>
                <span class="bm-fuse-in">
                    <input id=format!("bm-fuse-lim-{e}") type="number" class="bm-num"
                        min="100" max="1200" step="50"
                        prop:value=move || config.with(|c| c.efuses[e].sw_limit_ma.to_string())
                        on:input=move |ev| {
                            let v: u16 = event_target_value(&ev).parse().unwrap_or(500);
                            let v = v.clamp(100, 1200);
                            set_config.update(|c| c.efuses[e].sw_limit_ma = v);
                            let en = config.get_untracked().efuses[e].sw_limit_enabled;
                            push_efuse(e as u8, v, en);
                        } />
                    <span class="bm-unit">"mA"</span>
                </span>
            </div>
            <div class="row">
                <span class="row-label">"Enable SW limit"</span>
                <Switch
                    checked=Signal::derive(move || config.with(|c| c.efuses[e].sw_limit_enabled))
                    aria_label="Enable software current limit"
                    on_change=Callback::new(move |checked: bool| {
                        set_config.update(|c| c.efuses[e].sw_limit_enabled = checked);
                        let lim = config.get_untracked().efuses[e].sw_limit_ma;
                        push_efuse(e as u8, lim, checked);
                    })
                />
            </div>
        </div>
    }
}

#[component]
pub fn BoardTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (config, set_config) = signal(BoardConfig::default());
    // Currently-selected pin for the inspector (defaults to IO 1).
    let selected_pin = RwSignal::new(0usize);
    let imon = RwSignal::new(EfuseImonStatus::default());
    start_efuse_imon_poll(imon);

    let owners = RwSignal::new(Vec::<OwnerRow>::new());
    start_tab_poll(
        move || async move {
            if let Some(rows) = try_invoke("io_owner_status", JsValue::NULL)
                .await
                .and_then(|r| serde_wasm_bindgen::from_value::<Vec<OwnerRow>>(r).ok())
            {
                owners.try_set(rows);
            }
        },
        || 3000,
    );

    let hat = RwSignal::new(HatStatus::default());
    start_tab_poll(
        move || async move {
            if let Some(h) = fetch_hat_status().await {
                hat.try_set(h);
            }
        },
        move || {
            if hat.try_get_untracked().is_some_and(|h| h.detected) {
                HAT_POLL_MS
            } else {
                HAT_ABSENT_POLL_MS
            }
        },
    );

    let export_json = move |_| {
        let cfg = config.get();
        let export: BoardProfileExport = (&cfg).into();
        let json = serde_json::to_string_pretty(&export).unwrap();
        let default_name = cfg.name.clone();
        #[derive(Serialize)]
        struct PickArgs {
            #[serde(rename = "defaultName")]
            default_name: String,
        }
        #[derive(Serialize)]
        struct SaveArgs {
            #[serde(rename = "profileJson")]
            profile_json: String,
            #[serde(rename = "targetPath")]
            target_path: Option<String>,
        }
        spawn_local(async move {
            let pick_args = serde_wasm_bindgen::to_value(&PickArgs { default_name }).unwrap();
            let picked = try_invoke("pick_profile_save_path", pick_args).await;
            let target_path: Option<String> =
                picked.and_then(|r| serde_wasm_bindgen::from_value(r).ok().flatten());
            if target_path.is_none() {
                return;
            }

            let args = serde_wasm_bindgen::to_value(&SaveArgs {
                profile_json: json,
                target_path,
            })
            .unwrap();
            match try_invoke("save_board_profile", args).await {
                Some(val) => {
                    let path = val.as_string().unwrap_or_default();
                    if path.is_empty() {
                        show_toast("Board profile exported", "ok");
                    } else {
                        show_toast(&format!("Exported to {}", path), "ok");
                    }
                }
                None => {
                    show_toast("Board profile export failed (see log)", "err");
                }
            }
            let _ = JsValue::NULL;
        });
    };

    let reset_defaults = move |_| {
        set_config.set(BoardConfig::default());
        selected_pin.set(0);
        show_toast("Board profile reset", "ok");
    };

    let hat_line = move || {
        let h = hat.get();
        if !h.detected {
            ("tone-gray", "No HAT", "No HAT fitted. The HAT connectors are empty.")
        } else if h.hat_type == 0x10 {
            ("tone-green", "DAQ HAT", "DUT supply and power analyzer on the HAT connector.")
        } else {
            ("tone-green", "LA HAT", "4-channel logic analyzer and SWD probe on the HAT connectors.")
        }
    };

    view! {
        <div class="view bm">
            // ============ HEADER: identity, HAT, export ============
            <div class="bm-head">
                <span class="chip bm-badge">"BUGBUSTER_S3_V4"</span>
                <input class="bm-name" type="text" aria-label="Board name"
                    prop:value=move || config.with(|c| c.name.clone())
                    on:input=move |e| set_config.update(|c| c.name = event_target_value(&e))
                />
                <input class="bm-desc" type="text" placeholder="Description\u{2026}" aria-label="Board description"
                    prop:value=move || config.with(|c| c.description.clone())
                    on:input=move |e| set_config.update(|c| c.description = event_target_value(&e))
                />
                <span class="spacer"></span>
                <span class="badge" class:tone-green=move || hat_line().0 == "tone-green">
                    <span class=move || format!("dot {}", hat_line().0)></span>
                    {move || hat_line().1}
                </span>
                <button class="btn btn-sm" title="Reset all pins to default" on:click=reset_defaults>
                    <Icon name="rotate-ccw" size=14 />"Reset"
                </button>
                <button class="btn btn-sm btn-primary" on:click=export_json>
                    <Icon name="file-down" size=14 />"Export\u{2026}"
                </button>
            </div>

            // ============ POWER DOMAINS ============
            <div class="bm-rails">
                <RailCell label="VLOGIC" cls="bm-r-logic" lock_label="AI lockout VLOGIC" dflt=3.3
                    config=config set_config=set_config
                    get_v=|c| c.vlogic set_v=|c, v| c.vlogic = v
                    get_lock=|c| c.vlogic_locked set_lock=|c, v| c.vlogic_locked = v />
                <RailCell label="VADJ1" cls="bm-r-vadj1" lock_label="AI lockout VADJ1" dflt=3.3
                    config=config set_config=set_config
                    get_v=|c| c.vadj1 set_v=|c, v| c.vadj1 = v
                    get_lock=|c| c.vadj1_locked set_lock=|c, v| c.vadj1_locked = v />
                <RailCell label="VADJ2" cls="bm-r-vadj2" lock_label="AI lockout VADJ2" dflt=5.0
                    config=config set_config=set_config
                    get_v=|c| c.vadj2 set_v=|c, v| c.vadj2 = v
                    get_lock=|c| c.vadj2_locked set_lock=|c, v| c.vadj2_locked = v />
            </div>

            // ============ BOARD DIAGRAM + INSPECTOR ============
            <div class="bm-main">
                <section class="bm-board group" aria-label="Board map">
                    <div class="group-header">
                        <div class="group-title"><Icon name="circuit-board" size=16 />"Board map"</div>
                        <span class="group-subtitle">"Select an IO to inspect and configure it"</span>
                    </div>
                    <div class="bm-pcb">
                        {(0..4).map(|e| {
                            let dom = if e < 2 { "bm-d-vadj1" } else { "bm-d-vadj2" };
                            let rail = if e < 2 { "VADJ1" } else { "VADJ2" };
                            view! {
                                <div class=format!("bm-block {dom}")>
                                    <div class="bm-block-head">
                                        <span class="bm-block-rail">{rail}</span>
                                        <span class="bm-block-title">{format!("eFuse {}", efuse_label(e))}</span>
                                        <span class="spacer"></span>
                                        <span class="bm-block-io">{io_range(e)}</span>
                                    </div>
                                    <div class="bm-pins">
                                        {(e * 3..e * 3 + 3).map(|i| view! {
                                            <PinTile i=i config=config selected=selected_pin state=state owners=owners />
                                        }).collect::<Vec<_>>()}
                                    </div>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                    <div class="bm-hatbar">
                        <Icon name="cpu" size=14 />
                        <span class="bm-hat-name">{move || hat_line().1}</span>
                        <span class="subtle">{move || hat_line().2}</span>
                    </div>
                </section>

                <aside class="bm-insp group group-flush" aria-label="IO inspector">
                    <PinInspector config=config set_config=set_config selected=selected_pin state=state owners=owners />
                </aside>
            </div>

            // ============ EFUSE PANEL ============
            <div class="section-label">"eFuse current limits"</div>
            <Callout tone="blue">
                "The software current limit is saved to the board profile only. The firmware does not enforce it; each e-fuse trips on its hardware limit."
            </Callout>
            <div class="bm-fuses">
                {(0..4).map(|e| view! {
                    <FuseCard e=e config=config set_config=set_config imon=imon />
                }).collect::<Vec<_>>()}
            </div>

            // ============ STATUS STRIP ============
            <div class="bm-status">
                {move || {
                    let configured = config.with(|c| c.pins.iter().filter(|m| !matches!(m, PinMode::NC)).count());
                    let unset = 12 - configured;
                    view! {
                        <span class="chip">"Configured "<b>{configured}</b></span>
                        <span class="chip">"Unset "<b>{unset}</b></span>
                        <span class="chip">"Total "<b>"12"</b></span>
                    }
                }}
                <span class="spacer"></span>
                <span class="bm-hint">"Select an IO, then use the inspector to configure mode, name and drive."</span>
            </div>
        </div>
    }
}
