use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use std::collections::VecDeque;
use std::time::Duration;
use wasm_bindgen::closure::Closure;
use wasm_bindgen::JsCast;
use wasm_bindgen::JsValue;

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
    let (hat, set_hat) = signal(HatStatus::default());
    let (caps, set_caps) = signal(None::<HatCaps>);
    let (la_route, set_la_route_sig) = signal(0u8);

    let (io_dirs, set_io_dirs) = signal(0u8);
    let (io_ups, set_io_ups) = signal(0u8);
    let (io_dns, set_io_dns) = signal(0u8);

    let (ls_oe, set_ls_oe) = signal(false);
    let (ls_dir, set_ls_dir) = signal(false);

    let (log_enabled, set_log_enabled) = signal(false);
    let log_lines: RwSignal<VecDeque<String>> = RwSignal::new(VecDeque::new());
    let (uart_errors, _set_uart_errors) = signal(0u8);
    let (is_usb, set_is_usb) = signal(true);

    // ── Target power rails ─────────────────────────────────────────────────────
    let (rails, set_rails) = signal(Vec::<HatRailStatus>::new());
    // Per-rail voltage entry box (volts as text), keyed by rail_id 0=VLOGIC, 1=VADJ3, 2=VADJ4.
    let v_in: [RwSignal<String>; 3] = std::array::from_fn(|_| RwSignal::new(String::new()));
    let v_dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));
    // Pending high-voltage enable confirmation: Some((rail_id, setpoint_mv)).
    let confirm = RwSignal::new(None::<(u8, u16)>);
    // SWD target detection result + in-flight flag.
    let (target, set_target) = signal(None::<HatTargetInfo>);
    let detect_busy = RwSignal::new(false);

    // Alive flag — flips false on tab unmount so background tasks stop safely.
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    // ── hat-log Tauri event listener ─────────────────────────────────────────
    let alive_log = alive.clone();
    Effect::new(move |_| {
        let log_lines = log_lines;
        let alive = alive_log.clone();
        spawn_local(async move {
            let closure = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
                if !alive.load(std::sync::atomic::Ordering::Relaxed) {
                    return;
                }
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

    // ── Track USB vs HTTP transport ───────────────────────────────────────────
    let alive_conn = alive.clone();
    Effect::new(move |_| {
        let alive = alive_conn.clone();
        spawn_local(async move {
            let closure = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
                if !alive.load(std::sync::atomic::Ordering::Relaxed) {
                    return;
                }
                if let Some(payload) = js_sys::Reflect::get(&event, &JsValue::from_str("payload"))
                    .ok()
                    .and_then(|p| js_sys::Reflect::get(&p, &JsValue::from_str("mode")).ok())
                    .and_then(|m| m.as_string())
                {
                    set_is_usb.set(payload == "usb");
                }
            });
            listen("connection-status", &closure).await;
            closure.forget();
        });
    });

    // ── Initial fetch ─────────────────────────────────────────────────────────
    let alive_init = alive.clone();
    Effect::new(move |_| {
        let _ = state.get();
        let alive = alive_init.clone();
        spawn_local(async move {
            if let Some(st) = fetch_hat_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_hat.set(st);
                }
            }
            if let Some(cp) = hat_get_caps().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_caps.set(Some(cp));
                }
            }
            if let Some(rl) = hat_get_rail_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_rails.set(rl);
                }
            }
        });
    });

    // ── 3 s status poll ────────────────────────────────────────────────────────
    // Reduced from 1 s to 3 s: each cycle issues two BBP commands (hat_get_status
    // + hat_get_rail_status) that share the single-client CDC0 link with the
    // overview poll and the connection manager status poll.
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
            Duration::from_secs(3),
        )
        .ok();
        on_cleanup(move || {
            if let Some(h) = handle {
                h.clear();
            }
        });
    });

    // ── Rail control helpers ───────────────────────────────────────────────────
    let apply_voltage = move |id: u8| {
        let raw = v_in[id as usize].get_untracked();
        let volts: f64 = raw.trim().parse().unwrap_or(-1.0);
        if !(0.0..=40.0).contains(&volts) {
            show_toast("Enter a valid voltage", "err");
            return;
        }
        let mv = (volts * 1000.0).round() as u16;
        spawn_local(async move {
            if let Some(r) = hat_set_rail_voltage(id, mv).await {
                set_rails.set(r);
                v_dirty[id as usize].set(false);
                show_toast("Voltage applied", "ok");
            } else {
                show_toast("Failed to set voltage", "err");
            }
        });
    };

    let do_enable = move |id: u8, en: bool| {
        spawn_local(async move {
            if let Some(r) = hat_set_rail_enable(id, en).await {
                set_rails.set(r);
                show_toast(if en { "Rail enabled" } else { "Rail disabled" }, "ok");
            } else {
                show_toast("Failed to toggle rail", "err");
            }
        });
    };

    let toggle_rail = move |id: u8, currently_on: bool, setpoint_mv: u16, confirmable: bool| {
        if !currently_on && confirmable && setpoint_mv > 3400 {
            confirm.set(Some((id, setpoint_mv)));
        } else {
            do_enable(id, !currently_on);
        }
    };

    let detect = move || {
        spawn_local(async move {
            detect_busy.set(true);
            match hat_detect_target().await {
                Some(t) => {
                    show_toast(
                        if t.detected { "SWD target detected" } else { "No SWD target found" },
                        if t.detected { "ok" } else { "err" },
                    );
                    set_target.set(Some(t));
                }
                None => show_toast("Target detect failed", "err"),
            }
            detect_busy.set(false);
            if let Some(s) = fetch_hat_status().await {
                set_hat.set(s);
            }
        });
    };

    let prepare_swd = move |mv: u16| {
        spawn_local(async move {
            detect_busy.set(true);
            // Match the level-shifter reference and target supply to the target
            // voltage, then bring the rails up. Each call is awaited so the
            // sequence stays ordered on the single-client BBP link — firing them
            // concurrently floods the HAT UART bridge and causes 0x11 timeouts.
            let _ = hat_set_rail_voltage(2, mv).await; // VADJ4 — SWD target power
            let _ = hat_set_rail_voltage(0, mv).await; // VLOGIC — level-shifter reference
            // VLOGIC must be enabled before the level-shifter OE (the firmware
            // rejects OE otherwise with INVALID_PARAM). Only proceed to OE if it
            // actually came up.
            let vlogic_ok = hat_set_rail_enable(0, true).await.is_some();
            if let Some(r) = hat_set_rail_enable(2, true).await {
                set_rails.set(r);
            }
            if vlogic_ok {
                if let Some(s) = hat_set_level_shift(true, false).await {
                    set_ls_oe.set(s.oe);
                    set_ls_dir.set(s.dir);
                }
            } else {
                show_toast("VLOGIC enable timed out — skipping OE", "err");
            }
            match hat_detect_target().await {
                Some(t) => {
                    show_toast(
                        if t.detected { "SWD target detected" } else { "SWD ready — no target found" },
                        if t.detected { "ok" } else { "err" },
                    );
                    set_target.set(Some(t));
                }
                None => show_toast("SWD setup failed", "err"),
            }
            // The 1 s status/rail poll refreshes the rest of the UI; avoid extra
            // round-trips here that would pile onto the link right after detect.
            detect_busy.set(false);
        });
    };

    // One reusable rail control card (set voltage · Apply · Enable).
    let rail_card = move |id: u8,
                          name: &'static str,
                          role: &'static str,
                          color: &'static str,
                          vmin: f64,
                          vmax: f64,
                          confirmable: bool| {
        let find = move || rails.get().into_iter().find(|x| x.rail_id == id);
        let idx = id as usize;
        view! {
            <div style=format!("border-radius: 10px; background: var(--bg-secondary); padding: 12px; border-top: 2px solid {color}")>
                <div style=format!("font-size: 11px; font-weight: 700; color: {color}; text-transform: uppercase; letter-spacing: 0.05em")>{name}</div>
                <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 8px; height: 24px">{role}</div>

                <div style=format!("text-align: center; font-size: 30px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: {color}; letter-spacing: -1px")>
                    {move || {
                        let r = find();
                        let en = r.as_ref().map(|x| x.enabled).unwrap_or(false);
                        let mv = if en {
                            r.as_ref().map(|x| x.voltage_mv).unwrap_or(0)
                        } else {
                            r.as_ref().map(|x| x.target_mv).unwrap_or(0)
                        };
                        format!("{:.2}V", mv as f64 / 1000.0)
                    }}
                </div>
                <div style="text-align: center; font-size: 9px; color: var(--text-dim); margin-bottom: 8px; font-family: 'JetBrains Mono', monospace">
                    {move || {
                        let r = find();
                        let en = r.as_ref().map(|x| x.enabled).unwrap_or(false);
                        let sp = r.as_ref().map(|x| x.target_mv).unwrap_or(0);
                        let ma = r.as_ref().map(|x| x.current_ma).unwrap_or(0);
                        if en {
                            format!("set {:.2}V · {} mA", sp as f64 / 1000.0, ma)
                        } else {
                            format!("set {:.2}V · off", sp as f64 / 1000.0)
                        }
                    }}
                </div>

                <div style="display: flex; gap: 5px; align-items: center; margin-bottom: 4px">
                    <input type="number" class="number-input" style="flex: 1; font-size: 12px"
                        min=vmin max=vmax step="0.05"
                        prop:value=move || {
                            if v_dirty[idx].get() {
                                v_in[idx].get()
                            } else {
                                format!("{:.3}", find().map(|x| x.target_mv).unwrap_or(0) as f64 / 1000.0)
                            }
                        }
                        on:input=move |e| {
                            v_in[idx].set(event_target_value(&e));
                            v_dirty[idx].set(true);
                        }
                    />
                    <button class="btn btn-sm"
                        style=format!("font-size: 10px; padding: 4px 12px; background: {color}20; color: {color}; border: 1px solid {color}50")
                        on:click=move |_| apply_voltage(id)
                    >"Apply"</button>
                </div>
                <div style="font-size: 9px; height: 13px; color: #f59e0b; margin-bottom: 8px">
                    {move || if v_dirty[idx].get() {
                        let v: f64 = v_in[idx].get().trim().parse().unwrap_or(-1.0);
                        if (0.0..=40.0).contains(&v) {
                            format!("→ press Apply to set {:.2} V", v)
                        } else {
                            "enter 0–36 V".to_string()
                        }
                    } else {
                        String::new()
                    }}
                </div>

                <div style="display: flex; justify-content: space-between; align-items: center">
                    <span style="font-size: 10px; color: var(--text-dim)">
                        {move || if find().map(|x| x.enabled).unwrap_or(false) { "Enabled" } else { "Disabled" }}
                    </span>
                    <label class="toggle-wrap">
                        <div class="toggle" class:active=move || find().map(|x| x.enabled).unwrap_or(false)
                            on:click=move |_| {
                                let r = find();
                                let cur = r.as_ref().map(|x| x.enabled).unwrap_or(false);
                                let sp = r.as_ref().map(|x| x.target_mv).unwrap_or(0);
                                toggle_rail(id, cur, sp, confirmable);
                            }
                        ><div class="toggle-thumb"></div></div>
                    </label>
                </div>
            </div>
        }
    };

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "HAT Expansion Board v2 — Target power rails, SWD target detection, routing, shifted I/O, and debug logs."
            </div>

            // ── Summary Banner ────────────────────────────────────────────────
            {move || {
                let st  = hat.get();
                let cp  = caps.get();
                let fw  = format!("v{}.{}", st.fw_major, st.fw_minor);
                let rev = cp.as_ref().map(|c| format!("v{}", c.hw_revision)).unwrap_or("—".into());
                view! {
                    <div class="summary-banner" style="justify-content: space-between; padding: 8px 12px; margin-bottom: 14px">
                        <div style="display: flex; align-items: center; gap: 14px; flex-wrap: wrap">
                            <HatPill label="Detected" ok=st.detected    value={if st.detected      { "Yes".into() } else { "No".into() }} />
                            {move || {
                                let connected = hat.get().connected;
                                let errs = uart_errors.get();
                                let (ok, label) = if connected && errs == 0 {
                                    (true, "OK".to_string())
                                } else if connected && errs > 0 {
                                    (false, "Degraded".to_string())
                                } else {
                                    (false, "—".to_string())
                                };
                                view! { <HatPill label="UART" ok=ok value=label /> }
                            }}
                            <HatPill label="DAP"    ok=st.dap_connected    value={if st.dap_connected    { "OK".into() } else { "—".into() }} />
                            <HatPill label="Target" ok=st.target_detected  value={if st.target_detected  { "OK".into() } else { "—".into() }} />
                            <HatPill label="Rev" ok=true value=rev />
                            <HatPill label="FW"  ok=true value=fw />
                        </div>
                        <button class="btn btn-sm" style="font-size: 10px; padding: 2px 10px"
                            on:click=move |_| {
                                spawn_local(async move {
                                    if let Some(s) = fetch_hat_status().await { set_hat.set(s); }
                                });
                            }
                        >"Refresh"</button>
                    </div>
                }
            }}

            // ── No HAT warning ────────────────────────────────────────────────
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

                // ── Target Power Rails ──────────────────────────────────────────────
                <div class="card" style="margin-bottom: 16px">
                    <div class="card-header">
                        <span>"Target Power Rails"</span>
                        <span style="font-size: 10px; color: var(--text-dim)">"Set voltage · Apply · Enable"</span>
                    </div>
                    <div class="card-body" style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px">
                        {rail_card(0, "VLOGIC", "Logic rail · level-shifter reference · 1.7–5.0 V", "#10b981", 1.7, 5.0, false)}
                        {rail_card(1, "VADJ3", "Target A power · 0–36 V", "#06b6d4", 1.8, 36.0, true)}
                        {rail_card(2, "VADJ4", "Target B · SWD target power · 0–36 V", "#f59e0b", 1.8, 36.0, true)}
                    </div>
                </div>

                // High-voltage enable confirmation modal
                {move || confirm.get().map(|(cid, cmv)| {
                    let cname = match cid { 1 => "VADJ3", 2 => "VADJ4", _ => "rail" };
                    view! {
                        <div style="position: fixed; inset: 0; background: rgba(0,0,0,0.6); display: flex; align-items: center; justify-content: center; z-index: 1000"
                            on:click=move |_| confirm.set(None)
                        >
                            <div style="background: var(--bg-secondary); border: 1px solid #f59e0b60; border-radius: 14px; padding: 20px; max-width: 380px; box-shadow: 0 0 40px rgba(0,0,0,0.6)"
                                on:click=move |e| e.stop_propagation()
                            >
                                <div style="font-size: 13px; font-weight: 700; color: #f59e0b; margin-bottom: 8px">"⚠ High-voltage enable"</div>
                                <div style="font-size: 12px; margin-bottom: 6px">{format!("Enable {} at {:.2} V?", cname, cmv as f64 / 1000.0)}</div>
                                <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 16px; line-height: 1.5">"Voltages above 3.4 V can permanently damage a 3.3 V target. Confirm the connected device tolerates this level before continuing."</div>
                                <div style="display: flex; gap: 8px; justify-content: flex-end">
                                    <button class="btn btn-sm" style="font-size: 11px; padding: 5px 14px"
                                        on:click=move |_| confirm.set(None)
                                    >"Cancel"</button>
                                    <button class="btn btn-sm" style="font-size: 11px; padding: 5px 14px; background: #f59e0b25; color: #f59e0b; border: 1px solid #f59e0b60"
                                        on:click=move |_| { confirm.set(None); do_enable(cid, true); }
                                    >"Enable anyway"</button>
                                </div>
                            </div>
                        </div>
                    }
                })}

                // ── Routing & SWD + Level Shifter (full width) ────────────────
                <div class="card" style="margin-bottom: 16px">
                    <div class="card-header"><span>"Routing & SWD"</span></div>
                    <div class="card-body" style="display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 16px">

                        // LA Route
                        <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                            <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 8px">"LA Route"</div>
                            <div style="display: flex; gap: 8px; margin-bottom: 8px">
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
                            <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 8px">
                                {move || if la_route.get() == 0 {
                                    "EXP_EXT pins — up to 4 ch @ 1 MHz max."
                                } else {
                                    "Low-skew buffered Conn1 — up to 3 ch."
                                }}
                            </div>
                            <div style="display: flex; align-items: center; gap: 8px">
                                {move || {
                                    let is_usb = is_usb.get();
                                    view! {
                                        <button class="btn"
                                            disabled=move || !is_usb
                                            title=move || if is_usb { "Reset RP2040 USB endpoint".to_string() } else { "USB connection required".to_string() }
                                            style=move || format!("font-size: 10px; padding: 3px 10px{}",
                                                if !is_usb { "; opacity: 0.4; cursor: not-allowed" } else { "" })
                                            on:click=move |_| {
                                                if !is_usb { return; }
                                                spawn_local(async move {
                                                    if hat_la_usb_reset().await.is_some() {
                                                        show_toast("LA USB endpoint reset", "ok");
                                                    } else {
                                                        show_toast("LA USB reset failed", "err");
                                                    }
                                                });
                                            }
                                        >"Reset LA USB"</button>
                                        {if !is_usb {
                                            view! { <span style="font-size: 9px; color: var(--text-dim)">"(USB only)"</span> }.into_any()
                                        } else {
                                            ().into_any()
                                        }}
                                    }
                                }}
                            </div>
                        </div>

                        // SWD Target
                        <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                            <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 8px">"SWD Target"</div>
                            <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 8px">
                                {move || {
                                    let det = target.get().map(|t| t.detected).unwrap_or_else(|| hat.get().target_detected);
                                    let dpidr = target.get().map(|t| t.dpidr).unwrap_or_else(|| hat.get().target_dpidr);
                                    let dot_style = if det {
                                        "width:9px;height:9px;border-radius:50%;flex-shrink:0;background:#10b981;box-shadow:0 0 6px #10b981"
                                    } else {
                                        "width:9px;height:9px;border-radius:50%;flex-shrink:0;background:var(--text-dim)"
                                    };
                                    let label = if det {
                                        format!("DPIDR 0x{:08X}", dpidr)
                                    } else {
                                        "No target".into()
                                    };
                                    view! {
                                        <div style=dot_style></div>
                                        <span style="font-size: 11px; font-family: 'JetBrains Mono', monospace">{label}</span>
                                    }
                                }}
                            </div>
                            <button class="btn" style="width: 100%; font-size: 10px; padding: 5px; margin-bottom: 8px; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                disabled=move || detect_busy.get()
                                on:click=move |_| detect()
                            >{move || if detect_busy.get() { "Detecting…" } else { "Detect Target" }}</button>
                            <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 6px">"Quick setup — sets VADJ4 + VLOGIC, enables OE, detects:"</div>
                            <div style="display: flex; gap: 6px">
                                <button class="btn btn-sm" style="flex: 1; font-size: 10px; padding: 4px"
                                    disabled=move || detect_busy.get()
                                    on:click=move |_| prepare_swd(3300)
                                >"Prep 3.3 V"</button>
                                <button class="btn btn-sm" style="flex: 1; font-size: 10px; padding: 4px"
                                    disabled=move || detect_busy.get()
                                    on:click=move |_| prepare_swd(1800)
                                >"Prep 1.8 V"</button>
                            </div>
                        </div>

                        // Level Shifter
                        <div style="border-radius: 8px; background: var(--bg-secondary); padding: 10px">
                            <div style="font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 8px">"Level Shifter"</div>

                            <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px">
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
