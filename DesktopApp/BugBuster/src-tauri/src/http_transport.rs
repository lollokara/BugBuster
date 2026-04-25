// =============================================================================
// http_transport.rs - HTTP REST API transport (fallback when USB not available)
// =============================================================================

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use anyhow::{anyhow, Result};
use async_trait::async_trait;
use reqwest::Client;
use serde_json::Value;
use tauri::{AppHandle, Emitter};

use crate::bbp;
use crate::state::{ChannelState, DeviceState, DiagState};
use crate::transport::Transport;

fn encode_husb_current_code(max_current_a: f64) -> u8 {
    // HUSB238 current-code table in 0.5A..5.0A non-linear steps.
    const TABLE: [f64; 16] = [
        0.5, 0.7, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25, 3.5, 4.0, 4.5, 5.0,
    ];
    let mut best_idx = 0usize;
    let mut best_err = f64::INFINITY;
    for (i, v) in TABLE.iter().enumerate() {
        let err = (max_current_a - *v).abs();
        if err < best_err {
            best_err = err;
            best_idx = i;
        }
    }
    best_idx as u8
}

fn encode_husb_voltage_code(voltage_v: f64) -> u8 {
    // BBP currently treats these codes as informational only; keep closest match.
    if voltage_v >= 19.0 {
        6
    } else if voltage_v >= 17.0 {
        5
    } else if voltage_v >= 14.0 {
        4
    } else if voltage_v >= 11.0 {
        3
    } else if voltage_v >= 8.0 {
        2
    } else if voltage_v >= 4.0 {
        1
    } else {
        0
    }
}

pub struct HttpTransport {
    client: Client,      // Fast client for status polls and normal commands
    slow_client: Client, // Slow client for WiFi connect/scan (long-blocking)
    base_url: String,
    connected: AtomicBool,
    // Bug 2: HTTP scope streaming. The shared `scope_polling` flag controls a
    // background tokio task started by CMD_START_SCOPE_STREAM and stopped by
    // CMD_STOP_SCOPE_STREAM. The task GETs `/api/scope?since=<seq>` ~10 Hz and
    // emits the same `scope-data` event the USB transport emits, so the
    // existing frontend listener (parse_scope_event) works unchanged.
    scope_polling: Arc<AtomicBool>,
    app_handle: Option<AppHandle>,
}

impl HttpTransport {
    /// Connect to the device via HTTP. Verifies connectivity with /api/device/info.
    /// Returns (Self, mac_address) on success.
    pub async fn connect(base_url: &str) -> Result<(Self, String)> {
        let client = Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .pool_max_idle_per_host(4)
            .build()?;
        let slow_client = Client::builder()
            .timeout(std::time::Duration::from_secs(15))
            .build()?;

        // Verify the device is reachable
        let url = format!("{}/api/device/info", base_url);
        let resp = client.get(&url).send().await?;
        if !resp.status().is_success() {
            return Err(anyhow!("Device returned HTTP {}", resp.status()));
        }

        // Parse response to confirm it's a BugBuster and extract MAC
        let info: Value = resp.json().await?;
        if info.get("spiOk").is_none() {
            return Err(anyhow!("Not a BugBuster device"));
        }

        // Prefer macAddress, fall back to mac_address for snake_case
        // consistency.  ESP firmware ≥ 3.0.0 always emits one of these.
        // Older firmware omits the field — don't block the connection in
        // that case: warn and fall back to a host-URL sentinel so read-only
        // HTTP still works.  Any mutating call will then naturally surface
        // the `pairing-required` toast in `connection_manager::connect_http`
        // (no saved token can match a `legacy:<url>` key), which points the
        // user at the real fix: updating the firmware.
        let reported = info
            .get("macAddress")
            .or_else(|| info.get("mac_address"))
            .and_then(|v| v.as_str())
            .map(|s| s.to_string());

        let mac = match reported {
            Some(m) if !m.is_empty() && m != "00:00:00:00:00:00" => m,
            _ => {
                let sentinel = format!("legacy:{}", base_url);
                log::warn!(
                    "Device at {} did not report a MAC address over HTTP — \
                     using sentinel '{}' for pairing. Update to ESP firmware \
                     ≥ 3.0.0 so /api/device/info exposes `macAddress` for \
                     proper MAC-keyed pairing.",
                    base_url,
                    sentinel,
                );
                sentinel
            }
        };

        log::info!("HTTP transport connected to {} (MAC: {})", base_url, mac);

        let transport = Self {
            client,
            slow_client,
            base_url: base_url.to_string(),
            connected: AtomicBool::new(true),
            scope_polling: Arc::new(AtomicBool::new(false)),
            app_handle: None,
        };

        Ok((transport, mac))
    }

    /// Inject a Tauri AppHandle for emitting streaming events (e.g.
    /// `scope-data` from the HTTP scope polling task).
    pub fn set_app_handle(&mut self, app: AppHandle) {
        self.app_handle = Some(app);
    }

    /// Start the background scope polling task. Idempotent — repeat calls
    /// while polling is active are a no-op (Bug 2).
    fn start_scope_polling(&self) -> Result<()> {
        if self.scope_polling.swap(true, Ordering::AcqRel) {
            return Ok(()); // already running
        }
        let app = self
            .app_handle
            .clone()
            .ok_or_else(|| anyhow!("HttpTransport missing AppHandle for scope streaming"))?;
        let client = self.client.clone();
        let base_url = self.base_url.clone();
        let polling = self.scope_polling.clone();

        tokio::spawn(async move {
            let mut last_seq: i64 = -1;
            while polling.load(Ordering::Acquire) {
                tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                if !polling.load(Ordering::Acquire) {
                    break;
                }

                let since_param = if last_seq < 0 {
                    String::new()
                } else {
                    format!("?since={}", last_seq)
                };
                let url = format!("{}/api/scope{}", base_url, since_param);
                let resp = match client.get(&url).send().await {
                    Ok(r) => r,
                    Err(e) => {
                        log::warn!("scope poll request failed: {}", e);
                        continue;
                    }
                };
                if !resp.status().is_success() {
                    log::warn!("scope poll HTTP {}", resp.status());
                    continue;
                }
                let json: Value = match resp.json().await {
                    Ok(j) => j,
                    Err(e) => {
                        log::warn!("scope poll JSON parse failed: {}", e);
                        continue;
                    }
                };

                if let Some(seq) = json.get("seq").and_then(|v| v.as_i64()) {
                    last_seq = seq;
                }
                let samples = match json.get("samples").and_then(|v| v.as_array()) {
                    Some(s) => s,
                    None => continue,
                };

                let first_seq = last_seq
                    .saturating_sub(samples.len() as i64 - 1)
                    .max(0) as u32;
                for (i, bucket) in samples.iter().enumerate() {
                    let arr = match bucket.as_array() {
                        Some(a) if a.len() >= 13 => a,
                        _ => continue,
                    };
                    // Format: [t, ch0avg..ch3avg, ch0min,ch0max,ch1min,ch1max,...]
                    let t_ms = arr[0].as_f64().unwrap_or(0.0) as u32;
                    let avg = [
                        arr[1].as_f64().unwrap_or(0.0) as f32,
                        arr[2].as_f64().unwrap_or(0.0) as f32,
                        arr[3].as_f64().unwrap_or(0.0) as f32,
                        arr[4].as_f64().unwrap_or(0.0) as f32,
                    ];
                    let mut min = [0f32; 4];
                    let mut max = [0f32; 4];
                    for ch in 0..4 {
                        min[ch] = arr[5 + ch * 2].as_f64().unwrap_or(0.0) as f32;
                        max[ch] = arr[5 + ch * 2 + 1].as_f64().unwrap_or(0.0) as f32;
                    }

                    // Build EVT_SCOPE_DATA-compatible payload (parse_scope_event
                    // requires >=58 bytes; reads avg at pos 10/22/34/46 in
                    // 12-byte channel blocks of [avg f32, min f32, max f32]).
                    let mut payload = Vec::with_capacity(58);
                    let bucket_seq = first_seq + i as u32;
                    payload.extend_from_slice(&bucket_seq.to_le_bytes()); // 0..4
                    payload.extend_from_slice(&t_ms.to_le_bytes()); // 4..8
                    payload.extend_from_slice(&1u16.to_le_bytes()); // 8..10  count placeholder
                    for ch in 0..4 {
                        payload.extend_from_slice(&avg[ch].to_le_bytes()); // pos+0..pos+4
                        payload.extend_from_slice(&min[ch].to_le_bytes()); // pos+4..pos+8
                        payload.extend_from_slice(&max[ch].to_le_bytes()); // pos+8..pos+12
                    }
                    let _ = app.emit("scope-data", &payload);
                }
            }
            log::info!("scope polling task exited");
        });
        Ok(())
    }

    fn stop_scope_polling(&self) {
        self.scope_polling.store(false, Ordering::Release);
    }

    /// Set the admin token for future POST requests.
    pub fn set_admin_token(&mut self, token: &str) -> Result<()> {
        let mut headers = reqwest::header::HeaderMap::new();
        headers.insert(
            "X-BugBuster-Admin-Token",
            reqwest::header::HeaderValue::from_str(token)?,
        );

        self.client = Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .default_headers(headers.clone())
            .build()?;

        self.slow_client = Client::builder()
            .timeout(std::time::Duration::from_secs(15))
            .default_headers(headers)
            .build()?;

        Ok(())
    }

    async fn get_json(&self, path: &str) -> Result<Value> {
        let url = format!("{}{}", self.base_url, path);
        let resp = self.client.get(&url).send().await?;
        if !resp.status().is_success() {
            return Err(anyhow!("HTTP {} from {}", resp.status(), path));
        }
        Ok(resp.json().await?)
    }

    async fn get_json_slow(&self, path: &str) -> Result<Value> {
        let url = format!("{}{}", self.base_url, path);
        let resp = self.slow_client.get(&url).send().await?;
        if !resp.status().is_success() {
            return Err(anyhow!("HTTP {} from {}", resp.status(), path));
        }
        Ok(resp.json().await?)
    }

    async fn post_json_slow(&self, path: &str, body: &Value) -> Result<Value> {
        let url = format!("{}{}", self.base_url, path);
        let resp = self.slow_client.post(&url).json(body).send().await?;
        if !resp.status().is_success() {
            return Err(anyhow!("HTTP {} from {}", resp.status(), path));
        }
        Ok(resp.json().await?)
    }

    async fn post_json(&self, path: &str, body: &Value) -> Result<Value> {
        let url = format!("{}{}", self.base_url, path);
        let resp = self.client.post(&url).json(body).send().await?;
        if !resp.status().is_success() {
            return Err(anyhow!("HTTP {} from {}", resp.status(), path));
        }
        Ok(resp.json().await?)
    }

    /// POST with a per-request timeout override. Used for commands that the
    /// USB transport hardens with explicit long timeouts (e.g.
    /// SELFTEST_AUTO_CAL ≈ 30 s — see usb_transport.rs:280-287). Without this
    /// the HTTP path would surface false "no response" errors on otherwise
    /// valid blocking firmware operations.
    async fn post_json_with_timeout(
        &self,
        path: &str,
        body: &Value,
        timeout: std::time::Duration,
    ) -> Result<Value> {
        let url = format!("{}{}", self.base_url, path);
        let resp = self
            .client
            .post(&url)
            .timeout(timeout)
            .json(body)
            .send()
            .await?;
        if !resp.status().is_success() {
            return Err(anyhow!("HTTP {} from {}", resp.status(), path));
        }
        Ok(resp.json().await?)
    }

    /// Map channel function string from webserver to numeric ID used by BBP.
    fn parse_function(v: &Value) -> u8 {
        if let Some(n) = v.as_u64() {
            return n as u8;
        }
        match v.as_str().unwrap_or("") {
            "HIGH_IMP" => 0,
            "VOUT" => 1,
            "IOUT" => 2,
            "VIN" => 3,
            "IIN_EXT_PWR" => 4,
            "IIN_LOOP_PWR" => 5,
            "RES_MEAS" => 7,
            "DIN_LOGIC" => 8,
            "DIN_LOOP" => 9,
            "IOUT_HART" => 10,
            "IIN_EXT_PWR_HART" => 11,
            "IIN_LOOP_PWR_HART" => 12,
            _ => 0,
        }
    }

    /// Parse /api/status JSON into DeviceState
    fn parse_status_json(json: &Value) -> Option<DeviceState> {
        let mut state = DeviceState::default();

        state.spi_ok = json.get("spiOk").and_then(|v| v.as_bool()).unwrap_or(false);
        state.die_temperature = json.get("dieTemp").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32;
        state.alert_status = json
            .get("alertStatus")
            .and_then(|v| v.as_u64())
            .unwrap_or(0) as u16;
        state.alert_mask = json.get("alertMask").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
        state.supply_alert_status = json
            .get("supplyAlertStatus")
            .and_then(|v| v.as_u64())
            .unwrap_or(0) as u16;
        state.supply_alert_mask = json
            .get("supplyAlertMask")
            .and_then(|v| v.as_u64())
            .unwrap_or(0) as u16;
        state.live_status = json.get("liveStatus").and_then(|v| v.as_u64()).unwrap_or(0) as u16;

        if let Some(channels) = json.get("channels").and_then(|v| v.as_array()) {
            for (i, ch_json) in channels.iter().enumerate().take(4) {
                state.channels[i] = ChannelState {
                    function: Self::parse_function(ch_json.get("function").unwrap_or(&Value::Null)),
                    adc_raw: ch_json.get("adcRaw").and_then(|v| v.as_u64()).unwrap_or(0) as u32,
                    adc_value: ch_json
                        .get("adcValue")
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0) as f32,
                    adc_range: ch_json
                        .get("adcRange")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u8,
                    adc_rate: ch_json.get("adcRate").and_then(|v| v.as_u64()).unwrap_or(0) as u8,
                    adc_mux: ch_json.get("adcMux").and_then(|v| v.as_u64()).unwrap_or(0) as u8,
                    dac_code: ch_json.get("dacCode").and_then(|v| v.as_u64()).unwrap_or(0) as u16,
                    dac_value: ch_json
                        .get("dacValue")
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0) as f32,
                    din_state: ch_json
                        .get("dinState")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                    din_counter: ch_json
                        .get("dinCounter")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u32,
                    do_state: ch_json
                        .get("doState")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                    channel_alert: ch_json
                        .get("channelAlert")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u16,
                    channel_alert_mask: ch_json
                        .get("channelAlertMask")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u16,
                    rtd_excitation_ua: ch_json
                        .get("rtdExcitationUa")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u16,
                };
            }
        }

        // Parse diagnostic slots if present
        if let Some(diag) = json.get("diagnostics").and_then(|v| v.as_array()) {
            for (i, d) in diag.iter().enumerate().take(4) {
                state.diag[i] = DiagState {
                    source: d.get("source").and_then(|v| v.as_u64()).unwrap_or(0) as u8,
                    raw_code: d.get("raw").and_then(|v| v.as_u64()).unwrap_or(0) as u16,
                    value: d.get("value").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32,
                };
            }
        }

        // Parse MUX states if present
        if let Some(mux) = json.get("muxStates").and_then(|v| v.as_array()) {
            for (i, v) in mux.iter().enumerate().take(4) {
                state.mux_states[i] = v.as_u64().unwrap_or(0) as u8;
            }
        }

        Some(state)
    }
}

#[async_trait]
impl Transport for HttpTransport {
    async fn send_command(&self, cmd_id: u8, payload: &[u8]) -> Result<Vec<u8>> {
        if !self.connected.load(Ordering::Relaxed) {
            return Err(anyhow!("Not connected"));
        }

        // Map BBP command IDs to HTTP REST API calls
        match cmd_id {
            // Self-test / calibration
            bbp::CMD_SELFTEST_STATUS => {
                let json = self.get_json("/api/selftest").await?;
                let mut pw = bbp::PayloadWriter::new();
                let boot = json.get("boot");
                let cal = json.get("cal").or_else(|| json.get("calibration"));
                pw.put_bool(
                    boot.and_then(|v| v.get("ran"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    boot.and_then(|v| v.get("passed"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_f32(
                    boot.and_then(|v| v.get("vadj1V"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0) as f32,
                );
                pw.put_f32(
                    boot.and_then(|v| v.get("vadj2V"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0) as f32,
                );
                pw.put_f32(
                    boot.and_then(|v| v.get("vlogicV"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0) as f32,
                );
                pw.put_u8(
                    cal.and_then(|v| v.get("status"))
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u8,
                );
                pw.put_u8(
                    cal.and_then(|v| v.get("channel"))
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u8,
                );
                pw.put_u8(
                    cal.and_then(|v| v.get("points"))
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u8,
                );
                pw.put_f32(
                    cal.and_then(|v| v.get("lastVoltageV"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(-1.0) as f32,
                );
                pw.put_f32(
                    cal.and_then(|v| v.get("errorMv"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0) as f32,
                );
                pw.put_bool(
                    json.get("workerEnabled")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    json.get("supplyMonitorActive")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                Ok(pw.buf)
            }

            bbp::CMD_SELFTEST_WORKER => {
                // Match USB framing (firmware bbp.cpp handleSelftestWorker):
                // payload: 0=disable, 1=enable, 0xFF=query. Response: u8(workerEnabled).
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let op = payload[0];
                let json = if op == 0xFF {
                    self.get_json("/api/selftest").await?
                } else {
                    self.post_json(
                        "/api/selftest/worker",
                        &serde_json::json!({"enabled": op != 0}),
                    )
                    .await?
                };
                let worker_enabled = json
                    .get("workerEnabled")
                    .or_else(|| json.get("enabled"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                Ok(vec![if worker_enabled { 1 } else { 0 }])
            }

            bbp::CMD_SELFTEST_MEASURE_SUPPLY => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let rail = payload[0];
                let json = self
                    .get_json(&format!("/api/selftest/supply/{}", rail))
                    .await?;
                let mut pw = bbp::PayloadWriter::new();
                pw.put_u8(rail);
                pw.put_f32(json.get("voltage").and_then(|v| v.as_f64()).unwrap_or(-1.0) as f32);
                Ok(pw.buf)
            }

            bbp::CMD_SELFTEST_EFUSE_CURRENTS => {
                let json = self.get_json("/api/selftest/efuse").await?;
                let mut pw = bbp::PayloadWriter::new();
                pw.put_bool(
                    json.get("available")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_u32(
                    json.get("timestampMs")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u32,
                );
                let efuses = json.get("efuses").and_then(|v| v.as_array());
                for i in 0..4 {
                    let cur = efuses
                        .and_then(|a| a.get(i))
                        .and_then(|v| v.get("currentA"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(-1.0);
                    pw.put_f32(cur as f32);
                }
                Ok(pw.buf)
            }

            bbp::CMD_SELFTEST_AUTO_CAL => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let channel = payload[0];
                // Match USB transport hardening (usb_transport.rs:283): IDAC
                // sweep + measurement loop can take ~30 s. The default 3 s
                // request timeout would abort a perfectly valid run.
                let json = self
                    .post_json_with_timeout(
                        "/api/selftest/calibrate",
                        &serde_json::json!({"channel": channel}),
                        std::time::Duration::from_secs(35),
                    )
                    .await?;
                let mut pw = bbp::PayloadWriter::new();
                pw.put_u8(json.get("status").and_then(|v| v.as_u64()).unwrap_or(3) as u8);
                pw.put_u8(
                    json.get("channel")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(channel as u64) as u8,
                );
                pw.put_u8(json.get("points").and_then(|v| v.as_u64()).unwrap_or(0) as u8);
                pw.put_f32(
                    json.get("lastVoltageV")
                        .and_then(|v| v.as_f64())
                        .unwrap_or(-1.0) as f32,
                );
                pw.put_f32(json.get("errorMv").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32);
                Ok(pw.buf)
            }

            bbp::CMD_SELFTEST_INT_SUPPLIES => {
                let json = self.get_json_slow("/api/selftest/supplies").await?;
                let mut pw = bbp::PayloadWriter::new();
                pw.put_bool(json.get("valid").and_then(|v| v.as_bool()).unwrap_or(false));
                pw.put_bool(
                    json.get("suppliesOk")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_f32(json.get("avddHiV").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32);
                pw.put_f32(json.get("dvccV").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32);
                pw.put_f32(json.get("avccV").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32);
                pw.put_f32(json.get("avssV").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32);
                pw.put_f32(json.get("tempC").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32);
                Ok(pw.buf)
            }

            bbp::CMD_SET_CH_FUNC => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let func = payload[1];
                let body = serde_json::json!({"function": func});
                self.post_json(&format!("/api/channel/{}/function", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DAC_CODE => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let code = u16::from_le_bytes([payload[1], payload[2]]);
                let body = serde_json::json!({"code": code});
                self.post_json(&format!("/api/channel/{}/dac", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DAC_VOLTAGE => {
                if payload.len() < 6 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let voltage = f32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                let bipolar = payload[5] != 0;
                let body = serde_json::json!({"voltage": voltage, "bipolar": bipolar});
                self.post_json(&format!("/api/channel/{}/dac", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DAC_CURRENT => {
                if payload.len() < 5 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let current = f32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                let body = serde_json::json!({"current_mA": current});
                self.post_json(&format!("/api/channel/{}/dac", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_ADC_CONFIG => {
                if payload.len() < 4 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({
                    "mux": payload[1],
                    "range": payload[2],
                    "rate": payload[3]
                });
                self.post_json(&format!("/api/channel/{}/adc/config", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_CLEAR_ALL_ALERTS => {
                self.post_json("/api/faults/clear", &serde_json::json!({}))
                    .await?;
                Ok(vec![])
            }

            bbp::CMD_DEVICE_RESET => {
                self.post_json("/api/device/reset", &serde_json::json!({}))
                    .await?;
                Ok(vec![])
            }

            bbp::CMD_GET_GPIO_STATUS => {
                let json = self.get_json("/api/gpio").await?;
                let mut pw = bbp::PayloadWriter::new();
                let gpios = json.get("gpios").and_then(|v| v.as_array());
                for i in 0..6 {
                    let g = gpios.and_then(|a| a.get(i));
                    pw.put_u8(
                        g.and_then(|v| v.get("mode"))
                            .and_then(|v| v.as_u64())
                            .unwrap_or(0) as u8,
                    );
                    pw.put_bool(
                        g.and_then(|v| v.get("output"))
                            .and_then(|v| v.as_bool())
                            .unwrap_or(false),
                    );
                    pw.put_bool(
                        g.and_then(|v| v.get("input"))
                            .and_then(|v| v.as_bool())
                            .unwrap_or(false),
                    );
                    pw.put_bool(
                        g.and_then(|v| v.get("pulldown"))
                            .and_then(|v| v.as_bool())
                            .unwrap_or(false),
                    );
                }
                Ok(pw.buf)
            }

            bbp::CMD_SET_GPIO_CONFIG => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload"));
                }
                let gpio = payload[0];
                let body = serde_json::json!({"mode": payload[1], "pulldown": payload[2] != 0});
                self.post_json(&format!("/api/gpio/{}/config", gpio), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_GPIO_VALUE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let gpio = payload[0];
                let body = serde_json::json!({"value": payload[1] != 0});
                self.post_json(&format!("/api/gpio/{}/set", gpio), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DO_STATE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({"on": payload[1] != 0});
                self.post_json(&format!("/api/channel/{}/do/set", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_VOUT_RANGE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({"bipolar": payload[1] != 0});
                self.post_json(&format!("/api/channel/{}/vout/range", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_ILIMIT => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({"limit8mA": payload[1] != 0});
                self.post_json(&format!("/api/channel/{}/ilimit", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_RTD_CONFIG => {
                // payload: channel(u8) + current(u8)  (0=500µA, 1=1mA)
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({"current": payload[1]});
                self.post_json(&format!("/api/channel/{}/rtd/config", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            // DS4424 IDAC — re-encode JSON as BBP binary for uniform parsing
            bbp::CMD_IDAC_GET_STATUS => {
                let json = self.get_json("/api/idac").await?;
                let mut pw = bbp::PayloadWriter::new();
                let present = json
                    .get("present")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                pw.put_bool(present);
                let channels = json.get("channels").and_then(|v| v.as_array());
                for i in 0..4u8 {
                    let ch = channels.and_then(|arr| arr.get(i as usize));
                    pw.put_u8(i);
                    pw.put_u8(
                        ch.and_then(|c| c.get("code").and_then(|v| v.as_i64()))
                            .unwrap_or(0) as u8,
                    );
                    pw.put_f32(
                        ch.and_then(|c| c.get("targetV").and_then(|v| v.as_f64()))
                            .unwrap_or(0.0) as f32,
                    );
                    pw.put_f32(
                        ch.and_then(|c| c.get("actualV").and_then(|v| v.as_f64()))
                            .unwrap_or(
                                ch.and_then(|c| c.get("targetV").and_then(|v| v.as_f64()))
                                    .unwrap_or(0.0),
                            ) as f32,
                    );
                    pw.put_f32(
                        ch.and_then(|c| c.get("midpointV").and_then(|v| v.as_f64()))
                            .unwrap_or(0.0) as f32,
                    );
                    pw.put_f32(
                        ch.and_then(|c| c.get("vMin").and_then(|v| v.as_f64()))
                            .unwrap_or(0.0) as f32,
                    );
                    pw.put_f32(
                        ch.and_then(|c| c.get("vMax").and_then(|v| v.as_f64()))
                            .unwrap_or(0.0) as f32,
                    );
                    pw.put_f32(
                        ch.and_then(|c| c.get("stepMv").and_then(|v| v.as_f64()))
                            .unwrap_or(0.0) as f32,
                    );
                    let calibrated = ch
                        .and_then(|c| c.get("calibrated").and_then(|v| v.as_bool()))
                        .unwrap_or(false);
                    pw.put_bool(calibrated);
                    // Polynomial fit (firmware /api/idac exposes polyValid + calPoly[4]).
                    // Match USB framing in commands.rs::parse_idac_status.
                    let poly = ch.and_then(|c| c.get("calPoly").and_then(|v| v.as_array()));
                    let poly_valid = ch
                        .and_then(|c| c.get("polyValid").and_then(|v| v.as_bool()))
                        .unwrap_or(false);
                    pw.put_bool(poly_valid);
                    for j in 0..4 {
                        let v = poly
                            .and_then(|a| a.get(j))
                            .and_then(|v| v.as_f64())
                            .unwrap_or(0.0);
                        pw.put_f32(v as f32);
                    }
                }
                Ok(pw.buf)
            }

            bbp::CMD_IDAC_SET_CODE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let body = serde_json::json!({"ch": payload[0], "code": payload[1] as i8});
                self.post_json("/api/idac/code", &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_IDAC_SET_VOLTAGE => {
                if payload.len() < 5 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let voltage = f32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                let body = serde_json::json!({"ch": ch, "voltage": voltage});
                self.post_json("/api/idac/voltage", &body).await?;
                Ok(payload.to_vec())
            }

            // PCA9535 GPIO Expander — re-encode as BBP binary
            bbp::CMD_PCA_GET_STATUS => {
                let json = self.get_json("/api/ioexp").await?;
                let mut pw = bbp::PayloadWriter::new();
                pw.put_bool(
                    json.get("present")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_u8(json.get("input0").and_then(|v| v.as_u64()).unwrap_or(0) as u8);
                pw.put_u8(json.get("input1").and_then(|v| v.as_u64()).unwrap_or(0) as u8);
                pw.put_u8(json.get("output0").and_then(|v| v.as_u64()).unwrap_or(0) as u8);
                pw.put_u8(json.get("output1").and_then(|v| v.as_u64()).unwrap_or(0) as u8);
                let pg = json.get("powerGood");
                pw.put_bool(
                    pg.and_then(|v| v.get("logic"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    pg.and_then(|v| v.get("vadj1"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    pg.and_then(|v| v.get("vadj2"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                // E-Fuse faults
                let efuses = json.get("efuses").and_then(|v| v.as_array());
                for i in 0..4 {
                    let flt = efuses
                        .and_then(|a| a.get(i))
                        .and_then(|e| e.get("fault"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    pw.put_bool(flt);
                }
                let en = json.get("enables");
                pw.put_bool(
                    en.and_then(|v| v.get("vadj1"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    en.and_then(|v| v.get("vadj2"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    en.and_then(|v| v.get("analog15v"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    en.and_then(|v| v.get("mux"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    en.and_then(|v| v.get("usbHub"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                // E-Fuse enables
                for i in 0..4 {
                    let enabled = efuses
                        .and_then(|a| a.get(i))
                        .and_then(|e| e.get("enabled"))
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    pw.put_bool(enabled);
                }
                Ok(pw.buf)
            }

            bbp::CMD_PCA_SET_CONTROL => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ctrl_names = [
                    "vadj1", "vadj2", "15v", "mux", "usb", "efuse1", "efuse2", "efuse3", "efuse4",
                ];
                let idx = payload[0] as usize;
                let name = if idx < ctrl_names.len() {
                    ctrl_names[idx]
                } else {
                    "?"
                };
                let body = serde_json::json!({"control": name, "on": payload[1] != 0});
                self.post_json("/api/ioexp/control", &body).await?;
                Ok(payload.to_vec())
            }

            // HUSB238 USB PD — re-encode as BBP binary
            bbp::CMD_USBPD_GET_STATUS => {
                let json = self.get_json("/api/usbpd").await?;
                let mut pw = bbp::PayloadWriter::new();
                pw.put_bool(
                    json.get("present")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(
                    json.get("attached")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                );
                pw.put_bool(json.get("cc").and_then(|v| v.as_str()).unwrap_or("CC1") == "CC2");
                pw.put_u8(json.get("pdResponse").and_then(|v| v.as_u64()).unwrap_or(0) as u8);
                let voltage_v = json.get("voltageV").and_then(|v| v.as_f64()).unwrap_or(0.0);
                let current_a = json.get("currentA").and_then(|v| v.as_f64()).unwrap_or(0.0);
                pw.put_u8(encode_husb_voltage_code(voltage_v));
                pw.put_u8(encode_husb_current_code(current_a));
                pw.put_f32(voltage_v as f32);
                pw.put_f32(current_a as f32);
                pw.put_f32(json.get("powerW").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32);
                // Source PDOs
                let pdos = json.get("sourcePdos").and_then(|v| v.as_array());
                for i in 0..6 {
                    let pdo = pdos.and_then(|a| a.get(i));
                    pw.put_bool(
                        pdo.and_then(|p| p.get("detected"))
                            .and_then(|v| v.as_bool())
                            .unwrap_or(false),
                    );
                    let max_current = pdo
                        .and_then(|p| p.get("maxCurrentA"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.5);
                    pw.put_u8(encode_husb_current_code(max_current));
                }
                pw.put_u8(
                    json.get("selectedPdo")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as u8,
                );
                Ok(pw.buf)
            }

            bbp::CMD_USBPD_SELECT_PDO => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let v_map = [0, 5, 9, 12, 15, 18, 20];
                let v = if (payload[0] as usize) < v_map.len() {
                    v_map[payload[0] as usize]
                } else {
                    0
                };
                let body = serde_json::json!({"voltage": v});
                self.post_json("/api/usbpd/select", &body).await?;
                Ok(payload.to_vec())
            }

            // Waveform Generator
            bbp::CMD_START_WAVEGEN => {
                if payload.len() < 15 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let wf = payload[1];
                let freq = f32::from_le_bytes([payload[2], payload[3], payload[4], payload[5]]);
                let amp = f32::from_le_bytes([payload[6], payload[7], payload[8], payload[9]]);
                let off = f32::from_le_bytes([payload[10], payload[11], payload[12], payload[13]]);
                let mode = payload[14];
                let body = serde_json::json!({
                    "channel": ch, "waveform": wf, "freq_hz": freq,
                    "amplitude": amp, "offset": off, "mode": mode
                });
                self.post_json("/api/wavegen/start", &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_STOP_WAVEGEN => {
                self.post_json("/api/wavegen/stop", &serde_json::json!({}))
                    .await?;
                Ok(vec![])
            }

            bbp::CMD_SET_DIN_CONFIG => {
                if payload.len() < 8 {
                    return Err(anyhow!("Invalid payload"));
                }
                let mut r = bbp::PayloadReader::new(payload);
                let ch = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let thresh = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let thresh_mode = r.get_bool().ok_or_else(|| anyhow!("Payload too short"))?;
                let debounce = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let sink = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let sink_range = r.get_bool().ok_or_else(|| anyhow!("Payload too short"))?;
                let oc_det = r.get_bool().ok_or_else(|| anyhow!("Payload too short"))?;
                let sc_det = r.get_bool().ok_or_else(|| anyhow!("Payload too short"))?;
                let body = serde_json::json!({
                    "thresh": thresh,
                    "threshMode": thresh_mode,
                    "debounce": debounce,
                    "sink": sink,
                    "sinkRange": sink_range,
                    "ocDet": oc_det,
                    "scDet": sc_det
                });
                self.post_json(&format!("/api/channel/{}/din/config", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DO_CONFIG => {
                if payload.len() < 5 {
                    return Err(anyhow!("Invalid payload"));
                }
                let mut r = bbp::PayloadReader::new(payload);
                let ch = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let mode = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let src_sel_gpio = r.get_bool().ok_or_else(|| anyhow!("Payload too short"))?;
                let t1 = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let t2 = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let body = serde_json::json!({
                    "mode": mode,
                    "srcSelGpio": src_sel_gpio,
                    "t1": t1,
                    "t2": t2
                });
                self.post_json(&format!("/api/channel/{}/do/config", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_CLEAR_CH_ALERT => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                self.post_json(&format!("/api/faults/clear/{}", ch), &serde_json::json!({}))
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_CH_ALERT_MASK => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload"));
                }
                let mut r = bbp::PayloadReader::new(payload);
                let ch = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let mask = r.get_u16().ok_or_else(|| anyhow!("Payload too short"))?;
                let body = serde_json::json!({"mask": mask});
                self.post_json(&format!("/api/faults/mask/{}", ch), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_GET_UART_CONFIG => {
                // UART config — not critical, return empty for now
                Ok(vec![])
            }

            bbp::CMD_SET_UART_CONFIG => {
                if payload.len() < 12 {
                    return Err(anyhow!("Invalid payload"));
                }
                let mut r = bbp::PayloadReader::new(payload);
                let bridge_id = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let uart_num = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let tx_pin = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let rx_pin = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let baudrate = r.get_u32().ok_or_else(|| anyhow!("Payload too short"))?;
                let data_bits = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let parity = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let stop_bits = r.get_u8().ok_or_else(|| anyhow!("Payload too short"))?;
                let enabled = r.get_bool().ok_or_else(|| anyhow!("Payload too short"))?;
                let body = serde_json::json!({
                    "uartNum": uart_num,
                    "txPin": tx_pin,
                    "rxPin": rx_pin,
                    "baudrate": baudrate,
                    "dataBits": data_bits,
                    "parity": parity,
                    "stopBits": stop_bits,
                    "enabled": enabled
                });
                self.post_json(&format!("/api/uart/{}/config", bridge_id), &body)
                    .await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_GET_UART_PINS => {
                // UART pins — not critical, return empty for now
                Ok(vec![])
            }

            // MUX commands
            bbp::CMD_MUX_GET_ALL => {
                let json = self.get_json("/api/mux").await?;
                let states = json.get("states").and_then(|v| v.as_array());
                let mut pw = bbp::PayloadWriter::new();
                for i in 0..4 {
                    pw.put_u8(
                        states
                            .and_then(|a| a.get(i))
                            .and_then(|v| v.as_u64())
                            .unwrap_or(0) as u8,
                    );
                }
                Ok(pw.buf)
            }

            bbp::CMD_MUX_SET_ALL => {
                let states: Vec<u8> = payload.to_vec();
                let body = serde_json::json!({"states": states});
                self.post_json("/api/mux/all", &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_MUX_SET_SWITCH => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload"));
                }
                let body = serde_json::json!({
                    "device": payload[0],
                    "switch": payload[1],
                    "closed": payload[2] != 0
                });
                self.post_json("/api/mux/switch", &body).await?;
                Ok(payload.to_vec())
            }

            // Raw register access - requires direct SPI, not practical over HTTP
            bbp::CMD_REG_READ | bbp::CMD_REG_WRITE => Err(anyhow!(
                "Raw register read/write not available over HTTP (USB only)"
            )),

            // PCA9535 raw port write - no HTTP endpoint in webserver
            bbp::CMD_PCA_SET_PORT => Err(anyhow!(
                "PCA9535 raw port write not available over HTTP (USB only)"
            )),

            // USB PD re-negotiation - no HTTP endpoint in webserver
            bbp::CMD_USBPD_GO => Err(anyhow!(
                "USB PD re-negotiation not available over HTTP (USB only)"
            )),

            // IDAC calibration commands - no HTTP endpoint in webserver
            bbp::CMD_IDAC_CAL_ADD_POINT | bbp::CMD_IDAC_CAL_CLEAR | bbp::CMD_IDAC_CAL_SAVE => Err(
                anyhow!("IDAC calibration not available over HTTP (USB only)"),
            ),

            // WiFi Management
            bbp::CMD_WIFI_GET_STATUS => {
                let json = self.get_json("/api/wifi").await?;
                // Re-encode as BBP binary (length-prefixed strings)
                let mut pw = bbp::PayloadWriter::new();
                let connected = json
                    .get("connected")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                pw.put_bool(connected);
                let sta_ssid = json.get("staSSID").and_then(|v| v.as_str()).unwrap_or("");
                pw.put_u8(sta_ssid.len() as u8);
                pw.buf.extend_from_slice(sta_ssid.as_bytes());
                let sta_ip = json
                    .get("staIP")
                    .and_then(|v| v.as_str())
                    .unwrap_or("0.0.0.0");
                pw.put_u8(sta_ip.len() as u8);
                pw.buf.extend_from_slice(sta_ip.as_bytes());
                let rssi = json.get("rssi").and_then(|v| v.as_i64()).unwrap_or(0) as i32;
                pw.put_u32(rssi as u32);
                let ap_ssid = json.get("apSSID").and_then(|v| v.as_str()).unwrap_or("");
                pw.put_u8(ap_ssid.len() as u8);
                pw.buf.extend_from_slice(ap_ssid.as_bytes());
                let ap_ip = json.get("apIP").and_then(|v| v.as_str()).unwrap_or("");
                pw.put_u8(ap_ip.len() as u8);
                pw.buf.extend_from_slice(ap_ip.as_bytes());
                let ap_mac = json.get("apMAC").and_then(|v| v.as_str()).unwrap_or("");
                pw.put_u8(ap_mac.len() as u8);
                pw.buf.extend_from_slice(ap_mac.as_bytes());
                Ok(pw.buf)
            }

            bbp::CMD_WIFI_CONNECT => {
                // Parse payload: ssid_len(u8) + ssid + pass_len(u8) + pass
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let mut r = bbp::PayloadReader::new(payload);
                let ssid_len = r.get_u8().unwrap() as usize;
                if r.remaining() < ssid_len + 1 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ssid =
                    String::from_utf8_lossy(&payload[r.pos()..r.pos() + ssid_len]).to_string();
                r.skip(ssid_len);
                let pass_len = r.get_u8().unwrap() as usize;
                let pass = if pass_len > 0 && r.remaining() >= pass_len {
                    String::from_utf8_lossy(&payload[r.pos()..r.pos() + pass_len]).to_string()
                } else {
                    String::new()
                };

                let body = serde_json::json!({"ssid": ssid, "password": pass});
                let json = self.post_json_slow("/api/wifi/connect", &body).await?;
                let success = json
                    .get("success")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                Ok(vec![if success { 1 } else { 0 }])
            }

            bbp::CMD_WIFI_SCAN => {
                let json = self.get_json_slow("/api/wifi/scan").await?;
                // Re-encode as BBP binary: count(u8) + N * (ssid_len(u8) + ssid + rssi(i8) + auth(u8))
                let mut buf = Vec::new();
                let networks = json.get("networks").and_then(|v| v.as_array());
                let nets = networks.cloned().unwrap_or_default();
                buf.push(nets.len() as u8);
                for n in &nets {
                    let ssid = n.get("ssid").and_then(|v| v.as_str()).unwrap_or("");
                    let rssi = n.get("rssi").and_then(|v| v.as_i64()).unwrap_or(0) as i8;
                    let auth = n.get("auth").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                    buf.push(ssid.len() as u8);
                    buf.extend_from_slice(ssid.as_bytes());
                    buf.push(rssi as u8);
                    buf.push(auth);
                }
                Ok(buf)
            }

            // Scope streaming over HTTP — emulated via /api/scope polling.
            // ADC streaming over HTTP is intentionally unsupported (high-rate
            // raw samples would overwhelm the WebServer thread); the scope
            // bucket endpoint provides bounded ~10 Hz updates that keep the
            // Scope tab usable on WiFi (Bug 2).
            bbp::CMD_START_SCOPE_STREAM => {
                self.start_scope_polling()?;
                Ok(vec![])
            }
            bbp::CMD_STOP_SCOPE_STREAM => {
                self.stop_scope_polling();
                Ok(vec![])
            }
            bbp::CMD_START_ADC_STREAM | bbp::CMD_STOP_ADC_STREAM => {
                Err(anyhow!("ADC streaming not supported over HTTP"))
            }

            // -----------------------------------------------------------
            // Quick Setup slots (BBP 0xF0..0xF4)
            //
            // Each arm calls the matching REST endpoint registered in
            // Firmware/ESP32/src/webserver.cpp:3054-3066 and reframes the
            // response in the exact byte layout the firmware emits over USB
            // (Firmware/ESP32/src/bbp.cpp::handleQuickSetup*). That keeps the
            // existing parse_quicksetup_* helpers in commands.rs working
            // unchanged across both transports.
            // -----------------------------------------------------------
            bbp::CMD_QS_LIST => {
                let json = self.get_json("/api/quicksetup").await?;
                let mut bitmap: u8 = 0;
                let mut hashes: [u8; 4] = [0; 4];
                if let Some(arr) = json.get("slots").and_then(|v| v.as_array()) {
                    for (i, slot) in arr.iter().enumerate().take(4) {
                        let occupied = slot
                            .get("occupied")
                            .and_then(|v| v.as_bool())
                            .unwrap_or(false);
                        if occupied {
                            bitmap |= 1u8 << i;
                            // The firmware truncates the summary hash to u8 in
                            // the BBP framing (handleQuickSetupList) even
                            // though the JSON exposes the full 32-bit value.
                            hashes[i] = slot
                                .get("summary")
                                .and_then(|s| s.get("hash"))
                                .and_then(|v| v.as_u64())
                                .unwrap_or(0) as u8;
                        }
                    }
                }
                let mut buf = Vec::with_capacity(5);
                buf.push(bitmap);
                buf.extend_from_slice(&hashes);
                Ok(buf)
            }

            bbp::CMD_QS_GET => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let slot = payload[0];
                let url = format!("{}/api/quicksetup/{}", self.base_url, slot);
                let resp = self.client.get(&url).send().await?;
                if !resp.status().is_success() {
                    // 404 = empty slot maps cleanly to BBP_ERR_INVALID_STATE
                    // semantics; surface as a Rust error so the parser layer
                    // sees an empty response rather than spurious bytes.
                    return Err(anyhow!(
                        "HTTP {} from /api/quicksetup/{}",
                        resp.status(),
                        slot
                    ));
                }
                // Firmware returns the raw stored JSON (no envelope), so the
                // body bytes ARE the BBP payload. Cap to BBP_MAX_PAYLOAD via
                // QUICKSETUP_MAX_JSON_BYTES which the parser already enforces.
                Ok(resp.bytes().await?.to_vec())
            }

            bbp::CMD_QS_SAVE => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let slot = payload[0];
                let url = format!("{}/api/quicksetup/{}", self.base_url, slot);
                // POST with empty JSON body (firmware ignores body on save —
                // it snapshots the live device state into the slot).
                let resp = self
                    .client
                    .post(&url)
                    .json(&serde_json::json!({}))
                    .send()
                    .await?;
                if !resp.status().is_success() {
                    return Err(anyhow!(
                        "HTTP {} from /api/quicksetup/{}",
                        resp.status(),
                        slot
                    ));
                }
                Ok(resp.bytes().await?.to_vec())
            }

            bbp::CMD_QS_APPLY => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let slot = payload[0];
                let url = format!("{}/api/quicksetup/{}/apply", self.base_url, slot);
                // 200 / 409 are both well-formed responses; map status code
                // to the BBP single-byte status the parser expects:
                //   0 = applied, 1 = slot empty (404), 2 = apply error.
                let resp = self
                    .client
                    .post(&url)
                    .json(&serde_json::json!({}))
                    .send()
                    .await?;
                let status_code = resp.status();
                let json: Value = resp
                    .json()
                    .await
                    .unwrap_or_else(|_| serde_json::json!({}));
                let status_byte: u8 = if status_code.is_success()
                    && json
                        .get("ok")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false)
                {
                    0
                } else if status_code.as_u16() == 404 {
                    1
                } else {
                    2
                };
                Ok(vec![status_byte])
            }

            bbp::CMD_QS_DELETE => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload"));
                }
                let slot = payload[0];
                let url = format!("{}/api/quicksetup/{}/delete", self.base_url, slot);
                let resp = self
                    .client
                    .post(&url)
                    .json(&serde_json::json!({}))
                    .send()
                    .await?;
                if !resp.status().is_success() {
                    return Err(anyhow!(
                        "HTTP {} from /api/quicksetup/{}/delete",
                        resp.status(),
                        slot
                    ));
                }
                let json: Value = resp
                    .json()
                    .await
                    .unwrap_or_else(|_| serde_json::json!({}));
                // BBP status: 0 = deleted, 1 = was not present.
                let existed = json
                    .get("deleted")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                Ok(vec![if existed { 0 } else { 1 }])
            }

            _ => Err(anyhow!(
                "Command 0x{:02X} not implemented for HTTP transport",
                cmd_id
            )),
        }
    }

    async fn get_status(&self) -> Result<DeviceState> {
        // Fetch status and GPIO in parallel for lower latency
        let status_url = format!("{}/api/status", self.base_url);
        let gpio_url = format!("{}/api/gpio", self.base_url);
        let (status_res, gpio_res) = tokio::join!(
            self.client.get(&status_url).send(),
            self.client.get(&gpio_url).send()
        );

        let status_json: Value = status_res?.json().await?;
        let mut state = Self::parse_status_json(&status_json)
            .ok_or_else(|| anyhow!("Failed to parse HTTP status response"))?;

        // Merge GPIO state if available
        if let Ok(resp) = gpio_res {
            if let Ok(gpio_json) = resp.json::<Value>().await {
                // firmware returns a top-level array for /api/gpio
                let gpios_array = if gpio_json.is_array() {
                    gpio_json.as_array()
                } else {
                    gpio_json.get("gpios").and_then(|v| v.as_array())
                };

                if let Some(gpios) = gpios_array {
                    for (i, g) in gpios.iter().enumerate().take(12) {
                        state.gpio[i] = crate::state::GpioState {
                            mode: g.get("mode").and_then(|v| v.as_u64()).unwrap_or(0) as u8,
                            output: g.get("output").and_then(|v| v.as_bool()).unwrap_or(false),
                            input: g.get("input").and_then(|v| v.as_bool()).unwrap_or(false),
                            pulldown: g.get("pulldown").and_then(|v| v.as_bool()).unwrap_or(false),
                        };
                    }
                }
            }
        }

        Ok(state)
    }

    fn is_connected(&self) -> bool {
        self.connected.load(Ordering::Relaxed)
    }

    async fn disconnect(&self) -> Result<()> {
        self.connected.store(false, Ordering::Relaxed);
        // Stop the scope-polling task so it doesn't keep emitting after the
        // user has disconnected (Bug 2).
        self.stop_scope_polling();
        log::info!("HTTP transport disconnected from {}", self.base_url);
        Ok(())
    }

    fn transport_name(&self) -> &str {
        "HTTP"
    }

    fn base_url(&self) -> Option<String> {
        Some(self.base_url.clone())
    }
}
