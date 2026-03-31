use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

#[component]
pub fn OverviewTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let reset = move |_: leptos::ev::MouseEvent| {
        invoke_with_feedback("device_reset", wasm_bindgen::JsValue::NULL, "Device reset");
    };

    view! {
        <div class="tab-content">
            <div class="tab-desc">"Device status dashboard. Set each channel's function mode and monitor live ADC/DAC values. SPI health and die temperature shown in the header."</div>
            // Summary banner
            <div class="summary-banner">
                <div class="summary-item">
                    <div class="summary-icon" class:ok=move || state.get().spi_ok class:err=move || !state.get().spi_ok>
                        {move || if state.get().spi_ok { "✓" } else { "✗" }}
                    </div>
                    <div class="summary-detail">
                        <span class="summary-label">"SPI Bus"</span>
                        <span class="summary-value" class:ok=move || state.get().spi_ok class:err=move || !state.get().spi_ok>
                            {move || if state.get().spi_ok { "OK" } else { "ERROR" }}
                        </span>
                    </div>
                </div>
                <div class="summary-item">
                    <div class="summary-icon temp">"🌡"</div>
                    <div class="summary-detail">
                        <span class="summary-label">"Die Temp"</span>
                        <span class="summary-value temp">{move || format!("{:.1}°C", state.get().die_temperature)}</span>
                    </div>
                </div>
                <div class="summary-item">
                    <div class="summary-icon" class:err=move || state.get().alert_status != 0>
                        {move || if state.get().alert_status != 0 { "⚠" } else { "✓" }}
                    </div>
                    <div class="summary-detail">
                        <span class="summary-label">"Alerts"</span>
                        <span class="summary-value" class:err=move || state.get().alert_status != 0>
                            {move || {
                                let s = state.get().alert_status;
                                if s == 0 { "None".to_string() } else { format!("0x{:04X}", s) }
                            }}
                        </span>
                    </div>
                </div>
                <div class="summary-item">
                    <div class="summary-icon" class:err=move || state.get().supply_alert_status != 0>
                        {move || if state.get().supply_alert_status != 0 { "⚠" } else { "✓" }}
                    </div>
                    <div class="summary-detail">
                        <span class="summary-label">"Supply"</span>
                        <span class="summary-value" class:err=move || state.get().supply_alert_status != 0>
                            {move || {
                                let s = state.get().supply_alert_status;
                                if s == 0 { "OK".to_string() } else { format!("0x{:04X}", s) }
                            }}
                        </span>
                    </div>
                </div>
                <div class="summary-item summary-reset">
                    <button class="reset-btn" on:click=reset>
                        <span class="reset-icon">"↻"</span>
                        <span>"Reset"</span>
                    </button>
                </div>
            </div>

            // Channel overview
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let fn_label = func_name(ch.function);
                        let is_active = ch.function != 0;
                        let is_res = ch.function == 7;
                        let unit = if ch.function == 4 || ch.function == 5 { "mA" } else if is_res { "Ω" } else { "V" };
                        // For RES_MEAS use resistance range; for others use voltage/current span
                        let range_max: f64 = if is_res {
                            // max_r = rng_max_v / I_exc; use rng from ADC range code
                            let rng_max_v: f64 = match ch.adc_range { 0 => 12.0, 1 => 12.0, 7 => 2.5, 5 => 0.625, _ => 0.3125 };
                            let i_exc = if ch.rtd_excitation_ua > 0 { ch.rtd_excitation_ua as f64 * 1e-6 } else { 250e-6 };
                            rng_max_v / i_exc
                        } else {
                            match ch.adc_range { 0 => 12.0, 1 => 12.0, 7 => 2.5, _ => 0.625 }
                        };
                        let pct = if range_max > 0.0 { (ch.adc_value.abs() as f64 / range_max * 100.0).min(100.0) } else { 0.0 };
                        let color = CH_COLORS[i];

                        view! {
                            <div class="card channel-card" class:ch-active=is_active>
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <span class="channel-func">{fn_label}</span>
                                </div>
                                <div class="card-body">
                                    <div class="big-value" class:dimmed=!is_active>
                                        {if is_active { format!("{:.4}", ch.adc_value) } else { "---".to_string() }}
                                        <span class="unit">{if is_active { unit } else { "" }}</span>
                                    </div>
                                    <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                        <div class="bar-fill-dynamic" style=format!("width: {}%", if is_active { pct } else { 0.0 })></div>
                                    </div>
                                    <div class="card-details">
                                        <span>"DAC: "{format!("{:.3}", ch.dac_value)}</span>
                                        <span>"Raw: 0x"{format!("{:06X}", ch.adc_raw)}</span>
                                    </div>

                                    // Function selector
                                    <div class="config-row" style="margin-top: 8px;">
                                        <label>"Function"</label>
                                        <select class="dropdown"
                                            prop:value=ch.function.to_string()
                                            on:change=move |e| {
                                                let func: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                #[derive(Serialize)]
                                                struct Args { channel: u8, function: u8 }
                                                let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx, function: func }).unwrap();
                                                let label = format!("Set CH {} to {}", CH_NAMES[ch_idx as usize], func_name(func));
                                                invoke_with_feedback("set_channel_function", args, &label);
                                            }
                                        >
                                            {FN_OPTIONS.iter().map(|(code, name)| {
                                                view! { <option value=code.to_string()>{*name}</option> }
                                            }).collect::<Vec<_>>()}
                                        </select>
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
