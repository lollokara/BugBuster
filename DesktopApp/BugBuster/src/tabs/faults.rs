use crate::components::icons::Icon;
use crate::components::ui::EmptyState;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

/// (bit, name, description, is_error). Errors render red, warnings orange.
type AlertBit = (usize, &'static str, &'static str, bool);

// ALERT_STATUS register (0x3F)
const GLOBAL_ALERTS: &[AlertBit] = &[
    (0, "RESET", "Device was reset", false),
    (2, "SUPPLY_ERR", "Supply alert, see the supply register", true),
    (3, "SPI_ERR", "SPI communication error", true),
    (4, "TEMP_ALERT", "Die temperature limit exceeded", false),
    (5, "ADC_ERR", "ADC conversion error", true),
    (8, "CH_A", "Channel A alert, see its register", false),
    (9, "CH_B", "Channel B alert, see its register", false),
    (10, "CH_C", "Channel C alert, see its register", false),
    (11, "CH_D", "Channel D alert, see its register", false),
    (12, "HART_A", "HART alert on channel A", false),
    (13, "HART_B", "HART alert on channel B", false),
    (14, "HART_C", "HART alert on channel C", false),
    (15, "HART_D", "HART alert on channel D", false),
];
// SUPPLY_ALERT_STATUS register (0x57)
const SUPPLY_ALERTS: &[AlertBit] = &[
    (0, "CAL_MEM", "Calibration memory error", true),
    (1, "AVSS", "AVSS supply fault", true),
    (2, "DVCC", "DVCC supply fault", true),
    (3, "AVCC", "AVCC supply fault", true),
    (4, "DO_VDD", "Digital output supply fault", true),
    (5, "AVDD_LO", "AVDD below its threshold", true),
    (6, "AVDD_HI", "AVDD above its threshold", true),
];
// CHANNEL_ALERT_STATUS register (0x58 + ch)
const CH_ALERTS: &[AlertBit] = &[
    (0, "DIN_SC", "Digital input short circuit", true),
    (1, "DIN_OC", "Digital input open circuit", false),
    (2, "DO_SC", "Digital output short circuit", true),
    (3, "DO_TIMEOUT", "Digital output timeout", false),
    (4, "AIO_SC", "Analog IO short circuit", true),
    (5, "AIO_OC", "Analog IO open circuit", false),
    (6, "VIOUT_SHDN", "VIOUT shut down", true),
];

#[component]
fn AlertRow(
    name: &'static str,
    desc: &'static str,
    is_error: bool,
    #[prop(into)] set: Signal<bool>,
) -> impl IntoView {
    view! {
        <div class="fl-row" class:is-set=move || set.get() class:is-error=is_error>
            <span class="fl-icon">
                {move || if set.get() {
                    view! { <Icon name=if is_error { "circle-alert" } else { "triangle-alert" } size=16 /> }.into_any()
                } else {
                    view! { <Icon name="circle" size=16 /> }.into_any()
                }}
            </span>
            <span class="fl-name">{name}</span>
            <span class="fl-desc">{desc}</span>
            <span class=move || if !set.get() { "badge" } else if is_error { "badge tone-red" } else { "badge tone-orange" }>
                {move || if set.get() { "Active" } else { "Clear" }}
            </span>
        </div>
    }
}

#[component]
pub fn FaultsTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let clear_all = move |_: leptos::ev::MouseEvent| {
        invoke_with_feedback(
            "clear_all_alerts",
            wasm_bindgen::JsValue::NULL,
            "Clear all alerts",
        );
    };
    let clear_ch = move |ch: u8| {
        #[derive(Serialize)]
        struct Args {
            channel: u8,
        }
        let args = serde_wasm_bindgen::to_value(&Args { channel: ch }).unwrap();
        let label = format!("Clear CH {} alerts", CH_NAMES[ch as usize]);
        invoke_with_feedback("clear_channel_alert", args, &label);
    };

    let total_faults = move || {
        let ds = state.get();
        (ds.alert_status.count_ones()
            + ds.supply_alert_status.count_ones()
            + ds.channels
                .iter()
                .map(|c| c.channel_alert.count_ones())
                .sum::<u32>()) as usize
    };

    view! {
        <div class="view view-narrow fl">
            <div class="group fl-status">
                <div class="group-header">
                    <div class="stack-sm">
                        <span class="group-title">"Alert status"</span>
                        <span class="group-subtitle">"AD74416H global, supply and per-channel alert registers"</span>
                    </div>
                    <div class="group-actions">
                        <span class=move || if total_faults() == 0 { "badge tone-green" } else { "badge tone-red" }>
                            {move || { let n = total_faults(); if n == 0 { "All clear".to_string() } else if n == 1 { "1 active fault".to_string() } else { format!("{n} active faults") } }}
                        </span>
                        <button class="btn btn-tinted tone-red btn-sm" on:click=clear_all>"Clear all"</button>
                    </div>
                </div>
                {move || (total_faults() == 0).then(|| view! {
                    <div class="fl-empty">
                        <EmptyState icon="circle-check" title="No active faults" message="All alert registers read zero." />
                    </div>
                })}
            </div>

            <div class="grid-2 fl-reg-grid">
            <div class="group group-flush">
                <div class="group-header">
                    <span class="group-title">"Global alerts"</span>
                    <span class="fl-reg">
                        "ALERT_STATUS "
                        <span class="fl-hex">{move || format!("0x{:04X}", state.get().alert_status)}</span>
                    </span>
                </div>
                <div class="fl-rows">
                    {GLOBAL_ALERTS.iter().map(|&(bit, name, desc, is_error)| {
                        view! {
                            <AlertRow name=name desc=desc is_error=is_error
                                set=Signal::derive(move || (state.get().alert_status >> bit) & 1 != 0) />
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>

            <div class="group group-flush">
                <div class="group-header">
                    <span class="group-title">"Supply alerts"</span>
                    <span class="fl-reg">
                        "SUPPLY_ALERT_STATUS "
                        <span class="fl-hex">{move || format!("0x{:04X}", state.get().supply_alert_status)}</span>
                    </span>
                </div>
                <div class="fl-rows">
                    {SUPPLY_ALERTS.iter().map(|&(bit, name, desc, is_error)| {
                        view! {
                            <AlertRow name=name desc=desc is_error=is_error
                                set=Signal::derive(move || (state.get().supply_alert_status >> bit) & 1 != 0) />
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            </div>

            <div class="grid-2 fl-ch-grid">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let has_faults = ch.channel_alert != 0;
                        let alert = ch.channel_alert;
                        view! {
                            <div class="group group-flush fl-ch" data-ch=i.to_string()>
                                <div class="group-header">
                                    <span class="group-title">
                                        <span class="fl-ch-dot"></span>
                                        {format!("Channel {} alerts", CH_NAMES[i])}
                                    </span>
                                    <div class="group-actions">
                                        <span class="fl-hex">{format!("0x{:04X}", alert)}</span>
                                        <button class="btn btn-tinted tone-red btn-sm"
                                            disabled=!has_faults
                                            aria-label=format!("Clear channel {} alerts", CH_NAMES[i])
                                            on:click=move |_| clear_ch(ch_idx)
                                        >"Clear"</button>
                                    </div>
                                </div>
                                <div class="fl-rows">
                                    {CH_ALERTS.iter().map(|&(bit, name, desc, is_error)| {
                                        let is_set = (alert >> bit) & 1 != 0;
                                        view! { <AlertRow name=name desc=desc is_error=is_error set=Signal::derive(move || is_set) /> }
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
