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
        let transport = tokio::task::spawn_blocking(move || {
            UsbTransport::connect(&port_name_owned, event_tx_clone)
        }).await??;
        log::info!("USB handshake completed successfully");

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
            let mut status = self.connection_status.lock().unwrap();
            status.mode = ConnectionMode::Usb;
            status.port_or_url = port_name.to_string();
            status.device_info = device_info;
        }

        let status = self.connection_status.lock().unwrap().clone();
        let _ = app.emit("connection-status", &status);

        // Spawn event listener for USB stream data
        let app_handle = app.clone();
        tokio::spawn(async move {
            while let Some(msg) = event_rx.recv().await {
                match msg.cmd_id {
                    bbp::EVT_ADC_DATA => {
                        let _ = app_handle.emit("adc-stream", &msg.payload);
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
            let mut status = self.connection_status.lock().unwrap();
            status.mode = ConnectionMode::Http;
            status.port_or_url = base_url.to_string();
            status.device_info = None;
        }

        let status = self.connection_status.lock().unwrap().clone();
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
            let mut status = self.connection_status.lock().unwrap();
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
        self.device_state.lock().unwrap().clone()
    }

    /// Get current connection status.
    pub fn get_connection_status(&self) -> ConnectionStatus {
        self.connection_status.lock().unwrap().clone()
    }

    /// Start the status polling loop.
    fn start_polling(&self, app: AppHandle) {
        let transport = self.transport.clone();
        let device_state = self.device_state.clone();
        let connection_status = self.connection_status.clone();

        tokio::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_millis(200)).await;

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
                        {
                            let mut ds = device_state.lock().unwrap();
                            *ds = state.clone();
                        }
                        let _ = app.emit("device-state", &state);
                    }
                    Err(e) => {
                        log::warn!("Status poll failed: {}", e);
                    }
                }
            }

            // Mark as disconnected
            let mut status = connection_status.lock().unwrap();
            if status.mode != ConnectionMode::Disconnected {
                status.mode = ConnectionMode::Disconnected;
                status.port_or_url.clear();
                let _ = app.emit("connection-status", &*status);
            }
        });
    }

    /// Discover available devices.
    pub async fn discover(&self) -> Vec<DiscoveredDevice> {
        discovery::discover_all().await
    }
}
