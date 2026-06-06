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
    let (caps, set_caps) = signal(None::<HatCaps>);
    let (la_route, set_la_route_sig) = signal(0u8);

    let (io_dirs, set_io_dirs) = signal(0u8);
    let (io_ups,  set_io_ups)  = signal(0u8);
    let (io_dns,  set_io_dns)  = signal(0u8);

    let (ls_oe, set_ls_oe) = signal(false);
    let (ls_dir, set_ls_dir) = signal(false);

    let (log_enabled, set_log_enabled) = signal(false);
    let log_lines: RwSignal<VecDeque<String>> = RwSignal::new(VecDeque::new());
    let (uart_errors, _set_uart_errors) = signal(0u8);
    let (is_usb, set_is_usb) = signal(true);

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

    // ── Track USB vs HTTP transport ───────────────────────────────────────────
    Effect::new(move |_| {
        spawn_local(async move {
            let closure = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
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
    Effect::new(move |_| {
        let _ = state.get();
        spawn_local(async move {
            if let Some(st) = fetch_hat_status().await { set_hat.set(st); }
            if let Some(cp) = hat_get_caps().await     { set_caps.set(Some(cp)); }
        });
    });

    // ── 1 s status poll ───────────────────────────────────────────────────────
    Effect::new(move |_| {
        let handle = leptos::prelude::set_interval_with_handle(move || {
            spawn_local(async move {
                if let Some(st) = fetch_hat_status().await { set_hat.set(st); }
            });
        }, Duration::from_secs(1)).ok();
        on_cleanup(move || { if let Some(h) = handle { h.clear(); } });
    });

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "HAT Expansion Board v2 — Routing, SWD, shifted I/O, and debug logs. Voltage rail control is in the Voltages tab."
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
