#![allow(dead_code)]

use serde::{Deserialize, Serialize};
use wasm_bindgen::prelude::*;

// -----------------------------------------------------------------------------
// Tauri JS bridge
// -----------------------------------------------------------------------------

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "core"], catch)]
    pub async fn invoke(cmd: &str, args: JsValue) -> Result<JsValue, JsValue>;

    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "event"])]
    pub async fn listen(event: &str, handler: &Closure<dyn FnMut(JsValue)>) -> JsValue;
}

/// Safe invoke that returns None instead of panicking on error.
pub async fn try_invoke(cmd: &str, args: JsValue) -> Option<JsValue> {
    match invoke(cmd, args).await {
        Ok(val) => Some(val),
        Err(e) => {
            web_sys::console::warn_2(&format!("[try_invoke] {} rejected:", cmd).into(), &e);
            None
        }
    }
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
    #[serde(default)]
    pub channel_alert_mask: u16,
    #[serde(default)]
    pub rtd_excitation_ua: u16, // RTD excitation current in µA (500 or 1000; 0 when not RES_MEAS)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceState {
    pub spi_ok: bool,
    pub die_temperature: f32,
    pub alert_status: u16,
    pub alert_mask: u16,
    pub supply_alert_status: u16,
    pub supply_alert_mask: u16,
    pub live_status: u16,
    pub channels: [ChannelState; 4],
    pub diag: [DiagState; 4],
    pub gpio: [GpioState; 12],
    pub mux_states: [u8; 4],
}

impl Default for DeviceState {
    fn default() -> Self {
        Self {
            spi_ok: false,
            die_temperature: 0.0,
            alert_status: 0,
            alert_mask: 0,
            supply_alert_status: 0,
            supply_alert_mask: 0,
            live_status: 0,
            channels: std::array::from_fn(|_| ChannelState::default()),
            diag: std::array::from_fn(|_| DiagState::default()),
            gpio: std::array::from_fn(|_| GpioState::default()),
            mux_states: [0u8; 4],
        }
    }
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
        0 => "HIGH_IMP",
        1 => "VOUT",
        2 => "IOUT",
        3 => "VIN",
        4 => "IIN_EXT",
        5 => "IIN_LOOP",
        7 => "RES_MEAS",
        8 => "DIN_LOGIC",
        9 => "DIN_LOOP",
        10 => "IOUT_HART",
        11 => "IIN_EXT_HART",
        12 => "IIN_LOOP_HART",
        _ => "UNKNOWN",
    }
}

pub const FN_OPTIONS: &[(u8, &str)] = &[
    (0, "High Impedance"),
    (1, "Voltage Out"),
    (2, "Current Out"),
    (3, "Voltage In"),
    (4, "Current In (Ext)"),
    (5, "Current In (Loop)"),
    (7, "Resistance"),
    (8, "Digital In (Logic)"),
    (9, "Digital In (Loop)"),
    (10, "Current Out HART"),
    (11, "Current In HART (Ext)"),
    (12, "Current In HART (Loop)"),
];

pub const ADC_RANGE_OPTIONS: &[(u8, &str, f32, f32)] = &[
    (0, "0 to 12V", 0.0, 12.0),
    (1, "-12 to 12V", -12.0, 12.0),
    (2, "-312.5 to 312.5mV", -0.3125, 0.3125),
    (3, "-312.5 to 0mV", -0.3125, 0.0),
    (4, "0 to 312.5mV", 0.0, 0.3125),
    (5, "0 to 625mV", 0.0, 0.625),
    (6, "-104.16 to 104.16mV", -0.104167, 0.104167),
    (7, "-2.5 to 2.5V", -2.5, 2.5),
];

pub const ADC_RATE_OPTIONS: &[(u8, &str)] = &[
    (0, "10 SPS HR"),
    (1, "20 SPS"),
    (3, "20 SPS HR"),
    (4, "200 SPS HR1"),
    (6, "200 SPS HR"),
    (8, "1.2 kSPS"),
    (9, "1.2 kSPS HR"),
    (12, "4.8 kSPS"),
    (13, "9.6 kSPS"),
];

pub const ADC_MUX_OPTIONS: &[(u8, &str)] = &[
    (0, "LF to AGND"),
    (1, "HF to LF (diff)"),
    (2, "VSENSE- to AGND"),
    (3, "LF to VSENSE-"),
    (4, "AGND to AGND (self-test)"),
];

pub const GPIO_MODE_OPTIONS: &[(u8, &str)] = &[
    (0, "High Impedance"),
    (1, "Output"),
    (2, "Input"),
    (3, "DIN Out"),
    (4, "DO Ext"),
];

pub const DIAG_SOURCE_OPTIONS: &[(u8, &str)] = &[
    (0, "AGND"),
    (1, "Temperature"),
    (2, "DVCC"),
    (3, "AVCC"),
    (4, "LDO 1.8V"),
    (5, "AVDD HI"),
    (6, "AVDD LO"),
    (7, "AVSS"),
    (8, "LVIN"),
    (9, "DO VDD"),
    (10, "VSENSE+"),
    (11, "VSENSE-"),
    (12, "DO Current"),
    (13, "AVDD"),
];

// RTD excitation current options (RTD_CURRENT bit: 0 = 500 µA, 1 = 1000 µA / 1 mA)
// Per AD74416H datasheet Table 6 — stored as µA; used in RES_MEAS mode.
pub const RTD_EXCITATION_OPTIONS: &[(u16, &str)] = &[(500, "500 µA"), (1000, "1 mA")];

/// Send a command with feedback — shows toast on success/failure.
/// `label` is a human-readable description like "Set ADC range to ±12V".
pub fn invoke_with_feedback(cmd: &str, args: JsValue, label: &str) {
    let cmd = cmd.to_string();
    let label = label.to_string();
    leptos::task::spawn_local(async move {
        match try_invoke(&cmd, args).await {
            Some(result) => {
                let result_str = js_sys::JSON::stringify(&result)
                    .map(|s| s.as_string().unwrap_or_default())
                    .unwrap_or_default();
                if result_str.contains("error")
                    || result_str.contains("Error")
                    || result_str.contains("timeout")
                {
                    show_toast(&format!("Failed: {}", label), "err");
                    log(&format!("CMD FAIL [{}]: {}", cmd, result_str));
                } else {
                    show_toast(&label.to_string(), "ok");
                }
            }
            None => {
                show_toast(&format!("Failed: {}", label), "err");
                log(&format!("CMD FAIL [{}]: command rejected", cmd));
            }
        }
    });
}

pub fn invoke_with_io_claim(cmd: &str, args: JsValue, label: &str, slots: &[u8]) {
    let cmd = cmd.to_string();
    let label = label.to_string();
    let slots = slots.to_vec();
    leptos::task::spawn_local(async move {
        if !io_claim(&slots, 5000, &cmd).await {
            show_toast(&format!("IO held by another interface: {}", format_slots(&slots)), "err");
            return;
        }

        let ok = match try_invoke(&cmd, args).await {
            Some(result) => {
                let result_str = js_sys::JSON::stringify(&result)
                    .map(|s| s.as_string().unwrap_or_default())
                    .unwrap_or_default();
                !(result_str.contains("error")
                    || result_str.contains("Error")
                    || result_str.contains("timeout"))
            }
            None => false,
        };

        io_release(&slots).await;
        if ok {
            show_toast(&label, "ok");
        } else {
            show_toast(&format!("Failed: {}", label), "err");
            log(&format!("CMD FAIL [{}]: command rejected", cmd));
        }
    });
}

fn format_slots(slots: &[u8]) -> String {
    slots
        .iter()
        .map(|slot| {
            if *slot < 12 {
                format!("IO{}", slot + 1)
            } else {
                format!("CH{}", slot - 12)
            }
        })
        .collect::<Vec<_>>()
        .join(", ")
}

// Global toast system using a JS custom event
pub fn show_toast(msg: &str, kind: &str) {
    if let Some(window) = web_sys::window() {
        let detail = js_sys::Object::new();
        js_sys::Reflect::set(&detail, &"msg".into(), &msg.into()).ok();
        js_sys::Reflect::set(&detail, &"kind".into(), &kind.into()).ok();
        let init = web_sys::CustomEventInit::new();
        init.set_detail(&detail);
        if let Ok(event) = web_sys::CustomEvent::new_with_event_init_dict("bb-toast", &init) {
            window.dispatch_event(&event).ok();
        }
    }
}

pub fn log(msg: &str) {
    web_sys::console::log_1(&msg.into());
}

// -----------------------------------------------------------------------------
// Supply Voltages Cache
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SelftestStatus {
    pub boot: SelftestBootStatus,
    pub cal: SelftestCalStatus,
    #[serde(default)]
    pub worker_enabled: bool,
    #[serde(default)]
    pub supply_monitor_active: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SelftestBootStatus {
    pub ran: bool,
    pub passed: bool,
    pub vadj1_v: f32,
    pub vadj2_v: f32,
    pub vlogic_v: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SelftestCalStatus {
    pub status: u8,
    pub channel: u8,
    pub points: u8,
    pub last_voltage_v: f32,
    pub error_mv: f32,
}

pub async fn fetch_selftest_status() -> Option<SelftestStatus> {
    let result = try_invoke("selftest_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn fetch_selftest_worker_enabled() -> Option<bool> {
    let result = try_invoke("selftest_worker_get", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

/// Awaitable version of selftest_worker_set — returns the confirmed enabled state
/// from the device (or None on error). Use this instead of send_selftest_worker
/// when the caller needs to know the actual outcome before updating UI.
pub async fn fetch_selftest_worker_set(enabled: bool) -> Option<bool> {
    #[derive(Serialize)]
    struct Args {
        enabled: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args { enabled }).unwrap();
    let result = try_invoke("selftest_worker_set", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SupplyRail {
    pub rail: u8,
    pub name: String,
    pub voltage_v: f32,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SelftestSuppliesCached {
    pub available: bool,
    pub timestamp_ms: u32,
    pub rails: Vec<SupplyRail>,
}

pub async fn fetch_selftest_supplies_cached() -> Option<SelftestSuppliesCached> {
    let result = try_invoke("selftest_supplies_cached", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

// -----------------------------------------------------------------------------
// Quick Setups
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct QuickSetupSlot {
    pub index: u8,
    pub occupied: bool,
    pub summary_hash: u8,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct QuickSetupList {
    pub supported: bool,
    pub slots: Vec<QuickSetupSlot>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct QuickSetupPayload {
    pub slot: u8,
    pub json: String,
    pub name: Option<String>,
    pub byte_len: usize,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct QuickSetupActionResult {
    pub slot: u8,
    pub status: u8,
    pub ok: bool,
    pub message: String,
}

pub async fn fetch_quicksetup_list() -> Option<QuickSetupList> {
    let result = try_invoke("quicksetup_list", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn quicksetup_get_slot(slot: u8) -> Option<QuickSetupPayload> {
    #[derive(Serialize)]
    struct Args {
        slot: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { slot }).unwrap();
    let result = try_invoke("quicksetup_get", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn quicksetup_save_slot(slot: u8) -> Option<QuickSetupPayload> {
    #[derive(Serialize)]
    struct Args {
        slot: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { slot }).unwrap();
    let result = try_invoke("quicksetup_save", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn quicksetup_apply_slot(slot: u8) -> Option<QuickSetupActionResult> {
    #[derive(Serialize)]
    struct Args {
        slot: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { slot }).unwrap();
    let result = try_invoke("quicksetup_apply", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn quicksetup_delete_slot(slot: u8) -> Option<QuickSetupActionResult> {
    #[derive(Serialize)]
    struct Args {
        slot: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { slot }).unwrap();
    let result = try_invoke("quicksetup_delete", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

// -----------------------------------------------------------------------------
// I2C Device types (DS4424 IDAC)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct IdacCalPoint {
    pub code: i8,
    pub voltage: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct IdacChannelState {
    pub code: i8,
    pub target_v: f32,
    pub midpoint_v: f32,
    pub v_min: f32,
    pub v_max: f32,
    pub step_mv: f32,
    pub calibrated: bool,
    #[serde(default)]
    pub cal_points: Vec<IdacCalPoint>,
    pub cal_poly: Option<[f32; 4]>, // fitted cubic in normalized code cn = code/127
    pub name: String,
}

fn idac_poly_voltage(ch: &IdacChannelState, code: i8) -> Option<f32> {
    let poly = ch.cal_poly?;
    let cn = code as f32 / 127.0;
    // Horner: a0 + cn*(a1 + cn*(a2 + cn*a3))
    Some(poly[0] + cn * (poly[1] + cn * (poly[2] + cn * poly[3])))
}

fn idac_point_voltage(ch: &IdacChannelState, code: i8) -> Option<f32> {
    if ch.cal_points.len() < 2 {
        return None;
    }
    let mut pts = ch.cal_points.clone();
    pts.sort_by_key(|p| p.code);
    for pair in pts.windows(2) {
        let c0 = pair[0].code;
        let c1 = pair[1].code;
        if ((code >= c0 && code <= c1) || (code <= c0 && code >= c1)) && c1 != c0 {
            let t = (code - c0) as f32 / (c1 - c0) as f32;
            return Some(pair[0].voltage + t * (pair[1].voltage - pair[0].voltage));
        }
    }
    if code <= pts[0].code {
        Some(pts[0].voltage)
    } else {
        pts.last().map(|p| p.voltage)
    }
}

pub fn idac_interpolate_voltage(ch: &IdacChannelState, code: i8) -> f32 {
    idac_poly_voltage(ch, code)
        .or_else(|| idac_point_voltage(ch, code))
        .unwrap_or_else(|| ch.midpoint_v - (code as f32 * ch.step_mv / 1000.0))
}

pub fn idac_interpolate_voltage_opt(ch: &IdacChannelState, code: i8) -> Option<f32> {
    idac_poly_voltage(ch, code).or_else(|| idac_point_voltage(ch, code))
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct IdacState {
    pub present: bool,
    pub channels: Vec<IdacChannelState>,
}

// IDAC invoke helpers
pub fn send_idac_code(channel: u8, code: i8) {
    #[derive(Serialize)]
    struct Args {
        channel: u8,
        code: i8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { channel, code }).unwrap();
    let label = format!("Set IDAC{} code={}", channel, code);
    invoke_with_feedback("idac_set_code", args, &label);
}

pub fn send_idac_voltage(channel: u8, voltage: f32) {
    #[derive(Serialize)]
    struct Args {
        channel: u8,
        voltage: f32,
    }
    let args = serde_wasm_bindgen::to_value(&Args { channel, voltage }).unwrap();
    let label = format!("Set IDAC{} to {:.3}V", channel, voltage);
    invoke_with_feedback("idac_set_voltage", args, &label);
}

pub async fn fetch_idac_status() -> Option<IdacState> {
    let result = try_invoke("idac_get_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub fn send_pca_control(control: u8, on: bool) {
    #[derive(Serialize)]
    struct Args {
        control: u8,
        on: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args { control, on }).unwrap();
    let name = match control {
        0 => "VADJ1",
        1 => "VADJ2",
        2 => "+/-15V",
        3 => "LOGIC_EN",
        4 => "USB Hub",
        5 => "EFuse1",
        6 => "EFuse2",
        7 => "EFuse3",
        8 => "EFuse4",
        _ => "PCA",
    };
    let label = format!("{} {}", if on { "Enable" } else { "Disable" }, name);
    invoke_with_feedback("pca_set_control", args, &label);
}

pub fn send_set_rtd_config(channel: u8, excitation_ua: u16) {
    // current: 0 = 500 µA, 1 = 1000 µA / 1 mA (maps to RTD_CURRENT bit)
    let current: u8 = if excitation_ua >= 1000 { 1 } else { 0 };
    #[derive(Serialize)]
    struct Args {
        channel: u8,
        current: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { channel, current }).unwrap();
    let label = format!(
        "Set CH {} RTD excitation: {} µA",
        CH_NAMES[channel as usize], excitation_ua
    );
    invoke_with_feedback("set_rtd_config", args, &label);
}

// -----------------------------------------------------------------------------
// HUSB238 USB PD types & helpers
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct UsbPdPdo {
    pub voltage: String,
    pub detected: bool,
    pub max_current_a: f32,
    pub max_power_w: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct UsbPdState {
    pub present: bool,
    pub attached: bool,
    pub cc: String,
    pub voltage_v: f32,
    pub current_a: f32,
    pub power_w: f32,
    pub pd_response: u8,
    pub source_pdos: Vec<UsbPdPdo>,
    pub selected_pdo: u8,
}

pub async fn fetch_usbpd_status() -> Option<UsbPdState> {
    let result = try_invoke("usbpd_get_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub fn send_usbpd_select_pdo(voltage: u8) {
    #[derive(Serialize)]
    struct Args {
        voltage: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { voltage }).unwrap();
    let voltage_v = match voltage {
        1 => 5,
        2 => 9,
        3 => 12,
        4 => 15,
        5 => 18,
        6 => 20,
        _ => voltage as i32,
    };
    let label = format!("Select USB PD {}V", voltage_v);
    invoke_with_feedback("usbpd_select_pdo", args, &label);
}

// -----------------------------------------------------------------------------
// PCA9535 IO Expander types & helpers
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct EfuseState {
    pub id: u8,
    pub enabled: bool,
    pub fault: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct IoExpState {
    pub present: bool,
    pub input0: u8,
    pub input1: u8,
    pub output0: u8,
    pub output1: u8,
    pub logic_pg: bool,
    pub vadj1_pg: bool,
    pub vadj2_pg: bool,
    pub vadj1_en: bool,
    pub vadj2_en: bool,
    pub en_15v: bool,
    pub en_mux: bool,
    pub en_usb_hub: bool,
    pub efuses: Vec<EfuseState>,
}

pub async fn fetch_pca_status() -> Option<IoExpState> {
    let result = try_invoke("pca_get_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

// -----------------------------------------------------------------------------
// HAT Expansion Board types & helpers
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct HatConnectorStatus {
    pub enabled: bool,
    pub current_ma: f32,
    pub fault: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct HatStatus {
    pub detected: bool,
    pub connected: bool,
    pub hat_type: u8,
    pub detect_voltage: f32,
    pub fw_major: u8,
    pub fw_minor: u8,
    pub config_confirmed: bool,
    pub pin_config: Vec<u8>,
    // Power
    pub connectors: Vec<HatConnectorStatus>,
    pub io_voltage_mv: u16,
    // SWD
    pub dap_connected: bool,
    pub target_detected: bool,
    pub target_dpidr: u32,
}

pub async fn fetch_hat_status() -> Option<HatStatus> {
    let result = try_invoke("hat_get_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub fn send_hat_set_pin(pin: u8, function: u8) {
    #[derive(Serialize)]
    struct Args {
        pin: u8,
        function: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { pin, function }).unwrap();
    let func_names = [
        "Disconnected",
        "SWDIO",
        "SWCLK",
        "TRACE1",
        "TRACE2",
        "GPIO1",
        "GPIO2",
        "GPIO3",
        "GPIO4",
    ];
    let name = func_names.get(function as usize).unwrap_or(&"?");
    let label = format!("EXT_{} -> {}", pin + 1, name);
    invoke_with_feedback("hat_set_pin", args, &label);
}

pub fn send_hat_reset() {
    invoke_with_feedback("hat_reset", JsValue::NULL, "HAT Reset");
}

pub fn send_hat_set_power(connector: u8, enable: bool) {
    #[derive(Serialize)]
    struct Args {
        connector: u8,
        enable: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args { connector, enable }).unwrap();
    let name = if connector == 0 { "A" } else { "B" };
    let label = format!("Connector {} {}", name, if enable { "ON" } else { "OFF" });
    invoke_with_feedback("hat_set_power", args, &label);
}

pub fn send_hat_set_io_voltage(voltage_mv: u16) {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        voltage_mv: u16,
    }
    let args = serde_wasm_bindgen::to_value(&Args { voltage_mv }).unwrap();
    let label = format!("I/O Voltage: {:.1}V", voltage_mv as f32 / 1000.0);
    invoke_with_feedback("hat_set_io_voltage", args, &label);
}

pub fn send_hat_setup_swd(target_voltage_mv: u16, connector: u8) {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        target_voltage_mv: u16,
        connector: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        target_voltage_mv,
        connector,
    })
    .unwrap();
    let label = format!(
        "SWD Setup: {:.1}V on {}",
        target_voltage_mv as f32 / 1000.0,
        if connector == 0 { "A" } else { "B" }
    );
    invoke_with_feedback("hat_setup_swd", args, &label);
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HatCalibrateStatus {
    pub state: u8,
    pub progress: u8,
    pub rail_id: u8,
    pub last_error: u8,
    pub persist_state: u8,
    pub stage: u8,
    pub point: u8,
    pub code: i8,
    pub measured_mv: i32,
    pub min_mv: i32,
    pub max_mv: i32,
    pub max_gap_mv: i32,
    pub max_error_mv: i32,
    pub validation_flags: u16,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HatCalibratePoint {
    pub dac_code: i8,
    pub measured_v: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HatLevelShiftStatus {
    pub oe: bool,
    pub dir: bool,
}

pub async fn hat_calibrate_start(rail_id: u8) -> Option<u8> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        rail_id: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { rail_id }).unwrap();
    let result = try_invoke("hat_calibrate_start", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn hat_calibrate_status() -> Option<HatCalibrateStatus> {
    let result = try_invoke("hat_calibrate_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn hat_calibrate_import(rail_id: u8, points: Vec<HatCalibratePoint>) -> Option<()> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        rail_id: u8,
        points: Vec<HatCalibratePoint>,
    }
    let args = serde_wasm_bindgen::to_value(&Args { rail_id, points }).unwrap();
    try_invoke("hat_calibrate_import", args).await?;
    Some(())
}

pub async fn hat_set_io_bank(dirs: u8, ups: u8, dns: u8) -> Option<()> {
    #[derive(Serialize)]
    struct Args {
        dirs: u8,
        ups: u8,
        dns: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { dirs, ups, dns }).unwrap();
    try_invoke("hat_set_io_bank", args).await?;
    Some(())
}

pub async fn hat_set_level_shift(oe: bool, dir: bool) -> Option<HatLevelShiftStatus> {
    #[derive(Serialize)]
    struct Args {
        oe: bool,
        dir: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args { oe, dir }).unwrap();
    let result = try_invoke("hat_set_level_shift", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HatCaps {
    pub hw_revision: u8,
    pub flags: u32,
    pub rail_count: u8,
    pub led_count: u8,
    pub shifted_io_count: u8,
    pub la_route_count: u8,
    pub fw_major: u8,
    pub fw_minor: u8,
    pub hvpak_present: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HatRailStatus {
    pub rail_id: u8,
    pub enabled: bool,
    pub voltage_mv: u16,
    pub current_ma: u16,
    pub status: u8,
}

pub async fn hat_get_caps() -> Option<HatCaps> {
    let result = try_invoke("hat_get_caps", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn hat_get_rail_status() -> Option<Vec<HatRailStatus>> {
    let result = try_invoke("hat_get_rail_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn hat_set_rail_enable(rail_id: u8, enable: bool) -> Option<Vec<HatRailStatus>> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        rail_id: u8,
        enable: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args { rail_id, enable }).unwrap();
    let result = try_invoke("hat_set_rail_enable", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn hat_set_rail_voltage(rail_id: u8, voltage_mv: u16) -> Option<Vec<HatRailStatus>> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        rail_id: u8,
        voltage_mv: u16,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        rail_id,
        voltage_mv,
    })
    .unwrap();
    let result = try_invoke("hat_set_rail_voltage", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn hat_la_set_route(route: u8) -> Option<u8> {
    #[derive(Serialize)]
    struct Args {
        route: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { route }).unwrap();
    let result = try_invoke("hat_la_set_route", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn hat_la_log_enable(enable: bool) -> Option<()> {
    #[derive(Serialize)]
    struct Args {
        enable: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args { enable }).unwrap();
    try_invoke("hat_la_log_enable", args).await?;
    Some(())
}

pub async fn hat_la_usb_reset() -> Option<()> {
    try_invoke("hat_la_usb_reset", JsValue::NULL).await?;
    Some(())
}

pub async fn hat_la_log_get() -> Option<Vec<String>> {
    let result = try_invoke("hat_la_log_get", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

// -----------------------------------------------------------------------------
// Logic Analyzer types & helpers
// Logic Analyzer types & helpers
// -----------------------------------------------------------------------------
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LaViewData {
    pub channels: u8,
    pub sample_rate_hz: u32,
    pub total_samples: u64,
    pub view_start: u64,
    pub view_end: u64,
    pub trigger_sample: Option<u64>,
    pub channel_transitions: Vec<Vec<(u64, u8)>>,
    pub density: Vec<u16>,
    pub decimated: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LaCaptureInfo {
    pub channels: u8,
    pub sample_rate_hz: u32,
    pub total_samples: u64,
    pub duration_sec: f64,
    pub trigger_sample: Option<u64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LaStreamRuntimeStatus {
    pub active: bool,
    pub total_bytes: u64,
    pub chunk_count: u64,
    pub sequence_mismatches: u32,
    pub invalid_frames: u32,
    pub stop_reason: Option<String>,
    pub last_error: Option<String>,
}

pub fn summarize_la_stream_status(status: &LaStreamRuntimeStatus) -> String {
    if let Some(err) = &status.last_error {
        return format!(
            "Stream error: {} ({} B, {} packets)",
            err, status.total_bytes, status.chunk_count
        );
    }
    if let Some(reason) = &status.stop_reason {
        if reason != "none" {
            return format!(
                "Stream stopped: {} ({} B, {} packets)",
                reason, status.total_bytes, status.chunk_count
            );
        }
    }
    if status.active {
        return format!(
            "Streaming: {} B, {} packets, {} seq errors, {} invalid packets",
            status.total_bytes,
            status.chunk_count,
            status.sequence_mismatches,
            status.invalid_frames
        );
    }
    if status.total_bytes > 0 || status.chunk_count > 0 {
        return format!(
            "Last stream: {} B, {} packets",
            status.total_bytes, status.chunk_count
        );
    }
    "Stream idle".to_string()
}

pub async fn la_get_view(start: u64, end: u64, max_points: Option<usize>) -> Option<LaViewData> {
    #[derive(Serialize)]
    struct Args {
        #[serde(rename = "startSample")]
        start_sample: u64,
        #[serde(rename = "endSample")]
        end_sample: u64,
        #[serde(rename = "maxPoints")]
        max_points: Option<usize>,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        start_sample: start,
        end_sample: end,
        max_points,
    })
    .unwrap();
    let result = try_invoke("la_get_view", args).await?;
    serde_wasm_bindgen::from_value::<Option<LaViewData>>(result)
        .ok()
        .flatten()
}

pub async fn la_get_capture_info() -> Option<LaCaptureInfo> {
    let result = try_invoke("la_get_capture_info", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value::<Option<LaCaptureInfo>>(result)
        .ok()
        .flatten()
}

pub async fn la_invoke_configure(channels: u8, rate_hz: u32, depth: u32, rle_enabled: bool) {
    #[derive(Serialize)]
    struct Args {
        channels: u8,
        #[serde(rename = "rateHz")]
        rate_hz: u32,
        depth: u32,
        #[serde(rename = "rleEnabled")]
        rle_enabled: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        channels,
        rate_hz,
        depth,
        rle_enabled,
    })
    .unwrap();
    let _ = try_invoke("la_configure", args).await;
}

pub async fn la_invoke_arm() {
    let _ = try_invoke("la_arm", JsValue::NULL).await;
}

pub async fn la_export_vcd(path: &str) {
    #[derive(Serialize)]
    struct Args {
        path: String,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        path: path.to_string(),
    })
    .unwrap();
    let _ = try_invoke("la_export_vcd_file", args).await;
}

pub async fn la_export_json(path: &str) {
    #[derive(Serialize)]
    struct Args {
        path: String,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        path: path.to_string(),
    })
    .unwrap();
    let _ = try_invoke("la_export_json", args).await;
}

pub async fn la_import_json_file(path: &str) -> Option<LaCaptureInfo> {
    #[derive(Serialize)]
    struct Args {
        path: String,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        path: path.to_string(),
    })
    .unwrap();
    let result = try_invoke("la_import_json", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}
pub async fn la_invoke_force() {
    let _ = try_invoke("la_force", JsValue::NULL).await;
}
pub async fn la_invoke_stop() {
    let _ = try_invoke("la_stop", JsValue::NULL).await;
}

pub async fn la_decode_uart(
    tx_channel: u8,
    rx_channel: Option<u8>,
    baud_rate: u32,
    start_sample: u64,
    end_sample: u64,
) -> Vec<serde_json::Value> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct UartCfg {
        #[serde(rename = "type")]
        dtype: &'static str,
        tx_channel: u8,
        rx_channel: Option<u8>,
        baud_rate: u32,
        data_bits: u8,
        parity: &'static str,
        stop_bits: f32,
        idle_high: bool,
    }
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        config: UartCfg,
        start_sample: u64,
        end_sample: u64,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        config: UartCfg {
            dtype: "uart",
            tx_channel,
            rx_channel,
            baud_rate,
            data_bits: 8,
            parity: "none",
            stop_bits: 1.0,
            idle_high: true,
        },
        start_sample,
        end_sample,
    })
    .unwrap();
    let result = match try_invoke("la_decode", args).await {
        Some(v) => v,
        None => return Vec::new(),
    };
    serde_wasm_bindgen::from_value(result).unwrap_or_default()
}

pub async fn la_decode_i2c(
    sda_channel: u8,
    scl_channel: u8,
    start_sample: u64,
    end_sample: u64,
) -> Vec<serde_json::Value> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct I2cCfg {
        #[serde(rename = "type")]
        dtype: &'static str,
        sda_channel: u8,
        scl_channel: u8,
    }
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        config: I2cCfg,
        start_sample: u64,
        end_sample: u64,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        config: I2cCfg {
            dtype: "i2c",
            sda_channel,
            scl_channel,
        },
        start_sample,
        end_sample,
    })
    .unwrap();
    let result = match try_invoke("la_decode", args).await {
        Some(v) => v,
        None => return Vec::new(),
    };
    serde_wasm_bindgen::from_value(result).unwrap_or_default()
}

#[allow(clippy::too_many_arguments)]
pub async fn la_decode_spi(
    mosi: u8,
    miso: u8,
    clk: u8,
    cs: u8,
    cpol: u8,
    cpha: u8,
    start_sample: u64,
    end_sample: u64,
) -> Vec<serde_json::Value> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct SpiCfg {
        #[serde(rename = "type")]
        dtype: &'static str,
        mosi_channel: u8,
        miso_channel: u8,
        sclk_channel: u8,
        cs_channel: u8,
        cpol: u8,
        cpha: u8,
        bit_order: &'static str,
        word_size: u8,
        cs_active_low: bool,
    }
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        config: SpiCfg,
        start_sample: u64,
        end_sample: u64,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        config: SpiCfg {
            dtype: "spi",
            mosi_channel: mosi,
            miso_channel: miso,
            sclk_channel: clk,
            cs_channel: cs,
            cpol,
            cpha,
            bit_order: "msb",
            word_size: 8,
            cs_active_low: true,
        },
        start_sample,
        end_sample,
    })
    .unwrap();
    let result = match try_invoke("la_decode", args).await {
        Some(v) => v,
        None => return Vec::new(),
    };
    serde_wasm_bindgen::from_value(result).unwrap_or_default()
}

pub async fn la_delete_range(start: u64, end: u64) -> Option<LaCaptureInfo> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        start_sample: u64,
        end_sample: u64,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        start_sample: start,
        end_sample: end,
    })
    .unwrap();
    let result = try_invoke("la_delete_range", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

// -----------------------------------------------------------------------------
// IO Ownership types & helpers
// -----------------------------------------------------------------------------

/// Mirror of one entry in the firmware io_owner_t table returned by io_owner_status.
/// 16 entries total: indices 0..11 = IO1..IO12, 12..15 = CH0..CH3.
/// Wire format per slot: kind(u8) + session_id(u8) + token_fp32(u32 LE) + lease_until_ms(u32 LE)
/// = 10 bytes × 16 slots = 160 bytes total.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OwnerSlot {
    pub kind: u8, // io_owner_kind_t: 0=NONE,1=USB,2=HTTP,3=SCRIPT,4=CLI,5=INTERNAL
    pub session_id: u8,
    pub token_fp32: u32,
    pub lease_until_ms: u32, // low 32 bits of monotonic lease deadline (ms)
}

impl OwnerSlot {
    pub fn is_owned(&self) -> bool {
        self.kind != 0
    }

    pub fn kind_name(&self) -> &'static str {
        match self.kind {
            1 => "USB",
            2 => "HTTP",
            3 => "Script",
            4 => "CLI",
            5 => "Internal",
            _ => "None",
        }
    }
}

/// Claim IO slots. Returns true on IO_OK, false on IO_HELD_BY_OTHER or error.
/// `slots`: slot indices (0..11 = IO1..IO12, 12..15 = CH0..CH3).
/// `lease_ms`: lease duration in ms (0 = infinite, must release explicitly).
/// `purpose`: human-readable label for status display.
pub async fn io_claim(slots: &[u8], lease_ms: u32, purpose: &str) -> bool {
    #[derive(Serialize)]
    struct Args {
        slots: Vec<u8>,
        #[serde(rename = "leaseMs")]
        lease_ms: u32,
        purpose: String,
    }
    let args = match serde_wasm_bindgen::to_value(&Args {
        slots: slots.to_vec(),
        lease_ms,
        purpose: purpose.to_string(),
    }) {
        Ok(v) => v,
        Err(e) => {
            web_sys::console::warn_1(&format!("[io_claim] serialize error: {:?}", e).into());
            return false;
        }
    };
    match try_invoke("io_claim", args).await {
        Some(val) => {
            // Backend returns the raw BBP response: n_slots(u8) + status[n](u8).
            // Claim succeeded only if every per-slot status byte is 0 (CMD_OK).
            match serde_wasm_bindgen::from_value::<Vec<u8>>(val) {
                Ok(bytes) if bytes.len() >= 2 => {
                    let n = bytes[0] as usize;
                    bytes.len() >= 1 + n && bytes[1..1 + n].iter().all(|&s| s == 0)
                }
                _ => false,
            }
        }
        None => false,
    }
}

/// Release IO slots previously claimed.
/// `slots`: slot indices to release (empty = release all owned by this session).
pub async fn io_release(slots: &[u8]) {
    #[derive(Serialize)]
    struct Args {
        slots: Vec<u8>,
    }
    let args = match serde_wasm_bindgen::to_value(&Args {
        slots: slots.to_vec(),
    }) {
        Ok(v) => v,
        Err(e) => {
            web_sys::console::warn_1(&format!("[io_release] serialize error: {:?}", e).into());
            return;
        }
    };
    let _ = try_invoke("io_release", args).await;
}

/// Query the full 16-slot ownership table. Returns None on error.
pub async fn io_owner_status() -> Option<Vec<OwnerSlot>> {
    let result = try_invoke("io_owner_status", wasm_bindgen::JsValue::NULL).await?;
    let slots: Vec<OwnerSlot> = serde_wasm_bindgen::from_value(result).ok()?;
    assert_eq!(
        slots.len(),
        16,
        "[io_owner_status] expected 16 slots from backend, got {}",
        slots.len()
    );
    Some(slots)
}

/// Force-release a slot (admin path). `slot`: index 0..15, or 0xFF for all.
pub async fn io_force_release(slot: u8) -> bool {
    #[derive(Serialize)]
    struct Args {
        slot: u8,
    }
    let args = match serde_wasm_bindgen::to_value(&Args { slot }) {
        Ok(v) => v,
        Err(e) => {
            web_sys::console::warn_1(
                &format!("[io_force_release] serialize error: {:?}", e).into(),
            );
            return false;
        }
    };
    // Backend returns CmdResult<()>; try_invoke returns Some(JsValue::Null) on Ok
    // and None on Err. Treat any Some as success.
    try_invoke("io_force_release", args).await.is_some()
}

/// Single stream cycle: stop → configure → arm → poll → read (USB bulk or UART fallback)
pub async fn la_stream_cycle(
    channels: u8,
    sample_rate_hz: u32,
    depth: u32,
    rle_enabled: bool,
    trigger_type: u8,
    trigger_channel: u8,
) -> Option<LaCaptureInfo> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        channels: u8,
        sample_rate_hz: u32,
        depth: u32,
        rle_enabled: bool,
        trigger_type: u8,
        trigger_channel: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        channels,
        sample_rate_hz,
        depth,
        rle_enabled,
        trigger_type,
        trigger_channel,
    })
    .unwrap();
    let result = try_invoke("la_stream_cycle", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

/// Start gapless USB streaming — configures + starts continuous DMA→USB stream.
/// Returns immediately; data is appended to the store in background.
pub async fn la_stream_usb_start(
    channels: u8,
    sample_rate_hz: u32,
    depth: u32,
    rle_enabled: bool,
    trigger_type: u8,
    trigger_channel: u8,
) -> bool {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        channels: u8,
        sample_rate_hz: u32,
        depth: u32,
        rle_enabled: bool,
        trigger_type: u8,
        trigger_channel: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        channels,
        sample_rate_hz,
        depth,
        rle_enabled,
        trigger_type,
        trigger_channel,
    })
    .unwrap();
    // try_invoke returns None for both null (Ok(())) and error — use JS that distinguishes
    let promise = js_sys::Function::new_with_args(
        "cmd, args",
        "console.log('[stream_usb] invoking', cmd); return window.__TAURI__.core.invoke(cmd, args).then(function() { console.log('[stream_usb] OK'); return true; }).catch(function(e) { console.error('[stream_usb] FAILED:', e); return false; })"
    );
    let result = match promise.call2(&JsValue::NULL, &JsValue::from_str("la_stream_usb"), &args) {
        Ok(r) => r,
        Err(_) => return false,
    };
    let future = wasm_bindgen_futures::JsFuture::from(js_sys::Promise::from(result));
    match future.await {
        Ok(val) => val.as_bool().unwrap_or(false),
        Err(_) => false,
    }
}

/// Stop gapless USB streaming and return final capture info.
pub async fn la_stream_usb_stop() -> Option<LaCaptureInfo> {
    let result = try_invoke("la_stream_usb_stop", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

/// Check if gapless USB streaming is currently active.
pub async fn la_stream_usb_active() -> bool {
    let result = try_invoke("la_stream_usb_active", JsValue::NULL).await;
    match result {
        Some(v) => serde_wasm_bindgen::from_value(v).unwrap_or(false),
        None => false,
    }
}

pub async fn la_stream_usb_status() -> Option<LaStreamRuntimeStatus> {
    let result = try_invoke("la_stream_usb_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

#[cfg(test)]
mod tests {
    use super::{summarize_la_stream_status, LaStreamRuntimeStatus};

    #[test]
    fn summarizes_active_stream_status() {
        let status = LaStreamRuntimeStatus {
            active: true,
            total_bytes: 4096,
            chunk_count: 32,
            sequence_mismatches: 0,
            invalid_frames: 0,
            stop_reason: None,
            last_error: None,
        };
        let summary = summarize_la_stream_status(&status);
        assert!(summary.contains("Streaming: 4096 B"));
        assert!(summary.contains("32 packets"));
    }

    #[test]
    fn summarizes_stopped_stream_reason() {
        let status = LaStreamRuntimeStatus {
            active: false,
            total_bytes: 8192,
            chunk_count: 64,
            sequence_mismatches: 0,
            invalid_frames: 0,
            stop_reason: Some("dma_overrun".to_string()),
            last_error: None,
        };
        let summary = summarize_la_stream_status(&status);
        assert!(summary.contains("Stream stopped: dma_overrun"));
    }

    #[test]
    fn summarizes_stream_error() {
        let status = LaStreamRuntimeStatus {
            active: false,
            total_bytes: 120,
            chunk_count: 2,
            sequence_mismatches: 1,
            invalid_frames: 0,
            stop_reason: Some("sequence_mismatch".to_string()),
            last_error: Some("sequence mismatch".to_string()),
        };
        let summary = summarize_la_stream_status(&status);
        assert!(summary.contains("Stream error: sequence mismatch"));
    }
}

pub async fn la_invoke_set_trigger(trigger_type: u8, channel: u8) {
    #[derive(Serialize)]
    struct Args {
        #[serde(rename = "triggerType")]
        trigger_type: u8,
        channel: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        trigger_type,
        channel,
    })
    .unwrap();
    let _ = try_invoke("la_set_trigger", args).await;
}

// -----------------------------------------------------------------------------
// WiFi State types & helpers
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct WifiState {
    pub connected: bool,
    pub sta_ssid: String,
    pub sta_ip: String,
    pub rssi: i32,
    pub ap_ssid: String,
    pub ap_ip: String,
    pub ap_mac: String,
}

pub async fn fetch_wifi_status() -> Option<WifiState> {
    let result = try_invoke("wifi_get_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WifiNetwork {
    pub ssid: String,
    pub rssi: i32,
    pub auth: u8,
}

pub async fn fetch_wifi_scan() -> Vec<WifiNetwork> {
    let result = match try_invoke("wifi_scan", JsValue::NULL).await {
        Some(v) => v,
        None => return Vec::new(),
    };
    serde_wasm_bindgen::from_value(result).unwrap_or_default()
}

pub async fn wifi_forget() -> bool {
    let result = match try_invoke("wifi_forget", JsValue::NULL).await {
        Some(v) => v,
        None => return false,
    };
    serde_wasm_bindgen::from_value(result).unwrap_or(false)
}

// -----------------------------------------------------------------------------
// Firmware / OTA
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FirmwareInfo {
    pub fw_version: String,
    pub proto_version: u8,
    pub build_date: String,
    pub idf_version: String,
    pub partition: String,
    pub next_partition: String,
}

pub async fn fetch_firmware_info() -> Option<FirmwareInfo> {
    let result = try_invoke("get_firmware_info", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn upload_firmware(path: &str) -> Result<String, String> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        file_path: String,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        file_path: path.to_string(),
    })
    .unwrap();
    let result = try_invoke("ota_upload_firmware", args)
        .await
        .ok_or_else(|| "ota_upload_firmware command failed".to_string())?;
    serde_wasm_bindgen::from_value::<String>(result).map_err(|e| e.to_string())
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DesktopGitRelease {
    pub tag: String,
    #[serde(default)]
    pub published_at: String,
    pub manifest_build_id: String,
    pub commit: String,
    pub rp2040_version: String,
    pub rp2040_url: String,
    pub rp2040_size: u64,
    pub rp2040_sha256: String,
    pub rp2040_crc32: u32,
    pub esp32_version: String,
    pub esp32_url: String,
    pub esp32_size: u64,
    pub esp32_sha256: String,
    pub spiffs_version: String,
    pub spiffs_url: String,
    pub spiffs_size: u64,
    pub spiffs_sha256: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct OtaProgress {
    pub stage: String,
    pub percent: f32,
    pub message: String,
}

pub async fn fetch_github_releases() -> Result<Vec<DesktopGitRelease>, String> {
    let result = try_invoke("fetch_github_releases", JsValue::NULL)
        .await
        .ok_or_else(|| "fetch_github_releases command failed".to_string())?;
    serde_wasm_bindgen::from_value::<Vec<DesktopGitRelease>>(result).map_err(|e| e.to_string())
}

pub async fn start_desktop_ota(
    update_rp2040: bool,
    update_esp32: bool,
    rp2040_url: String,
    rp2040_size: u64,
    rp2040_sha256: String,
    esp32_url: String,
    esp32_size: u64,
    esp32_sha256: String,
) -> Result<(), String> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        update_rp2040: bool,
        update_esp32: bool,
        rp2040_url: String,
        rp2040_size: u64,
        rp2040_sha256: String,
        esp32_url: String,
        esp32_size: u64,
        esp32_sha256: String,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        update_rp2040,
        update_esp32,
        rp2040_url,
        rp2040_size,
        rp2040_sha256,
        esp32_url,
        esp32_size,
        esp32_sha256,
    })
    .unwrap();
    match try_invoke("start_desktop_ota", args).await {
        Some(_) => Ok(()),
        None => Err("start_desktop_ota command failed".to_string()),
    }
}

pub async fn start_desktop_spiffs_ota(
    spiffs_url: String,
    spiffs_size: u64,
    spiffs_sha256: String,
) -> Result<(), String> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        spiffs_url: String,
        spiffs_size: u64,
        spiffs_sha256: String,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        spiffs_url,
        spiffs_size,
        spiffs_sha256,
    })
    .unwrap();
    match try_invoke("start_desktop_spiffs_ota", args).await {
        Some(_) => Ok(()),
        None => Err("start_desktop_spiffs_ota command failed".to_string()),
    }
}

// -----------------------------------------------------------------------------
// High-speed DAQ (ESP32-P4) types & helpers
// Mirror src-tauri/src/daq_store.rs, daq_commands.rs, daq_proto.rs (camelCase).
// -----------------------------------------------------------------------------

/// Sample-rate enum index → samples/second (mirror SAMPLE_RATES).
pub const DAQ_SAMPLE_RATES: [u32; 5] = [10_000, 50_000, 100_000, 250_000, 1_000_000];

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqViewData {
    pub sample_rate_hz: u32,
    pub total_samples: u64,
    pub view_start: u64,
    pub view_end: u64,
    pub decimated: bool,
    pub overflow: bool,
    pub i_min: Vec<f32>,
    pub i_max: Vec<f32>,
    pub v_min: Vec<f32>,
    pub v_max: Vec<f32>,
    pub p_min: Vec<f32>,
    pub p_max: Vec<f32>,
    pub source: Vec<u8>,
    pub didt: Vec<f32>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqIntegral {
    pub start: u64,
    pub end: u64,
    pub duration_s: f64,
    pub charge_c: f64,
    pub charge_mah: f64,
    pub energy_j: f64,
    pub energy_mwh: f64,
    pub avg_i: f64,
    pub avg_v: f64,
    pub avg_p: f64,
    pub min_i: f64,
    pub max_i: f64,
    pub projected_mwh_per_hour: f64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqStreamRuntimeStatus {
    pub connected: bool,
    pub mock: bool,
    pub active: bool,
    pub total_samples: u64,
    pub frame_count: u64,
    pub sample_rate_hz: u32,
    pub overflow: bool,
    pub ingest_sps: f64,
    pub max_samples: u64,
    pub raw_cap: u64,
    pub mem_used_mb: f64,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct DaqStatBlock {
    pub min: f32,
    pub max: f32,
    pub mean: f32,
    pub rms: f32,
    pub std: f32,
    pub count: u32,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct DaqStats {
    pub i: DaqStatBlock,
    pub v: DaqStatBlock,
    pub p: DaqStatBlock,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqEnergy {
    pub energy_mwh: f64,
    pub energy_j: f64,
    pub charge_mah: f64,
    pub charge_c: f64,
    pub elapsed_s: f64,
    pub last_i: f32,
    pub last_v: f32,
    pub last_p: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqFft {
    pub sample_rate: u32,
    pub source: u8,
    pub window: u8,
    pub bins: Vec<f32>,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqStatus {
    pub sample_rate: u32,
    pub overflow_count: u32,
    pub range: u8,
    pub streaming: bool,
    pub range_locked: bool,
    pub source_enabled: bool,
    pub vdut_set: f32,
    pub ilimit_set: f32,
    pub in_voltage: f32,
    pub in_current: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqSnapshots {
    pub total_samples: u64,
    pub sample_rate_hz: u32,
    pub stats: Option<DaqStats>,
    pub energy: Option<DaqEnergy>,
    pub fft: Option<DaqFft>,
    pub status: Option<DaqStatus>,
}

/// SMU calibration status (mirror daq_commands::DaqCalStatus).
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqCalStatus {
    pub phase: u8,      // 0 idle,1 prompt,2 running,3 success,4 failed
    pub prompt: u8,     // 0 none,1 disconnect_load,2 short_output
    pub mode: u8,       // 0 voltage,1 current
    pub progress: u8,   // 0..100
    pub point: u8,
    pub code: i8,
    pub persist: u8,    // 0 ram,1 saving,2 saved,3 failed
    pub measured: f32,
    pub min: f32,
    pub max: f32,
    pub flags: u16,
    pub vcount: u8,
    pub icount: u8,
}

/// Live fused measurement (mirror daq_commands::DaqMeasure).
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqMeasure {
    pub range: u8,
    pub streaming: bool,
    pub source_enabled: bool,
    pub current_a: f32,
    pub voltage_v: f32,
    pub power_w: f32,
    pub energy_mwh: f32,
}

pub async fn daq_check_usb() -> bool {
    try_invoke("daq_check_usb", JsValue::NULL)
        .await
        .and_then(|v| serde_wasm_bindgen::from_value::<bool>(v).ok())
        .unwrap_or(false)
}

pub async fn daq_connect(mock: bool) -> bool {
    #[derive(Serialize)]
    struct Args {
        mock: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args { mock }).unwrap();
    try_invoke("daq_connect", args)
        .await
        .and_then(|v| serde_wasm_bindgen::from_value::<bool>(v).ok())
        .unwrap_or(false)
}

pub async fn daq_disconnect() {
    try_invoke("daq_disconnect", JsValue::NULL).await;
}

pub async fn daq_stream_start(sample_rate_idx: u8, decimation: u16) {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        sample_rate_idx: u8,
        decimation: u16,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        sample_rate_idx,
        decimation,
    })
    .unwrap();
    try_invoke("daq_stream_start", args).await;
}

pub async fn daq_stream_stop() {
    try_invoke("daq_stream_stop", JsValue::NULL).await;
}

pub async fn daq_stream_status() -> Option<DaqStreamRuntimeStatus> {
    let result = try_invoke("daq_stream_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn daq_get_view(
    start: u64,
    end: u64,
    max_points: u32,
    smooth: u32,
    filter_type: u8,
) -> Option<DaqViewData> {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        start: u64,
        end: u64,
        max_points: u32,
        smooth: u32,
        filter_type: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        start,
        end,
        max_points,
        smooth,
        filter_type,
    })
    .unwrap();
    let result = try_invoke("daq_get_view", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn daq_get_integral(start: u64, end: u64) -> Option<DaqIntegral> {
    #[derive(Serialize)]
    struct Args {
        start: u64,
        end: u64,
    }
    let args = serde_wasm_bindgen::to_value(&Args { start, end }).unwrap();
    let result = try_invoke("daq_get_integral", args).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn daq_get_snapshots() -> Option<DaqSnapshots> {
    let result = try_invoke("daq_get_snapshots", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

/// Start an SMU calibration run (mode: 0=voltage, 1=current).
pub async fn daq_cal_start(mode: u8) {
    #[derive(Serialize)]
    struct Args {
        mode: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { mode }).unwrap();
    try_invoke("daq_cal_start", args).await;
}

/// Acknowledge the current calibration prompt so the run proceeds.
pub async fn daq_cal_ack() {
    try_invoke("daq_cal_ack", JsValue::NULL).await;
}

/// Abort the in-progress calibration and restore a safe SMU state.
pub async fn daq_cal_abort() {
    try_invoke("daq_cal_abort", JsValue::NULL).await;
}

/// Poll the live SMU calibration status.
pub async fn daq_cal_status() -> Option<DaqCalStatus> {
    let result = try_invoke("daq_cal_status", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

/// Read the DAQ HAT's latest fused measurement (I/V/P/energy).
pub async fn daq_measure() -> Option<DaqMeasure> {
    let result = try_invoke("daq_measure", JsValue::NULL).await?;
    serde_wasm_bindgen::from_value(result).ok()
}

pub async fn daq_set_range_lock(range: u8) {
    #[derive(Serialize)]
    struct Args {
        range: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args { range }).unwrap();
    try_invoke("daq_set_range_lock", args).await;
}

pub async fn daq_set_rate(sample_rate_idx: u8, decimation: u16) {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        sample_rate_idx: u8,
        decimation: u16,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        sample_rate_idx,
        decimation,
    })
    .unwrap();
    try_invoke("daq_set_rate", args).await;
}

pub async fn daq_set_source(vdut_mv: u32, ilimit_ma: u32, enable: bool) {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Args {
        vdut_mv: u32,
        ilimit_ma: u32,
        enable: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        vdut_mv,
        ilimit_ma,
        enable,
    })
    .unwrap();
    try_invoke("daq_set_source", args).await;
}

pub async fn daq_set_fft(nbins: u16, source: u8, window: u8, enabled: bool) {
    #[derive(Serialize)]
    struct Args {
        nbins: u16,
        source: u8,
        window: u8,
        enabled: bool,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        nbins,
        source,
        window,
        enabled,
    })
    .unwrap();
    try_invoke("daq_set_fft", args).await;
}

pub async fn daq_reset_energy() {
    try_invoke("daq_reset_energy", JsValue::NULL).await;
}

pub async fn daq_reset_stats() {
    try_invoke("daq_reset_stats", JsValue::NULL).await;
}
