use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use std::time::Duration;
use wasm_bindgen::JsValue;

/// IO slots claimed by this tab — CH0..CH3 (indices 12..15) for selftest auto-cal.
pub const SLOTS: &[u8] = &[12, 13, 14, 15];

const CAL_TOTAL_POINTS: u32 = 100;

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
        if let Some(r) =
            result.and_then(|r| serde_wasm_bindgen::from_value::<serde_json::Value>(r).ok())
        {
            let status = r.get("status").and_then(|v| v.as_u64()).unwrap_or(3) as u8;
            if status == 3 {
                set_log.update(|l| l.push("Rejected (busy / interlock / error).".into()));
                set_state.set(IdacCalState::Failed);
            } else {
                set_log.update(|l| l.push("Started — polling…".into()));
            }
        } else {
            set_log.update(|l| l.push("Error: failed to start calibration.".into()));
            set_state.set(IdacCalState::Failed);
        }
    });
}

// ── Glass card style ──────────────────────────────────────────────────────────

fn glass(color: &str) -> String {
    format!(
        "background: rgba(255,255,255,0.03); \
         backdrop-filter: blur(12px); \
         border: 1px solid rgba(255,255,255,0.07); \
         border-radius: 16px; \
         border-top: 2px solid {color}; \
         box-shadow: 0 0 28px {color}14, 0 4px 24px rgba(0,0,0,0.55); \
         padding: 20px; \
         transition: box-shadow 0.3s ease;"
    )
}

// ── Main component ────────────────────────────────────────────────────────────

#[component]
pub fn VoltagesTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // IDAC
    let (idac, set_idac) = signal(IdacState::default());
    let slider_vals: [RwSignal<f64>; 3] = std::array::from_fn(|_| RwSignal::new(0.0));
    let dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));
    let code_vals: [RwSignal<i32>; 3] = std::array::from_fn(|_| RwSignal::new(0));
    let code_dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));

    // HAT rails
    let (hat, set_hat) = signal(HatStatus::default());
    let (rails, set_rails) = signal(Vec::<HatRailStatus>::new());
    let (v3v3_target_mv, set_v3v3_target_mv) = signal(3300u16);
    let (vadj3_target_mv, set_vadj3_target_mv) = signal(3300u16);
    let (vadj4_target_mv, set_vadj4_target_mv) = signal(3300u16);

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

    // Alive flag — flips false on tab unmount so background polls stop safely.
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    // ── Initial + reactive fetch ───────────────────────────────────────────────
    let alive_init = alive.clone();
    Effect::new(move |_| {
        let _ = state.get();
        let alive = alive_init.clone();
        spawn_local(async move {
            if let Some(st) = fetch_idac_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    for (i, ch) in st.channels.iter().take(3).enumerate() {
                        if !dirty[i].get_untracked() {
                            slider_vals[i].set(ch.target_v as f64);
                        }
                        if !code_dirty[i].get_untracked() {
                            code_vals[i].set(ch.code as i32);
                        }
                    }
                    set_idac.set(st);
                }
            }
            if let Some(st) = fetch_hat_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_hat.set(st);
                }
            }
            if let Some(rl) = hat_get_rail_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_rails.set(rl);
                }
            }
        });
    });

    // 2 s HAT status + rails refresh
    let alive_poll = alive.clone();
    Effect::new(move |_| {
        let alive = alive_poll.clone();
        let handle = leptos::prelude::set_interval_with_handle(
            move || {
                let alive = alive.clone();
                spawn_local(async move {
                    if let Some(st) = fetch_hat_status().await {
                        if alive.load(std::sync::atomic::Ordering::Relaxed) {
                            set_hat.set(st);
                        }
                    }
                    if let Some(rl) = hat_get_rail_status().await {
                        if alive.load(std::sync::atomic::Ordering::Relaxed) {
                            set_rails.set(rl);
                        }
                    }
                });
            },
            Duration::from_secs(2),
        )
        .ok();
        on_cleanup(move || {
            if let Some(h) = handle {
                h.clear();
            }
        });
    });

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
                                            "Progress: {}/{} pts  ·  {:.4}V",
                                            points, CAL_TOTAL_POINTS, meas_v
                                        ))
                                    });
                                    set_idac_last_points.set(points);
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

    // ── Rail helpers ───────────────────────────────────────────────────────────
    let rail_en = move |id: u8| {
        rails
            .get()
            .iter()
            .find(|r| r.rail_id == id)
            .map(|r| r.enabled)
            .unwrap_or(false)
    };
    let rail_mv = move |id: u8| {
        rails
            .get()
            .iter()
            .find(|r| r.rail_id == id)
            .map(|r| r.voltage_mv)
            .unwrap_or(0)
    };
    let rail_ma = move |id: u8| {
        rails
            .get()
            .iter()
            .find(|r| r.rail_id == id)
            .map(|r| r.current_ma)
            .unwrap_or(0)
    };

    // ── Static channel metadata ────────────────────────────────────────────────
    let ch_colors = ["#10b981", "#06b6d4", "#ff4d6a"];
    let ch_titles = [
        "Level Shifter  ·  TPS74601PDRVT",
        "V_ADJ1 — Domain A  ·  LTM8063 #1",
        "V_ADJ2 — Domain B  ·  LTM8063 #2",
    ];
    let ch_vfb = [0.8f32, 0.774, 0.774];
    let ch_rint = 249.0f32;
    let ifs_ua = 50.0f32;
    let r_fs = (0.976 * 127.0) / (16.0 * ifs_ua * 1e-6) / 1000.0;

    view! {
        <div class="tab-content">

            // Keyframe animations injected once
            <style>
                "@keyframes vt-slide-in {
                    from { opacity: 0; transform: translateY(-10px); }
                    to   { opacity: 1; transform: translateY(0); }
                }
                @keyframes vt-progress-sweep {
                    0%   { background-position: -200% 0; }
                    100% { background-position: 200% 0; }
                }"
            </style>

            // ── SECTION: IDAC Voltage Control ─────────────────────────────────
            <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.1em; color: var(--text-dim); margin-bottom: 10px">
                "DS4424 IDAC Voltage Control"
            </div>

            {move || {
                let st = idac.get();
                if !st.present {
                    return view! {
                        <div style="background: rgba(255,255,255,0.02); border: 1px solid rgba(255,255,255,0.07); border-radius: 16px; padding: 20px; margin-bottom: 24px">
                            <div class="mode-warning">
                                <span class="mode-warning-icon">"⚠"</span>
                                <span>"DS4424 not detected on I2C bus (0x20). Check hardware connection."</span>
                            </div>
                        </div>
                    }.into_any();
                }

                view! {
                    <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; margin-bottom: 28px">
                        {st.channels.into_iter().enumerate().map(|(i, ch)| {
                            let color = ch_colors[i];
                            let v_fb  = ch_vfb[i];
                            let r_fb  = ch_rint / (ch.midpoint_v / v_fb - 1.0);

                            let has_cal     = ch.calibrated || ch.cal_points.len() >= 2;
                            let disp_code   = if code_dirty[i].get() { code_vals[i].get() as i8 } else { ch.code };
                            let raw_v       = if code_dirty[i].get() {
                                idac_interpolate_voltage(&ch, disp_code)
                            } else if dirty[i].get() {
                                slider_vals[i].get() as f32
                            } else if has_cal {
                                idac_interpolate_voltage(&ch, ch.code)
                            } else {
                                ch.target_v
                            };
                            let display_v   = raw_v.clamp(ch.v_min, ch.v_max);
                            let safe_min    = ch.v_min;
                            let safe_max    = ch.v_max;
                            let pct         = if safe_max > safe_min {
                                ((display_v - safe_min) / (safe_max - safe_min) * 100.0).clamp(0.0, 100.0)
                            } else { 50.0 };
                            let step_v      = ch.step_mv / 1000.0;
                            let max_sink    = if i == 0 { 127i32 } else if step_v > 0.0 {
                                (((safe_max - ch.midpoint_v) / step_v).floor() as i32).clamp(0, 127)
                            } else { 0 };
                            let max_src     = if i == 0 { 127i32 } else if step_v > 0.0 {
                                (((ch.midpoint_v - safe_min) / step_v).floor() as i32).clamp(0, 127)
                            } else { 0 };
                            let ch_idx = i as u8;

                            view! {
                                <div style=glass(color)>
                                    // Header
                                    <div style="margin-bottom: 14px">
                                        <div style=format!("font-size: 11px; font-weight: 700; color: {color}; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 3px")>
                                            {ch_titles[i]}
                                        </div>
                                        <div style="font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">
                                            {if i == 0 {
                                                format!("Midpoint 3.3V  ·  code ±127  ·  R_FS: {r_fs:.0}kΩ")
                                            } else {
                                                format!("V_FB: {v_fb}V  ·  R_int: {ch_rint}kΩ  ·  R_FB: {r_fb:.1}kΩ  ·  R_FS: {r_fs:.0}kΩ")
                                            }}
                                        </div>
                                    </div>

                                    // Big voltage readout
                                    <div style=format!(
                                        "text-align: center; font-size: 40px; font-weight: 800; \
                                         font-family: 'JetBrains Mono', monospace; color: {color}; \
                                         text-shadow: 0 0 28px {color}55; letter-spacing: -2px; \
                                         padding: 6px 0 2px; transition: color 0.2s ease;"
                                    )>
                                        {format!("{:.3}V", display_v)}
                                    </div>
                                    <div style="text-align: center; font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 10px">
                                        {format!("code {}  ·  {:.2} mV/step", disp_code, ch.step_mv)}
                                        {if code_dirty[i].get() {
                                            view! { <span style="color: #f59e0b; font-weight: 700"> " preview"</span> }.into_any()
                                        } else { view! { <span></span> }.into_any() }}
                                    </div>

                                    // Voltage bar
                                    <div style="height: 4px; border-radius: 2px; background: rgba(255,255,255,0.08); margin-bottom: 4px; overflow: hidden">
                                        <div style=format!(
                                            "height: 100%; width: {pct:.1}%; background: {color}; \
                                             box-shadow: 0 0 10px {color}70; transition: width 0.35s ease;"
                                        )></div>
                                    </div>
                                    <div style="display: flex; justify-content: space-between; font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 12px">
                                        <span>{format!("{:.1}V", safe_min)}</span>
                                        <span>{format!("{:.1}V mid", ch.midpoint_v)}</span>
                                        <span>{format!("{:.1}V", safe_max)}</span>
                                    </div>

                                    // Code slider (right = higher V)
                                    <input type="range" class="slider slider-colored"
                                        style=format!("--slider-color: {color}; width: 100%; margin-bottom: 12px")
                                        min=-max_src max=max_sink step="1"
                                        prop:value=move || (-code_vals[i].get()).clamp(-max_src, max_sink)
                                        on:input=move |e| {
                                            if let Ok(v) = event_target_value(&e).parse::<i32>() {
                                                let c = v.clamp(-max_src, max_sink);
                                                code_vals[i].set(-c);
                                                code_dirty[i].set(true);
                                                dirty[i].set(true);
                                            }
                                        }
                                    />

                                    // Voltage input + SET
                                    <div style="display: flex; gap: 6px; align-items: center; margin-bottom: 10px">
                                        <input type="number" class="number-input" style="flex: 1; font-size: 12px"
                                            min=safe_min max=safe_max step="0.01"
                                            value=format!("{:.3}", display_v)
                                            on:change=move |e| {
                                                if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                    slider_vals[i].set(v);
                                                    dirty[i].set(true);
                                                    code_dirty[i].set(false);
                                                }
                                            }
                                        />
                                        <span style="font-size: 11px; color: var(--text-dim)">"V"</span>
                                        <button
                                            style=format!(
                                                "padding: 6px 18px; border-radius: 8px; \
                                                 border: 1px solid {color}40; background: {color}18; color: {color}; \
                                                 font-weight: 700; font-size: 12px; cursor: pointer; \
                                                 font-family: 'JetBrains Mono', monospace; letter-spacing: 0.05em; \
                                                 transition: background 0.2s ease;"
                                            )
                                            on:click=move |_| {
                                                if code_dirty[i].get_untracked() {
                                                    send_idac_code(ch_idx, code_vals[i].get_untracked() as i8);
                                                } else if dirty[i].get_untracked() {
                                                    let v = (slider_vals[i].get_untracked() as f32).clamp(safe_min, safe_max);
                                                    send_idac_voltage(ch_idx, v);
                                                }
                                                dirty[i].set(false);
                                                code_dirty[i].set(false);
                                            }
                                        >"SET"</button>
                                    </div>

                                    // Calibration badge
                                    <div style="border-top: 1px solid rgba(255,255,255,0.06); padding-top: 10px; display: flex; align-items: center; gap: 8px; font-size: 10px; font-family: 'JetBrains Mono', monospace">
                                        <span style="color: var(--text-dim)">"Cal:"</span>
                                        {if has_cal {
                                            view! { <span style="color: #10b981; font-weight: 600">"Active ✓"</span> }.into_any()
                                        } else {
                                            view! { <span style="color: var(--text-dim)">"Formula only"</span> }.into_any()
                                        }}
                                    </div>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                }.into_any()
            }}

            // ── SECTION: HAT Voltage Rails ─────────────────────────────────────
            <Show when=move || hat.get().detected>
                <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.1em; color: var(--text-dim); margin-bottom: 10px">
                    "HAT Voltage Rails"
                </div>
                <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; margin-bottom: 28px; animation: vt-slide-in 0.3s cubic-bezier(0.4,0,0.2,1)">

                    // 3V3_ADJ — rail 0
                    <div style=glass("#10b981")>
                        <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 14px">
                            <div>
                                <div style="font-size: 12px; font-weight: 700; color: #10b981; text-transform: uppercase; letter-spacing: 0.06em">"3V3_ADJ"</div>
                                <div style="font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">"1.7 – 5.0 V  ·  Level shifter power"</div>
                            </div>
                            <div style="display: flex; align-items: center; gap: 6px">
                                <label class="toggle-wrap">
                                    <div class="toggle" class:active=move || rail_en(0)
                                        on:click=move |_| {
                                            let cur = rail_en(0);
                                            spawn_local(async move {
                                                if let Some(r) = hat_set_rail_enable(0, !cur).await {
                                                    set_rails.set(r);
                                                } else {
                                                    show_toast("Failed to toggle 3V3_ADJ", "err");
                                                }
                                            });
                                        }
                                    ><div class="toggle-thumb"></div></div>
                                </label>
                                <span style="font-size: 10px; color: var(--text-dim)">
                                    {move || if rail_en(0) { "On" } else { "Off" }}
                                </span>
                            </div>
                        </div>

                        <div style="text-align: center; font-size: 36px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: #10b981; text-shadow: 0 0 24px #10b98155; letter-spacing: -2px; padding: 4px 0; transition: all 0.3s ease">
                            {move || {
                                if rail_en(0) { format!("{:.3}V", rail_mv(0) as f32 / 1000.0) }
                                else          { format!("{:.2}V", v3v3_target_mv.get() as f32 / 1000.0) }
                            }}
                        </div>
                        <div style="text-align: center; font-size: 9px; color: var(--text-dim); margin-bottom: 12px">
                            {move || if rail_en(0) { "live" } else { "preview" }}
                        </div>

                        <input type="range" class="slider slider-colored"
                            style="--slider-color: #10b981; width: 100%; margin-bottom: 8px"
                            min="1700" max="5000" step="100"
                            prop:value=move || v3v3_target_mv.get().to_string()
                            on:input=move |ev| {
                                let v = event_target_value(&ev).parse::<u16>().unwrap_or(3300);
                                set_v3v3_target_mv.set(v);
                            }
                        />
                        <div style="text-align: right; font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 8px">
                            {move || format!("{:.2} V target", v3v3_target_mv.get() as f32 / 1000.0)}
                        </div>
                        <button
                            style="width: 100%; padding: 7px; border-radius: 8px; border: 1px solid #10b98140; background: #10b98118; color: #10b981; font-weight: 700; font-size: 11px; cursor: pointer; letter-spacing: 0.05em; transition: background 0.2s ease"
                            on:click=move |_| {
                                let mv = v3v3_target_mv.get_untracked();
                                spawn_local(async move {
                                    if let Some(r) = hat_set_rail_voltage(0, mv).await {
                                        set_rails.set(r);
                                    } else {
                                        show_toast("Failed to set 3V3_ADJ", "err");
                                    }
                                });
                            }
                        >"CONFIRM"</button>
                    </div>

                    // VADJ3 — rail 1
                    <div style=glass("#06b6d4")>
                        <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 14px">
                            <div>
                                <div style="font-size: 12px; font-weight: 700; color: #06b6d4; text-transform: uppercase; letter-spacing: 0.06em">"VADJ3"</div>
                                <div style="font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">"1.8 – 36 V  ·  Connector 1"</div>
                            </div>
                            <div style="display: flex; align-items: center; gap: 6px">
                                <label class="toggle-wrap">
                                    <div class="toggle" class:active=move || rail_en(1)
                                        on:click=move |_| {
                                            let cur = rail_en(1);
                                            spawn_local(async move {
                                                if let Some(r) = hat_set_rail_enable(1, !cur).await {
                                                    set_rails.set(r);
                                                } else {
                                                    show_toast("Failed to toggle VADJ3", "err");
                                                }
                                            });
                                        }
                                    ><div class="toggle-thumb"></div></div>
                                </label>
                                <span style="font-size: 10px; color: var(--text-dim)">
                                    {move || if rail_en(1) { "On" } else { "Off" }}
                                </span>
                            </div>
                        </div>

                        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 12px">
                            <div style="text-align: center; padding: 10px; border-radius: 10px; background: #06b6d410">
                                <div style="font-size: 9px; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 3px">"Voltage"</div>
                                <div style="font-size: 22px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: #06b6d4; text-shadow: 0 0 16px #06b6d455">
                                    {move || {
                                        if rail_en(1) { format!("{:.2}V", rail_mv(1) as f32 / 1000.0) }
                                        else          { format!("{:.2}V", vadj3_target_mv.get() as f32 / 1000.0) }
                                    }}
                                </div>
                            </div>
                            <div style="text-align: center; padding: 10px; border-radius: 10px; background: #06b6d410">
                                <div style="font-size: 9px; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 3px">"Current"</div>
                                <div style="font-size: 22px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: #06b6d4">
                                    {move || format!("{}mA", rail_ma(1))}
                                </div>
                            </div>
                        </div>

                        <input type="range" class="slider slider-colored"
                            style="--slider-color: #06b6d4; width: 100%; margin-bottom: 8px"
                            min="0" max="36000" step="100"
                            prop:value=move || vadj3_target_mv.get().to_string()
                            on:input=move |ev| {
                                let v = event_target_value(&ev).parse::<u16>().unwrap_or(0);
                                set_vadj3_target_mv.set(v);
                            }
                        />
                        <div style="text-align: right; font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 8px">
                            {move || format!("{:.2} V target", vadj3_target_mv.get() as f32 / 1000.0)}
                        </div>
                        <button
                            style="width: 100%; padding: 7px; border-radius: 8px; border: 1px solid #06b6d440; background: #06b6d418; color: #06b6d4; font-weight: 700; font-size: 11px; cursor: pointer; letter-spacing: 0.05em; transition: background 0.2s ease"
                            on:click=move |_| {
                                let mv = vadj3_target_mv.get_untracked();
                                spawn_local(async move {
                                    if let Some(r) = hat_set_rail_voltage(1, mv).await {
                                        set_rails.set(r);
                                    } else {
                                        show_toast("Failed to set VADJ3", "err");
                                    }
                                });
                            }
                        >"CONFIRM"</button>
                    </div>

                    // VADJ4 — rail 2
                    <div style=glass("#ff4d6a")>
                        <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 14px">
                            <div>
                                <div style="font-size: 12px; font-weight: 700; color: #ff4d6a; text-transform: uppercase; letter-spacing: 0.06em">"VADJ4"</div>
                                <div style="font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">"1.8 – 36 V  ·  Connector 2"</div>
                            </div>
                            <div style="display: flex; align-items: center; gap: 6px">
                                <label class="toggle-wrap">
                                    <div class="toggle" class:active=move || rail_en(2)
                                        on:click=move |_| {
                                            let cur = rail_en(2);
                                            spawn_local(async move {
                                                if let Some(r) = hat_set_rail_enable(2, !cur).await {
                                                    set_rails.set(r);
                                                } else {
                                                    show_toast("Failed to toggle VADJ4", "err");
                                                }
                                            });
                                        }
                                    ><div class="toggle-thumb"></div></div>
                                </label>
                                <span style="font-size: 10px; color: var(--text-dim)">
                                    {move || if rail_en(2) { "On" } else { "Off" }}
                                </span>
                            </div>
                        </div>

                        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 12px">
                            <div style="text-align: center; padding: 10px; border-radius: 10px; background: #ff4d6a10">
                                <div style="font-size: 9px; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 3px">"Voltage"</div>
                                <div style="font-size: 22px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: #ff4d6a; text-shadow: 0 0 16px #ff4d6a55">
                                    {move || {
                                        if rail_en(2) { format!("{:.2}V", rail_mv(2) as f32 / 1000.0) }
                                        else          { format!("{:.2}V", vadj4_target_mv.get() as f32 / 1000.0) }
                                    }}
                                </div>
                            </div>
                            <div style="text-align: center; padding: 10px; border-radius: 10px; background: #ff4d6a10">
                                <div style="font-size: 9px; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 3px">"Current"</div>
                                <div style="font-size: 22px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: #ff4d6a">
                                    {move || format!("{}mA", rail_ma(2))}
                                </div>
                            </div>
                        </div>

                        <input type="range" class="slider slider-colored"
                            style="--slider-color: #ff4d6a; width: 100%; margin-bottom: 8px"
                            min="0" max="36000" step="100"
                            prop:value=move || vadj4_target_mv.get().to_string()
                            on:input=move |ev| {
                                let v = event_target_value(&ev).parse::<u16>().unwrap_or(0);
                                set_vadj4_target_mv.set(v);
                            }
                        />
                        <div style="text-align: right; font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 8px">
                            {move || format!("{:.2} V target", vadj4_target_mv.get() as f32 / 1000.0)}
                        </div>
                        <button
                            style="width: 100%; padding: 7px; border-radius: 8px; border: 1px solid #ff4d6a40; background: #ff4d6a18; color: #ff4d6a; font-weight: 700; font-size: 11px; cursor: pointer; letter-spacing: 0.05em; transition: background 0.2s ease"
                            on:click=move |_| {
                                let mv = vadj4_target_mv.get_untracked();
                                spawn_local(async move {
                                    if let Some(r) = hat_set_rail_voltage(2, mv).await {
                                        set_rails.set(r);
                                    } else {
                                        show_toast("Failed to set VADJ4", "err");
                                    }
                                });
                            }
                        >"CONFIRM"</button>
                    </div>

                </div>
            </Show>

            // ── SECTION: Calibration ───────────────────────────────────────────
            <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.1em; color: var(--text-dim); margin-bottom: 10px">
                "Calibration"
            </div>
            <div style="background: rgba(255,255,255,0.02); border: 1px solid rgba(255,255,255,0.07); border-radius: 16px; padding: 20px">

                // ── IDAC auto-calibration ──────────────────────────────────────
                <div style="font-size: 10px; font-weight: 700; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 12px">
                    "IDAC Auto-Calibration  ·  U23 Self-test MUX → AD74416H Ch D"
                </div>

                {move || match idac_cal_state.get() {
                    IdacCalState::Idle => {
                        let idac_st = idac.get();
                        view! {
                            <div>
                                <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin-bottom: 12px">
                                    // VLOGIC
                                    {
                                        let has_cal = idac_st.channels.first().map(|c| c.calibrated).unwrap_or(false);
                                        let mid     = idac_st.channels.first().map(|c| c.midpoint_v).unwrap_or(0.0);
                                        view! {
                                            <div style="background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.07); border-radius: 12px; border-left: 3px solid #10b981; padding: 14px">
                                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px">
                                                    <span style="font-weight: 700; color: #10b981; font-size: 13px">"VLOGIC"</span>
                                                    {if has_cal { view! { <span style="color: #10b981; font-size: 10px; font-weight: 600">"✓ Cal"</span> }.into_any() }
                                                     else        { view! { <span style="color: #f59e0b; font-size: 10px">"Uncal"</span> }.into_any() }}
                                                </div>
                                                <div style="font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 10px">
                                                    {format!("mid {:.2}V  ·  IDAC ch 0", mid)}
                                                </div>
                                                <button
                                                    style="width: 100%; padding: 7px; border-radius: 8px; border: 1px solid #10b98140; background: #10b98115; color: #10b981; font-weight: 700; font-size: 10px; cursor: pointer"
                                                    disabled=move || !idac.get().present
                                                    on:click=move |_| start_idac_cal(0, set_idac_cal_state, set_idac_cal_channel, set_idac_cal_points, set_idac_cal_last_v, set_idac_cal_error_mv, set_idac_last_points, set_idac_cal_log)
                                                >"Auto-Calibrate"</button>
                                            </div>
                                        }
                                    }
                                    // VADJ1
                                    {
                                        let has_cal = idac_st.channels.get(1).map(|c| c.calibrated).unwrap_or(false);
                                        let mid     = idac_st.channels.get(1).map(|c| c.midpoint_v).unwrap_or(0.0);
                                        view! {
                                            <div style="background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.07); border-radius: 12px; border-left: 3px solid #06b6d4; padding: 14px">
                                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px">
                                                    <span style="font-weight: 700; color: #06b6d4; font-size: 13px">"VADJ1"</span>
                                                    {if has_cal { view! { <span style="color: #10b981; font-size: 10px; font-weight: 600">"✓ Cal"</span> }.into_any() }
                                                     else        { view! { <span style="color: #f59e0b; font-size: 10px">"Uncal"</span> }.into_any() }}
                                                </div>
                                                <div style="font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 10px">
                                                    {format!("mid {:.2}V  ·  IDAC ch 1", mid)}
                                                </div>
                                                <button
                                                    style="width: 100%; padding: 7px; border-radius: 8px; border: 1px solid #06b6d440; background: #06b6d415; color: #06b6d4; font-weight: 700; font-size: 10px; cursor: pointer"
                                                    disabled=move || !idac.get().present
                                                    on:click=move |_| start_idac_cal(1, set_idac_cal_state, set_idac_cal_channel, set_idac_cal_points, set_idac_cal_last_v, set_idac_cal_error_mv, set_idac_last_points, set_idac_cal_log)
                                                >"Auto-Calibrate"</button>
                                            </div>
                                        }
                                    }
                                    // VADJ2
                                    {
                                        let has_cal = idac_st.channels.get(2).map(|c| c.calibrated).unwrap_or(false);
                                        let mid     = idac_st.channels.get(2).map(|c| c.midpoint_v).unwrap_or(0.0);
                                        view! {
                                            <div style="background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.07); border-radius: 12px; border-left: 3px solid #ff4d6a; padding: 14px">
                                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px">
                                                    <span style="font-weight: 700; color: #ff4d6a; font-size: 13px">"VADJ2"</span>
                                                    {if has_cal { view! { <span style="color: #10b981; font-size: 10px; font-weight: 600">"✓ Cal"</span> }.into_any() }
                                                     else        { view! { <span style="color: #f59e0b; font-size: 10px">"Uncal"</span> }.into_any() }}
                                                </div>
                                                <div style="font-size: 9px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 10px">
                                                    {format!("mid {:.2}V  ·  IDAC ch 2", mid)}
                                                </div>
                                                <button
                                                    style="width: 100%; padding: 7px; border-radius: 8px; border: 1px solid #ff4d6a40; background: #ff4d6a15; color: #ff4d6a; font-weight: 700; font-size: 10px; cursor: pointer"
                                                    disabled=move || !idac.get().present
                                                    on:click=move |_| start_idac_cal(2, set_idac_cal_state, set_idac_cal_channel, set_idac_cal_points, set_idac_cal_last_v, set_idac_cal_error_mv, set_idac_last_points, set_idac_cal_log)
                                                >"Auto-Calibrate"</button>
                                            </div>
                                        }
                                    }
                                </div>
                                {if !idac_st.present {
                                    view! {
                                        <div class="mode-warning">
                                            <span class="mode-warning-icon">"⚠"</span>
                                            <span>"DS4424 not detected. Check I2C connection."</span>
                                        </div>
                                    }.into_any()
                                } else {
                                    view! {
                                        <div style="font-size: 10px; color: var(--text-dim)">
                                            "Level shifter OE and all e-fuses are disabled automatically during calibration. Disconnect external loads first."
                                        </div>
                                    }.into_any()
                                }}
                            </div>
                        }.into_any()
                    }

                    IdacCalState::Running => {
                        let ch = idac_cal_channel.get();
                        let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "VLOGIC" };
                        view! {
                            <div style="text-align: center; padding: 16px 0">
                                <div style="font-size: 13px; font-weight: 700; color: #3b82f6; margin-bottom: 10px">
                                    {format!("Calibrating {} …", ch_name)}
                                </div>
                                <div style="width: 100%; height: 6px; background: rgba(255,255,255,0.08); border-radius: 3px; overflow: hidden; margin-bottom: 8px">
                                    <div style="height: 100%; width: 100%; background: linear-gradient(90deg, #1d4ed8, #3b82f6, #60a5fa, #3b82f6, #1d4ed8); background-size: 200% 100%; animation: vt-progress-sweep 1.5s linear infinite; box-shadow: 0 0 12px #3b82f680"></div>
                                </div>
                                <div style="font-size: 11px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 8px">
                                    {move || format!("{}/{} points  ·  {:.4} V", idac_cal_points.get(), CAL_TOTAL_POINTS, idac_cal_last_v.get())}
                                </div>
                                <div style="max-height: 100px; overflow-y: auto; background: rgba(0,0,0,0.35); border-radius: 8px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left">
                                    {move || idac_cal_log.get().iter().rev().take(10).map(|l| view! { <div>{l.clone()}</div> }).collect::<Vec<_>>()}
                                </div>
                            </div>
                        }.into_any()
                    }

                    IdacCalState::Complete => {
                        let ch   = idac_cal_channel.get();
                        let name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "VLOGIC" };
                        let pts  = idac_cal_points.get();
                        let err  = idac_cal_error_mv.get();
                        view! {
                            <div style="text-align: center; padding: 16px 0">
                                <div style="font-size: 13px; font-weight: 700; color: #10b981; margin-bottom: 6px">
                                    {format!("{} calibrated — {} pts  ·  {:.1} mV error", name, pts, err)}
                                </div>
                                <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 14px">
                                    "Saved to NVS flash. Level shifter and e-fuses restored."
                                </div>
                                <div style="display: flex; gap: 8px; justify-content: center">
                                    <button class="btn" on:click=move |_| set_idac_cal_state.set(IdacCalState::Idle)>"Done"</button>
                                    <button class="btn btn-primary" on:click=move |_| set_idac_cal_state.set(IdacCalState::Idle)>"Calibrate Another"</button>
                                </div>
                            </div>
                        }.into_any()
                    }

                    IdacCalState::Failed => {
                        view! {
                            <div style="text-align: center; padding: 16px 0">
                                <div style="font-size: 13px; font-weight: 700; color: #ef4444; margin-bottom: 6px">"Calibration Failed"</div>
                                <div style="max-height: 80px; overflow-y: auto; background: rgba(0,0,0,0.35); border-radius: 8px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left; margin-bottom: 14px">
                                    {move || idac_cal_log.get().iter().map(|l| view! { <div>{l.clone()}</div> }).collect::<Vec<_>>()}
                                </div>
                                <button class="btn" on:click=move |_| set_idac_cal_state.set(IdacCalState::Idle)>"Back"</button>
                            </div>
                        }.into_any()
                    }
                }}

                // ── HAT rail calibration (only when HAT detected) ──────────────
                <Show when=move || hat.get().detected>
                    <div style="border-top: 1px solid rgba(255,255,255,0.07); padding-top: 16px; margin-top: 16px">
                        <div style="font-size: 10px; font-weight: 700; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 12px">
                            "HAT Rail Calibration Sweep  ·  RP2040 DAC Sweep"
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
                                <div style="text-align: center; padding: 10px 0">
                                    <div style="font-size: 12px; font-weight: 700; color: #3b82f6; margin-bottom: 8px">
                                        {format!("Calibrating {} …  {}%", name, prog)}
                                    </div>
                                    <div style="width: 100%; height: 6px; background: rgba(255,255,255,0.08); border-radius: 3px; overflow: hidden; margin-bottom: 8px">
                                        <div style=format!("width: {}%; height: 100%; background: linear-gradient(90deg, #3b82f6, #8b5cf6); box-shadow: 0 0 10px #3b82f680; transition: width 0.35s ease", prog)></div>
                                    </div>
                                    <div style="font-size: 10px; color: var(--text-dim); display: flex; gap: 12px; justify-content: center; flex-wrap: wrap; font-family: 'JetBrains Mono', monospace">
                                        <span>{format!("stage: {}", stage)}</span>
                                        <span>{format!("code: {}", hat_cal_code.get())}</span>
                                        <span>{format!("pt: {}", hat_cal_point.get())}</span>
                                        <span>{if meas >= 0 { format!("{:.3}V", meas as f32 / 1000.0) } else { "—".into() }}</span>
                                    </div>
                                    {move || {
                                        let (label, color) = match hat_cal_persist_state.get() {
                                            1 => ("Saved to flash", "#10b981"),
                                            2 => ("Imported, not verified", "#3b82f6"),
                                            _ => ("RAM only — reboot will reset", "#f59e0b"),
                                        };
                                        view! { <div style=format!("margin-top: 6px; font-size: 9px; color: {color}")>{label}</div> }
                                    }}
                                </div>
                            }.into_any()
                        } else {
                            view! {
                                <div style="display: flex; gap: 10px; align-items: center; flex-wrap: wrap">
                                    <span style="font-size: 10px; color: var(--text-dim)">"Rail:"</span>
                                    <select
                                        style="background: var(--bg-secondary); border: 1px solid rgba(255,255,255,0.1); color: var(--text); border-radius: 6px; padding: 4px 8px; font-size: 11px"
                                        on:change=move |ev| {
                                            let val = event_target_value(&ev);
                                            if let Ok(id) = val.parse::<u8>() { set_hat_cal_rail_id.set(id); }
                                        }
                                    >
                                        <option value="1">"VADJ3 (1.8–36 V)"</option>
                                        <option value="2">"VADJ4 (1.8–36 V)"</option>
                                    </select>
                                    <button class="btn btn-primary" style="font-size: 11px; padding: 5px 16px"
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
                                    >"Start Sweep"</button>
                                    <button class="btn" style="font-size: 11px; padding: 5px 14px"
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
                                    >"Import JSON…"</button>
                                </div>
                            }.into_any()
                        }}
                    </div>
                </Show>

            </div>
        </div>
    }
}
