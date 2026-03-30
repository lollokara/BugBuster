use leptos::prelude::*;
use crate::tauri_bridge::*;

const DEBOUNCE_OPTIONS: &[(u8, &str)] = &[
    (0, "None"), (1, "1ms"), (2, "2ms"), (3, "4ms"),
    (4, "8ms"), (5, "16ms"), (6, "32ms"), (7, "64ms"),
];

#[component]
pub fn DinTab(state: ReadSignal<DeviceState>) -> impl IntoView {
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
                                                        <select class="dropdown">
                                                            {DEBOUNCE_OPTIONS.iter().map(|(code, name)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"Threshold"</label>
                                                        <input type="number" class="number-input" min="0" max="127" step="1" value="64" />
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"OC Detect"</label>
                                                        <div class="toggle"><div class="toggle-thumb"></div></div>
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"SC Detect"</label>
                                                        <div class="toggle"><div class="toggle-thumb"></div></div>
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
