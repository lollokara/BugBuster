use std::collections::BTreeMap;

use leptos::either::Either;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::{Deserialize, Serialize};
use wasm_bindgen::JsCast;
use wasm_bindgen::JsValue;

use crate::tauri_bridge::*;

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq)]
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
            PinMode::Analog => "Analog",
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
            vlogic: VLockedF32 { value: c.vlogic, locked: c.vlogic_locked },
            vadj1: VLockedF32 { value: c.vadj1, locked: c.vadj1_locked },
            vadj2: VLockedF32 { value: c.vadj2, locked: c.vadj2_locked },
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
        Self { sw_limit_ma: 500, sw_limit_enabled: false }
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
    pub pins: Vec<PinMode>, // 12 pins
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

#[component]
pub fn BoardTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let _ = state; // reserved for future live-pin state readouts
    let (config, set_config) = signal(BoardConfig::default());
    // Currently-selected pin for the inspector (defaults to IO 1).
    let selected_pin = RwSignal::new(0usize);

    let export_json = move |_| {
        let cfg = config.get();
        let export: BoardProfileExport = (&cfg).into();
        let json = serde_json::to_string_pretty(&export).unwrap();
        let default_name = cfg.name.clone();
        #[derive(Serialize)]
        struct PickArgs { #[serde(rename = "defaultName")] default_name: String }
        #[derive(Serialize)]
        struct SaveArgs {
            #[serde(rename = "profileJson")] profile_json: String,
            #[serde(rename = "targetPath")] target_path: Option<String>,
        }
        spawn_local(async move {
            let pick_args = serde_wasm_bindgen::to_value(&PickArgs { default_name }).unwrap();
            let picked = invoke("pick_profile_save_path", pick_args).await;
            let target_path: Option<String> = serde_wasm_bindgen::from_value(picked).ok().flatten();
            if target_path.is_none() { return; }

            let args = serde_wasm_bindgen::to_value(&SaveArgs {
                profile_json: json,
                target_path,
            }).unwrap();
            match try_invoke("save_board_profile", args).await {
                Some(val) => {
                    let path = val.as_string().unwrap_or_default();
                    if path.is_empty() { show_toast("Board profile exported", "ok"); }
                    else { show_toast(&format!("Exported to {}", path), "ok"); }
                }
                None => { show_toast("Board profile export failed (see log)", "err"); }
            }
            let _ = JsValue::NULL;
        });
    };

    let reset_defaults = move |_| {
        set_config.set(BoardConfig::default());
        selected_pin.set(0);
        show_toast("Board profile reset", "ok");
    };

    // Pin→power-domain helper. Pins 1-6 → VADJ1, 7-12 → VADJ2.
    let pin_domain = |i: usize| -> (&'static str, &'static str) {
        if i < 6 { ("VADJ1", "vadj1") } else { ("VADJ2", "vadj2") }
    };
    // Pin→efuse-block index. Each VADJ has two efuses covering 3 pins each:
    // IO1-3 → efuse 0 (VADJ1-A), IO4-6 → efuse 1 (VADJ1-B),
    // IO7-9 → efuse 2 (VADJ2-A), IO10-12 → efuse 3 (VADJ2-B).
    let pin_efuse = |i: usize| -> usize { i / 3 };
    let efuse_label = |e: usize| -> &'static str {
        match e { 0 => "VADJ1-A", 1 => "VADJ1-B", 2 => "VADJ2-A", _ => "VADJ2-B" }
    };
    let efuse_domain_cls = |e: usize| -> &'static str {
        if e < 2 { "vadj1" } else { "vadj2" }
    };
    let is_analog_capable = |i: usize| matches!(i, 0 | 3 | 6 | 9);

    // Push efuse config to backend stub.
    let push_efuse = move |efuse: u8, sw_limit_ma: u16, enabled: bool| {
        #[derive(Serialize)]
        struct Args { efuse: u8, sw_limit_ma: u16, enabled: bool }
        let args = serde_wasm_bindgen::to_value(&Args { efuse, sw_limit_ma, enabled }).unwrap();
        spawn_local(async move {
            let _ = invoke("set_efuse_config", args).await;
        });
    };

    // Push drive-strength to backend (stub command — logs and returns Ok on
    // the Rust side until firmware wiring lands).
    let push_drive = move |pin: u8, drive: DriveStrength| {
        #[derive(Serialize)]
        struct Args { pin: u8, drive: u8 }
        let payload = Args { pin, drive: drive.to_u8() };
        let args = serde_wasm_bindgen::to_value(&payload).unwrap();
        spawn_local(async move {
            let _ = invoke("set_pin_drive_strength", args).await;
        });
    };

    view! {
        <div class="tab-content board-tab-pro">
            // ============ TOP BAR: name / desc / power readouts / export ============
            <div class="board-topbar">
                <div class="board-id">
                    <div class="board-id-badge">"BUGBUSTER_S3_V4"</div>
                    <input class="board-name-input" type="text"
                        prop:value=move || config.get().name
                        on:input=move |e| set_config.update(|c| c.name = event_target_value(&e))
                    />
                    <input class="board-desc-input" type="text" placeholder="Description…"
                        prop:value=move || config.get().description
                        on:input=move |e| set_config.update(|c| c.description = event_target_value(&e))
                    />
                </div>
                <div class="board-power-row">
                    <div class="board-rail">
                        <span class="board-rail-label">"VLOGIC"</span>
                        <input type="number" step="0.1" class="board-rail-input"
                            prop:value=move || format!("{:.1}", config.get().vlogic)
                            on:input=move |e| set_config.update(|c| c.vlogic = event_target_value(&e).parse().unwrap_or(3.3)) />
                        <span class="board-rail-unit">"V"</span>
                        <label class="board-ai-lockout"
                            title="When locked, AI assistants cannot modify this supply voltage.">
                            <input type="checkbox"
                                prop:checked=move || config.get().vlogic_locked
                                on:change=move |e| {
                                    let checked: bool = e.target().unwrap().unchecked_into::<web_sys::HtmlInputElement>().checked();
                                    set_config.update(|c| c.vlogic_locked = checked);
                                }
                            />
                            <span>"AI Lockout"</span>
                        </label>
                    </div>
                    <div class="board-rail board-rail-vadj1">
                        <span class="board-rail-label">"VADJ1"</span>
                        <input type="number" step="0.1" class="board-rail-input"
                            prop:value=move || format!("{:.1}", config.get().vadj1)
                            on:input=move |e| set_config.update(|c| c.vadj1 = event_target_value(&e).parse().unwrap_or(3.3)) />
                        <span class="board-rail-unit">"V"</span>
                        <label class="board-ai-lockout"
                            title="When locked, AI assistants cannot modify this supply voltage.">
                            <input type="checkbox"
                                prop:checked=move || config.get().vadj1_locked
                                on:change=move |e| {
                                    let checked: bool = e.target().unwrap().unchecked_into::<web_sys::HtmlInputElement>().checked();
                                    set_config.update(|c| c.vadj1_locked = checked);
                                }
                            />
                            <span>"AI Lockout"</span>
                        </label>
                    </div>
                    <div class="board-rail board-rail-vadj2">
                        <span class="board-rail-label">"VADJ2"</span>
                        <input type="number" step="0.1" class="board-rail-input"
                            prop:value=move || format!("{:.1}", config.get().vadj2)
                            on:input=move |e| set_config.update(|c| c.vadj2 = event_target_value(&e).parse().unwrap_or(5.0)) />
                        <span class="board-rail-unit">"V"</span>
                        <label class="board-ai-lockout"
                            title="When locked, AI assistants cannot modify this supply voltage.">
                            <input type="checkbox"
                                prop:checked=move || config.get().vadj2_locked
                                on:change=move |e| {
                                    let checked: bool = e.target().unwrap().unchecked_into::<web_sys::HtmlInputElement>().checked();
                                    set_config.update(|c| c.vadj2_locked = checked);
                                }
                            />
                            <span>"AI Lockout"</span>
                        </label>
                    </div>
                </div>
                <div class="board-actions">
                    <button class="scope-btn" on:click=export_json>"Export…"</button>
                    <button class="scope-btn" on:click=reset_defaults title="Reset all pins to default">"Reset"</button>
                </div>
            </div>

            // ============ MAIN LAYOUT: pin map + inspector ============
            <div class="board-main">
                // ---- LEFT: pin map (12 tiles, 2 rows of 6) ----
                <div class="board-pinmap card">
                    <div class="board-pinmap-head">
                        <span class="channel-label">"Pin Map"</span>
                        <span class="board-pinmap-sub">"Click a pin to configure"</span>
                    </div>
                    <div class="board-pinmap-domains">
                        {(0..4).map(|row| {
                            let (dom_cls_str, dom_label) = if row < 2 {
                                ("board-domain-vadj1", "VADJ1")
                            } else {
                                ("board-domain-vadj2", "VADJ2")
                            };
                            let sub = efuse_label(row);
                            let pin_range_start = row * 3;
                            let pin_range_end = pin_range_start + 3;
                            view! {
                                <div class=format!("board-domain-row {}", dom_cls_str)>
                                    <div class="board-domain-tag">
                                        <span>{dom_label}</span>
                                        <span class="board-domain-sub">{sub}</span>
                                        <span class="board-domain-sub-io">
                                            {format!("IO{}-{}", pin_range_start + 1, pin_range_end)}
                                        </span>
                                    </div>
                                    <div class="board-pin-row">
                                        {(pin_range_start..pin_range_end).map(|i| {
                                            let cfg = config.get();
                                            let mode = cfg.pins[i];
                                            let name = cfg.pin_names[i].clone();
                                            let analog_cap = is_analog_capable(i);
                                            let sel = selected_pin.get() == i;
                                            let tile_cls = format!("board-pin-tile {}", dom_cls_str);
                                            view! {
                                                <button class=tile_cls
                                                    class:board-pin-selected=move || selected_pin.get() == i
                                                    on:click=move |_| selected_pin.set(i)
                                                >
                                                    <span class="board-pin-num">{format!("IO{:02}", i + 1)}</span>
                                                    <span class="board-pin-icon">{pin_icon(mode)}</span>
                                                    <span class="board-pin-name">{name}</span>
                                                    <span class="board-pin-mode">{mode.to_str()}</span>
                                                    <span class="board-pin-dot" class:board-dot-set=move || matches!(config.get().pins[i], PinMode::NC) == false></span>
                                                    {if analog_cap { Either::Left(view! { <span class="board-pin-cap" title="Analog capable">"~"</span> }) } else { Either::Right(()) }}
                                                    {if sel { Either::Left(view! { <span class="board-pin-arrow"></span> }) } else { Either::Right(()) }}
                                                </button>
                                            }
                                        }).collect::<Vec<_>>()}
                                    </div>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                // ---- RIGHT: inspector ----
                <div class="board-inspector card">
                    {move || {
                        let i = selected_pin.get();
                        let cfg = config.get();
                        let mode = cfg.pins[i];
                        let drive = cfg.pin_drive[i];
                        let pin_name = cfg.pin_names[i].clone();
                        let (dom_label, dom_cls) = pin_domain(i);
                        let analog_cap = is_analog_capable(i);
                        let dom_v = if dom_label == "VADJ1" { cfg.vadj1 } else { cfg.vadj2 };
                        let digital = mode.is_digital();
                        let warn = matches!(mode, PinMode::Analog) && !analog_cap;
                        view! {
                            <div class="board-inspector-head">
                                <span class="board-inspector-io">{format!("IO{:02}", i + 1)}</span>
                                <span class=format!("board-inspector-domain board-{}", dom_cls)>{dom_label}</span>
                                <span class="board-inspector-voltage">{format!("{:.1} V", dom_v)}</span>
                            </div>
                            <div class="board-inspector-row">
                                <label>"Name"</label>
                                <input type="text" class="dropdown"
                                    prop:value=pin_name.clone()
                                    on:input=move |e| set_config.update(|c| c.pin_names[i] = event_target_value(&e)) />
                            </div>
                            <div class="board-inspector-row">
                                <label>"Mode"</label>
                                <select class="dropdown"
                                    prop:value=mode.to_str().to_string()
                                    on:change=move |e| {
                                        let v = event_target_value(&e);
                                        let new_mode = match v.as_str() {
                                            "GPIO" => PinMode::GPIO, "GPI" => PinMode::GPI, "GPO" => PinMode::GPO,
                                            "Analog" => PinMode::Analog, _ => PinMode::NC,
                                        };
                                        set_config.update(|c| c.pins[i] = new_mode);
                                    }
                                >
                                    <option value="NC" selected=matches!(mode, PinMode::NC)>"NC — Not connected"</option>
                                    <option value="GPIO" selected=matches!(mode, PinMode::GPIO)>"GPIO — Digital bidir"</option>
                                    <option value="GPI" selected=matches!(mode, PinMode::GPI)>"GPI — Digital input"</option>
                                    <option value="GPO" selected=matches!(mode, PinMode::GPO)>"GPO — Digital output"</option>
                                    {if analog_cap {
                                        Either::Left(view! { <option value="Analog" selected=matches!(mode, PinMode::Analog)>"Analog"</option> })
                                    } else { Either::Right(()) }}
                                </select>
                            </div>
                            <div class="board-inspector-row" class:board-row-disabled=move || !config.get().pins[selected_pin.get()].is_digital()>
                                <label title="2k series resistor limits peak current for protected driving into unknown loads">"Drive"</label>
                                <select class="dropdown"
                                    prop:disabled=move || !config.get().pins[selected_pin.get()].is_digital()
                                    on:change=move |e| {
                                        let v = event_target_value(&e);
                                        let d = match v.as_str() { "weak_2k" => DriveStrength::Weak2k, _ => DriveStrength::Standard };
                                        set_config.update(|c| c.pin_drive[i] = d);
                                        push_drive(i as u8, d);
                                    }
                                >
                                    <option value="standard" selected=matches!(drive, DriveStrength::Standard)>"Standard"</option>
                                    <option value="weak_2k" selected=matches!(drive, DriveStrength::Weak2k)>"Weak (2k series)"</option>
                                </select>
                            </div>
                            <div class="board-inspector-info">
                                <div class="board-inspector-info-row">
                                    <span>"Power domain"</span>
                                    <b class=format!("board-{}", dom_cls)>{dom_label}</b>
                                </div>
                                <div class="board-inspector-info-row">
                                    <span>"Rail voltage"</span>
                                    <b>{format!("{:.1} V", dom_v)}</b>
                                </div>
                                <div class="board-inspector-info-row">
                                    <span>"Analog capable"</span>
                                    <b>{if analog_cap { "yes" } else { "no" }}</b>
                                </div>
                                <div class="board-inspector-info-row">
                                    <span>"Drive limit"</span>
                                    <b>{if digital { drive.to_str() } else { "—" }}</b>
                                </div>
                                <div class="board-inspector-info-row">
                                    <span>"eFuse"</span>
                                    <b>{efuse_label(pin_efuse(i))}</b>
                                </div>
                            </div>
                            {if warn {
                                Either::Left(view! {
                                    <div class="board-inspector-warn">
                                        "Analog function not available on this pin. Only IO1/IO4/IO7/IO10 support analog."
                                    </div>
                                })
                            } else { Either::Right(()) }}
                        }
                    }}
                </div>
            </div>

            // ============ EFUSE PANEL ============
            <div class="board-efuse-panel">
                {(0..4).map(|e| {
                    let label = efuse_label(e);
                    let dom_cls = efuse_domain_cls(e);
                    view! {
                        <div class=format!("board-efuse-card board-efuse-{}", dom_cls)>
                            <div class="board-efuse-head">
                                <span class="board-efuse-label">{format!("eFuse {}", label)}</span>
                                <span class="board-efuse-current">"—mA"</span>
                                <span class="board-efuse-fault"
                                    class:board-efuse-fault-ok=move || config.get().efuses[e].sw_limit_enabled
                                ></span>
                            </div>
                            <div class="board-efuse-row">
                                <label title="Software current limit (100 – 1200 mA)">"SW Limit"</label>
                                <input type="number" class="board-efuse-input"
                                    min="100" max="1200" step="50"
                                    prop:value=move || config.get().efuses[e].sw_limit_ma.to_string()
                                    on:input=move |ev| {
                                        let v: u16 = event_target_value(&ev).parse().unwrap_or(500);
                                        let v = v.clamp(100, 1200);
                                        set_config.update(|c| c.efuses[e].sw_limit_ma = v);
                                        let en = config.get_untracked().efuses[e].sw_limit_enabled;
                                        push_efuse(e as u8, v, en);
                                    }
                                />
                                <span class="board-efuse-unit">"mA"</span>
                            </div>
                            <div class="board-efuse-row">
                                <label class="board-efuse-toggle">
                                    <input type="checkbox"
                                        prop:checked=move || config.get().efuses[e].sw_limit_enabled
                                        on:change=move |ev| {
                                            let checked: bool = ev.target().unwrap()
                                                .unchecked_into::<web_sys::HtmlInputElement>().checked();
                                            set_config.update(|c| c.efuses[e].sw_limit_enabled = checked);
                                            let lim = config.get_untracked().efuses[e].sw_limit_ma;
                                            push_efuse(e as u8, lim, checked);
                                        }
                                    />
                                    <span>"EN SW Lim"</span>
                                </label>
                            </div>
                        </div>
                    }
                }).collect::<Vec<_>>()}
            </div>

            // ============ BOTTOM STATUS STRIP ============
            <div class="board-status">
                {move || {
                    let cfg = config.get();
                    let configured = cfg.pins.iter().filter(|m| !matches!(m, PinMode::NC)).count();
                    let unset = 12 - configured;
                    view! {
                        <span class="board-status-chip">"Configured: "<b>{configured}</b></span>
                        <span class="board-status-chip">"Unset: "<b>{unset}</b></span>
                        <span class="board-status-chip">"Total: "<b>"12"</b></span>
                    }
                }}
                <span class="board-status-spacer"></span>
                <span class="board-status-hint">"Click a pin above, then use the inspector to configure mode, name and drive."</span>
            </div>
        </div>
    }
}

/// Single-char icon for a pin mode (used in pin tiles).
fn pin_icon(mode: PinMode) -> &'static str {
    match mode {
        PinMode::NC => "·",
        PinMode::GPIO => "\u{21C4}", // ⇄
        PinMode::GPI => "\u{2193}",  // ↓
        PinMode::GPO => "\u{2191}",  // ↑
        PinMode::Analog => "\u{223F}", // ∿
    }
}
