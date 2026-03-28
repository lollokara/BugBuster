use serde::{Deserialize, Serialize};
use wasm_bindgen::prelude::*;

// -----------------------------------------------------------------------------
// Tauri JS bridge
// -----------------------------------------------------------------------------

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "core"])]
    pub async fn invoke(cmd: &str, args: JsValue) -> JsValue;

    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "event"])]
    pub async fn listen(event: &str, handler: &Closure<dyn FnMut(JsValue)>) -> JsValue;
}

// -----------------------------------------------------------------------------
// Shared types (match src-tauri/src/state.rs)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct DiscoveredDevice {
    pub id: String,
    pub name: String,
    pub transport: String,
    pub address: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ConnectionStatus {
    pub mode: String,
    pub port_or_url: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ChannelState {
    pub function: u8,
    pub adc_raw: u32,
    pub adc_value: f32,
    pub adc_range: u8,
    pub adc_rate: u8,
    pub adc_mux: u8,
    pub dac_code: u16,
    pub dac_value: f32,
    pub din_state: bool,
    pub din_counter: u32,
    pub do_state: bool,
    pub channel_alert: u16,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DeviceState {
    pub spi_ok: bool,
    pub die_temperature: f32,
    pub alert_status: u16,
    pub alert_mask: u16,
    pub supply_alert_status: u16,
    pub supply_alert_mask: u16,
    pub live_status: u16,
    pub channels: Vec<ChannelState>,
    pub diag: Vec<DiagState>,
    pub gpio: Vec<GpioState>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DiagState {
    pub source: u8,
    pub raw_code: u16,
    pub value: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct GpioState {
    pub mode: u8,
    pub output: bool,
    pub input: bool,
    pub pulldown: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct TauriEvent<T> {
    pub payload: T,
}

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

pub const CH_NAMES: [&str; 4] = ["A", "B", "C", "D"];
pub const CH_COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];

pub fn func_name(code: u8) -> &'static str {
    match code {
        0 => "HIGH_IMP", 1 => "VOUT", 2 => "IOUT", 3 => "VIN",
        4 => "IIN_EXT", 5 => "IIN_LOOP", 7 => "RES_MEAS",
        8 => "DIN_LOGIC", 9 => "DIN_LOOP", 10 => "IOUT_HART",
        11 => "IIN_EXT_HART", 12 => "IIN_LOOP_HART", _ => "UNKNOWN",
    }
}

pub const FN_OPTIONS: &[(u8, &str)] = &[
    (0, "High Impedance"), (1, "Voltage Out"), (2, "Current Out"),
    (3, "Voltage In"), (4, "Current In (Ext)"), (5, "Current In (Loop)"),
    (7, "Resistance"), (8, "Digital In (Logic)"), (9, "Digital In (Loop)"),
    (10, "Current Out HART"), (11, "Current In HART (Ext)"), (12, "Current In HART (Loop)"),
];

pub const ADC_RANGE_OPTIONS: &[(u8, &str, f32, f32)] = &[
    (0, "0 to 12V", 0.0, 12.0),
    (1, "-12 to 12V", -12.0, 12.0),
    (2, "-312.5 to 312.5mV", -0.3125, 0.3125),
    (3, "-312.5 to 0mV", -0.3125, 0.0),
    (4, "0 to 312.5mV", 0.0, 0.3125),
    (5, "0 to 625mV", 0.0, 0.625),
    (6, "-104 to 104mV", -0.104, 0.104),
    (7, "-2.5 to 2.5V", -2.5, 2.5),
];

pub const ADC_RATE_OPTIONS: &[(u8, &str)] = &[
    (0, "10 SPS HR"), (1, "20 SPS"), (3, "20 SPS HR"),
    (4, "200 SPS HR1"), (6, "200 SPS HR"), (8, "1.2 kSPS"),
    (9, "1.2 kSPS HR"), (12, "4.8 kSPS"), (13, "9.6 kSPS"),
];

pub const ADC_MUX_OPTIONS: &[(u8, &str)] = &[
    (0, "LF to AGND"), (1, "HF to LF (diff)"),
    (2, "VSENSE- to AGND"), (3, "LF to VSENSE-"),
    (4, "AGND to AGND (self-test)"),
];

pub const GPIO_MODE_OPTIONS: &[(u8, &str)] = &[
    (0, "High Impedance"), (1, "Output"), (2, "Input"),
    (3, "DIN Out"), (4, "DO Ext"),
];

pub const DIAG_SOURCE_OPTIONS: &[(u8, &str)] = &[
    (0, "AGND"), (1, "Temperature"), (2, "DVCC"), (3, "AVCC"),
    (4, "LDO 1.8V"), (5, "AVDD HI"), (6, "AVDD LO"), (7, "AVSS"),
    (8, "LVIN"), (9, "DO VDD"), (10, "VSENSE+"), (11, "VSENSE-"),
    (12, "DO Current"), (13, "AVDD"),
];

// -----------------------------------------------------------------------------
// Invoke helpers
// -----------------------------------------------------------------------------

pub fn invoke_void(cmd: &str, args: JsValue) {
    let cmd = cmd.to_string();
    leptos::task::spawn_local(async move {
        let _result = invoke(&cmd, args).await;
    });
}

pub fn log(msg: &str) {
    web_sys::console::log_1(&msg.into());
}
