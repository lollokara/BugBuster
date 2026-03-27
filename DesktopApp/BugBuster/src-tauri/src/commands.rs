// =============================================================================
// commands.rs - Tauri command handlers exposed to the Leptos frontend
// =============================================================================

use tauri::State;

use crate::bbp::{self, PayloadWriter};
use crate::connection_manager::ConnectionManager;
use crate::state::*;

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
    mgr.connect(&device_id, &app).await.map_err(map_err)?;
    Ok(mgr.get_connection_status())
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
