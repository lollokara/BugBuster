// =============================================================================
// commands.rs - Tauri command handlers exposed to the Leptos frontend
// =============================================================================

use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::Mutex;

use tauri::State;

use crate::bbp::{self, PayloadWriter};
use crate::connection_manager::ConnectionManager;
use crate::state::*;

use serde::{Deserialize, Serialize};
use serde_json;

/// Global CSV writer protected by a Mutex.
/// `None` means no recording is in progress.
pub static CSV_WRITER: Mutex<Option<BufWriter<File>>> = Mutex::new(None);

type CmdResult<T> = Result<T, String>;
const QUICKSETUP_SLOT_COUNT: u8 = 4;
const QUICKSETUP_MAX_JSON_BYTES: usize = 1000;

fn map_err(e: anyhow::Error) -> String {
    e.to_string()
}

fn validate_quicksetup_slot(slot: u8) -> CmdResult<()> {
    if slot < QUICKSETUP_SLOT_COUNT {
        Ok(())
    } else {
        Err(format!(
            "quick-setup slot {} out of range (expected 0..{})",
            slot,
            QUICKSETUP_SLOT_COUNT - 1
        ))
    }
}

// -----------------------------------------------------------------------------
// Discovery & Connection
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn discover_devices(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<Vec<DiscoveredDevice>> {
    Ok(mgr.discover().await)
}

#[tauri::command]
pub async fn connect_device(
    device_id: String,
    mgr: State<'_, ConnectionManager>,
    app: tauri::AppHandle,
) -> CmdResult<ConnectionStatus> {
    log::info!("connect_device called with: {}", device_id);
    match mgr.connect(&device_id, &app).await {
        Ok(()) => {
            let status = mgr.get_connection_status();
            log::info!("Connected successfully: {:?}", status.mode);
            Ok(status)
        }
        Err(e) => {
            log::error!("Connection failed: {}", e);
            Err(format!("Connection failed: {}", e))
        }
    }
}

#[tauri::command]
pub async fn disconnect_device(
    mgr: State<'_, ConnectionManager>,
    app: tauri::AppHandle,
) -> CmdResult<()> {
    mgr.disconnect(&app).await.map_err(map_err)
}

#[tauri::command]
pub fn get_connection_status(mgr: State<'_, ConnectionManager>) -> ConnectionStatus {
    mgr.get_connection_status()
}

// -----------------------------------------------------------------------------
// Device State
// -----------------------------------------------------------------------------

#[tauri::command]
pub fn get_device_state(mgr: State<'_, ConnectionManager>) -> DeviceState {
    mgr.get_device_state()
}

// -----------------------------------------------------------------------------
// Channel Configuration
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn set_channel_function(
    channel: u8,
    function: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(function);
    mgr.send_command(bbp::CMD_SET_CH_FUNC, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_dac_code(
    channel: u8,
    code: u16,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u16(code);
    mgr.send_command(bbp::CMD_SET_DAC_CODE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_dac_voltage(
    channel: u8,
    voltage: f32,
    bipolar: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    // Validate range BEFORE sending to firmware so the toast surfaces a real error.
    let (lo, hi) = if bipolar {
        (-12.0_f32, 12.0_f32)
    } else {
        (0.0_f32, 12.0_f32)
    };
    if voltage < lo || voltage > hi {
        log::error!(
            "[set_dac_voltage] REJECT ch={} V={} bipolar={} — out of range [{}, {}]",
            channel,
            voltage,
            bipolar,
            lo,
            hi
        );
        return Err(format!(
            "voltage {} out of range for bipolar={}",
            voltage, bipolar
        ));
    }
    log::info!(
        "[set_dac_voltage] ch={} V={} bipolar={}",
        channel,
        voltage,
        bipolar
    );
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_f32(voltage);
    pw.put_bool(bipolar);
    mgr.send_command(bbp::CMD_SET_DAC_VOLTAGE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_dac_current(
    channel: u8,
    current_ma: f32,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_f32(current_ma);
    mgr.send_command(bbp::CMD_SET_DAC_CURRENT, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_adc_config(
    channel: u8,
    mux: u8,
    range: u8,
    rate: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    log::info!(
        "[set_adc_config] ch={} mux={} range={} rate={}",
        channel,
        mux,
        range,
        rate
    );
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(mux);
    pw.put_u8(range);
    pw.put_u8(rate);
    mgr.send_command(bbp::CMD_SET_ADC_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_rtd_config(
    channel: u8,
    current: u8, // 0 = 500 µA, 1 = 1000 µA (1 mA)
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(current);
    mgr.send_command(bbp::CMD_SET_RTD_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_do_state(
    channel: u8,
    on: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_bool(on);
    mgr.send_command(bbp::CMD_SET_DO_STATE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_din_config(
    channel: u8,
    thresh: u8,
    thresh_mode: bool,
    debounce: u8,
    sink: u8,
    sink_range: bool,
    oc_det: bool,
    sc_det: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(thresh);
    pw.put_bool(thresh_mode);
    pw.put_u8(debounce);
    pw.put_u8(sink);
    pw.put_bool(sink_range);
    pw.put_bool(oc_det);
    pw.put_bool(sc_det);
    mgr.send_command(bbp::CMD_SET_DIN_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_do_config(
    channel: u8,
    mode: u8,
    src_sel_gpio: bool,
    t1: u8,
    t2: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(mode);
    pw.put_bool(src_sel_gpio);
    pw.put_u8(t1);
    pw.put_u8(t2);
    mgr.send_command(bbp::CMD_SET_DO_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_vout_range(
    channel: u8,
    bipolar: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_bool(bipolar);
    mgr.send_command(bbp::CMD_SET_VOUT_RANGE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_current_limit(
    channel: u8,
    limit_8ma: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_bool(limit_8ma);
    mgr.send_command(bbp::CMD_SET_ILIMIT, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_gpio_config(
    gpio: u8,
    mode: u8,
    pulldown: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(gpio);
    pw.put_u8(mode);
    pw.put_bool(pulldown);
    mgr.send_command(bbp::CMD_SET_GPIO_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_gpio_value(
    gpio: u8,
    value: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(gpio);
    pw.put_bool(value);
    mgr.send_command(bbp::CMD_SET_GPIO_VALUE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// UART Bridge
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn set_uart_config(
    bridge_id: u8,
    uart_num: u8,
    tx_pin: u8,
    rx_pin: u8,
    baudrate: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
    enabled: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(bridge_id);
    pw.put_u8(uart_num);
    pw.put_u8(tx_pin);
    pw.put_u8(rx_pin);
    pw.put_u32(baudrate);
    pw.put_u8(data_bits);
    pw.put_u8(parity);
    pw.put_u8(stop_bits);
    pw.put_bool(enabled);
    mgr.send_command(bbp::CMD_SET_UART_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn set_diag_config(
    slot: u8,
    source: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(slot);
    pw.put_u8(source);
    mgr.send_command(bbp::CMD_SET_DIAG_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

// Level Shifter OE
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn set_lshift_oe(on: bool, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_bool(on);
    mgr.send_command(bbp::CMD_SET_LSHIFT_OE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// WiFi Management
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn wifi_get_status(mgr: State<'_, ConnectionManager>) -> CmdResult<WifiState> {
    let rsp = mgr
        .send_command(bbp::CMD_WIFI_GET_STATUS, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_wifi_status(&rsp))
}

#[tauri::command]
pub async fn wifi_connect(
    ssid: String,
    password: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<bool> {
    let ssid_bytes = ssid.as_bytes();
    if ssid_bytes.len() > 32 {
        return Err("SSID too long (max 32 bytes)".into());
    }
    let pass_bytes = password.as_bytes();
    if pass_bytes.len() > 64 {
        return Err("Password too long (max 64 bytes)".into());
    }
    let mut pw = PayloadWriter::new();
    pw.put_u8(ssid_bytes.len() as u8);
    pw.buf.extend_from_slice(ssid_bytes);
    pw.put_u8(pass_bytes.len() as u8);
    pw.buf.extend_from_slice(pass_bytes);
    let rsp = mgr
        .send_command(bbp::CMD_WIFI_CONNECT, &pw.buf)
        .await
        .map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);
    Ok(r.get_bool().unwrap_or(false))
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WifiNetwork {
    pub ssid: String,
    pub rssi: i32,
    pub auth: u8,
}

#[tauri::command]
pub async fn wifi_scan(mgr: State<'_, ConnectionManager>) -> CmdResult<Vec<WifiNetwork>> {
    let rsp = mgr
        .send_command(bbp::CMD_WIFI_SCAN, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_wifi_scan(&rsp))
}

// -----------------------------------------------------------------------------
// Faults
// -----------------------------------------------------------------------------
// Firmware Version & OTA
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FirmwareInfo {
    pub fw_version: String,
    pub proto_version: u8,
    pub build_date: String,
    pub idf_version: String,
    pub partition: String,
    pub next_partition: String,
}

#[tauri::command]
pub async fn get_firmware_info(mgr: State<'_, ConnectionManager>) -> CmdResult<FirmwareInfo> {
    // Try HTTP endpoint first (richer info)
    let base_url = mgr.get_base_url().await;
    if let Some(url) = base_url {
        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .build()
            .map_err(|e| e.to_string())?;
        let resp = client
            .get(format!("{}/api/device/version", url))
            .send()
            .await
            .map_err(|e| e.to_string())?;
        if resp.status().is_success() {
            let json: serde_json::Value = resp.json().await.map_err(|e| e.to_string())?;
            return Ok(FirmwareInfo {
                fw_version: format!(
                    "{}.{}.{}",
                    json.get("fwMajor").and_then(|v| v.as_u64()).unwrap_or(0),
                    json.get("fwMinor").and_then(|v| v.as_u64()).unwrap_or(0),
                    json.get("fwPatch").and_then(|v| v.as_u64()).unwrap_or(0)
                ),
                proto_version: json
                    .get("protoVersion")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(0) as u8,
                build_date: format!(
                    "{} {}",
                    json.get("date").and_then(|v| v.as_str()).unwrap_or("?"),
                    json.get("time").and_then(|v| v.as_str()).unwrap_or("")
                ),
                idf_version: json
                    .get("idfVersion")
                    .and_then(|v| v.as_str())
                    .unwrap_or("?")
                    .to_string(),
                partition: json
                    .get("partition")
                    .and_then(|v| v.as_str())
                    .unwrap_or("?")
                    .to_string(),
                next_partition: json
                    .get("nextPartition")
                    .and_then(|v| v.as_str())
                    .unwrap_or("?")
                    .to_string(),
            });
        }
    }
    // Fallback: use handshake info from connection
    let info = mgr.get_device_info().await;
    let (ver, proto) = match info {
        Some(i) => (i.fw_version, i.proto_version),
        None => ("?".into(), 0),
    };
    Ok(FirmwareInfo {
        fw_version: ver,
        proto_version: proto,
        build_date: String::new(),
        idf_version: String::new(),
        partition: String::new(),
        next_partition: String::new(),
    })
}

#[tauri::command]
pub async fn ota_upload_firmware(
    file_path: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<String> {
    // Read the firmware binary
    let data = std::fs::read(&file_path).map_err(|e| format!("Failed to read file: {}", e))?;
    let size = data.len();

    if size < 1024 || size > 2 * 1024 * 1024 {
        return Err(format!(
            "Invalid firmware size: {} bytes (expected 100KB-2MB)",
            size
        ));
    }

    log::info!("OTA: uploading {} bytes from {}", size, file_path);

    // Get HTTP base URL (OTA only works over HTTP)
    let base_url = mgr
        .get_base_url()
        .await
        .ok_or("OTA requires HTTP connection (WiFi). Connect via WiFi first.")?;

    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(120))
        .build()
        .map_err(|e| e.to_string())?;

    let resp = client
        .post(format!("{}/api/ota/upload", base_url))
        .header("Content-Type", "application/octet-stream")
        .body(data)
        .send()
        .await
        .map_err(|e| format!("OTA upload failed: {}", e))?;

    let status = resp.status();
    if !status.is_success() {
        let body = resp.text().await.unwrap_or_default();
        return Err(format!("OTA failed (HTTP {}): {}", status, body));
    }

    Ok(format!(
        "Firmware updated ({} bytes). Device is rebooting...",
        size
    ))
}

// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn clear_all_alerts(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_CLEAR_ALL_ALERTS, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn clear_channel_alert(channel: u8, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_CLEAR_CH_ALERT, &[channel])
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// System
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn device_reset(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_DEVICE_RESET, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// MUX Switch Matrix
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn mux_set_all(states: Vec<u8>, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    if states.len() < 4 {
        return Err("Need 4 device states".to_string());
    }
    log::info!("[mux_set_all] states={:?}", &states[..4.min(states.len())]);
    mgr.send_command(bbp::CMD_MUX_SET_ALL, &states[..4])
        .await
        .map_err(|e| {
            log::warn!("[mux_set_all] send failed: {e:?}");
            map_err(e)
        })?;
    Ok(())
}

#[tauri::command]
pub async fn mux_get_all(mgr: State<'_, ConnectionManager>) -> CmdResult<Vec<u8>> {
    let rsp = mgr
        .send_command(bbp::CMD_MUX_GET_ALL, &[])
        .await
        .map_err(map_err)?;
    Ok(rsp)
}

#[tauri::command]
pub async fn mux_set_switch(
    device: u8,
    switch_num: u8,
    state: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(device);
    pw.put_u8(switch_num);
    pw.put_bool(state);
    log::info!(
        "[mux_set_switch] device={} switch_num={} state={} payload={:?}",
        device,
        switch_num,
        state,
        pw.buf
    );
    mgr.send_command(bbp::CMD_MUX_SET_SWITCH, &pw.buf)
        .await
        .map_err(|e| {
            log::warn!("[mux_set_switch] send failed: {e:?}");
            map_err(e)
        })?;
    Ok(())
}

// -----------------------------------------------------------------------------
// Streaming
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn start_adc_stream(
    channel_mask: u8,
    divider: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel_mask);
    pw.put_u8(divider);
    mgr.send_command(bbp::CMD_START_ADC_STREAM, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn stop_adc_stream(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_STOP_ADC_STREAM, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn start_scope_stream(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    log::info!("[start_scope_stream] sending CMD_START_SCOPE_STREAM, payload_len=0");
    mgr.send_command(bbp::CMD_START_SCOPE_STREAM, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn stop_scope_stream(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    log::info!("[stop_scope_stream] sending CMD_STOP_SCOPE_STREAM, payload_len=0");
    mgr.send_command(bbp::CMD_STOP_SCOPE_STREAM, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// Waveform Generator
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn start_wavegen(
    channel: u8,
    waveform: String,
    freq_hz: f64,
    amplitude: f64,
    offset: f64,
    mode: String, // "voltage" or "current"
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    // Map waveform string to type enum
    let waveform_type: u8 = match waveform.as_str() {
        "sine" => 0,
        "square" => 1,
        "triangle" => 2,
        "sawtooth" => 3,
        _ => return Err(format!("Unknown waveform type: {}", waveform)),
    };

    // Map mode string to enum
    let mode_val: u8 = if mode == "current" { 1 } else { 0 };

    // Build START_WAVEGEN payload:
    // channel(u8) + waveform_type(u8) + freq_hz(f32) + amplitude(f32) + offset(f32) + mode(u8)
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(waveform_type);
    pw.put_f32(freq_hz as f32);
    pw.put_f32(amplitude as f32);
    pw.put_f32(offset as f32);
    pw.put_u8(mode_val);

    log::info!(
        "[wavegen_start] ch={} mode={} wf={} freq={} amp={} off={}",
        channel,
        mode,
        waveform,
        freq_hz,
        amplitude,
        offset
    );
    mgr.send_command(bbp::CMD_START_WAVEGEN, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn stop_wavegen(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_STOP_WAVEGEN, &[])
        .await
        .map_err(map_err)?;
    log::info!("Wavegen stopped");
    Ok(())
}

// -----------------------------------------------------------------------------
// DS4424 IDAC
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn idac_get_status(mgr: State<'_, ConnectionManager>) -> CmdResult<IdacState> {
    let rsp = mgr
        .send_command(bbp::CMD_IDAC_GET_STATUS, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_idac_status(&rsp))
}

#[tauri::command]
pub async fn idac_set_code(
    channel: u8,
    code: i8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(code as u8);
    mgr.send_command(bbp::CMD_IDAC_SET_CODE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn idac_set_voltage(
    channel: u8,
    voltage: f32,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_f32(voltage);
    mgr.send_command(bbp::CMD_IDAC_SET_VOLTAGE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

// Calibration point management

#[tauri::command]
pub async fn idac_cal_add_point(
    channel: u8,
    code: i8,
    measured_v: f32,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(code as u8);
    pw.put_f32(measured_v);
    mgr.send_command(bbp::CMD_IDAC_CAL_ADD_POINT, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn idac_cal_clear(channel: u8, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    mgr.send_command(bbp::CMD_IDAC_CAL_CLEAR, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn idac_cal_save(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_IDAC_CAL_SAVE, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// Self-Test / Auto-Calibration
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn selftest_auto_calibrate(
    channel: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<serde_json::Value> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    // This command takes several seconds — the firmware sweeps IDAC codes
    // and measures each via U23 before responding.
    let resp = mgr
        .send_command(bbp::CMD_SELFTEST_AUTO_CAL, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(parse_selftest_auto_cal(&resp))
}

#[tauri::command]
pub async fn selftest_status(mgr: State<'_, ConnectionManager>) -> CmdResult<serde_json::Value> {
    if let Some(url) = mgr.get_base_url().await {
        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .build()
            .map_err(|e| e.to_string())?;
        let resp = client
            .get(format!("{}/api/selftest", url))
            .send()
            .await
            .map_err(|e| e.to_string())?;
        if resp.status().is_success() {
            let json: serde_json::Value = resp.json().await.map_err(|e| e.to_string())?;
            return Ok(normalize_selftest_status_json(&json));
        }
    }

    let rsp = mgr
        .send_command(bbp::CMD_SELFTEST_STATUS, &[])
        .await
        .map_err(map_err)?;
    let mut status = parse_selftest_status(&rsp);
    if rsp.len() < SELFTEST_STATUS_WITH_WORKER_FLAGS_LEN {
        let enabled = mgr
            .send_command(bbp::CMD_SELFTEST_WORKER, &[0xFF])
            .await
            .ok()
            .and_then(|rsp| rsp.first().copied())
            .unwrap_or(0)
            != 0;
        apply_legacy_selftest_worker_fallback(&mut status, enabled);
    }
    Ok(status)
}

#[tauri::command]
pub async fn selftest_worker_get(mgr: State<'_, ConnectionManager>) -> CmdResult<bool> {
    if let Some(url) = mgr.get_base_url().await {
        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .build()
            .map_err(|e| e.to_string())?;
        let resp = client
            .get(format!("{}/api/selftest", url))
            .send()
            .await
            .map_err(|e| e.to_string())?;
        if resp.status().is_success() {
            let json: serde_json::Value = resp.json().await.map_err(|e| e.to_string())?;
            return Ok(json
                .get("workerEnabled")
                .and_then(|v| v.as_bool())
                .unwrap_or(false));
        }
    }

    let rsp = mgr
        .send_command(bbp::CMD_SELFTEST_WORKER, &[0xFF])
        .await
        .map_err(map_err)?;
    Ok(rsp.first().copied().unwrap_or(0) != 0)
}

#[tauri::command]
pub async fn selftest_worker_set(
    enabled: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<bool> {
    if let Some(url) = mgr.get_base_url().await {
        let status = mgr.get_connection_status();
        let mut headers = reqwest::header::HeaderMap::new();
        if let Some(token) = status.admin_token {
            headers.insert(
                "X-BugBuster-Admin-Token",
                reqwest::header::HeaderValue::from_str(&token).map_err(|e| e.to_string())?,
            );
        }
        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .default_headers(headers)
            .build()
            .map_err(|e| e.to_string())?;
        let resp = client
            .post(format!("{}/api/selftest/worker", url))
            .json(&serde_json::json!({ "enabled": enabled }))
            .send()
            .await
            .map_err(|e| e.to_string())?;
        if !resp.status().is_success() {
            return Err(format!("HTTP {} from /api/selftest/worker", resp.status()));
        }
        let json: serde_json::Value = resp.json().await.map_err(|e| e.to_string())?;
        return Ok(json
            .get("workerEnabled")
            .or_else(|| json.get("enabled"))
            .and_then(|v| v.as_bool())
            .unwrap_or(enabled));
    }

    let rsp = mgr
        .send_command(bbp::CMD_SELFTEST_WORKER, &[if enabled { 1 } else { 0 }])
        .await
        .map_err(map_err)?;
    Ok(rsp.first().copied().unwrap_or(if enabled { 1 } else { 0 }) != 0)
}

#[tauri::command]
pub async fn selftest_measure_supply(
    rail: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<serde_json::Value> {
    let rsp = mgr
        .send_command(bbp::CMD_SELFTEST_MEASURE_SUPPLY, &[rail])
        .await
        .map_err(map_err)?;
    Ok(parse_selftest_measure_supply(rail, &rsp))
}

#[tauri::command]
pub async fn selftest_supplies_cached(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<crate::state::SelftestSuppliesCached> {
    let rsp = mgr
        .send_command(bbp::CMD_SELFTEST_SUPPLY_VOLTAGES_CACHED, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_selftest_supplies_cached(&rsp))
}

// -----------------------------------------------------------------------------
// Quick Setups
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn quicksetup_list(mgr: State<'_, ConnectionManager>) -> CmdResult<QuickSetupList> {
    let rsp = mgr
        .send_command(bbp::CMD_QS_LIST, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_quicksetup_list(&rsp))
}

#[tauri::command]
pub async fn quicksetup_get(
    slot: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<QuickSetupPayload> {
    validate_quicksetup_slot(slot)?;
    let rsp = mgr
        .send_command(bbp::CMD_QS_GET, &[slot])
        .await
        .map_err(map_err)?;
    parse_quicksetup_payload(slot, &rsp)
}

#[tauri::command]
pub async fn quicksetup_save(
    slot: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<QuickSetupPayload> {
    validate_quicksetup_slot(slot)?;
    let rsp = mgr
        .send_command(bbp::CMD_QS_SAVE, &[slot])
        .await
        .map_err(map_err)?;
    parse_quicksetup_payload(slot, &rsp)
}

#[tauri::command]
pub async fn quicksetup_apply(
    slot: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<QuickSetupActionResult> {
    validate_quicksetup_slot(slot)?;
    let rsp = mgr
        .send_command(bbp::CMD_QS_APPLY, &[slot])
        .await
        .map_err(map_err)?;
    Ok(parse_quicksetup_action(slot, &rsp, true))
}

#[tauri::command]
pub async fn quicksetup_delete(
    slot: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<QuickSetupActionResult> {
    validate_quicksetup_slot(slot)?;
    let rsp = mgr
        .send_command(bbp::CMD_QS_DELETE, &[slot])
        .await
        .map_err(map_err)?;
    Ok(parse_quicksetup_action(slot, &rsp, false))
}

// -----------------------------------------------------------------------------
// PCA9535 GPIO Expander
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn pca_get_status(mgr: State<'_, ConnectionManager>) -> CmdResult<IoExpState> {
    let rsp = mgr
        .send_command(bbp::CMD_PCA_GET_STATUS, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_pca_status(&rsp))
}

#[tauri::command]
pub async fn pca_set_control(
    control: u8,
    on: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(control);
    pw.put_bool(on);
    mgr.send_command(bbp::CMD_PCA_SET_CONTROL, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// HAT Expansion Board
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

#[tauri::command]
pub async fn hat_get_status(mgr: State<'_, ConnectionManager>) -> CmdResult<HatStatus> {
    let rsp = mgr
        .send_command(bbp::CMD_HAT_GET_STATUS, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_hat_status(&rsp))
}

#[tauri::command]
pub async fn hat_set_pin(
    pin: u8,
    function: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_u8(pin);
    pw.put_u8(function);
    mgr.send_command(bbp::CMD_HAT_SET_PIN, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn hat_set_all_pins(pins: [u8; 4], mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_SET_ALL_PINS, &pins)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn hat_reset(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_RESET, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn hat_detect(mgr: State<'_, ConnectionManager>) -> CmdResult<HatStatus> {
    let rsp = mgr
        .send_command(bbp::CMD_HAT_DETECT, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_hat_detect(&rsp))
}

// HAT Power Query
#[tauri::command]
pub async fn hat_get_power(mgr: State<'_, ConnectionManager>) -> CmdResult<serde_json::Value> {
    let rsp = mgr
        .send_command(bbp::CMD_HAT_GET_POWER, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_hat_get_power(&rsp))
}

// HAT Power Management
#[tauri::command]
pub async fn hat_set_power(
    connector: u8,
    enable: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_u8(connector);
    pw.put_bool(enable);
    mgr.send_command(bbp::CMD_HAT_SET_POWER, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn hat_set_io_voltage(
    voltage_mv: u16,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_u16(voltage_mv);
    mgr.send_command(bbp::CMD_HAT_SET_IO_VOLTAGE, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn hat_setup_swd(
    target_voltage_mv: u16,
    connector: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_u16(target_voltage_mv);
    pw.put_u8(connector);
    mgr.send_command(bbp::CMD_HAT_SETUP_SWD, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// HUSB238 USB PD
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn usbpd_get_status(mgr: State<'_, ConnectionManager>) -> CmdResult<UsbPdState> {
    let rsp = mgr
        .send_command(bbp::CMD_USBPD_GET_STATUS, &[])
        .await
        .map_err(map_err)?;
    Ok(parse_usbpd_status(&rsp))
}

fn decode_husb_current(code: u8) -> f32 {
    match code {
        0 => 0.5,
        1 => 0.7,
        2 => 1.0,
        3 => 1.25,
        4 => 1.5,
        5 => 1.75,
        6 => 2.0,
        7 => 2.25,
        8 => 2.5,
        9 => 2.75,
        10 => 3.0,
        11 => 3.25,
        12 => 3.5,
        13 => 4.0,
        14 => 4.5,
        15 => 5.0,
        _ => 0.0,
    }
}

#[tauri::command]
pub async fn usbpd_select_pdo(voltage: u8, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(voltage);
    mgr.send_command(bbp::CMD_USBPD_SELECT_PDO, &pw.buf)
        .await
        .map_err(map_err)?;
    // Trigger negotiation. Some adapters/controllers occasionally NACK this
    // command transiently even when SELECT_PDO was accepted; retry and avoid
    // surfacing a hard UI failure for that case.
    let mut go_err: Option<anyhow::Error> = None;
    for _ in 0..3 {
        let mut pw2 = PayloadWriter::new();
        pw2.put_u8(0x01); // GO_SELECT_PDO
        match mgr.send_command(bbp::CMD_USBPD_GO, &pw2.buf).await {
            Ok(_) => {
                go_err = None;
                break;
            }
            Err(e) => {
                go_err = Some(e);
                tokio::time::sleep(std::time::Duration::from_millis(80)).await;
            }
        }
    }
    if let Some(e) = go_err {
        log::warn!("usbpd_select_pdo: GO command failed after retries: {}", e);
    }
    Ok(())
}

// -----------------------------------------------------------------------------
// File Dialog
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn pick_save_file(app: tauri::AppHandle) -> CmdResult<Option<String>> {
    use tauri_plugin_dialog::DialogExt;

    let path = app
        .dialog()
        .file()
        .set_title("Save CSV")
        .add_filter("CSV Files", &["csv"])
        .set_file_name("bugbuster_scope.csv")
        .blocking_save_file();

    Ok(path.map(|p| p.to_string()))
}

// -----------------------------------------------------------------------------
// BBSC Binary Recording + CSV Export
//
// Format: [4B magic "BBSC"] [4B header_len LE] [JSON header] [raw samples...]
// Each sample: 3 bytes per active channel (24-bit raw ADC codes, LE)
// -----------------------------------------------------------------------------

/// Recording state: file writer + metadata
pub struct RecordingState {
    pub writer: BufWriter<File>,
    pub sample_count: u64,
    pub path: String,
}

pub static RECORDING: Mutex<Option<RecordingState>> = Mutex::new(None);

#[tauri::command]
pub fn start_recording(
    path: String,
    channel_mask: u8,
    adc_ranges: Vec<u8>,
    sample_rate: u32,
) -> CmdResult<()> {
    let num_ch = (0..4).filter(|b| channel_mask & (1 << b) != 0).count() as u8;
    if num_ch == 0 {
        return Err("No channels selected".into());
    }

    let file = File::create(&path).map_err(|e| format!("Failed to create file: {}", e))?;
    let mut writer = BufWriter::with_capacity(65536, file); // 64KB buffer for throughput

    // Write magic
    writer
        .write_all(b"BBSC")
        .map_err(|e| format!("Write error: {}", e))?;

    // Build JSON header
    let header = serde_json::json!({
        "version": 1,
        "channels": num_ch,
        "mask": channel_mask,
        "sample_rate": sample_rate,
        "adc_ranges": adc_ranges,
        "start_time": chrono::Local::now().to_rfc3339(),
        "channel_names": ["CH_A", "CH_B", "CH_C", "CH_D"],
    });
    let header_bytes = header.to_string().into_bytes();

    // Write header length + header
    writer
        .write_all(&(header_bytes.len() as u32).to_le_bytes())
        .map_err(|e| format!("Write error: {}", e))?;
    writer
        .write_all(&header_bytes)
        .map_err(|e| format!("Write error: {}", e))?;
    writer.flush().map_err(|e| format!("Flush error: {}", e))?;

    let mut guard = RECORDING.lock().map_err(|e| format!("Lock error: {}", e))?;
    *guard = Some(RecordingState {
        writer,
        sample_count: 0,
        path: path.clone(),
    });

    log::info!(
        "BBSC recording started: {} (mask=0x{:02X}, {}ch, {}SPS)",
        path,
        channel_mask,
        num_ch,
        sample_rate
    );
    Ok(())
}

#[tauri::command]
pub fn stop_recording() -> CmdResult<u64> {
    let mut guard = RECORDING.lock().map_err(|e| format!("Lock error: {}", e))?;
    if let Some(mut rec) = guard.take() {
        rec.writer
            .flush()
            .map_err(|e| format!("Flush error: {}", e))?;
        let count = rec.sample_count;
        log::info!("BBSC recording stopped: {} samples to {}", count, rec.path);
        Ok(count)
    } else {
        Ok(0)
    }
}

/// Append raw ADC samples (binary, from adc-stream event payload)
/// This receives the raw EVT_ADC_DATA payload and writes sample data directly.
#[tauri::command]
pub fn append_recording_data(raw_payload: Vec<u8>) -> CmdResult<()> {
    let mut guard = RECORDING.lock().map_err(|e| format!("Lock error: {}", e))?;
    let rec = match guard.as_mut() {
        Some(r) => r,
        None => return Ok(()),
    };

    // Parse: [mask:1][timestamp:4][count:2][samples: count * num_ch * 3]
    if raw_payload.len() < 7 {
        return Ok(());
    }
    let mask = raw_payload[0];
    let count = u16::from_le_bytes([raw_payload[5], raw_payload[6]]) as usize;

    // Only write the raw sample data (skip the 7-byte header)
    let data_start = 7;
    let num_ch = (0..4).filter(|b| mask & (1 << b) != 0).count();
    let data_len = count * num_ch * 3;
    let data_end = data_start + data_len;

    if raw_payload.len() >= data_end {
        rec.writer
            .write_all(&raw_payload[data_start..data_end])
            .map_err(|e| format!("Write error: {}", e))?;
        rec.sample_count += count as u64;
    }
    Ok(())
}

/// Export a BBSC file to CSV
#[tauri::command]
pub fn export_bbsc_to_csv(bbsc_path: String, csv_path: String) -> CmdResult<u64> {
    use std::io::{BufReader, Read};

    let file = File::open(&bbsc_path).map_err(|e| format!("Open error: {}", e))?;
    let mut reader = BufReader::new(file);

    // Read and verify magic
    let mut magic = [0u8; 4];
    reader
        .read_exact(&mut magic)
        .map_err(|e| format!("Read error: {}", e))?;
    if &magic != b"BBSC" {
        return Err("Not a BBSC file".into());
    }

    // Read header length and header
    let mut hlen_buf = [0u8; 4];
    reader
        .read_exact(&mut hlen_buf)
        .map_err(|e| format!("Read error: {}", e))?;
    let hlen = u32::from_le_bytes(hlen_buf) as usize;

    let mut header_buf = vec![0u8; hlen];
    reader
        .read_exact(&mut header_buf)
        .map_err(|e| format!("Read error: {}", e))?;
    let header: serde_json::Value =
        serde_json::from_slice(&header_buf).map_err(|e| format!("JSON parse error: {}", e))?;

    let mask = header["mask"].as_u64().unwrap_or(0x0F) as u8;
    let adc_ranges: Vec<u8> = header["adc_ranges"]
        .as_array()
        .map(|a| a.iter().map(|v| v.as_u64().unwrap_or(0) as u8).collect())
        .unwrap_or_else(|| vec![0; 4]);
    let sample_rate = header["sample_rate"].as_u64().unwrap_or(20) as u32;

    let active_channels: Vec<usize> = (0..4).filter(|b| mask & (1 << b) != 0).collect();
    let num_ch = active_channels.len();
    let bytes_per_sample = num_ch * 3;

    // Create CSV
    let csv_file = File::create(&csv_path).map_err(|e| format!("Create error: {}", e))?;
    let mut csv = BufWriter::new(csv_file);

    // CSV header
    let mut hdr = "sample,time_s".to_string();
    let ch_names = ["ch_a", "ch_b", "ch_c", "ch_d"];
    for &ch in &active_channels {
        hdr.push(',');
        hdr.push_str(ch_names[ch]);
        hdr.push_str("_raw");
        hdr.push(',');
        hdr.push_str(ch_names[ch]);
        hdr.push_str("_v");
    }
    writeln!(csv, "{}", hdr).map_err(|e| format!("Write error: {}", e))?;

    // Read and convert samples
    let mut sample_buf = vec![0u8; bytes_per_sample];
    let mut sample_idx: u64 = 0;
    let dt = if sample_rate > 0 {
        1.0 / sample_rate as f64
    } else {
        0.001
    };

    loop {
        match reader.read_exact(&mut sample_buf) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(format!("Read error: {}", e)),
        }

        let time_s = sample_idx as f64 * dt;
        let mut line = format!("{},{:.6}", sample_idx, time_s);

        let mut pos = 0;
        for &ch in &active_channels {
            let raw = sample_buf[pos] as u32
                | ((sample_buf[pos + 1] as u32) << 8)
                | ((sample_buf[pos + 2] as u32) << 16);
            pos += 3;

            let range = if ch < adc_ranges.len() {
                adc_ranges[ch]
            } else {
                0
            };
            let voltage = raw_to_voltage_f64(raw, range);
            line.push_str(&format!(",{},{:.6}", raw, voltage));
        }

        writeln!(csv, "{}", line).map_err(|e| format!("Write error: {}", e))?;
        sample_idx += 1;
    }

    csv.flush().map_err(|e| format!("Flush error: {}", e))?;
    log::info!(
        "Exported {} samples from {} to {}",
        sample_idx,
        bbsc_path,
        csv_path
    );
    Ok(sample_idx)
}

fn raw_to_voltage_f64(raw: u32, range: u8) -> f64 {
    let code = raw as f64;
    let fs = 16777216.0; // 2^24
    match range {
        0 => code / fs * 12.0,
        1 => (code / fs * 24.0) - 12.0,
        2 => (code / fs * 0.625) - 0.3125,
        3 => (code / fs * 0.3125) - 0.3125,
        4 => code / fs * 0.3125,
        5 => code / fs * 0.625,
        6 => (code / fs * 0.208333) - 0.104167,
        7 => (code / fs * 5.0) - 2.5,
        _ => code / fs * 12.0,
    }
}

// Keep legacy CSV commands for backwards compatibility
#[tauri::command]
pub fn start_csv_recording(path: String) -> CmdResult<()> {
    let file = File::create(&path).map_err(|e| format!("Failed to create CSV file: {}", e))?;
    let mut writer = BufWriter::new(file);
    writeln!(writer, "timestamp_ms,ch_a,ch_b,ch_c,ch_d")
        .map_err(|e| format!("Failed to write CSV header: {}", e))?;
    writer.flush().map_err(|e| format!("Flush error: {}", e))?;
    let mut guard = CSV_WRITER
        .lock()
        .map_err(|e| format!("Lock error: {}", e))?;
    *guard = Some(writer);
    Ok(())
}

#[tauri::command]
pub fn stop_csv_recording() -> CmdResult<()> {
    let mut guard = CSV_WRITER
        .lock()
        .map_err(|e| format!("Lock error: {}", e))?;
    if let Some(mut w) = guard.take() {
        w.flush().ok();
    }
    Ok(())
}

#[tauri::command]
pub fn append_csv_data(timestamp_ms: f64, ch_values: Vec<f32>) -> CmdResult<()> {
    let mut guard = CSV_WRITER
        .lock()
        .map_err(|e| format!("Lock error: {}", e))?;
    if let Some(ref mut w) = *guard {
        let a = ch_values.first().copied().unwrap_or(0.0);
        let b = ch_values.get(1).copied().unwrap_or(0.0);
        let c = ch_values.get(2).copied().unwrap_or(0.0);
        let d = ch_values.get(3).copied().unwrap_or(0.0);
        writeln!(w, "{},{},{},{},{}", timestamp_ms, a, b, c, d).ok();
    }
    Ok(())
}

// -----------------------------------------------------------------------------
// Config Export / Import
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn export_config(path: String, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let state = mgr.get_device_state();

    // Channel functions and ADC config
    let channels: Vec<serde_json::Value> = (0..4)
        .map(|i| {
            let ch = &state.channels[i];
            serde_json::json!({
                "function": ch.function,
                "adc_range": ch.adc_range,
                "adc_rate": ch.adc_rate,
                "adc_mux": ch.adc_mux,
            })
        })
        .collect();

    // MUX switch states
    let mux_states: Vec<u8> = state.mux_states.to_vec();

    // IDAC codes (fetch live)
    let idac_codes: Vec<i8> = {
        let rsp = mgr.send_command(bbp::CMD_IDAC_GET_STATUS, &[]).await;
        match rsp {
            Ok(data) => {
                let mut r = bbp::PayloadReader::new(&data);
                let _present = r.get_bool();
                let mut codes = Vec::new();
                for _ in 0..3 {
                    let _ch = r.get_u8();
                    let code = r.get_u8().unwrap_or(0) as i8;
                    // skip remaining fixed fields per channel
                    let _target = r.get_f32();
                    let _actual = r.get_f32();
                    let _mid = r.get_f32();
                    let _vmin = r.get_f32();
                    let _vmax = r.get_f32();
                    let _step = r.get_f32();
                    let _cal = r.get_bool();
                    // skip poly: polyValid(bool) + 4*f32
                    let _poly_valid = r.get_bool();
                    let _a0 = r.get_f32();
                    let _a1 = r.get_f32();
                    let _a2 = r.get_f32();
                    let _a3 = r.get_f32();
                    codes.push(code);
                }
                codes
            }
            Err(_) => vec![0, 0, 0],
        }
    };

    // PCA9535 enables (fetch live)
    let pca_enables = {
        let rsp = mgr.send_command(bbp::CMD_PCA_GET_STATUS, &[]).await;
        match rsp {
            Ok(data) => {
                let mut r = bbp::PayloadReader::new(&data);
                let _present = r.get_bool();
                let _i0 = r.get_u8();
                let _i1 = r.get_u8();
                let _o0 = r.get_u8();
                let _o1 = r.get_u8();
                let _lpg = r.get_bool();
                let _v1pg = r.get_bool();
                let _v2pg = r.get_bool();
                for _ in 0..4 {
                    let _ef = r.get_bool();
                }
                let vadj1_en = r.get_bool().unwrap_or(false);
                let vadj2_en = r.get_bool().unwrap_or(false);
                let en_15v = r.get_bool().unwrap_or(false);
                let en_mux = r.get_bool().unwrap_or(false);
                let en_usb_hub = r.get_bool().unwrap_or(false);
                let mut efuse_en = [false; 4];
                for i in 0..4 {
                    efuse_en[i] = r.get_bool().unwrap_or(false);
                }
                serde_json::json!({
                    "vadj1_en": vadj1_en,
                    "vadj2_en": vadj2_en,
                    "en_15v": en_15v,
                    "en_mux": en_mux,
                    "en_usb_hub": en_usb_hub,
                    "efuse_en": efuse_en,
                })
            }
            Err(_) => serde_json::json!({}),
        }
    };

    let config = serde_json::json!({
        "version": 1,
        "channels": channels,
        "mux_states": mux_states,
        "idac_codes": idac_codes,
        "pca_enables": pca_enables,
    });

    let json_str = serde_json::to_string_pretty(&config)
        .map_err(|e| format!("JSON serialize error: {}", e))?;
    std::fs::write(&path, json_str).map_err(|e| format!("File write error: {}", e))?;

    log::info!("Config exported to {}", path);
    Ok(())
}

#[tauri::command]
pub async fn import_config(path: String, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let json_str = std::fs::read_to_string(&path).map_err(|e| format!("File read error: {}", e))?;
    let config: serde_json::Value =
        serde_json::from_str(&json_str).map_err(|e| format!("JSON parse error: {}", e))?;

    // Validate config version
    let version = config["version"].as_u64().unwrap_or(0);
    if version == 0 {
        return Err("Invalid config file: missing version field".into());
    }
    if version > 1 {
        return Err(format!("Config version {} is not supported (max: 1)", version).into());
    }
    if config["channels"].as_array().is_none() {
        return Err("Invalid config file: missing channels array".into());
    }

    // Restore channel functions and ADC config
    if let Some(channels) = config["channels"].as_array() {
        for (i, ch) in channels.iter().enumerate() {
            if i >= 4 {
                break;
            }
            let ch_idx = i as u8;

            // Set channel function
            if let Some(func) = ch["function"].as_u64() {
                let mut pw = PayloadWriter::new();
                pw.put_u8(ch_idx);
                pw.put_u8(func as u8);
                mgr.send_command(bbp::CMD_SET_CH_FUNC, &pw.buf)
                    .await
                    .map_err(map_err)?;
            }

            // Set ADC config
            let mux = ch["adc_mux"].as_u64().unwrap_or(0) as u8;
            let range = ch["adc_range"].as_u64().unwrap_or(0) as u8;
            let rate = ch["adc_rate"].as_u64().unwrap_or(0) as u8;
            let mut pw = PayloadWriter::new();
            pw.put_u8(ch_idx);
            pw.put_u8(mux);
            pw.put_u8(range);
            pw.put_u8(rate);
            mgr.send_command(bbp::CMD_SET_ADC_CONFIG, &pw.buf)
                .await
                .map_err(map_err)?;
        }
    }

    // Restore MUX states
    if let Some(mux_arr) = config["mux_states"].as_array() {
        let states: Vec<u8> = mux_arr
            .iter()
            .map(|v| v.as_u64().unwrap_or(0) as u8)
            .collect();
        if states.len() >= 4 {
            mgr.send_command(bbp::CMD_MUX_SET_ALL, &states[..4])
                .await
                .map_err(map_err)?;
        }
    }

    // Restore IDAC codes
    if let Some(idac_arr) = config["idac_codes"].as_array() {
        for (i, v) in idac_arr.iter().enumerate() {
            if i >= 3 {
                break;
            }
            let code = v.as_i64().unwrap_or(0) as i8;
            let mut pw = PayloadWriter::new();
            pw.put_u8(i as u8);
            pw.put_u8(code as u8);
            mgr.send_command(bbp::CMD_IDAC_SET_CODE, &pw.buf)
                .await
                .map_err(map_err)?;
        }
    }

    // Restore PCA9535 enables
    if let Some(pca) = config.get("pca_enables") {
        let controls = [
            ("vadj1_en", 0u8),
            ("vadj2_en", 1u8),
            ("en_15v", 2u8),
            ("en_mux", 3u8),
            ("en_usb_hub", 4u8),
        ];
        for (key, ctrl_id) in &controls {
            if let Some(on) = pca[key].as_bool() {
                let mut pw = PayloadWriter::new();
                pw.put_u8(*ctrl_id);
                pw.put_bool(on);
                mgr.send_command(bbp::CMD_PCA_SET_CONTROL, &pw.buf)
                    .await
                    .map_err(map_err)?;
            }
        }
        // Restore e-fuse enables
        if let Some(efuse_arr) = pca["efuse_en"].as_array() {
            for (i, v) in efuse_arr.iter().enumerate() {
                if i >= 4 {
                    break;
                }
                let on = v.as_bool().unwrap_or(false);
                let mut pw = PayloadWriter::new();
                pw.put_u8(5 + i as u8); // e-fuse control IDs start at 5
                pw.put_bool(on);
                mgr.send_command(bbp::CMD_PCA_SET_CONTROL, &pw.buf)
                    .await
                    .map_err(map_err)?;
            }
        }
    }

    log::info!("Config imported from {}", path);
    Ok(())
}

#[tauri::command]
pub async fn save_board_profile(
    profile_json: String,
    target_path: Option<String>,
) -> CmdResult<String> {
    // Parse + validate the profile.
    let profile: serde_json::Value = serde_json::from_str(&profile_json).map_err(|e| {
        log::error!("[save_board_profile] invalid JSON: {}", e);
        format!("Invalid profile JSON: {}", e)
    })?;

    let name = profile["name"].as_str().ok_or_else(|| {
        log::error!("[save_board_profile] missing 'name' field");
        "Profile must have a 'name' field".to_string()
    })?;

    if name.is_empty() {
        log::error!("[save_board_profile] empty name");
        return Err("Profile name cannot be empty".into());
    }

    // If a user-picked path is supplied, write exactly there. Otherwise fall
    // back to the repo's python/bugbuster_mcp/board_profiles/ directory.
    let path = if let Some(t) = target_path.filter(|s| !s.is_empty()) {
        let p = std::path::PathBuf::from(t);
        if let Some(parent) = p.parent() {
            if !parent.as_os_str().is_empty() && !parent.exists() {
                std::fs::create_dir_all(parent).map_err(|e| {
                    log::error!("[save_board_profile] mkdir {:?} failed: {}", parent, e);
                    format!("Failed to create directory: {}", e)
                })?;
            }
        }
        p
    } else {
        // Sanitize name to prevent directory traversal.
        let safe_name: String = name
            .chars()
            .filter(|c| c.is_alphanumeric() || *c == '_' || *c == '-')
            .collect();

        // Resolve the repo-root board_profiles dir:
        //  - BUGBUSTER_REPO_ROOT override (useful for packaged builds), OR
        //  - CARGO_MANIFEST_DIR (DesktopApp/BugBuster/src-tauri) -> climb 3x.
        let mut path = if let Ok(root) = std::env::var("BUGBUSTER_REPO_ROOT") {
            std::path::PathBuf::from(root)
        } else {
            let manifest = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
            // src-tauri -> BugBuster -> DesktopApp -> <repo root>
            manifest
                .parent()
                .unwrap_or(&manifest)
                .parent()
                .unwrap_or(&manifest)
                .parent()
                .unwrap_or(&manifest)
                .to_path_buf()
        };
        path.push("python");
        path.push("bugbuster_mcp");
        path.push("board_profiles");

        if !path.exists() {
            std::fs::create_dir_all(&path).map_err(|e| {
                log::error!("[save_board_profile] mkdir {:?} failed: {}", path, e);
                format!("Failed to create directory: {}", e)
            })?;
        }

        path.push(format!("{}.json", safe_name));
        path
    };

    std::fs::write(&path, &profile_json).map_err(|e| {
        log::error!("[save_board_profile] write {:?} failed: {}", path, e);
        format!("Failed to write profile: {}", e)
    })?;

    let saved = path.to_string_lossy().to_string();
    log::info!("Board profile saved to {}", saved);
    Ok(saved)
}

/// Configure per-pin drive strength (e.g. 2 kΩ series resistor for protected
/// drive). Currently a stub that only logs the request; hardware wiring
/// (likely via PCA GPIO expander controlling series-resistor bypass switches)
/// is pending firmware support.
#[tauri::command]
pub async fn set_pin_drive_strength(pin: u8, drive: u8) -> CmdResult<()> {
    log::info!(
        "[set_pin_drive_strength] pin={} drive={} (stub — no firmware wiring yet)",
        pin,
        drive
    );
    Ok(())
}

/// Configure software current limit for one of the 4 efuse blocks.
/// efuse index: 0=VADJ1-A, 1=VADJ1-B, 2=VADJ2-A, 3=VADJ2-B.
/// Stub — logs only until firmware wiring lands.
#[tauri::command]
pub async fn set_efuse_config(efuse: u8, sw_limit_ma: u16, enabled: bool) -> CmdResult<()> {
    log::info!(
        "[set_efuse_config] efuse={} sw_limit_ma={} enabled={}",
        efuse,
        sw_limit_ma,
        enabled
    );
    Ok(())
}

/// Open a native "Save As" dialog for a board profile JSON file.
/// Returns the picked absolute path, or None if the user cancelled.
#[tauri::command]
pub async fn pick_profile_save_path(
    app: tauri::AppHandle,
    default_name: Option<String>,
) -> CmdResult<Option<String>> {
    use tauri_plugin_dialog::DialogExt;

    let mut dlg = app
        .dialog()
        .file()
        .set_title("Export Board Profile")
        .add_filter("Board Profile", &["json"]);
    if let Some(name) = default_name.filter(|s| !s.is_empty()) {
        let safe: String = name
            .chars()
            .filter(|c| c.is_alphanumeric() || *c == '_' || *c == '-')
            .collect();
        let fname = if safe.is_empty() {
            "board_profile.json".to_string()
        } else {
            format!("{}.json", safe)
        };
        dlg = dlg.set_file_name(&fname);
    } else {
        dlg = dlg.set_file_name("board_profile.json");
    }
    let path = dlg.blocking_save_file();
    Ok(path.map(|p| p.to_string()))
}

#[tauri::command]
pub async fn pick_config_save_file(app: tauri::AppHandle) -> CmdResult<Option<String>> {
    use tauri_plugin_dialog::DialogExt;

    let path = app
        .dialog()
        .file()
        .set_title("Export Config")
        .add_filter("JSON Files", &["json"])
        .set_file_name("bugbuster_config.json")
        .blocking_save_file();

    Ok(path.map(|p| p.to_string()))
}

#[tauri::command]
pub async fn pick_config_open_file(app: tauri::AppHandle) -> CmdResult<Option<String>> {
    use tauri_plugin_dialog::DialogExt;

    let path = app
        .dialog()
        .file()
        .set_title("Import Config")
        .add_filter("JSON Files", &["json"])
        .blocking_pick_file();

    Ok(path.map(|p| p.to_string()))
}

// =============================================================================
// Parse helpers — extracted from command handlers for unit-testability.
// Each function takes a raw response payload (&[u8]) and returns the parsed
// value.  Command handlers are thin wrappers that call these.
// =============================================================================

const SELFTEST_STATUS_WITH_WORKER_FLAGS_LEN: usize = 27;

pub fn parse_selftest_status(data: &[u8]) -> serde_json::Value {
    let mut r = bbp::PayloadReader::new(data);
    let boot_ran = r.get_bool().unwrap_or(false);
    let boot_passed = r.get_bool().unwrap_or(false);
    let vadj1_v = r.get_f32().unwrap_or(0.0);
    let vadj2_v = r.get_f32().unwrap_or(0.0);
    let vlogic_v = r.get_f32().unwrap_or(0.0);
    let cal_status = r.get_u8().unwrap_or(0);
    let cal_channel = r.get_u8().unwrap_or(0);
    let cal_points = r.get_u8().unwrap_or(0);
    let last_voltage_v = r.get_f32().unwrap_or(-1.0);
    let error_mv = r.get_f32().unwrap_or(0.0);
    let worker_enabled = r.get_bool().unwrap_or(false);
    let supply_monitor_active = r.get_bool().unwrap_or(false);

    serde_json::json!({
        "boot": {
            "ran":     boot_ran,
            "passed":  boot_passed,
            "vadj1V":  vadj1_v,
            "vadj2V":  vadj2_v,
            "vlogicV": vlogic_v,
        },
        "cal": {
            "status":  cal_status,
            "channel": cal_channel,
            "points":  cal_points,
            "lastVoltageV": last_voltage_v,
            "errorMv": error_mv,
        },
        "workerEnabled": worker_enabled,
        "supplyMonitorActive": supply_monitor_active,
    })
}

fn apply_legacy_selftest_worker_fallback(status: &mut serde_json::Value, worker_enabled: bool) {
    if let Some(obj) = status.as_object_mut() {
        obj.insert(
            "workerEnabled".to_string(),
            serde_json::json!(worker_enabled),
        );
        // Older USB firmware exposes the worker query byte but not the active
        // reservation flag. Treat enabled as reserved so CH-D cannot be edited
        // out from under the supply monitor.
        obj.insert(
            "supplyMonitorActive".to_string(),
            serde_json::json!(worker_enabled),
        );
    }
}

fn normalize_selftest_status_json(json: &serde_json::Value) -> serde_json::Value {
    let boot = json.get("boot");
    let cal = json.get("cal").or_else(|| json.get("calibration"));
    serde_json::json!({
        "boot": {
            "ran":     boot.and_then(|v| v.get("ran")).and_then(|v| v.as_bool()).unwrap_or(false),
            "passed":  boot.and_then(|v| v.get("passed")).and_then(|v| v.as_bool()).unwrap_or(false),
            "vadj1V":  boot.and_then(|v| v.get("vadj1V")).and_then(|v| v.as_f64()).unwrap_or(0.0),
            "vadj2V":  boot.and_then(|v| v.get("vadj2V")).and_then(|v| v.as_f64()).unwrap_or(0.0),
            "vlogicV": boot.and_then(|v| v.get("vlogicV")).and_then(|v| v.as_f64()).unwrap_or(0.0),
        },
        "cal": {
            "status":  cal.and_then(|v| v.get("status")).and_then(|v| v.as_u64()).unwrap_or(0),
            "channel": cal.and_then(|v| v.get("channel")).and_then(|v| v.as_u64()).unwrap_or(0),
            "points":  cal.and_then(|v| v.get("points")).and_then(|v| v.as_u64()).unwrap_or(0),
            "lastVoltageV": cal.and_then(|v| v.get("lastVoltageV")).and_then(|v| v.as_f64()).unwrap_or(-1.0),
            "errorMv": cal.and_then(|v| v.get("errorMv")).and_then(|v| v.as_f64()).unwrap_or(0.0),
        },
        "workerEnabled": json.get("workerEnabled").and_then(|v| v.as_bool()).unwrap_or(false),
        "supplyMonitorActive": json.get("supplyMonitorActive").and_then(|v| v.as_bool()).unwrap_or(false),
    })
}

pub fn parse_selftest_auto_cal(data: &[u8]) -> serde_json::Value {
    let status = data.get(0).copied().unwrap_or(3);
    let cal_ch = data.get(1).copied().unwrap_or(0);
    let points = data.get(2).copied().unwrap_or(0);
    let last_voltage_v = if data.len() >= 7 {
        f32::from_le_bytes([data[3], data[4], data[5], data[6]])
    } else {
        -1.0
    };
    let error_mv = if data.len() >= 11 {
        f32::from_le_bytes([data[7], data[8], data[9], data[10]])
    } else {
        0.0
    };
    serde_json::json!({
        "status":  status,
        "channel": cal_ch,
        "points":  points,
        "lastVoltageV": last_voltage_v,
        "errorMv": error_mv,
    })
}

pub fn parse_selftest_measure_supply(rail: u8, data: &[u8]) -> serde_json::Value {
    let mut r = bbp::PayloadReader::new(data);
    serde_json::json!({
        "rail":    r.get_u8().unwrap_or(rail),
        "voltage": r.get_f32().unwrap_or(0.0),
    })
}

pub fn parse_selftest_supplies_cached(data: &[u8]) -> crate::state::SelftestSuppliesCached {
    use crate::state::{SelftestSuppliesCached, SupplyRail};
    let mut r = bbp::PayloadReader::new(data);
    let available = r.get_bool().unwrap_or(false);
    let timestamp_ms = r.get_u32().unwrap_or(0);
    let rail_names = ["VADJ1", "VADJ2", "VLOGIC"];
    let rails: Vec<SupplyRail> = rail_names
        .iter()
        .enumerate()
        .map(|(i, name)| SupplyRail {
            rail: i as u8,
            name: name.to_string(),
            voltage_v: r.get_f32().unwrap_or(-1.0),
        })
        .collect();
    SelftestSuppliesCached {
        available,
        timestamp_ms,
        rails,
    }
}

pub fn parse_quicksetup_list(data: &[u8]) -> QuickSetupList {
    let mut slots = Vec::with_capacity(QUICKSETUP_SLOT_COUNT as usize);
    if data.len() >= 5 {
        let occupied_bitmap = data[0];
        for i in 0..QUICKSETUP_SLOT_COUNT {
            slots.push(QuickSetupSlot {
                index: i,
                occupied: (occupied_bitmap & (1u8 << i)) != 0,
                summary_hash: data.get(1 + i as usize).copied().unwrap_or(0),
            });
        }
    } else if data.len() == QUICKSETUP_SLOT_COUNT as usize {
        // Compatibility with early firmware drafts that returned one byte per
        // slot and used zero as "empty".
        for i in 0..QUICKSETUP_SLOT_COUNT {
            let summary_hash = data.get(i as usize).copied().unwrap_or(0);
            slots.push(QuickSetupSlot {
                index: i,
                occupied: summary_hash != 0,
                summary_hash,
            });
        }
    } else {
        let occupied_bitmap = data.first().copied().unwrap_or(0);
        for i in 0..QUICKSETUP_SLOT_COUNT {
            slots.push(QuickSetupSlot {
                index: i,
                occupied: (occupied_bitmap & (1u8 << i)) != 0,
                summary_hash: 0,
            });
        }
    }

    QuickSetupList {
        supported: true,
        slots,
    }
}

pub fn parse_quicksetup_payload(slot: u8, data: &[u8]) -> CmdResult<QuickSetupPayload> {
    if data.len() > QUICKSETUP_MAX_JSON_BYTES {
        return Err(format!(
            "quick-setup slot {} payload too large: {} bytes (max {})",
            slot,
            data.len(),
            QUICKSETUP_MAX_JSON_BYTES
        ));
    }
    let json = String::from_utf8(data.to_vec())
        .map_err(|e| format!("quick-setup slot {} returned invalid UTF-8: {}", slot, e))?;
    let name = serde_json::from_str::<serde_json::Value>(&json)
        .ok()
        .and_then(|v| v.get("name").and_then(|n| n.as_str()).map(str::to_string));
    Ok(QuickSetupPayload {
        slot,
        byte_len: data.len(),
        json,
        name,
    })
}

pub fn parse_quicksetup_action(slot: u8, data: &[u8], apply: bool) -> QuickSetupActionResult {
    let status = data.first().copied().unwrap_or(2);
    let (ok, message) = if apply {
        match status {
            0 => (true, "applied"),
            1 => (false, "slot empty"),
            2 => (false, "apply error"),
            _ => (false, "unknown status"),
        }
    } else {
        match status {
            0 => (true, "deleted"),
            1 => (false, "not found"),
            _ => (false, "unknown status"),
        }
    };
    QuickSetupActionResult {
        slot,
        status,
        ok,
        message: message.to_string(),
    }
}

pub fn parse_hat_get_power(data: &[u8]) -> serde_json::Value {
    let mut r = bbp::PayloadReader::new(data);
    let mut connectors = Vec::new();
    for _ in 0..2 {
        let enabled = r.get_bool().unwrap_or(false);
        let current_ma = r.get_f32().unwrap_or(0.0);
        let fault = r.get_bool().unwrap_or(false);
        connectors.push(serde_json::json!({
            "enabled":   enabled,
            "currentMa": current_ma,
            "fault":     fault,
        }));
    }
    let io_voltage_mv = r.get_u16().unwrap_or(0);
    serde_json::json!({ "connectors": connectors, "ioVoltageMv": io_voltage_mv })
}

pub fn parse_usbpd_status(data: &[u8]) -> UsbPdState {
    let mut r = bbp::PayloadReader::new(data);
    let present = r.get_bool().unwrap_or(false);
    let attached = r.get_bool().unwrap_or(false);
    let cc_dir = r.get_bool().unwrap_or(false);
    let pd_response = r.get_u8().unwrap_or(0);
    let _voltage_code = r.get_u8().unwrap_or(0);
    let _current_code = r.get_u8().unwrap_or(0);
    let voltage_v = r.get_f32().unwrap_or(0.0);
    let current_a = r.get_f32().unwrap_or(0.0);
    let power_w = r.get_f32().unwrap_or(0.0);

    let pdo_names = ["5V", "9V", "12V", "15V", "18V", "20V"];
    let pdo_volts = [5.0f32, 9.0, 12.0, 15.0, 18.0, 20.0];
    let mut source_pdos = Vec::new();
    for i in 0..6 {
        let detected = r.get_bool().unwrap_or(false);
        let cur_code = r.get_u8().unwrap_or(0);
        let max_a = decode_husb_current(cur_code);
        source_pdos.push(UsbPdPdo {
            voltage: pdo_names[i].to_string(),
            detected,
            max_current_a: max_a,
            max_power_w: pdo_volts[i] * max_a,
        });
    }
    let selected_pdo = r.get_u8().unwrap_or(0);
    UsbPdState {
        present,
        attached,
        cc: if cc_dir { "CC2".into() } else { "CC1".into() },
        voltage_v,
        current_a,
        power_w,
        pd_response,
        source_pdos,
        selected_pdo,
    }
}

pub fn parse_idac_status(data: &[u8]) -> IdacState {
    let mut r = bbp::PayloadReader::new(data);
    let present = r.get_bool().unwrap_or(false);
    let mut channels = Vec::new();
    let names = ["LevelShift", "V_ADJ1", "V_ADJ2", "Spare"];
    for i in 0..4 {
        let _ch = r.get_u8();
        let code = r.get_u8().unwrap_or(0) as i8;
        let target_v = r.get_f32().unwrap_or(0.0);
        let _actual_v = r.get_f32().unwrap_or(0.0);
        let midpoint_v = r.get_f32().unwrap_or(0.0);
        let v_min = r.get_f32().unwrap_or(0.0);
        let v_max = r.get_f32().unwrap_or(0.0);
        let step_mv = r.get_f32().unwrap_or(0.0);
        let calibrated = r.get_bool().unwrap_or(false);
        let poly_valid = r.get_bool().unwrap_or(false);
        let a0 = r.get_f32().unwrap_or(0.0);
        let a1 = r.get_f32().unwrap_or(0.0);
        let a2 = r.get_f32().unwrap_or(0.0);
        let a3 = r.get_f32().unwrap_or(0.0);
        let cal_poly = if poly_valid {
            Some([a0, a1, a2, a3])
        } else {
            None
        };
        if i < 3 {
            channels.push(IdacChannelState {
                code,
                target_v,
                midpoint_v,
                v_min,
                v_max,
                step_mv,
                calibrated,
                cal_poly,
                name: names[i].to_string(),
            });
        }
    }
    IdacState { present, channels }
}

pub fn parse_pca_status(data: &[u8]) -> IoExpState {
    let mut r = bbp::PayloadReader::new(data);
    let present = r.get_bool().unwrap_or(false);
    let input0 = r.get_u8().unwrap_or(0);
    let input1 = r.get_u8().unwrap_or(0);
    let output0 = r.get_u8().unwrap_or(0);
    let output1 = r.get_u8().unwrap_or(0);
    let logic_pg = r.get_bool().unwrap_or(false);
    let vadj1_pg = r.get_bool().unwrap_or(false);
    let vadj2_pg = r.get_bool().unwrap_or(false);
    let mut efuse_flt = [false; 4];
    for i in 0..4 {
        efuse_flt[i] = r.get_bool().unwrap_or(false);
    }
    let vadj1_en = r.get_bool().unwrap_or(false);
    let vadj2_en = r.get_bool().unwrap_or(false);
    let en_15v = r.get_bool().unwrap_or(false);
    let en_mux = r.get_bool().unwrap_or(false);
    let en_usb_hub = r.get_bool().unwrap_or(false);
    let mut efuse_en = [false; 4];
    for i in 0..4 {
        efuse_en[i] = r.get_bool().unwrap_or(false);
    }
    let efuses = (0..4)
        .map(|i| EfuseState {
            id: (i + 1) as u8,
            enabled: efuse_en[i],
            fault: efuse_flt[i],
        })
        .collect();
    IoExpState {
        present,
        input0,
        input1,
        output0,
        output1,
        logic_pg,
        vadj1_pg,
        vadj2_pg,
        vadj1_en,
        vadj2_en,
        en_15v,
        en_mux,
        en_usb_hub,
        efuses,
    }
}

pub fn parse_hat_status(data: &[u8]) -> HatStatus {
    let mut r = bbp::PayloadReader::new(data);
    let detected = r.get_bool().unwrap_or(false);
    let connected = r.get_bool().unwrap_or(false);
    let hat_type = r.get_u8().unwrap_or(0);
    let detect_voltage = r.get_f32().unwrap_or(0.0);
    let fw_major = r.get_u8().unwrap_or(0);
    let fw_minor = r.get_u8().unwrap_or(0);
    let config_confirmed = r.get_bool().unwrap_or(false);
    let mut pin_config = vec![0u8; 4];
    for i in 0..4 {
        pin_config[i] = r.get_u8().unwrap_or(0);
    }
    let mut connectors = vec![HatConnectorStatus::default(), HatConnectorStatus::default()];
    for c in connectors.iter_mut() {
        c.enabled = r.get_bool().unwrap_or(false);
        c.current_ma = r.get_f32().unwrap_or(0.0);
        c.fault = r.get_bool().unwrap_or(false);
    }
    let io_voltage_mv = r.get_u16().unwrap_or(0);
    let dap_connected = r.get_bool().unwrap_or(false);
    let target_detected = r.get_bool().unwrap_or(false);
    let target_dpidr = r.get_u32().unwrap_or(0);
    HatStatus {
        detected,
        connected,
        hat_type,
        detect_voltage,
        fw_major,
        fw_minor,
        config_confirmed,
        pin_config,
        connectors,
        io_voltage_mv,
        dap_connected,
        target_detected,
        target_dpidr,
    }
}

pub fn parse_hat_detect(data: &[u8]) -> HatStatus {
    let mut r = bbp::PayloadReader::new(data);
    let detected = r.get_bool().unwrap_or(false);
    let hat_type = r.get_u8().unwrap_or(0);
    let detect_voltage = r.get_f32().unwrap_or(0.0);
    let connected = r.get_bool().unwrap_or(false);
    HatStatus {
        detected,
        connected,
        hat_type,
        detect_voltage,
        ..Default::default()
    }
}

/// Parse a WiFi status response (length-prefixed strings).
pub fn parse_wifi_status(data: &[u8]) -> WifiState {
    let mut r = bbp::PayloadReader::new(data);

    let connected = r.get_bool().unwrap_or(false);

    let ssid_len = r.get_u8().unwrap_or(0) as usize;
    let sta_ssid = if ssid_len > 0 && r.remaining() >= ssid_len {
        let s = String::from_utf8_lossy(&data[r.pos()..r.pos() + ssid_len]).to_string();
        r.skip(ssid_len);
        s
    } else {
        String::new()
    };

    let ip_len = r.get_u8().unwrap_or(0) as usize;
    let sta_ip = if ip_len > 0 && r.remaining() >= ip_len {
        let s = String::from_utf8_lossy(&data[r.pos()..r.pos() + ip_len]).to_string();
        r.skip(ip_len);
        s
    } else {
        String::new()
    };

    let rssi = r.get_u32().unwrap_or(0) as i32;

    let ap_ssid_len = r.get_u8().unwrap_or(0) as usize;
    let ap_ssid = if ap_ssid_len > 0 && r.remaining() >= ap_ssid_len {
        let s = String::from_utf8_lossy(&data[r.pos()..r.pos() + ap_ssid_len]).to_string();
        r.skip(ap_ssid_len);
        s
    } else {
        String::new()
    };

    let ap_ip_len = r.get_u8().unwrap_or(0) as usize;
    let ap_ip = if ap_ip_len > 0 && r.remaining() >= ap_ip_len {
        let s = String::from_utf8_lossy(&data[r.pos()..r.pos() + ap_ip_len]).to_string();
        r.skip(ap_ip_len);
        s
    } else {
        String::new()
    };

    let ap_mac_len = r.get_u8().unwrap_or(0) as usize;
    let ap_mac = if ap_mac_len > 0 && r.remaining() >= ap_mac_len {
        let s = String::from_utf8_lossy(&data[r.pos()..r.pos() + ap_mac_len]).to_string();
        r.skip(ap_mac_len);
        s
    } else {
        String::new()
    };

    WifiState {
        connected,
        sta_ssid,
        sta_ip,
        rssi,
        ap_ssid,
        ap_ip,
        ap_mac,
    }
}

/// Parse a WiFi scan response, returning deduplicated networks sorted by RSSI.
pub fn parse_wifi_scan(data: &[u8]) -> Vec<WifiNetwork> {
    let mut r = bbp::PayloadReader::new(data);
    let count = r.get_u8().unwrap_or(0) as usize;
    let mut networks = Vec::with_capacity(count);
    for _ in 0..count {
        let slen = r.get_u8().unwrap_or(0) as usize;
        if r.remaining() < slen + 2 {
            break;
        }
        let ssid = String::from_utf8_lossy(&data[r.pos()..r.pos() + slen]).to_string();
        r.skip(slen);
        let rssi = r.get_u8().unwrap_or(0) as i8 as i32;
        let auth = r.get_u8().unwrap_or(0);
        if !ssid.is_empty() {
            networks.push(WifiNetwork { ssid, rssi, auth });
        }
    }
    dedup_wifi_networks(networks)
}

fn dedup_wifi_networks(mut networks: Vec<WifiNetwork>) -> Vec<WifiNetwork> {
    networks.sort_by(|a, b| b.rssi.cmp(&a.rssi));
    let mut seen = std::collections::HashSet::new();
    networks.retain(|n| seen.insert(n.ssid.clone()));
    networks
}

// =============================================================================
// Unit Tests
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bbp::PayloadWriter;

    // -------------------------------------------------------------------------
    // Helper: build a raw payload from a closure operating on a PayloadWriter
    // -------------------------------------------------------------------------
    fn build<F: FnOnce(&mut PayloadWriter)>(f: F) -> Vec<u8> {
        let mut w = PayloadWriter::new();
        f(&mut w);
        w.buf
    }

    // -------------------------------------------------------------------------
    // decode_husb_current — all 16 defined codes + unknown code
    // -------------------------------------------------------------------------

    #[test]
    fn husb_current_code_0_is_0_5a() {
        assert_eq!(decode_husb_current(0), 0.5);
    }

    #[test]
    fn husb_current_code_15_is_5a() {
        assert_eq!(decode_husb_current(15), 5.0);
    }

    #[test]
    fn husb_current_all_codes() {
        let expected = [
            0.5, 0.7, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25, 3.5, 4.0, 4.5, 5.0,
        ];
        for (code, &exp) in expected.iter().enumerate() {
            assert_eq!(
                decode_husb_current(code as u8),
                exp,
                "code {} should map to {} A",
                code,
                exp
            );
        }
    }

    #[test]
    fn husb_current_unknown_code_returns_zero() {
        assert_eq!(decode_husb_current(16), 0.0);
        assert_eq!(decode_husb_current(255), 0.0);
    }

    // -------------------------------------------------------------------------
    // raw_to_voltage_f64 — all 8 ADC ranges
    // -------------------------------------------------------------------------

    #[test]
    fn raw_voltage_range0_zero_code_is_0v() {
        // Range 0: 0..12V unipolar.  code=0 → 0 V
        let v = raw_to_voltage_f64(0, 0);
        assert!(
            (v - 0.0).abs() < 1e-6,
            "range 0 zero code: expected 0 V, got {}",
            v
        );
    }

    #[test]
    fn raw_voltage_range0_full_scale_is_12v() {
        let fs = 16_777_216u32; // 2^24
        let v = raw_to_voltage_f64(fs, 0); // full-scale (exclusive upper bound)
        assert!(
            (v - 12.0).abs() < 1e-4,
            "range 0 full scale: expected 12 V, got {}",
            v
        );
    }

    #[test]
    fn raw_voltage_range1_bipolar_12v() {
        // Range 1: -12..+12V.  code=0 → -12 V, mid → 0 V, full → +12 V
        let fs = 16_777_216u32;
        let low = raw_to_voltage_f64(0, 1);
        let mid = raw_to_voltage_f64(fs / 2, 1);
        let high = raw_to_voltage_f64(fs, 1);
        assert!((low - (-12.0)).abs() < 1e-4, "range 1 low: {}", low);
        assert!(mid.abs() < 0.01, "range 1 mid: {}", mid);
        assert!((high - 12.0).abs() < 1e-4, "range 1 high: {}", high);
    }

    #[test]
    fn raw_voltage_range7_bipolar_5v() {
        // Range 7: -2.5..+2.5V.  code=0 → -2.5 V, full → +2.5 V
        let fs = 16_777_216u32;
        let low = raw_to_voltage_f64(0, 7);
        let high = raw_to_voltage_f64(fs, 7);
        assert!((low - (-2.5)).abs() < 1e-4, "range 7 low: {}", low);
        assert!((high - 2.5).abs() < 1e-4, "range 7 high: {}", high);
    }

    #[test]
    fn raw_voltage_range6_matches_datasheet_endpoints() {
        // Range 6: ±104.16 mV. Use the exact datasheet transfer values.
        let fs = 16_777_216u32;
        let low = raw_to_voltage_f64(0, 6);
        let high = raw_to_voltage_f64(fs, 6);
        assert!((low - (-0.104167)).abs() < 1e-6, "range 6 low: {}", low);
        assert!((high - 0.104166).abs() < 5e-6, "range 6 high: {}", high);
    }

    #[test]
    fn raw_voltage_unknown_range_falls_back_to_range0() {
        // Unknown range → same as range 0 (0..12 V)
        let v_default = raw_to_voltage_f64(0, 99);
        let v_range0 = raw_to_voltage_f64(0, 0);
        assert_eq!(v_default, v_range0);
    }

    // -------------------------------------------------------------------------
    // parse_selftest_status
    // -------------------------------------------------------------------------

    #[test]
    fn parse_selftest_status_normal() {
        let data = build(|w| {
            w.put_bool(true); // boot.ran
            w.put_bool(true); // boot.passed
            w.put_f32(12.34); // boot.vadj1V
            w.put_f32(11.98); // boot.vadj2V
            w.put_f32(3.302); // boot.vlogicV
            w.put_u8(0); // cal.status = OK
            w.put_u8(2); // cal.channel
            w.put_u8(7); // cal.points
            w.put_f32(4.2); // cal.lastVoltageV
            w.put_f32(1.5); // cal.errorMv
            w.put_bool(true); // workerEnabled
            w.put_bool(true); // supplyMonitorActive
        });
        let v = parse_selftest_status(&data);
        assert_eq!(v["boot"]["ran"], true);
        assert_eq!(v["boot"]["passed"], true);
        assert!((v["boot"]["vadj1V"].as_f64().unwrap() - 12.34).abs() < 0.01);
        assert!((v["boot"]["vadj2V"].as_f64().unwrap() - 11.98).abs() < 0.01);
        assert!((v["boot"]["vlogicV"].as_f64().unwrap() - 3.302).abs() < 0.01);
        assert_eq!(v["cal"]["status"], 0);
        assert_eq!(v["cal"]["channel"], 2);
        assert_eq!(v["cal"]["points"], 7);
        assert!((v["cal"]["lastVoltageV"].as_f64().unwrap() - 4.2).abs() < 0.01);
        assert!((v["cal"]["errorMv"].as_f64().unwrap() - 1.5).abs() < 0.01);
        assert_eq!(v["workerEnabled"], true);
        assert_eq!(v["supplyMonitorActive"], true);
    }

    #[test]
    fn parse_selftest_status_old_firmware_defaults_worker_flags() {
        let data = build(|w| {
            w.put_bool(true);
            w.put_bool(true);
            w.put_f32(12.34);
            w.put_f32(11.98);
            w.put_f32(3.302);
            w.put_u8(0);
            w.put_u8(2);
            w.put_u8(7);
            w.put_f32(4.2);
            w.put_f32(1.5);
        });
        let v = parse_selftest_status(&data);
        assert_eq!(v["workerEnabled"], false);
        assert_eq!(v["supplyMonitorActive"], false);
    }

    #[test]
    fn legacy_selftest_status_fallback_reserves_channel_d_when_worker_enabled() {
        let mut v = parse_selftest_status(&build(|w| {
            w.put_bool(true);
            w.put_bool(true);
            w.put_f32(12.34);
            w.put_f32(11.98);
            w.put_f32(3.302);
            w.put_u8(0);
            w.put_u8(2);
            w.put_u8(7);
            w.put_f32(4.2);
            w.put_f32(1.5);
        }));

        apply_legacy_selftest_worker_fallback(&mut v, true);

        assert_eq!(v["workerEnabled"], true);
        assert_eq!(v["supplyMonitorActive"], true);
    }

    #[test]
    fn parse_selftest_status_empty_payload_gives_defaults() {
        let v = parse_selftest_status(&[]);
        assert_eq!(v["boot"]["ran"], false);
        assert_eq!(v["boot"]["passed"], false);
        assert_eq!(v["boot"]["vadj1V"], 0.0);
        assert_eq!(v["cal"]["status"], 0);
        assert_eq!(v["workerEnabled"], false);
        assert_eq!(v["supplyMonitorActive"], false);
    }

    // -------------------------------------------------------------------------
    // parse_selftest_auto_cal
    // -------------------------------------------------------------------------

    #[test]
    fn parse_selftest_auto_cal_success() {
        let last_voltage_v: f32 = 4.2;
        let error_mv: f32 = 2.5;
        let mut data = vec![0u8, 1u8, 5u8]; // status=OK, ch=1, points=5
        data.extend_from_slice(&last_voltage_v.to_le_bytes());
        data.extend_from_slice(&error_mv.to_le_bytes());
        let v = parse_selftest_auto_cal(&data);
        assert_eq!(v["status"], 0);
        assert_eq!(v["channel"], 1);
        assert_eq!(v["points"], 5);
        assert!((v["lastVoltageV"].as_f64().unwrap() - 4.2).abs() < 0.01);
        assert!((v["errorMv"].as_f64().unwrap() - 2.5).abs() < 0.01);
    }

    #[test]
    fn parse_selftest_auto_cal_empty_gives_defaults() {
        let v = parse_selftest_auto_cal(&[]);
        assert_eq!(v["status"], 3); // default "unknown" status
        assert_eq!(v["errorMv"], 0.0);
    }

    // -------------------------------------------------------------------------
    // parse_selftest_measure_supply
    // -------------------------------------------------------------------------

    #[test]
    fn parse_selftest_measure_supply_normal() {
        let data = build(|w| {
            w.put_u8(1); // rail 1
            w.put_f32(12.05); // voltage
        });
        let v = parse_selftest_measure_supply(1, &data);
        assert_eq!(v["rail"], 1);
        assert!((v["voltage"].as_f64().unwrap() - 12.05).abs() < 0.01);
    }

    #[test]
    fn parse_selftest_measure_supply_empty_uses_fallback_rail() {
        // If payload is empty, rail falls back to the argument
        let v = parse_selftest_measure_supply(2, &[]);
        assert_eq!(v["rail"], 2);
        assert_eq!(v["voltage"], 0.0);
    }

    // -------------------------------------------------------------------------
    // parse_selftest_supplies_cached
    // -------------------------------------------------------------------------

    #[test]
    fn parse_selftest_supplies_cached_normal() {
        let data = build(|w| {
            w.put_bool(true); // available
            w.put_u32(12345); // timestampMs
            w.put_f32(5.01); // VADJ1
            w.put_f32(-1.0); // VADJ2 disabled
            w.put_f32(3.31); // VLOGIC
        });
        let v = parse_selftest_supplies_cached(&data);
        assert!(v.available);
        assert_eq!(v.timestamp_ms, 12345);
        assert_eq!(v.rails.len(), 3);
        assert_eq!(v.rails[0].name, "VADJ1");
        assert!((v.rails[0].voltage_v - 5.01).abs() < 0.001);
        assert!((v.rails[1].voltage_v - (-1.0)).abs() < 0.001);
        assert_eq!(v.rails[2].name, "VLOGIC");
    }

    #[test]
    fn parse_selftest_supplies_cached_unavailable() {
        let data = build(|w| {
            w.put_bool(false);
            w.put_u32(0);
            for _ in 0..3 {
                w.put_f32(-1.0);
            }
        });
        let v = parse_selftest_supplies_cached(&data);
        assert!(!v.available);
        assert_eq!(v.rails.len(), 3);
    }

    // -------------------------------------------------------------------------
    // parse_quicksetup_*
    // -------------------------------------------------------------------------

    #[test]
    fn parse_quicksetup_list_bitmap_plus_hashes() {
        let v = parse_quicksetup_list(&[0b0000_0101, 0xAA, 0x00, 0xCC, 0x00]);
        assert!(v.supported);
        assert_eq!(v.slots.len(), 4);
        assert!(v.slots[0].occupied);
        assert!(!v.slots[1].occupied);
        assert!(v.slots[2].occupied);
        assert_eq!(v.slots[0].summary_hash, 0xAA);
        assert_eq!(v.slots[2].summary_hash, 0xCC);
    }

    #[test]
    fn parse_quicksetup_list_legacy_four_hashes() {
        let v = parse_quicksetup_list(&[0x00, 0x22, 0x00, 0x44]);
        assert!(!v.slots[0].occupied);
        assert!(v.slots[1].occupied);
        assert!(!v.slots[2].occupied);
        assert!(v.slots[3].occupied);
        assert_eq!(v.slots[3].summary_hash, 0x44);
    }

    #[test]
    fn parse_quicksetup_payload_extracts_name() {
        let v = parse_quicksetup_payload(2, br#"{"name":"Bench 5V","idac":{"codes":[0,1,2]}}"#)
            .unwrap();
        assert_eq!(v.slot, 2);
        assert_eq!(v.name.as_deref(), Some("Bench 5V"));
        assert_eq!(v.byte_len, 44);
    }

    #[test]
    fn parse_quicksetup_action_statuses() {
        let applied = parse_quicksetup_action(1, &[0], true);
        assert!(applied.ok);
        assert_eq!(applied.message, "applied");
        let missing = parse_quicksetup_action(1, &[1], false);
        assert!(!missing.ok);
        assert_eq!(missing.message, "not found");
    }

    // -------------------------------------------------------------------------
    // parse_hat_get_power
    // -------------------------------------------------------------------------

    #[test]
    fn parse_hat_get_power_both_connectors() {
        let data = build(|w| {
            // Connector 0: enabled, 150 mA, no fault
            w.put_bool(true);
            w.put_f32(150.0);
            w.put_bool(false);
            // Connector 1: disabled, 0 mA, no fault
            w.put_bool(false);
            w.put_f32(0.0);
            w.put_bool(false);
            // IO voltage
            w.put_u16(3300);
        });
        let v = parse_hat_get_power(&data);
        assert_eq!(v["connectors"][0]["enabled"], true);
        assert!((v["connectors"][0]["currentMa"].as_f64().unwrap() - 150.0).abs() < 1.0);
        assert_eq!(v["connectors"][0]["fault"], false);
        assert_eq!(v["connectors"][1]["enabled"], false);
        assert_eq!(v["ioVoltageMv"], 3300);
    }

    #[test]
    fn parse_hat_get_power_with_fault() {
        let data = build(|w| {
            w.put_bool(true);
            w.put_f32(500.0);
            w.put_bool(true); // fault!
            w.put_bool(false);
            w.put_f32(0.0);
            w.put_bool(false);
            w.put_u16(5000);
        });
        let v = parse_hat_get_power(&data);
        assert_eq!(v["connectors"][0]["fault"], true);
        assert_eq!(v["ioVoltageMv"], 5000);
    }

    // -------------------------------------------------------------------------
    // parse_usbpd_status
    // -------------------------------------------------------------------------

    #[test]
    fn parse_usbpd_status_attached_cc1() {
        let data = build(|w| {
            w.put_bool(true); // present
            w.put_bool(true); // attached
            w.put_bool(false); // cc_dir → CC1
            w.put_u8(1); // pd_response
            w.put_u8(3); // voltage_code (ignored)
            w.put_u8(6); // current_code (ignored)
            w.put_f32(12.0); // voltage_v
            w.put_f32(2.0); // current_a
            w.put_f32(24.0); // power_w
                             // 6 PDOs: 5V(detected, code=6=2A), others not detected
            w.put_bool(true);
            w.put_u8(6); // 5V, 2A
            w.put_bool(true);
            w.put_u8(10); // 9V, 3A
            w.put_bool(false);
            w.put_u8(0); // 12V
            w.put_bool(false);
            w.put_u8(0); // 15V
            w.put_bool(false);
            w.put_u8(0); // 18V
            w.put_bool(false);
            w.put_u8(0); // 20V
            w.put_u8(1); // selected_pdo
        });
        let s = parse_usbpd_status(&data);
        assert!(s.present);
        assert!(s.attached);
        assert_eq!(s.cc, "CC1");
        assert!((s.voltage_v - 12.0).abs() < 0.01);
        assert!((s.current_a - 2.0).abs() < 0.01);
        assert_eq!(s.source_pdos.len(), 6);
        assert!(s.source_pdos[0].detected);
        assert_eq!(s.source_pdos[0].voltage, "5V");
        assert!((s.source_pdos[0].max_current_a - 2.0).abs() < 0.01); // code 6 → 2.0A
        assert!(!s.source_pdos[2].detected);
        assert_eq!(s.selected_pdo, 1);
    }

    #[test]
    fn parse_usbpd_status_cc2_direction() {
        let data = build(|w| {
            w.put_bool(true); // present
            w.put_bool(true); // attached
            w.put_bool(true); // cc_dir → CC2
            for _ in 0..15 {
                w.put_u8(0);
            } // rest
        });
        let s = parse_usbpd_status(&data);
        assert_eq!(s.cc, "CC2");
    }

    // -------------------------------------------------------------------------
    // parse_wifi_status
    // -------------------------------------------------------------------------

    #[test]
    fn parse_wifi_status_connected() {
        let ssid = "MyNetwork";
        let ip = "192.168.1.101";
        let rssi: i32 = -67;
        let ap_ssid = "BugBuster-AP";
        let ap_ip = "192.168.4.1";
        let ap_mac = "AA:BB:CC:DD:EE:FF";

        let data = build(|w| {
            w.put_bool(true);
            w.put_u8(ssid.len() as u8);
            w.buf.extend_from_slice(ssid.as_bytes());
            w.put_u8(ip.len() as u8);
            w.buf.extend_from_slice(ip.as_bytes());
            w.put_u32(rssi as u32);
            w.put_u8(ap_ssid.len() as u8);
            w.buf.extend_from_slice(ap_ssid.as_bytes());
            w.put_u8(ap_ip.len() as u8);
            w.buf.extend_from_slice(ap_ip.as_bytes());
            w.put_u8(ap_mac.len() as u8);
            w.buf.extend_from_slice(ap_mac.as_bytes());
        });

        let s = parse_wifi_status(&data);
        assert!(s.connected);
        assert_eq!(s.sta_ssid, ssid);
        assert_eq!(s.sta_ip, ip);
        assert_eq!(s.rssi, rssi);
        assert_eq!(s.ap_ssid, ap_ssid);
        assert_eq!(s.ap_ip, ap_ip);
        assert_eq!(s.ap_mac, ap_mac);
    }

    #[test]
    fn parse_wifi_status_disconnected_empty_strings() {
        let data = build(|w| {
            w.put_bool(false);
            w.put_u8(0); // ssid_len = 0
            w.put_u8(0); // ip_len = 0
            w.put_u32(0); // rssi
            w.put_u8(0); // ap_ssid_len = 0
            w.put_u8(0); // ap_ip_len = 0
            w.put_u8(0); // ap_mac_len = 0
        });
        let s = parse_wifi_status(&data);
        assert!(!s.connected);
        assert!(s.sta_ssid.is_empty());
        assert!(s.sta_ip.is_empty());
        assert_eq!(s.rssi, 0);
    }

    // -------------------------------------------------------------------------
    // dedup_wifi_networks (via parse_wifi_scan with crafted payload)
    // -------------------------------------------------------------------------

    fn make_scan_payload(entries: &[(&str, i8, u8)]) -> Vec<u8> {
        build(|w| {
            w.put_u8(entries.len() as u8);
            for (ssid, rssi, auth) in entries {
                w.put_u8(ssid.len() as u8);
                w.buf.extend_from_slice(ssid.as_bytes());
                w.put_u8(*rssi as u8);
                w.put_u8(*auth);
            }
        })
    }

    #[test]
    fn wifi_scan_dedup_keeps_strongest_signal() {
        // Same SSID appears twice; we should keep the one with higher RSSI (-40 > -80)
        let data = make_scan_payload(&[("HomeNet", -80, 2), ("HomeNet", -40, 2)]);
        let nets = parse_wifi_scan(&data);
        assert_eq!(nets.len(), 1);
        assert_eq!(nets[0].ssid, "HomeNet");
        assert_eq!(nets[0].rssi, -40);
    }

    #[test]
    fn wifi_scan_dedup_preserves_unique_ssids() {
        let data = make_scan_payload(&[("Alpha", -50, 2), ("Beta", -60, 0), ("Gamma", -70, 2)]);
        let nets = parse_wifi_scan(&data);
        assert_eq!(nets.len(), 3);
        // Should be sorted strongest first
        assert_eq!(nets[0].ssid, "Alpha");
        assert_eq!(nets[1].ssid, "Beta");
        assert_eq!(nets[2].ssid, "Gamma");
    }

    #[test]
    fn wifi_scan_empty_payload_returns_empty() {
        let data = build(|w| w.put_u8(0));
        let nets = parse_wifi_scan(&data);
        assert!(nets.is_empty());
    }

    #[test]
    fn wifi_scan_skips_empty_ssid_entries() {
        // SSID with length 0 should be filtered out
        let data = build(|w| {
            w.put_u8(2); // 2 entries
                         // Entry with empty SSID
            w.put_u8(0); // slen = 0  -- but remaining < slen+2 guard fires before filter
                         // Since slen=0 and remaining >= 0+2=2, we proceed.
                         // rssi and auth bytes must be present; ssid will be empty string → filtered
            w.put_u8(0xC0u8); // rssi = -64
            w.put_u8(2); // auth
                         // Valid entry
            w.put_u8(4);
            w.buf.extend_from_slice(b"Test");
            w.put_u8(0xD0u8); // rssi = -48
            w.put_u8(0);
        });
        let nets = parse_wifi_scan(&data);
        // Only the non-empty SSID should survive
        assert_eq!(nets.len(), 1);
        assert_eq!(nets[0].ssid, "Test");
    }

    // -------------------------------------------------------------------------
    // parse_pca_status
    // -------------------------------------------------------------------------

    #[test]
    fn parse_pca_status_all_enabled() {
        let data = build(|w| {
            w.put_bool(true); // present
            w.put_u8(0xFF); // input0
            w.put_u8(0x0F); // input1
            w.put_u8(0xAA); // output0
            w.put_u8(0x55); // output1
            w.put_bool(true); // logic_pg
            w.put_bool(true); // vadj1_pg
            w.put_bool(false); // vadj2_pg
                               // efuse faults (4 bools)
            w.put_bool(false);
            w.put_bool(false);
            w.put_bool(true);
            w.put_bool(false);
            w.put_bool(true); // vadj1_en
            w.put_bool(true); // vadj2_en
            w.put_bool(false); // en_15v
            w.put_bool(true); // en_mux
            w.put_bool(false); // en_usb_hub
                               // efuse enables (4 bools)
            w.put_bool(true);
            w.put_bool(true);
            w.put_bool(false);
            w.put_bool(true);
        });
        let s = parse_pca_status(&data);
        assert!(s.present);
        assert_eq!(s.input0, 0xFF);
        assert_eq!(s.output0, 0xAA);
        assert!(s.logic_pg);
        assert!(s.vadj1_pg);
        assert!(!s.vadj2_pg);
        assert!(s.vadj1_en);
        assert!(s.en_mux);
        assert!(!s.en_usb_hub);
        assert_eq!(s.efuses.len(), 4);
        assert!(s.efuses[2].fault); // efuse 3 had fault
        assert!(!s.efuses[0].fault);
        assert!(s.efuses[0].enabled);
        assert!(!s.efuses[2].enabled);
    }

    // -------------------------------------------------------------------------
    // parse_hat_status / parse_hat_detect
    // -------------------------------------------------------------------------

    #[test]
    fn parse_hat_status_full() {
        let data = build(|w| {
            w.put_bool(true); // detected
            w.put_bool(true); // connected
            w.put_u8(1); // hat_type
            w.put_f32(3.3); // detect_voltage
            w.put_u8(1); // fw_major
            w.put_u8(2); // fw_minor
            w.put_bool(true); // config_confirmed
            for _ in 0..4 {
                w.put_u8(0);
            } // pin_config
              // connector 0
            w.put_bool(true);
            w.put_f32(200.0);
            w.put_bool(false);
            // connector 1
            w.put_bool(false);
            w.put_f32(0.0);
            w.put_bool(false);
            w.put_u16(3300); // io_voltage_mv
            w.put_bool(true); // dap_connected
            w.put_bool(false); // target_detected
            w.put_u32(0); // target_dpidr
        });
        let s = parse_hat_status(&data);
        assert!(s.detected);
        assert!(s.connected);
        assert_eq!(s.hat_type, 1);
        assert!((s.detect_voltage - 3.3).abs() < 0.01);
        assert_eq!(s.fw_major, 1);
        assert_eq!(s.fw_minor, 2);
        assert!(s.config_confirmed);
        assert_eq!(s.io_voltage_mv, 3300);
        assert!(s.dap_connected);
        assert!(!s.target_detected);
        assert!(s.connectors[0].enabled);
        assert!((s.connectors[0].current_ma - 200.0).abs() < 1.0);
    }

    #[test]
    fn parse_hat_detect_minimal() {
        let data = build(|w| {
            w.put_bool(true); // detected
            w.put_u8(2); // hat_type
            w.put_f32(5.0); // detect_voltage
            w.put_bool(false); // connected
        });
        let s = parse_hat_detect(&data);
        assert!(s.detected);
        assert_eq!(s.hat_type, 2);
        assert!((s.detect_voltage - 5.0).abs() < 0.01);
        assert!(!s.connected);
        // Default fields should be zero/false
        assert_eq!(s.fw_major, 0);
        assert_eq!(s.io_voltage_mv, 0);
    }
}
