use leptos::prelude::*;
use crate::tauri_bridge::*;

const ALERT_BITS: &[&str] = &[
    "RESET", "CAL_MEM_ERR", "SPI_CRC_ERR", "SPI_SCLK_ERR", "ADC_ERR",
    "SUPPLY_ERR", "TEMP_ALERT", "CH_ALERT_D", "CH_ALERT_C", "CH_ALERT_B", "CH_ALERT_A",
];

const SUPPLY_BITS: &[&str] = &[
    "AVDD_HI", "AVDD_LO", "AVSS_ERR", "AVCC_ERR", "DVCC_ERR", "IOVDD_ERR", "REFIO_ERR",
];

#[component]
pub fn DiagTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            <div class="diag-layout">
                // Temperature
                <div class="card temp-card">
                    <div class="card-header"><span>"Die Temperature"</span></div>
                    <div class="card-body temp-body">
                        <div class="temp-value-large">
                            {move || format!("{:.1}", state.get().die_temperature)}
                            <span class="unit">"°C"</span>
                        </div>
                        <div class="temp-bar-container">
                            <div class="temp-bar-fill" style=move || {
                                let t = state.get().die_temperature;
                                let pct = ((t + 25.0) / 150.0 * 100.0).clamp(0.0, 100.0);
                                let color = if t > 100.0 { "var(--rose)" } else if t > 70.0 { "var(--amber)" } else { "var(--green)" };
                                format!("height: {}%; background: {}", pct, color)
                            }></div>
                        </div>
                        <div class="temp-status" style=move || {
                            let t = state.get().die_temperature;
                            let color = if t > 100.0 { "var(--rose)" } else if t > 70.0 { "var(--amber)" } else { "var(--green)" };
                            format!("color: {}", color)
                        }>
                            {move || {
                                let t = state.get().die_temperature;
                                if t > 100.0 { "HOT" } else if t > 70.0 { "Warm" } else { "Normal" }
                            }}
                        </div>
                    </div>
                </div>

                // Status LEDs
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

            // Diagnostic slots
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    ds.diag.into_iter().enumerate().map(|(i, d)| {
                        let source_name = DIAG_SOURCE_OPTIONS.iter()
                            .find(|(c, _)| *c == d.source)
                            .map(|(_, n)| *n).unwrap_or("?");
                        let unit = if d.source == 1 { "°C" } else { "V" };
                        view! {
                            <div class="card">
                                <div class="card-header">
                                    <span>{format!("Diag Slot {}", i)}</span>
                                    <span class="channel-func">{source_name}</span>
                                </div>
                                <div class="card-body">
                                    <div class="big-value">{format!("{:.3}", d.value)}<span class="unit">{unit}</span></div>
                                    <div class="card-details">
                                        <span>"Raw: 0x"{format!("{:04X}", d.raw_code)}</span>
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
