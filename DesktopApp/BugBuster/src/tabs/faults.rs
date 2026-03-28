use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

const GLOBAL_ALERTS: &[(usize, &str)] = &[
    (0, "RESET"), (1, "CAL_MEM"), (2, "SPI_CRC"), (3, "SPI_SCLK"), (4, "ADC_ERR"),
    (5, "SUPPLY"), (6, "TEMP"), (7, "CH_D"), (8, "CH_C"), (9, "CH_B"), (10, "CH_A"),
];
const SUPPLY_ALERTS: &[(usize, &str)] = &[
    (0, "AVDD_HI"), (1, "AVDD_LO"), (2, "AVSS"), (3, "AVCC"), (4, "DVCC"), (5, "IOVDD"), (6, "REFIO"),
];
const CH_ALERTS: &[(usize, &str)] = &[
    (0, "VIN_LO"), (1, "VIN_HI"), (2, "OC"), (3, "SC"), (4, "DO_SC"),
    (5, "DIN_OC"), (6, "DIN_SC"), (7, "DAC"), (8, "AVDD"), (9, "DVCC"),
];

#[component]
pub fn FaultsTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let clear_all = move |_: leptos::ev::MouseEvent| {
        invoke_void("clear_all_alerts", wasm_bindgen::JsValue::NULL);
    };
    let clear_ch = move |ch: u8| {
        #[derive(Serialize)]
        struct Args { channel: u8 }
        let args = serde_wasm_bindgen::to_value(&Args { channel: ch }).unwrap();
        invoke_void("clear_channel_alert", args);
    };

    let total_faults = move || {
        let ds = state.get();
        (ds.alert_status.count_ones() + ds.supply_alert_status.count_ones() +
         ds.channels.iter().map(|c| c.channel_alert.count_ones()).sum::<u32>()) as usize
    };

    view! {
        <div class="tab-content fault-tab">
            // Top bar
            <div class="fault-topbar">
                <div class="fault-indicator-big"
                    style={move || if total_faults() > 0 {
                        "background: var(--rose); box-shadow: 0 0 20px var(--rose)"
                    } else {
                        "background: var(--green); box-shadow: 0 0 12px var(--green)"
                    }}
                ></div>
                <div class="fault-summary-text">
                    <span class="fault-total">{move || total_faults()}</span>
                    " active "
                    {move || if total_faults() == 1 { "fault" } else { "faults" }}
                </div>
                <div style="flex:1"></div>
                <button class="scope-btn" on:click=clear_all>"Clear All"</button>
            </div>

            // Register strips
            <div class="fault-strips">
                // Global
                <div class="fault-strip">
                    <div class="fault-strip-header">
                        <span class="fault-strip-title">"Global"</span>
                        <span class="badge-hex">{move || format!("0x{:04X}", state.get().alert_status)}</span>
                    </div>
                    <div class="fault-strip-bits">
                        {GLOBAL_ALERTS.iter().map(|(bit, name)| {
                            let bit = *bit;
                            view! {
                                <div class="fault-bit" class:fault-bit-set={move || (state.get().alert_status >> bit) & 1 != 0}>
                                    <div class="fault-bit-dot"></div>
                                    <span class="fault-bit-name">{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                // Supply
                <div class="fault-strip">
                    <div class="fault-strip-header">
                        <span class="fault-strip-title">"Supply"</span>
                        <span class="badge-hex">{move || format!("0x{:04X}", state.get().supply_alert_status)}</span>
                    </div>
                    <div class="fault-strip-bits">
                        {SUPPLY_ALERTS.iter().map(|(bit, name)| {
                            let bit = *bit;
                            view! {
                                <div class="fault-bit" class:fault-bit-set={move || (state.get().supply_alert_status >> bit) & 1 != 0}>
                                    <div class="fault-bit-dot"></div>
                                    <span class="fault-bit-name">{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>
            </div>

            // Per-channel strips
            <div class="fault-ch-grid">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let color = CH_COLORS[i];
                        let has_faults = ch.channel_alert != 0;
                        view! {
                            <div class="fault-strip" class:fault-strip-active=has_faults>
                                <div class="fault-strip-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <div style="display:flex; align-items:center; gap:6px;">
                                        <span class="badge-hex">{format!("0x{:04X}", ch.channel_alert)}</span>
                                        {if has_faults { Some(view! {
                                            <button class="fault-clear-btn" on:click=move |_| clear_ch(ch_idx)>"Clear"</button>
                                        })} else { None }}
                                    </div>
                                </div>
                                <div class="fault-strip-bits">
                                    {CH_ALERTS.iter().map(|(bit, name)| {
                                        let is_set = (ch.channel_alert >> bit) & 1 != 0;
                                        view! {
                                            <div class="fault-bit" class:fault-bit-set=is_set>
                                                <div class="fault-bit-dot"></div>
                                                <span class="fault-bit-name">{*name}</span>
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
