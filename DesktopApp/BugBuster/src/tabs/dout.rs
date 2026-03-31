use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

const DO_MODE_OPTIONS: &[(u8, &str)] = &[
    (0, "High-Z"), (1, "Push-Pull"), (2, "Open Drain"), (3, "Push-Pull HART"),
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
        channel: ch, mode, src_sel_gpio, t1, t2,
    }).unwrap();
    let label = format!("Set CH {} DO config", CH_NAMES[ch as usize]);
    invoke_with_feedback("set_do_config", args, &label);
}

#[component]
pub fn DoutTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Local config state per channel (firmware doesn't report these back)
    let do_mode = [RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8)];
    let src_gpio = [RwSignal::new(false), RwSignal::new(false), RwSignal::new(false), RwSignal::new(false)];
    let t1_val = [RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8)];
    let t2_val = [RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8), RwSignal::new(0u8)];

    view! {
        <div class="tab-content">
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let color = CH_COLORS[i];
                        let mode = do_mode[i];
                        let sg = src_gpio[i];
                        let t1 = t1_val[i];
                        let t2 = t2_val[i];

                        view! {
                            <div class="card channel-card">
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <span class="channel-func">{func_name(ch.function)}</span>
                                </div>
                                <div class="card-body">
                                    // Toggle button
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

                                    // Config
                                    <div class="config-section">
                                        <div class="config-row">
                                            <label>"DO Mode"</label>
                                            <select class="dropdown"
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
                                        </div>
                                        <div class="config-row">
                                            <label>"Source"</label>
                                            <label class="toggle-wrap">
                                                <span class="toggle-off-label">"SPI"</span>
                                                <div class="toggle" class:active=move || sg.get()
                                                    on:click=move |_| {
                                                        let new_val = !sg.get_untracked();
                                                        sg.set(new_val);
                                                        send_do_config(ch_idx, mode.get_untracked(), new_val, t1.get_untracked(), t2.get_untracked());
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
                                                    send_do_config(ch_idx, mode.get_untracked(), sg.get_untracked(), val, t2.get_untracked());
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
                                                    send_do_config(ch_idx, mode.get_untracked(), sg.get_untracked(), t1.get_untracked(), val);
                                                }
                                            />
                                        </div>
                                    </div>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
