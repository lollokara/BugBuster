// =============================================================================
// daq_trigger_panel.rs — DAQ Trigger / Flag configuration panel.
//
// A slide-in panel for the DAQ tab. Presents the 12 mainboard IOs as 4 connector
// blocks of 3 IOs each (the physical layout), badging the analog-capable HV IOs
// (3/6/9/12 → AD74416H) vs the LV digital IOs. Each IO can be tagged Off / Flag
// / Trigger with an edge selector; HV IOs additionally choose a digital or
// analog (voltage-threshold) source. A VLOGIC slider sets the logic-rail voltage
// (reuses the existing hat_set_io_voltage path), and the arm controls combine
// triggers with OR / AND logic plus a pre-trigger depth.
//
// All state is mirrored from / pushed to the S3 trigger engine over BBP
// (CMD_DAQ_TRIG); edge events are forwarded to the P4 as USB MARKER records.
// =============================================================================

use leptos::prelude::*;
use leptos::task::spawn_local;

use crate::tauri_bridge::{
    daq_arm, daq_get_trig_state, daq_set_io_role, daq_set_trig_logic, send_hat_set_io_voltage,
    DaqTrigIoCfg, DaqTrigState,
};

// Role / edge / source / logic codes (mirror daq_trigger.h on the S3).
const ROLE_OFF: u8 = 0;
const ROLE_FLAG: u8 = 1;
const ROLE_TRIGGER: u8 = 2;
const EDGE_RISING: u8 = 0;
const EDGE_FALLING: u8 = 1;
const EDGE_ANY: u8 = 2;
const SRC_DIGITAL: u8 = 0;
const SRC_ANALOG: u8 = 1;
const LOGIC_OR: u8 = 1;
const LOGIC_AND: u8 = 2;

/// Analog-capable HV IOs (position 3 of each connector block).
fn is_hv(io: u8) -> bool {
    matches!(io, 3 | 6 | 9 | 12)
}

fn role_color(role: u8) -> &'static str {
    match role {
        ROLE_FLAG => "#f59e0b",
        ROLE_TRIGGER => "#22d3ee",
        _ => "#475569",
    }
}

#[component]
pub fn TriggerPanel(open: RwSignal<bool>) -> impl IntoView {
    // 12 IO configs (index 0 = IO1). Defaults: all Off / rising / digital.
    let ios = RwSignal::new(vec![DaqTrigIoCfg::default(); 12]);
    let logic = RwSignal::new(LOGIC_OR);
    let armed = RwSignal::new(false);
    let fired = RwSignal::new(false);
    // Pre-trigger depth in milliseconds (converted to samples on arm).
    let pre_ms = RwSignal::new(50u32);
    let sample_rate = RwSignal::new(250_000u32);
    // VLOGIC rail in millivolts (1.8–5.0 V).
    let vlogic_mv = RwSignal::new(3300u32);

    // Pull the current engine state from the device when the panel opens.
    Effect::new(move |_| {
        if !open.get() {
            return;
        }
        spawn_local(async move {
            if let Some(st) = daq_get_trig_state().await {
                apply_state(&st, ios, logic, armed, fired);
            }
        });
    });

    let push_io = move |io: u8, cfg: DaqTrigIoCfg| {
        ios.update(|v| {
            if let Some(slot) = v.get_mut((io - 1) as usize) {
                *slot = cfg;
            }
        });
        spawn_local(async move {
            daq_set_io_role(io, cfg.role, cfg.edge, cfg.source, cfg.threshold_v).await;
        });
    };

    let set_logic = move |l: u8| {
        logic.set(l);
        spawn_local(async move { daq_set_trig_logic(l).await });
    };

    let toggle_arm = move |_| {
        let next = !armed.get_untracked();
        armed.set(next);
        let samples = ((pre_ms.get_untracked() as u64 * sample_rate.get_untracked() as u64)
            / 1000) as u32;
        spawn_local(async move { daq_arm(next, samples).await });
    };

    let apply_vlogic = move |mv: u32| {
        vlogic_mv.set(mv);
        send_hat_set_io_voltage(mv as u16);
    };

    view! {
        <div class="daq-trig-panel"
            style=move || format!(
                "width:{};min-width:0;overflow:hidden;transition:width 0.3s ease;background:#0f172a;border-radius:8px;",
                if open.get() { "360px" } else { "0px" })
        >
            <div style="width:360px;padding:12px;overflow-y:auto;height:100%;box-sizing:border-box;font-size:13px;">
                <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:10px;">
                    <strong style="color:#22d3ee;letter-spacing:0.5px;">"TRIGGERS / FLAGS"</strong>
                    <button class="btn btn-ghost btn-sm" on:click=move |_| open.set(false)>"✕"</button>
                </div>

                // ---- VLOGIC slider --------------------------------------------------
                <div style="margin-bottom:12px;padding:8px;background:#020617;border-radius:8px;">
                    <div style="display:flex;justify-content:space-between;margin-bottom:4px;">
                        <span style="color:#94a3b8;">"VLOGIC (digital IO level)"</span>
                        <span style="color:#e2e8f0;font-variant-numeric:tabular-nums;">
                            {move || format!("{:.2} V", vlogic_mv.get() as f64 / 1000.0)}
                        </span>
                    </div>
                    <input type="range" min="1800" max="5000" step="100"
                        prop:value=move || vlogic_mv.get().to_string()
                        on:input=move |ev| {
                            if let Ok(v) = event_target_value(&ev).parse::<u32>() { apply_vlogic(v); }
                        }
                        style="width:100%;"
                    />
                </div>

                // ---- Trigger logic + arm -------------------------------------------
                <div style="margin-bottom:12px;padding:8px;background:#020617;border-radius:8px;">
                    <div style="display:flex;align-items:center;gap:8px;margin-bottom:8px;">
                        <span style="color:#94a3b8;">"Combine"</span>
                        <button class="btn btn-sm"
                            style=move || pill_style(logic.get() == LOGIC_OR)
                            on:click=move |_| set_logic(LOGIC_OR)>"OR"</button>
                        <button class="btn btn-sm"
                            style=move || pill_style(logic.get() == LOGIC_AND)
                            on:click=move |_| set_logic(LOGIC_AND)>"AND"</button>
                        <span style="color:#64748b;font-size:11px;">
                            {move || if logic.get() == LOGIC_AND { "all fire" } else { "first fires" }}
                        </span>
                    </div>
                    <div style="display:flex;align-items:center;gap:8px;margin-bottom:8px;">
                        <span style="color:#94a3b8;">"Pre-trigger"</span>
                        <input type="number" min="0" max="5000" step="10"
                            prop:value=move || pre_ms.get().to_string()
                            on:input=move |ev| {
                                if let Ok(v) = event_target_value(&ev).parse::<u32>() { pre_ms.set(v); }
                            }
                            style="width:72px;" />
                        <span style="color:#64748b;font-size:11px;">"ms"</span>
                    </div>
                    <div style="display:flex;align-items:center;gap:8px;">
                        <button class="btn btn-sm"
                            style=move || if armed.get() {
                                "background:#ef4444;color:#fff;font-weight:700;".to_string()
                            } else {
                                "background:#22d3ee;color:#04293a;font-weight:700;".to_string()
                            }
                            on:click=toggle_arm>
                            {move || if armed.get() { "■ Disarm" } else { "◉ Arm trigger" }}
                        </button>
                        <span style=move || format!(
                            "font-size:11px;font-weight:700;padding:2px 8px;border-radius:8px;{}",
                            if fired.get() { "color:#22d3ee;border:1px solid #22d3ee66;" }
                            else if armed.get() { "color:#f59e0b;border:1px solid #f59e0b66;" }
                            else { "color:#64748b;border:1px solid #33415566;" })>
                            {move || if fired.get() { "TRIGGERED" } else if armed.get() { "ARMED" } else { "IDLE" }}
                        </span>
                    </div>
                </div>

                // ---- 4 connector blocks × 3 IOs ------------------------------------
                {move || {
                    (0..4).map(|blk| {
                        let rail = if blk < 2 { "VADJ1" } else { "VADJ2" };
                        view! {
                            <div style="margin-bottom:10px;border:1px solid #1e293b;border-radius:8px;overflow:hidden;">
                                <div style="display:flex;justify-content:space-between;padding:4px 8px;background:#111c33;color:#94a3b8;font-size:11px;">
                                    <span>{format!("Block {}", blk + 1)}</span>
                                    <span>{rail}</span>
                                </div>
                                {(0..3).map(|pos| {
                                    let io = (blk * 3 + pos + 1) as u8;
                                    view!{ <IoRow io=io ios=ios push_io=Callback::new(move |(i, c)| push_io(i, c))/> }
                                }).collect_view()}
                            </div>
                        }
                    }).collect_view()
                }}

                <p style="color:#64748b;font-size:11px;line-height:1.5;margin-top:8px;">
                    "Flags mark events as vertical lines on the acquisition + timeline (kept through every zoom level). \
                     Triggers start the capture window on the selected edge."
                </p>
            </div>
        </div>
    }
}

fn pill_style(active: bool) -> String {
    if active {
        "background:#22d3ee;color:#04293a;font-weight:700;".to_string()
    } else {
        "background:#1e293b;color:#94a3b8;".to_string()
    }
}

fn apply_state(
    st: &DaqTrigState,
    ios: RwSignal<Vec<DaqTrigIoCfg>>,
    logic: RwSignal<u8>,
    armed: RwSignal<bool>,
    fired: RwSignal<bool>,
) {
    if st.ios.len() == 12 {
        ios.set(st.ios.clone());
    }
    logic.set(st.logic);
    armed.set(st.armed);
    fired.set(st.fired);
}

#[component]
fn IoRow(
    io: u8,
    ios: RwSignal<Vec<DaqTrigIoCfg>>,
    push_io: Callback<(u8, DaqTrigIoCfg)>,
) -> impl IntoView {
    let cfg = move || ios.get().get((io - 1) as usize).copied().unwrap_or_default();
    let hv = is_hv(io);

    let set_role = move |role: u8| {
        let mut c = cfg();
        c.role = role;
        push_io.run((io, c));
    };
    let set_edge = move |edge: u8| {
        let mut c = cfg();
        c.edge = edge;
        push_io.run((io, c));
    };
    let set_source = move |source: u8| {
        let mut c = cfg();
        c.source = source;
        push_io.run((io, c));
    };
    let set_threshold = move |v: f32| {
        let mut c = cfg();
        c.threshold_v = v;
        push_io.run((io, c));
    };

    view! {
        <div style="padding:6px 8px;border-top:1px solid #0f1a2e;">
            <div style="display:flex;align-items:center;gap:6px;margin-bottom:4px;">
                <span style=move || format!(
                    "width:8px;height:8px;border-radius:50%;background:{};", role_color(cfg().role))></span>
                <strong style="color:#e2e8f0;min-width:38px;">{format!("IO{}", io)}</strong>
                <span style=move || format!(
                    "font-size:9px;font-weight:800;padding:1px 6px;border-radius:6px;{}",
                    if hv { "color:#fb7185;border:1px solid #fb718566;background:#fb71851a;" }
                    else { "color:#38bdf8;border:1px solid #38bdf866;background:#38bdf81a;" })>
                    {if hv { "HV 12V" } else { "LV" }}
                </span>
                <div style="flex:1;"></div>
                // Role segmented control.
                <div style="display:flex;border-radius:6px;overflow:hidden;border:1px solid #1e293b;">
                    {[("Off", ROLE_OFF), ("Flag", ROLE_FLAG), ("Trig", ROLE_TRIGGER)].iter().map(|(lbl, r)| {
                        let r = *r;
                        view!{
                            <button
                                style=move || seg_style(cfg().role == r, role_color(r))
                                on:click=move |_| set_role(r)>{*lbl}</button>
                        }
                    }).collect_view()}
                </div>
            </div>

            // Edge + (HV) source/threshold row — only when not Off.
            <Show when=move || cfg().role != ROLE_OFF>
                <div style="display:flex;align-items:center;gap:6px;flex-wrap:wrap;padding-left:14px;">
                    <span style="color:#64748b;font-size:11px;">"Edge"</span>
                    <div style="display:flex;border-radius:6px;overflow:hidden;border:1px solid #1e293b;">
                        {[("↑", EDGE_RISING), ("↓", EDGE_FALLING), ("⇅", EDGE_ANY)].iter().map(|(lbl, e)| {
                            let e = *e;
                            view!{
                                <button style=move || seg_style(cfg().edge == e, "#94a3b8")
                                    on:click=move |_| set_edge(e)>{*lbl}</button>
                            }
                        }).collect_view()}
                    </div>

                    {move || hv.then(|| view!{
                        <div style="display:flex;border-radius:6px;overflow:hidden;border:1px solid #1e293b;">
                            <button style=move || seg_style(cfg().source == SRC_DIGITAL, "#94a3b8")
                                on:click=move |_| set_source(SRC_DIGITAL)>"Dig"</button>
                            <button style=move || seg_style(cfg().source == SRC_ANALOG, "#a855f7")
                                on:click=move |_| set_source(SRC_ANALOG)>"Ana"</button>
                        </div>
                    })}

                    {move || (hv && cfg().source == SRC_ANALOG).then(|| view!{
                        <span style="display:flex;align-items:center;gap:4px;">
                            <input type="number" step="0.1" min="0" max="12"
                                prop:value=move || format!("{:.2}", cfg().threshold_v)
                                on:input=move |ev| {
                                    if let Ok(v) = event_target_value(&ev).parse::<f32>() { set_threshold(v); }
                                }
                                style="width:60px;" />
                            <span style="color:#64748b;font-size:11px;">"V"</span>
                        </span>
                    })}
                </div>
            </Show>
        </div>
    }
}

fn seg_style(active: bool, color: &str) -> String {
    if active {
        format!(
            "padding:2px 8px;font-size:11px;border:none;cursor:pointer;background:{c}22;color:{c};font-weight:700;",
            c = color
        )
    } else {
        "padding:2px 8px;font-size:11px;border:none;cursor:pointer;background:#0b1220;color:#64748b;".to_string()
    }
}
