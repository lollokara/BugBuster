//! Unified HV IO tab — each of the 4 channel tiles has its own independent
//! In/Out mode selector. Compact layout so 2x2 tiles fit in a 1400x900 window
//! without scrolling.
use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;
use crate::components::channel_sparkline::ChannelSparkline;

const SPARK_CAP: usize = 120;

const DEBOUNCE_OPTIONS: &[(u8, &str)] = &[
    (0, "None"), (1, "1ms"), (2, "2ms"), (3, "4ms"),
    (4, "8ms"), (5, "16ms"), (6, "32ms"), (7, "64ms"),
];

const DO_MODE_OPTIONS: &[(u8, &str)] = &[
    (0, "High-Z"), (1, "Push-Pull"), (2, "Open Drain"), (3, "Push-Pull HART"),
];

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum HvMode { Din, Dout }

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DinConfigArgs {
    channel: u8,
    thresh: u8,
    thresh_mode: bool,
    debounce: u8,
    sink: u8,
    sink_range: bool,
    oc_det: bool,
    sc_det: bool,
}

fn send_din_config(ch: u8, thresh: u8, debounce: u8, oc_det: bool, sc_det: bool) {
    let args = serde_wasm_bindgen::to_value(&DinConfigArgs {
        channel: ch, thresh, thresh_mode: false, debounce,
        sink: 0, sink_range: false, oc_det, sc_det,
    }).unwrap();
    let label = format!("Set CH {} DIN config", CH_NAMES[ch as usize]);
    invoke_with_feedback("set_din_config", args, &label);
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DoConfigArgs {
    channel: u8,
    mode: u8,
    src_sel_gpio: bool,
    t1: u8,
    t2: u8,
}

fn send_do_config(ch: u8, mode: u8, src_sel_gpio: bool, t1: u8, t2: u8) {
    let args = serde_wasm_bindgen::to_value(&DoConfigArgs {
        channel: ch, mode, src_sel_gpio, t1, t2,
    }).unwrap();
    let label = format!("Set CH {} DO config", CH_NAMES[ch as usize]);
    invoke_with_feedback("set_do_config", args, &label);
}

#[component]
pub fn HvIoTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Per-channel mode (not global). Default all to DIN.
    let mode: [RwSignal<HvMode>; 4] = std::array::from_fn(|_| RwSignal::new(HvMode::Din));

    // DIN state (per channel)
    let din_thresh   = [RwSignal::new(64u8),  RwSignal::new(64u8),  RwSignal::new(64u8),  RwSignal::new(64u8)];
    let din_debounce = [RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8)];
    let din_oc_det   = [RwSignal::new(false), RwSignal::new(false), RwSignal::new(false), RwSignal::new(false)];
    let din_sc_det   = [RwSignal::new(false), RwSignal::new(false), RwSignal::new(false), RwSignal::new(false)];

    // DOUT state (per channel)
    let do_mode   = [RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8)];
    let do_srcgp  = [RwSignal::new(false), RwSignal::new(false), RwSignal::new(false), RwSignal::new(false)];
    let do_t1     = [RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8)];
    let do_t2     = [RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8),   RwSignal::new(0u8)];

    // History of digital state as 0/1 floats (per channel) for the sparkline.
    let history: [RwSignal<Vec<f32>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    Effect::new(move |_| {
        let ds = state.get();
        for (i, ch) in ds.channels.iter().enumerate().take(4) {
            let v = match mode[i].get() {
                HvMode::Din  => if ch.din_state { 1.0_f32 } else { 0.0 },
                HvMode::Dout => if ch.do_state  { 1.0_f32 } else { 0.0 },
            };
            history[i].update(|buf| {
                buf.push(v);
                if buf.len() > SPARK_CAP {
                    let d = buf.len() - SPARK_CAP;
                    buf.drain(0..d);
                }
            });
        }
    });

    view! {
        <div class="tab-content hv-io-tab">
            <div class="tab-desc">"High-voltage digital I/O. Each channel has its own In/Out selector. Per-channel settings are preserved across mode toggles."</div>

            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let color = CH_COLORS[i];
                        let is_din_func = ch.function == 8 || ch.function == 9;
                        let hist = history[i];
                        let spark_color = color.to_string();

                        // Per-channel signals (captured into closures below).
                        let th = din_thresh[i];
                        let db = din_debounce[i];
                        let oc = din_oc_det[i];
                        let sc = din_sc_det[i];
                        let dm = do_mode[i];
                        let sg = do_srcgp[i];
                        let t1 = do_t1[i];
                        let t2 = do_t2[i];
                        let md = mode[i];

                        let body = move || {
                            match md.get() {
                                HvMode::Din => {
                                    if !is_din_func {
                                        view! {
                                            <div class="mode-warning">
                                                <span class="mode-warning-icon">"ℹ"</span>
                                                <span>"Set channel to DIN mode to monitor digital input"</span>
                                            </div>
                                        }.into_any()
                                    } else {
                                        view! {
                                            <div>
                                                <div class="din-display">
                                                    <div class="din-led" class:din-led-on=ch.din_state
                                                        style=if ch.din_state { format!("background: {}; box-shadow: 0 0 20px {}", color, color) } else { String::new() }
                                                    ></div>
                                                    <div class="din-info">
                                                        <span class="din-state-label">{if ch.din_state { "HIGH" } else { "LOW" }}</span>
                                                        <span class="din-counter">"Events: "{format!("{}", ch.din_counter)}</span>
                                                    </div>
                                                </div>

                                                <div class="config-section">
                                                    <div class="config-row">
                                                        <label>"Debounce"</label>
                                                        <select class="dropdown"
                                                            prop:value=move || db.get().to_string()
                                                            on:change=move |ev| {
                                                                let val: u8 = event_target_value(&ev).parse().unwrap_or(0);
                                                                db.set(val);
                                                                send_din_config(ch_idx, th.get_untracked(), val, oc.get_untracked(), sc.get_untracked());
                                                            }
                                                        >
                                                            {DEBOUNCE_OPTIONS.iter().map(|(code, name)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"Threshold"</label>
                                                        <input type="number" class="number-input" min="0" max="127" step="1"
                                                            prop:value=move || th.get().to_string()
                                                            on:change=move |ev| {
                                                                let val: u8 = event_target_value(&ev).parse().unwrap_or(64);
                                                                th.set(val);
                                                                send_din_config(ch_idx, val, db.get_untracked(), oc.get_untracked(), sc.get_untracked());
                                                            }
                                                        />
                                                    </div>
                                                    <details class="hv-io-advanced">
                                                        <summary>"Fault detection"</summary>
                                                        <div class="config-row">
                                                            <label>"OC Detect"</label>
                                                            <div class="toggle" class:active=move || oc.get()
                                                                on:click=move |_| {
                                                                    let new_val = !oc.get_untracked();
                                                                    oc.set(new_val);
                                                                    send_din_config(ch_idx, th.get_untracked(), db.get_untracked(), new_val, sc.get_untracked());
                                                                }
                                                            ><div class="toggle-thumb"></div></div>
                                                        </div>
                                                        <div class="config-row">
                                                            <label>"SC Detect"</label>
                                                            <div class="toggle" class:active=move || sc.get()
                                                                on:click=move |_| {
                                                                    let new_val = !sc.get_untracked();
                                                                    sc.set(new_val);
                                                                    send_din_config(ch_idx, th.get_untracked(), db.get_untracked(), oc.get_untracked(), new_val);
                                                                }
                                                            ><div class="toggle-thumb"></div></div>
                                                        </div>
                                                    </details>
                                                </div>
                                            </div>
                                        }.into_any()
                                    }
                                }
                                HvMode::Dout => {
                                    view! {
                                        <div>
                                            <div class="do-center">
                                                <button class="do-btn" class:do-btn-on=ch.do_state
                                                    style=format!("--do-color: {}", color)
                                                    on:click=move |_| {
                                                        #[derive(Serialize)]
                                                        struct Args { channel: u8, on: bool }
                                                        let new_state = !ch.do_state;
                                                        let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx, on: new_state }).unwrap();
                                                        let label = format!("Set CH {} DO {}", CH_NAMES[ch_idx as usize], if new_state { "ON" } else { "OFF" });
                                                        invoke_with_feedback("set_do_state", args, &label);
                                                    }
                                                >
                                                    <div class="do-btn-indicator"></div>
                                                    <span>{if ch.do_state { "ON" } else { "OFF" }}</span>
                                                </button>
                                            </div>

                                            <div class="config-section">
                                                <div class="config-row">
                                                    <label>"DO Mode"</label>
                                                    <select class="dropdown"
                                                        prop:value=move || dm.get().to_string()
                                                        on:change=move |e| {
                                                            let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                            dm.set(val);
                                                            send_do_config(ch_idx, val, sg.get_untracked(), t1.get_untracked(), t2.get_untracked());
                                                        }
                                                    >
                                                        {DO_MODE_OPTIONS.iter().map(|(code, name)| {
                                                            view! { <option value=code.to_string()>{*name}</option> }
                                                        }).collect::<Vec<_>>()}
                                                    </select>
                                                </div>
                                                <details class="hv-io-advanced">
                                                    <summary>"Advanced"</summary>
                                                    <div class="config-row">
                                                        <label>"Source"</label>
                                                        <label class="toggle-wrap">
                                                            <span class="toggle-off-label">"SPI"</span>
                                                            <div class="toggle" class:active=move || sg.get()
                                                                on:click=move |_| {
                                                                    let new_val = !sg.get_untracked();
                                                                    sg.set(new_val);
                                                                    send_do_config(ch_idx, dm.get_untracked(), new_val, t1.get_untracked(), t2.get_untracked());
                                                                }
                                                            ><div class="toggle-thumb"></div></div>
                                                            <span class="toggle-on-label">"GPIO"</span>
                                                        </label>
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"T1 (μs)"</label>
                                                        <input type="number" class="number-input" min="0" max="15" step="1"
                                                            prop:value=move || t1.get().to_string()
                                                            on:change=move |e| {
                                                                let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                                t1.set(val);
                                                                send_do_config(ch_idx, dm.get_untracked(), sg.get_untracked(), val, t2.get_untracked());
                                                            }
                                                        />
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"T2 (μs)"</label>
                                                        <input type="number" class="number-input" min="0" max="255" step="1"
                                                            prop:value=move || t2.get().to_string()
                                                            on:change=move |e| {
                                                                let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                                t2.set(val);
                                                                send_do_config(ch_idx, dm.get_untracked(), sg.get_untracked(), t1.get_untracked(), val);
                                                            }
                                                        />
                                                    </div>
                                                </details>
                                            </div>
                                        </div>
                                    }.into_any()
                                }
                            }
                        };

                        view! {
                            <div class="card channel-card hv-io-card">
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <span class="channel-func">{func_name(ch.function)}</span>
                                    <select class="dropdown hv-io-mode-select"
                                        prop:value=move || match md.get() { HvMode::Din => "din".to_string(), HvMode::Dout => "dout".to_string() }
                                        on:change=move |e| {
                                            let v = event_target_value(&e);
                                            md.set(if v == "dout" { HvMode::Dout } else { HvMode::Din });
                                        }
                                    >
                                        <option value="din">"In"</option>
                                        <option value="dout">"Out"</option>
                                    </select>
                                </div>
                                <div class="card-body">
                                    {body}
                                    <ChannelSparkline
                                        values=Signal::from(hist)
                                        min=Signal::derive(move || -0.1f32)
                                        max=Signal::derive(move || 1.1f32)
                                        color=spark_color
                                    />
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
