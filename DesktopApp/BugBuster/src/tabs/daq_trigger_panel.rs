// =============================================================================
// daq_trigger_panel.rs — DAQ Trigger / Flag configuration panel.
//
// Inspector content for the DAQ tab (the host provides the inspector chrome and
// title). Presents the 12 mainboard IOs as 4 connector blocks of 3 IOs each (the
// physical layout), badging the analog-capable HV IOs (3/6/9/12 → AD74416H) vs
// the LV digital IOs. Each IO can be tagged Off / Flag / Trigger with an edge
// selector; HV IOs additionally choose a digital or analog (voltage-threshold)
// source. A VLOGIC slider sets the logic-rail voltage (reuses the existing
// hat_set_io_voltage path), and the arm controls combine triggers with OR / AND
// logic plus a pre-trigger depth.
//
// All state is mirrored from / pushed to the S3 trigger engine over BBP
// (CMD_DAQ_TRIG); edge events are forwarded to the P4 as USB MARKER records.
// =============================================================================

use leptos::prelude::*;
use leptos::task::spawn_local;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

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

fn role_class(role: u8) -> &'static str {
    match role {
        ROLE_FLAG => "dq-role-dot role-flag",
        ROLE_TRIGGER => "dq-role-dot role-trig",
        _ => "dq-role-dot",
    }
}

#[component]
pub fn TriggerPanel(open: Signal<bool>) -> impl IntoView {
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

    let alive = Arc::new(AtomicBool::new(true));
    on_cleanup({
        let alive = alive.clone();
        move || alive.store(false, Ordering::SeqCst)
    });

    // Pull the current engine state from the device when the panel opens.
    Effect::new(move |_| {
        if !open.get() {
            return;
        }
        let alive = alive.clone();
        spawn_local(async move {
            let st = daq_get_trig_state().await;
            if !alive.load(Ordering::SeqCst) {
                return;
            }
            if let Some(st) = st {
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
        // ---- Arm ------------------------------------------------------------
        <section class="inspector-section">
            <div class="dq-section-head">
                <h4>"Trigger"</h4>
                <span class=move || if fired.get() { "badge tone-blue" } else if armed.get() { "badge tone-green" } else { "badge" }
                    role="status">
                    {move || if fired.get() { "Triggered" } else if armed.get() { "Armed" } else { "Idle" }}
                </span>
            </div>
            <div class="row">
                <span class="row-label">"Combine"
                    <span class="row-hint">{move || if logic.get() == LOGIC_AND { "All triggers must fire" } else { "First trigger fires" }}</span>
                </span>
                <div class="seg seg-sm" role="radiogroup" aria-label="Trigger combine logic">
                    <button class:active=move || logic.get() == LOGIC_OR
                        on:click=move |_| set_logic(LOGIC_OR)>"OR"</button>
                    <button class:active=move || logic.get() == LOGIC_AND
                        on:click=move |_| set_logic(LOGIC_AND)>"AND"</button>
                </div>
            </div>
            <div class="row">
                <span class="row-label">"Pre-trigger"<span class="row-hint">"Samples kept before the event"</span></span>
                <span class="dq-num-unit">
                    <input class="dq-num" type="number" min="0" max="5000" step="10" aria-label="Pre-trigger depth in milliseconds"
                        prop:value=move || pre_ms.get().to_string()
                        on:input=move |ev| {
                            if let Ok(v) = event_target_value(&ev).parse::<u32>() { pre_ms.set(v); }
                        } />
                    <span class="dq-unit">"ms"</span>
                </span>
            </div>
            <button type="button"
                class=move || if armed.get() { "btn btn-sm btn-block btn-tinted tone-orange" } else { "btn btn-sm btn-block btn-primary" }
                on:click=toggle_arm>
                {move || if armed.get() { "Disarm trigger" } else { "Arm trigger" }}
            </button>
        </section>

        // ---- VLOGIC ---------------------------------------------------------
        <section class="inspector-section">
            <h4>"Logic level"</h4>
            <div class="dq-field">
                <div class="dq-field-head">
                    <span class="row-label">"VLOGIC"<span class="row-hint">"Digital IO level"</span></span>
                    <span class="row-value">{move || format!("{:.2} V", vlogic_mv.get() as f64 / 1000.0)}</span>
                </div>
                <input type="range" min="1800" max="5000" step="100" aria-label="VLOGIC in millivolts"
                    prop:value=move || vlogic_mv.get().to_string()
                    on:input=move |ev| {
                        if let Ok(v) = event_target_value(&ev).parse::<u32>() { apply_vlogic(v); }
                    }
                />
            </div>
        </section>

        // ---- 4 connector blocks × 3 IOs ------------------------------------
        {move || {
            (0..4).map(|blk| {
                let rail = if blk < 2 { "VADJ1" } else { "VADJ2" };
                view! {
                    <section class="inspector-section">
                        <div class="dq-section-head">
                            <h4>{format!("Block {}", blk + 1)}</h4>
                            <span class="badge">{rail}</span>
                        </div>
                        {(0..3).map(|pos| {
                            let io = (blk * 3 + pos + 1) as u8;
                            view!{ <IoRow io=io ios=ios push_io=Callback::new(move |(i, c)| push_io(i, c))/> }
                        }).collect_view()}
                    </section>
                }
            }).collect_view()
        }}

        <section class="inspector-section">
            <p class="row-hint">
                "Flags mark events as vertical lines on the acquisition and timeline (kept through every zoom level). \
                 Triggers start the capture window on the selected edge."
            </p>
        </section>
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
        <div class="dq-io-row">
            <div class="dq-io-head">
                <span class=move || role_class(cfg().role) aria-hidden="true"></span>
                <strong class="dq-io-name">{format!("IO{}", io)}</strong>
                <span class=if hv { "badge tone-orange" } else { "badge" }
                    title=if hv { "Analog-capable high-voltage IO (AD74416H)" } else { "Low-voltage digital IO" }>
                    {if hv { "HV 12 V" } else { "LV" }}
                </span>
                <span class="dq-spacer"></span>
                <div class="seg seg-sm" role="radiogroup" aria-label=format!("IO{} role", io)>
                    {[("Off", ROLE_OFF), ("Flag", ROLE_FLAG), ("Trigger", ROLE_TRIGGER)].iter().map(|(lbl, r)| {
                        let r = *r;
                        view!{
                            <button class:active=move || cfg().role == r
                                on:click=move |_| set_role(r)>{*lbl}</button>
                        }
                    }).collect_view()}
                </div>
            </div>

            // Edge + (HV) source/threshold rows - only when not Off.
            <Show when=move || cfg().role != ROLE_OFF>
                <div class="dq-io-opts">
                    <div class="row">
                        <span class="row-label">"Edge"</span>
                        <div class="seg seg-sm" role="radiogroup" aria-label=format!("IO{} edge", io)>
                            {[("Rising", EDGE_RISING), ("Falling", EDGE_FALLING), ("Any", EDGE_ANY)].iter().map(|(lbl, e)| {
                                let e = *e;
                                view!{
                                    <button class:active=move || cfg().edge == e
                                        on:click=move |_| set_edge(e)>{*lbl}</button>
                                }
                            }).collect_view()}
                        </div>
                    </div>

                    {move || hv.then(|| view!{
                        <div class="row">
                            <span class="row-label">"Source"</span>
                            <div class="seg seg-sm" role="radiogroup" aria-label=format!("IO{} source", io)>
                                <button class:active=move || cfg().source == SRC_DIGITAL
                                    on:click=move |_| set_source(SRC_DIGITAL)>"Digital"</button>
                                <button class:active=move || cfg().source == SRC_ANALOG
                                    on:click=move |_| set_source(SRC_ANALOG)>"Analog"</button>
                            </div>
                        </div>
                    })}

                    {move || (hv && cfg().source == SRC_ANALOG).then(|| view!{
                        <div class="dq-field">
                            <div class="dq-field-head">
                                <span class="row-label">"Threshold"</span>
                                <span class="dq-num-unit">
                                    <input class="dq-num" type="number" step="0.05" min="0" max="12"
                                        aria-label=format!("IO{} threshold in volts", io)
                                        prop:value=move || format!("{:.2}", cfg().threshold_v)
                                        on:input=move |ev| {
                                            if let Ok(v) = event_target_value(&ev).parse::<f32>() { set_threshold(v); }
                                        } />
                                    <span class="dq-unit">"V"</span>
                                </span>
                            </div>
                            <input type="range" min="0" max="12" step="0.05"
                                aria-label=format!("IO{} threshold slider", io)
                                prop:value=move || format!("{:.2}", cfg().threshold_v)
                                on:input=move |ev| {
                                    if let Ok(v) = event_target_value(&ev).parse::<f32>() { set_threshold(v); }
                                } />
                        </div>
                    })}
                </div>
            </Show>
        </div>
    }
}
