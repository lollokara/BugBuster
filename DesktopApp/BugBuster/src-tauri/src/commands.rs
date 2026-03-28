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

// -----------------------------------------------------------------------------
// Faults
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
        if i < 3 {
            channels.push(IdacChannelState {
                code, target_v, midpoint_v, v_min, v_max, step_mv, calibrated,
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
// CSV Recording
// -----------------------------------------------------------------------------

#[tauri::command]
pub fn start_csv_recording(path: String) -> CmdResult<()> {
    let file = File::create(&path).map_err(|e| format!("Failed to create CSV file: {}", e))?;
    let mut writer = BufWriter::new(file);
    writeln!(writer, "timestamp_ms,ch_a,ch_b,ch_c,ch_d")
        .map_err(|e| format!("Failed to write CSV header: {}", e))?;
    writer.flush().map_err(|e| format!("Failed to flush CSV header: {}", e))?;

    let mut guard = CSV_WRITER.lock().map_err(|e| format!("Lock error: {}", e))?;
    *guard = Some(writer);

    log::info!("CSV recording started: {}", path);
    Ok(())
}

#[tauri::command]
pub fn stop_csv_recording() -> CmdResult<()> {
    let mut guard = CSV_WRITER.lock().map_err(|e| format!("Lock error: {}", e))?;
    if let Some(mut writer) = guard.take() {
        writer.flush().map_err(|e| format!("Failed to flush CSV: {}", e))?;
    }
    log::info!("CSV recording stopped");
    Ok(())
}

#[tauri::command]
pub fn append_csv_data(timestamp_ms: f64, ch_values: Vec<f32>) -> CmdResult<()> {
    let mut guard = CSV_WRITER.lock().map_err(|e| format!("Lock error: {}", e))?;
    if let Some(ref mut writer) = *guard {
        let a = ch_values.first().copied().unwrap_or(0.0);
        let b = ch_values.get(1).copied().unwrap_or(0.0);
        let c = ch_values.get(2).copied().unwrap_or(0.0);
        let d = ch_values.get(3).copied().unwrap_or(0.0);
        writeln!(writer, "{},{},{},{},{}", timestamp_ms, a, b, c, d)
            .map_err(|e| format!("Failed to write CSV row: {}", e))?;
    }
    Ok(())
}
