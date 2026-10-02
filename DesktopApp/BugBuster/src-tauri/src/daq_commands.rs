// =============================================================================
// daq_commands.rs — Tauri commands for the high-speed DAQ (ESP32-P4) tab.
//
// Two transports are involved:
//   * The P4 USB-HS vendor-bulk stream (daq_usb.rs) for live measurement frames
//     and low-latency control (START/STOP/RANGE_LOCK/FFT_CONFIG/SET_SOURCE).
//   * The S3 BBP control plane (CMD_DAQ_CONFIG = 0xB6) for persistent,
//     schema-backed settings carried as TLV.
//
// A `MockDaqTransport` lets the whole tab run as a "Demo / Mock device" with no
// hardware attached (synthetic I/V/P + DSP + FFT + autorange behaviour).
// =============================================================================

use crate::bbp;
use crate::connection_manager::ConnectionManager;
use crate::daq_proto::{self, DaqRecord, EnergyRecord, FftRecord, StatsRecord, StatusRecord};
use crate::daq_store::{DaqIntegral, DaqStore};
use crate::daq_usb::{daq_usb_present, DaqTransport, DaqUsbConnection, MockDaqTransport};
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, RecvTimeoutError, SyncSender};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, Instant};
use tauri::State;

type CmdResult<T> = Result<T, String>;
fn map_err(e: impl std::fmt::Display) -> String {
    e.to_string()
}

/// Sample-rate enum index → samples/second (mirror DaqKey.SAMPLE_RATE_IDX).
pub const SAMPLE_RATES: [u32; 5] = [10_000, 50_000, 100_000, 250_000, 1_000_000];

fn rate_from_idx(idx: u8) -> u32 {
    *SAMPLE_RATES.get(idx as usize).unwrap_or(&250_000)
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
    /// Measured effective sample rate, accounting for gaps/drops (Hz).
    pub actual_rate_hz: f64,
    /// Total samples lost to detected gaps in the stream.
    pub dropped_samples: u64,
    /// USB stream rate for the voltage ADC channel.
    pub voltage_sps_hz: u32,
    pub overflow: bool,
    /// Measured ingestion throughput (samples/second folded into the store).
    pub ingest_sps: f64,
    /// RAM-adaptive caps and current store footprint.
    pub max_samples: u64,
    pub raw_cap: u64,
    pub mem_used_mb: f64,
    pub last_error: Option<String>,
}

/// Aggregate snapshots pushed by the device, surfaced to the front-end.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqSnapshots {
    pub total_samples: u64,
    pub sample_rate_hz: u32,
    pub actual_rate_hz: f64,
    pub dropped_samples: u64,
    pub stats: Option<StatsRecord>,
    pub energy: Option<EnergyRecord>,
    pub fft: Option<FftRecord>,
    pub status: Option<StatusRecord>,
}

pub struct DaqState {
    pub transport: Arc<Mutex<Option<Box<dyn DaqTransport>>>>,
    pub store: Arc<RwLock<DaqStore>>,
    pub running: Arc<AtomicBool>,
    pub mock: Arc<AtomicBool>,
    /// Background worker handles (ingest + processor).
    pub tasks: Arc<Mutex<Vec<tokio::task::JoinHandle<()>>>>,
    pub status: Arc<Mutex<DaqStreamRuntimeStatus>>,
}

impl Default for DaqState {
    fn default() -> Self {
        Self::new()
    }
}

impl DaqState {
    pub fn new() -> Self {
        Self {
            transport: Arc::new(Mutex::new(None)),
            store: Arc::new(RwLock::new(DaqStore::new(250_000))),
            running: Arc::new(AtomicBool::new(false)),
            mock: Arc::new(AtomicBool::new(false)),
            tasks: Arc::new(Mutex::new(Vec::new())),
            status: Arc::new(Mutex::new(DaqStreamRuntimeStatus::default())),
        }
    }
}

/// Stop the worker threads and wait for them to exit.
async fn stop_workers(daq: &DaqState) {
    daq.running.store(false, Ordering::SeqCst);
    let handles: Vec<_> = daq
        .tasks
        .lock()
        .map(|mut t| t.drain(..).collect())
        .unwrap_or_default();
    for h in handles {
        let _ = tokio::time::timeout(Duration::from_secs(1), h).await;
    }
}

/// True if a P4 DAQ device is present on USB.
#[tauri::command]
pub fn daq_check_usb() -> bool {
    daq_usb_present()
}

/// Open a transport. `mock = true` attaches the synthetic source instead of USB.
#[tauri::command]
pub async fn daq_connect(mock: bool, daq: State<'_, DaqState>) -> CmdResult<bool> {
    // Stop any prior stream and clear state.
    stop_workers(&daq).await;

    let transport: Box<dyn DaqTransport> = if mock {
        Box::new(MockDaqTransport::new())
    } else {
        let mut conn = DaqUsbConnection::new();
        // USB claim can block briefly; run it on a blocking thread.
        let connected = tokio::task::spawn_blocking(move || match conn.connect() {
            Ok(()) => Ok(conn),
            Err(e) => Err(e.to_string()),
        })
        .await
        .map_err(map_err)??;
        Box::new(connected)
    };

    *daq.transport.lock().map_err(map_err)? = Some(transport);
    daq.mock.store(mock, Ordering::SeqCst);
    {
        let mut store = daq.store.write().map_err(map_err)?;
        *store = DaqStore::new(250_000);
    }
    {
        let mut st = daq.status.lock().map_err(map_err)?;
        *st = DaqStreamRuntimeStatus {
            connected: true,
            mock,
            ..Default::default()
        };
    }
    Ok(true)
}

#[tauri::command]
pub async fn daq_disconnect(daq: State<'_, DaqState>) -> CmdResult<()> {
    stop_workers(&daq).await;
    *daq.transport.lock().map_err(map_err)? = None;
    *daq.status.lock().map_err(map_err)? = DaqStreamRuntimeStatus::default();
    Ok(())
}

/// Send a control command over the active transport (locks briefly).
fn send_ctrl(daq: &DaqState, cmd_type: u8, payload: &[u8]) -> CmdResult<()> {
    let mut guard = daq.transport.lock().map_err(map_err)?;
    let t = guard
        .as_mut()
        .ok_or_else(|| "DAQ not connected".to_string())?;
    t.send(cmd_type, payload).map_err(map_err)
}

/// Configure rate + decimation, reset the store, and start streaming.
#[tauri::command]
pub async fn daq_stream_start(
    sample_rate_idx: u8,
    voltage_rate_idx: u8,
    decimation: u16,
    daq: State<'_, DaqState>,
) -> CmdResult<()> {
    // Stop any prior workers first.
    stop_workers(&daq).await;

    let rate = rate_from_idx(sample_rate_idx);
    let vrate = rate_from_idx(voltage_rate_idx);
    let dec = decimation.clamp(1, 256) as u8;

    {
        let mut store = daq.store.write().map_err(map_err)?;
        *store = DaqStore::new(rate);
        store.decimation = dec;
    }
    send_ctrl(
        &daq,
        daq_proto::CMD_SET_RATE,
        &daq_proto::rate_payload(rate, vrate, dec),
    )?;
    send_ctrl(&daq, daq_proto::CMD_START, &[])?;

    daq.running.store(true, Ordering::SeqCst);
    {
        let mut st = daq.status.lock().map_err(map_err)?;
        st.active = true;
        st.sample_rate_hz = rate;
        st.voltage_sps_hz = vrate;
        st.last_error = None;
    }

    // Two-thread pipeline: ingest (transport → channel) and processor
    // (channel → store + pyramid). A bounded channel applies backpressure.
    let (tx, rx) = sync_channel::<Vec<DaqRecord>>(512);
    let ingest = {
        let transport = daq.transport.clone();
        let running = daq.running.clone();
        tokio::task::spawn_blocking(move || ingest_loop(transport, running, tx))
    };
    let processor = {
        let store = daq.store.clone();
        let running = daq.running.clone();
        let status = daq.status.clone();
        tokio::task::spawn_blocking(move || process_loop(rx, store, running, status))
    };
    {
        let mut tasks = daq.tasks.lock().map_err(map_err)?;
        tasks.clear();
        tasks.push(ingest);
        tasks.push(processor);
    }
    Ok(())
}

/// Stop streaming (sends STOP, joins both worker threads).
#[tauri::command]
pub async fn daq_stream_stop(daq: State<'_, DaqState>) -> CmdResult<()> {
    let _ = send_ctrl(&daq, daq_proto::CMD_STOP, &[]);
    stop_workers(&daq).await;
    if let Ok(mut st) = daq.status.lock() {
        st.active = false;
    }
    Ok(())
}

/// Ingest thread: read decoded records from the transport and hand them to the
/// processor over a bounded channel. Does no heavy work so the USB/mock read is
/// never stalled by store writes or pyramid building.
fn ingest_loop(
    transport: Arc<Mutex<Option<Box<dyn DaqTransport>>>>,
    running: Arc<AtomicBool>,
    tx: SyncSender<Vec<DaqRecord>>,
) {
    while running.load(Ordering::SeqCst) {
        let records = {
            let mut guard = match transport.lock() {
                Ok(g) => g,
                Err(_) => break,
            };
            match guard.as_mut() {
                Some(t) => t.read_records(),
                None => break,
            }
        };
        match records {
            Ok(r) if !r.is_empty() => {
                // Bounded send applies backpressure if the processor lags.
                if tx.send(r).is_err() {
                    break; // receiver gone
                }
            }
            // DESK-24: give a waiting control command a chance at the mutex.
            Ok(_) => std::thread::yield_now(),
            Err(_) => std::thread::sleep(Duration::from_millis(20)),
        }
    }
}

/// Processor thread: owns the store, folds incoming records into the raw arrays
/// and the multi-resolution pyramid, and publishes throughput/memory metrics.
fn process_loop(
    rx: Receiver<Vec<DaqRecord>>,
    store: Arc<RwLock<DaqStore>>,
    running: Arc<AtomicBool>,
    status: Arc<Mutex<DaqStreamRuntimeStatus>>,
) {
    let mut perf_t = Instant::now();
    let mut perf_last_total = 0u64;
    loop {
        match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(records) => {
                let mut frames = 0u64;
                {
                    let mut s = store.write().unwrap_or_else(|e| e.into_inner());
                    for rec in records {
                        frames += 1;
                        match rec {
                            DaqRecord::WaveI(w) => s.append_wave_i(&w),
                            DaqRecord::WaveV(w) => s.append_wave_v(&w),
                            DaqRecord::Stats(st) => s.last_stats = Some(st),
                            DaqRecord::Energy(en) => s.last_energy = Some(en),
                            DaqRecord::Fft(f) => s.last_fft = Some(f),
                            DaqRecord::Status(stt) => s.last_status = Some(stt),
                            DaqRecord::Marker(m) => s.push_marker(&m),
                            DaqRecord::Other(_) => {}
                        }
                    }
                }
                if let (Ok(mut st), Ok(s)) = (status.lock(), store.read()) {
                    st.total_samples = s.total_samples();
                    st.sample_rate_hz = s.sample_rate_hz;
                    st.actual_rate_hz = s.actual_rate_hz();
                    st.dropped_samples = s.dropped_samples();
                    st.overflow = s.overflow;
                    st.frame_count += frames;
                    st.max_samples = s.max_samples as u64;
                    st.raw_cap = s.raw_cap as u64;
                    st.mem_used_mb = s.mem_used_bytes() as f64 / 1e6;
                    let now = Instant::now();
                    let dt = now.duration_since(perf_t).as_secs_f64();
                    if dt >= 0.5 {
                        let cur = s.total_samples();
                        st.ingest_sps = (cur.saturating_sub(perf_last_total)) as f64 / dt;
                        perf_last_total = cur;
                        perf_t = now;
                    }
                }
            }
            Err(RecvTimeoutError::Timeout) => {
                if !running.load(Ordering::SeqCst) {
                    break;
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
        }
    }
}

#[tauri::command]
pub fn daq_stream_status(daq: State<'_, DaqState>) -> CmdResult<DaqStreamRuntimeStatus> {
    let mut st = daq.status.lock().map_err(map_err)?.clone();
    st.connected = daq.transport.lock().map_err(map_err)?.is_some();
    st.mock = daq.mock.load(Ordering::SeqCst);
    if let Ok(s) = daq.store.read() {
        st.total_samples = s.total_samples();
        st.actual_rate_hz = s.actual_rate_hz();
        st.dropped_samples = s.dropped_samples();
        st.overflow = s.overflow;
        st.max_samples = s.max_samples as u64;
        st.raw_cap = s.raw_cap as u64;
        st.mem_used_mb = s.mem_used_bytes() as f64 / 1e6;
    }
    Ok(st)
}

#[tauri::command]
pub fn daq_get_view(
    start: u64,
    end: u64,
    max_points: u32,
    smooth: u32,
    filter_type: u8,
    daq: State<'_, DaqState>,
) -> CmdResult<tauri::ipc::Response> {
    // DESK-25: raw bytes (ArrayBuffer on the JS side), not JSON.
    let store = daq.store.read().map_err(map_err)?;
    let view = store.get_view(start, end, max_points, smooth, filter_type);
    Ok(tauri::ipc::Response::new(view.to_view_bytes()))
}

#[tauri::command]
pub fn daq_get_integral(start: u64, end: u64, daq: State<'_, DaqState>) -> CmdResult<DaqIntegral> {
    let store = daq.store.read().map_err(map_err)?;
    Ok(store.integrate(start, end))
}

#[tauri::command]
pub fn daq_get_snapshots(daq: State<'_, DaqState>) -> CmdResult<DaqSnapshots> {
    let s = daq.store.read().map_err(map_err)?;
    Ok(DaqSnapshots {
        total_samples: s.total_samples(),
        sample_rate_hz: s.sample_rate_hz,
        actual_rate_hz: s.actual_rate_hz(),
        dropped_samples: s.dropped_samples(),
        stats: s.last_stats,
        energy: s.last_energy,
        fft: s.last_fft.clone(),
        status: s.last_status,
    })
}

#[tauri::command]
pub async fn daq_set_range_lock(range: u8, daq: State<'_, DaqState>) -> CmdResult<()> {
    send_ctrl(&daq, daq_proto::CMD_RANGE_LOCK, &[range])
}

/// Update the acquisition rate + decimation on a live stream (no restart).
#[tauri::command]
pub async fn daq_set_rate(sample_rate_idx: u8, voltage_rate_idx: u8, decimation: u16, daq: State<'_, DaqState>) -> CmdResult<()> {
    let rate = rate_from_idx(sample_rate_idx);
    let vrate = rate_from_idx(voltage_rate_idx);
    let dec = decimation.clamp(1, 256) as u8;
    if let Ok(mut store) = daq.store.write() {
        store.decimation = dec;
    }
    if let Ok(mut st) = daq.status.lock() {
        st.voltage_sps_hz = vrate;
    }
    send_ctrl(
        &daq,
        daq_proto::CMD_SET_RATE,
        &daq_proto::rate_payload(rate, vrate, dec),
    )
}

#[tauri::command]
pub async fn daq_set_source(
    vdut_mv: u32,
    ilimit_ma: u32,
    enable: bool,
    daq: State<'_, DaqState>,
) -> CmdResult<()> {
    let vdut = vdut_mv as f32 / 1000.0;
    let ilimit = ilimit_ma as f32 / 1000.0;
    send_ctrl(
        &daq,
        daq_proto::CMD_SET_SOURCE,
        &daq_proto::source_payload(vdut, ilimit, enable),
    )
}

#[tauri::command]
pub async fn daq_set_fft(
    nbins: u16,
    source: u8,
    window: u8,
    enabled: bool,
    daq: State<'_, DaqState>,
) -> CmdResult<()> {
    send_ctrl(
        &daq,
        daq_proto::CMD_FFT_CONFIG,
        &daq_proto::fft_payload(nbins, source, window, enabled),
    )
}

#[tauri::command]
pub async fn daq_reset_energy(daq: State<'_, DaqState>) -> CmdResult<()> {
    send_ctrl(&daq, daq_proto::CMD_RESET_ENERGY, &[])
}

#[tauri::command]
pub async fn daq_reset_stats(daq: State<'_, DaqState>) -> CmdResult<()> {
    send_ctrl(&daq, daq_proto::CMD_RESET_STATS, &[])
}

/// Drop all captured samples on the host (the stream, if running, keeps going).
#[tauri::command]
pub async fn daq_clear_capture(daq: State<'_, DaqState>) -> CmdResult<()> {
    let mut store = daq.store.write().map_err(map_err)?;
    let (rate, dec) = (store.sample_rate_hz, store.decimation);
    *store = DaqStore::new(rate);
    store.decimation = dec;
    Ok(())
}

// ---- Persistent settings via the S3 BBP control plane (CMD_DAQ_CONFIG) ------

const DAQ_CFG_GET: u8 = 0x00;
const DAQ_CFG_SET: u8 = 0x01;

/// Set a single persistent DAQ setting (TLV: [op][key u16 LE][type u8][len u8][value]).
#[tauri::command]
pub async fn daq_cfg_set(
    key: u16,
    type_tag: u8,
    value: Vec<u8>,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let mut payload = vec![DAQ_CFG_SET];
    payload.extend_from_slice(&key.to_le_bytes());
    payload.push(type_tag);
    payload.push(value.len() as u8);
    payload.extend_from_slice(&value);
    mgr.send_command(bbp::CMD_DAQ_CONFIG, &payload)
        .await
        .map(|_| ())
        .map_err(map_err)
}

// ---- Trigger / flag configuration via the S3 BBP control plane (CMD_DAQ_TRIG) ┐
// The 12 IO event sources live on the S3 mainboard; the engine forwards edge
// events to the P4 as digital MARKERs (rendered as vertical lines), and trigger
// edges anchor t=0 for the acquisition window.

/// Per-IO trigger/flag configuration (mirrors daq_trig_io_cfg_t on the S3).
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqTrigIoCfg {
    pub role: u8,        // 0 off, 1 flag, 2 trigger
    pub edge: u8,        // 0 rising, 1 falling, 2 any
    pub source: u8,      // 0 digital, 1 analog (HV IOs 3/6/9/12 only)
    pub threshold_v: f32, // analog threshold (V) when source == analog
}

/// Whole-engine status + all 12 IO configs (mirrors BBP_DAQ_TRIG_GET_ALL).
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqTrigState {
    pub logic: u8,  // 0 none, 1 OR, 2 AND
    pub armed: bool,
    pub fired: bool,
    pub ios: Vec<DaqTrigIoCfg>, // index 0 = IO1 .. index 11 = IO12
}

/// Configure one IO (1..12) as off / flag / trigger.
#[tauri::command]
pub async fn daq_set_io_role(
    io: u8,
    role: u8,
    edge: u8,
    source: u8,
    threshold_v: f32,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    // [op][io][role][edge][src][_pad][thr f32 LE]
    let mut payload = vec![bbp::DAQ_TRIG_SET_IO, io, role, edge, source, 0];
    payload.extend_from_slice(&threshold_v.to_le_bytes());
    mgr.send_command(bbp::CMD_DAQ_TRIG, &payload)
        .await
        .map(|_| ())
        .map_err(map_err)
}

/// Set the trigger-group combination logic (0 none, 1 OR, 2 AND).
#[tauri::command]
pub async fn daq_set_trig_logic(logic: u8, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_DAQ_TRIG, &[bbp::DAQ_TRIG_SET_LOGIC, logic])
        .await
        .map(|_| ())
        .map_err(map_err)
}

/// Arm / disarm the trigger latch with a pre-trigger depth (fused samples).
#[tauri::command]
pub async fn daq_arm(
    armed: bool,
    pre_samples: u32,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    // [op][armed][_pad][pre_samples u32 LE]
    let mut payload = vec![bbp::DAQ_TRIG_ARM, armed as u8, 0];
    payload.extend_from_slice(&pre_samples.to_le_bytes());
    mgr.send_command(bbp::CMD_DAQ_TRIG, &payload)
        .await
        .map(|_| ())
        .map_err(map_err)
}

/// Read the whole trigger engine state + all 12 IO configs.
#[tauri::command]
pub async fn daq_get_trig_state(mgr: State<'_, ConnectionManager>) -> CmdResult<DaqTrigState> {
    let resp = mgr
        .send_command(bbp::CMD_DAQ_TRIG, &[bbp::DAQ_TRIG_GET_ALL])
        .await
        .map_err(map_err)?;
    // [logic][armed][fired][_pad] + 12 * {role,edge,src,_pad,thr f32} (8 bytes)
    if resp.len() < 4 {
        return Err(format!("short trig state: {} bytes", resp.len()));
    }
    let mut st = DaqTrigState {
        logic: resp[0],
        armed: resp[1] != 0,
        fired: resp[2] != 0,
        ios: Vec::with_capacity(12),
    };
    let mut o = 4;
    for _ in 0..12 {
        if o + 8 > resp.len() {
            break;
        }
        st.ios.push(DaqTrigIoCfg {
            role: resp[o],
            edge: resp[o + 1],
            source: resp[o + 2],
            threshold_v: f32::from_le_bytes([
                resp[o + 4],
                resp[o + 5],
                resp[o + 6],
                resp[o + 7],
            ]),
        });
        o += 8;
    }
    Ok(st)
}

/// Return all event markers within an absolute sample range (never decimated).
#[tauri::command]
pub fn daq_get_markers(
    start: u64,
    end: u64,
    daq: State<'_, DaqState>,
) -> CmdResult<Vec<crate::daq_store::DaqMarker>> {
    let store = daq.store.read().map_err(map_err)?;
    let view = store.get_view(start, end.max(start + 1), 1, 0, 0);
    Ok(view.markers)
}

// ---- SMU factory calibration via the S3 BBP control plane (CMD_DAQ_CAL) ------

const DAQ_CAL_START: u8 = 0x00;
const DAQ_CAL_ACK: u8 = 0x01;
const DAQ_CAL_STATUS: u8 = 0x02;
const DAQ_CAL_ABORT: u8 = 0x03;

/// Parsed smu_cal_status_t (24 bytes packed LE) for the calibration wizard.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqCalStatus {
    pub phase: u8,        // 0 idle,1 prompt,2 running,3 success,4 failed
    pub prompt: u8,       // 0 none,1 disconnect_load,2 short_output
    pub mode: u8,         // 0 voltage,1 current
    pub progress: u8,     // 0..100
    pub point: u8,
    pub code: i8,
    pub persist: u8,      // 0 ram,1 saving,2 saved,3 failed
    pub measured: f32,
    pub min: f32,
    pub max: f32,
    pub flags: u16,
    pub vcount: u8,
    pub icount: u8,
}

fn parse_cal_status(b: &[u8]) -> Result<DaqCalStatus, String> {
    if b.len() < 24 {
        return Err(format!("short cal status: {} < 24 bytes", b.len()));
    }
    let f32le = |o: usize| f32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    Ok(DaqCalStatus {
        phase: b[0],
        prompt: b[1],
        mode: b[2],
        progress: b[3],
        point: b[4],
        code: b[5] as i8,
        persist: b[6],
        // b[7] is padding
        measured: f32le(8),
        min: f32le(12),
        max: f32le(16),
        flags: u16::from_le_bytes([b[20], b[21]]),
        vcount: b[22],
        icount: b[23],
    })
}

/// Start an SMU calibration run (mode: 0=voltage, 1=current). Interactive: poll
/// `daq_cal_status` until `phase==1` (prompt), perform the requested action,
/// then call `daq_cal_ack`.
#[tauri::command]
pub async fn daq_cal_start(mode: u8, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_DAQ_CAL, &[DAQ_CAL_START, mode])
        .await
        .map(|_| ())
        .map_err(map_err)
}

/// Acknowledge the current calibration prompt so the run can proceed.
#[tauri::command]
pub async fn daq_cal_ack(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_DAQ_CAL, &[DAQ_CAL_ACK])
        .await
        .map(|_| ())
        .map_err(map_err)
}

/// Abort the in-progress calibration and restore a safe SMU state.
#[tauri::command]
pub async fn daq_cal_abort(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_DAQ_CAL, &[DAQ_CAL_ABORT])
        .await
        .map(|_| ())
        .map_err(map_err)
}

/// Poll the live calibration status.
#[tauri::command]
pub async fn daq_cal_status(mgr: State<'_, ConnectionManager>) -> CmdResult<DaqCalStatus> {
    let resp = mgr
        .send_command(bbp::CMD_DAQ_CAL, &[DAQ_CAL_STATUS])
        .await
        .map_err(map_err)?;
    parse_cal_status(&resp)
}

// ---- Live measurement readback via the S3 BBP control plane (CMD_DAQ_MEASURE) -

/// Parsed s3link_daq_status_t (20 bytes packed LE): the latest fused reading.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqMeasure {
    pub range: u8,           // 0 hi,1 mid,2 lo,0xFF unknown
    pub streaming: bool,
    pub source_enabled: bool,
    pub current_a: f32,
    pub voltage_v: f32,
    pub power_w: f32,
    pub energy_mwh: f32,
}

/// Read the DAQ HAT's latest fused measurement (I/V/P/energy + range/state).
#[tauri::command]
pub async fn daq_measure(mgr: State<'_, ConnectionManager>) -> CmdResult<DaqMeasure> {
    let b = mgr
        .send_command(bbp::CMD_DAQ_MEASURE, &[])
        .await
        .map_err(map_err)?;
    if b.len() < 20 {
        return Err(format!("short DAQ status: {} < 20 bytes", b.len()));
    }
    let f32le = |o: usize| f32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    Ok(DaqMeasure {
        range: b[0],
        streaming: b[1] != 0,
        source_enabled: b[2] != 0,
        // b[3] is padding
        current_a: f32le(4),
        voltage_v: f32le(8),
        power_w: f32le(12),
        energy_mwh: f32le(16),
    })
}

// ---- DUT supply (V_DUT) control that works over USB *and* HTTP --------------
// The settings registry and DAQ_MEASURE are BBP-only, so over HTTP these use
// the firmware's /api/daq/vdut/* routes instead.

const DAQ_K_SOURCE_ENABLE: u16 = 0x0201;
const DAQ_K_DUT_VOLTAGE_MV: u16 = 0x0202;
const DAQ_K_DUT_ILIMIT_MA: u16 = 0x0203;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VdutStatus {
    pub enabled: bool,
    pub setpoint_mv: u32,
    pub ilimit_ma: u32,
    pub voltage_v: f32,
    pub current_a: f32,
    pub fault: bool,
}

async fn vdut_http(mgr: &ConnectionManager) -> Option<(String, reqwest::Client)> {
    let url = mgr.get_base_url().await?;
    let mut headers = reqwest::header::HeaderMap::new();
    if let Some(token) = mgr.get_connection_status().admin_token {
        if let Ok(v) = reqwest::header::HeaderValue::from_str(&token) {
            headers.insert("X-BugBuster-Admin-Token", v);
        }
    }
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(5))
        .default_headers(headers)
        .build()
        .ok()?;
    Some((url, client))
}

async fn http_json(req: reqwest::RequestBuilder, what: &str) -> CmdResult<serde_json::Value> {
    let resp = req.send().await.map_err(map_err)?;
    if !resp.status().is_success() {
        return Err(format!("HTTP {} from {}", resp.status(), what));
    }
    let json: serde_json::Value = resp.json().await.map_err(map_err)?;
    if json.get("ok").and_then(|v| v.as_bool()) == Some(false) {
        return Err(json.get("error").and_then(|v| v.as_str()).unwrap_or(what).to_string());
    }
    Ok(json)
}

async fn cfg_get_u32(mgr: &ConnectionManager, key: u16) -> CmdResult<u32> {
    let mut payload = vec![DAQ_CFG_GET];
    payload.extend_from_slice(&key.to_le_bytes());
    let raw = mgr.send_command(bbp::CMD_DAQ_CONFIG, &payload).await.map_err(map_err)?;
    // One TLV: key u16, type u8, len u8, value.
    let vlen = *raw.get(3).ok_or("short DAQ config reply")? as usize;
    let val = raw.get(4..4 + vlen).ok_or("truncated DAQ config reply")?;
    let mut b = [0u8; 4];
    b[..vlen.min(4)].copy_from_slice(&val[..vlen.min(4)]);
    Ok(u32::from_le_bytes(b))
}

async fn cfg_set(mgr: &ConnectionManager, key: u16, type_tag: u8, value: &[u8]) -> CmdResult<()> {
    let mut payload = vec![DAQ_CFG_SET];
    payload.extend_from_slice(&key.to_le_bytes());
    payload.push(type_tag);
    payload.push(value.len() as u8);
    payload.extend_from_slice(value);
    mgr.send_command(bbp::CMD_DAQ_CONFIG, &payload).await.map(|_| ()).map_err(map_err)
}

#[tauri::command]
pub async fn daq_vdut_status(mgr: State<'_, ConnectionManager>) -> CmdResult<VdutStatus> {
    if let Some((url, client)) = vdut_http(&mgr).await {
        let j = http_json(client.get(format!("{url}/api/daq/vdut/status")), "/api/daq/vdut/status").await?;
        let f = |k: &str| j.get(k).and_then(|v| v.as_f64()).unwrap_or(0.0);
        let b = |k: &str| j.get(k).and_then(|v| v.as_bool()).unwrap_or(false);
        return Ok(VdutStatus {
            enabled: b("enabled"),
            setpoint_mv: (f("voltageSetpointV") * 1000.0).round() as u32,
            ilimit_ma: f("currentLimitMa").round() as u32,
            voltage_v: f("measuredVoltageV") as f32,
            current_a: (f("measuredCurrentMa") / 1000.0) as f32,
            fault: b("fault"),
        });
    }
    let m = daq_measure(mgr.clone()).await?;
    Ok(VdutStatus {
        enabled: m.source_enabled,
        setpoint_mv: cfg_get_u32(&mgr, DAQ_K_DUT_VOLTAGE_MV).await?,
        ilimit_ma: cfg_get_u32(&mgr, DAQ_K_DUT_ILIMIT_MA).await?,
        voltage_v: m.voltage_v,
        current_a: m.current_a,
        fault: false,
    })
}

#[tauri::command]
pub async fn daq_vdut_set_enable(enabled: bool, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    if let Some((url, client)) = vdut_http(&mgr).await {
        let req = client
            .post(format!("{url}/api/daq/vdut/enable"))
            .json(&serde_json::json!({ "enabled": enabled }));
        return http_json(req, "/api/daq/vdut/enable").await.map(|_| ());
    }
    cfg_set(&mgr, DAQ_K_SOURCE_ENABLE, 1, &[enabled as u8]).await
}

#[tauri::command]
pub async fn daq_vdut_set_setpoint(
    voltage_mv: u16,
    ilimit_ma: u16,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    if let Some((url, client)) = vdut_http(&mgr).await {
        let req = client.post(format!("{url}/api/daq/vdut/setpoint")).json(&serde_json::json!({
            "voltageV": voltage_mv as f64 / 1000.0,
            "currentLimitMa": ilimit_ma,
        }));
        return http_json(req, "/api/daq/vdut/setpoint").await.map(|_| ());
    }
    // The P4 ramps V_DUT one code per 10 ms inside the SET (TODO P4-9), so a big
    // step outlives the S3's 300 ms HAT timeout and returns 0x11 although it
    // applies. Confirm by reading the key back instead of failing.
    if let Err(e) = cfg_set(&mgr, DAQ_K_DUT_VOLTAGE_MV, 4, &voltage_mv.to_le_bytes()).await {
        if !e.contains("0x11") {
            return Err(e);
        }
        let mut confirmed = false;
        for _ in 0..8 {
            tokio::time::sleep(Duration::from_millis(500)).await;
            if cfg_get_u32(&mgr, DAQ_K_DUT_VOLTAGE_MV).await.ok() == Some(voltage_mv as u32) {
                confirmed = true;
                break;
            }
        }
        if !confirmed {
            return Err(e);
        }
    }
    cfg_set(&mgr, DAQ_K_DUT_ILIMIT_MA, 4, &ilimit_ma.to_le_bytes()).await
}
