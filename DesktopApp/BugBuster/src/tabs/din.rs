#![allow(dead_code)]

use crate::components::channel_sparkline::{
    ch_key, set_channel_function, ChannelEmpty, ChannelHead,
};
use crate::components::ui::Switch;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

/// DIN claims only the edited logical channel slot when a write action is sent.
pub const SLOTS: &[u8] = &[];

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
    invoke_with_io_claim("set_din_config", args, &label, &[12 + ch]);
}

#[component]
pub fn DinTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Local config state per channel (firmware doesn't report these back)
    let thresh = [
        RwSignal::new(64u8),
        RwSignal::new(64u8),
        RwSignal::new(64u8),
        RwSignal::new(64u8),
    ];
    let debounce = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];
    let oc_det = [
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
    ];
    let sc_det = [
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
    ];

    view! {
        <div class="view">
            <p class="group-subtitle">"Digital inputs on the high-voltage channels. Debounce and threshold apply per channel."</p>
            <div class="grid-2">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let is_din = ch.function == 8 || ch.function == 9;
                        let ch_idx = i as u8;

                        let body = if !is_din {
                            view! {
                                <ChannelEmpty icon="log-in" message="Set the channel to a digital input mode to monitor it.">
                                    <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 8)>"Digital in"</button>
                                </ChannelEmpty>
                            }.into_any()
                        } else {
                            let th = thresh[i];
                            let db = debounce[i];
                            let oc = oc_det[i];
                            let sc = sc_det[i];
                            view! {
                                <div class="io-body">
                                    <div class="io-primary">
                                        <div class="io-level" class:on=ch.din_state role="status">
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
                                            <span class="row-label">"Threshold"<span class="row-hint">"0 to 127"</span></span>
                                            <input type="number" class="number-input" min="0" max="127" step="1"
                                                prop:value=move || th.get().to_string()
                                                on:change=move |ev| {
                                                    let val: u8 = event_target_value(&ev).parse().unwrap_or(64);
                                                    th.set(val);
                                                    send_din_config(ch_idx, val, db.get_untracked(), oc.get_untracked(), sc.get_untracked());
                                                }
                                            />
                                        </label>
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
                                </div>
                            }.into_any()
                        };

                        view! {
                            <section class="group io-card" data-ch=ch_key(i)>
                                <ChannelHead idx=i func=func_name(ch.function) />
                                {body}
                            </section>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
