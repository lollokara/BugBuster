use crate::components::icons::Icon;
use crate::components::ui::{Callout, Switch};
use crate::tabs::daq_cal::CalibrationWizard;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use std::time::Duration;
use wasm_bindgen::JsValue;

/// Voltages is read-only on open; calibration commands manage ownership server-side.
pub const SLOTS: &[u8] = &[];

const CAL_TOTAL_POINTS: u32 = 100;
const HAT_TYPE_DAQ: u8 = 0x10;

/// IDAC channel order: 0 = VLOGIC, 1 = VADJ1, 2 = VADJ2.
const SUPPLY_NAMES: [&str; 3] = ["VLOGIC", "VADJ1", "VADJ2"];
const SUPPLY_SUBS: [&str; 3] = [
    "Level shifter \u{b7} LTM8078 Out2",
    "Domain A \u{b7} LTM8063 #1",
    "Domain B \u{b7} LTM8063 #2",
];
const SUPPLY_CLS: [&str; 3] = ["bv-r-logic", "bv-r-vadj1", "bv-r-vadj2"];
const SUPPLY_VFB: [f32; 3] = [0.8, 0.774, 0.774];
const SUPPLY_RINT_K: f32 = 249.0;

#[derive(Clone, Copy, PartialEq)]
enum IdacCalState {
    Idle,
    Running,
    Complete,
    Failed,
}

// ── Free function so the same logic can be called from 3 separate buttons ────

#[allow(clippy::too_many_arguments)]
fn start_idac_cal(
    ch: u8,
    set_state: WriteSignal<IdacCalState>,
    set_channel: WriteSignal<u8>,
    set_points: WriteSignal<u32>,
    set_last_v: WriteSignal<f32>,
    set_error_mv: WriteSignal<f32>,
    set_last_points: WriteSignal<u32>,
    set_log: WriteSignal<Vec<String>>,
) {
    set_channel.set(ch);
    set_state.set(IdacCalState::Running);
    set_log.set(Vec::new());
    set_points.set(0);
    set_last_v.set(-1.0);
    set_error_mv.set(0.0);
    set_last_points.set(0);
    let ch_name = match ch {
        1 => "VADJ1",
        2 => "VADJ2",
        _ => "VLOGIC",
    };
    set_log.update(|l| {
        l.push(format!(
            "Starting auto-calibration for {} (IDAC ch {})",
            ch_name, ch
        ))
    });
    spawn_local(async move {
        let args = serde_wasm_bindgen::to_value(&serde_json::json!({"channel": ch})).unwrap();
        let result = try_invoke("selftest_auto_calibrate", args).await;
        let rejected = match result
            .and_then(|r| serde_wasm_bindgen::from_value::<serde_json::Value>(r).ok())
        {
            Some(r) => r.get("status").and_then(|v| v.as_u64()).unwrap_or(3) == 3,
            None => true,
        };
        if !rejected {
            set_log.update(|l| l.push("Started \u{2014} polling\u{2026}".into()));
            return;
        }
        // VADJ sweeps reach 15 V, so the firmware refuses without a 20 V PD contract.
        let pd_msg = if ch != 0 {
            match fetch_usbpd_status().await {
                Some(pd) if !pd.attached => Some("No USB-PD source: VADJ calibration needs a 20 V PD supply.".to_string()),
                Some(pd) if pd.voltage_v < 17.5 => Some(format!(
                    "USB-PD is {:.1} V: VADJ calibration needs a 20 V PD supply (the firmware tries to negotiate it).",
                    pd.voltage_v
                )),
                _ => None,
            }
        } else {
            None
        };
        set_log.update(|l| {
            l.push(pd_msg.unwrap_or_else(|| {
                "Rejected (busy, interlock, e-fuse monitor active, or error).".into()
            }))
        });
        set_state.set(IdacCalState::Failed);
    });
}

/// Compare supply names across firmware spellings ("V_ADJ1", "VADJ1").
fn norm_name(s: &str) -> String {
    s.chars()
        .filter(|c| *c != '_' && *c != ' ')
        .collect::<String>()
        .to_uppercase()
}

fn fmt_ma(ma: f32) -> String {
    if ma.abs() < 1.0 {
        format!("{:.0}\u{b5}A", ma * 1000.0)
    } else {
        format!("{:.1}mA", ma)
    }
}

// ── Supply cards ──────────────────────────────────────────────────────────────

/// One DS4424 IDAC supply (VLOGIC / VADJ1 / VADJ2): setpoint vs measured, code
/// slider and SET. SET changes the rail on the live board immediately.
#[component]
fn IdacCard(
    i: usize,
    idac: ReadSignal<IdacState>,
    set_idac: WriteSignal<IdacState>,
    supplies: ReadSignal<SelftestSuppliesCached>,
) -> impl IntoView {
    let ch = move || idac.with(|s| s.channels.get(i).cloned());
    let slider_v = RwSignal::new(0.0f64);
    let dirty = RwSignal::new(false);
    let code_v = RwSignal::new(0i32);
    let code_dirty = RwSignal::new(false);

    // Follow the device while the user has no pending edit.
    Effect::new(move |_| {
        if let Some(c) = ch() {
            if !dirty.get() {
                slider_v.set(c.target_v as f64);
            }
            if !code_dirty.get() {
                code_v.set(c.code as i32);
            }
        }
    });

    let has_cal = move || {
        ch().map(|c| c.calibrated || c.cal_points.len() >= 2)
            .unwrap_or(false)
    };
    let disp_code = move || -> i8 {
        if code_dirty.get() {
            code_v.get() as i8
        } else {
            ch().map(|c| c.code).unwrap_or(0)
        }
    };
    let display_v = move || -> f32 {
        let Some(c) = ch() else { return 0.0 };
        let raw = if code_dirty.get() {
            idac_interpolate_voltage(&c, disp_code())
        } else if dirty.get() {
            slider_v.get() as f32
        } else if has_cal() {
            idac_interpolate_voltage(&c, c.code)
        } else {
            c.target_v
        };
        if c.v_min <= c.v_max {
            raw.clamp(c.v_min, c.v_max)
        } else {
            raw
        }
    };
    // (max_src, max_sink): how far the code can move below / above the midpoint.
    let limits = move || -> (i32, i32) {
        let Some(c) = ch() else { return (0, 0) };
        let step_v = c.step_mv / 1000.0;
        if i == 0 {
            (127, 127)
        } else if step_v > 0.0 {
            (
                (((c.midpoint_v - c.v_min) / step_v).floor() as i32).clamp(0, 127),
                (((c.v_max - c.midpoint_v) / step_v).floor() as i32).clamp(0, 127),
            )
        } else {
            (0, 0)
        }
    };
    let pct = move || -> f32 {
        let Some(c) = ch() else { return 0.0 };
        if c.v_max > c.v_min {
            ((display_v() - c.v_min) / (c.v_max - c.v_min) * 100.0).clamp(0.0, 100.0)
        } else {
            50.0
        }
    };
    let measured = move || -> Option<f32> {
        supplies.with(|s| {
            if !s.available {
                return None;
            }
            s.rails
                .iter()
                .find(|r| norm_name(&r.name) == SUPPLY_NAMES[i])
                .map(|r| r.voltage_v)
                .filter(|v| *v >= 0.0)
        })
    };

    let on_set = move |_| {
        let Some(c) = ch() else { return };
        if code_dirty.get_untracked() {
            send_idac_code(i as u8, code_v.get_untracked() as i8);
        } else if dirty.get_untracked() {
            let v = (slider_v.get_untracked() as f32).clamp(c.v_min, c.v_max);
            send_idac_voltage(i as u8, v);
        }
        dirty.set(false);
        code_dirty.set(false);
        // Read the new target back so the card shows what the device applied.
        spawn_local(async move {
            slp(700).await;
            if let Some(st) = fetch_idac_status().await {
                set_idac.try_set(st);
            }
        });
    };

    view! {
        <div class=format!("group bv-card {}", SUPPLY_CLS[i])>
            <div class="bv-card-head">
                <div class="bv-card-id">
                    <span class="bv-card-name"><span class="bv-dot" aria-hidden="true"></span>{SUPPLY_NAMES[i]}</span>
                    <span class="bv-card-sub">{SUPPLY_SUBS[i]}</span>
                </div>
                <span class="badge"
                    class:tone-green=has_cal
                    title="Calibration table stored on the device. Without it the output is estimated from the feedback formula."
                >{move || if has_cal() { "Calibrated" } else { "Formula only" }}</span>
            </div>

            <div class="bv-sm">
                <div class="bv-sm-col">
                    <span class="bv-sm-label">"Setpoint"</span>
                    <span class="bv-sm-val">{move || format!("{:.3}", display_v())}<span class="bv-unit">"V"</span></span>
                    <span class="bv-sm-note">
                        {move || {
                            let step = ch().map(|c| c.step_mv).unwrap_or(0.0);
                            format!("code {} \u{b7} {:.2} mV/step", disp_code(), step)
                        }}
                        <Show when=move || code_dirty.get()>
                            <span class="bv-pending">"preview"</span>
                        </Show>
                    </span>
                </div>
                <div class="bv-sm-col">
                    <span class="bv-sm-label">"Measured"</span>
                    <span class="bv-sm-val"
                        class:bv-muted=move || measured().is_none()
                    >
                        {move || match measured() {
                            Some(v) => format!("{:.3}", v),
                            None => "\u{2014}".to_string(),
                        }}
                        <span class="bv-unit">"V"</span>
                    </span>
                    <span class="bv-sm-note"
                        title="Measured by the supply monitor. Enable it on the Overview tab."
                    >
                        {move || match measured() {
                            Some(v) => {
                                let d = (v - display_v()) * 1000.0;
                                format!("\u{394} {:+.0} mV vs setpoint", d)
                            }
                            None => "Supply monitor off".to_string(),
                        }}
                    </span>
                </div>
            </div>

            <div class="bv-range">
                <div class="meter bv-meter" aria-hidden="true">
                    <span style=move || format!("width: {:.1}%", pct())></span>
                </div>
                <div class="bv-scale">
                    <span>{move || format!("{:.1} V", ch().map(|c| c.v_min).unwrap_or(0.0))}</span>
                    <span>{move || format!("{:.1} V mid", ch().map(|c| c.midpoint_v).unwrap_or(0.0))}</span>
                    <span>{move || format!("{:.1} V", ch().map(|c| c.v_max).unwrap_or(0.0))}</span>
                </div>
            </div>

            // Code slider (right = higher V)
            <input type="range" class="slider bv-slider"
                aria-label=format!("{} IDAC code", SUPPLY_NAMES[i])
                min=move || (-limits().0).to_string()
                max=move || limits().1.to_string()
                step="1"
                prop:value=move || {
                    let (src, sink) = limits();
                    (-code_v.get()).clamp(-src, sink)
                }
                on:input=move |e| {
                    if let Ok(v) = event_target_value(&e).parse::<i32>() {
                        let (src, sink) = limits();
                        let c = v.clamp(-src, sink);
                        code_v.set(-c);
                        code_dirty.set(true);
                        dirty.set(true);
                    }
                }
            />

            <div class="bv-set">
                <input type="number" class="bv-num"
                    aria-label=format!("{} setpoint (V)", SUPPLY_NAMES[i])
                    min=move || ch().map(|c| c.v_min.to_string()).unwrap_or_default()
                    max=move || ch().map(|c| c.v_max.to_string()).unwrap_or_default()
                    step="0.01"
                    prop:value=move || format!("{:.3}", display_v())
                    on:change=move |e| {
                        if let Ok(v) = event_target_value(&e).parse::<f64>() {
                            slider_v.set(v);
                            dirty.set(true);
                            code_dirty.set(false);
                        }
                    }
                />
                <span class="bv-unit-sm">"V"</span>
                <span class="spacer"></span>
                <button class="btn btn-sm btn-tinted tone-red"
                    disabled=move || !dirty.get()
                    title=move || if dirty.get() {
                        "Apply to the rail now. This changes the live supply voltage."
                    } else {
                        "Change the setpoint or code first"
                    }
                    on:click=on_set
                >"Set"</button>
            </div>

            <div class="bv-formula">
                {move || {
                    let mid = ch().map(|c| c.midpoint_v).unwrap_or(0.0);
                    let r_fs = (0.976 * 127.0) / (16.0 * 50.0e-6) / 1000.0;
                    if i == 0 {
                        format!("Midpoint 3.3 V \u{b7} code \u{b1}127 \u{b7} R_FS {:.0} k\u{3a9}", r_fs)
                    } else {
                        let v_fb = SUPPLY_VFB[i];
                        let r_fb = SUPPLY_RINT_K / (mid / v_fb - 1.0);
                        format!(
                            "V_FB {} V \u{b7} R_int {} k\u{3a9} \u{b7} R_FB {:.1} k\u{3a9} \u{b7} R_FS {:.0} k\u{3a9}",
                            v_fb, SUPPLY_RINT_K, r_fb, r_fs
                        )
                    }
                }}
            </div>
        </div>
    }
}

/// One LA HAT rail (3V3_ADJ / VADJ3 / VADJ4): enable switch, setpoint vs measured.
#[component]
#[allow(clippy::too_many_arguments)]
fn HatRailCard(
    id: u8,
    name: &'static str,
    spec: &'static str,
    cls: &'static str,
    min_mv: u16,
    max_mv: u16,
    parse_dflt: u16,
    show_current: bool,
    target: RwSignal<u16>,
    rails: ReadSignal<Vec<HatRailStatus>>,
    set_rails: WriteSignal<Vec<HatRailStatus>>,
) -> impl IntoView {
    let rail = move || rails.with(|r| r.iter().find(|x| x.rail_id == id).cloned());
    let en = move || rail().map(|r| r.enabled).unwrap_or(false);
    let measured_mv = move || rail().map(|r| r.voltage_mv).unwrap_or(0);

    view! {
        <div class=format!("group bv-card {cls}")>
            <div class="bv-card-head">
                <div class="bv-card-id">
                    <span class="bv-card-name"><span class="bv-dot" aria-hidden="true"></span>{name}</span>
                    <span class="bv-card-sub">{spec}</span>
                </div>
                <label class="bv-en" title="Enable or disable this rail on the live board">
                    <Switch
                        checked=Signal::derive(en)
                        aria_label=name
                        on_change=Callback::new(move |_: bool| {
                            let cur = en();
                            spawn_local(async move {
                                if let Some(r) = hat_set_rail_enable(id, !cur).await {
                                    set_rails.set(r);
                                } else {
                                    show_toast(&format!("Failed to toggle {}", name), "err");
                                }
                            });
                        })
                    />
                    <span class="bv-en-text" class:tone-text-green=en>{move || if en() { "On" } else { "Off" }}</span>
                </label>
            </div>

            <div class="bv-sm" class:bv-sm-3=move || show_current>
                <div class="bv-sm-col">
                    <span class="bv-sm-label">"Setpoint"</span>
                    <span class="bv-sm-val">{move || format!("{:.2}", target.get() as f32 / 1000.0)}<span class="bv-unit">"V"</span></span>
                    <span class="bv-sm-note">"Slider target"</span>
                </div>
                <div class="bv-sm-col">
                    <span class="bv-sm-label">"Measured"</span>
                    <span class="bv-sm-val" class:bv-muted=move || !en()>
                        {move || if en() { format!("{:.3}", measured_mv() as f32 / 1000.0) } else { "\u{2014}".to_string() }}
                        <span class="bv-unit">"V"</span>
                    </span>
                    <span class="bv-sm-note">{move || if en() { "Live" } else { "Rail off" }}</span>
                </div>
                {show_current.then(|| view! {
                    <div class="bv-sm-col">
                        <span class="bv-sm-label">"Current"</span>
                        <span class="bv-sm-val" class:bv-muted=move || !en()>
                            {move || rail().map(|r| r.current_ma).unwrap_or(0)}
                            <span class="bv-unit">"mA"</span>
                        </span>
                        <span class="bv-sm-note">"Rail monitor"</span>
                    </div>
                })}
            </div>

            <input type="range" class="slider bv-slider"
                aria-label=format!("{name} setpoint")
                min=min_mv.to_string() max=max_mv.to_string() step="100"
                prop:value=move || target.get().to_string()
                on:input=move |ev| {
                    let v = event_target_value(&ev).parse::<u16>().unwrap_or(parse_dflt);
                    target.set(v);
                }
            />
            <div class="bv-scale">
                <span>{format!("{:.1} V", min_mv as f32 / 1000.0)}</span>
                <span>{format!("{:.1} V", max_mv as f32 / 1000.0)}</span>
            </div>

            <div class="bv-set">
                <span class="bv-sm-note">{move || format!("{:.2} V target", target.get() as f32 / 1000.0)}</span>
                <span class="spacer"></span>
                <button class="btn btn-sm btn-tinted tone-red"
                    title="Apply the target to this rail now. This changes the live supply voltage."
                    on:click=move |_| {
                        let mv = target.get_untracked();
                        spawn_local(async move {
                            if let Some(r) = hat_set_rail_voltage(id, mv).await {
                                set_rails.set(r);
                            } else {
                                show_toast(&format!("Failed to set {}", name), "err");
                            }
                        });
                    }
                >"Confirm"</button>
            </div>
        </div>
    }
}

// ── Main component ────────────────────────────────────────────────────────────

#[component]
pub fn VoltagesTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // IDAC
    let (idac, set_idac) = signal(IdacState::default());
    let (supplies, set_supplies) = signal(SelftestSuppliesCached::default());
    let idac_present = Memo::new(move |_| idac.with(|s| s.present));

    // HAT rails
    let (hat, set_hat) = signal(HatStatus::default());
    let is_daq = move || {
        let h = hat.get();
        h.detected && h.hat_type == HAT_TYPE_DAQ
    };
    let is_la_hat = move || {
        let h = hat.get();
        h.detected && h.hat_type != HAT_TYPE_DAQ
    };
    let daq_cal_open = RwSignal::new(false);
    let (rails, set_rails) = signal(Vec::<HatRailStatus>::new());
    let v3v3_target = RwSignal::new(3300u16);
    let vadj3_target = RwSignal::new(3300u16);
    let vadj4_target = RwSignal::new(3300u16);

    // IDAC calibration
    let (idac_cal_state, set_idac_cal_state) = signal(IdacCalState::Idle);
    let (idac_cal_channel, set_idac_cal_channel) = signal(0u8);
    let (idac_cal_log, set_idac_cal_log) = signal(Vec::<String>::new());
    let (idac_cal_points, set_idac_cal_points) = signal(0u32);
    let (idac_cal_last_v, set_idac_cal_last_v) = signal(-1.0f32);
    let (idac_cal_error_mv, set_idac_cal_error_mv) = signal(0.0f32);
    let (idac_last_points, set_idac_last_points) = signal(0u32);
    let idac_poll_handle = RwSignal::new(None::<IntervalHandle>);

    // HAT calibration
    let (hat_cal_active, set_hat_cal_active) = signal(false);
    let (hat_cal_progress, set_hat_cal_progress) = signal(0u8);
    let (hat_cal_rail_id, set_hat_cal_rail_id) = signal(1u8);
    let (hat_cal_stage, set_hat_cal_stage) = signal(0u8);
    let (hat_cal_point, set_hat_cal_point) = signal(0u8);
    let (hat_cal_code, set_hat_cal_code) = signal(0i8);
    let (hat_cal_measured_mv, set_hat_cal_measured_mv) = signal(-1i32);
    let (hat_cal_persist_state, set_hat_cal_persist_state) = signal(0u8);
    let hat_cal_poll_handle = RwSignal::new(None::<IntervalHandle>);

    // Calibration is collapsed by default; it opens by itself while a run is active.
    let cal_open = RwSignal::new(false);
    let cal_visible = move || {
        cal_open.get() || idac_cal_state.get() != IdacCalState::Idle || hat_cal_active.get()
    };

    // Alive flag — flips false on tab unmount so background polls stop safely.
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    // ── Initial fetch (once; DESK-22: no longer re-run on every device-state tick)
    let _ = state;
    let alive_init = alive.clone();
    Effect::new(move |_| {
        let alive = alive_init.clone();
        spawn_local(async move {
            if let Some(st) = fetch_idac_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_idac.set(st);
                }
            }
            if let Some(st) = fetch_hat_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_hat.set(st);
                }
            }
            let la_hat = hat.try_get_untracked().is_some_and(|h| h.detected && h.hat_type != HAT_TYPE_DAQ);
            if let Some(rl) = if la_hat { hat_get_rail_status().await } else { None } {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_rails.set(rl);
                }
            }
        });
    });

    // HAT status + rails refresh (DESK-22: non-overlapping; backs off on a bare board)
    start_tab_poll(
        move || async move {
            if let Some(st) = fetch_hat_status().await {
                set_hat.try_set(st);
            }
            let la_hat = hat.try_get_untracked().is_some_and(|h| h.detected && h.hat_type != HAT_TYPE_DAQ);
            if let Some(rl) = if la_hat { hat_get_rail_status().await } else { None } {
                set_rails.try_set(rl);
            }
        },
        move || {
            if hat.try_get_untracked().is_some_and(|h| h.detected) { 2000 } else { HAT_ABSENT_POLL_MS }
        },
    );

    // Measured supply voltages (cached by the firmware supply monitor).
    start_tab_poll(
        move || async move {
            if let Some(sup) = fetch_selftest_supplies_cached().await {
                set_supplies.try_set(sup);
            }
        },
        || 2000,
    );

    // IDAC calibration poll (400 ms, only while running)
    let alive_cal = alive.clone();
    Effect::new(move |_| {
        let running = idac_cal_state.get() == IdacCalState::Running;
        let alive = alive_cal.clone();
        if running {
            if idac_poll_handle.get_untracked().is_none() {
                let handle = leptos::prelude::set_interval_with_handle(
                    move || {
                        if idac_cal_state.get_untracked() != IdacCalState::Running {
                            return;
                        }
                        let alive = alive.clone();
                        spawn_local(async move {
                            let result = try_invoke("selftest_status", JsValue::NULL).await;
                            if !alive.load(std::sync::atomic::Ordering::Relaxed) {
                                return;
                            }
                            if let Some(v) = result.and_then(|r| {
                                serde_wasm_bindgen::from_value::<serde_json::Value>(r).ok()
                            }) {
                                let cal = v.get("cal").cloned().unwrap_or(serde_json::Value::Null);
                                let status =
                                    cal.get("status").and_then(|x| x.as_u64()).unwrap_or(0) as u8;
                                let points =
                                    cal.get("points").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
                                let meas_v =
                                    cal.get("lastVoltageV")
                                        .and_then(|x| x.as_f64())
                                        .unwrap_or(-1.0) as f32;
                                let err_mv =
                                    cal.get("errorMv").and_then(|x| x.as_f64()).unwrap_or(0.0)
                                        as f32;
                                set_idac_cal_points.set(points);
                                set_idac_cal_last_v.set(meas_v);
                                set_idac_cal_error_mv.set(err_mv);
                                let prev = idac_last_points.get_untracked();
                                if points > prev {
                                    set_idac_cal_log.update(|l| {
                                        l.push(format!(
                                            "Progress: {}/{} pts  \u{b7}  {:.4}V",
                                            points, CAL_TOTAL_POINTS, meas_v
                                        ))
                                    });
                                    set_idac_last_points.set(points);
                                }
                                if idac_cal_state.get_untracked() != IdacCalState::Running {
                                    return;
                                }
                                if status == 2 {
                                    set_idac_cal_log.update(|l| {
                                        l.push(format!(
                                            "Complete: {} pts, error {:.1} mV",
                                            points, err_mv
                                        ))
                                    });
                                    set_idac_cal_state.set(IdacCalState::Complete);
                                } else if status == 3 {
                                    set_idac_cal_log
                                        .update(|l| l.push(format!("Failed (status={})", status)));
                                    set_idac_cal_state.set(IdacCalState::Failed);
                                }
                            }
                        });
                    },
                    Duration::from_millis(400),
                )
                .ok();
                idac_poll_handle.set(handle);
            }
        } else if let Some(h) = idac_poll_handle.get_untracked() {
            h.clear();
            idac_poll_handle.set(None);
        }
    });

    // HAT calibration poll (400 ms, only while active)
    let alive_hat = alive.clone();
    Effect::new(move |_| {
        let alive = alive_hat.clone();
        if hat_cal_active.get() {
            if hat_cal_poll_handle.get_untracked().is_none() {
                let handle = leptos::prelude::set_interval_with_handle(
                    move || {
                        if !hat_cal_active.get_untracked() {
                            return;
                        }
                        let alive = alive.clone();
                        spawn_local(async move {
                            if let Some(s) = hat_calibrate_status().await {
                                if !alive.load(std::sync::atomic::Ordering::Relaxed) {
                                    return;
                                }
                                set_hat_cal_progress.set(s.progress);
                                set_hat_cal_stage.set(s.stage);
                                set_hat_cal_point.set(s.point);
                                set_hat_cal_code.set(s.code);
                                set_hat_cal_measured_mv.set(s.measured_mv);
                                set_hat_cal_persist_state.set(s.persist_state);
                                if s.state == 2 {
                                    set_hat_cal_active.set(false);
                                    show_toast("HAT calibration complete!", "ok");
                                } else if s.state == 3 {
                                    set_hat_cal_active.set(false);
                                    show_toast("HAT calibration failed!", "err");
                                }
                            }
                        });
                    },
                    Duration::from_millis(400),
                )
                .ok();
                hat_cal_poll_handle.set(handle);
            }
        } else if let Some(h) = hat_cal_poll_handle.get_untracked() {
            h.clear();
            hat_cal_poll_handle.set(None);
        }
    });

    let start_cal = move |ch: u8| {
        start_idac_cal(
            ch,
            set_idac_cal_state,
            set_idac_cal_channel,
            set_idac_cal_points,
            set_idac_cal_last_v,
            set_idac_cal_error_mv,
            set_idac_last_points,
            set_idac_cal_log,
        )
    };

    view! {
        <div class="view bv">
            <p class="group-subtitle">"Supply rails with setpoint against measured value, and calibration. Rail changes act on the connected hardware immediately."</p>

            // ── SECTION: IDAC supplies ────────────────────────────────────────
            <div class="section-label">"Supplies \u{b7} DS4424 IDAC"</div>
            <Show
                when=move || idac_present.get()
                fallback=|| view! {
                    <Callout tone="orange">"DS4424 not detected on I2C bus (0x20). Check hardware connection."</Callout>
                }
            >
                <div class="grid-3 bv-grid">
                    {(0..3).map(|i| view! {
                        <IdacCard i=i idac=idac set_idac=set_idac supplies=supplies />
                    }).collect::<Vec<_>>()}
                </div>
            </Show>

            // ── SECTION: DAQ HAT DUT supply (replaces the LA-HAT rails) ──────────
            <Show when=is_daq>
                <DaqSupplyPanel/>
            </Show>

            // ── SECTION: HAT Voltage Rails ─────────────────────────────────────
            <Show when=is_la_hat>
                <div class="section-label">"HAT voltage rails"</div>
                <div class="grid-3 bv-grid">
                    <HatRailCard id=0 name="3V3_ADJ" spec="1.7 \u{2013} 5.0 V \u{b7} Level shifter power" cls="bv-r-logic"
                        min_mv=1700 max_mv=5000 parse_dflt=3300 show_current=false
                        target=v3v3_target rails=rails set_rails=set_rails />
                    <HatRailCard id=1 name="VADJ3" spec="1.8 \u{2013} 36 V \u{b7} Connector 1" cls="bv-r-vadj1"
                        min_mv=0 max_mv=36000 parse_dflt=0 show_current=true
                        target=vadj3_target rails=rails set_rails=set_rails />
                    <HatRailCard id=2 name="VADJ4" spec="1.8 \u{2013} 36 V \u{b7} Connector 2" cls="bv-r-vadj2"
                        min_mv=0 max_mv=36000 parse_dflt=0 show_current=true
                        target=vadj4_target rails=rails set_rails=set_rails />
                </div>
            </Show>

            // ── SECTION: Calibration (separated, collapsed by default) ─────────
            <div class="section-label bv-cal-label">"Calibration"</div>
            <div class="group bv-cal">
                <div class="bv-cal-head">
                    <div class="bv-cal-title">
                        <span class="bv-cal-icon"><Icon name="triangle-alert" size=16 /></span>
                        <div>
                            <div class="group-title">"Calibrate supplies"</div>
                            <div class="group-subtitle">"Writes calibration tables to the device and drives the rails through their range."</div>
                        </div>
                    </div>
                    <div class="bv-cal-sum">
                        {move || {
                            let st = idac.get();
                            (0..3).map(|i| {
                                let cal = st.channels.get(i).map(|c| c.calibrated).unwrap_or(false);
                                view! {
                                    <span class="badge" class:tone-green=cal class:tone-orange=!cal>
                                        {SUPPLY_NAMES[i]}{if cal { " cal" } else { " uncal" }}
                                    </span>
                                }
                            }).collect::<Vec<_>>()
                        }}
                    </div>
                    <button class="btn btn-sm"
                        aria-expanded=move || if cal_visible() { "true" } else { "false" }
                        disabled=move || idac_cal_state.get() != IdacCalState::Idle || hat_cal_active.get()
                        title=move || if idac_cal_state.get() != IdacCalState::Idle || hat_cal_active.get() {
                            "A calibration is running"
                        } else {
                            ""
                        }
                        on:click=move |_| cal_open.update(|o| *o = !*o)
                    >
                        <Icon name="chevron-down" size=14 />
                        {move || if cal_visible() { "Hide controls" } else { "Show controls" }}
                    </button>
                </div>

                <Show when=cal_visible>
                    <div class="bv-cal-body">
                        // ── IDAC auto-calibration ──────────────────────────────
                        <div class="bv-cal-sec">
                            <div class="bv-cal-sec-head">
                                <span class="bv-cal-sec-title">"IDAC auto-calibration"</span>
                                <span class="subtle">"U23 self-test MUX \u{2192} AD74416H Ch D"</span>
                            </div>

                            {move || match idac_cal_state.get() {
                                IdacCalState::Idle => {
                                    let idac_st = idac.get();
                                    view! {
                                        <div class="stack">
                                            <div class="grid-3 bv-cal-grid">
                                                {(0..3).map(|i| {
                                                    let has_cal = idac_st.channels.get(i).map(|c| c.calibrated).unwrap_or(false);
                                                    let mid = idac_st.channels.get(i).map(|c| c.midpoint_v).unwrap_or(0.0);
                                                    view! {
                                                        <div class=format!("bv-cal-card {}", SUPPLY_CLS[i])>
                                                            <div class="bv-cal-card-top">
                                                                <span class="bv-card-name"><span class="bv-dot" aria-hidden="true"></span>{SUPPLY_NAMES[i]}</span>
                                                                <span class="badge" class:tone-green=has_cal class:tone-orange=!has_cal>
                                                                    {if has_cal { "Calibrated" } else { "Uncalibrated" }}
                                                                </span>
                                                            </div>
                                                            <span class="bv-card-sub">{format!("mid {:.2} V \u{b7} IDAC ch {}", mid, i)}</span>
                                                            <button class="btn btn-sm btn-danger btn-block"
                                                                disabled=move || !idac.get().present
                                                                title=move || if idac.get().present { "Overwrites the stored calibration for this supply" } else { "DS4424 not detected" }
                                                                on:click=move |_| start_cal(i as u8)
                                                            >"Auto-calibrate"</button>
                                                        </div>
                                                    }
                                                }).collect::<Vec<_>>()}
                                            </div>
                                            {if !idac_st.present {
                                                view! { <Callout tone="orange">"DS4424 not detected. Check I2C connection."</Callout> }.into_any()
                                            } else {
                                                view! { <Callout tone="orange">"Level shifter OE and all e-fuses are disabled automatically during calibration. Disconnect external loads first."</Callout> }.into_any()
                                            }}
                                        </div>
                                    }.into_any()
                                }

                                IdacCalState::Running => {
                                    let ch = idac_cal_channel.get();
                                    let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "VLOGIC" };
                                    view! {
                                        <div class="bv-run">
                                            <div class="bv-run-title">
                                                <span class="spinner" aria-hidden="true"></span>
                                                {format!("Calibrating {} \u{2026}", ch_name)}
                                            </div>
                                            <div class="progress" role="progressbar" aria-valuemin="0" aria-valuemax="100"
                                                aria-valuenow=move || (idac_cal_points.get() * 100 / CAL_TOTAL_POINTS).min(100).to_string()>
                                                <span style=move || format!("width: {}%", (idac_cal_points.get() * 100 / CAL_TOTAL_POINTS).min(100))></span>
                                            </div>
                                            <div class="bv-run-stats">
                                                {move || format!("{}/{} points  \u{b7}  {:.4} V", idac_cal_points.get(), CAL_TOTAL_POINTS, idac_cal_last_v.get())}
                                            </div>
                                            <div class="bv-log">
                                                {move || idac_cal_log.get().iter().rev().take(10).map(|l| view! { <div>{l.clone()}</div> }).collect::<Vec<_>>()}
                                            </div>
                                        </div>
                                    }.into_any()
                                }

                                IdacCalState::Complete => {
                                    let ch = idac_cal_channel.get();
                                    let name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "VLOGIC" };
                                    let pts = idac_cal_points.get();
                                    let err = idac_cal_error_mv.get();
                                    view! {
                                        <div class="stack">
                                            <Callout tone="green">
                                                <strong>{format!("{} calibrated \u{2014} {} pts \u{b7} {:.1} mV error", name, pts, err)}</strong>
                                                " Saved to NVS flash. Level shifter and e-fuses restored."
                                            </Callout>
                                            <div class="hstack">
                                                <button class="btn btn-sm" on:click=move |_| set_idac_cal_state.set(IdacCalState::Idle)>"Done"</button>
                                                <button class="btn btn-sm btn-primary" on:click=move |_| set_idac_cal_state.set(IdacCalState::Idle)>"Calibrate another"</button>
                                            </div>
                                        </div>
                                    }.into_any()
                                }

                                IdacCalState::Failed => {
                                    view! {
                                        <div class="stack">
                                            <Callout tone="red"><strong>"Calibration failed"</strong></Callout>
                                            <div class="bv-log">
                                                {move || idac_cal_log.get().iter().map(|l| view! { <div>{l.clone()}</div> }).collect::<Vec<_>>()}
                                            </div>
                                            <div class="hstack">
                                                <button class="btn btn-sm" on:click=move |_| set_idac_cal_state.set(IdacCalState::Idle)>"Back"</button>
                                            </div>
                                        </div>
                                    }.into_any()
                                }
                            }}
                        </div>

                        // ── DAQ HAT DUT supply calibration ────────────────────
                        <Show when=is_daq>
                            <div class="bv-cal-sec">
                                <div class="bv-cal-sec-head">
                                    <span class="bv-cal-sec-title">"DAQ HAT calibration"</span>
                                    <span class="subtle">"DUT supply voltage and current"</span>
                                </div>
                                <div class="hstack bv-wrap">
                                    <button class="btn btn-sm btn-danger"
                                        title="Starts the guided calibration. It replaces the stored DUT supply calibration."
                                        on:click=move |_| daq_cal_open.set(true)
                                    >"Open calibration wizard"</button>
                                    <span class="bv-hint">"Guided voltage and current-range calibration on the P4. Needs a reference meter."</span>
                                </div>
                            </div>
                            <CalibrationWizard open=daq_cal_open/>
                        </Show>

                        // ── HAT rail calibration (LA HAT only) ─────────────────
                        <Show when=is_la_hat>
                            <div class="bv-cal-sec">
                                <div class="bv-cal-sec-head">
                                    <span class="bv-cal-sec-title">"HAT rail calibration sweep"</span>
                                    <span class="subtle">"RP2040 DAC sweep"</span>
                                </div>

                                {move || if hat_cal_active.get() {
                                    let prog  = hat_cal_progress.get();
                                    let name  = if hat_cal_rail_id.get() == 1 { "VADJ3" } else { "VADJ4" };
                                    let stage = match hat_cal_stage.get() {
                                        1 => "prepare", 2 => "step", 3 => "settle",
                                        4 => "measure", 5 => "done", 8 => "error", _ => "idle",
                                    };
                                    let meas  = hat_cal_measured_mv.get();
                                    view! {
                                        <div class="bv-run">
                                            <div class="bv-run-title">
                                                <span class="spinner" aria-hidden="true"></span>
                                                {format!("Calibrating {} \u{2026}  {}%", name, prog)}
                                            </div>
                                            <div class="progress" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow=prog.to_string()>
                                                <span style=format!("width: {}%", prog)></span>
                                            </div>
                                            <div class="bv-run-stats">
                                                <span>{format!("stage: {}", stage)}</span>
                                                <span>{format!("code: {}", hat_cal_code.get())}</span>
                                                <span>{format!("pt: {}", hat_cal_point.get())}</span>
                                                <span>{if meas >= 0 { format!("{:.3} V", meas as f32 / 1000.0) } else { "\u{2014}".into() }}</span>
                                            </div>
                                            {move || {
                                                let (label, tone) = match hat_cal_persist_state.get() {
                                                    1 => ("Saved to flash", "badge tone-green"),
                                                    2 => ("Imported, not verified", "badge tone-blue"),
                                                    _ => ("RAM only \u{2014} reboot will reset", "badge tone-orange"),
                                                };
                                                view! { <span class=tone>{label}</span> }
                                            }}
                                        </div>
                                    }.into_any()
                                } else {
                                    view! {
                                        <div class="hstack bv-wrap">
                                            <label class="bv-field-label" for="bv-hat-rail">"Rail"</label>
                                            <select id="bv-hat-rail" class="dropdown"
                                                on:change=move |ev| {
                                                    let val = event_target_value(&ev);
                                                    if let Ok(id) = val.parse::<u8>() { set_hat_cal_rail_id.set(id); }
                                                }
                                            >
                                                <option value="1">"VADJ3 (1.8\u{2013}36 V)"</option>
                                                <option value="2">"VADJ4 (1.8\u{2013}36 V)"</option>
                                            </select>
                                            <button class="btn btn-sm btn-danger"
                                                title="Sweeps the selected rail through its range and replaces its calibration"
                                                on:click=move |_| {
                                                    spawn_local(async move {
                                                        let id = hat_cal_rail_id.get_untracked();
                                                        if let Some(code) = hat_calibrate_start(id).await {
                                                            if code == 1 {
                                                                set_hat_cal_active.set(true);
                                                                set_hat_cal_progress.set(0);
                                                                show_toast("HAT calibration sweep started!", "ok");
                                                            } else {
                                                                show_toast(&format!("Start failed (code {})", code), "err");
                                                            }
                                                        }
                                                    });
                                                }
                                            >"Start sweep"</button>
                                            <button class="btn btn-sm btn-tinted tone-red"
                                                title="Load a calibration table from a JSON file and apply it to the selected rail"
                                                on:click=move |_| {
                                                    use wasm_bindgen::JsCast;
                                                    let input = web_sys::window()
                                                        .and_then(|w| w.document())
                                                        .and_then(|d| d.create_element("input").ok())
                                                        .and_then(|el| el.dyn_into::<web_sys::HtmlInputElement>().ok());
                                                    if let Some(inp) = input {
                                                        inp.set_type("file");
                                                        inp.set_accept(".json,application/json");
                                                        let rail_id = hat_cal_rail_id.get_untracked();
                                                        let closure = wasm_bindgen::closure::Closure::<dyn FnMut(web_sys::Event)>::new(move |ev: web_sys::Event| {
                                                            let target = ev.target().and_then(|t| t.dyn_into::<web_sys::HtmlInputElement>().ok());
                                                            if let Some(t) = target {
                                                                if let Some(file) = t.files().and_then(|fl| fl.get(0)) {
                                                                    let reader = web_sys::FileReader::new().unwrap();
                                                                    let reader_clone = reader.clone();
                                                                    let onload = wasm_bindgen::closure::Closure::<dyn FnMut(web_sys::Event)>::new(move |_: web_sys::Event| {
                                                                        if let Ok(result) = reader_clone.result() {
                                                                            if let Some(text) = result.as_string() {
                                                                                if let Ok(json) = serde_json::from_str::<serde_json::Value>(&text) {
                                                                                    let pts = json.get("points").and_then(|v| v.as_array()).cloned().unwrap_or_default();
                                                                                    let points = pts.iter().map(|p| HatCalibratePoint {
                                                                                        dac_code:   p.get("dacCode").and_then(|v| v.as_i64()).unwrap_or(0) as i8,
                                                                                        measured_v: p.get("measuredV").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32,
                                                                                    }).collect::<Vec<_>>();
                                                                                    spawn_local(async move {
                                                                                        if hat_calibrate_import(rail_id, points).await.is_some() {
                                                                                            show_toast("Calibration imported!", "ok");
                                                                                        } else {
                                                                                            show_toast("Import failed", "err");
                                                                                        }
                                                                                    });
                                                                                }
                                                                            }
                                                                        }
                                                                    });
                                                                    reader.set_onload(Some(onload.as_ref().unchecked_ref()));
                                                                    let _ = reader.read_as_text(&file);
                                                                    onload.forget();
                                                                }
                                                            }
                                                        });
                                                        inp.set_onchange(Some(closure.as_ref().unchecked_ref()));
                                                        inp.click();
                                                        closure.forget();
                                                    }
                                                }
                                            >"Import JSON\u{2026}"</button>
                                        </div>
                                    }.into_any()
                                }}
                            </div>
                        </Show>
                    </div>
                </Show>
            </div>
        </div>
    }
}

// ── DAQ HAT DUT supply card ───────────────────────────────────────────────────
async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        if let Some(w) = web_sys::window() {
            w.set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32)
                .ok();
        }
    });
    wasm_bindgen_futures::JsFuture::from(p).await.ok();
}

// Uses daq_vdut_* commands: BBP (0xB6/0xBF) over USB, /api/daq/vdut/* over HTTP,
// so it works without the HS DAQ tab's USB-HS stream being connected.
#[component]
fn DaqSupplyPanel() -> impl IntoView {
    let set_v = RwSignal::new(5.0f64);
    let set_ma = RwSignal::new(500u32);
    let loaded = RwSignal::new(false);
    let live = RwSignal::new(Option::<VdutStatus>::None);
    let busy = RwSignal::new(false);
    let pd_warn = RwSignal::new(String::new());

    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    {
        let a = alive.clone();
        on_cleanup(move || a.store(false, std::sync::atomic::Ordering::Relaxed));
    }
    {
        let alive = alive.clone();
        spawn_local(async move {
            while alive.load(std::sync::atomic::Ordering::Relaxed) {
                let st = daq_vdut_status().await;
                // The panel may have been unmounted during the await.
                if !alive.load(std::sync::atomic::Ordering::Relaxed) {
                    break;
                }
                if let Some(s) = st {
                    if loaded.try_get_untracked() == Some(false) {
                        set_v.try_set(s.setpoint_mv as f64 / 1000.0);
                        set_ma.try_set(s.ilimit_ma);
                        loaded.try_set(true);
                    }
                    live.try_set(Some(s));
                }
                slp(1000).await;
            }
        });
    }

    let on = move || live.get().map(|m| m.enabled).unwrap_or(false);

    let apply_setpoint = move |_| {
        let mv = (set_v.get_untracked() * 1000.0).round().clamp(1800.0, 20000.0) as u16;
        let ma = set_ma.get_untracked().clamp(100, 2500) as u16;
        busy.set(true);
        spawn_local(async move {
            let ok = daq_vdut_set_setpoint(mv, ma).await;
            busy.try_set(false);
            if ok {
                show_toast(&format!("DUT setpoint {:.2} V / {} mA", mv as f32 / 1000.0, ma), "ok");
            } else {
                show_toast("Failed to set DUT setpoint", "err");
            }
        });
    };

    let toggle = move || {
        let want = !live.get_untracked().map(|m| m.enabled).unwrap_or(false);
        busy.set(true);
        spawn_local(async move {
            if want {
                // The P4 enforces the same guard; this only gives a clearer message.
                if let Some(pd) = fetch_usbpd_status().await {
                    if !(pd.attached && pd.voltage_v >= 9.0 && pd.current_a >= 3.0) {
                        pd_warn.try_set(if !pd.attached {
                            "Blocked: no USB-PD source (need \u{2265} 9 V / 3 A)".to_string()
                        } else {
                            format!("Blocked: USB-PD {:.1} V / {:.1} A, need \u{2265} 9 V / 3 A", pd.voltage_v, pd.current_a)
                        });
                        busy.try_set(false);
                        return;
                    }
                }
            }
            pd_warn.try_set(String::new());
            if !daq_vdut_set_enable(want).await {
                show_toast("Failed to switch the DUT supply", "err");
            }
            if let Some(s) = daq_vdut_status().await {
                live.try_set(Some(s));
            }
            busy.try_set(false);
        });
    };

    // Applied setpoint reported by the device; falls back to the input until the first status.
    let applied_v = move || {
        live.get()
            .map(|m| m.setpoint_mv as f64 / 1000.0)
            .unwrap_or_else(|| set_v.get())
    };
    let pending = move || {
        live.get()
            .map(|m| {
                (set_v.get() * 1000.0).round() as i64 != m.setpoint_mv as i64
                    || set_ma.get() != m.ilimit_ma
            })
            .unwrap_or(false)
    };

    view! {
        <div class="section-label">"DAQ HAT DUT supply"</div>
        <div class="group bv-card bv-r-dut">
            <div class="bv-card-head">
                <div class="bv-card-id">
                    <span class="bv-card-name"><span class="bv-dot" aria-hidden="true"></span>"V_DUT"</span>
                    <span class="bv-card-sub">"1.8 \u{2013} 20 V \u{b7} 100 \u{2013} 2500 mA limit \u{b7} needs USB-PD \u{2265} 9 V / 3 A"</span>
                </div>
                <label class="bv-en" title="Switch the DUT supply output on or off">
                    <Switch
                        checked=Signal::derive(on)
                        aria_label="DUT supply output"
                        disabled=Signal::derive(move || busy.get())
                        on_change=Callback::new(move |_: bool| { if !busy.get_untracked() { toggle() } })
                    />
                    <span class="bv-en-text" class:tone-text-green=on>{move || if on() { "On" } else { "Off" }}</span>
                </label>
            </div>

            <div class="bv-sm bv-sm-3">
                <div class="bv-sm-col">
                    <span class="bv-sm-label">"Setpoint"</span>
                    <span class="bv-sm-val">{move || format!("{:.2}", applied_v())}<span class="bv-unit">"V"</span></span>
                    <span class="bv-sm-note">
                        {move || live.get().map(|m| format!("limit {} mA", m.ilimit_ma)).unwrap_or_else(|| "Waiting for device".to_string())}
                        <Show when=pending><span class="bv-pending">"unsent edit"</span></Show>
                    </span>
                </div>
                <div class="bv-sm-col">
                    <span class="bv-sm-label">"Measured"</span>
                    <span class="bv-sm-val" class:bv-muted=move || live.get().is_none()>
                        {move || live.get().map(|m| format!("{:.3}", m.voltage_v)).unwrap_or_else(|| "\u{2014}".into())}
                        <span class="bv-unit">"V"</span>
                    </span>
                    <span class="bv-sm-note">
                        {move || match live.get() {
                            Some(m) if m.enabled => format!("\u{394} {:+.0} mV vs setpoint", (m.voltage_v as f64 - applied_v()) * 1000.0),
                            Some(_) => "Output off".to_string(),
                            None => "\u{2014}".to_string(),
                        }}
                    </span>
                </div>
                <div class="bv-sm-col">
                    <span class="bv-sm-label">"Current"</span>
                    <span class="bv-sm-val" class:bv-muted=move || !on()>
                        {move || match live.get() {
                            Some(m) if m.enabled => fmt_ma(m.current_a * 1000.0),
                            Some(_) => "OFF".into(),
                            None => "\u{2014}".into(),
                        }}
                    </span>
                    <span class="bv-sm-note">{move || if live.get().map(|m| m.fault).unwrap_or(false) { "Fault flagged" } else { "Output current" }}</span>
                </div>
            </div>

            <div class="bv-set bv-wrap">
                <label class="bv-field-label" for="bv-dut-v">"Voltage"</label>
                <input id="bv-dut-v" type="number" class="bv-num"
                    min="1.8" max="20" step="0.1"
                    prop:value=move || format!("{:.2}", set_v.get())
                    on:change=move |e| { if let Ok(v) = event_target_value(&e).parse::<f64>() { set_v.set(v.clamp(1.8, 20.0)); } }
                />
                <span class="bv-unit-sm">"V"</span>
                <label class="bv-field-label" for="bv-dut-ma">"Limit"</label>
                <input id="bv-dut-ma" type="number" class="bv-num"
                    min="100" max="2500" step="10"
                    prop:value=move || set_ma.get().to_string()
                    on:change=move |e| { if let Ok(v) = event_target_value(&e).parse::<u32>() { set_ma.set(v.clamp(100, 2500)); } }
                />
                <span class="bv-unit-sm">"mA"</span>
                <span class="spacer"></span>
                <button class="btn btn-sm btn-tinted tone-red"
                    title="Apply the voltage and current limit to the DUT supply now"
                    disabled=move || busy.get() || !loaded.get()
                    on:click=apply_setpoint
                >"Confirm"</button>
            </div>
            <Show when=move || !pd_warn.get().is_empty()>
                <Callout tone="orange">{move || pd_warn.get()}</Callout>
            </Show>
        </div>
    }
}
