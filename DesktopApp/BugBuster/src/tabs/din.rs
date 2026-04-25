#![allow(dead_code)]

use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

const DEBOUNCE_OPTIONS: &[(u8, &str)] = &[
    (0, "None"), (1, "1ms"), (2, "2ms"), (3, "4ms"),
    (4, "8ms"), (5, "16ms"), (6, "32ms"), (7, "64ms"),
];

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

#[component]
pub fn DinTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Local config state per channel (firmware doesn't report these back)
    let thresh = [RwSignal::new(64u8), RwSignal::new(64u8), RwSignal::new(64u8), RwSignal::new(64u8)];
    let debounce = [RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8)];
    let oc_det = [RwSignal::new(false), RwSignal::new(false), RwSignal::new(false), RwSignal::new(false)];
    let sc_det = [RwSignal::new(false), RwSignal::new(false), RwSignal::new(false), RwSignal::new(false)];

    view! {
        <div class="tab-content">
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let is_din = ch.function == 8 || ch.function == 9;
                        let color = CH_COLORS[i];

                        view! {
                            <div class="card channel-card" class:ch-disabled=!is_din>
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <span class="channel-func">{func_name(ch.function)}</span>
                                </div>
                                <div class="card-body">
                                    {if !is_din {
                                        view! {
                                            <div class="mode-warning">
                                                <span class="mode-warning-icon">"ℹ"</span>
                                                <span>"Set channel to DIN mode to monitor digital input"</span>
                                            </div>
                                        }.into_any()
                                    } else {
                                        let ch_idx = i as u8;
                                        let th = thresh[i];
                                        let db = debounce[i];
                                        let oc = oc_det[i];
                                        let sc = sc_det[i];
                                        view! {
                                            <div>
                                                // State display
                                                <div class="din-display">
                                                    <div class="din-led" class:din-led-on=ch.din_state
                                                        style=if ch.din_state {
                                                            format!("background: {}; box-shadow: 0 0 20px {}", color, color)
                                                        } else { String::new() }
                                                    ></div>
                                                    <div class="din-info">
                                                        <span class="din-state-label">{if ch.din_state { "HIGH" } else { "LOW" }}</span>
                                                        <span class="din-counter">"Events: "{format!("{}", ch.din_counter)}</span>
                                                    </div>
                                                </div>

                                                // Config
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
                                                </div>
                                            </div>
                                        }.into_any()
                                    }}
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
