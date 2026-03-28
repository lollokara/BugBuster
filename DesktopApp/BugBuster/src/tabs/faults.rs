use leptos::prelude::*;
use crate::tauri_bridge::*;

const ALERT_BITS: &[&str] = &[
    "RESET", "CAL_MEM", "SPI_CRC", "SPI_SCLK", "ADC_ERR",
    "SUPPLY", "TEMP", "CH_D", "CH_C", "CH_B", "CH_A",
];
const SUPPLY_BITS: &[&str] = &[
    "AVDD_HI", "AVDD_LO", "AVSS", "AVCC", "DVCC", "IOVDD", "REFIO",
];
const CH_ALERT_BITS: &[&str] = &[
    "VIN_UNDER", "VIN_OVER", "IOUT_OC", "IOUT_SC", "DO_SC",
    "DIN_OC", "DIN_SC", "DAC_RNG", "AVDD", "DVCC",
];

#[component]
pub fn FaultsTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let clear_all = move |_: leptos::ev::MouseEvent| {
        invoke_void("clear_all_alerts", wasm_bindgen::JsValue::NULL);
    };

    view! {
        <div class="tab-content">
            <div class="fault-header">
                <h3>"Fault Monitor"</h3>
                <button class="btn btn-danger btn-sm" on:click=clear_all>"Clear All"</button>
            </div>

            <div class="fault-grid">
                // Global alerts
                <div class="card">
                    <div class="card-header"><span>"Alert Status"</span></div>
                    <div class="card-body led-grid">
                        {ALERT_BITS.iter().enumerate().map(|(bit, name)| {
                            view! {
                                <div class="led-wrap">
                                    <div class="led"
                                        class:led-on=move || (state.get().alert_status >> bit) & 1 != 0
                                        style=move || if (state.get().alert_status >> bit) & 1 != 0 {
                                            "background: var(--amber); box-shadow: 0 0 8px var(--amber)".to_string()
                                        } else { String::new() }
                                    ></div>
                                    <span class="led-label">{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                // Supply alerts
                <div class="card">
                    <div class="card-header"><span>"Supply Alert"</span></div>
                    <div class="card-body led-grid">
                        {SUPPLY_BITS.iter().enumerate().map(|(bit, name)| {
                            view! {
                                <div class="led-wrap">
                                    <div class="led"
                                        class:led-on=move || (state.get().supply_alert_status >> bit) & 1 != 0
                                        style=move || if (state.get().supply_alert_status >> bit) & 1 != 0 {
                                            "background: var(--rose); box-shadow: 0 0 8px var(--rose)".to_string()
                                        } else { String::new() }
                                    ></div>
                                    <span class="led-label">{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>
            </div>

            // Per-channel alerts
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        view! {
                            <div class="card">
                                <div class="card-header">
                                    <span>{format!("CH {} Alerts", CH_NAMES[i])}</span>
                                    <span class="channel-func">{format!("0x{:04X}", ch.channel_alert)}</span>
                                </div>
                                <div class="card-body led-grid">
                                    {CH_ALERT_BITS.iter().enumerate().map(|(bit, name)| {
                                        let is_set = (ch.channel_alert >> bit) & 1 != 0;
                                        view! {
                                            <div class="led-wrap">
                                                <div class="led" class:led-on=is_set
                                                    style=if is_set {
                                                        "background: var(--rose); box-shadow: 0 0 8px var(--rose)"
                                                    } else { "" }
                                                ></div>
                                                <span class="led-label">{*name}</span>
                                            </div>
                                        }
                                    }).collect::<Vec<_>>()}
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
