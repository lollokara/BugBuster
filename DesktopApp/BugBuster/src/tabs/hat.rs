use leptos::prelude::*;
use leptos::task::spawn_local;
use std::collections::VecDeque;
use std::time::Duration;
use wasm_bindgen::JsCast;
use wasm_bindgen::closure::Closure;
use wasm_bindgen::JsValue;
use crate::tauri_bridge::*;

// ── Local helpers ─────────────────────────────────────────────────────────────

#[component]
fn HatPill(label: &'static str, ok: bool, value: String) -> impl IntoView {
    view! {
        <span style="display: inline-flex; align-items: center; gap: 5px; font-size: 10px">
            <span style="color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.06em">{label}</span>
            <span style=if ok { "color: #10b981" } else { "color: var(--text-dim)" }>{value}</span>
        </span>
    }
}

// ── Main tab ──────────────────────────────────────────────────────────────────

#[component]
pub fn HatTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (hat, set_hat)   = signal(HatStatus::default());
    let (rails, set_rails) = signal(Vec::<HatRailStatus>::new());
    let (caps, set_caps) = signal(None::<HatCaps>);
    let (la_route, set_la_route_sig) = signal(0u8);

    let (io_dirs, set_io_dirs) = signal(0u8);
    let (io_ups,  set_io_ups)  = signal(0u8);
    let (io_dns,  set_io_dns)  = signal(0u8);

    let (ls_oe, set_ls_oe) = signal(false);
    let (ls_dir, set_ls_dir) = signal(false);

    let (cal_active,   set_cal_active)   = signal(false);
    let (cal_progress, set_cal_progress) = signal(0u8);
    let (cal_rail_id,  set_cal_rail_id)  = signal(1u8);
    let (_cal_state_val, set_cal_state_val) = signal(0u8);
    let (_cal_last_error, set_cal_last_error) = signal(0u8);
    let (cal_stage, set_cal_stage) = signal(0u8);
    let (cal_point, set_cal_point) = signal(0u8);
    let (cal_code, set_cal_code) = signal(0i8);
    let (cal_measured_mv, set_cal_measured_mv) = signal(-1i32);
    let (cal_persist_state, set_cal_persist_state) = signal(0u8);
    let (v3v3_target_mv, set_v3v3_target_mv) = signal(3300u16);
    let (vadj3_target_mv, set_vadj3_target_mv) = signal(3300u16);
    let (vadj4_target_mv, set_vadj4_target_mv) = signal(3300u16);
    let cal_poll_handle = RwSignal::new(None::<leptos::prelude::IntervalHandle>);

    let (log_enabled, set_log_enabled) = signal(false);
    let log_lines: RwSignal<VecDeque<String>> = RwSignal::new(VecDeque::new());

    // ── hat-log Tauri event listener ─────────────────────────────────────────
    Effect::new(move |_| {
        let log_lines = log_lines;
        spawn_local(async move {
            let closure = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
                if let Some(payload) = js_sys::Reflect::get(&event, &JsValue::from_str("payload"))
                    .ok().and_then(|v| v.as_string())
                {
                    log_lines.update(|lines| {
                        lines.push_back(payload);
                        if lines.len() > 200 { lines.pop_front(); }
                    });
                }
            });
            listen("hat-log", &closure).await;
            closure.forget();
        });
    });

    // ── Initial fetch ─────────────────────────────────────────────────────────
    Effect::new(move |_| {
        let _ = state.get();
        spawn_local(async move {
            if let Some(st) = fetch_hat_status().await    { set_hat.set(st); }
            if let Some(cp) = hat_get_caps().await        { set_caps.set(Some(cp)); }
            if let Some(rl) = hat_get_rail_status().await { set_rails.set(rl); }
        });
    });

    // ── 1 s status poll ───────────────────────────────────────────────────────
    Effect::new(move |_| {
        let handle = leptos::prelude::set_interval_with_handle(move || {
            spawn_local(async move {
                if let Some(st) = fetch_hat_status().await    { set_hat.set(st); }
                if let Some(rl) = hat_get_rail_status().await { set_rails.set(rl); }
            });
        }, Duration::from_secs(1)).ok();
        on_cleanup(move || { if let Some(h) = handle { h.clear(); } });
    });

    // ── Calibration poll (400 ms, only while running) ─────────────────────────
    Effect::new(move |_| {
        if cal_active.get() {
            if cal_poll_handle.get_untracked().is_none() {
                let handle = leptos::prelude::set_interval_with_handle(move || {
                    if !cal_active.get_untracked() { return; }
                    spawn_local(async move {
                        if let Some(s) = hat_calibrate_status().await {
                            set_cal_progress.set(s.progress);
                            set_cal_state_val.set(s.state);
                            set_cal_last_error.set(s.last_error);
                            set_cal_persist_state.set(s.persist_state);
                            set_cal_stage.set(s.stage);
                            set_cal_point.set(s.point);
                            set_cal_code.set(s.code);
                            set_cal_measured_mv.set(s.measured_mv);
                            if s.state == 2 {
                                set_cal_active.set(false);
                                show_toast("Calibration completed successfully!", "ok");
                            } else if s.state == 3 {
                                set_cal_active.set(false);
                                show_toast("Calibration failed!", "err");
                            }
                        }
                    });
                }, Duration::from_millis(400)).ok();
                cal_poll_handle.set(handle);
            }
        } else if let Some(h) = cal_poll_handle.get_untracked() {
            h.clear();
            cal_poll_handle.set(None);
        }
    });

    // ── Helpers to read individual rail fields without re-running stable DOM ──
    let rail_en  = move |id: u8| rails.get().iter().find(|r| r.rail_id == id).map(|r| r.enabled).unwrap_or(false);
    let rail_mv  = move |id: u8| rails.get().iter().find(|r| r.rail_id == id).map(|r| r.voltage_mv).unwrap_or(0);
    let rail_ma  = move |id: u8| rails.get().iter().find(|r| r.rail_id == id).map(|r| r.current_ma).unwrap_or(0);

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "HAT Expansion Board v2 — Advanced rail control, shifted I/O bank config, and high-speed routing."
            </div>

            // ── Summary Banner — re-renders only when hat/caps change ──────────
            {move || {
                let st = hat.get();
                let cp = caps.get();
                let fw  = format!("v{}.{}", st.fw_major, st.fw_minor);
                let rev = cp.as_ref().map(|c| format!("v{}", c.hw_revision)).unwrap_or("—".into());
                view! {
                    <div class="summary-banner" style="justify-content: space-between; padding: 8px 12px; margin-bottom: 14px">
                        <div style="display: flex; align-items: center; gap: 14px; flex-wrap: wrap">
                            <HatPill label="Detected" ok=st.detected    value={if st.detected      { "Yes".into() } else { "No".into() }} />
                            <HatPill label="UART"     ok=st.connected   value={if st.connected     { "OK".into()  } else { "—".into()  }} />
                            <HatPill label="DAP"      ok=st.dap_connected value={if st.dap_connected { "OK".into() } else { "—".into() }} />
                            <HatPill label="Target"   ok=st.target_detected value={if st.target_detected { "OK".into() } else { "—".into() }} />
                            <HatPill label="Rev"  ok=true value=rev />
                            <HatPill label="FW"   ok=true value=fw />
                        </div>
                        <button class="btn btn-sm" style="font-size: 10px; padding: 2px 10px"
                            on:click=move |_| {
                                spawn_local(async move {
                                    if let Some(s) = fetch_hat_status().await    { set_hat.set(s); }
                                    if let Some(rl) = hat_get_rail_status().await { set_rails.set(rl); }
                                });
                            }
                        >"Refresh"</button>
                    </div>
                }
            }}

            // ── "No HAT" warning — only shown when not detected ───────────────
            {move || if !hat.get().detected {
                view! {
                    <div class="card">
                        <div class="card-header"><span class="channel-func">"HAT v2"</span></div>
                        <div class="card-body">
                            <div class="mode-warning">
                                <span class="mode-warning-icon">"!"</span>
                                <span>"No HAT detected. Connect a HAT board to the expansion header and click Refresh."</span>
                            </div>
                        </div>
                    </div>
                }.into_any()
            } else {
                ().into_any()
            }}

            // ── All detail cards — mounted once when HAT first detected ────────
            // They use fine-grained signal reads internally, so stable DOM is
            // never torn down by the 1 s poll.
            <Show when=move || hat.get().detected>

                // Capabilities badges
                {move || {
                    if let Some(cp) = caps.get() {
                        let defs: &[(u32, &str, &str)] = &[
                            (1,  "#10b981", "Rails Control"),
                            (2,  "#3b82f6", "RGB LEDs"),
                            (4,  "#8b5cf6", "LA Low-Speed"),
                            (8,  "#a855f7", "LA High-Speed"),
                            (16, "#ec4899", "Shifted I/O"),
                        ];
                        let badges = defs.iter().filter(|(f,_,_)| cp.flags & f != 0).map(|(_, col, name)| {
                            let col = *col; let name = *name;
                            view! {
                                <span style=format!("background: {col}20; color: {col}; border: 1px solid {col}40; font-size: 9px; padding: 2px 7px; border-radius: 4px")>{name}</span>
                            }
                        }).collect::<Vec<_>>();
                        view! {
                            <div style="display: flex; align-items: center; gap: 6px; flex-wrap: wrap; margin-bottom: 12px">
                                <span style="font-size: 10px; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.06em">"Capabilities"</span>
                                {badges}
                            </div>
                        }.into_any()
                    } else { ().into_any() }
                }}

                // ── Row 1: Power Rails + Routing/SWD/Level-Shifter ────────────
                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 16px">

                    // Power Rails card
                    <div class="card">
                        <div class="card-header"><span>"Power Rails"</span></div>
                        <div class="card-body" style="display: flex; flex-direction: column; gap: 12px">

                            // 3V3_ADJ
                            <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                    <span style="display:flex; align-items:baseline; gap:5px">
                                        <span style="font-size: 11px; font-weight: 700; color: #10b981; text-transform: uppercase; letter-spacing: 0.05em">"3V3_ADJ"</span>
                                        <span style="font-size: 9px; color: var(--text-dim)">"(1.7–5.0 V)"</span>
                                    </span>
                                    <div style="display: flex; align-items: center; gap: 6px">
                                        <label class="toggle-wrap">
                                            <div class="toggle" class:active=move || rail_en(0)
                                                on:click=move |_| {
                                                    let cur = rail_en(0);
                                                    spawn_local(async move {
                                                        if let Some(r) = hat_set_rail_enable(0, !cur).await {
                                                            set_rails.set(r);
                                                        } else {
                                                            show_toast("Failed to toggle 3V3_ADJ rail", "err");
                                                        }
                                                    });
                                                }
                                            ><div class="toggle-thumb"></div></div>
                                        </label>
                                        <span style="font-size: 10px; color: var(--text-dim)">
                                            {move || if rail_en(0) { "Enabled" } else { "Disabled" }}
                                        </span>
                                    </div>
                                </div>

                                <div style="display: flex; justify-content: center; padding: 6px; margin-bottom: 8px; border-radius: 6px; background: #10b98110">
                                    <div style="text-align: center">
                                        <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 2px">
                                            {move || if rail_en(0) { "Current Target" } else { "Preview Target" }}
                                        </div>
                                        <div style="font-size: 18px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #10b981">
                                            {move || {
                                                if rail_en(0) {
                                                    format!("{:.3} V", rail_mv(0) as f32 / 1000.0)
                                                } else {
                                                    format!("{:.2} V (Preview)", v3v3_target_mv.get() as f32 / 1000.0)
                                                }
                                            }}
                                        </div>
                                    </div>
                                </div>

                                <div style="margin-top: 10px; display: grid; grid-template-columns: 1fr auto; gap: 8px; align-items: center">
                                    <input
                                        type="range"
                                        min="1700"
                                        max="5000"
                                        step="100"
                                        prop:value=move || v3v3_target_mv.get().to_string()
                                        on:input=move |ev| {
                                            let v = event_target_value(&ev).parse::<u16>().unwrap_or(3300);
                                            set_v3v3_target_mv.set(v);
                                        }
                                    />
                                    <div style="display: flex; gap: 8px; align-items: center">
                                        <span style="font-size: 10px; color: var(--text-dim); min-width: 50px; text-align: right">
                                            {move || format!("{:.2} V", v3v3_target_mv.get() as f32 / 1000.0)}
                                        </span>
                                        <button class="btn btn-primary" style="font-size: 10px; padding: 4px 10px"
                                            on:click=move |_| {
                                                let mv = v3v3_target_mv.get_untracked();
                                                spawn_local(async move {
                                                    if let Some(r) = hat_set_rail_voltage(0, mv).await {
                                                        set_rails.set(r);
                                                    } else {
                                                        show_toast("Failed to set 3V3_ADJ voltage", "err");
                                                    }
                                                });
                                            }
                                        >"Confirm"</button>
                                    </div>
                                </div>
                                <div style="font-size: 9px; color: var(--text-dim); margin-top: 6px">"Level-shifter power. OE requires this rail."</div>
                            </div>

                            // VADJ3
                            <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                    <span style="display:flex; align-items:baseline; gap:5px">
                                        <span style="font-size: 11px; font-weight: 700; color: #06b6d4; text-transform: uppercase; letter-spacing: 0.05em">"VADJ3"</span>
                                        <span style="font-size: 9px; color: var(--text-dim)">"(1.8–36 V)"</span>
                                    </span>
                                    <div style="display: flex; align-items: center; gap: 6px">
                                        <label class="toggle-wrap">
                                            <div class="toggle" class:active=move || rail_en(1)
                                                on:click=move |_| {
                                                    let cur = rail_en(1);
                                                    spawn_local(async move {
                                                        if let Some(r) = hat_set_rail_enable(1, !cur).await {
                                                            set_rails.set(r);
                                                        } else {
                                                            show_toast("Failed to toggle VADJ3 rail", "err");
                                                        }
                                                    });
                                                }
                                            ><div class="toggle-thumb"></div></div>
                                        </label>
                                        <span style="font-size: 10px; color: var(--text-dim)">
                                            {move || if rail_en(1) { "Enabled" } else { "Disabled" }}
                                        </span>
                                    </div>
                                </div>
                                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px">
                                    <div style="text-align: center; padding: 12px; border-radius: 8px; background: #06b6d410">
                                        <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"Voltage"</div>
                                        <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #06b6d4">
                                            {move || {
                                                if rail_en(1) {
                                                    format!("{:.3} V", rail_mv(1) as f32 / 1000.0)
                                                } else {
                                                    format!("{:.2} V (Preview)", vadj3_target_mv.get() as f32 / 1000.0)
                                                }
                                            }}
                                        </div>
                                    </div>
                                    <div style="text-align: center; padding: 12px; border-radius: 8px; background: #06b6d410">
                                        <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"Current"</div>
                                        <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #06b6d4">
                                            {move || format!("{} mA", rail_ma(1))}
                                        </div>
                                    </div>
                                </div>
                                <div style="margin-top: 10px; display: grid; grid-template-columns: 1fr auto; gap: 8px; align-items: center">
                                    <input
                                        type="range"
                                        min="0"
                                        max="36000"
                                        step="100"
                                        prop:value=move || vadj3_target_mv.get().to_string()
                                        on:input=move |ev| {
                                            let v = event_target_value(&ev).parse::<u16>().unwrap_or(0);
                                            set_vadj3_target_mv.set(v);
                                        }
                                    />
                                    <div style="display: flex; gap: 8px; align-items: center">
                                        <span style="font-size: 10px; color: var(--text-dim); min-width: 72px; text-align: right">
                                            {move || format!("{:.2} V", vadj3_target_mv.get() as f32 / 1000.0)}
                                        </span>
                                        <button class="btn btn-primary" style="font-size: 10px; padding: 4px 10px"
                                            on:click=move |_| {
                                                let mv = vadj3_target_mv.get_untracked();
                                                spawn_local(async move {
                                                    if let Some(r) = hat_set_rail_voltage(1, mv).await {
                                                        set_rails.set(r);
                                                    } else {
                                                        show_toast("Failed to set VADJ3 voltage", "err");
                                                    }
                                                });
                                            }
                                        >"Confirm"</button>
                                    </div>
                                </div>
                            </div>

                            // VADJ4
                            <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                    <span style="display:flex; align-items:baseline; gap:5px">
                                        <span style="font-size: 11px; font-weight: 700; color: #ff4d6a; text-transform: uppercase; letter-spacing: 0.05em">"VADJ4"</span>
                                        <span style="font-size: 9px; color: var(--text-dim)">"(1.8–36 V)"</span>
                                    </span>
                                    <div style="display: flex; align-items: center; gap: 6px">
                                        <label class="toggle-wrap">
                                            <div class="toggle" class:active=move || rail_en(2)
                                                on:click=move |_| {
                                                    let cur = rail_en(2);
                                                    spawn_local(async move {
                                                        if let Some(r) = hat_set_rail_enable(2, !cur).await {
                                                            set_rails.set(r);
                                                        } else {
                                                            show_toast("Failed to toggle VADJ4 rail", "err");
                                                        }
                                                    });
                                                }
                                            ><div class="toggle-thumb"></div></div>
                                        </label>
                                        <span style="font-size: 10px; color: var(--text-dim)">
                                            {move || if rail_en(2) { "Enabled" } else { "Disabled" }}
                                        </span>
                                    </div>
                                </div>
                                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px">
                                    <div style="text-align: center; padding: 12px; border-radius: 8px; background: #ff4d6a10">
                                        <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"Voltage"</div>
                                        <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #ff4d6a">
                                            {move || {
                                                if rail_en(2) {
                                                    format!("{:.3} V", rail_mv(2) as f32 / 1000.0)
                                                } else {
                                                    format!("{:.2} V (Preview)", vadj4_target_mv.get() as f32 / 1000.0)
                                                }
                                            }}
                                        </div>
                                    </div>
                                    <div style="text-align: center; padding: 12px; border-radius: 8px; background: #ff4d6a10">
                                        <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"Current"</div>
                                        <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #ff4d6a">
                                            {move || format!("{} mA", rail_ma(2))}
                                        </div>
                                    </div>
                                </div>
                                <div style="margin-top: 10px; display: grid; grid-template-columns: 1fr auto; gap: 8px; align-items: center">
                                    <input
                                        type="range"
                                        min="0"
                                        max="36000"
                                        step="100"
                                        prop:value=move || vadj4_target_mv.get().to_string()
                                        on:input=move |ev| {
                                            let v = event_target_value(&ev).parse::<u16>().unwrap_or(0);
                                            set_vadj4_target_mv.set(v);
                                        }
                                    />
                                    <div style="display: flex; gap: 8px; align-items: center">
                                        <span style="font-size: 10px; color: var(--text-dim); min-width: 72px; text-align: right">
                                            {move || format!("{:.2} V", vadj4_target_mv.get() as f32 / 1000.0)}
                                        </span>
                                        <button class="btn btn-primary" style="font-size: 10px; padding: 4px 10px"
                                            on:click=move |_| {
                                                let mv = vadj4_target_mv.get_untracked();
                                                spawn_local(async move {
                                                    if let Some(r) = hat_set_rail_voltage(2, mv).await {
                                                        set_rails.set(r);
                                                    } else {
                                                        show_toast("Failed to set VADJ4 voltage", "err");
                                                    }
                                                });
                                            }
                                        >"Confirm"</button>
                                    </div>
                                </div>
                            </div>

                        </div>
                    </div>

                    // Routing & SWD & Level Shifter
                    <div class="card">
                        <div class="card-header"><span>"Routing & SWD"</span></div>
                        <div class="card-body" style="display: flex; flex-direction: column; gap: 12px">

                            // LA Route selector
                            <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                                <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 8px">"LA Route"</div>
                                <div style="display: flex; gap: 8px">
                                    <button class="btn"
                                        style=move || format!("font-size: 10px; padding: 4px 10px; flex: 1{}",
                                            if la_route.get() == 0 { "; background: #3b82f630; color: #3b82f6; border: 1px solid #3b82f650" } else { "" })
                                        on:click=move |_| {
                                            spawn_local(async move {
                                                if let Some(r) = hat_la_set_route(0).await {
                                                    set_la_route_sig.set(r);
                                                    show_toast("Route → Low-Speed (Conn2)", "ok");
                                                }
                                            });
                                        }
                                    >"Low-Speed (Conn2)"</button>
                                    <button class="btn"
                                        style=move || format!("font-size: 10px; padding: 4px 10px; flex: 1{}",
                                            if la_route.get() == 1 { "; background: #3b82f630; color: #3b82f6; border: 1px solid #3b82f650" } else { "" })
                                        on:click=move |_| {
                                            spawn_local(async move {
                                                if let Some(r) = hat_la_set_route(1).await {
                                                    set_la_route_sig.set(r);
                                                    show_toast("Route → High-Speed (Conn1)", "ok");
                                                }
                                            });
                                        }
                                    >"High-Speed (Conn1)"</button>
                                </div>
                                <div style="font-size: 9px; color: var(--text-dim); margin-top: 6px">
                                    {move || if la_route.get() == 0 {
                                        "EXP_EXT pins — up to 4 ch @ 1 MHz max."
                                    } else {
                                        "Low-skew buffered Conn1 — up to 3 ch."
                                    }}
                                </div>
                            </div>

                            // SWD target
                            <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                                <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 8px">"SWD Target"</div>
                                <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 8px">
                                    {move || {
                                        let st = hat.get();
                                        let dot_style = if st.target_detected {
                                            "width:9px;height:9px;border-radius:50%;flex-shrink:0;background:#f59e0b;box-shadow:0 0 6px #f59e0b"
                                        } else {
                                            "width:9px;height:9px;border-radius:50%;flex-shrink:0;background:var(--text-dim)"
                                        };
                                        let label = if st.target_detected {
                                            format!("DPIDR 0x{:08X}", st.target_dpidr)
                                        } else {
                                            "No target".into()
                                        };
                                        view! {
                                            <div style=dot_style></div>
                                            <span style="font-size: 11px; font-family: 'JetBrains Mono', monospace">{label}</span>
                                        }
                                    }}
                                </div>
                                <div style="display: flex; gap: 6px">
                                    <button class="btn" style="font-size: 10px; padding: 4px 10px; flex: 1; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                        on:click=move |_| { send_hat_setup_swd(3300, 0); }
                                    >"Setup 3.3 V"</button>
                                    <button class="btn" style="font-size: 10px; padding: 4px 10px; flex: 1; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                        on:click=move |_| { send_hat_setup_swd(1800, 0); }
                                    >"Setup 1.8 V"</button>
                                </div>
                            </div>

                            // Level Shifter
                            <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                                <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 8px">"Level Shifter"</div>

                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                    <div>
                                        <div style="font-size: 11px; font-weight: 600">"Outputs Enable (OE)"</div>
                                        <div style="font-size: 9px; color: var(--text-dim)">"Requires 3V3_ADJ."</div>
                                    </div>
                                    <div style="display: flex; align-items: center; gap: 6px">
                                        <label class="toggle-wrap">
                                            <div class="toggle" class:active=move || ls_oe.get()
                                                on:click=move |_| {
                                                    spawn_local(async move {
                                                        let next = !ls_oe.get_untracked();
                                                        let dir  = ls_dir.get_untracked();
                                                        if let Some(s) = hat_set_level_shift(next, dir).await {
                                                            set_ls_oe.set(s.oe);
                                                            set_ls_dir.set(s.dir);
                                                            show_toast(if s.oe { "Outputs enabled" } else { "Outputs tri-stated" }, "ok");
                                                        }
                                                    });
                                                }
                                            ><div class="toggle-thumb"></div></div>
                                        </label>
                                        <span style="font-size: 10px; color: var(--text-dim)">
                                            {move || if ls_oe.get() { "Active" } else { "Tri-State" }}
                                        </span>
                                    </div>
                                </div>

                                <div style="display: flex; justify-content: space-between; align-items: center">
                                    <div>
                                        <div style="font-size: 11px; font-weight: 600">"Direction (DIR)"</div>
                                        <div style="font-size: 9px; color: var(--text-dim)">"A→B drives; B→A listens."</div>
                                    </div>
                                    <div style="display: flex; align-items: center; gap: 6px">
                                        <label class="toggle-wrap">
                                            <div class="toggle" class:active=move || ls_dir.get()
                                                on:click=move |_| {
                                                    spawn_local(async move {
                                                        let oe   = ls_oe.get_untracked();
                                                        let next = !ls_dir.get_untracked();
                                                        if let Some(s) = hat_set_level_shift(oe, next).await {
                                                            set_ls_oe.set(s.oe);
                                                            set_ls_dir.set(s.dir);
                                                        }
                                                    });
                                                }
                                            ><div class="toggle-thumb"></div></div>
                                        </label>
                                        <span style="font-size: 10px; color: var(--text-dim)">
                                            {move || if ls_dir.get() { "A→B" } else { "B→A" }}
                                        </span>
                                    </div>
                                </div>
                            </div>

                        </div>
                    </div>
                </div>

                // ── Shifted I/O Bank ──────────────────────────────────────────
                <div class="card" style="margin-bottom: 16px">
                    <div class="card-header">
                        <span>"Shifted I/O Bank"</span>
                        <span style="font-size: 10px; color: var(--text-dim)">"GPIO 10–15, 20–21"</span>
                    </div>
                    <div class="card-body">
                        <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; margin-bottom: 12px">
                            {(0..8u8).map(|i| {
                                let is_out = Signal::derive(move || (io_dirs.get() & (1 << i)) != 0);
                                let is_up  = Signal::derive(move || (io_ups.get()  & (1 << i)) != 0);
                                let is_dn  = Signal::derive(move || (io_dns.get()  & (1 << i)) != 0);
                                view! {
                                    <div style="padding: 8px; border-radius: 6px; background: var(--bg-secondary); border: 1px solid var(--border-color, #333)">
                                        <div style="font-size: 10px; font-weight: 700; color: #3b82f6; text-transform: uppercase; letter-spacing: 0.04em; margin-bottom: 5px">
                                            {format!("SH_IO_{}", i + 1)}
                                        </div>
                                        <div style="display: flex; gap: 3px; margin-bottom: 5px">
                                            <button class="btn btn-sm"
                                                style=move || format!("padding: 1px 4px; font-size: 9px; flex: 1{}",
                                                    if !is_out.get() { "; background: #3b82f640; color:#3b82f6" } else { "" })
                                                on:click=move |_| { set_io_dirs.update(|d| *d &= !(1 << i)); }
                                            >"IN"</button>
                                            <button class="btn btn-sm"
                                                style=move || format!("padding: 1px 4px; font-size: 9px; flex: 1{}",
                                                    if is_out.get() { "; background: #3b82f640; color:#3b82f6" } else { "" })
                                                on:click=move |_| { set_io_dirs.update(|d| *d |= 1 << i); }
                                            >"OUT"</button>
                                        </div>
                                        <div style="display: flex; flex-direction: column; gap: 2px">
                                            <label style="font-size: 9px; display: flex; align-items: center; gap: 3px; cursor: pointer">
                                                <input type="checkbox" prop:checked=move || is_up.get()
                                                    on:change=move |ev| {
                                                        let chk = ev.target().unwrap()
                                                            .unchecked_into::<web_sys::HtmlInputElement>().checked();
                                                        if chk {
                                                            set_io_ups.update(|u| *u |= 1 << i);
                                                            set_io_dns.update(|d| *d &= !(1 << i));
                                                        } else {
                                                            set_io_ups.update(|u| *u &= !(1 << i));
                                                        }
                                                    }
                                                />
                                                "Pull-Up"
                                            </label>
                                            <label style="font-size: 9px; display: flex; align-items: center; gap: 3px; cursor: pointer">
                                                <input type="checkbox" prop:checked=move || is_dn.get()
                                                    on:change=move |ev| {
                                                        let chk = ev.target().unwrap()
                                                            .unchecked_into::<web_sys::HtmlInputElement>().checked();
                                                        if chk {
                                                            set_io_dns.update(|d| *d |= 1 << i);
                                                            set_io_ups.update(|u| *u &= !(1 << i));
                                                        } else {
                                                            set_io_dns.update(|d| *d &= !(1 << i));
                                                        }
                                                    }
                                                />
                                                "Pull-Down"
                                            </label>
                                        </div>
                                    </div>
                                }
                            }).collect::<Vec<_>>()}
                        </div>
                        <button class="btn btn-primary" style="width: 100%; font-size: 11px; padding: 6px"
                            on:click=move |_| {
                                spawn_local(async move {
                                    let d  = io_dirs.get_untracked();
                                    let u  = io_ups.get_untracked();
                                    let dn = io_dns.get_untracked();
                                    if hat_set_io_bank(d, u, dn).await.is_some() {
                                        show_toast("I/O bank configuration applied!", "ok");
                                    }
                                });
                            }
                        >"Apply I/O Bank Config"</button>
                    </div>
                </div>

                // ── DS4424 Calibration ────────────────────────────────────────
                // NOTE: This card is outside the 1 s poll reactive block so the
                // <select> is never destroyed while the user has it open.
                <div class="card" style="margin-bottom: 16px">
                    <div class="card-header"><span>"DS4424 Calibration Sweep"</span></div>
                    <div class="card-body">
                        {move || if cal_active.get() {
                            let prog = cal_progress.get();
                            let name = if cal_rail_id.get() == 1 { "VADJ3" } else { "VADJ4" };
                            let stage = match cal_stage.get() {
                                1 => "prepare",
                                2 => "step",
                                3 => "settle",
                                4 => "measure",
                                5 => "done",
                                8 => "error",
                                _ => "idle",
                            };
                            let measured = cal_measured_mv.get();
                            view! {
                                <div style="padding: 12px 0; text-align: center">
                                    <div style="font-size: 12px; font-weight: 700; color: #3b82f6; margin-bottom: 8px">
                                        {format!("Calibrating {} …", name)}
                                    </div>
                                    <div style="width: 100%; height: 6px; background: var(--bg-secondary); border-radius: 3px; overflow: hidden; margin-bottom: 6px">
                                        <div style=format!("width: {}%; height: 100%; background: #3b82f6; transition: width 0.3s ease", prog)></div>
                                    </div>
                                    <div style="font-size: 10px; color: var(--text-dim); display: flex; gap: 10px; justify-content: center; flex-wrap: wrap">
                                        <span>{format!("{}% complete", prog)}</span>
                                        <span>{format!("stage: {}", stage)}</span>
                                        <span>{format!("code: {}", cal_code.get())}</span>
                                        <span>{if measured >= 0 { format!("measured: {:.3} V", measured as f32 / 1000.0) } else { "measured: —".into() }}</span>
                                        <span>{format!("point: {}", cal_point.get())}</span>
                                    </div>
                                </div>
                            }.into_any()
                        } else {
                            view! {
                                <div style="display: flex; gap: 10px; align-items: center; flex-wrap: wrap">
                                    <span style="font-size: 11px; color: var(--text-dim)">"Rail:"</span>
                                    <select
                                        style="background: var(--bg-secondary); border: 1px solid var(--border-color); color: var(--text); border-radius: 4px; padding: 4px 8px; font-size: 11px"
                                        on:change=move |ev| {
                                            let val = event_target_value(&ev);
                                            if let Ok(id) = val.parse::<u8>() { set_cal_rail_id.set(id); }
                                        }
                                    >
                                        <option value="1">"VADJ3 (1.8–36 V)"</option>
                                        <option value="2">"VADJ4 (1.8–36 V)"</option>
                                    </select>
                                    <button class="btn btn-primary" style="font-size: 11px; padding: 4px 16px"
                                        on:click=move |_| {
                                            spawn_local(async move {
                                                let id = cal_rail_id.get_untracked();
                                                if let Some(code) = hat_calibrate_start(id).await {
                                                    if code == 1 {
                                                        set_cal_active.set(true);
                                                        set_cal_progress.set(0);
                                                        show_toast("Calibration sweep started!", "ok");
                                                    } else {
                                                        show_toast(&format!("Calibration failed to start (code {})", code), "err");
                                                    }
                                                }
                                            });
                                        }
                                    >"Start Sweep"</button>
                                </div>
                            }.into_any()
                        }}
                    </div>
                </div>

                // ── RP2040 Debug Logs ─────────────────────────────────────────
                <div class="card">
                    <div class="card-header">
                        <span>"RP2040 Debug Logs"</span>
                        <div style="display: flex; align-items: center; gap: 8px">
                            <label class="toggle-wrap">
                                <div class="toggle" class:active=move || log_enabled.get()
                                    on:click=move |_| {
                                        let new_val = !log_enabled.get_untracked();
                                        spawn_local(async move {
                                            if hat_la_log_enable(new_val).await.is_some() {
                                                set_log_enabled.set(new_val);
                                            } else {
                                                show_toast("Failed to toggle log relay", "err");
                                            }
                                        });
                                    }
                                ><div class="toggle-thumb"></div></div>
                            </label>
                            <span style="font-size: 10px; color: var(--text-dim)">
                                {move || if log_enabled.get() { "Streaming" } else { "Off" }}
                            </span>
                            <button class="btn btn-sm" style="font-size: 10px; padding: 2px 8px"
                                on:click=move |_| { log_lines.update(|l| l.clear()); }
                            >"Clear"</button>
                        </div>
                    </div>
                    <div class="card-body" style="padding-top: 0">
                        <pre style="font-size: 10px; font-family: 'JetBrains Mono', monospace; background: var(--bg-secondary); border-radius: 6px; padding: 10px; max-height: 200px; overflow-y: auto; white-space: pre-wrap; word-break: break-all; color: var(--text-dim); margin: 0">
                            {move || {
                                let lines = log_lines.get();
                                if lines.is_empty() {
                                    "(no log output — enable relay above)".to_string()
                                } else {
                                    lines.iter().cloned().collect::<Vec<_>>().join("\n")
                                }
                            }}
                        </pre>
                    </div>
                </div>

            </Show>
        </div>
    }
}
