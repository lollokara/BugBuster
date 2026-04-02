// =============================================================================
// la_commands.rs — Tauri commands for Logic Analyzer
//
// Provides commands to:
//   - Connect to RP2040 LA USB interface
//   - Read captured data
//   - Decode channels
//   - Manage LA state
// =============================================================================

use tauri::State;
use serde::{Serialize, Deserialize};
use std::sync::{Arc, Mutex};
use crate::la_usb::{LaUsbConnection, LaCaptureData, decode_capture, hat_usb_present};
use crate::la_store::{LaStore, LaViewData};
use crate::la_decoders::{self, Annotation, DecoderConfig};
use crate::connection_manager::ConnectionManager;
use crate::bbp;

type CmdResult<T> = Result<T, String>;
fn map_err(e: impl std::fmt::Display) -> String { e.to_string() }

/// Shared LA USB connection state
pub struct LaState {
    pub usb: Arc<Mutex<LaUsbConnection>>,
    pub last_capture: Mutex<Option<LaCaptureData>>,
    pub store: Mutex<Option<LaStore>>,
}

impl LaState {
    pub fn new() -> Self {
        Self {
            usb: Arc::new(Mutex::new(LaUsbConnection::new())),
            last_capture: Mutex::new(None),
            store: Mutex::new(None),
        }
    }
}

#[derive(Serialize, Deserialize)]
pub struct LaStatusResponse {
    pub usb_present: bool,
    pub usb_connected: bool,
    pub state: u8,          // 0=idle, 1=armed, 2=capturing, 3=done, 4=error
    pub state_name: String,
    pub channels: u8,
    pub samples_captured: u32,
    pub total_samples: u32,
    pub actual_rate_hz: u32,
    pub has_capture: bool,
}

/// Check if RP2040 LA is available on USB
#[tauri::command]
pub fn la_check_usb() -> CmdResult<bool> {
    Ok(hat_usb_present())
}

/// Connect to the RP2040 LA USB interface
#[tauri::command]
pub fn la_connect_usb(la: State<'_, LaState>) -> CmdResult<bool> {
    let mut usb = la.usb.lock().map_err(map_err)?;
    usb.connect().map_err(map_err)?;
    Ok(true)
}

/// Configure LA capture (sends command via ESP32 → UART → RP2040)
#[tauri::command]
pub async fn la_configure(
    channels: u8,
    rate_hz: u32,
    depth: u32,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_u8(channels);
    pw.put_u32(rate_hz);
    pw.put_u32(depth);
    mgr.send_command(bbp::CMD_HAT_LA_CONFIG, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

/// Set trigger condition
#[tauri::command]
pub async fn la_set_trigger(
    trigger_type: u8,
    channel: u8,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_u8(trigger_type);
    pw.put_u8(channel);
    mgr.send_command(bbp::CMD_HAT_LA_TRIGGER, &pw.buf).await.map_err(map_err)?;
    Ok(())
}

/// Arm the trigger
#[tauri::command]
pub async fn la_arm(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_LA_ARM, &[]).await.map_err(map_err)?;
    Ok(())
}

/// Force trigger
#[tauri::command]
pub async fn la_force(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_LA_FORCE, &[]).await.map_err(map_err)?;
    Ok(())
}

/// Stop capture
#[tauri::command]
pub async fn la_stop(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_LA_STOP, &[]).await.map_err(map_err)?;
    Ok(())
}

/// Get LA capture status
#[tauri::command]
pub async fn la_get_status(
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaStatusResponse> {
    let state_names = ["idle", "armed", "capturing", "done", "error"];

    let rsp = mgr.send_command(bbp::CMD_HAT_LA_STATUS, &[]).await.map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);
    let state = r.get_u8().unwrap_or(0);
    let channels = r.get_u8().unwrap_or(0);
    let samples_captured = r.get_u32().unwrap_or(0);
    let total_samples = r.get_u32().unwrap_or(0);
    let actual_rate_hz = r.get_u32().unwrap_or(0);

    let usb = la.usb.lock().map_err(map_err)?;
    let has_capture = la.last_capture.lock().map_err(map_err)?.is_some();

    Ok(LaStatusResponse {
        usb_present: hat_usb_present(),
        usb_connected: usb.is_connected(),
        state,
        state_name: state_names.get(state as usize).unwrap_or(&"unknown").to_string(),
        channels,
        samples_captured,
        total_samples,
        actual_rate_hz,
        has_capture,
    })
}

/// Read captured data from RP2040 via USB bulk endpoint
#[tauri::command]
pub async fn la_read_capture(
    channels: u8,
    sample_rate_hz: u32,
    total_samples: u32,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureData> {
    // Connect if needed (sync, no await)
    {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            usb.connect().map_err(map_err)?;
        }
    }

    // Read data in a blocking task (nusb bulk reads are sync via block_on)
    let usb_mutex = la.usb.clone();
    let raw = tokio::task::spawn_blocking(move || -> Result<Vec<u8>, String> {
        let mut usb = usb_mutex.lock().map_err(|e| e.to_string())?;
        usb.read_capture_blocking().map_err(|e| e.to_string())
    }).await.map_err(|e| e.to_string())??;

    let channel_data = decode_capture(&raw, channels);

    let capture = LaCaptureData {
        channels,
        sample_rate_hz,
        total_samples,
        raw_data: raw,
        channel_data,
    };

    // Build transition-based store for efficient viewport rendering
    let store = LaStore::from_raw(&capture.raw_data, channels, sample_rate_hz);

    *la.last_capture.lock().map_err(map_err)? = Some(capture.clone());
    *la.store.lock().map_err(map_err)? = Some(store);
    Ok(capture)
}

/// Get the last captured data (cached, no USB read)
#[tauri::command]
pub fn la_get_cached_capture(la: State<'_, LaState>) -> CmdResult<Option<LaCaptureData>> {
    let capture = la.last_capture.lock().map_err(map_err)?;
    Ok(capture.clone())
}

/// Get view data for a viewport range (efficient — only transitions in range)
#[tauri::command]
pub fn la_get_view(
    start_sample: u64,
    end_sample: u64,
    la: State<'_, LaState>,
) -> CmdResult<Option<LaViewData>> {
    let store = la.store.lock().map_err(map_err)?;
    match store.as_ref() {
        Some(s) => Ok(Some(s.to_view_data(start_sample, end_sample))),
        None => Ok(None),
    }
}

/// Load raw capture data directly (e.g. from test/simulation) and build store
#[tauri::command]
pub fn la_load_raw(
    raw_data: Vec<u8>,
    channels: u8,
    sample_rate_hz: u32,
    la: State<'_, LaState>,
) -> CmdResult<u64> {
    let store = LaStore::from_raw(&raw_data, channels, sample_rate_hz);
    let total = store.total_samples;
    *la.store.lock().map_err(map_err)? = Some(store);
    Ok(total)
}

/// Export capture as VCD string
#[tauri::command]
pub fn la_export_vcd(la: State<'_, LaState>) -> CmdResult<String> {
    let store = la.store.lock().map_err(map_err)?;
    match store.as_ref() {
        Some(s) => Ok(s.export_vcd()),
        None => Err("No capture data".into()),
    }
}

/// Get capture info (without full data)
#[tauri::command]
pub fn la_get_capture_info(la: State<'_, LaState>) -> CmdResult<Option<LaCaptureInfo>> {
    let store = la.store.lock().map_err(map_err)?;
    match store.as_ref() {
        Some(s) => Ok(Some(LaCaptureInfo {
            channels: s.channels,
            sample_rate_hz: s.sample_rate_hz,
            total_samples: s.total_samples,
            duration_sec: s.total_duration_sec(),
            trigger_sample: s.trigger_sample,
        })),
        None => Ok(None),
    }
}

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LaCaptureInfo {
    pub channels: u8,
    pub sample_rate_hz: u32,
    pub total_samples: u64,
    pub duration_sec: f64,
    pub trigger_sample: Option<u64>,
}

/// Export capture as VCD file
#[tauri::command]
pub fn la_export_vcd_file(
    path: String,
    la: State<'_, LaState>,
) -> CmdResult<()> {
    let store = la.store.lock().map_err(map_err)?;
    let s = store.as_ref().ok_or("No capture data")?;
    let vcd = s.export_vcd();
    std::fs::write(&path, vcd).map_err(|e| e.to_string())?;
    Ok(())
}

/// Export capture as JSON file (full capture data + metadata for reload)
#[tauri::command]
pub fn la_export_json(
    path: String,
    la: State<'_, LaState>,
) -> CmdResult<()> {
    let store = la.store.lock().map_err(map_err)?;
    let s = store.as_ref().ok_or("No capture data")?;

    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct ExportData {
        channels: u8,
        sample_rate_hz: u32,
        total_samples: u64,
        trigger_sample: Option<u64>,
        transitions: Vec<Vec<(u64, u8)>>,
    }

    let data = ExportData {
        channels: s.channels,
        sample_rate_hz: s.sample_rate_hz,
        total_samples: s.total_samples,
        trigger_sample: s.trigger_sample,
        transitions: s.transitions.clone(),
    };
    let json = serde_json::to_string_pretty(&data).map_err(|e| e.to_string())?;
    std::fs::write(&path, json).map_err(|e| e.to_string())?;
    Ok(())
}

/// Import capture from JSON file
#[tauri::command]
pub fn la_import_json(
    path: String,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    let json = std::fs::read_to_string(&path).map_err(|e| e.to_string())?;

    #[derive(Deserialize)]
    #[serde(rename_all = "camelCase")]
    struct ImportData {
        channels: u8,
        sample_rate_hz: u32,
        total_samples: u64,
        trigger_sample: Option<u64>,
        transitions: Vec<Vec<(u64, u8)>>,
    }

    let data: ImportData = serde_json::from_str(&json).map_err(|e| e.to_string())?;
    let store = LaStore {
        channels: data.channels,
        sample_rate_hz: data.sample_rate_hz,
        transitions: data.transitions,
        total_samples: data.total_samples,
        trigger_sample: data.trigger_sample,
    };
    let info = LaCaptureInfo {
        channels: store.channels,
        sample_rate_hz: store.sample_rate_hz,
        total_samples: store.total_samples,
        duration_sec: store.total_duration_sec(),
        trigger_sample: store.trigger_sample,
    };
    *la.store.lock().map_err(map_err)? = Some(store);
    Ok(info)
}

/// Read capture data from RP2040 via UART chunks (slower fallback when USB bulk not available)
#[tauri::command]
pub async fn la_read_uart_chunks(
    channels: u8,
    sample_rate_hz: u32,
    total_samples: u32,
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    let samples_per_word = 32u32 / channels as u32;
    let total_bytes = ((total_samples + samples_per_word - 1) / samples_per_word) * 4;
    let chunk_size: u16 = 28;

    let mut all_data: Vec<u8> = Vec::new();
    let mut offset: u32 = 0;

    while offset < total_bytes {
        let req_len = chunk_size.min((total_bytes - offset) as u16);
        let mut pw = bbp::PayloadWriter::new();
        pw.put_u32(offset);
        pw.put_u16(req_len);
        let rsp = mgr.send_command(bbp::CMD_HAT_LA_READ, &pw.buf).await.map_err(map_err)?;
        let mut r = bbp::PayloadReader::new(&rsp);
        let _rsp_offset = r.get_u32().unwrap_or(0);
        let actual_len = r.get_u8().unwrap_or(0) as usize;
        if actual_len == 0 { break; }
        let remaining = &rsp[5..5+actual_len.min(rsp.len()-5)];
        all_data.extend_from_slice(remaining);
        offset += actual_len as u32;
    }

    // Build store from raw data
    let store = LaStore::from_raw(&all_data, channels, sample_rate_hz);
    let info = LaCaptureInfo {
        channels: store.channels,
        sample_rate_hz: store.sample_rate_hz,
        total_samples: store.total_samples,
        duration_sec: store.total_duration_sec(),
        trigger_sample: store.trigger_sample,
    };
    *la.store.lock().map_err(map_err)? = Some(store);
    Ok(info)
}

/// Run a protocol decoder on the capture data
#[tauri::command]
pub fn la_decode(
    config: DecoderConfig,
    start_sample: u64,
    end_sample: u64,
    la: State<'_, LaState>,
) -> CmdResult<Vec<Annotation>> {
    let store = la.store.lock().map_err(map_err)?;
    match store.as_ref() {
        Some(s) => Ok(la_decoders::decode(&config, s, start_sample, end_sample)),
        None => Err("No capture data".into()),
    }
}
