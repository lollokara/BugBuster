use leptos::prelude::*;
use leptos::task::spawn_local;
use std::collections::VecDeque;
use std::time::Duration;
use wasm_bindgen::JsCast;
use wasm_bindgen::closure::Closure;
use wasm_bindgen::JsValue;
use crate::tauri_bridge::*;
use crate::components::controls::Dropdown;

const IO_VOLTAGE_OPTIONS: &[(&str, &str)] = &[
    ("1200", "1.2V"),
    ("1800", "1.8V"),
    ("2500", "2.5V"),
    ("3300", "3.3V"),
    ("5000", "5.0V"),
];

fn status_dot(active: bool, color: &str) -> String {
    format!("width: 10px; height: 10px; border-radius: 50%; display: inline-block; {}",
        if active { format!("background: {}; box-shadow: 0 0 6px {}", color, color) }
        else { "background: var(--text-dim)".into() })
}

#[component]
pub fn HatTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (hat, set_hat) = signal(HatStatus::default());
    let (rails, set_rails) = signal(Vec::<HatRailStatus>::new());
    let (caps, set_caps) = signal(None::<HatCaps>);
    let (la_route, set_la_route_sig) = signal(0u8);

    // Shifted IO Bank States
    let (io_dirs, set_io_dirs) = signal(0u8);
    let (io_ups, set_io_ups) = signal(0u8);
    let (io_dns, set_io_dns) = signal(0u8);

    // Level Shifter States
    let (ls_oe, set_ls_oe) = signal(false);
    let (ls_dir, set_ls_dir) = signal(false);

    // Calibration States
    let (cal_active, set_cal_active) = signal(false);
    let (cal_progress, set_cal_progress) = signal(0u8);
    let (cal_rail_id, set_cal_rail_id) = signal(1u8); // 1 = VADJ3, 2 = VADJ4
    let (_cal_state_val, set_cal_state_val) = signal(0u8); // 0=idle, 1=running, 2=done, 3=failed
    let (_cal_last_error, set_cal_last_error) = signal(0u8);
    let cal_poll_handle = RwSignal::new(None::<leptos::prelude::IntervalHandle>);

    // HAT Log States
    let (log_enabled, set_log_enabled) = signal(false);
    let log_lines: RwSignal<VecDeque<String>> = RwSignal::new(VecDeque::new());

    // Set up the "hat-log" Tauri event listener once
    Effect::new(move |_| {
        let log_lines = log_lines;
        spawn_local(async move {
            let closure = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
                if let Some(payload) = js_sys::Reflect::get(&event, &JsValue::from_str("payload"))
                    .ok()
                    .and_then(|v| v.as_string())
                {
                    log_lines.update(|lines| {
                        lines.push_back(payload);
                        if lines.len() > 200 {
                            lines.pop_front();
                        }
                    });
                }
            });
            listen("hat-log", &closure).await;
            closure.forget();
        });
    });

    // Fetch initial status and capabilities
    let set_hat_clone = set_hat;
    let set_caps_clone = set_caps;
    let set_rails_clone = set_rails;
    Effect::new(move |_| {
        let _ = state.get();
        spawn_local(async move {
            if let Some(st) = fetch_hat_status().await {
                set_hat_clone.set(st);
            }
            if let Some(cp) = hat_get_caps().await {
                set_caps_clone.set(Some(cp));
            }
            if let Some(rl) = hat_get_rail_status().await {
                set_rails_clone.set(rl);
            }
        });
    });

    // Periodic status polling (every 1s)
    let set_hat_poll = set_hat;
    let set_rails_poll = set_rails;
    Effect::new(move |_| {
        let handle = leptos::prelude::set_interval_with_handle(
            move || {
                spawn_local(async move {
                    if let Some(st) = fetch_hat_status().await {
                        set_hat_poll.set(st);
                    }
                    if let Some(rl) = hat_get_rail_status().await {
                        set_rails_poll.set(rl);
                    }
                });
            },
            Duration::from_secs(1),
        ).ok();
        
        on_cleanup(move || {
            if let Some(h) = handle {
                h.clear();
            }
        });
    });

    // Calibration Polling Effect
    Effect::new(move |_| {
        let running = cal_active.get();
        if running {
            if cal_poll_handle.get_untracked().is_none() {
                let handle = leptos::prelude::set_interval_with_handle(
                    move || {
                        if !cal_active.get_untracked() {
                            return;
                        }
                        spawn_local(async move {
                            if let Some(status) = hat_calibrate_status().await {
                                set_cal_progress.set(status.progress);
                                set_cal_state_val.set(status.state);
                                set_cal_last_error.set(status.last_error);

                                if status.state == 2 {
                                    set_cal_active.set(false);
                                    show_toast("Calibration completed successfully!", "ok");
                                } else if status.state == 3 {
                                    set_cal_active.set(false);
                                    show_toast("Calibration failed!", "err");
                                }
                            }
                        });
                    },
                    Duration::from_millis(400),
                ).ok();
                cal_poll_handle.set(handle);
            }
        } else if let Some(h) = cal_poll_handle.get_untracked() {
            h.clear();
            cal_poll_handle.set(None);
        }
    });

    let io_volt_options: Vec<(String, String)> = IO_VOLTAGE_OPTIONS
        .iter().map(|(v, l)| (v.to_string(), l.to_string())).collect();

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "HAT Expansion Board v2 — Advanced rail control, shifted I/O bank config, and high-speed routing."
            </div>

            {move || {
                let st = hat.get();
                let cp_opt = caps.get();
                let rail_list = rails.get();

                // ── Status Bar ──
                let status_card = view! {
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-header">
                            <span>"HAT Status"</span>
                            <div style="display: flex; gap: 8px">
                                <button class="btn btn-sm" style="font-size: 10px; padding: 2px 8px"
                                    on:click=move |_| {
                                        spawn_local(async move {
                                            if let Some(s) = fetch_hat_status().await { set_hat.set(s); }
                                            if let Some(rl) = hat_get_rail_status().await { set_rails.set(rl); }
                                        });
                                    }
                                >"Refresh"</button>
                            </div>
                        </div>
                        <div class="card-body">
                            <div style="display: grid; grid-template-columns: repeat(6, 1fr); gap: 10px">
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"Detected"</div>
                                    <div style=status_dot(st.detected, "#10b981")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"UART"</div>
                                    <div style=status_dot(st.connected, "#3b82f6")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"DAP"</div>
                                    <div style=status_dot(st.dap_connected, "#8b5cf6")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"Target"</div>
                                    <div style=status_dot(st.target_detected, "#f59e0b")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"Revision"</div>
                                    <div style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                        {if let Some(ref c) = cp_opt { format!("v{}", c.hw_revision) } else { "-".into() }}
                                    </div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"Firmware"</div>
                                    <div style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                        {format!("v{}.{}", st.fw_major, st.fw_minor)}
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                };

                if !st.detected {
                    return view! {
                        {status_card}
                        <div class="card">
                            <div class="card-body" style="text-align: center; padding: 32px; color: var(--text-dim)">
                                <div style="font-size: 24px; margin-bottom: 8px">"No HAT Detected"</div>
                                <div style="font-size: 12px">"Connect a HAT board to the expansion header and click Refresh."</div>
                            </div>
                        </div>
                    }.into_any();
                }

                // Capabilities list
                let caps_view = if let Some(ref cp) = cp_opt {
                    let mut badges = Vec::new();
                    if cp.flags & 1 != 0 { badges.push(view! { <span class="badge" style="background: #10b98120; color: #10b981; border: 1px solid #10b98140; margin-right: 4px; font-size: 9px; padding: 2px 6px; border-radius: 4px">"Rails Control"</span> }); }
                    if cp.flags & 2 != 0 { badges.push(view! { <span class="badge" style="background: #3b82f620; color: #3b82f6; border: 1px solid #3b82f640; margin-right: 4px; font-size: 9px; padding: 2px 6px; border-radius: 4px">"RGB Status LEDs"</span> }); }
                    if cp.flags & 4 != 0 { badges.push(view! { <span class="badge" style="background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf640; margin-right: 4px; font-size: 9px; padding: 2px 6px; border-radius: 4px">"LA Route Low-Speed"</span> }); }
                    if cp.flags & 8 != 0 { badges.push(view! { <span class="badge" style="background: #a855f720; color: #a855f7; border: 1px solid #a855f740; margin-right: 4px; font-size: 9px; padding: 2px 6px; border-radius: 4px">"LA Route High-Speed"</span> }); }
                    if cp.flags & 16 != 0 { badges.push(view! { <span class="badge" style="background: #ec489920; color: #ec4899; border: 1px solid #ec489940; margin-right: 4px; font-size: 9px; padding: 2px 6px; border-radius: 4px">"Shifted I/O Bank"</span> }); }
                    view! {
                        <div style="margin-bottom: 12px">
                            <span style="font-size: 10px; color: var(--text-dim); margin-right: 8px">"Capabilities:"</span>
                            {badges}
                        </div>
                    }.into_any()
                } else {
                    view! { <div></div> }.into_any()
                };

                // Power rails rows mapping
                let rail_v3 = rail_list.iter().find(|r| r.rail_id == 1); // VADJ3
                let rail_v4 = rail_list.iter().find(|r| r.rail_id == 2); // VADJ4
                let rail_adj = rail_list.iter().find(|r| r.rail_id == 0); // 3V3_ADJ

                let v3_en = rail_v3.map(|r| r.enabled).unwrap_or(false);
                let v4_en = rail_v4.map(|r| r.enabled).unwrap_or(false);
                let adj_en = rail_adj.map(|r| r.enabled).unwrap_or(false);

                let v3_mv = rail_v3.map(|r| r.voltage_mv).unwrap_or(0);
                let v4_mv = rail_v4.map(|r| r.voltage_mv).unwrap_or(0);

                let v3_ma = rail_v3.map(|r| r.current_ma).unwrap_or(0);
                let v4_ma = rail_v4.map(|r| r.current_ma).unwrap_or(0);

                let is_cal_running = cal_active.get();
                let cal_prog_val = cal_progress.get();

                let io_opts = io_volt_options.clone();

                view! {
                    {status_card}
                    {caps_view}

                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 16px">
                        
                        // ── Power Rails Card ──
                        <div class="card">
                            <div class="card-header"><span>"Power Rails"</span></div>
                            <div class="card-body">
                                
                                // 3V3_ADJ Rail
                                <div style="margin-bottom: 12px; padding: 10px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px">
                                        <span style="font-size: 12px; font-weight: 700; color: #10b981">"3V3_ADJ Rail (Level Shifter Power)"</span>
                                        <button
                                            class="btn btn-sm"
                                            style=format!("font-size: 10px; padding: 2px 10px; {}",
                                                if adj_en { "background: #10b98130; color: #10b981; border: 1px solid #10b98150" }
                                                else { "" })
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    if let Some(r) = hat_set_rail_enable(0, !adj_en).await {
                                                        set_rails.set(r);
                                                    }
                                                });
                                            }
                                        >{if adj_en { "ON" } else { "OFF" }}</button>
                                    </div>
                                    <div style="font-size: 10px; color: var(--text-dim)">
                                        "Required for level shifter Outputs Enable (OE). Hard interlocked."
                                    </div>
                                </div>

                                // VADJ3 Rail
                                <div style="margin-bottom: 12px; padding: 10px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                        <span style="font-size: 12px; font-weight: 700; color: #06b6d4">"VADJ3 Rail (Adjustable 0–36V)"</span>
                                        <button
                                            class="btn btn-sm"
                                            style=format!("font-size: 10px; padding: 2px 10px; {}",
                                                if v3_en { "background: #10b98130; color: #10b981; border: 1px solid #10b98150" }
                                                else { "" })
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    if let Some(r) = hat_set_rail_enable(1, !v3_en).await {
                                                        set_rails.set(r);
                                                    }
                                                });
                                            }
                                        >{if v3_en { "ON" } else { "OFF" }}</button>
                                    </div>
                                    <div style="display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; align-items: center">
                                        <div>
                                            <div style="font-size: 9px; color: var(--text-dim)">"Target"</div>
                                            <Dropdown
                                                value=Signal::derive(move || st.io_voltage_mv.to_string())
                                                on_change=Callback::new(move |val: String| {
                                                    if let Ok(mv) = val.parse::<u16>() {
                                                        send_hat_set_io_voltage(mv);
                                                    }
                                                })
                                                options=io_opts.clone()
                                            />
                                        </div>
                                        <div>
                                            <div style="font-size: 9px; color: var(--text-dim)">"Voltage"</div>
                                            <span style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                                {format!("{:.3} V", v3_mv as f32 / 1000.0)}
                                            </span>
                                        </div>
                                        <div>
                                            <div style="font-size: 9px; color: var(--text-dim)">"Current"</div>
                                            <span style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                                {format!("{} mA", v3_ma)}
                                            </span>
                                        </div>
                                    </div>
                                </div>

                                // VADJ4 Rail
                                <div style="padding: 10px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                        <span style="font-size: 12px; font-weight: 700; color: #ff4d6a">"VADJ4 Rail (Adjustable 0–36V)"</span>
                                        <button
                                            class="btn btn-sm"
                                            style=format!("font-size: 10px; padding: 2px 10px; {}",
                                                if v4_en { "background: #10b98130; color: #10b981; border: 1px solid #10b98150" }
                                                else { "" })
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    if let Some(r) = hat_set_rail_enable(2, !v4_en).await {
                                                        set_rails.set(r);
                                                    }
                                                });
                                            }
                                        >{if v4_en { "ON" } else { "OFF" }}</button>
                                    </div>
                                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px">
                                        <div>
                                            <div style="font-size: 9px; color: var(--text-dim)">"Voltage"</div>
                                            <span style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                                {format!("{:.3} V", v4_mv as f32 / 1000.0)}
                                            </span>
                                        </div>
                                        <div>
                                            <div style="font-size: 9px; color: var(--text-dim)">"Current"</div>
                                            <span style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                                {format!("{} mA", v4_ma)}
                                            </span>
                                        </div>
                                    </div>
                                </div>

                            </div>
                        </div>

                        // ── Logic Analyzer Route & SWD Card ──
                        <div class="card">
                            <div class="card-header"><span>"Routing & SWD"</span></div>
                            <div class="card-body">
                                
                                // Route selection
                                <div style="padding: 10px; border-radius: 6px; background: var(--bg-secondary); margin-bottom: 12px">
                                    <span style="font-size: 12px; font-weight: 700; display: block; margin-bottom: 6px">"LA Route Selector"</span>
                                    <div style="display: flex; gap: 8px">
                                        <button
                                            class="btn"
                                            style=format!("font-size: 10px; padding: 4px 12px; flex: 1; {}",
                                                if la_route.get() == 0 { "background: #3b82f630; color: #3b82f6; border: 1px solid #3b82f650" } else { "" })
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    if let Some(r) = hat_la_set_route(0).await {
                                                        set_la_route_sig.set(r);
                                                        show_toast("Route set to Low-Speed (Conn2)", "ok");
                                                    }
                                                });
                                            }
                                        >"Low-Speed Route (Conn2)"</button>
                                        <button
                                            class="btn"
                                            style=format!("font-size: 10px; padding: 4px 12px; flex: 1; {}",
                                                if la_route.get() == 1 { "background: #3b82f630; color: #3b82f6; border: 1px solid #3b82f650" } else { "" })
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    if let Some(r) = hat_la_set_route(1).await {
                                                        set_la_route_sig.set(r);
                                                        show_toast("Route set to High-Speed (Conn1)", "ok");
                                                    }
                                                });
                                            }
                                        >"High-Speed Route (Conn1)"</button>
                                    </div>
                                    <div style="font-size: 9px; color: var(--text-dim); margin-top: 6px">
                                        {move || if la_route.get() == 0 {
                                            "Low-speed capture routes EXP_EXT pins (up to 4 channels @ 1MHz max)."
                                        } else {
                                            "High-speed route routes through low-skew buffered Conn1 connector (max 3 channels)."
                                        }}
                                    </div>
                                </div>

                                // SWD setup
                                <div style="padding: 10px; border-radius: 6px; background: var(--bg-secondary)">
                                    <span style="font-size: 12px; font-weight: 700; display: block; margin-bottom: 6px">"SWD Target Setup (Dedicated Header)"</span>
                                    <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 8px">
                                        <div style=status_dot(st.target_detected, "#f59e0b")></div>
                                        <span style="font-size: 11px">
                                            {if st.target_detected {
                                                format!("Target: DPIDR 0x{:08X}", st.target_dpidr)
                                            } else {
                                                "No target detected".into()
                                            }}
                                        </span>
                                    </div>
                                    <div style="display: flex; gap: 6px">
                                        <button class="btn" style="font-size: 10px; padding: 4px 12px; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                            on:click=move |_| { send_hat_setup_swd(3300, 0); }
                                        >"Setup SWD 3.3V"</button>
                                        <button class="btn" style="font-size: 10px; padding: 4px 12px; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                            on:click=move |_| { send_hat_setup_swd(1800, 0); }
                                        >"Setup SWD 1.8V"</button>
                                    </div>
                                </div>

                            </div>
                        </div>

                    </div>

                    <div style="display: grid; grid-template-columns: 1.2fr 0.8fr; gap: 16px; margin-bottom: 16px">

                        // ── Shifted I/O Bank Card ──
                        <div class="card">
                            <div class="card-header">
                                <span>"Shifted I/O Bank Configuration (GPIO 10-15, 20-21)"</span>
                            </div>
                            <div class="card-body">
                                <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; margin-bottom: 12px">
                                    {(0..8u8).map(|i| {
                                        let is_out = Signal::derive(move || (io_dirs.get() & (1 << i)) != 0);
                                        let is_up = Signal::derive(move || (io_ups.get() & (1 << i)) != 0);
                                        let is_dn = Signal::derive(move || (io_dns.get() & (1 << i)) != 0);
                                        view! {
                                            <div style="padding: 8px; border-radius: 6px; background: var(--bg-secondary); border: 1px solid var(--border-color, #333)">
                                                <div style="font-size: 10px; font-weight: 700; color: #3b82f6; margin-bottom: 4px">
                                                    {format!("SH_IO_{}", i + 1)}
                                                </div>
                                                <div style="display: flex; gap: 4px; margin-bottom: 4px">
                                                    <button
                                                        class="btn btn-sm"
                                                        style=format!("padding: 1px 4px; font-size: 9px; flex: 1; {}", if !is_out.get() { "background: #3b82f640; color:#3b82f6" } else { "" })
                                                        on:click=move |_| {
                                                            set_io_dirs.update(|d| *d &= !(1 << i));
                                                        }
                                                    >"IN"</button>
                                                    <button
                                                        class="btn btn-sm"
                                                        style=format!("padding: 1px 4px; font-size: 9px; flex: 1; {}", if is_out.get() { "background: #3b82f640; color:#3b82f6" } else { "" })
                                                        on:click=move |_| {
                                                            set_io_dirs.update(|d| *d |= 1 << i);
                                                        }
                                                    >"OUT"</button>
                                                </div>
                                                <div style="display: flex; flex-direction: column; gap: 2px">
                                                    <label style="font-size: 9px; display: flex; align-items: center; gap: 3px; cursor: pointer">
                                                        <input
                                                            type="checkbox"
                                                            checked=is_up.get()
                                                            prop:checked=is_up.get()
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
                                                        <input
                                                            type="checkbox"
                                                            checked=is_dn.get()
                                                            prop:checked=is_dn.get()
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
                                            let d = io_dirs.get_untracked();
                                            let u = io_ups.get_untracked();
                                            let dn = io_dns.get_untracked();
                                            if hat_set_io_bank(d, u, dn).await.is_some() {
                                                show_toast("I/O bank configuration applied!", "ok");
                                            }
                                        });
                                    }
                                >"Apply I/O Bank Config"</button>
                            </div>
                        </div>

                        // ── Level Shifter Overrides Card ──
                        <div class="card">
                            <div class="card-header">
                                <span>"Level Shifter Overrides"</span>
                            </div>
                            <div class="card-body">
                                <div style="margin-bottom: 12px; padding: 8px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px">
                                        <span style="font-size: 11px; font-weight: 700">"Outputs Enable (OE)"</span>
                                        <button
                                            class="btn btn-sm"
                                            style=format!("font-size: 10px; padding: 2px 10px; {}",
                                                if ls_oe.get() { "background: #ef444430; color: #ef4444; border: 1px solid #ef444450" }
                                                else { "" })
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    let next = !ls_oe.get_untracked();
                                                    let dir = ls_dir.get_untracked();
                                                    if let Some(status) = hat_set_level_shift(next, dir).await {
                                                        set_ls_oe.set(status.oe);
                                                        set_ls_dir.set(status.dir);
                                                        if status.oe {
                                                            show_toast("Outputs Enabled!", "ok");
                                                        } else {
                                                            show_toast("Outputs Tri-stated!", "ok");
                                                        }
                                                    }
                                                });
                                            }
                                        >{if ls_oe.get() { "ACTIVE" } else { "TRI-STATE" }}</button>
                                    </div>
                                    <div style="font-size: 9px; color: var(--text-dim)">
                                        "Warning: Safety interlock. OE requires the 3V3_ADJ rail to be enabled first."
                                    </div>
                                </div>

                                <div style="padding: 8px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px">
                                        <span style="font-size: 11px; font-weight: 700">"Direction (DIR)"</span>
                                        <button
                                            class="btn btn-sm"
                                            style="font-size: 10px; padding: 2px 10px"
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    let oe = ls_oe.get_untracked();
                                                    let next = !ls_dir.get_untracked();
                                                    if let Some(status) = hat_set_level_shift(oe, next).await {
                                                        set_ls_oe.set(status.oe);
                                                        set_ls_dir.set(status.dir);
                                                    }
                                                });
                                            }
                                        >{if ls_dir.get() { "A → B (Output)" } else { "B → A (Input)" }}</button>
                                    </div>
                                    <div style="font-size: 9px; color: var(--text-dim)">
                                        "A → B sets EXP_EXT to drive target. B → A sets EXP_EXT as inputs."
                                    </div>
                                </div>
                            </div>
                        </div>

                    </div>

                    // ── Calibration Card ──
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-header">
                            <span>"DS4424 Auto-Calibration Sweep"</span>
                        </div>
                        <div class="card-body">
                            {move || {
                                if is_cal_running {
                                    view! {
                                        <div style="padding: 12px; text-align: center">
                                            <div style="font-size: 12px; font-weight: 700; color: #3b82f6; margin-bottom: 6px">
                                                {format!("Calibrating Rail {} ...", if cal_rail_id.get() == 1 { "VADJ3" } else { "VADJ4" })}
                                            </div>
                                            <div style="width: 100%; height: 8px; background: var(--bg-secondary); border-radius: 4px; overflow: hidden; margin-bottom: 8px">
                                                <div style=format!("width: {}%; height: 100%; background: #3b82f6; transition: width 0.3s ease", cal_prog_val) / >
                                            </div>
                                            <div style="font-size: 10px; color: var(--text-dim)">
                                                {format!("Progress: {}% complete", cal_prog_val)}
                                            </div>
                                        </div>
                                    }.into_any()
                                } else {
                                    view! {
                                        <div style="display: flex; gap: 12px; align-items: center">
                                            <span style="font-size: 11px; color: var(--text-dim)">"Select rail to calibrate:"</span>
                                            <select
                                                style="background: var(--bg-secondary); border: 1px solid var(--border-color); color: var(--text); border-radius: 4px; padding: 4px; font-size: 11px"
                                                on:change=move |ev| {
                                                    let val = event_target_value(&ev);
                                                    if let Ok(id) = val.parse::<u8>() {
                                                        set_cal_rail_id.set(id);
                                                    }
                                                }
                                            >
                                                <option value="1">"VADJ3 (0–36V, 18V midpoint)"</option>
                                                <option value="2">"VADJ4 (0–36V, 18V midpoint)"</option>
                                            </select>
                                            <button
                                                class="btn btn-primary"
                                                style="font-size: 11px; padding: 4px 16px"
                                                on:click=move |_| {
                                                    spawn_local(async move {
                                                        let id = cal_rail_id.get_untracked();
                                                        if let Some(status_code) = hat_calibrate_start(id).await {
                                                            if status_code == 0 {
                                                                set_cal_active.set(true);
                                                                set_cal_progress.set(0);
                                                                show_toast("Calibration sweep started!", "ok");
                                                            } else {
                                                                show_toast(&format!("Failed to start calibration: Error code {}", status_code), "err");
                                                            }
                                                        }
                                                    });
                                                }
                                            >"Start Calibration Sweep"</button>
                                        </div>
                                    }.into_any()
                                }
                            }}
                        </div>
                    </div>

                    // ── HAT Logs Card ──
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-header">
                            <span>"HAT Debug Logs"</span>
                        </div>
                        <div class="card-body">
                            <div style="display: flex; gap: 12px; align-items: center; margin-bottom: 8px">
                                <span style="font-size: 11px; color: var(--text-dim)">"RP2040 log relay:"</span>
                                <button
                                    class="btn btn-sm"
                                    style=move || format!("font-size: 10px; padding: 2px 10px; {}",
                                        if log_enabled.get() { "background: #10b98130; color: #10b981; border: 1px solid #10b98150" }
                                        else { "" })
                                    on:click=move |_| {
                                        let new_val = !log_enabled.get_untracked();
                                        set_log_enabled.set(new_val);
                                        spawn_local(async move {
                                            hat_la_log_enable(new_val).await;
                                        });
                                    }
                                >
                                    {move || if log_enabled.get() { "Enabled" } else { "Disabled" }}
                                </button>
                                <button
                                    class="btn btn-sm"
                                    style="font-size: 10px; padding: 2px 10px"
                                    on:click=move |_| { log_lines.update(|l| l.clear()); }
                                >"Clear"</button>
                            </div>
                            <pre style="font-size: 10px; font-family: 'JetBrains Mono', monospace; background: var(--bg-secondary); border-radius: 4px; padding: 8px; max-height: 200px; overflow-y: auto; white-space: pre-wrap; word-break: break-all; color: var(--text-dim)">
                                {move || {
                                    let lines = log_lines.get();
                                    if lines.is_empty() {
                                        "(no log output — enable log relay above)".to_string()
                                    } else {
                                        lines.iter().cloned().collect::<Vec<_>>().join("\n")
                                    }
                                }}
                            </pre>
                        </div>
                    </div>

                }.into_any()
            }}
        </div>
    }
}
