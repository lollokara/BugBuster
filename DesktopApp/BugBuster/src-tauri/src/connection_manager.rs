// =============================================================================
// connection_manager.rs - Central connection and state management
//
// Owns the active transport, polls device state, emits Tauri events.
// Uses tokio::sync::Mutex for the transport since we need to hold it across await.
// =============================================================================

use std::sync::{Arc, Mutex as StdMutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::collections::HashMap;
use std::fs::OpenOptions;

use anyhow::{anyhow, Result};
use tauri::{AppHandle, Emitter, Manager};
use tokio::sync::{mpsc, Mutex as TokioMutex};

use crate::bbp::{self, Message};
use crate::discovery;
use crate::http_transport::HttpTransport;
use crate::state::*;
use crate::transport::Transport;
use crate::usb_transport::UsbTransport;

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
}

impl ConnectionManager {
    pub fn new() -> Self {
        Self {
            transport: Arc::new(TokioMutex::new(None)),
            device_state: Arc::new(StdMutex::new(DeviceState::default())),
            connection_status: Arc::new(StdMutex::new(ConnectionStatus::default())),
            poll_shutdown: Arc::new(AtomicBool::new(false)),
            tokens: Arc::new(StdMutex::new(HashMap::new())),
        }
    }

    /// Connect to a device by its discovery ID.
    pub async fn connect(&self, device_id: &str, app: &AppHandle) -> Result<()> {
        // la_selector stays None → DeviceSelector::Any.
        // The ESP32 CDC port and the RP2040 vendor-bulk interface are separate USB
        // devices with independent serial numbers; the CDC serial cannot be used to
        // identify the paired RP2040. Use Any (first matching VID/PID) for now.
        let la_selector: Option<crate::la_usb::DeviceSelector> = None;

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

    async fn connect_usb(&self, port_name: &str, la_selector: Option<crate::la_usb::DeviceSelector>, app: &AppHandle) -> Result<()> {
        let (_event_tx, mut event_rx) = mpsc::unbounded_channel::<Message>();

        log::info!("Opening USB port: {}", port_name);
        let port_name_owned = port_name.to_string();
        let event_tx_clone = _event_tx.clone();
        let first_attempt = {
            let pn = port_name_owned.clone();
            let tx = event_tx_clone.clone();
            tokio::task::spawn_blocking(move || {
                UsbTransport::connect(&pn, tx)
            }).await?
        };
        let transport = match first_attempt {
            Ok(t) => t,
            Err(e) => {
                log::warn!("USB connect failed, retrying in 2s: {}", e);
                tokio::time::sleep(std::time::Duration::from_secs(2)).await;
                let pn = port_name_owned.clone();
                let tx = event_tx_clone.clone();
                tokio::task::spawn_blocking(move || {
                    UsbTransport::connect(&pn, tx)
                }).await??
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
                let _ = app.emit("version-mismatch", &serde_json::json!({
                    "device_version": h.proto_version,
                    "expected_version": bbp::PROTO_VERSION,
                }));
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
                    let token = String::from_utf8_lossy(&rsp[1..1+len]).to_string();
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
            let mut status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner());
            status.mode = ConnectionMode::Usb;
            status.port_or_url = port_name.to_string();
            status.device_info = device_info;
            status.admin_token = admin_token;
            status.la_selector = la_selector;
        }

        let status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner()).clone();
        let _ = app.emit("connection-status", &status);

        // Spawn event listener for USB stream data
        let app_handle = app.clone();
        tokio::spawn(async move {
            let mut last_adc_emit = std::time::Instant::now();
            let mut adc_buffer: Vec<u8> = Vec::new();
            let emit_interval = std::time::Duration::from_millis(33); // ~30 Hz

            loop {
                // Use a short timeout so we can flush the buffer periodically
                match tokio::time::timeout(
                    std::time::Duration::from_millis(10),
                    event_rx.recv()
                ).await {
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
                                                let count = u16::from_le_bytes([msg.payload[5], msg.payload[6]]) as usize;
                                                let mask = msg.payload[0];
                                                let num_ch = (0..4).filter(|b| mask & (1 << b) != 0).count();
                                                let data_len = count * num_ch * 3;
                                                let data_end = 7 + data_len;
                                                if msg.payload.len() >= data_end {
                                                    use std::io::Write;
                                                    let _ = rec.writer.write_all(&msg.payload[7..data_end]);
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

        let (mut transport, mac) = HttpTransport::connect(base_url).await?;

        // 2. Check for Pairing (Admin Token)
        let admin_token = self.get_token(&mac);
        if admin_token.is_none() {
            log::warn!("HTTP connection to {} (MAC: {}) requires pairing via USB", base_url, mac);
            let _ = app.emit("pairing-required", &serde_json::json!({
                "mac": mac,
                "url": base_url,
            }));
            return Err(anyhow!("Pairing required: connect via USB once to authorize this computer"));
        }

        let token = admin_token.unwrap();
        transport.set_admin_token(&token)?;

        let mut device_info = DeviceInfo::default();
        device_info.mac_address = Some(mac);

        {
            let mut t = self.transport.lock().await;
            *t = Some(Box::new(transport));
        }

        {
            let mut status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner());
            status.mode = ConnectionMode::Http;
            status.port_or_url = base_url.to_string();
            status.device_info = Some(device_info);
            status.admin_token = Some(token);
            status.la_selector = None; // Reset for WiFi
        }

        let status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner()).clone();
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
            let mut status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner());
            *status = ConnectionStatus::default();
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
            let _ = app.emit("la-stream-stopped", &serde_json::json!({"reason": "main_disconnect"}));
        }

        let _ = app.emit("device-disconnected", &serde_json::json!({"reason": "manual", "stream_running": false}));

        Ok(())
    }

    /// Send a command through the active transport.
    pub async fn send_command(&self, cmd_id: u8, payload: &[u8]) -> Result<Vec<u8>> {
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
        self.device_state.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// Get current connection status.
    pub fn get_connection_status(&self) -> ConnectionStatus {
        self.connection_status.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// Start the status polling loop.
    fn start_polling(&self, app: AppHandle) {
        // Reset shutdown flag for this new connection
        self.poll_shutdown.store(false, Ordering::Release);
        let shutdown = self.poll_shutdown.clone();
        let transport = self.transport.clone();
        let device_state = self.device_state.clone();
        let connection_status = self.connection_status.clone();

        tokio::spawn(async move {
            // Determine poll interval based on transport type
            let poll_ms = {
                let t = transport.lock().await;
                match t.as_ref() {
                    Some(tr) if tr.transport_name() == "HTTP" => 300,
                    _ => 200,
                }
            };

            let mut consecutive_failures: u32 = 0;
            const MAX_RETRIES: u32 = 3;

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
                    Ok(state) => {
                        consecutive_failures = 0;
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
                                let names: Vec<&str> = CHANNEL_ALERT_BITS
                                    .iter()
                                    .filter_map(|(bit, name)| if rising & bit != 0 { Some(*name) } else { None })
                                    .collect();
                                let names_joined = if names.is_empty() {
                                    "unknown".to_string()
                                } else {
                                    names.join(",")
                                };
                                log::warn!(
                                    "[faults] ch{} rising=0x{:04X} ({}) full=0x{:04X}",
                                    i, rising, names_joined, ch.channel_alert
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
                        log::warn!(
                            "Status poll failed (attempt {}/{}): {}",
                            consecutive_failures,
                            MAX_RETRIES,
                            e
                        );
                        if consecutive_failures >= MAX_RETRIES {
                            log::error!(
                                "Status poll failed {} consecutive times, marking disconnected",
                                MAX_RETRIES
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

    /// Load tokens from persistent storage.
    pub fn load_tokens(&self, app: &AppHandle) {
        if let Ok(app_dir) = app.path().app_data_dir() {
            let path = app_dir.join("tokens.json");
            if let Ok(content) = std::fs::read_to_string(path) {
                if let Ok(map) = serde_json::from_str::<HashMap<String, String>>(&content) {
                    if let Ok(mut tokens) = self.tokens.lock() {
                        *tokens = map;
                        log::info!("Loaded {} admin tokens from storage", tokens.len());
                    }
                }
            }
        }
    }

    /// Save a token to persistent storage.
    pub fn save_token(&self, mac: String, token: String, app: &AppHandle) {
        let mut map_clone = HashMap::new();
        if let Ok(mut tokens) = self.tokens.lock() {
            tokens.insert(mac, token);
            map_clone = tokens.clone();
        }

        if let Ok(app_dir) = app.path().app_data_dir() {
            let _ = std::fs::create_dir_all(&app_dir);
            let path = app_dir.join("tokens.json");

            let mut opts = OpenOptions::new();
            opts.create(true).write(true).truncate(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::OpenOptionsExt;
                opts.mode(0o600);
            }

            let file_result = opts.open(&path);

            if let Ok(file) = file_result {
                if let Ok(content) = serde_json::to_string(&map_clone) {
                    use std::io::Write;
                    let mut writer = std::io::BufWriter::new(file);
                    if let Err(e) = writer.write_all(content.as_bytes()) {
                        log::error!("Failed to write admin tokens: {}", e);
                    } else {
                        log::info!("Admin tokens persisted securely to storage");
                    }
                }
            } else if let Err(e) = file_result {
                log::error!("Failed to open admin tokens file for writing: {}", e);
            }
        }
    }

    /// Get a stored token for a device MAC.
    pub fn get_token(&self, mac: &str) -> Option<String> {
        self.tokens.lock().ok()?.get(mac).cloned()
    }
}
