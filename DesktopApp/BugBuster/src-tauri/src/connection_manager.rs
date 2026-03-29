// =============================================================================
// connection_manager.rs - Central connection and state management
//
// Owns the active transport, polls device state, emits Tauri events.
// Uses tokio::sync::Mutex for the transport since we need to hold it across await.
// =============================================================================

use std::sync::{Arc, Mutex as StdMutex};

use anyhow::{anyhow, Result};
use tauri::{AppHandle, Emitter};
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
}

impl ConnectionManager {
    pub fn new() -> Self {
        Self {
            transport: Arc::new(TokioMutex::new(None)),
            device_state: Arc::new(StdMutex::new(DeviceState::default())),
            connection_status: Arc::new(StdMutex::new(ConnectionStatus::default())),
        }
    }

    /// Connect to a device by its discovery ID.
    pub async fn connect(&self, device_id: &str, app: &AppHandle) -> Result<()> {
        self.disconnect().await?;

        if device_id.starts_with("usb:") {
            let port_name = &device_id[4..];
            self.connect_usb(port_name, app).await
        } else if device_id.starts_with("http:") {
            let base_url = &device_id[5..];
            self.connect_http(base_url, app).await
        } else {
            Err(anyhow!("Unknown device ID format: {}", device_id))
        }
    }

    async fn connect_usb(&self, port_name: &str, app: &AppHandle) -> Result<()> {
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
            ..Default::default()
        });

        {
            let mut t = self.transport.lock().await;
            *t = Some(Box::new(transport));
        }

        {
            let mut status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner());
            status.mode = ConnectionMode::Usb;
            status.port_or_url = port_name.to_string();
            status.device_info = device_info;
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
                                // Always keep the latest payload for display
                                adc_buffer = msg.payload.clone();

                                // Emit to "adc-stream-raw" for recording (every event)
                                let _ = app_handle.emit("adc-stream-raw", &msg.payload);

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
        let transport = HttpTransport::connect(base_url).await?;

        {
            let mut t = self.transport.lock().await;
            *t = Some(Box::new(transport));
        }

        {
            let mut status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner());
            status.mode = ConnectionMode::Http;
            status.port_or_url = base_url.to_string();
            status.device_info = None;
        }

        let status = self.connection_status.lock().unwrap_or_else(|e| e.into_inner()).clone();
        let _ = app.emit("connection-status", &status);

        self.start_polling(app.clone());

        log::info!("Connected via HTTP to {}", base_url);
        Ok(())
    }

    /// Disconnect the current transport.
    pub async fn disconnect(&self) -> Result<()> {
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
        let transport = self.transport.clone();
        let device_state = self.device_state.clone();
        let connection_status = self.connection_status.clone();

        tokio::spawn(async move {
            // Determine poll interval based on transport type
            let poll_ms = {
                let t = transport.lock().await;
                match t.as_ref() {
                    Some(tr) if tr.transport_name() == "HTTP" => 1000,
                    _ => 200,
                }
            };

            let mut consecutive_failures: u32 = 0;
            const MAX_RETRIES: u32 = 3;

            loop {
                tokio::time::sleep(std::time::Duration::from_millis(poll_ms)).await;

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
}
