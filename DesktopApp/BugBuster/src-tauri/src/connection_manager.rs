// =============================================================================
// connection_manager.rs - Central connection and state management
//
// Owns the active transport, polls device state, emits Tauri events.
// Uses tokio::sync::Mutex for the transport since we need to hold it across await.
// =============================================================================

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::{Duration, Instant};

/// How long after a per-channel write to suppress polled overwrites of the
/// affected fields. Covers HTTP latency (~50–300 ms) plus a margin so that the
/// poll loop never clobbers a fresh user value with the firmware-pre-apply
/// snapshot. Bug 3 (ADC mode set sometimes does not stick over WiFi).
const WRITE_SUPPRESS: Duration = Duration::from_millis(500);

use anyhow::{anyhow, Result};
use tauri::{AppHandle, Emitter, Manager};
use tokio::sync::{mpsc, Mutex as TokioMutex};

use crate::bbp::{self, Message};
use crate::discovery;
use crate::http_transport::HttpTransport;
use crate::state::*;
use crate::transport::Transport;
use crate::usb_transport::UsbTransport;

#[derive(Clone)]
pub struct ConnectionManager {
    // tokio::Mutex because we hold it across .await in send_command / get_status
    transport: Arc<TokioMutex<Option<Box<dyn Transport>>>>,
    // std::Mutex is fine for these since we never hold across await
    device_state: Arc<StdMutex<DeviceState>>,
    connection_status: Arc<StdMutex<ConnectionStatus>>,
    // Shutdown flag for the poll loop — set on disconnect, checked each iteration
    poll_shutdown: Arc<AtomicBool>,
    // Persistent admin tokens keyed by device MAC
    tokens: Arc<StdMutex<HashMap<String, String>>>,
    // Per-channel write timestamps — used to suppress polled state overwrites
    // for the channel's adc/function fields right after a user-initiated write
    // (Bug 3). Keyed by channel index 0..3.
    recent_writes: Arc<StdMutex<[Option<Instant>; 4]>>,
    // IO slots the frontend has claimed and wants kept alive.
    // The keep-alive timer (2 s) re-issues CMD_IO_CLAIM for the union of these
    // slots. Cleared on disconnect so the device frees them automatically.
    pub active_slots: Arc<StdMutex<HashSet<u8>>>,
    // Flag set during OTA to relax health-check failure thresholds.
    // OTA can take 30-120 s during which /api/status may time out (M19).
    ota_in_progress: Arc<AtomicBool>,
    // Set for the duration of a connect() call so the USB watcher does not
    // probe the same port concurrently and corrupt the BBP handshake.
    connecting: Arc<AtomicBool>,
    // Set while discover_streaming() is probing USB ports so the watcher does
    // not probe the same ports at the same time and corrupt the \r\n + MAGIC
    // byte stream that probe_bbp sends.
    scanning: Arc<AtomicBool>,
}

pub struct OtaGuard {
    mgr: ConnectionManager,
}

impl Drop for OtaGuard {
    fn drop(&mut self) {
        self.mgr.set_ota_in_progress(false);
    }
}

impl ConnectionManager {
    pub fn new() -> Self {
        Self {
            transport: Arc::new(TokioMutex::new(None)),
            device_state: Arc::new(StdMutex::new(DeviceState::default())),
            connection_status: Arc::new(StdMutex::new(ConnectionStatus::default())),
            poll_shutdown: Arc::new(AtomicBool::new(false)),
            tokens: Arc::new(StdMutex::new(HashMap::new())),
            recent_writes: Arc::new(StdMutex::new([None; 4])),
            active_slots: Arc::new(StdMutex::new(HashSet::new())),
            ota_in_progress: Arc::new(AtomicBool::new(false)),
            connecting: Arc::new(AtomicBool::new(false)),
            scanning: Arc::new(AtomicBool::new(false)),
        }
    }

    /// Set whether an OTA update is currently in progress.
    pub fn set_ota_in_progress(&self, in_progress: bool) {
        log::info!("OTA in progress: {}", in_progress);
        self.ota_in_progress.store(in_progress, Ordering::Release);
    }

    /// Return a guard that sets ota_in_progress to true, and back to false when dropped.
    pub fn ota_guard(&self) -> OtaGuard {
        self.set_ota_in_progress(true);
        OtaGuard { mgr: self.clone() }
    }

    /// Connect to a device by its discovery ID.
    pub async fn connect(&self, device_id: &str, app: &AppHandle) -> Result<()> {
        // Reject concurrent connection attempts — only one can be in flight.
        // The USB watcher also uses this flag to avoid probing a port while we
        // are mid-handshake, preventing BBP response corruption.
        if self
            .connecting
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            return Err(anyhow!("Connection attempt already in progress"));
        }
        let la_selector: Option<crate::la_usb::DeviceSelector> = None;
        let result = self._connect(device_id, la_selector, app).await;
        self.connecting.store(false, Ordering::Release);
        result
    }

    async fn _connect(
        &self,
        device_id: &str,
        la_selector: Option<crate::la_usb::DeviceSelector>,
        app: &AppHandle,
    ) -> Result<()> {
        self.disconnect(app).await?;

        if device_id.starts_with("usb:") {
            let port_name = &device_id[4..];
            self.connect_usb(port_name, la_selector, app).await
        } else if device_id.starts_with("http:") {
            let base_url = &device_id[5..];
            self.connect_http(base_url, app).await
        } else {
            Err(anyhow!("Unknown device ID format: {}", device_id))
        }
    }

    async fn connect_usb(
        &self,
        port_name: &str,
        la_selector: Option<crate::la_usb::DeviceSelector>,
        app: &AppHandle,
    ) -> Result<()> {
        let (_event_tx, mut event_rx) = mpsc::unbounded_channel::<Message>();

        log::info!("Opening USB port: {}", port_name);
        let port_name_owned = port_name.to_string();
        let event_tx_clone = _event_tx.clone();
        let first_attempt = {
            let pn = port_name_owned.clone();
            let tx = event_tx_clone.clone();
            tokio::task::spawn_blocking(move || UsbTransport::connect(&pn, tx)).await?
        };
        let transport = match first_attempt {
            Ok(t) => t,
            Err(e) => {
                log::warn!("USB connect failed, retrying in 2s: {}", e);
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                let pn = port_name_owned.clone();
                let tx = event_tx_clone.clone();
                tokio::task::spawn_blocking(move || UsbTransport::connect(&pn, tx)).await??
            }
        };
        log::info!("USB handshake completed successfully");

        // Check firmware version compatibility
        if let Some(h) = transport.handshake_info() {
            if h.proto_version != bbp::PROTO_VERSION {
                log::warn!(
                    "Protocol version mismatch: device reports v{}, expected v{}. Allowing connection but features may not work correctly.",
                    h.proto_version,
                    bbp::PROTO_VERSION
                );
                let _ = app.emit(
                    "version-mismatch",
                    &serde_json::json!({
                        "device_version": h.proto_version,
                        "expected_version": bbp::PROTO_VERSION,
                    }),
                );
            }
        }

        let device_info = transport.handshake_info().map(|h| DeviceInfo {
            proto_version: h.proto_version,
            fw_version: format!("{}.{}.{}", h.fw_major, h.fw_minor, h.fw_patch),
            mac_address: Some(h.mac_address.clone()),
            ..Default::default()
        });

        // 1. Fetch Admin Token via USB
        let mut admin_token = None;
        if let Ok(rsp) = transport.send_command(bbp::CMD_GET_ADMIN_TOKEN, &[]).await {
            if rsp.len() > 1 {
                let len = rsp[0] as usize;
                if rsp.len() >= 1 + len {
                    let token = String::from_utf8_lossy(&rsp[1..1 + len]).to_string();
                    if let Some(h) = transport.handshake_info() {
                        log::info!("Retrieved admin token via USB for device {}", h.mac_address);
                        self.save_token(h.mac_address.clone(), token.clone(), app);
                    }
                    admin_token = Some(token);
                }
            }
        }

        {
            let mut t = self.transport.lock().await;
            *t = Some(Box::new(transport));
        }

        {
            let mut status = self.connection_status.lock().unwrap_or_else(|e| {
                log::warn!(
                    "ConnectionStatus mutex poisoned — recovering stale state: {}",
                    e
                );
                e.into_inner()
            });
            status.mode = ConnectionMode::Usb;
            status.port_or_url = port_name.to_string();
            status.device_info = device_info;
            status.admin_token = admin_token;
            status.la_selector = la_selector;
        }

        let status = self
            .connection_status
            .lock()
            .unwrap_or_else(|e| {
                log::warn!(
                    "ConnectionStatus mutex poisoned — recovering stale state: {}",
                    e
                );
                e.into_inner()
            })
            .clone();
        let _ = app.emit("connection-status", &status);

        // Spawn event listener for USB stream data
        let app_handle = app.clone();
        tokio::spawn(async move {
            let mut last_adc_emit = std::time::Instant::now();
            let mut adc_buffer: Vec<u8> = Vec::new();
            let emit_interval = std::time::Duration::from_millis(33); // ~30 Hz

            loop {
                // Use a short timeout so we can flush the buffer periodically
                match tokio::time::timeout(std::time::Duration::from_millis(10), event_rx.recv())
                    .await
                {
                    Ok(Some(msg)) => {
                        match msg.cmd_id {
                            bbp::EVT_ADC_DATA => {
                                // Forward to recording backend (no frontend involvement)
                                {
                                    use crate::commands::RECORDING;
                                    if let Ok(mut guard) = RECORDING.lock() {
                                        if let Some(ref mut rec) = *guard {
                                            // Parse count from payload and write raw sample data
                                            if msg.payload.len() >= 7 {
                                                let count = u16::from_le_bytes([
                                                    msg.payload[5],
                                                    msg.payload[6],
                                                ])
                                                    as usize;
                                                let mask = msg.payload[0];
                                                let num_ch =
                                                    (0..4).filter(|b| mask & (1 << b) != 0).count();
                                                let data_len = count * num_ch * 3;
                                                let data_end = 7 + data_len;
                                                if msg.payload.len() >= data_end {
                                                    use std::io::Write;
                                                    let _ = rec
                                                        .writer
                                                        .write_all(&msg.payload[7..data_end]);
                                                    rec.sample_count += count as u64;
                                                }
                                            }
                                        }
                                    }
                                }

                                // Keep latest payload for throttled display
                                adc_buffer = msg.payload;

                                // Throttle display emit to ~30 Hz
                                if last_adc_emit.elapsed() >= emit_interval {
                                    let _ = app_handle.emit("adc-stream", &adc_buffer);
                                    last_adc_emit = std::time::Instant::now();
                                }
                            }
                            bbp::EVT_SCOPE_DATA => {
                                let _ = app_handle.emit("scope-data", &msg.payload);
                            }
                            bbp::EVT_ALERT => {
                                let _ = app_handle.emit("alert-event", &msg.payload);
                            }
                            bbp::EVT_PCA_FAULT => {
                                let _ = app_handle.emit("pca-fault", &msg.payload);
                            }
                            bbp::EVT_LA_DONE => {
                                let _ = app_handle.emit("la-done", &msg.payload);
                            }
                            bbp::EVT_LA_LOG => {
                                if let Ok(text) = std::str::from_utf8(&msg.payload) {
                                    let _ = app_handle.emit("hat-log", text.trim_end());
                                }
                            }
                            bbp::EVT_IO_OWNER_REJECT => {
                                if let Some(evt) =
                                    crate::state::IoOwnerRejectEvent::from_payload(&msg.payload)
                                {
                                    let _ = app_handle.emit("io-owner-reject", &evt);
                                } else {
                                    log::warn!(
                                        "EVT_IO_OWNER_REJECT: payload too short ({} bytes)",
                                        msg.payload.len()
                                    );
                                }
                            }
                            bbp::EVT_DISCONNECT => {
                                log::warn!("USB reader reported disconnection");
                                let _ = app_handle.emit("device-disconnected", &serde_json::json!({"reason": "serial_error", "stream_running": false}));
                                break;
                            }
                            _ => {}
                        }
                    }
                    Ok(None) => break, // Channel closed
                    Err(_) => {
                        // Timeout — flush any pending ADC data
                        if !adc_buffer.is_empty() && last_adc_emit.elapsed() >= emit_interval {
                            let _ = app_handle.emit("adc-stream", &adc_buffer);
                            last_adc_emit = std::time::Instant::now();
                        }
                    }
                }
            }
        });

        self.start_polling(app.clone());

        log::info!("Connected via USB to {}", port_name);
        Ok(())
    }

    async fn connect_http(&self, base_url: &str, app: &AppHandle) -> Result<()> {
        // Ensure tokens are loaded
        if self.tokens.lock().map(|t| t.is_empty()).unwrap_or(false) {
            self.load_tokens(app);
        }

        let candidate_tokens: Vec<String> = self
            .tokens
            .lock()
            .map(|t| t.values().cloned().collect())
            .unwrap_or_default();
        let (mut transport, mac) = HttpTransport::connect(base_url, &candidate_tokens).await?;

        // M19: If the device did not report a MAC address (legacy firmware), emit a
        // non-blocking Tauri event so the UI can prompt the user to update firmware.
        // Connection is NOT blocked — read-only HTTP still works with the sentinel key.
        if mac.starts_with("legacy:") {
            log::warn!(
                "Device at {} is missing 'macAddress' in /api/device/info — \
                 firmware update required for proper MAC-keyed pairing",
                base_url
            );
            let _ = app.emit(
                "firmware-update-required",
                &serde_json::json!({
                    "url": base_url,
                    "reason": "macAddress missing from /api/device/info — update to ESP firmware >= 3.0.0",
                }),
            );
        }

        // 2. Check for Pairing (Admin Token)
        let admin_token = self.get_token(&mac, app);
        if admin_token.is_none() {
            log::warn!(
                "HTTP connection to {} (MAC: {}) requires pairing via USB",
                base_url,
                mac
            );
            let _ = app.emit(
                "pairing-required",
                &serde_json::json!({
                    "mac": mac,
                    "url": base_url,
                }),
            );
            return Err(anyhow!(
                "Pairing required: connect via USB once to authorize this computer"
            ));
        }

        let token = admin_token
            .ok_or_else(|| anyhow::anyhow!("admin token missing after pairing check"))?;
        transport.set_admin_token(&token)?;
        // Inject AppHandle so background scope-polling task can emit
        // `scope-data` events (Bug 2).
        transport.set_app_handle(app.clone());

        let mut device_info = DeviceInfo::default();
        device_info.mac_address = Some(mac);

        {
            let mut t = self.transport.lock().await;
            *t = Some(Box::new(transport));
        }

        {
            let mut status = self.connection_status.lock().unwrap_or_else(|e| {
                log::warn!(
                    "ConnectionStatus mutex poisoned — recovering stale state: {}",
                    e
                );
                e.into_inner()
            });
            status.mode = ConnectionMode::Http;
            status.port_or_url = base_url.to_string();
            status.device_info = Some(device_info);
            status.admin_token = Some(token);
            status.la_selector = None; // Reset for WiFi
        }

        let status = self
            .connection_status
            .lock()
            .unwrap_or_else(|e| {
                log::warn!(
                    "ConnectionStatus mutex poisoned — recovering stale state: {}",
                    e
                );
                e.into_inner()
            })
            .clone();
        let _ = app.emit("connection-status", &status);

        self.start_polling(app.clone());

        log::info!("Connected via HTTP to {}", base_url);
        Ok(())
    }

    /// Disconnect the current transport.
    pub async fn disconnect(&self, app: &AppHandle) -> Result<()> {
        // Signal poll loop to exit immediately
        self.poll_shutdown.store(true, Ordering::Release);

        let transport = {
            let mut t = self.transport.lock().await;
            t.take()
        };

        if let Some(t) = transport {
            let _ = t.disconnect().await;
        }

        {
            let mut status = self.connection_status.lock().unwrap_or_else(|e| {
                log::warn!(
                    "ConnectionStatus mutex poisoned — recovering stale state: {}",
                    e
                );
                e.into_inner()
            });
            *status = ConnectionStatus::default();
        }

        // Release IO ownership tracking — device will expire the leases on its
        // own (30 s), but clearing here means a quick reconnect won't try to
        // renew slots from the old session.
        if let Ok(mut slots) = self.active_slots.lock() {
            slots.clear();
        }

        // Clean up LA state if it exists
        if let Some(la) = app.try_state::<crate::la_commands::LaState>() {
            log::info!("Cleaning up LA USB connection due to main disconnect");
            // Stop any background stream
            la.stream_running.store(false, Ordering::SeqCst);
            if let Ok(mut task) = la.stream_task.lock() {
                if let Some(handle) = task.take() {
                    handle.abort();
                }
            }
            // Close USB
            if let Ok(mut usb) = la.usb.lock() {
                let _ = usb.close();
            }
            // Notify frontend
            let _ = app.emit(
                "la-stream-stopped",
                &serde_json::json!({"reason": "main_disconnect"}),
            );
        }

        let _ = app.emit(
            "device-disconnected",
            &serde_json::json!({"reason": "manual", "stream_running": false}),
        );

        Ok(())
    }

    /// Send a command through the active transport.
    pub async fn send_command(&self, cmd_id: u8, payload: &[u8]) -> Result<Vec<u8>> {
        // Optimistically apply user intent to the locally cached device state
        // and mark the channel as recently written. The poll loop will then
        // suppress overwrites of these fields for WRITE_SUPPRESS, so a
        // pre-apply poll snapshot can't revert the UI (Bug 3 — ADC mode set
        // sometimes does not stick over WiFi).
        if !payload.is_empty() {
            let ch = payload[0];
            if ch < 4 && cmd_id != bbp::CMD_OTA {
                let mark = match cmd_id {
                    bbp::CMD_SET_ADC_CONFIG if payload.len() >= 4 => {
                        if let Ok(mut ds) = self.device_state.lock() {
                            if let Some(cur) = ds.channels.get_mut(ch as usize) {
                                cur.adc_mux = payload[1];
                                cur.adc_range = payload[2];
                                cur.adc_rate = payload[3];
                            }
                        }
                        true
                    }
                    bbp::CMD_SET_CH_FUNC if payload.len() >= 2 => {
                        if let Ok(mut ds) = self.device_state.lock() {
                            if let Some(cur) = ds.channels.get_mut(ch as usize) {
                                cur.function = payload[1];
                            }
                        }
                        true
                    }
                    bbp::CMD_SET_DAC_CODE | bbp::CMD_SET_DAC_VOLTAGE | bbp::CMD_SET_DAC_CURRENT => {
                        true
                    }
                    _ => false,
                };
                if mark {
                    if let Ok(mut writes) = self.recent_writes.lock() {
                        writes[ch as usize] = Some(Instant::now());
                    }
                }
            }
        }

        let t = self.transport.lock().await;
        match t.as_ref() {
            Some(transport) => {
                if !transport.is_connected() {
                    return Err(anyhow!("Not connected"));
                }
                transport.send_command(cmd_id, payload).await
            }
            None => Err(anyhow!("Not connected")),
        }
    }

    /// Get current device state.
    pub fn get_device_state(&self) -> DeviceState {
        self.device_state
            .lock()
            .unwrap_or_else(|e| {
                log::warn!(
                    "ConnectionStatus mutex poisoned — recovering stale state: {}",
                    e
                );
                e.into_inner()
            })
            .clone()
    }

    /// Get current connection status.
    pub fn get_connection_status(&self) -> ConnectionStatus {
        self.connection_status
            .lock()
            .unwrap_or_else(|e| {
                log::warn!(
                    "ConnectionStatus mutex poisoned — recovering stale state: {}",
                    e
                );
                e.into_inner()
            })
            .clone()
    }

    /// Start the status polling loop.
    fn start_polling(&self, app: AppHandle) {
        // Reset shutdown flag for this new connection
        self.poll_shutdown.store(false, Ordering::Release);
        let shutdown = self.poll_shutdown.clone();
        let transport = self.transport.clone();
        let device_state = self.device_state.clone();
        let connection_status = self.connection_status.clone();
        let recent_writes = self.recent_writes.clone();
        let active_slots = self.active_slots.clone();

        // ── IO keep-alive timer ───────────────────────────────────────────────
        // Every 2 s, re-issue CMD_IO_CLAIM for the union of slots the frontend
        // has registered via `io_claim`. Lease is renewed to 30 s on each tick.
        // If `active_slots` is empty the tick is a no-op (no wire traffic).
        // The task exits when the shutdown flag is set (same flag as the poll loop).
        {
            let shutdown_ka = shutdown.clone();
            let transport_ka = transport.clone();
            let active_slots_ka = active_slots.clone();
            tokio::spawn(async move {
                loop {
                    tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                    if shutdown_ka.load(Ordering::Acquire) {
                        break;
                    }
                    let slots: Vec<u8> = active_slots_ka
                        .lock()
                        .map(|s| s.iter().copied().collect())
                        .unwrap_or_default();
                    if slots.is_empty() {
                        continue;
                    }
                    // Build CMD_IO_CLAIM payload:
                    // n_slots(u8), slots(u8[n]), lease_ms(u32 LE), purpose_tag(u32 LE)
                    let mut payload = Vec::with_capacity(1 + slots.len() + 8);
                    payload.push(slots.len() as u8);
                    payload.extend_from_slice(&slots);
                    payload.extend_from_slice(&30_000_u32.to_le_bytes()); // 30 s lease
                    payload.extend_from_slice(&0_u32.to_le_bytes()); // purpose_tag = 0 for renewal
                    let t = transport_ka.lock().await;
                    if let Some(tr) = t.as_ref() {
                        if tr.is_connected() {
                            match tr.send_command(bbp::CMD_IO_CLAIM, &payload).await {
                                Ok(rsp) => {
                                    let n = rsp.first().copied().unwrap_or(0) as usize;
                                    let rejected: Vec<u8> = slots
                                        .iter()
                                        .take(n)
                                        .enumerate()
                                        .filter_map(|(idx, slot)| {
                                            (rsp.get(1 + idx).copied() != Some(0)).then_some(*slot)
                                        })
                                        .collect();
                                    if !rejected.is_empty() {
                                        log::warn!(
                                            "IO keep-alive dropped rejected slots: {:?}",
                                            rejected
                                        );
                                        if let Ok(mut active) = active_slots_ka.lock() {
                                            for slot in rejected {
                                                active.remove(&slot);
                                            }
                                        }
                                    }
                                }
                                Err(e) => {
                                    log::warn!("IO keep-alive renewal failed: {}", e);
                                }
                            }
                        }
                    }
                }
            });
        }

        let ota_in_progress = self.ota_in_progress.clone();

        tokio::spawn(async move {
            // Determine poll interval based on transport type
            // Determine poll interval based on transport type.
            // HTTP uses 300 ms (HTTP round-trips are slower but latency tolerance is higher).
            // USB uses 1000 ms: 200 ms was too aggressive — it competed with HAT/Overview
            // polls for the single-client BBP mutex and caused spurious disconnects.
            let poll_ms = {
                let t = transport.lock().await;
                match t.as_ref() {
                    Some(tr) if tr.transport_name() == "HTTP" => 300,
                    _ => 1000,
                }
            };

            let mut consecutive_failures: u32 = 0;
            // Raised from 3 to 6: a short burst of BBP contention at tab-mount time
            // could cause 3 consecutive timeouts even with the device fully connected.
            // 6 consecutive failures (~20-30 s) is still fast enough for genuine
            // disconnections while surviving a noisy startup.
            const MAX_RETRIES: u32 = 6;
            // During OTA, the device is extremely busy and may ignore HTTP requests
            // for up to 60-90 seconds. We increase the threshold to prevent
            // dropping the connection UI (M19).
            const MAX_RETRIES_OTA: u32 = 300; // ~90 s at 300 ms poll

            // Edge-detect channel_alert transitions (Bug Issue 5 — AIO_SC diag).
            // Log only the bits that went 0 -> 1 since last poll to avoid spam.
            let mut last_ch_alert: [u16; 4] = [0; 4];

            loop {
                tokio::time::sleep(std::time::Duration::from_millis(poll_ms)).await;

                // Check shutdown flag (set by disconnect())
                if shutdown.load(Ordering::Acquire) {
                    log::info!("Poll loop: shutdown signal received, exiting");
                    break;
                }

                // Check connectivity and poll status while holding the lock
                let result = {
                    let t = transport.lock().await;
                    match t.as_ref() {
                        Some(tr) if tr.is_connected() => tr.get_status().await,
                        _ => break, // Disconnected
                    }
                };

                match result {
                    Ok(mut state) => {
                        consecutive_failures = 0;
                        // ... (rest of Ok branch)

                        // Bug 3 suppression: for any channel with a recent
                        // write (CMD_SET_ADC_CONFIG / CMD_SET_CH_FUNC / DAC),
                        // preserve the previously emitted adc-config and
                        // function values rather than letting a pre-apply
                        // poll snapshot revert the user's change. The window
                        // (WRITE_SUPPRESS) covers HTTP latency + firmware
                        // command-queue drain.
                        let prev_state = device_state
                            .lock()
                            .ok()
                            .map(|g| g.clone())
                            .unwrap_or_default();
                        if let Ok(mut writes) = recent_writes.lock() {
                            let now = Instant::now();
                            for ch in 0..4 {
                                let suppress = matches!(
                                    writes[ch],
                                    Some(t) if now.duration_since(t) < WRITE_SUPPRESS
                                );
                                if suppress {
                                    if let (Some(prev), Some(cur)) =
                                        (prev_state.channels.get(ch), state.channels.get_mut(ch))
                                    {
                                        cur.function = prev.function;
                                        cur.adc_mux = prev.adc_mux;
                                        cur.adc_range = prev.adc_range;
                                        cur.adc_rate = prev.adc_rate;
                                    }
                                } else if writes[ch].is_some() {
                                    writes[ch] = None;
                                }
                            }
                        }
                        // Edge-log channel_alert rising bits (Issue 5 AIO_SC diag).
                        // Decode rising bits into names per ad74416h.h:294-300.
                        const CHANNEL_ALERT_BITS: &[(u16, &str)] = &[
                            (0x0001, "DIN_SC"),
                            (0x0002, "DIN_OC"),
                            (0x0004, "DO_SC"),
                            (0x0008, "DO_TIMEOUT"),
                            (0x0010, "AIO_SC"),
                            (0x0020, "AIO_OC"),
                            (0x0040, "VIOUT_SHUTDOWN"),
                        ];
                        for (i, ch) in state.channels.iter().enumerate().take(4) {
                            let rising = ch.channel_alert & !last_ch_alert[i];
                            if rising != 0 {
                                let names: Vec<&str> =
                                    CHANNEL_ALERT_BITS
                                        .iter()
                                        .filter_map(|(bit, name)| {
                                            if rising & bit != 0 {
                                                Some(*name)
                                            } else {
                                                None
                                            }
                                        })
                                        .collect();
                                let names_joined = if names.is_empty() {
                                    "unknown".to_string()
                                } else {
                                    names.join(",")
                                };
                                log::warn!(
                                    "[faults] ch{} rising=0x{:04X} ({}) full=0x{:04X}",
                                    i,
                                    rising,
                                    names_joined,
                                    ch.channel_alert
                                );
                            }
                            last_ch_alert[i] = ch.channel_alert;
                        }
                        if let Ok(mut ds) = device_state.lock() {
                            *ds = state.clone();
                        }
                        let _ = app.emit("device-state", &state);
                    }
                    Err(e) => {
                        consecutive_failures += 1;
                        let in_ota = ota_in_progress.load(Ordering::Acquire);
                        let threshold = if in_ota { MAX_RETRIES_OTA } else { MAX_RETRIES };

                        log::warn!(
                            "Status poll failed (attempt {}/{}): {}{}",
                            consecutive_failures,
                            threshold,
                            e,
                            if in_ota { " (OTA mode active)" } else { "" }
                        );

                        if consecutive_failures >= threshold {
                            log::error!(
                                "Status poll failed {} consecutive times, marking disconnected",
                                threshold
                            );
                            break;
                        }
                        // Wait 1 second before retrying
                        tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                        continue;
                    }
                }
            }

            // Mark as disconnected
            if let Ok(mut status) = connection_status.lock() {
                if status.mode != ConnectionMode::Disconnected {
                    status.mode = ConnectionMode::Disconnected;
                    status.port_or_url.clear();
                    let _ = app.emit("connection-status", &*status);
                }
            }
        });
    }

    /// Discover available devices.
    pub async fn discover(&self) -> Vec<DiscoveredDevice> {
        discovery::discover_all().await
    }

    /// Whether a connection attempt is currently in progress.
    pub fn is_connecting(&self) -> bool {
        self.connecting.load(Ordering::Relaxed)
    }

    /// Whether a transport is currently active.
    pub fn is_connected(&self) -> bool {
        self.connection_status
            .lock()
            .map_or(false, |s| s.mode != ConnectionMode::Disconnected)
    }

    /// Discover devices, emitting a "device-found" Tauri event for each one
    /// as soon as it is confirmed so the UI can update immediately.
    pub async fn discover_streaming(&self, app: &AppHandle) -> Vec<DiscoveredDevice> {
        // Hold the scanning flag only for the USB portion — the USB watcher must
        // not probe the same serial port simultaneously. The HTTP scan doesn't
        // touch serial ports so the watcher can run freely during it (the HTTP
        // scan takes ~23 s; suppressing the watcher that long delays USB discovery).
        self.scanning.store(true, Ordering::Relaxed);
        let app1 = app.clone();
        let usb_devices = tokio::task::spawn_blocking(move || {
            discovery::discover_usb_streaming(move |dev| {
                let _ = app1.emit("device-found", &dev);
            })
        })
        .await
        .unwrap_or_default();
        // USB scan complete — release the lock so the watcher can probe during HTTP.
        self.scanning.store(false, Ordering::Relaxed);

        let app2 = app.clone();
        let http_devices =
            discovery::discover_http_streaming(move |dev| {
                let _ = app2.emit("device-found", &dev);
            })
            .await;

        let mut all = usb_devices;
        all.extend(http_devices);
        all
    }

    /// Spawn a background task that polls Espressif USB ports every 2 s and
    /// emits "device-found" for each newly confirmed BugBuster port.
    /// Polling pauses (and the confirmed-port set is cleared) while connected,
    /// so ports are re-probed cleanly after a disconnect / board reboot.
    pub fn start_usb_watcher(&self, app: AppHandle) {
        let mgr = self.clone();
        // tauri::async_runtime::spawn is used instead of tokio::spawn because
        // start_usb_watcher is called from the Tauri .setup() hook, which runs on
        // the main UI thread before the Tokio runtime is active. Tauri's own
        // runtime handle is always valid at that point.
        tauri::async_runtime::spawn(async move {
            let mut confirmed: HashSet<String> = HashSet::new();
            loop {
                tokio::time::sleep(Duration::from_secs(2)).await;

                if mgr.is_connected() {
                    // Already connected — reset so ports are re-probed after the next disconnect.
                    confirmed.clear();
                    continue;
                }
                if mgr.is_connecting() {
                    // A connection attempt is in progress on one of the ports.
                    // Skip probing to avoid corrupting the BBP handshake.
                    continue;
                }
                if mgr.scanning.load(Ordering::Relaxed) {
                    // An active scan (discover_streaming) is already probing USB
                    // ports. Skip this cycle to prevent two concurrent probes on
                    // the same port from corrupting each other's \r\n + MAGIC stream.
                    continue;
                }

                // Enumerate candidates (non-blocking list — no I/O on ports).
                let candidates = tokio::task::spawn_blocking(|| {
                    discovery::espressif_port_candidates()
                })
                .await
                .unwrap_or_default();

                // Drop entries for ports that have disappeared.
                let names: HashSet<_> = candidates.iter().map(|(n, _)| n.clone()).collect();
                confirmed.retain(|p| names.contains(p));

                // Probe each port not yet confirmed (board may still be booting).
                for (port_name, serial_number) in candidates {
                    if confirmed.contains(&port_name) {
                        continue;
                    }
                    log::info!("[USB watcher] probing {} (waiting for board to boot)...", port_name);
                    let pn = port_name.clone();
                    let app2 = app.clone();
                    if let Ok(Some(dev)) = tokio::task::spawn_blocking(move || {
                        discovery::probe_usb_port(&pn, serial_number)
                    })
                    .await
                    {
                        log::info!("[USB watcher] ✓ {} confirmed", dev.address);
                        confirmed.insert(dev.address.clone());
                        let _ = app2.emit("device-found", &dev);
                    } else {
                        log::info!("[USB watcher] {} not ready yet, retrying in 2 s", port_name);
                    }
                }
            }
        });
    }

    /// Get HTTP base URL if connected via HTTP transport.
    pub async fn get_base_url(&self) -> Option<String> {
        let t = self.transport.lock().await;
        t.as_ref().and_then(|tr| tr.base_url())
    }

    /// Get device info from connection handshake.
    pub async fn get_device_info(&self) -> Option<DeviceInfo> {
        let status = self.connection_status.lock().ok()?;
        status.device_info.clone()
    }

    /// Normalise a MAC address for use as a pairing-store key.
    ///
    /// USB handshake formats MAC as uppercase (`D0:CF:...`), firmware
    /// `/api/device/info` emits lowercase (`d0:cf:...`). Without
    /// normalisation, tokens saved over USB never match HTTP lookups.
    fn normalize_mac(mac: &str) -> String {
        mac.to_ascii_lowercase()
    }

    /// Returns the path to the token storage file.
    fn tokens_path(app: &AppHandle) -> Option<std::path::PathBuf> {
        app.path()
            .app_data_dir()
            .ok()
            .map(|d| d.join("tokens.json"))
    }

    /// Load tokens from tokens.json into the in-memory cache.
    pub fn load_tokens(&self, app: &AppHandle) {
        let Some(path) = Self::tokens_path(app) else {
            log::warn!("load_tokens: could not resolve app data dir");
            return;
        };
        if !path.exists() {
            log::info!("load_tokens: no tokens.json found — fresh install");
            return;
        }
        match std::fs::read_to_string(&path) {
            Ok(content) => match serde_json::from_str::<HashMap<String, String>>(&content) {
                Ok(map) => {
                    let n = map.len();
                    if let Ok(mut tokens) = self.tokens.lock() {
                        *tokens = map;
                    }
                    log::info!("load_tokens: loaded {} token(s) from tokens.json", n);
                }
                Err(e) => log::warn!("load_tokens: could not parse tokens.json: {}", e),
            },
            Err(e) => log::warn!("load_tokens: could not read tokens.json: {}", e),
        }
    }

    /// Save a token to the in-memory cache and persist the whole map to tokens.json.
    pub fn save_token(&self, mac: String, token: String, app: &AppHandle) {
        let mac = Self::normalize_mac(&mac);
        if let Ok(mut tokens) = self.tokens.lock() {
            tokens.insert(mac.clone(), token);
            if let Some(path) = Self::tokens_path(app) {
                // Ensure parent directory exists (first run).
                if let Some(parent) = path.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                match serde_json::to_string(&*tokens) {
                    Ok(json) => {
                        if let Err(e) = std::fs::write(&path, json) {
                            log::warn!("save_token: could not write tokens.json: {}", e);
                        } else {
                            log::info!("save_token: persisted token for {} to tokens.json", mac);
                        }
                    }
                    Err(e) => log::warn!("save_token: could not serialise tokens: {}", e),
                }
            }
        }
    }

    /// Get a stored token for a device MAC.
    ///
    /// Checks the in-memory cache first (populated by load_tokens on startup
    /// and save_token on each USB pairing). If the cache is empty (load_tokens
    /// was not called yet), falls back to reading tokens.json directly.
    pub fn get_token(&self, mac: &str, app: &AppHandle) -> Option<String> {
        let key = Self::normalize_mac(mac);

        // Check in-memory cache.
        if let Ok(tokens) = self.tokens.lock() {
            if let Some(t) = tokens.get(&key) {
                return Some(t.clone());
            }
            // Case-insensitive fallback for tokens saved before normalisation fix.
            if let Some((_, v)) = tokens.iter().find(|(k, _)| k.to_ascii_lowercase() == key) {
                return Some(v.clone());
            }
            // If cache is non-empty and key not found, no token exists.
            if !tokens.is_empty() {
                log::warn!("get_token: no token for {} — device needs USB pairing", key);
                return None;
            }
        }

        // Cache is empty — load_tokens may not have been called yet. Try file directly.
        if let Some(path) = Self::tokens_path(app) {
            if let Ok(content) = std::fs::read_to_string(&path) {
                if let Ok(map) = serde_json::from_str::<HashMap<String, String>>(&content) {
                    // Populate cache for future calls.
                    if let Ok(mut tokens) = self.tokens.lock() {
                        *tokens = map;
                        if let Some(t) = tokens.get(&key) {
                            return Some(t.clone());
                        }
                    }
                }
            }
        }

        log::warn!("get_token: no token for {} — device needs USB pairing", key);
        None
    }
}
