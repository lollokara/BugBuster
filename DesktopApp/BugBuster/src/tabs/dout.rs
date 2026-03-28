use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

#[component]
pub fn DoutTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        view! {
                            <div class="card channel-card">
                                <div class="card-header">
                                    <span class="channel-label">{format!("CH {}", CH_NAMES[i])}</span>
                                    <span class="channel-func">"DOUT"</span>
                                </div>
                                <div class="card-body">
                                    <div class="do-display">
                                        <button class="do-toggle" class:do-on=ch.do_state
                                            on:click=move |_| {
                                                #[derive(Serialize)]
                                                struct Args { channel: u8, on: bool }
                                                let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx, on: !ch.do_state }).unwrap();
                                                invoke_void("set_do_state", args);
                                            }
                                        >
                                            {if ch.do_state { "ON" } else { "OFF" }}
                                        </button>
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
