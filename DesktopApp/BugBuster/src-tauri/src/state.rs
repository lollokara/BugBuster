// =============================================================================
// state.rs - Shared device state types (mirrors firmware DeviceState)
// =============================================================================

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ChannelState {
    pub function: u8,           // ChannelFunction code (0-12)
    pub adc_raw: u32,           // 24-bit ADC code
    pub adc_value: f32,         // Converted value (V or mA)
    pub adc_range: u8,          // AdcRange code
    pub adc_rate: u8,           // AdcRate code
    pub adc_mux: u8,            // AdcConvMux code
    pub dac_code: u16,          // Active DAC code
    pub dac_value: f32,         // Converted DAC value
    pub din_state: bool,        // Digital input comparator output
    pub din_counter: u32,       // DIN event counter
    pub do_state: bool,         // Digital output state
    pub channel_alert: u16,     // Per-channel alert bits
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DiagState {
    pub source: u8,             // Diagnostic source code (0-13)
    pub raw_code: u16,          // Raw diagnostic ADC code
    pub value: f32,             // Converted value (V or C)
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct GpioState {
    pub mode: u8,               // GpioSelect mode (0-4)
    pub output: bool,           // GPO_DATA state
    pub input: bool,            // GPI_DATA state
    pub pulldown: bool,         // GP_WK_PD_EN
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct UartBridgeState {
    pub uart_num: u8,
    pub tx_pin: u8,
    pub rx_pin: u8,
    pub baudrate: u32,
    pub data_bits: u8,
    pub parity: u8,
    pub stop_bits: u8,
    pub enabled: bool,
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
    pub gpio: [GpioState; 6],
    pub mux_states: [u8; 4],
}

impl DeviceState {
    /// Parse a GET_STATUS response payload into DeviceState.
    pub fn from_status_payload(payload: &[u8]) -> Option<Self> {
        use crate::bbp::PayloadReader;
        let mut r = PayloadReader::new(payload);

        let mut state = DeviceState::default();
        state.spi_ok = r.get_bool()?;
        state.die_temperature = r.get_f32()?;
        state.alert_status = r.get_u16()?;
        state.alert_mask = r.get_u16()?;
        state.supply_alert_status = r.get_u16()?;
        state.supply_alert_mask = r.get_u16()?;
        state.live_status = r.get_u16()?;

        for ch in 0..4 {
            let _id = r.get_u8()?; // channel_id
            state.channels[ch].function = r.get_u8()?;
            state.channels[ch].adc_raw = r.get_u24()?;
            state.channels[ch].adc_value = r.get_f32()?;
            state.channels[ch].adc_range = r.get_u8()?;
            state.channels[ch].adc_rate = r.get_u8()?;
            state.channels[ch].adc_mux = r.get_u8()?;
            state.channels[ch].dac_code = r.get_u16()?;
            state.channels[ch].dac_value = r.get_f32()?;
            state.channels[ch].din_state = r.get_bool()?;
            state.channels[ch].din_counter = r.get_u32()?;
            state.channels[ch].do_state = r.get_bool()?;
            state.channels[ch].channel_alert = r.get_u16()?;
        }

        // Diagnostic slots (optional — older firmware won't include these)
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
                if m < state.mux_states.len() {
                    state.mux_states[m] = v;
                }
            }
        }

        Some(state)
    }
}

// -----------------------------------------------------------------------------
// I2C Device States (DS4424 / HUSB238 / PCA9535)
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
    pub cal_points: Vec<IdacCalPoint>,
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

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConnectionStatus {
    pub mode: ConnectionMode,
    pub port_or_url: String,
    pub device_info: Option<DeviceInfo>,
}

impl Default for ConnectionStatus {
    fn default() -> Self {
        Self {
            mode: ConnectionMode::Disconnected,
            port_or_url: String::new(),
            device_info: None,
        }
    }
}

// -----------------------------------------------------------------------------
// Discovered device entry
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiscoveredDevice {
    pub id: String,             // Unique identifier
    pub name: String,           // Display name
    pub transport: String,      // "usb" or "http"
    pub address: String,        // Port path or URL
}

// -----------------------------------------------------------------------------
// ADC stream sample (for frontend plotting)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AdcStreamBatch {
    pub channel_mask: u8,
    pub timestamp_us: u32,
    pub samples: Vec<AdcSample>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AdcSample {
    pub raw: [u32; 4],         // 24-bit raw codes (0 for inactive channels)
}

// -----------------------------------------------------------------------------
// Scope data bucket
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScopeBucket {
    pub seq: u32,
    pub timestamp_ms: u32,
    pub count: u16,
    pub channels: [ScopeChannelData; 4],
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ScopeChannelData {
    pub avg: f32,
    pub min: f32,
    pub max: f32,
}
