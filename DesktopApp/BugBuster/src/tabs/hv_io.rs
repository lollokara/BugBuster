//! Unified HV IO tab — each of the 4 channel tiles has its own independent
//! In/Out mode selector. Compact layout so 2x2 tiles fit in a 1440x900 window
//! without scrolling.
use crate::components::channel_sparkline::{
    ch_key, ch_var, set_channel_function, ChannelEmpty, ChannelHead, ChannelSparkline,
};
use crate::components::ui::{SegmentedControl, Switch};
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

const SPARK_CAP: usize = 120;

const DEBOUNCE_OPTIONS: &[(u8, &str)] = &[
    (0, "None"),
    (1, "1ms"),
    (2, "2ms"),
    (3, "4ms"),
    (4, "8ms"),
    (5, "16ms"),
    (6, "32ms"),
    (7, "64ms"),
];

const DO_MODE_OPTIONS: &[(u8, &str)] = &[
    (0, "High-Z"),
    (1, "Push-Pull"),
    (2, "Open Drain"),
    (3, "Push-Pull HART"),
];

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum HvMode {
    Din,
    Dout,
}

/// Current `open` state of the <details> element that fired a toggle event.
fn details_open(e: &web_sys::Event) -> bool {
    e.target()
        .and_then(|t| js_sys::Reflect::get(&t, &wasm_bindgen::JsValue::from_str("open")).ok())
        .and_then(|v| v.as_bool())
        .unwrap_or(false)
}

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
        channel: ch,
        thresh,
        thresh_mode: false,
        debounce,
        sink: 0,
        sink_range: false,
        oc_det,
        sc_det,
    })
    .unwrap();
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
        channel: ch,
        mode,
        src_sel_gpio,
        t1,
        t2,
    })
    .unwrap();
    let label = format!("Set CH {} DO config", CH_NAMES[ch as usize]);
    invoke_with_feedback("set_do_config", args, &label);
}

#[component]
pub fn HvIoTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Per-channel mode (not global). Default all to DIN.
    let mode: [RwSignal<HvMode>; 4] = std::array::from_fn(|_| RwSignal::new(HvMode::Din));
    // Disclosure state survives the card rebuild on every device-state tick.
    let fault_open: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));
    let adv_open: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));

    // DIN state (per channel)
    let din_thresh = [
        RwSignal::new(64u8),
        RwSignal::new(64u8),
        RwSignal::new(64u8),
        RwSignal::new(64u8),
    ];
    let din_debounce = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];
    let din_oc_det = [
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
    ];
    let din_sc_det = [
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
    ];

    // DOUT state (per channel)
    let do_mode = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];
    let do_srcgp = [
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
    ];
    let do_t1 = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];
    let do_t2 = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];

    // History of digital state as 0/1 floats (per channel) for the sparkline.
    let history: [RwSignal<Vec<f32>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    Effect::new(move |_| {
        let ds = state.get();
        for (i, ch) in ds.channels.iter().enumerate().take(4) {
            let v = match mode[i].get() {
                HvMode::Din => {
                    if ch.din_state {
                        1.0_f32
                    } else {
                        0.0
                    }
                }
                HvMode::Dout => {
                    if ch.do_state {
                        1.0_f32
                    } else {
                        0.0
                    }
                }
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
        <div class="view hv-io-view">
            <p class="group-subtitle">"High-voltage digital I/O. Each channel has its own In/Out selector; per-channel settings are kept when you switch."</p>

            <div class="grid-2">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let is_din_func = ch.function == 8 || ch.function == 9;
                        let hist = history[i];

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
                                            <ChannelEmpty icon="log-in" message="Set the channel to a digital input mode to monitor it.">
                                                <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 8)>"Digital in"</button>
                                            </ChannelEmpty>
                                        }.into_any()
                                    } else {
                                        view! {
                                            <div class="io-body">
                                                <div class="io-primary">
                                                    <div class="io-level io-level-sm" class:on=ch.din_state role="status">
                                                        <span class="io-lamp" aria-hidden="true"></span>
                                                        <span class="io-level-text">{if ch.din_state { "High" } else { "Low" }}</span>
                                                    </div>
                                                    <div class="io-meta">
                                                        <span>"Events"</span>
                                                        <span class="io-meta-value">{format!("{}", ch.din_counter)}</span>
                                                    </div>
                                                </div>

                                                <div class="rows io-rows">
                                                    <label class="row">
                                                        <span class="row-label">"Debounce"</span>
                                                        <select
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
                                                    </label>
                                                    <label class="row">
                                                        <span class="row-label">"Threshold"</span>
                                                        <input type="number" class="number-input" min="0" max="127" step="1"
                                                            prop:value=move || th.get().to_string()
                                                            on:change=move |ev| {
                                                                let val: u8 = event_target_value(&ev).parse().unwrap_or(64);
                                                                th.set(val);
                                                                send_din_config(ch_idx, val, db.get_untracked(), oc.get_untracked(), sc.get_untracked());
                                                            }
                                                        />
                                                    </label>
                                                </div>
                                                <details class="disclosure" prop:open=move || fault_open[i].get() on:toggle=move |e| fault_open[i].set(details_open(&e))>
                                                    <summary>"Fault detection"</summary>
                                                    <div class="rows io-rows">
                                                        <div class="row">
                                                            <span class="row-label">"Open-circuit detect"</span>
                                                            <Switch
                                                                checked=Signal::derive(move || oc.get())
                                                                on_change=Callback::new(move |new_val: bool| {
                                                                    oc.set(new_val);
                                                                    send_din_config(ch_idx, th.get_untracked(), db.get_untracked(), new_val, sc.get_untracked());
                                                                })
                                                                aria_label="Open-circuit detect"
                                                            />
                                                        </div>
                                                        <div class="row">
                                                            <span class="row-label">"Short-circuit detect"</span>
                                                            <Switch
                                                                checked=Signal::derive(move || sc.get())
                                                                on_change=Callback::new(move |new_val: bool| {
                                                                    sc.set(new_val);
                                                                    send_din_config(ch_idx, th.get_untracked(), db.get_untracked(), oc.get_untracked(), new_val);
                                                                })
                                                                aria_label="Short-circuit detect"
                                                            />
                                                        </div>
                                                    </div>
                                                </details>

                                                <ChannelSparkline
                                                    values=Signal::from(hist)
                                                    min=Signal::derive(move || -0.1f32)
                                                    max=Signal::derive(move || 1.1f32)
                                                    color_var=ch_var(i)
                                                />
                                            </div>
                                        }.into_any()
                                    }
                                }
                                HvMode::Dout => {
                                    let current = ch.do_state;
                                    view! {
                                        <div class="io-body">
                                            <div class="io-primary">
                                                <div class="io-level io-level-sm" class:on=current role="status">
                                                    <span class="io-lamp" aria-hidden="true"></span>
                                                    <span class="io-level-text">{if current { "High" } else { "Low" }}</span>
                                                </div>
                                                <SegmentedControl
                                                    options=vec![(false, "Low"), (true, "High")]
                                                    value=Signal::derive(move || current)
                                                    on_change=Callback::new(move |new_state: bool| {
                                                        if new_state == current { return; }
                                                        #[derive(Serialize)]
                                                        struct Args { channel: u8, on: bool }
                                                        let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx, on: new_state }).unwrap();
                                                        let label = format!("Set CH {} DO {}", CH_NAMES[ch_idx as usize], if new_state { "ON" } else { "OFF" });
                                                        invoke_with_feedback("set_do_state", args, &label);
                                                    })
                                                    aria_label="Output level"
                                                />
                                            </div>

                                            <div class="rows io-rows">
                                                <label class="row">
                                                    <span class="row-label">"Output mode"</span>
                                                    <select
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
                                                </label>
                                            </div>
                                            <details class="disclosure" prop:open=move || adv_open[i].get() on:toggle=move |e| adv_open[i].set(details_open(&e))>
                                                <summary>"Advanced"</summary>
                                                <div class="rows io-rows">
                                                    <div class="row">
                                                        <span class="row-label">"Source"</span>
                                                        <SegmentedControl
                                                            options=vec![(false, "SPI"), (true, "GPIO")]
                                                            value=Signal::derive(move || sg.get())
                                                            on_change=Callback::new(move |new_val: bool| {
                                                                sg.set(new_val);
                                                                send_do_config(ch_idx, dm.get_untracked(), new_val, t1.get_untracked(), t2.get_untracked());
                                                            })
                                                            aria_label="Output source"
                                                        />
                                                    </div>
                                                    <label class="row">
                                                        <span class="row-label">"T1 (µs)"</span>
                                                        <input type="number" class="number-input" min="0" max="15" step="1"
                                                            prop:value=move || t1.get().to_string()
                                                            on:change=move |e| {
                                                                let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                                t1.set(val);
                                                                send_do_config(ch_idx, dm.get_untracked(), sg.get_untracked(), val, t2.get_untracked());
                                                            }
                                                        />
                                                    </label>
                                                    <label class="row">
                                                        <span class="row-label">"T2 (µs)"</span>
                                                        <input type="number" class="number-input" min="0" max="255" step="1"
                                                            prop:value=move || t2.get().to_string()
                                                            on:change=move |e| {
                                                                let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                                t2.set(val);
                                                                send_do_config(ch_idx, dm.get_untracked(), sg.get_untracked(), t1.get_untracked(), val);
                                                            }
                                                        />
                                                    </label>
                                                </div>
                                            </details>

                                            <ChannelSparkline
                                                values=Signal::from(hist)
                                                min=Signal::derive(move || -0.1f32)
                                                max=Signal::derive(move || 1.1f32)
                                                color_var=ch_var(i)
                                            />
                                        </div>
                                    }.into_any()
                                }
                            }
                        };

                        view! {
                            <section class="group io-card io-card-compact" data-ch=ch_key(i)>
                                <ChannelHead idx=i func=func_name(ch.function)>
                                    <SegmentedControl
                                        options=vec![(HvMode::Din, "In"), (HvMode::Dout, "Out")]
                                        value=Signal::derive(move || md.get())
                                        on_change=Callback::new(move |m: HvMode| md.set(m))
                                        small=true
                                        aria_label="Direction"
                                    />
                                </ChannelHead>
                                {body}
                            </section>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
