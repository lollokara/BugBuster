use leptos::prelude::*;
use crate::tauri_bridge::*;

#[component]
pub fn DinTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        view! {
                            <div class="card channel-card">
                                <div class="card-header">
                                    <span class="channel-label">{format!("CH {}", CH_NAMES[i])}</span>
                                    <span class="channel-func">"DIN"</span>
                                </div>
                                <div class="card-body">
                                    <div class="din-display">
                                        <div class="led led-large"
                                            class:led-on=ch.din_state
                                            style=if ch.din_state {
                                                "background: var(--green); box-shadow: 0 0 16px var(--green)"
                                            } else { "" }
                                        ></div>
                                        <span class="din-state-label">
                                            {if ch.din_state { "ON" } else { "OFF" }}
                                        </span>
                                    </div>
                                    <div class="card-details">
                                        <span>"Counter: "{format!("{}", ch.din_counter)}</span>
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
