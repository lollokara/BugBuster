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
use crate::daq_store::{DaqIntegral, DaqStore, DaqViewData};
use crate::daq_usb::{daq_usb_present, DaqTransport, DaqUsbConnection, MockDaqTransport};
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
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
    pub overflow: bool,
    pub last_error: Option<String>,
}

/// Aggregate snapshots pushed by the device, surfaced to the front-end.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqSnapshots {
    pub total_samples: u64,
    pub sample_rate_hz: u32,
    pub stats: Option<StatsRecord>,
    pub energy: Option<EnergyRecord>,
    pub fft: Option<FftRecord>,
    pub status: Option<StatusRecord>,
}

pub struct DaqState {
    pub transport: Arc<Mutex<Option<Box<dyn DaqTransport>>>>,
    pub store: Arc<Mutex<DaqStore>>,
    pub running: Arc<AtomicBool>,
    pub mock: Arc<AtomicBool>,
    pub task: Arc<Mutex<Option<tokio::task::JoinHandle<()>>>>,
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
            store: Arc::new(Mutex::new(DaqStore::new(250_000))),
            running: Arc::new(AtomicBool::new(false)),
            mock: Arc::new(AtomicBool::new(false)),
            task: Arc::new(Mutex::new(None)),
            status: Arc::new(Mutex::new(DaqStreamRuntimeStatus::default())),
        }
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
    daq.running.store(false, Ordering::SeqCst);
    let prior = daq.task.lock().map_err(map_err)?.take();
    if let Some(handle) = prior {
        let _ = tokio::time::timeout(std::time::Duration::from_millis(500), handle).await;
    }

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
        let mut store = daq.store.lock().map_err(map_err)?;
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
    daq.running.store(false, Ordering::SeqCst);
    let prior = daq.task.lock().map_err(map_err)?.take();
    if let Some(handle) = prior {
        let _ = tokio::time::timeout(std::time::Duration::from_millis(500), handle).await;
    }
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
    decimation: u16,
    daq: State<'_, DaqState>,
) -> CmdResult<()> {
    // Stop any prior task first.
    daq.running.store(false, Ordering::SeqCst);
    let prior = daq.task.lock().map_err(map_err)?.take();
    if let Some(handle) = prior {
        let _ = tokio::time::timeout(std::time::Duration::from_millis(500), handle).await;
    }

    let rate = rate_from_idx(sample_rate_idx);
    let dec = decimation.clamp(1, 256) as u8;

    {
        let mut store = daq.store.lock().map_err(map_err)?;
        *store = DaqStore::new(rate);
        store.decimation = dec;
    }
    send_ctrl(
        &daq,
        daq_proto::CMD_SET_RATE,
        &daq_proto::rate_payload(rate, 50_000, dec),
    )?;
    send_ctrl(&daq, daq_proto::CMD_START, &[])?;

    daq.running.store(true, Ordering::SeqCst);
    {
        let mut st = daq.status.lock().map_err(map_err)?;
        st.active = true;
        st.sample_rate_hz = rate;
        st.last_error = None;
    }

    let transport = daq.transport.clone();
    let store = daq.store.clone();
    let running = daq.running.clone();
    let status = daq.status.clone();

    let handle = tokio::task::spawn_blocking(move || {
        run_stream_loop(&transport, &store, &running, &status);
    });
    *daq.task.lock().map_err(map_err)? = Some(handle);
    Ok(())
}

/// Stop streaming (sends STOP, joins the reader task).
#[tauri::command]
pub async fn daq_stream_stop(daq: State<'_, DaqState>) -> CmdResult<()> {
    let _ = send_ctrl(&daq, daq_proto::CMD_STOP, &[]);
    daq.running.store(false, Ordering::SeqCst);
    let prior = daq.task.lock().map_err(map_err)?.take();
    if let Some(handle) = prior {
        let _ = tokio::time::timeout(std::time::Duration::from_secs(1), handle).await;
    }
    if let Ok(mut st) = daq.status.lock() {
        st.active = false;
    }
    Ok(())
}

/// Background reader: pulls decoded records and folds them into the store.
fn run_stream_loop(
    transport: &Arc<Mutex<Option<Box<dyn DaqTransport>>>>,
    store: &Arc<Mutex<DaqStore>>,
    running: &Arc<AtomicBool>,
    status: &Arc<Mutex<DaqStreamRuntimeStatus>>,
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
        let records = match records {
            Ok(r) => r,
            Err(e) => {
                if let Ok(mut st) = status.lock() {
                    st.last_error = Some(e.to_string());
                }
                // Transient read timeout while idle is not fatal for the mock;
                // for USB a persistent error will keep landing here. Brief nap.
                std::thread::sleep(std::time::Duration::from_millis(20));
                continue;
            }
        };
        if records.is_empty() {
            continue;
        }
        let mut frames = 0u64;
        {
            let mut s = store.lock().unwrap_or_else(|e| e.into_inner());
            for rec in records {
                frames += 1;
                match rec {
                    DaqRecord::Waveform(w) => s.append_waveform(&w),
                    DaqRecord::Stats(st) => s.last_stats = Some(st),
                    DaqRecord::Energy(en) => s.last_energy = Some(en),
                    DaqRecord::Fft(f) => s.last_fft = Some(f),
                    DaqRecord::Status(stt) => s.last_status = Some(stt),
                    DaqRecord::Other(_) => {}
                }
            }
        }
        if let (Ok(mut st), Ok(s)) = (status.lock(), store.lock()) {
            st.total_samples = s.total_samples();
            st.sample_rate_hz = s.sample_rate_hz;
            st.overflow = s.overflow;
            st.frame_count += frames;
        }
    }
}

#[tauri::command]
pub fn daq_stream_status(daq: State<'_, DaqState>) -> CmdResult<DaqStreamRuntimeStatus> {
    let mut st = daq.status.lock().map_err(map_err)?.clone();
    st.connected = daq.transport.lock().map_err(map_err)?.is_some();
    st.mock = daq.mock.load(Ordering::SeqCst);
    if let Ok(s) = daq.store.lock() {
        st.total_samples = s.total_samples();
        st.overflow = s.overflow;
    }
    Ok(st)
}

#[tauri::command]
pub fn daq_get_view(
    start: u64,
    end: u64,
    max_points: u32,
    daq: State<'_, DaqState>,
) -> CmdResult<DaqViewData> {
    let store = daq.store.lock().map_err(map_err)?;
    Ok(store.get_view(start, end, max_points))
}

#[tauri::command]
pub fn daq_get_integral(start: u64, end: u64, daq: State<'_, DaqState>) -> CmdResult<DaqIntegral> {
    let store = daq.store.lock().map_err(map_err)?;
    Ok(store.integrate(start, end))
}

#[tauri::command]
pub fn daq_get_snapshots(daq: State<'_, DaqState>) -> CmdResult<DaqSnapshots> {
    let s = daq.store.lock().map_err(map_err)?;
    Ok(DaqSnapshots {
        total_samples: s.total_samples(),
        sample_rate_hz: s.sample_rate_hz,
        stats: s.last_stats,
        energy: s.last_energy,
        fft: s.last_fft.clone(),
        status: s.last_status,
    })
}

#[tauri::command]
pub fn daq_set_range_lock(range: u8, daq: State<'_, DaqState>) -> CmdResult<()> {
    send_ctrl(&daq, daq_proto::CMD_RANGE_LOCK, &[range])
}

#[tauri::command]
pub fn daq_set_source(
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
pub fn daq_set_fft(
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
pub fn daq_reset_energy(daq: State<'_, DaqState>) -> CmdResult<()> {
    send_ctrl(&daq, daq_proto::CMD_RESET_ENERGY, &[])
}

#[tauri::command]
pub fn daq_reset_stats(daq: State<'_, DaqState>) -> CmdResult<()> {
    send_ctrl(&daq, daq_proto::CMD_RESET_STATS, &[])
}

// ---- Persistent settings via the S3 BBP control plane (CMD_DAQ_CONFIG) ------

const DAQ_CFG_GET: u8 = 0x00;
const DAQ_CFG_SET: u8 = 0x01;
const DAQ_CFG_ACTION: u8 = 0x04;

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

/// Read a single persistent DAQ setting; returns the raw TLV value bytes.
#[tauri::command]
pub async fn daq_cfg_get(key: u16, mgr: State<'_, ConnectionManager>) -> CmdResult<Vec<u8>> {
    let mut payload = vec![DAQ_CFG_GET];
    payload.extend_from_slice(&key.to_le_bytes());
    mgr.send_command(bbp::CMD_DAQ_CONFIG, &payload)
        .await
        .map_err(map_err)
}

/// Trigger a one-shot DAQ action (1=energy reset, 2=charge reset, 3=factory reset).
#[tauri::command]
pub async fn daq_cfg_action(action: u8, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    mgr.send_command(bbp::CMD_DAQ_CONFIG, &[DAQ_CFG_ACTION, action])
        .await
        .map(|_| ())
        .map_err(map_err)
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
