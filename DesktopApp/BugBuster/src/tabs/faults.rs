use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

// ALERT_STATUS register (0x3F)
const GLOBAL_ALERTS: &[(usize, &str)] = &[
    (0, "RESET"), (2, "SUPPLY_ERR"), (3, "SPI_ERR"), (4, "TEMP_ALERT"), (5, "ADC_ERR"),
    (8, "CH_A"), (9, "CH_B"), (10, "CH_C"), (11, "CH_D"),
    (12, "HART_A"), (13, "HART_B"), (14, "HART_C"), (15, "HART_D"),
];
// SUPPLY_ALERT_STATUS register (0x57)
const SUPPLY_ALERTS: &[(usize, &str)] = &[
    (0, "CAL_MEM"), (1, "AVSS"), (2, "DVCC"), (3, "AVCC"), (4, "DO_VDD"), (5, "AVDD_LO"), (6, "AVDD_HI"),
];
// CHANNEL_ALERT_STATUS register (0x58 + ch)
const CH_ALERTS: &[(usize, &str)] = &[
    (0, "DIN_SC"), (1, "DIN_OC"), (2, "DO_SC"), (3, "DO_TIMEOUT"),
    (4, "AIO_SC"), (5, "AIO_OC"), (6, "VIOUT_SHDN"),
];

#[component]
pub fn FaultsTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let clear_all = move |_: leptos::ev::MouseEvent| {
        invoke_with_feedback("clear_all_alerts", wasm_bindgen::JsValue::NULL, "Clear all alerts");
    };
    let clear_ch = move |ch: u8| {
        #[derive(Serialize)]
        struct Args { channel: u8 }
        let args = serde_wasm_bindgen::to_value(&Args { channel: ch }).unwrap();
        let label = format!("Clear CH {} alerts", CH_NAMES[ch as usize]);
        invoke_with_feedback("clear_channel_alert", args, &label);
    };

    let total_faults = move || {
        let ds = state.get();
        (ds.alert_status.count_ones() + ds.supply_alert_status.count_ones() +
         ds.channels.iter().map(|c| c.channel_alert.count_ones()).sum::<u32>()) as usize
    };

    view! {
        <div class="tab-content fault-tab">
            <div class="tab-desc">"AD74416H fault and alert monitoring. Shows global and per-channel alert flags. Clear individual or all alerts. Monitor for overcurrent, open-wire, and thermal faults."</div>
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
