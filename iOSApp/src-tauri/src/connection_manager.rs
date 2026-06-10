use anyhow::{anyhow, Result};
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager};
use tokio::sync::Mutex as TokioMutex;

use crate::bbp;
use crate::discovery;
use crate::http_transport::HttpTransport;
use crate::state::*;
use crate::transport::Transport;

const WRITE_SUPPRESS: Duration = Duration::from_millis(1500);

#[derive(Clone)]
pub struct ConnectionManager {
    transport: Arc<TokioMutex<Option<Box<dyn Transport>>>>,
    device_state: Arc<StdMutex<DeviceState>>,
    connection_status: Arc<StdMutex<ConnectionStatus>>,
    poll_shutdown: Arc<AtomicBool>,
    tokens: Arc<StdMutex<HashMap<String, String>>>,
    recent_writes: Arc<StdMutex<[Option<Instant>; 4]>>,
    pub active_slots: Arc<StdMutex<HashSet<u8>>>,
    ota_in_progress: Arc<AtomicBool>,
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
        }
    }

    pub fn set_ota_in_progress(&self, in_progress: bool) {
        log::info!("OTA in progress: {}", in_progress);
        self.ota_in_progress.store(in_progress, Ordering::Release);
    }

    pub fn ota_guard(&self) -> OtaGuard {
        self.set_ota_in_progress(true);
        OtaGuard { mgr: self.clone() }
    }

    pub async fn connect(&self, device_id: &str, app: &AppHandle) -> Result<()> {
        self.disconnect(app).await?;

        if device_id.starts_with("http:") {
            let base_url = &device_id[5..];
            self.connect_http(base_url, app).await
        } else {
            Err(anyhow!("USB connection not supported on iOS: {}", device_id))
        }
    }

    async fn connect_http(&self, base_url: &str, app: &AppHandle) -> Result<()> {
        if self.tokens.lock().map(|t| t.is_empty()).unwrap_or(false) {
            self.load_tokens(app);
        }

        let candidate_tokens: Vec<String> = self
            .tokens
            .lock()
            .map(|t| t.values().cloned().collect())
            .unwrap_or_default();
        let (mut transport, mac) = HttpTransport::connect(base_url, &candidate_tokens).await?;

        if mac.starts_with("legacy:") {
            let _ = app.emit(
                "firmware-update-required",
                &serde_json::json!({
                    "url": base_url,
                    "reason": "macAddress missing from /api/device/info — update to ESP firmware >= 3.0.0",
                }),
            );
        }

        let admin_token = self.get_token(&mac, app);
        if admin_token.is_none() {
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
        transport.set_app_handle(app.clone());

        let mut device_info = DeviceInfo::default();
        device_info.mac_address = Some(mac);

        {
            let mut t = self.transport.lock().await;
            *t = Some(Box::new(transport));
        }

        {
            let mut status = self.connection_status.lock().unwrap();
            status.mode = ConnectionMode::Http;
            status.port_or_url = base_url.to_string();
            status.device_info = Some(device_info);
            status.admin_token = Some(token);
            status.la_selector = None;
        }

        let status = self.connection_status.lock().unwrap().clone();
        let _ = app.emit("connection-status", &status);

        self.start_polling(app.clone());

        log::info!("Connected via HTTP to {}", base_url);
        Ok(())
    }

    pub async fn disconnect(&self, app: &AppHandle) -> Result<()> {
        self.poll_shutdown.store(true, Ordering::Release);

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

        if let Ok(mut slots) = self.active_slots.lock() {
            slots.clear();
        }

        let _ = app.emit(
            "device-disconnected",
            &serde_json::json!({"reason": "manual", "stream_running": false}),
        );

        Ok(())
    }

    pub async fn send_command(&self, cmd_id: u8, payload: &[u8]) -> Result<Vec<u8>> {
        if !payload.is_empty() {
            let ch = payload[0];
            if ch < 4 && cmd_id != bbp::CMD_OTA {
                let mark = match cmd_id {
                    bbp::CMD_SET_ADC_CONFIG if payload.len() >= 4 => {
                        if let Ok(mut ds) = self.device_state.lock() {
                            let cur = &mut ds.channels[ch as usize];
                            cur.adc_mux = payload[1];
                            cur.adc_range = payload[2];
                            cur.adc_rate = payload[3];
                        }
                        true
                    }
                    bbp::CMD_SET_CH_FUNC if payload.len() >= 2 => {
                        if let Ok(mut ds) = self.device_state.lock() {
                            ds.channels[ch as usize].function = payload[1];
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

    pub fn get_device_state(&self) -> DeviceState {
        self.device_state.lock().unwrap().clone()
    }

    pub fn get_connection_status(&self) -> ConnectionStatus {
        self.connection_status.lock().unwrap().clone()
    }

    fn start_polling(&self, app: AppHandle) {
        self.poll_shutdown.store(false, Ordering::Release);
        let shutdown = self.poll_shutdown.clone();
        let transport = self.transport.clone();
        let device_state = self.device_state.clone();
        let connection_status = self.connection_status.clone();
        let recent_writes = self.recent_writes.clone();
        let active_slots = self.active_slots.clone();

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
                    let mut payload = Vec::with_capacity(1 + slots.len() + 8);
                    payload.push(slots.len() as u8);
                    payload.extend_from_slice(&slots);
                    payload.extend_from_slice(&30_000_u32.to_le_bytes());
                    payload.extend_from_slice(&0_u32.to_le_bytes());
                    let t = transport_ka.lock().await;
                    if let Some(tr) = t.as_ref() {
                        if tr.is_connected() {
                            let _ = tr.send_command(bbp::CMD_IO_CLAIM, &payload).await;
                        }
                    }
                }
            });
        }

        let ota_in_progress = self.ota_in_progress.clone();

        tokio::spawn(async move {
            let mut consecutive_failures: u32 = 0;
            const MAX_RETRIES: u32 = 3;
            const MAX_RETRIES_OTA: u32 = 300;

            loop {
                tokio::time::sleep(std::time::Duration::from_millis(300)).await;

                if shutdown.load(Ordering::Acquire) {
                    break;
                }

                let result = {
                    let t = transport.lock().await;
                    match t.as_ref() {
                        Some(tr) if tr.is_connected() => tr.get_status().await,
                        _ => break,
                    }
                };

                match result {
                    Ok(mut state) => {
                        consecutive_failures = 0;
                        let prev_state = device_state.lock().ok().map(|g| g.clone()).unwrap_or_default();
                        if let Ok(mut writes) = recent_writes.lock() {
                            let now = Instant::now();
                            for ch in 0..4 {
                                let suppress = matches!(
                                    writes[ch],
                                    Some(t) if now.duration_since(t) < WRITE_SUPPRESS
                                );
                                if suppress {
                                    state.channels[ch].function = prev_state.channels[ch].function;
                                    state.channels[ch].adc_mux = prev_state.channels[ch].adc_mux;
                                    state.channels[ch].adc_range = prev_state.channels[ch].adc_range;
                                    state.channels[ch].adc_rate = prev_state.channels[ch].adc_rate;
                                } else if writes[ch].is_some() {
                                    writes[ch] = None;
                                }
                            }
                        }
                        if let Ok(mut ds) = device_state.lock() {
                            *ds = state.clone();
                        }
                        let _ = app.emit("device-state", &state);
                    }
                    Err(_e) => {
                        consecutive_failures += 1;
                        let in_ota = ota_in_progress.load(Ordering::Acquire);
                        let threshold = if in_ota { MAX_RETRIES_OTA } else { MAX_RETRIES };

                        if consecutive_failures >= threshold {
                            break;
                        }
                        tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                        continue;
                    }
                }
            }

            if let Ok(mut status) = connection_status.lock() {
                if status.mode != ConnectionMode::Disconnected {
                    status.mode = ConnectionMode::Disconnected;
                    status.port_or_url.clear();
                    let _ = app.emit("connection-status", &*status);
                }
            }
        });
    }

    pub async fn discover(&self) -> Vec<DiscoveredDevice> {
        discovery::discover_all().await
    }

    pub async fn get_base_url(&self) -> Option<String> {
        let t = self.transport.lock().await;
        t.as_ref().and_then(|tr| tr.base_url())
    }

    pub async fn get_device_info(&self) -> Option<DeviceInfo> {
        let status = self.connection_status.lock().ok()?;
        status.device_info.clone()
    }

    fn normalize_mac(mac: &str) -> String {
        mac.to_ascii_lowercase()
    }

    fn tokens_path(app: &AppHandle) -> Option<std::path::PathBuf> {
        app.path().app_data_dir().ok().map(|d| d.join("tokens.json"))
    }

    pub fn load_tokens(&self, app: &AppHandle) {
        let Some(path) = Self::tokens_path(app) else { return; };
        if !path.exists() { return; }
        if let Ok(content) = std::fs::read_to_string(&path) {
            if let Ok(map) = serde_json::from_str::<HashMap<String, String>>(&content) {
                if let Ok(mut tokens) = self.tokens.lock() {
                    *tokens = map;
                }
            }
        }
    }

    pub fn save_token(&self, mac: String, token: String, app: &AppHandle) {
        let mac = Self::normalize_mac(&mac);
        if let Ok(mut tokens) = self.tokens.lock() {
            tokens.insert(mac.clone(), token);
            if let Some(path) = Self::tokens_path(app) {
                if let Some(parent) = path.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                if let Ok(json) = serde_json::to_string(&*tokens) {
                    let _ = std::fs::write(&path, json);
                }
            }
        }
    }

    pub fn get_token(&self, mac: &str, _app: &AppHandle) -> Option<String> {
        let key = Self::normalize_mac(mac);
        if let Ok(tokens) = self.tokens.lock() {
            if let Some(t) = tokens.get(&key) {
                return Some(t.clone());
            }
        }
        None
    }
}
