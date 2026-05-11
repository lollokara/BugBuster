// =============================================================================
// state.rs - Shared device state types (mirrors firmware DeviceState)
// =============================================================================

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ChannelState {
    pub function: u8,            // ChannelFunction code (0-12)
    pub adc_raw: u32,            // 24-bit ADC code
    pub adc_value: f32,          // Converted value (V or mA)
    pub adc_range: u8,           // AdcRange code
    pub adc_rate: u8,            // AdcRate code
    pub adc_mux: u8,             // AdcConvMux code
    pub dac_code: u16,           // Active DAC code
    pub dac_value: f32,          // Converted DAC value
    pub din_state: bool,         // Digital input comparator output
    pub din_counter: u32,        // DIN event counter
    pub do_state: bool,          // Digital output state
    pub channel_alert: u16,      // Per-channel alert status bits
    pub channel_alert_mask: u16, // Per-channel alert mask bits
    pub rtd_excitation_ua: u16, // RTD excitation current in µA (500 or 1000; 0 when not in RES_MEAS)
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DiagState {
    pub source: u8,    // Diagnostic source code (0-13)
    pub raw_code: u16, // Raw diagnostic ADC code
    pub value: f32,    // Converted value (V or C)
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct GpioState {
    pub mode: u8,       // GpioSelect mode (0-4)
    pub output: bool,   // GPO_DATA state
    pub input: bool,    // GPI_DATA state
    pub pulldown: bool, // GP_WK_PD_EN
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
    pub channels: [ChannelState; 4],
    pub diag: [DiagState; 4],
    pub gpio: [GpioState; 12],
    pub mux_states: [u8; 4],
}

impl DeviceState {
    /// Parse a GET_STATUS response payload into DeviceState.
    ///
    /// Returns `Err` if mandatory fields are missing (payload too short or
    /// otherwise malformed).  Optional trailing fields (`channel_alert_mask`,
    /// `rtd_excitation_ua`, diag/mux/gpio) silently default to zero when absent
    /// — those are firmware-version-gated extensions.
    pub fn from_status_payload(payload: &[u8]) -> Result<Self> {
        use crate::bbp::PayloadReader;
        let mut r = PayloadReader::new(payload);

        macro_rules! req {
            ($expr:expr, $field:literal) => {
                $expr.ok_or_else(|| {
                    log::warn!(
                        "Malformed GET_STATUS payload ({} bytes): short at field '{}' — \
                         firmware version mismatch?",
                        payload.len(),
                        $field
                    );
                    anyhow!(
                        "Malformed GET_STATUS payload: short at field '{}' ({} bytes total)",
                        $field,
                        payload.len()
                    )
                })?
            };
        }

        let mut state = DeviceState::default();
        state.spi_ok = req!(r.get_bool(), "spi_ok");
        state.die_temperature = req!(r.get_f32(), "die_temperature");
        state.alert_status = req!(r.get_u16(), "alert_status");
        state.alert_mask = req!(r.get_u16(), "alert_mask");
        state.supply_alert_status = req!(r.get_u16(), "supply_alert_status");
        state.supply_alert_mask = req!(r.get_u16(), "supply_alert_mask");
        state.live_status = req!(r.get_u16(), "live_status");

        for ch in 0..4 {
            let _id = req!(r.get_u8(), "channel_id");
            state.channels[ch].function = req!(r.get_u8(), "channel.function");
            state.channels[ch].adc_raw = req!(r.get_u24(), "channel.adc_raw");
            state.channels[ch].adc_value = req!(r.get_f32(), "channel.adc_value");
            state.channels[ch].adc_range = req!(r.get_u8(), "channel.adc_range");
            state.channels[ch].adc_rate = req!(r.get_u8(), "channel.adc_rate");
            state.channels[ch].adc_mux = req!(r.get_u8(), "channel.adc_mux");
            state.channels[ch].dac_code = req!(r.get_u16(), "channel.dac_code");
            state.channels[ch].dac_value = req!(r.get_f32(), "channel.dac_value");
            state.channels[ch].din_state = req!(r.get_bool(), "channel.din_state");
            state.channels[ch].din_counter = req!(r.get_u32(), "channel.din_counter");
            state.channels[ch].do_state = req!(r.get_bool(), "channel.do_state");
            state.channels[ch].channel_alert = req!(r.get_u16(), "channel.channel_alert");
            // Optional firmware-extension fields — default to 0 if absent.
            state.channels[ch].channel_alert_mask = r.get_u16().unwrap_or(0);
            state.channels[ch].rtd_excitation_ua = r.get_u16().unwrap_or(0);
        }

        // Per diagnostic slot (4x, stride = 7 bytes)
        for d in 0..4 {
            if let Some(source) = r.get_u8() {
                state.diag[d].source = source;
                state.diag[d].raw_code = r.get_u16().unwrap_or(0);
                state.diag[d].value = r.get_f32().unwrap_or(0.0);
            }
        }

        // MUX switch states (4 bytes)
        for m in 0..4 {
            if let Some(v) = r.get_u8() {
                state.mux_states[m] = v;
            }
        }

        // ESP32 GPIO states (12 pins * 4 bytes each = 48 bytes)
        for g in 0..12 {
            if r.remaining() >= 4 {
                state.gpio[g].mode = r.get_u8().unwrap_or(0);
                state.gpio[g].output = r.get_bool().unwrap_or(false);
                state.gpio[g].input = r.get_bool().unwrap_or(false);
                state.gpio[g].pulldown = r.get_bool().unwrap_or(false);
            }
        }

        Ok(state)
    }
}

// -----------------------------------------------------------------------------
// I2C Device States (DS4424 / HUSB238 / PCA9535)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct IdacChannelState {
    pub code: i8,
    pub target_v: f32,
    pub midpoint_v: f32,
    pub v_min: f32,
    pub v_max: f32,
    pub step_mv: f32,
    pub calibrated: bool,
    pub cal_poly: Option<[f32; 4]>, // fitted cubic in normalized code cn = code/127
    pub name: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct IdacState {
    pub present: bool,
    pub channels: Vec<IdacChannelState>,
}

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

// -----------------------------------------------------------------------------
// Supply Voltages Cache (BBP 0x07)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SupplyRail {
    pub rail: u8,
    pub name: String,
    pub voltage_v: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SelftestSuppliesCached {
    pub available: bool,
    pub timestamp_ms: u32,
    pub rails: Vec<SupplyRail>,
}

// -----------------------------------------------------------------------------
// Quick Setups (BBP 0xF0..0xF4)
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

// -----------------------------------------------------------------------------
// WiFi State
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

// -----------------------------------------------------------------------------
// Device info from handshake or GET_DEVICE_INFO
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DeviceInfo {
    pub spi_ok: bool,
    pub silicon_rev: u8,
    pub silicon_id0: u16,
    pub silicon_id1: u16,
    pub fw_version: String,
    pub proto_version: u8,
    pub mac_address: Option<String>,
}

// -----------------------------------------------------------------------------
// Connection status (emitted to frontend)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum ConnectionMode {
    Disconnected,
    Usb,
    Http,
}

impl Default for ConnectionMode {
    fn default() -> Self {
        Self::Disconnected
    }
}

use crate::la_usb::DeviceSelector;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConnectionStatus {
    pub mode: ConnectionMode,
    pub port_or_url: String,
    pub device_info: Option<DeviceInfo>,
    #[serde(skip_serializing)]
    pub admin_token: Option<String>,
    pub la_selector: Option<DeviceSelector>,
}

impl Default for ConnectionStatus {
    fn default() -> Self {
        Self {
            mode: ConnectionMode::Disconnected,
            port_or_url: String::new(),
            device_info: None,
            admin_token: None,
            la_selector: None,
        }
    }
}

// -----------------------------------------------------------------------------
// Discovered device entry
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiscoveredDevice {
    pub id: String,        // Unique identifier
    pub name: String,      // Display name
    pub transport: String, // "usb" or "http"
    pub address: String,   // Port path or URL
    pub serial_number: Option<String>,
}
