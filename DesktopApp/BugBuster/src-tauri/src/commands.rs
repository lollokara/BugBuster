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

use serde_json;

/// Global CSV writer protected by a Mutex.
/// `None` means no recording is in progress.
pub static CSV_WRITER: Mutex<Option<BufWriter<File>>> = Mutex::new(None);

type CmdResult<T> = Result<T, String>;

fn map_err(e: anyhow::Error) -> String {
    e.to_string()
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
) -> CmdResult<()> {
    mgr.disconnect().await.map_err(map_err)
}

#[tauri::command]
pub fn get_connection_status(
    mgr: State<'_, ConnectionManager>,
) -> ConnectionStatus {
    mgr.get_connection_status()
}

// -----------------------------------------------------------------------------
// Device State
// -----------------------------------------------------------------------------

#[tauri::command]
pub fn get_device_state(
    mgr: State<'_, ConnectionManager>,
) -> DeviceState {
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
    mgr.send_command(bbp::CMD_SET_CH_FUNC, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_DAC_CODE, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn set_dac_voltage(
    channel: u8,
    voltage: f32,
    bipolar: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_f32(voltage);
    pw.put_bool(bipolar);
    mgr.send_command(bbp::CMD_SET_DAC_VOLTAGE, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_DAC_CURRENT, &pw.buf).await.map_err(map_err)?;
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
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    pw.put_u8(mux);
    pw.put_u8(range);
    pw.put_u8(rate);
    mgr.send_command(bbp::CMD_SET_ADC_CONFIG, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_DO_STATE, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_VOUT_RANGE, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_ILIMIT, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_GPIO_CONFIG, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_GPIO_VALUE, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_UART_CONFIG, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_SET_DIAG_CONFIG, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

// Level Shifter OE
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn set_lshift_oe(
    on: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_bool(on);
    mgr.send_command(bbp::CMD_SET_LSHIFT_OE, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// WiFi Management
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn wifi_get_status(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<WifiState> {
    let rsp = mgr.send_command(bbp::CMD_WIFI_GET_STATUS, &[]).await.map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);

    let connected = r.get_bool().unwrap_or(false);

    // Read length-prefixed strings
    let ssid_len = r.get_u8().unwrap_or(0) as usize;
    let sta_ssid = if ssid_len > 0 && r.remaining() >= ssid_len {
        let s = String::from_utf8_lossy(&rsp[r.pos()..r.pos() + ssid_len]).to_string();
        r.skip(ssid_len);
        s
    } else { String::new() };

    let ip_len = r.get_u8().unwrap_or(0) as usize;
    let sta_ip = if ip_len > 0 && r.remaining() >= ip_len {
        let s = String::from_utf8_lossy(&rsp[r.pos()..r.pos() + ip_len]).to_string();
        r.skip(ip_len);
        s
    } else { String::new() };

    let rssi = r.get_u32().unwrap_or(0) as i32;

    let ap_ssid_len = r.get_u8().unwrap_or(0) as usize;
    let ap_ssid = if ap_ssid_len > 0 && r.remaining() >= ap_ssid_len {
        let s = String::from_utf8_lossy(&rsp[r.pos()..r.pos() + ap_ssid_len]).to_string();
        r.skip(ap_ssid_len);
        s
    } else { String::new() };

    let ap_ip_len = r.get_u8().unwrap_or(0) as usize;
    let ap_ip = if ap_ip_len > 0 && r.remaining() >= ap_ip_len {
        let s = String::from_utf8_lossy(&rsp[r.pos()..r.pos() + ap_ip_len]).to_string();
        r.skip(ap_ip_len);
        s
    } else { String::new() };

    let ap_mac_len = r.get_u8().unwrap_or(0) as usize;
    let ap_mac = if ap_mac_len > 0 && r.remaining() >= ap_mac_len {
        let s = String::from_utf8_lossy(&rsp[r.pos()..r.pos() + ap_mac_len]).to_string();
        r.skip(ap_mac_len);
        s
    } else { String::new() };

    Ok(WifiState { connected, sta_ssid, sta_ip, rssi, ap_ssid, ap_ip, ap_mac })
}

#[tauri::command]
pub async fn wifi_connect(
    ssid: String,
    password: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<bool> {
    let mut pw = PayloadWriter::new();
    let ssid_bytes = ssid.as_bytes();
    pw.put_u8(ssid_bytes.len() as u8);
    pw.buf.extend_from_slice(ssid_bytes);
    let pass_bytes = password.as_bytes();
    pw.put_u8(pass_bytes.len() as u8);
    pw.buf.extend_from_slice(pass_bytes);
    let rsp = mgr.send_command(bbp::CMD_WIFI_CONNECT, &pw.buf).await.map_err(map_err)?;
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
pub async fn wifi_scan(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<Vec<WifiNetwork>> {
    let rsp = mgr.send_command(bbp::CMD_WIFI_SCAN, &[]).await.map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);
    let count = r.get_u8().unwrap_or(0) as usize;
    let mut networks = Vec::with_capacity(count);
    for _ in 0..count {
        let slen = r.get_u8().unwrap_or(0) as usize;
        if r.remaining() < slen + 2 { break; }
        let ssid = String::from_utf8_lossy(&rsp[r.pos()..r.pos() + slen]).to_string();
        r.skip(slen);
        let rssi = r.get_u8().unwrap_or(0) as i8 as i32;
        let auth = r.get_u8().unwrap_or(0);
        if !ssid.is_empty() {
            networks.push(WifiNetwork { ssid, rssi, auth });
        }
    }
    // Deduplicate by SSID, keep strongest signal
    networks.sort_by(|a, b| b.rssi.cmp(&a.rssi));
    let mut seen = std::collections::HashSet::new();
    networks.retain(|n| seen.insert(n.ssid.clone()));
    Ok(networks)
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
pub async fn get_firmware_info(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<FirmwareInfo> {
    // Try HTTP endpoint first (richer info)
    let base_url = mgr.get_base_url().await;
    if let Some(url) = base_url {
        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .build().map_err(|e| e.to_string())?;
        let resp = client.get(format!("{}/api/device/version", url))
            .send().await.map_err(|e| e.to_string())?;
        if resp.status().is_success() {
            let json: serde_json::Value = resp.json().await.map_err(|e| e.to_string())?;
            return Ok(FirmwareInfo {
                fw_version: format!("{}.{}.{}",
                    json.get("fwMajor").and_then(|v| v.as_u64()).unwrap_or(0),
                    json.get("fwMinor").and_then(|v| v.as_u64()).unwrap_or(0),
                    json.get("fwPatch").and_then(|v| v.as_u64()).unwrap_or(0)),
                proto_version: json.get("protoVersion").and_then(|v| v.as_u64()).unwrap_or(0) as u8,
                build_date: format!("{} {}",
                    json.get("date").and_then(|v| v.as_str()).unwrap_or("?"),
                    json.get("time").and_then(|v| v.as_str()).unwrap_or("")),
                idf_version: json.get("idfVersion").and_then(|v| v.as_str()).unwrap_or("?").to_string(),
                partition: json.get("partition").and_then(|v| v.as_str()).unwrap_or("?").to_string(),
                next_partition: json.get("nextPartition").and_then(|v| v.as_str()).unwrap_or("?").to_string(),
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
        return Err(format!("Invalid firmware size: {} bytes (expected 100KB-2MB)", size));
    }

    log::info!("OTA: uploading {} bytes from {}", size, file_path);

    // Get HTTP base URL (OTA only works over HTTP)
    let base_url = mgr.get_base_url().await
        .ok_or("OTA requires HTTP connection (WiFi). Connect via WiFi first.")?;

    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(120))
        .build().map_err(|e| e.to_string())?;

    let resp = client.post(format!("{}/api/ota/upload", base_url))
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

    Ok(format!("Firmware updated ({} bytes). Device is rebooting...", size))
}

// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn clear_all_alerts(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_CLEAR_ALL_ALERTS, &[]).await.map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn clear_channel_alert(
    channel: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_CLEAR_CH_ALERT, &[channel]).await.map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// System
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn device_reset(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_DEVICE_RESET, &[]).await.map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// MUX Switch Matrix
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn mux_set_all(
    states: Vec<u8>,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    if states.len() < 4 { return Err("Need 4 device states".to_string()); }
    mgr.send_command(bbp::CMD_MUX_SET_ALL, &states[..4]).await.map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn mux_get_all(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<Vec<u8>> {
    let rsp = mgr.send_command(bbp::CMD_MUX_GET_ALL, &[]).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_MUX_SET_SWITCH, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_START_ADC_STREAM, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn stop_adc_stream(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_STOP_ADC_STREAM, &[]).await.map_err(map_err)?;
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

    mgr.send_command(bbp::CMD_START_WAVEGEN, &pw.buf).await.map_err(map_err)?;

    log::info!("Wavegen started: ch={} mode={} wf={} freq={} amp={} off={}",
               channel, mode, waveform, freq_hz, amplitude, offset);
    Ok(())
}

#[tauri::command]
pub async fn stop_wavegen(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_STOP_WAVEGEN, &[]).await.map_err(map_err)?;
    log::info!("Wavegen stopped");
    Ok(())
}

// -----------------------------------------------------------------------------
// DS4424 IDAC
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn idac_get_status(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<IdacState> {
    let rsp = mgr.send_command(bbp::CMD_IDAC_GET_STATUS, &[]).await.map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);
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
        let cal_count = r.get_u8().unwrap_or(0);
        let mut cal_points = Vec::new();
        for _ in 0..cal_count {
            let pc = r.get_u8().unwrap_or(0) as i8;
            let pv = r.get_f32().unwrap_or(0.0);
            cal_points.push(IdacCalPoint { code: pc, voltage: pv });
        }
        if i < 3 {
            channels.push(IdacChannelState {
                code, target_v, midpoint_v, v_min, v_max, step_mv, calibrated,
                cal_points,
                name: names[i].to_string(),
            });
        }
    }
    Ok(IdacState { present, channels })
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
    mgr.send_command(bbp::CMD_IDAC_SET_CODE, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_IDAC_SET_VOLTAGE, &pw.buf).await.map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_IDAC_CAL_ADD_POINT, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn idac_cal_clear(
    channel: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(channel);
    mgr.send_command(bbp::CMD_IDAC_CAL_CLEAR, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn idac_cal_save(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_IDAC_CAL_SAVE, &[]).await.map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// PCA9535 GPIO Expander
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn pca_get_status(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<IoExpState> {
    let rsp = mgr.send_command(bbp::CMD_PCA_GET_STATUS, &[]).await.map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);
    let present = r.get_bool().unwrap_or(false);
    let input0 = r.get_u8().unwrap_or(0);
    let input1 = r.get_u8().unwrap_or(0);
    let output0 = r.get_u8().unwrap_or(0);
    let output1 = r.get_u8().unwrap_or(0);
    let logic_pg = r.get_bool().unwrap_or(false);
    let vadj1_pg = r.get_bool().unwrap_or(false);
    let vadj2_pg = r.get_bool().unwrap_or(false);
    let mut efuse_flt = [false; 4];
    for i in 0..4 { efuse_flt[i] = r.get_bool().unwrap_or(false); }
    let vadj1_en = r.get_bool().unwrap_or(false);
    let vadj2_en = r.get_bool().unwrap_or(false);
    let en_15v = r.get_bool().unwrap_or(false);
    let en_mux = r.get_bool().unwrap_or(false);
    let en_usb_hub = r.get_bool().unwrap_or(false);
    let mut efuse_en = [false; 4];
    for i in 0..4 { efuse_en[i] = r.get_bool().unwrap_or(false); }

    let efuses = (0..4).map(|i| EfuseState {
        id: (i + 1) as u8,
        enabled: efuse_en[i],
        fault: efuse_flt[i],
    }).collect();

    Ok(IoExpState {
        present, input0, input1, output0, output1,
        logic_pg, vadj1_pg, vadj2_pg,
        vadj1_en, vadj2_en, en_15v, en_mux, en_usb_hub,
        efuses,
    })
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
    mgr.send_command(bbp::CMD_PCA_SET_CONTROL, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// HUSB238 USB PD
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn usbpd_get_status(
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<UsbPdState> {
    let rsp = mgr.send_command(bbp::CMD_USBPD_GET_STATUS, &[]).await.map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);
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

    Ok(UsbPdState {
        present, attached,
        cc: if cc_dir { "CC2".into() } else { "CC1".into() },
        voltage_v, current_a, power_w, pd_response, source_pdos, selected_pdo,
    })
}

fn decode_husb_current(code: u8) -> f32 {
    match code {
        0 => 0.5, 1 => 0.7, 2 => 1.0, 3 => 1.25, 4 => 1.5, 5 => 1.75,
        6 => 2.0, 7 => 2.25, 8 => 2.5, 9 => 2.75, 10 => 3.0, 11 => 3.25,
        12 => 3.5, 13 => 4.0, 14 => 4.5, 15 => 5.0, _ => 0.0,
    }
}

#[tauri::command]
pub async fn usbpd_select_pdo(
    voltage: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = PayloadWriter::new();
    pw.put_u8(voltage);
    mgr.send_command(bbp::CMD_USBPD_SELECT_PDO, &pw.buf).await.map_err(map_err)?;
    // Trigger negotiation
    let mut pw2 = PayloadWriter::new();
    pw2.put_u8(0x01); // GO_SELECT_PDO
    mgr.send_command(bbp::CMD_USBPD_GO, &pw2.buf).await.map_err(map_err)?;
    Ok(())
}

// -----------------------------------------------------------------------------
// File Dialog
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn pick_save_file(app: tauri::AppHandle) -> CmdResult<Option<String>> {
    use tauri_plugin_dialog::DialogExt;

    let path = app.dialog()
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
    pub mask: u8,
    pub num_channels: u8,
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
    if num_ch == 0 { return Err("No channels selected".into()); }

    let file = File::create(&path).map_err(|e| format!("Failed to create file: {}", e))?;
    let mut writer = BufWriter::with_capacity(65536, file);  // 64KB buffer for throughput

    // Write magic
    writer.write_all(b"BBSC").map_err(|e| format!("Write error: {}", e))?;

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
    writer.write_all(&(header_bytes.len() as u32).to_le_bytes())
        .map_err(|e| format!("Write error: {}", e))?;
    writer.write_all(&header_bytes)
        .map_err(|e| format!("Write error: {}", e))?;
    writer.flush().map_err(|e| format!("Flush error: {}", e))?;

    let mut guard = RECORDING.lock().map_err(|e| format!("Lock error: {}", e))?;
    *guard = Some(RecordingState {
        writer,
        mask: channel_mask,
        num_channels: num_ch,
        sample_count: 0,
        path: path.clone(),
    });

    log::info!("BBSC recording started: {} (mask=0x{:02X}, {}ch, {}SPS)",
               path, channel_mask, num_ch, sample_rate);
    Ok(())
}

#[tauri::command]
pub fn stop_recording() -> CmdResult<u64> {
    let mut guard = RECORDING.lock().map_err(|e| format!("Lock error: {}", e))?;
    if let Some(mut rec) = guard.take() {
        rec.writer.flush().map_err(|e| format!("Flush error: {}", e))?;
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
    let rec = match guard.as_mut() { Some(r) => r, None => return Ok(()) };

    // Parse: [mask:1][timestamp:4][count:2][samples: count * num_ch * 3]
    if raw_payload.len() < 7 { return Ok(()); }
    let mask = raw_payload[0];
    let count = u16::from_le_bytes([raw_payload[5], raw_payload[6]]) as usize;

    // Only write the raw sample data (skip the 7-byte header)
    let data_start = 7;
    let num_ch = (0..4).filter(|b| mask & (1 << b) != 0).count();
    let data_len = count * num_ch * 3;
    let data_end = data_start + data_len;

    if raw_payload.len() >= data_end {
        rec.writer.write_all(&raw_payload[data_start..data_end])
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
    reader.read_exact(&mut magic).map_err(|e| format!("Read error: {}", e))?;
    if &magic != b"BBSC" { return Err("Not a BBSC file".into()); }

    // Read header length and header
    let mut hlen_buf = [0u8; 4];
    reader.read_exact(&mut hlen_buf).map_err(|e| format!("Read error: {}", e))?;
    let hlen = u32::from_le_bytes(hlen_buf) as usize;

    let mut header_buf = vec![0u8; hlen];
    reader.read_exact(&mut header_buf).map_err(|e| format!("Read error: {}", e))?;
    let header: serde_json::Value = serde_json::from_slice(&header_buf)
        .map_err(|e| format!("JSON parse error: {}", e))?;

    let mask = header["mask"].as_u64().unwrap_or(0x0F) as u8;
    let adc_ranges: Vec<u8> = header["adc_ranges"].as_array()
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
    let dt = if sample_rate > 0 { 1.0 / sample_rate as f64 } else { 0.001 };

    loop {
        match reader.read_exact(&mut sample_buf) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(format!("Read error: {}", e)),
        }

        let time_s = sample_idx as f64 * dt;
        let mut line = format!("{},{:.6}", sample_idx, time_s);

        let mut pos = 0;
        for (ci, &ch) in active_channels.iter().enumerate() {
            let raw = sample_buf[pos] as u32
                | ((sample_buf[pos + 1] as u32) << 8)
                | ((sample_buf[pos + 2] as u32) << 16);
            pos += 3;

            let range = if ch < adc_ranges.len() { adc_ranges[ch] } else { 0 };
            let voltage = raw_to_voltage_f64(raw, range);
            line.push_str(&format!(",{},{:.6}", raw, voltage));
        }

        writeln!(csv, "{}", line).map_err(|e| format!("Write error: {}", e))?;
        sample_idx += 1;
    }

    csv.flush().map_err(|e| format!("Flush error: {}", e))?;
    log::info!("Exported {} samples from {} to {}", sample_idx, bbsc_path, csv_path);
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
        6 => (code / fs * 0.208) - 0.104,
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
    let mut guard = CSV_WRITER.lock().map_err(|e| format!("Lock error: {}", e))?;
    *guard = Some(writer);
    Ok(())
}

#[tauri::command]
pub fn stop_csv_recording() -> CmdResult<()> {
    let mut guard = CSV_WRITER.lock().map_err(|e| format!("Lock error: {}", e))?;
    if let Some(mut w) = guard.take() { w.flush().ok(); }
    Ok(())
}

#[tauri::command]
pub fn append_csv_data(timestamp_ms: f64, ch_values: Vec<f32>) -> CmdResult<()> {
    let mut guard = CSV_WRITER.lock().map_err(|e| format!("Lock error: {}", e))?;
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
pub async fn export_config(
    path: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let state = mgr.get_device_state();

    // Channel functions and ADC config
    let channels: Vec<serde_json::Value> = (0..4).map(|i| {
        let ch = &state.channels[i];
        serde_json::json!({
            "function": ch.function,
            "adc_range": ch.adc_range,
            "adc_rate": ch.adc_rate,
            "adc_mux": ch.adc_mux,
        })
    }).collect();

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
                    // skip remaining fields per channel
                    let _target = r.get_f32();
                    let _actual = r.get_f32();
                    let _mid = r.get_f32();
                    let _vmin = r.get_f32();
                    let _vmax = r.get_f32();
                    let _step = r.get_f32();
                    let _cal = r.get_bool();
                    let cal_count = r.get_u8().unwrap_or(0);
                    for _ in 0..cal_count {
                        let _pc = r.get_u8();
                        let _pv = r.get_f32();
                    }
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
                let _i0 = r.get_u8(); let _i1 = r.get_u8();
                let _o0 = r.get_u8(); let _o1 = r.get_u8();
                let _lpg = r.get_bool(); let _v1pg = r.get_bool(); let _v2pg = r.get_bool();
                for _ in 0..4 { let _ef = r.get_bool(); }
                let vadj1_en = r.get_bool().unwrap_or(false);
                let vadj2_en = r.get_bool().unwrap_or(false);
                let en_15v = r.get_bool().unwrap_or(false);
                let en_mux = r.get_bool().unwrap_or(false);
                let en_usb_hub = r.get_bool().unwrap_or(false);
                let mut efuse_en = [false; 4];
                for i in 0..4 { efuse_en[i] = r.get_bool().unwrap_or(false); }
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
    std::fs::write(&path, json_str)
        .map_err(|e| format!("File write error: {}", e))?;

    log::info!("Config exported to {}", path);
    Ok(())
}

#[tauri::command]
pub async fn import_config(
    path: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let json_str = std::fs::read_to_string(&path)
        .map_err(|e| format!("File read error: {}", e))?;
    let config: serde_json::Value = serde_json::from_str(&json_str)
        .map_err(|e| format!("JSON parse error: {}", e))?;

    // Restore channel functions and ADC config
    if let Some(channels) = config["channels"].as_array() {
        for (i, ch) in channels.iter().enumerate() {
            if i >= 4 { break; }
            let ch_idx = i as u8;

            // Set channel function
            if let Some(func) = ch["function"].as_u64() {
                let mut pw = PayloadWriter::new();
                pw.put_u8(ch_idx);
                pw.put_u8(func as u8);
                mgr.send_command(bbp::CMD_SET_CH_FUNC, &pw.buf).await.map_err(map_err)?;
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
            mgr.send_command(bbp::CMD_SET_ADC_CONFIG, &pw.buf).await.map_err(map_err)?;
        }
    }

    // Restore MUX states
    if let Some(mux_arr) = config["mux_states"].as_array() {
        let states: Vec<u8> = mux_arr.iter()
            .map(|v| v.as_u64().unwrap_or(0) as u8)
            .collect();
        if states.len() >= 4 {
            mgr.send_command(bbp::CMD_MUX_SET_ALL, &states[..4]).await.map_err(map_err)?;
        }
    }

    // Restore IDAC codes
    if let Some(idac_arr) = config["idac_codes"].as_array() {
        for (i, v) in idac_arr.iter().enumerate() {
            if i >= 3 { break; }
            let code = v.as_i64().unwrap_or(0) as i8;
            let mut pw = PayloadWriter::new();
            pw.put_u8(i as u8);
            pw.put_u8(code as u8);
            mgr.send_command(bbp::CMD_IDAC_SET_CODE, &pw.buf).await.map_err(map_err)?;
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
                mgr.send_command(bbp::CMD_PCA_SET_CONTROL, &pw.buf).await.map_err(map_err)?;
            }
        }
        // Restore e-fuse enables
        if let Some(efuse_arr) = pca["efuse_en"].as_array() {
            for (i, v) in efuse_arr.iter().enumerate() {
                if i >= 4 { break; }
                let on = v.as_bool().unwrap_or(false);
                let mut pw = PayloadWriter::new();
                pw.put_u8(5 + i as u8);  // e-fuse control IDs start at 5
                pw.put_bool(on);
                mgr.send_command(bbp::CMD_PCA_SET_CONTROL, &pw.buf).await.map_err(map_err)?;
            }
        }
    }

    log::info!("Config imported from {}", path);
    Ok(())
}

#[tauri::command]
pub async fn pick_config_save_file(app: tauri::AppHandle) -> CmdResult<Option<String>> {
    use tauri_plugin_dialog::DialogExt;

    let path = app.dialog()
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

    let path = app.dialog()
        .file()
        .set_title("Import Config")
        .add_filter("JSON Files", &["json"])
        .blocking_pick_file();

    Ok(path.map(|p| p.to_string()))
}
