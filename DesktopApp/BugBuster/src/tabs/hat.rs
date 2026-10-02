use crate::components::icons::Icon;
use crate::components::ui::{EmptyState, SegmentedControl, Switch};
use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use std::collections::VecDeque;
use wasm_bindgen::closure::Closure;
use wasm_bindgen::JsValue;

// ── Local helpers ─────────────────────────────────────────────────────────────

/// Label over a status value; `ok` tints the value green and adds a status dot.
#[component]
fn HealthCell(label: &'static str, ok: bool, value: String, #[prop(optional)] plain: bool) -> impl IntoView {
    view! {
        <div class="sy-cell">
            <span class="sy-cell-label">{label}</span>
            <span class="sy-cell-value">
                {(!plain).then(|| view! { <span class=if ok { "dot tone-green" } else { "dot" }></span> })}
                <span class=if ok { "tone-text-green" } else { "" }>{value}</span>
            </span>
        </div>
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

    // ── Initial fetch (once; DESK-22: no longer re-run on every device-state tick)
    let _ = state;
    let alive_init = alive.clone();
    Effect::new(move |_| {
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
    // Each cycle issues two BBP commands (hat_get_status + hat_get_rail_status)
    // that share the single-client CDC0 link. DESK-22: non-overlapping, and a
    // bare board is probed every HAT_ABSENT_POLL_MS instead of every 3 s forever.
    start_tab_poll(
        move || async move {
            if let Some(st) = fetch_hat_status().await {
                set_hat.try_set(st);
            }
            if hat.try_get_untracked().is_some_and(|h| h.detected) {
                if let Some(rl) = hat_get_rail_status().await {
                    set_rails.try_set(rl);
                }
            }
        },
        move || {
            if hat.try_get_untracked().is_some_and(|h| h.detected) { HAT_POLL_MS } else { HAT_ABSENT_POLL_MS }
        },
    );

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
                          enable_label: &'static str,
                          vmin: f64,
                          vmax: f64,
                          confirmable: bool| {
        let find = move || rails.get().into_iter().find(|x| x.rail_id == id);
        let idx = id as usize;
        let is_on = move || find().map(|x| x.enabled).unwrap_or(false);
        let apply_class = if confirmable {
            "btn btn-tinted tone-orange"
        } else {
            "btn btn-tinted"
        };
        view! {
            <div class="group sy-rail" data-rail=idx class:is-on=is_on>
                <div class="group-header">
                    <span class="sy-rail-name"><span class="sy-rail-dot"></span>{name}</span>
                    <span class=move || if is_on() { "badge tone-green" } else { "badge" }>
                        {move || if is_on() { "On" } else { "Off" }}
                    </span>
                </div>
                <div class="sy-rail-role">{role}</div>

                <div class="sy-rail-main">
                    <span class="readout-label">{move || if is_on() { "Measured" } else { "Setpoint" }}</span>
                    <span class="sy-rail-value">
                        {move || {
                            let r = find();
                            let mv = if is_on() {
                                r.as_ref().map(|x| x.voltage_mv).unwrap_or(0)
                            } else {
                                r.as_ref().map(|x| x.target_mv).unwrap_or(0)
                            };
                            format!("{:.2}", mv as f64 / 1000.0)
                        }}
                        <span class="unit">"V"</span>
                    </span>
                </div>
                <dl class="kv sy-rail-kv">
                    <dt>"Setpoint"</dt>
                    <dd>{move || format!("{:.2} V", find().map(|x| x.target_mv).unwrap_or(0) as f64 / 1000.0)}</dd>
                    <dt>"Current"</dt>
                    <dd>{move || if is_on() { format!("{} mA", find().map(|x| x.current_ma).unwrap_or(0)) } else { "off".to_string() }}</dd>
                </dl>

                <div class="sy-rail-field">
                    <input type="number" class="number-input"
                        aria-label=format!("{name} setpoint in volts")
                        aria-invalid=move || {
                            let bad = v_dirty[idx].get()
                                && !(0.0..=40.0).contains(&v_in[idx].get().trim().parse::<f64>().unwrap_or(-1.0));
                            if bad { "true" } else { "false" }
                        }
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
                    <span class="number-unit">"V"</span>
                    <button class=apply_class
                        title=if confirmable { "Apply the voltage setpoint (high-voltage rail)" } else { "Apply the voltage setpoint" }
                        on:click=move |_| apply_voltage(id)
                    >"Apply"</button>
                </div>
                <div class="sy-hint"
                    class:is-error=move || {
                        v_dirty[idx].get()
                            && !(0.0..=40.0).contains(&v_in[idx].get().trim().parse::<f64>().unwrap_or(-1.0))
                    }
                >
                    {move || if v_dirty[idx].get() {
                        let v: f64 = v_in[idx].get().trim().parse().unwrap_or(-1.0);
                        if (0.0..=40.0).contains(&v) {
                            format!("Press Apply to set {:.2} V", v)
                        } else {
                            "Enter 0–36 V".to_string()
                        }
                    } else {
                        String::new()
                    }}
                </div>

                <div class="row sy-rail-enable">
                    <span class="row-label">{move || if is_on() { "Output enabled" } else { "Output disabled" }}</span>
                    <span title=if confirmable { "Asks for confirmation above 3.4 V" } else { "" }>
                        <Switch
                            checked=Signal::derive(is_on)
                            aria_label=enable_label
                            on_change=Callback::new(move |_: bool| {
                                let r = find();
                                let cur = r.as_ref().map(|x| x.enabled).unwrap_or(false);
                                let sp = r.as_ref().map(|x| x.target_mv).unwrap_or(0);
                                toggle_rail(id, cur, sp, confirmable);
                            })
                        />
                    </span>
                </div>
            </div>
        }
    };

    view! {
        <div class="view sy-hat">
            <p class="sy-lead">
                "HAT expansion board: target power rails, SWD target detection, routing, shifted I/O and debug logs."
            </p>

            // ── Status strip ──────────────────────────────────────────────────
            {move || {
                let st  = hat.get();
                let cp  = caps.get();
                let fw  = format!("v{}.{}", st.fw_major, st.fw_minor);
                let rev = cp.as_ref().map(|c| format!("v{}", c.hw_revision)).unwrap_or("—".into());
                view! {
                    <div class="group sy-health">
                        <HealthCell label="Detected" ok=st.detected value=if st.detected { "Yes".into() } else { "No".into() } />
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
                            view! { <HealthCell label="UART" ok=ok value=label /> }
                        }}
                        <HealthCell label="DAP" ok=st.dap_connected value=if st.dap_connected { "OK".into() } else { "—".into() } />
                        <HealthCell label="Target" ok=st.target_detected value=if st.target_detected { "OK".into() } else { "—".into() } />
                        <HealthCell label="Revision" ok=false value=rev plain=true />
                        <HealthCell label="Firmware" ok=false value=fw plain=true />
                        <div class="sy-health-actions">
                            <button class="btn btn-sm" title="Re-read HAT status"
                                on:click=move |_| {
                                    spawn_local(async move {
                                        if let Some(s) = fetch_hat_status().await { set_hat.set(s); }
                                    });
                                }
                            ><Icon name="refresh-cw" size=13 />"Refresh"</button>
                        </div>
                    </div>
                }
            }}

            // ── No HAT ────────────────────────────────────────────────────────
            {move || if !hat.get().detected {
                view! {
                    <div class="group">
                        <EmptyState icon="cpu" title="No HAT detected"
                            message="Connect a HAT board to the expansion header, then click Refresh.">
                            <button class="btn btn-sm"
                                on:click=move |_| {
                                    spawn_local(async move {
                                        if let Some(s) = fetch_hat_status().await { set_hat.set(s); }
                                    });
                                }
                            ><Icon name="refresh-cw" size=13 />"Refresh"</button>
                        </EmptyState>
                    </div>
                }.into_any()
            } else {
                ().into_any()
            }}

            <Show when=move || hat.get().detected>

                // Capabilities
                {move || {
                    if let Some(cp) = caps.get() {
                        let defs: &[(u32, &str, &str)] = &[
                            (1,  "green",  "Rails Control"),
                            (2,  "blue",   "RGB LEDs"),
                            (4,  "purple", "LA Low-Speed"),
                            (8,  "purple", "LA High-Speed"),
                            (16, "teal",   "Shifted I/O"),
                        ];
                        let badges = defs.iter().filter(|(f,_,_)| cp.flags & f != 0).map(|(_, tone, name)| {
                            let tone = *tone; let name = *name;
                            view! { <span class=format!("badge tone-{tone}")>{name}</span> }
                        }).collect::<Vec<_>>();
                        view! {
                            <div class="sy-caps">
                                <span class="sy-caps-label">"Capabilities"</span>
                                {badges}
                            </div>
                        }.into_any()
                    } else { ().into_any() }
                }}

                // ── Target power rails ────────────────────────────────────────
                <div class="section-label">"Target power rails"</div>
                <div class="grid-3 sy-rails">
                    {rail_card(0, "VLOGIC", "Logic rail and level-shifter reference · 1.7–5.0 V", "Enable VLOGIC", 1.7, 5.0, false)}
                    {rail_card(1, "VADJ3", "Target A power · 0–36 V", "Enable VADJ3", 1.8, 36.0, true)}
                    {rail_card(2, "VADJ4", "Target B and SWD target power · 0–36 V", "Enable VADJ4", 1.8, 36.0, true)}
                </div>

                // High-voltage enable confirmation
                {move || confirm.get().map(|(cid, cmv)| {
                    let cname = match cid { 1 => "VADJ3", 2 => "VADJ4", _ => "rail" };
                    view! {
                        <div class="scrim" on:click=move |_| confirm.set(None)></div>
                        <div class="dialog sy-dialog" role="dialog" aria-modal="true" aria-labelledby="sy-hv-title">
                            <div class="group">
                                <div class="stack">
                                    <div class="text-headline sy-dialog-title" id="sy-hv-title">
                                        <Icon name="triangle-alert" size=16 />"High-voltage enable"
                                    </div>
                                    <div>{format!("Enable {} at {:.2} V?", cname, cmv as f64 / 1000.0)}</div>
                                    <div class="muted text-footnote">"Voltages above 3.4 V can permanently damage a 3.3 V target. Confirm the connected device tolerates this level before continuing."</div>
                                    <div class="hstack sy-dialog-actions">
                                        <button class="btn" on:click=move |_| confirm.set(None)>"Cancel"</button>
                                        <button class="btn btn-danger"
                                            on:click=move |_| { confirm.set(None); do_enable(cid, true); }
                                        >"Enable anyway"</button>
                                    </div>
                                </div>
                            </div>
                        </div>
                    }
                })}

                // ── Routing, SWD, level shifter ───────────────────────────────
                <div class="section-label">"Routing and SWD"</div>
                <div class="grid-3 sy-routing">

                    // LA route
                    <div class="group">
                        <div class="group-header"><span class="group-title">"LA route"</span></div>
                        <div class="stack-sm">
                            <SegmentedControl
                                options=vec![(0u8, "Low-Speed (Conn2)"), (1u8, "High-Speed (Conn1)")]
                                value=la_route
                                block=true
                                aria_label="Logic analyzer route"
                                on_change=Callback::new(move |r: u8| {
                                    spawn_local(async move {
                                        if let Some(x) = hat_la_set_route(r).await {
                                            set_la_route_sig.set(x);
                                            show_toast(
                                                if r == 0 { "Route → Low-Speed (Conn2)" } else { "Route → High-Speed (Conn1)" },
                                                "ok",
                                            );
                                        }
                                    });
                                })
                            />
                            <div class="sy-hint sy-hint-static">
                                {move || if la_route.get() == 0 {
                                    "EXP_EXT pins: up to 4 channels at 1 MHz max."
                                } else {
                                    "Low-skew buffered Conn1: up to 3 channels."
                                }}
                            </div>
                            {move || {
                                let is_usb = is_usb.get();
                                view! {
                                    <div class="hstack">
                                        <button class="btn btn-sm"
                                            disabled=!is_usb
                                            title=if is_usb { "Reset RP2040 USB endpoint" } else { "USB connection required" }
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
                                            view! { <span class="text-caption subtle">"USB only"</span> }.into_any()
                                        } else {
                                            ().into_any()
                                        }}
                                    </div>
                                }
                            }}
                        </div>
                    </div>

                    // SWD target
                    <div class="group">
                        <div class="group-header"><span class="group-title">"SWD target"</span></div>
                        <div class="stack-sm">
                            {move || {
                                let det = target.get().map(|t| t.detected).unwrap_or_else(|| hat.get().target_detected);
                                let dpidr = target.get().map(|t| t.dpidr).unwrap_or_else(|| hat.get().target_dpidr);
                                let label = if det {
                                    format!("DPIDR 0x{:08X}", dpidr)
                                } else {
                                    "No target".into()
                                };
                                view! {
                                    <div class="sy-swd-status">
                                        <span class=if det { "dot tone-green" } else { "dot" }></span>
                                        <span class="sy-mono">{label}</span>
                                    </div>
                                }
                            }}
                            <button class="btn btn-tinted btn-block"
                                disabled=move || detect_busy.get()
                                on:click=move |_| detect()
                            >{move || if detect_busy.get() { "Detecting…" } else { "Detect Target" }}</button>
                            <div class="sy-hint sy-hint-static">"Quick setup: sets VADJ4 and VLOGIC, enables outputs, then detects."</div>
                            <div class="grid-2 sy-prep">
                                <button class="btn btn-sm"
                                    title="Power the target at 3.3 V and detect"
                                    disabled=move || detect_busy.get()
                                    on:click=move |_| prepare_swd(3300)
                                >"Prep 3.3 V"</button>
                                <button class="btn btn-sm"
                                    title="Power the target at 1.8 V and detect"
                                    disabled=move || detect_busy.get()
                                    on:click=move |_| prepare_swd(1800)
                                >"Prep 1.8 V"</button>
                            </div>
                        </div>
                    </div>

                    // Level shifter
                    <div class="group">
                        <div class="group-header"><span class="group-title">"Level shifter"</span></div>
                        <div class="rows">
                            <div class="row">
                                <div>
                                    <span class="row-label">"Outputs enable (OE)"</span>
                                    <span class="row-hint">"Requires 3V3_ADJ."</span>
                                </div>
                                <div class="hstack">
                                    <span class="text-footnote muted">{move || if ls_oe.get() { "Active" } else { "Tri-state" }}</span>
                                    <Switch
                                        checked=ls_oe
                                        aria_label="Level shifter outputs enable"
                                        on_change=Callback::new(move |_: bool| {
                                            spawn_local(async move {
                                                let next = !ls_oe.get_untracked();
                                                let dir  = ls_dir.get_untracked();
                                                if let Some(s) = hat_set_level_shift(next, dir).await {
                                                    set_ls_oe.set(s.oe);
                                                    set_ls_dir.set(s.dir);
                                                    show_toast(if s.oe { "Outputs enabled" } else { "Outputs tri-stated" }, "ok");
                                                }
                                            });
                                        })
                                    />
                                </div>
                            </div>
                            <div class="row">
                                <div>
                                    <span class="row-label">"Direction (DIR)"</span>
                                    <span class="row-hint">"A→B drives; B→A listens."</span>
                                </div>
                                <div class="hstack">
                                    <span class="text-footnote muted">{move || if ls_dir.get() { "A→B" } else { "B→A" }}</span>
                                    <Switch
                                        checked=ls_dir
                                        aria_label="Level shifter direction"
                                        on_change=Callback::new(move |_: bool| {
                                            spawn_local(async move {
                                                let oe   = ls_oe.get_untracked();
                                                let next = !ls_dir.get_untracked();
                                                if let Some(s) = hat_set_level_shift(oe, next).await {
                                                    set_ls_oe.set(s.oe);
                                                    set_ls_dir.set(s.dir);
                                                }
                                            });
                                        })
                                    />
                                </div>
                            </div>
                        </div>
                    </div>

                </div>

                // ── Shifted I/O bank ──────────────────────────────────────────
                <div class="group">
                    <div class="group-header">
                        <span class="group-title">"Shifted I/O bank"</span>
                        <span class="group-subtitle">"GPIO 10–15, 20–21"</span>
                    </div>
                    <div class="sy-io-grid">
                        {(0..8u8).map(|i| {
                            let dir_val = Signal::derive(move || (io_dirs.get() & (1 << i)) != 0);
                            let pull_val = Signal::derive(move || {
                                if (io_ups.get() & (1 << i)) != 0 { 1u8 }
                                else if (io_dns.get() & (1 << i)) != 0 { 2u8 }
                                else { 0u8 }
                            });
                            view! {
                                <div class="sy-io">
                                    <div class="sy-io-name">{format!("SH_IO_{}", i + 1)}</div>
                                    <SegmentedControl
                                        options=vec![(false, "IN"), (true, "OUT")]
                                        value=dir_val
                                        block=true
                                        small=true
                                        aria_label="Direction"
                                        on_change=Callback::new(move |out: bool| {
                                            if out {
                                                set_io_dirs.update(|d| *d |= 1 << i);
                                            } else {
                                                set_io_dirs.update(|d| *d &= !(1 << i));
                                            }
                                        })
                                    />
                                    <SegmentedControl
                                        options=vec![(0u8, "No pull"), (1u8, "Up"), (2u8, "Down")]
                                        value=pull_val
                                        block=true
                                        small=true
                                        aria_label="Pull resistor"
                                        on_change=Callback::new(move |p: u8| {
                                            match p {
                                                1 => {
                                                    set_io_ups.update(|u| *u |= 1 << i);
                                                    set_io_dns.update(|d| *d &= !(1 << i));
                                                }
                                                2 => {
                                                    set_io_dns.update(|d| *d |= 1 << i);
                                                    set_io_ups.update(|u| *u &= !(1 << i));
                                                }
                                                _ => {
                                                    set_io_ups.update(|u| *u &= !(1 << i));
                                                    set_io_dns.update(|d| *d &= !(1 << i));
                                                }
                                            }
                                        })
                                    />
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                    <div class="hstack sy-io-actions">
                        <span class="text-footnote muted">"Changes take effect when applied."</span>
                        <span class="spacer"></span>
                        <button class="btn btn-primary"
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

                // ── RP2040 debug logs ─────────────────────────────────────────
                <div class="group">
                    <div class="group-header">
                        <span class="group-title">"RP2040 debug logs"</span>
                        <div class="group-actions">
                            <span class="text-footnote muted">
                                {move || if log_enabled.get() { "Streaming" } else { "Off" }}
                            </span>
                            <Switch
                                checked=log_enabled
                                aria_label="Stream RP2040 debug logs"
                                on_change=Callback::new(move |_: bool| {
                                    let new_val = !log_enabled.get_untracked();
                                    spawn_local(async move {
                                        if hat_la_log_enable(new_val).await.is_some() {
                                            set_log_enabled.set(new_val);
                                        } else {
                                            show_toast("Failed to toggle log relay", "err");
                                        }
                                    });
                                })
                            />
                            <button class="btn btn-sm"
                                on:click=move |_| { log_lines.update(|l| l.clear()); }
                            >"Clear"</button>
                        </div>
                    </div>
                    <pre class="sy-log">
                        {move || {
                            let lines = log_lines.get();
                            if lines.is_empty() {
                                "(no log output; enable the relay above)".to_string()
                            } else {
                                lines.iter().cloned().collect::<Vec<_>>().join("\n")
                            }
                        }}
                    </pre>
                </div>

            </Show>
        </div>
    }
}
