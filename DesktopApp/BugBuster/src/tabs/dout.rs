#![allow(dead_code)]

use crate::components::channel_sparkline::{ch_key, ChannelHead};
use crate::components::ui::SegmentedControl;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

/// DOUT claims only the edited logical channel slot when a write action is sent.
pub const SLOTS: &[u8] = &[];

const DO_MODE_OPTIONS: &[(u8, &str)] = &[
    (0, "High-Z"),
    (1, "Push-Pull"),
    (2, "Open Drain"),
    (3, "Push-Pull HART"),
];

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
    invoke_with_io_claim("set_do_config", args, &label, &[12 + ch]);
}

#[component]
pub fn DoutTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Local config state per channel (firmware doesn't report these back)
    let do_mode = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];
    let src_gpio = [
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
        RwSignal::new(false),
    ];
    let t1_val = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];
    let t2_val = [
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
        RwSignal::new(0u8),
    ];

    view! {
        <div class="view">
            <p class="group-subtitle">"Digital outputs on the high-voltage channels. Drive each channel high or low and set its output stage."</p>
            <div class="grid-2">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let mode = do_mode[i];
                        let sg = src_gpio[i];
                        let t1 = t1_val[i];
                        let t2 = t2_val[i];
                        let current = ch.do_state;

                        view! {
                            <section class="group io-card" data-ch=ch_key(i)>
                                <ChannelHead idx=i func=func_name(ch.function) />
                                <div class="io-body">
                                    <div class="io-primary">
                                        <div class="io-level" class:on=current role="status">
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
                                                invoke_with_io_claim("set_do_state", args, &label, &[12 + ch_idx]);
                                            })
                                            aria_label="Output level"
                                        />
                                    </div>

                                    <div class="rows io-rows">
                                        <label class="row">
                                            <span class="row-label">"Output mode"</span>
                                            <select
                                                prop:value=move || mode.get().to_string()
                                                on:change=move |e| {
                                                    let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                    mode.set(val);
                                                    send_do_config(ch_idx, val, sg.get_untracked(), t1.get_untracked(), t2.get_untracked());
                                                }
                                            >
                                                {DO_MODE_OPTIONS.iter().map(|(code, name)| {
                                                    view! { <option value=code.to_string()>{*name}</option> }
                                                }).collect::<Vec<_>>()}
                                            </select>
                                        </label>
                                        <div class="row">
                                            <span class="row-label">"Source"</span>
                                            <SegmentedControl
                                                options=vec![(false, "SPI"), (true, "GPIO")]
                                                value=Signal::derive(move || sg.get())
                                                on_change=Callback::new(move |new_val: bool| {
                                                    sg.set(new_val);
                                                    send_do_config(ch_idx, mode.get_untracked(), new_val, t1.get_untracked(), t2.get_untracked());
                                                })
                                                aria_label="Output source"
                                            />
                                        </div>
                                        <label class="row">
                                            <span class="row-label">"T1"<span class="row-hint">"0 to 15 µs"</span></span>
                                            <input type="number" class="number-input" min="0" max="15" step="1"
                                                prop:value=move || t1.get().to_string()
                                                on:change=move |e| {
                                                    let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                    t1.set(val);
                                                    send_do_config(ch_idx, mode.get_untracked(), sg.get_untracked(), val, t2.get_untracked());
                                                }
                                            />
                                        </label>
                                        <label class="row">
                                            <span class="row-label">"T2"<span class="row-hint">"0 to 255 µs"</span></span>
                                            <input type="number" class="number-input" min="0" max="255" step="1"
                                                prop:value=move || t2.get().to_string()
                                                on:change=move |e| {
                                                    let val: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                    t2.set(val);
                                                    send_do_config(ch_idx, mode.get_untracked(), sg.get_untracked(), t1.get_untracked(), val);
                                                }
                                            />
                                        </label>
                                    </div>
                                </div>
                            </section>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
