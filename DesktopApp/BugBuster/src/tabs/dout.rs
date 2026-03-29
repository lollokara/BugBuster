use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

const DO_MODE_OPTIONS: &[(u8, &str)] = &[
    (0, "High-Z"), (1, "Push-Pull"), (2, "Open Drain"), (3, "Push-Pull HART"),
];

#[component]
pub fn DoutTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let is_dout = ch.function == 8 || ch.function == 9 || ch.function == 0; // Any function can have DO
                        let color = CH_COLORS[i];

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
                                            <select class="dropdown">
                                                {DO_MODE_OPTIONS.iter().map(|(code, name)| {
                                                    view! { <option value=code.to_string()>{*name}</option> }
                                                }).collect::<Vec<_>>()}
                                            </select>
                                        </div>
                                        <div class="config-row">
                                            <label>"Source"</label>
                                            <label class="toggle-wrap">
                                                <span class="toggle-off-label">"SPI"</span>
                                                <div class="toggle"><div class="toggle-thumb"></div></div>
                                                <span class="toggle-on-label">"GPIO"</span>
                                            </label>
                                        </div>
                                        <div class="config-row">
                                            <label>"T1 (μs)"</label>
                                            <input type="number" class="number-input" min="0" max="15" step="1" value="0" />
                                        </div>
                                        <div class="config-row">
                                            <label>"T2 (μs)"</label>
                                            <input type="number" class="number-input" min="0" max="255" step="1" value="0" />
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
