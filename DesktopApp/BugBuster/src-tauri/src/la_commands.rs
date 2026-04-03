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
use std::sync::atomic::{AtomicBool, Ordering};
use crate::la_usb::{LaUsbConnection, LaCaptureData, decode_capture, hat_usb_present};

/// Find the RP2040 BugBuster HAT CDC serial port.
/// Looks for a serial port associated with VID=0x2E8A, PID=0x000C.
fn find_rp2040_cdc_port() -> Option<String> {
    let ports = serialport::available_ports().ok()?;
    for port in &ports {
        if let serialport::SerialPortType::UsbPort(usb_info) = &port.port_type {
            if usb_info.vid == 0x2E8A && usb_info.pid == 0x000C {
                log::info!("[find_rp2040_cdc] Found: {} (VID={:04X} PID={:04X})",
                    port.port_name, usb_info.vid, usb_info.pid);
                // The CDC data interface (interface 2) is the one we want
                // On macOS this shows up as a cu.usbmodem port
                return Some(port.port_name.clone());
            }
        }
    }
    // Fallback: look for known port patterns
    for port in &ports {
        if port.port_name.contains("usbmodem1302") {
            return Some(port.port_name.clone());
        }
    }
    None
}
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
    pub store: Arc<Mutex<Option<LaStore>>>,
    /// Flag to signal the background USB streaming task to stop
    pub stream_running: Arc<AtomicBool>,
}

impl LaState {
    pub fn new() -> Self {
        Self {
            usb: Arc::new(Mutex::new(LaUsbConnection::new())),
            last_capture: Mutex::new(None),
            store: Arc::new(Mutex::new(None)),
            stream_running: Arc::new(AtomicBool::new(false)),
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
    rle_enabled: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_u8(channels);
    pw.put_u32(rate_hz);
    pw.put_u32(depth);
    pw.put_u8(rle_enabled as u8);
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
    let chunk_size: u16 = 900; // Max ~1024 payload, leave room for framing

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

/// Read capture data and append to existing store (for stream mode)
#[tauri::command]
pub async fn la_read_append(
    channels: u8,
    sample_rate_hz: u32,
    total_samples: u32,
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    log::info!("[la_read_append] ch={} rate={} depth={}", channels, sample_rate_hz, total_samples);
    let samples_per_word = 32u32 / channels as u32;
    let total_bytes = ((total_samples + samples_per_word - 1) / samples_per_word) * 4;
    let chunk_size: u16 = 900; // Max ~1024 payload, leave room for framing

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

    log::info!("[la_read_append] Read {} bytes from device", all_data.len());

    let mut store_guard = la.store.lock().map_err(map_err)?;
    if let Some(ref mut store) = *store_guard {
        store.append_raw(&all_data);
    } else {
        *store_guard = Some(LaStore::from_raw(&all_data, channels, sample_rate_hz));
    }

    let store = store_guard.as_ref().unwrap();
    log::info!("[la_read_append] Store now has {} total samples", store.total_samples);
    Ok(LaCaptureInfo {
        channels: store.channels,
        sample_rate_hz: store.sample_rate_hz,
        total_samples: store.total_samples,
        duration_sec: store.total_duration_sec(),
        trigger_sample: store.trigger_sample,
    })
}

/// Full stream cycle: arm → poll → USB_SEND → USB bulk read (all in one command)
/// Starts USB read in background BEFORE sending USB_SEND to avoid deadlock.
#[tauri::command]
pub async fn la_stream_cycle(
    channels: u8,
    sample_rate_hz: u32,
    depth: u32,
    rle_enabled: bool,
    trigger_type: u8,
    trigger_channel: u8,
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    let t_start = std::time::Instant::now();
    log::info!("[SC] START ch={} rate={} depth={}", channels, sample_rate_hz, depth);

    // Check/connect USB
    let use_usb = if hat_usb_present() {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            log::info!("[SC] +{:?} USB connecting...", t_start.elapsed());
            usb.connect().is_ok()
        } else { true }
    } else { false };
    log::info!("[SC] +{:?} use_usb={}", t_start.elapsed(), use_usb);

    // Start USB bulk read in background BEFORE arming
    let usb_read = if use_usb {
        let usb_mutex = la.usb.clone();
        let t = t_start;
        Some(tokio::task::spawn_blocking(move || -> Result<Vec<u8>, String> {
            log::info!("[SC] +{:?} bulk_in submit", t.elapsed());
            let mut usb = usb_mutex.lock().map_err(|e| e.to_string())?;
            log::info!("[SC] +{:?} bulk_in locked, calling read_capture_blocking", t.elapsed());
            let result = usb.read_capture_blocking();
            log::info!("[SC] +{:?} bulk_in returned: {}", t.elapsed(),
                match &result { Ok(d) => format!("OK {} bytes", d.len()), Err(e) => format!("ERR: {}", e) });
            result.map_err(|e| e.to_string())
        }))
    } else { None };

    // Stop previous
    log::info!("[SC] +{:?} sending STOP", t_start.elapsed());
    let _ = mgr.send_command(bbp::CMD_HAT_LA_STOP, &[]).await;
    log::info!("[SC] +{:?} STOP done", t_start.elapsed());
    tokio::time::sleep(std::time::Duration::from_millis(5)).await;

    // Configure
    log::info!("[SC] +{:?} sending CONFIG", t_start.elapsed());
    let mut cfg_buf = vec![channels];
    cfg_buf.extend_from_slice(&sample_rate_hz.to_le_bytes());
    cfg_buf.extend_from_slice(&depth.to_le_bytes());
    cfg_buf.push(if rle_enabled { 1 } else { 0 });
    let _ = mgr.send_command(bbp::CMD_HAT_LA_CONFIG, &cfg_buf).await.map_err(map_err)?;
    log::info!("[SC] +{:?} CONFIG done", t_start.elapsed());

    // Trigger + Arm
    let _ = mgr.send_command(bbp::CMD_HAT_LA_TRIGGER, &[trigger_type, trigger_channel]).await.map_err(map_err)?;
    log::info!("[SC] +{:?} sending ARM", t_start.elapsed());
    let _ = mgr.send_command(bbp::CMD_HAT_LA_ARM, &[]).await.map_err(map_err)?;
    log::info!("[SC] +{:?} ARM done", t_start.elapsed());

    // Poll for DONE
    for i in 0..200 {
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
        let rsp = mgr.send_command(bbp::CMD_HAT_LA_STATUS, &[]).await.map_err(map_err)?;
        if !rsp.is_empty() && rsp[0] == 3 {
            log::info!("[SC] +{:?} DONE at poll {}", t_start.elapsed(), i);
            break;
        }
        if i < 3 {
            log::info!("[SC] +{:?} poll {}: state={}", t_start.elapsed(), i, rsp.get(0).unwrap_or(&0));
        }
    }

    let raw = if let Some(read_handle) = usb_read {
        // USB read was started before arm — wait with timeout
        log::info!("[stream_cycle] Waiting for USB data...");
        match tokio::time::timeout(std::time::Duration::from_secs(3), read_handle).await {
            Ok(Ok(Ok(data))) => {
                log::info!("[la_stream_cycle] USB bulk: {} bytes", data.len());
                data
            }
            _ => {
                log::warn!("[la_stream_cycle] USB bulk failed, falling back to UART");
                read_uart_chunks(&mgr, channels, depth).await?
            }
        }
    } else {
        read_uart_chunks(&mgr, channels, depth).await?
    };

    // Append to store
    let mut store_guard = la.store.lock().map_err(map_err)?;
    if let Some(ref mut store) = *store_guard {
        store.append_raw(&raw);
    } else {
        *store_guard = Some(LaStore::from_raw(&raw, channels, sample_rate_hz));
    }
    let store = store_guard.as_ref().unwrap();
    Ok(LaCaptureInfo {
        channels: store.channels,
        sample_rate_hz: store.sample_rate_hz,
        total_samples: store.total_samples,
        duration_sec: store.total_duration_sec(),
        trigger_sample: store.trigger_sample,
    })
}

/// Helper: read capture data via UART chunks
async fn read_uart_chunks(mgr: &ConnectionManager, channels: u8, depth: u32) -> Result<Vec<u8>, String> {
    let samples_per_word = 32u32 / channels as u32;
    let total_bytes = ((depth + samples_per_word - 1) / samples_per_word) * 4;
    let chunk_size: u16 = 900;
    let mut all_data: Vec<u8> = Vec::new();
    let mut offset: u32 = 0;
    while offset < total_bytes {
        let req_len = chunk_size.min((total_bytes - offset) as u16);
        let mut pw = bbp::PayloadWriter::new();
        pw.put_u32(offset);
        pw.put_u16(req_len);
        let rsp = mgr.send_command(bbp::CMD_HAT_LA_READ, &pw.buf).await.map_err(|e| e.to_string())?;
        let mut r = bbp::PayloadReader::new(&rsp);
        let _rsp_offset = r.get_u32().unwrap_or(0);
        let actual_len = r.get_u8().unwrap_or(0) as usize;
        if actual_len == 0 { break; }
        let remaining = &rsp[5..5+actual_len.min(rsp.len()-5)];
        all_data.extend_from_slice(remaining);
        offset += actual_len as u32;
    }
    Ok(all_data)
}

/// Start gapless USB streaming from RP2040.
/// 1. Connects to USB if needed
/// 2. Sends configure + arm via BBP (for sample rate / channels setup)
/// 3. Sends stream start command directly via USB OUT endpoint
/// 4. Spawns background task that continuously reads USB IN and appends to LaStore
/// Returns immediately — streaming happens in background.
#[tauri::command]
pub async fn la_stream_usb(
    channels: u8,
    sample_rate_hz: u32,
    depth: u32,
    rle_enabled: bool,
    trigger_type: u8,
    trigger_channel: u8,
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<()> {
    log::info!("[la_stream_usb] Starting gapless CDC stream: ch={} rate={} depth={}", channels, sample_rate_hz, depth);

    // Stop any previous streaming
    la.stream_running.store(false, Ordering::SeqCst);
    tokio::time::sleep(std::time::Duration::from_millis(50)).await;

    // Disconnect USB vendor interface if held (it blocks the CDC serial port on macOS)
    {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if usb.is_connected() {
            usb.disconnect();
            log::info!("[la_stream_usb] Released USB vendor interface");
        }
    }
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;

    // Stop previous capture on RP2040
    let _ = mgr.send_command(bbp::CMD_HAT_LA_STOP, &[]).await;
    tokio::time::sleep(std::time::Duration::from_millis(10)).await;

    // Configure via BBP (ESP32 → UART → RP2040)
    let mut cfg_buf = vec![channels];
    cfg_buf.extend_from_slice(&sample_rate_hz.to_le_bytes());
    cfg_buf.extend_from_slice(&depth.to_le_bytes());
    cfg_buf.push(if rle_enabled { 1 } else { 0 });
    mgr.send_command(bbp::CMD_HAT_LA_CONFIG, &cfg_buf).await.map_err(map_err)?;
    mgr.send_command(bbp::CMD_HAT_LA_TRIGGER, &[trigger_type, trigger_channel]).await.map_err(map_err)?;
    // Wait for RP2040 to finish processing configure (goes through ESP32 → UART)
    tokio::time::sleep(std::time::Duration::from_millis(100)).await;

    // Initialize store
    {
        let mut store_guard = la.store.lock().map_err(map_err)?;
        *store_guard = Some(LaStore::from_raw(&[], channels, sample_rate_hz));
    }

    // Find RP2040 CDC serial port for direct streaming
    let cdc_port = find_rp2040_cdc_port()
        .ok_or_else(|| "RP2040 CDC serial port not found".to_string())?;
    log::info!("[la_stream_usb] Found RP2040 CDC: {}", cdc_port);

    la.stream_running.store(true, Ordering::SeqCst);
    let store_mutex = la.store.clone();
    let running = la.stream_running.clone();

    tokio::task::spawn_blocking(move || {
        // Open CDC serial port
        let mut port = match serialport::new(&cdc_port, 115200)
            .timeout(std::time::Duration::from_secs(2))
            .open() {
            Ok(p) => p,
            Err(e) => {
                log::error!("[la_stream_usb] Cannot open {}: {}", cdc_port, e);
                running.store(false, Ordering::SeqCst);
                return;
            }
        };
        log::info!("[la_stream_usb] Opened CDC port");

        // Drain any stale data from previous session
        {
            let mut drain = [0u8; 4096];
            loop {
                match port.read(&mut drain) {
                    Ok(n) if n > 0 => {
                        log::info!("[la_stream_usb] Drained {} stale bytes", n);
                        continue;
                    }
                    _ => break,
                }
            }
        }

        // Send start with retry
        let mut started = false;
        for attempt in 0..3 {
            port.write_all(&[0x01]).ok();
            port.flush().ok();
            let mut resp = [0u8; 64];
            match port.read(&mut resp) {
                Ok(n) if n > 0 => {
                    let s = String::from_utf8_lossy(&resp[..n]);
                    log::info!("[la_stream_usb] Attempt {}: {:?}", attempt, s.trim());
                    if s.contains("START") { started = true; break; }
                    // Got ERR — stop and retry
                    port.write_all(&[0x00]).ok();
                    port.flush().ok();
                    std::thread::sleep(std::time::Duration::from_millis(300));
                }
                _ => {
                    log::warn!("[la_stream_usb] Attempt {}: no response", attempt);
                    std::thread::sleep(std::time::Duration::from_millis(300));
                }
            }
        }
        if !started {
            log::error!("[la_stream_usb] Failed to start stream");
            running.store(false, Ordering::SeqCst);
            return;
        }

        // Read loop
        log::info!("[la_stream_usb] Streaming active");
        let mut total_bytes: u64 = 0;
        let mut chunk_count: u64 = 0;
        let t0 = std::time::Instant::now();
        let mut buf = vec![0u8; 8192];

        // Send stream start command (0x01) via CDC
        while running.load(Ordering::SeqCst) {
            let n = match port.read(&mut buf) {
                Ok(0) => { std::thread::sleep(std::time::Duration::from_millis(1)); continue; }
                Ok(n) => n,
                Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => continue,
                Err(e) => { log::warn!("[la_stream_usb] Read error: {}", e); break; }
            };
            chunk_count += 1;
            total_bytes += n as u64;
            if chunk_count <= 3 || chunk_count % 200 == 0 {
                log::info!("[la_stream_usb] Chunk {}: {}B (total: {}, {:.0} KB/s)",
                    chunk_count, n, total_bytes,
                    total_bytes as f64 / t0.elapsed().as_secs_f64().max(0.001) / 1024.0);
            }
            if let Ok(mut sg) = store_mutex.lock() {
                if let Some(ref mut s) = *sg { s.append_raw(&buf[..n]); }
            }
        }

        // Stop
        port.write_all(&[0x00]).ok();
        port.flush().ok();
        log::info!("[la_stream_usb] Stopped: {} chunks, {} bytes, {:.0} KB/s",
            chunk_count, total_bytes,
            total_bytes as f64 / t0.elapsed().as_secs_f64().max(0.001) / 1024.0);
    });

    Ok(())
}

/// Stop gapless USB streaming.
/// Sends stop command via USB OUT and signals the background reader to stop.
#[tauri::command]
pub async fn la_stream_usb_stop(
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    log::info!("[la_stream_usb_stop] Stopping gapless USB stream");

    // Signal background reader to stop
    la.stream_running.store(false, Ordering::SeqCst);

    // Send stop command via USB OUT
    {
        let usb = la.usb.lock().map_err(map_err)?;
        if usb.is_connected() {
            let _ = usb.send_stream_stop();
        }
    }

    // Small delay for background task to finish
    tokio::time::sleep(std::time::Duration::from_millis(100)).await;

    // Return current store info
    let store_guard = la.store.lock().map_err(map_err)?;
    let store = store_guard.as_ref().ok_or("No capture data")?;
    Ok(LaCaptureInfo {
        channels: store.channels,
        sample_rate_hz: store.sample_rate_hz,
        total_samples: store.total_samples,
        duration_sec: store.total_duration_sec(),
        trigger_sample: store.trigger_sample,
    })
}

/// Check if USB streaming is currently active
#[tauri::command]
pub fn la_stream_usb_active(
    la: State<'_, LaState>,
) -> CmdResult<bool> {
    Ok(la.stream_running.load(Ordering::SeqCst))
}

/// Trigger USB bulk send from RP2040 and read the data (fast path for stream mode)
/// Sends HAT_CMD_LA_USB_SEND command, then reads the bulk USB data
#[tauri::command]
pub async fn la_read_append_fast(
    channels: u8,
    sample_rate_hz: u32,
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    // Check USB is available
    if !hat_usb_present() {
        return Err("USB not available".into());
    }

    // Send USB_SEND command to RP2040 (via ESP32/BBP)
    log::info!("[la_read_append_fast] Sending USB_SEND command");
    let _rsp = mgr.send_command(bbp::CMD_HAT_LA_USB_SEND, &[]).await.map_err(map_err)?;

    // Connect USB if needed
    {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            usb.connect().map_err(map_err)?;
        }
    }

    // Read bulk data from RP2040 USB
    let usb_mutex = la.usb.clone();
    let raw = tokio::task::spawn_blocking(move || -> Result<Vec<u8>, String> {
        let mut usb = usb_mutex.lock().map_err(|e| e.to_string())?;
        usb.read_capture_blocking().map_err(|e| e.to_string())
    }).await.map_err(|e| e.to_string())??;

    log::info!("[la_read_append_fast] Read {} bytes via USB bulk", raw.len());

    let mut store_guard = la.store.lock().map_err(map_err)?;
    if let Some(ref mut store) = *store_guard {
        store.append_raw(&raw);
    } else {
        *store_guard = Some(LaStore::from_raw(&raw, channels, sample_rate_hz));
    }

    let store = store_guard.as_ref().unwrap();
    log::info!("[la_read_append_fast] Store now has {} total samples", store.total_samples);
    Ok(LaCaptureInfo {
        channels: store.channels,
        sample_rate_hz: store.sample_rate_hz,
        total_samples: store.total_samples,
        duration_sec: store.total_duration_sec(),
        trigger_sample: store.trigger_sample,
    })
}

/// Read capture data via USB bulk and append to existing store (fast stream path)
/// RP2040 auto-pushes data on DONE transition — this reads it with a 2s timeout.
#[tauri::command]
pub async fn la_read_append_usb(
    channels: u8,
    sample_rate_hz: u32,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    // Quick check: bail if USB not present or not connected
    {
        if !hat_usb_present() {
            return Err("USB not available".into());
        }
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            usb.connect().map_err(map_err)?;
        }
    }

    let usb_mutex = la.usb.clone();
    let read_future = tokio::task::spawn_blocking(move || -> Result<Vec<u8>, String> {
        let mut usb = usb_mutex.lock().map_err(|e| e.to_string())?;
        usb.read_capture_blocking().map_err(|e| e.to_string())
    });

    // 2-second timeout so we don't hang forever
    let raw = match tokio::time::timeout(std::time::Duration::from_secs(2), read_future).await {
        Ok(Ok(Ok(data))) => data,
        Ok(Ok(Err(e))) => return Err(format!("USB read error: {}", e)),
        Ok(Err(e)) => return Err(format!("USB task error: {}", e)),
        Err(_) => return Err("USB read timeout (2s)".into()),
    };

    log::info!("[la_read_append_usb] Read {} bytes via USB bulk", raw.len());

    let mut store_guard = la.store.lock().map_err(map_err)?;
    if let Some(ref mut store) = *store_guard {
        store.append_raw(&raw);
    } else {
        *store_guard = Some(LaStore::from_raw(&raw, channels, sample_rate_hz));
    }

    let store = store_guard.as_ref().unwrap();
    Ok(LaCaptureInfo {
        channels: store.channels,
        sample_rate_hz: store.sample_rate_hz,
        total_samples: store.total_samples,
        duration_sec: store.total_duration_sec(),
        trigger_sample: store.trigger_sample,
    })
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

/// Delete a range of samples from the capture store
#[tauri::command]
pub fn la_delete_range(
    start_sample: u64,
    end_sample: u64,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    let mut store = la.store.lock().map_err(map_err)?;
    let s = store.as_mut().ok_or("No capture data")?;
    s.delete_range(start_sample, end_sample);
    Ok(LaCaptureInfo {
        channels: s.channels,
        sample_rate_hz: s.sample_rate_hz,
        total_samples: s.total_samples,
        duration_sec: s.total_duration_sec(),
        trigger_sample: s.trigger_sample,
    })
}
