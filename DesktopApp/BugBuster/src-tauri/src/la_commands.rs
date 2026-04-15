// =============================================================================
// la_commands.rs — Tauri commands for Logic Analyzer
//
// Provides commands to:
//   - Connect to RP2040 LA USB interface
//   - Read captured data
//   - Decode channels
//   - Manage LA state
// =============================================================================

use crate::bbp;
use crate::connection_manager::ConnectionManager;
use crate::la_decoders::{self, Annotation, DecoderConfig};
use crate::la_store::{LaStore, LaViewData};
use crate::la_transport::{LaTransport, LockedLaTransport, StreamStopReason};
use crate::la_usb::{
    decode_capture, hat_usb_present, LaCaptureData, LaStreamPacket, LaStreamPacketKind,
    LaUsbConnection, LA_USB_CMD_START_STREAM, LA_USB_CMD_STOP, STREAM_INFO_START_REJECTED,
};
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use tauri::State;

type CmdResult<T> = Result<T, String>;
fn map_err(e: impl std::fmt::Display) -> String {
    e.to_string()
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

fn stream_stop_reason_name(reason: u8) -> &'static str {
    match reason {
        0 => "none",
        1 => "host_stop",
        2 => "usb_short_write",
        3 => "dma_overrun",
        STREAM_INFO_START_REJECTED => "start_rejected",
        _ => "unknown",
    }
}

#[derive(Debug, PartialEq, Eq)]
enum StreamPacketOutcome {
    Continue,
    Stop,
}

fn apply_stream_packet<F>(
    packet: &LaStreamPacket,
    expected_seq: &mut u8,
    status: &mut LaStreamRuntimeStatus,
    mut on_payload: F,
) -> Result<StreamPacketOutcome, String>
where
    F: FnMut(&[u8]),
{
    match packet.kind {
        LaStreamPacketKind::Start => {
            status.active = true;
            status.stop_reason = None;
            status.last_error = None;
            Ok(StreamPacketOutcome::Continue)
        }
        LaStreamPacketKind::Data => {
            if packet.seq != *expected_seq {
                let dropped = if packet.seq > *expected_seq {
                    packet.seq - *expected_seq
                } else {
                    (255 - *expected_seq) + packet.seq + 1
                };
                status.active = false;
                status.sequence_mismatches += 1;
                status.stop_reason = Some("sequence_mismatch".to_string());
                status.last_error = Some(format!(
                    "sequence mismatch: got {}, expected {} (~{} packets dropped)",
                    packet.seq, *expected_seq, dropped
                ));
                return Err(status.last_error.clone().unwrap());
            }

            *expected_seq = expected_seq.wrapping_add(1);
            status.chunk_count += 1;
            status.total_bytes += packet.payload.len() as u64;
            on_payload(&packet.payload);
            Ok(StreamPacketOutcome::Continue)
        }
        LaStreamPacketKind::Stop => {
            status.active = false;
            status.stop_reason = Some(stream_stop_reason_name(packet.info).to_string());
            Ok(StreamPacketOutcome::Stop)
        }
        LaStreamPacketKind::Error => {
            status.active = false;
            status.invalid_frames += 1;
            status.stop_reason = Some(stream_stop_reason_name(packet.info).to_string());
            status.last_error = Some(format!(
                "firmware reported stream error: {}",
                stream_stop_reason_name(packet.info)
            ));
            Err(status.last_error.clone().unwrap())
        }
    }
}

/// Flush stale packets from the vendor-bulk IN FIFO before starting a new stream.
///
/// Sends `LA_USB_CMD_STOP (0x00)` twice and drains until `PKT_STOP` each time.
/// Two passes are kept as a conservative safety net: the firmware's
/// `HAT_CMD_LA_STOP` now calls `bb_la_usb_abort_bulk()` unconditionally, so the
/// second pass is rarely needed, but it costs little and guards against any residue
/// that slips through during the abort→stop race on the firmware side.
///
/// **Must be called BEFORE `CMD_HAT_LA_CONFIG`** so that the `bb_la_stop()` side-
/// effect (which unloads the PIO program) happens before configure reloads it.
/// If called AFTER configure, the PIO would be unloaded and `bb_la_start_stream()`
/// would reject the subsequent start with `START_REJECTED`.
pub(crate) fn pre_stream_drain(transport: &mut impl LaTransport) {
    const MAX_DRAIN: usize = 8192;
    for pass in 0u8..2 {
        if let Err(e) = transport.send_command(LA_USB_CMD_STOP) {
            log::warn!("[pre_stream_drain] cannot send LA_USB_CMD_STOP (pass {pass}): {e}");
            return;
        }
        let mut count = 0usize;
        loop {
            match transport.read_packet() {
                Ok(p) if p.kind == LaStreamPacketKind::Stop => break,
                Ok(p) => {
                    count += 1;
                    log::debug!(
                        "[pre_stream_drain] pass {pass}: discarding stale {:?} ({})",
                        p.kind, count
                    );
                    if count >= MAX_DRAIN {
                        log::warn!(
                            "[pre_stream_drain] pass {pass}: MAX_DRAIN reached — aborting"
                        );
                        break;
                    }
                }
                Err(e) => {
                    log::warn!("[pre_stream_drain] pass {pass}: USB error: {e}");
                    return;
                }
            }
        }
        if count > 0 {
            log::warn!("[pre_stream_drain] pass {pass}: discarded {count} stale packets");
        }
    }
}

/// Core streaming loop — runs synchronously in a `spawn_blocking` context.
///
/// Assumes the vendor-bulk FIFO has already been drained by `pre_stream_drain()`
/// and the firmware PIO has been reloaded by `CMD_HAT_LA_CONFIG`. Sends
/// `LA_USB_CMD_START_STREAM`, then reads packets until the device sends STOP, a USB
/// error occurs, or `running` is cleared by the host. On every exit path the
/// function guarantees `running = false` and `status.active = false`.
pub(crate) fn run_stream_loop(
    transport: &mut impl LaTransport,
    running: &std::sync::atomic::AtomicBool,
    store: &std::sync::Mutex<Option<crate::la_store::LaStore>>,
    status: &std::sync::Mutex<LaStreamRuntimeStatus>,
    stream_seq: &std::sync::atomic::AtomicU8,
) -> StreamStopReason {
    // Teardown helper — must run on every return path.
    let teardown = |running: &std::sync::atomic::AtomicBool,
                    status: &std::sync::Mutex<LaStreamRuntimeStatus>| {
        running.store(false, Ordering::SeqCst);
        if let Ok(mut s) = status.lock() {
            s.active = false;
        }
    };

    // FIFO has been pre-drained by pre_stream_drain() in la_stream_usb() before
    // spawning this task, and CMD_HAT_LA_CONFIG has reloaded the PIO. Send START.
    if let Err(e) = transport.send_command(LA_USB_CMD_START_STREAM) {
        teardown(running, status);
        return StreamStopReason::UsbError(format!("cannot send stream start: {e}"));
    }

    // Read the immediate firmware response — must be PKT_START or PKT_ERROR.
    //
    // pre_stream_drain() sends STOP twice to flush the vendor-bulk FIFO.
    // MAX_SKIP is a generous safety net for any residual DATA/STOP packets
    // that arrive before PKT_START in pathological timing windows.
    let first = match transport.read_packet() {
        Ok(p) => p,
        Err(e) => {
            if let Ok(mut s) = status.lock() {
                s.stop_reason = Some("usb_error".to_string());
                s.last_error = Some(format!("USB error waiting for stream start: {e}"));
            }
            teardown(running, status);
            return StreamStopReason::UsbError(format!("waiting for stream start: {e}"));
        }
    };
    const MAX_SKIP: usize = 512;
    let mut start_packet = first;
    let mut skipped = 0usize;
    while skipped < MAX_SKIP
        && start_packet.kind != LaStreamPacketKind::Start
        && start_packet.kind != LaStreamPacketKind::Error
    {
        log::debug!(
            "[run_stream_loop] skipping stale {:?} before PKT_START (count={})",
            start_packet.kind,
            skipped
        );
        skipped += 1;
        start_packet = match transport.read_packet() {
            Ok(p) => p,
            Err(e) => {
                if let Ok(mut s) = status.lock() {
                    s.stop_reason = Some("usb_error".to_string());
                    s.last_error = Some(format!("USB error waiting for stream start: {e}"));
                }
                teardown(running, status);
                return StreamStopReason::UsbError(format!("waiting for stream start: {e}"));
            }
        };
    }
    if skipped > 0 {
        log::warn!("[run_stream_loop] skipped {skipped} stale packets before PKT_START");
    }
    match start_packet.kind {
        LaStreamPacketKind::Start => {
            if let Ok(mut s) = status.lock() {
                s.active = true;
            }
        }
        LaStreamPacketKind::Error if start_packet.info == STREAM_INFO_START_REJECTED => {
            if let Ok(mut s) = status.lock() {
                s.stop_reason = Some("start_rejected".to_string());
                s.last_error = Some(format!(
                    "Firmware rejected stream start (info={:#04x})",
                    start_packet.info
                ));
            }
            teardown(running, status);
            return StreamStopReason::FirmwareError(start_packet.info);
        }
        _ => {
            if let Ok(mut s) = status.lock() {
                s.stop_reason = Some("protocol_error".to_string());
                s.last_error = Some(format!(
                    "Expected PKT_START but got {:?} after stream start",
                    start_packet.kind
                ));
            }
            teardown(running, status);
            return StreamStopReason::UsbError(format!(
                "Expected PKT_START but got {:?}",
                start_packet.kind
            ));
        }
    }

    log::info!("[run_stream_loop] Streaming active");
    let t0 = std::time::Instant::now();
    let mut expected_seq = 0u8;
    stream_seq.store(expected_seq, Ordering::SeqCst);

    while running.load(Ordering::SeqCst) {
        let packet = match transport.read_packet() {
            Ok(p) => p,
            Err(e) => {
                teardown(running, status);
                return StreamStopReason::UsbError(e.to_string());
            }
        };

        let mut status_guard = match status.lock() {
            Ok(g) => g,
            Err(e) => {
                log::error!("[run_stream_loop] cannot lock stream status: {e}");
                teardown(running, status);
                return StreamStopReason::UsbError(format!("status mutex poisoned: {e}"));
            }
        };

        match apply_stream_packet(&packet, &mut expected_seq, &mut status_guard, |payload| {
            if let Ok(mut sg) = store.lock() {
                if let Some(ref mut s) = *sg {
                    s.append_raw(payload);
                }
            }
        }) {
            Ok(StreamPacketOutcome::Continue) => {
                stream_seq.store(expected_seq, Ordering::SeqCst);
                if status_guard.chunk_count > 0 && status_guard.chunk_count % 1000 == 0 {
                    log::info!(
                        "[run_stream_loop] {} packets, {}B ({:.0} KB/s)",
                        status_guard.chunk_count,
                        status_guard.total_bytes,
                        status_guard.total_bytes as f64
                            / t0.elapsed().as_secs_f64().max(0.001)
                            / 1024.0
                    );
                }
                drop(status_guard);
            }
            Ok(StreamPacketOutcome::Stop) => {
                drop(status_guard);
                log::info!(
                    "[run_stream_loop] Stopped after {:.2}s",
                    t0.elapsed().as_secs_f64()
                );
                teardown(running, status);
                return StreamStopReason::Normal;
            }
            Err(err) => {
                // Sequence mismatch or firmware error — status fields already set by apply_stream_packet.
                let reason = if err.contains("sequence mismatch") {
                    // Parse expected/got from the error message isn't reliable; use the stored expected.
                    let got = packet.seq;
                    let expected = expected_seq;
                    StreamStopReason::SeqMismatch { expected, got }
                } else {
                    StreamStopReason::UsbError(err)
                };
                drop(status_guard);
                log::error!("[run_stream_loop] {reason:?}");
                teardown(running, status);
                return reason;
            }
        }
    }

    // running was cleared externally (host stop).
    log::info!(
        "[run_stream_loop] Host-stopped after {:.2}s",
        t0.elapsed().as_secs_f64()
    );
    teardown(running, status);
    StreamStopReason::HostStopped
}

/// Shared LA USB connection state
pub struct LaState {
    pub usb: Arc<Mutex<LaUsbConnection>>,
    pub last_capture: Mutex<Option<LaCaptureData>>,
    pub store: Arc<Mutex<Option<LaStore>>>,
    /// Flag to signal the background USB streaming task to stop
    pub stream_running: Arc<AtomicBool>,
    /// Join handle for the background streaming task, so we can wait for it to finish
    pub stream_task: Arc<Mutex<Option<tokio::task::JoinHandle<()>>>>,
    /// Next expected data-packet sequence number for the vendor-bulk live stream
    pub stream_seq: Arc<AtomicU8>,
    /// Runtime health and counters for the current/last bulk live stream
    pub stream_status: Arc<Mutex<LaStreamRuntimeStatus>>,
}

impl LaState {
    pub fn new() -> Self {
        Self {
            usb: Arc::new(Mutex::new(LaUsbConnection::new())),
            last_capture: Mutex::new(None),
            store: Arc::new(Mutex::new(None)),
            stream_running: Arc::new(AtomicBool::new(false)),
            stream_task: Arc::new(Mutex::new(None)),
            stream_seq: Arc::new(AtomicU8::new(0)),
            stream_status: Arc::new(Mutex::new(LaStreamRuntimeStatus::default())),
        }
    }
}

#[derive(Serialize, Deserialize)]
pub struct LaStatusResponse {
    pub usb_present: bool,
    pub usb_connected: bool,
    pub state: u8, // 0=idle, 1=armed, 2=capturing, 3=done, 4=error
    pub state_name: String,
    pub channels: u8,
    pub samples_captured: u32,
    pub total_samples: u32,
    pub actual_rate_hz: u32,
    pub has_capture: bool,
    pub stream_stop_reason: Option<String>,
    pub stream_overrun_count: Option<u32>,
    pub stream_short_write_count: Option<u32>,
}

/// Check if RP2040 LA is available on USB
#[tauri::command]
pub fn la_check_usb() -> CmdResult<bool> {
    Ok(hat_usb_present())
}

/// Connect to the RP2040 LA USB interface
#[tauri::command]
pub fn la_connect_usb(la: State<'_, LaState>, mgr: State<'_, ConnectionManager>) -> CmdResult<bool> {
    let mut usb = la.usb.lock().map_err(map_err)?;
    let status = mgr.get_connection_status();
    usb.connect(status.la_selector).map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_HAT_LA_CONFIG, &pw.buf)
        .await
        .map_err(map_err)?;
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
    mgr.send_command(bbp::CMD_HAT_LA_TRIGGER, &pw.buf)
        .await
        .map_err(map_err)?;
    Ok(())
}

/// Arm the trigger
#[tauri::command]
pub async fn la_arm(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_LA_ARM, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

/// Force trigger
#[tauri::command]
pub async fn la_force(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_LA_FORCE, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

/// Stop capture
#[tauri::command]
pub async fn la_stop(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_HAT_LA_STOP, &[])
        .await
        .map_err(map_err)?;
    Ok(())
}

/// Get LA capture status
#[tauri::command]
pub async fn la_get_status(
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaStatusResponse> {
    let state_names = ["idle", "armed", "capturing", "done", "streaming", "error"];

    let rsp = mgr
        .send_command(bbp::CMD_HAT_LA_STATUS, &[])
        .await
        .map_err(map_err)?;
    let mut r = bbp::PayloadReader::new(&rsp);
    let state = r.get_u8().unwrap_or(0);
    let channels = r.get_u8().unwrap_or(0);
    let samples_captured = r.get_u32().unwrap_or(0);
    let total_samples = r.get_u32().unwrap_or(0);
    let actual_rate_hz = r.get_u32().unwrap_or(0);
    let _usb_connected = r.get_u8();
    let _usb_mounted = r.get_u8();
    let stream_stop_reason = r.get_u8().map(stream_stop_reason_name).map(str::to_string);
    let stream_overrun_count = r.get_u32();
    let stream_short_write_count = r.get_u32();

    let usb = la.usb.lock().map_err(map_err)?;
    let has_capture = la.last_capture.lock().map_err(map_err)?.is_some();

    Ok(LaStatusResponse {
        usb_present: hat_usb_present(),
        usb_connected: usb.is_connected(),
        state,
        state_name: state_names
            .get(state as usize)
            .unwrap_or(&"unknown")
            .to_string(),
        channels,
        samples_captured,
        total_samples,
        actual_rate_hz,
        has_capture,
        stream_stop_reason,
        stream_overrun_count,
        stream_short_write_count,
    })
}

/// Read captured data from RP2040 via USB bulk endpoint
#[tauri::command]
pub async fn la_read_capture(
    channels: u8,
    sample_rate_hz: u32,
    total_samples: u32,
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureData> {
    // Connect if needed (sync, no await)
    {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            let status = mgr.get_connection_status();
            usb.connect(status.la_selector).map_err(map_err)?;
        }
    }

    // Read data in a blocking task (nusb bulk reads are sync via block_on)
    let usb_mutex = la.usb.clone();
    let raw = tokio::task::spawn_blocking(move || -> Result<Vec<u8>, String> {
        let mut usb = usb_mutex.lock().map_err(|e| e.to_string())?;
        usb.read_capture_blocking().map_err(|e| e.to_string())
    })
    .await
    .map_err(|e| e.to_string())??;

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
    max_points: Option<usize>,
    la: State<'_, LaState>,
) -> CmdResult<Option<LaViewData>> {
    let mut store = la.store.lock().map_err(map_err)?;
    match store.as_mut() {
        Some(s) => {
            #[cfg(debug_assertions)]
            let t0 = std::time::Instant::now();
            let result = s.to_view_data(start_sample, end_sample, max_points);
            #[cfg(debug_assertions)]
            log::info!(
                "[la_get_view] to_view_data took {}µs",
                t0.elapsed().as_micros()
            );
            Ok(Some(result))
        }
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
pub fn la_export_vcd_file(path: String, la: State<'_, LaState>) -> CmdResult<()> {
    let store = la.store.lock().map_err(map_err)?;
    let s = store.as_ref().ok_or("No capture data")?;
    let vcd = s.export_vcd();
    std::fs::write(&path, vcd).map_err(|e| e.to_string())?;
    Ok(())
}

/// Export capture as JSON file (full capture data + metadata for reload)
#[tauri::command]
pub fn la_export_json(path: String, la: State<'_, LaState>) -> CmdResult<()> {
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
pub fn la_import_json(path: String, la: State<'_, LaState>) -> CmdResult<LaCaptureInfo> {
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
    let store = LaStore::from_transitions(
        data.channels,
        data.sample_rate_hz,
        data.transitions,
        data.total_samples,
        data.trigger_sample,
    );
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
        let rsp = mgr
            .send_command(bbp::CMD_HAT_LA_READ, &pw.buf)
            .await
            .map_err(map_err)?;
        let mut r = bbp::PayloadReader::new(&rsp);
        let _rsp_offset = r.get_u32().unwrap_or(0);
        let actual_len = r.get_u8().unwrap_or(0) as usize;
        if actual_len == 0 {
            break;
        }
        let remaining = &rsp[5..5 + actual_len.min(rsp.len() - 5)];
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
    log::info!(
        "[la_read_append] ch={} rate={} depth={}",
        channels,
        sample_rate_hz,
        total_samples
    );
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
        let rsp = mgr
            .send_command(bbp::CMD_HAT_LA_READ, &pw.buf)
            .await
            .map_err(map_err)?;
        let mut r = bbp::PayloadReader::new(&rsp);
        let _rsp_offset = r.get_u32().unwrap_or(0);
        let actual_len = r.get_u8().unwrap_or(0) as usize;
        if actual_len == 0 {
            break;
        }
        let remaining = &rsp[5..5 + actual_len.min(rsp.len() - 5)];
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
    log::info!(
        "[la_read_append] Store now has {} total samples",
        store.total_samples
    );
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
    log::info!(
        "[SC] START ch={} rate={} depth={}",
        channels,
        sample_rate_hz,
        depth
    );

    // Check/connect USB
    let use_usb = if hat_usb_present() {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            log::info!("[SC] +{:?} USB connecting...", t_start.elapsed());
            let status = mgr.get_connection_status();
            usb.connect(status.la_selector).is_ok()
        } else {
            true
        }
    } else {
        false
    };
    log::info!("[SC] +{:?} use_usb={}", t_start.elapsed(), use_usb);

    // Start USB bulk read in background BEFORE arming
    let usb_read = if use_usb {
        let usb_mutex = la.usb.clone();
        let t = t_start;
        Some(tokio::task::spawn_blocking(
            move || -> Result<Vec<u8>, String> {
                log::info!("[SC] +{:?} bulk_in submit", t.elapsed());
                let mut usb = usb_mutex.lock().map_err(|e| e.to_string())?;
                log::info!(
                    "[SC] +{:?} bulk_in locked, calling read_capture_blocking",
                    t.elapsed()
                );
                let result = usb.read_capture_blocking();
                log::info!(
                    "[SC] +{:?} bulk_in returned: {}",
                    t.elapsed(),
                    match &result {
                        Ok(d) => format!("OK {} bytes", d.len()),
                        Err(e) => format!("ERR: {}", e),
                    }
                );
                result.map_err(|e| e.to_string())
            },
        ))
    } else {
        None
    };

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
    let _ = mgr
        .send_command(bbp::CMD_HAT_LA_CONFIG, &cfg_buf)
        .await
        .map_err(map_err)?;
    log::info!("[SC] +{:?} CONFIG done", t_start.elapsed());

    // Trigger + Arm
    let _ = mgr
        .send_command(bbp::CMD_HAT_LA_TRIGGER, &[trigger_type, trigger_channel])
        .await
        .map_err(map_err)?;
    log::info!("[SC] +{:?} sending ARM", t_start.elapsed());
    let _ = mgr
        .send_command(bbp::CMD_HAT_LA_ARM, &[])
        .await
        .map_err(map_err)?;
    log::info!("[SC] +{:?} ARM done", t_start.elapsed());

    // Poll for DONE
    for i in 0..200 {
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
        let rsp = mgr
            .send_command(bbp::CMD_HAT_LA_STATUS, &[])
            .await
            .map_err(map_err)?;
        if !rsp.is_empty() && rsp[0] == 3 {
            log::info!("[SC] +{:?} DONE at poll {}", t_start.elapsed(), i);
            break;
        }
        if i < 3 {
            log::info!(
                "[SC] +{:?} poll {}: state={}",
                t_start.elapsed(),
                i,
                rsp.get(0).unwrap_or(&0)
            );
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
async fn read_uart_chunks(
    mgr: &ConnectionManager,
    channels: u8,
    depth: u32,
) -> Result<Vec<u8>, String> {
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
        let rsp = mgr
            .send_command(bbp::CMD_HAT_LA_READ, &pw.buf)
            .await
            .map_err(|e| e.to_string())?;
        let mut r = bbp::PayloadReader::new(&rsp);
        let _rsp_offset = r.get_u32().unwrap_or(0);
        let actual_len = r.get_u8().unwrap_or(0) as usize;
        if actual_len == 0 {
            break;
        }
        let remaining = &rsp[5..5 + actual_len.min(rsp.len() - 5)];
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
    log::info!(
        "[la_stream_usb] Starting gapless bulk stream: ch={} rate={} depth={}",
        channels,
        sample_rate_hz,
        depth
    );

    la.stream_running.store(false, Ordering::SeqCst);
    // Stop firmware FIRST so it emits PKT_STOP on vendor-bulk IN.
    // The background task's bulk_in() is blocking with the USB mutex held; sending
    // CMD_HAT_LA_STOP causes the RP2040 to send PKT_STOP (see HAT_CMD_LA_STOP
    // handler in bb_main.c), which unblocks bulk_in() and lets the task exit.
    // Without this ordering the task never releases the mutex and the new
    // run_stream_loop deadlocks trying to acquire it.
    let _ = mgr.send_command(bbp::CMD_HAT_LA_STOP, &[]).await;
    {
        let old_task = la.stream_task.lock().map_err(map_err)?.take();
        if let Some(handle) = old_task {
            let _ = tokio::time::timeout(std::time::Duration::from_millis(500), handle).await;
        }
    }

    // Pre-stream drain: flush stale vendor-bulk packets BEFORE configure.
    // LA_USB_CMD_STOP calls bb_la_stop() on the firmware (unloads PIO); configure
    // then reloads it. This ordering is critical — drain must come first.
    {
        let usb_mutex = la.usb.clone();
        tokio::task::spawn_blocking(move || {
            let mut transport = LockedLaTransport { usb: usb_mutex };
            pre_stream_drain(&mut transport);
        })
        .await
        .map_err(|e| format!("pre_stream_drain task panicked: {e}"))?;
    }

    let mut cfg_buf = vec![channels];
    cfg_buf.extend_from_slice(&sample_rate_hz.to_le_bytes());
    cfg_buf.extend_from_slice(&depth.to_le_bytes());
    cfg_buf.push(if rle_enabled { 1 } else { 0 });
    mgr.send_command(bbp::CMD_HAT_LA_CONFIG, &cfg_buf)
        .await
        .map_err(map_err)?;
    mgr.send_command(bbp::CMD_HAT_LA_TRIGGER, &[trigger_type, trigger_channel])
        .await
        .map_err(map_err)?;
    tokio::time::sleep(std::time::Duration::from_millis(100)).await;

    {
        let mut store_guard = la.store.lock().map_err(map_err)?;
        *store_guard = Some(LaStore::from_raw(&[], channels, sample_rate_hz));
    }
    {
        let mut status = la.stream_status.lock().map_err(map_err)?;
        *status = LaStreamRuntimeStatus::default();
    }

    {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            let status = mgr.get_connection_status();
            usb.connect(status.la_selector).map_err(map_err)?;
        }
    }

    la.stream_running.store(true, Ordering::SeqCst);
    let store_mutex = la.store.clone();
    let running = la.stream_running.clone();
    let task_slot = la.stream_task.clone();
    let stream_seq = la.stream_seq.clone();
    let stream_status = la.stream_status.clone();
    let usb_mutex = la.usb.clone();

    let handle = tokio::task::spawn_blocking(move || {
        let mut transport = LockedLaTransport { usb: usb_mutex.clone() };
        let reason = run_stream_loop(&mut transport, &running, &store_mutex, &stream_status, &stream_seq);
        log::info!("[la_stream_usb] stream task exited: {:?}", reason);
    });

    if let Ok(mut slot) = task_slot.lock() {
        *slot = Some(handle);
    }

    Ok(())
}

/// Stop gapless USB streaming.
/// Sends stop via the BBP control plane so the firmware can emit a STOP marker
/// without contending on the vendor-bulk reader mutex.
#[tauri::command]
pub async fn la_stream_usb_stop(
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    log::info!("[la_stream_usb_stop] Stopping gapless USB stream");

    la.stream_running.store(false, Ordering::SeqCst);

    let _ = mgr.send_command(bbp::CMD_HAT_LA_STOP, &[]).await;

    {
        let old_task = la.stream_task.lock().map_err(map_err)?.take();
        if let Some(handle) = old_task {
            // 500 ms is generous — firmware sends PKT_STOP immediately on CMD_HAT_LA_STOP
            // which unblocks the streaming task's bulk_in() call within a few ms.
            let _ = tokio::time::timeout(std::time::Duration::from_millis(500), handle).await;
        }
    }

    {
        let mut status = la.stream_status.lock().map_err(map_err)?;
        if status.stop_reason.is_none() {
            status.stop_reason = Some("host_stop".to_string());
        }
        status.active = false;
    }

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
pub fn la_stream_usb_active(la: State<'_, LaState>) -> CmdResult<bool> {
    Ok(la.stream_running.load(Ordering::SeqCst))
}

/// Return runtime health for the current/last gapless USB stream.
#[tauri::command]
pub fn la_stream_usb_status(la: State<'_, LaState>) -> CmdResult<LaStreamRuntimeStatus> {
    let status = la.stream_status.lock().map_err(map_err)?;
    Ok(status.clone())
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

    // Reset vendor bulk endpoint on RP2040 before reading
    log::info!("[la_read_append_fast] Sending LA_USB_RESET command");
    let _rsp = mgr
        .send_command(bbp::CMD_HAT_LA_USB_RESET, &[])
        .await
        .map_err(map_err)?;

    // Connect USB if needed
    {
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            let status = mgr.get_connection_status();
            usb.connect(status.la_selector).map_err(map_err)?;
        }
    }

    // Read bulk data from RP2040 USB
    let usb_mutex = la.usb.clone();
    let raw = tokio::task::spawn_blocking(move || -> Result<Vec<u8>, String> {
        let mut usb = usb_mutex.lock().map_err(|e| e.to_string())?;
        usb.read_capture_blocking().map_err(|e| e.to_string())
    })
    .await
    .map_err(|e| e.to_string())??;

    log::info!(
        "[la_read_append_fast] Read {} bytes via USB bulk",
        raw.len()
    );

    let mut store_guard = la.store.lock().map_err(map_err)?;
    if let Some(ref mut store) = *store_guard {
        store.append_raw(&raw);
    } else {
        *store_guard = Some(LaStore::from_raw(&raw, channels, sample_rate_hz));
    }

    let store = store_guard.as_ref().unwrap();
    log::info!(
        "[la_read_append_fast] Store now has {} total samples",
        store.total_samples
    );
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
    mgr: State<'_, ConnectionManager>,
    la: State<'_, LaState>,
) -> CmdResult<LaCaptureInfo> {
    // Quick check: bail if USB not present or not connected
    {
        if !hat_usb_present() {
            return Err("USB not available".into());
        }
        let mut usb = la.usb.lock().map_err(map_err)?;
        if !usb.is_connected() {
            let status = mgr.get_connection_status();
            usb.connect(status.la_selector).map_err(map_err)?;
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

#[cfg(test)]
mod tests {
    use super::{
        apply_stream_packet, stream_stop_reason_name, LaStreamRuntimeStatus, StreamPacketOutcome,
    };
    use crate::la_usb::{LaStreamPacket, LaStreamPacketKind, STREAM_INFO_START_REJECTED};

    #[test]
    fn start_packet_marks_stream_active() {
        let packet = LaStreamPacket {
            kind: LaStreamPacketKind::Start,
            seq: 0,
            info: 0,
            payload: Vec::new(),
        };
        let mut status = LaStreamRuntimeStatus::default();
        let mut expected_seq = 0u8;
        let outcome = apply_stream_packet(&packet, &mut expected_seq, &mut status, |_| {}).unwrap();
        assert_eq!(outcome, StreamPacketOutcome::Continue);
        assert!(status.active);
        assert_eq!(expected_seq, 0);
    }

    #[test]
    fn data_packet_updates_counters_and_sequence() {
        let packet = LaStreamPacket {
            kind: LaStreamPacketKind::Data,
            seq: 0,
            info: 0,
            payload: vec![0xAA, 0xBB, 0xCC],
        };
        let mut status = LaStreamRuntimeStatus::default();
        let mut expected_seq = 0u8;
        let mut payloads = Vec::new();

        let outcome = apply_stream_packet(&packet, &mut expected_seq, &mut status, |payload| {
            payloads.push(payload.to_vec());
        })
        .unwrap();

        assert_eq!(outcome, StreamPacketOutcome::Continue);
        assert_eq!(expected_seq, 1);
        assert_eq!(status.chunk_count, 1);
        assert_eq!(status.total_bytes, 3);
        assert_eq!(payloads, vec![vec![0xAA, 0xBB, 0xCC]]);
    }

    #[test]
    fn sequence_gap_is_terminal_and_recorded() {
        let packet = LaStreamPacket {
            kind: LaStreamPacketKind::Data,
            seq: 2,
            info: 0,
            payload: vec![0xAB],
        };
        let mut status = LaStreamRuntimeStatus::default();
        let mut expected_seq = 0u8;

        let err = apply_stream_packet(&packet, &mut expected_seq, &mut status, |_| {}).unwrap_err();
        assert!(err.contains("sequence mismatch"));
        assert_eq!(status.sequence_mismatches, 1);
        assert_eq!(status.stop_reason.as_deref(), Some("sequence_mismatch"));
        assert!(!status.active);
    }

    #[test]
    fn stop_packet_records_stop_reason() {
        let packet = LaStreamPacket {
            kind: LaStreamPacketKind::Stop,
            seq: 0,
            info: 1,
            payload: Vec::new(),
        };
        let mut status = LaStreamRuntimeStatus::default();
        let mut expected_seq = 0u8;

        let outcome = apply_stream_packet(&packet, &mut expected_seq, &mut status, |_| {}).unwrap();
        assert_eq!(outcome, StreamPacketOutcome::Stop);
        assert_eq!(status.stop_reason.as_deref(), Some("host_stop"));
        assert!(!status.active);
    }

    #[test]
    fn error_packet_records_firmware_error() {
        let packet = LaStreamPacket {
            kind: LaStreamPacketKind::Error,
            seq: 0,
            info: STREAM_INFO_START_REJECTED,
            payload: Vec::new(),
        };
        let mut status = LaStreamRuntimeStatus::default();
        let mut expected_seq = 0u8;

        let err = apply_stream_packet(&packet, &mut expected_seq, &mut status, |_| {}).unwrap_err();
        assert!(err.contains("start_rejected"));
        assert_eq!(status.stop_reason.as_deref(), Some("start_rejected"));
        assert_eq!(
            stream_stop_reason_name(STREAM_INFO_START_REJECTED),
            "start_rejected"
        );
    }
}
