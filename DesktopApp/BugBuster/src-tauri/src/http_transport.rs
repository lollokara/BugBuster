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

fn encode_gpio_status_payload(gpios: Option<&[Value]>) -> Vec<u8> {
    let mut pw = bbp::PayloadWriter::new();
    for i in 0..12 {
        let g = gpios.and_then(|a| a.get(i));
        pw.put_u8(
            g.and_then(|v| v.get("id"))
                .and_then(|v| v.as_u64())
                .unwrap_or(i as u64) as u8,
        );
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
    pw.buf
}

fn encode_selftest_supplies_cached_payload(json: &Value) -> Vec<u8> {
    let mut pw = bbp::PayloadWriter::new();
    pw.put_bool(
        json.get("available")
            .and_then(|v| v.as_bool())
            .unwrap_or(false),
    );
    pw.put_u32(
        json.get("timestampMs")
            .or_else(|| json.get("timestamp_ms"))
            .and_then(|v| v.as_u64())
            .unwrap_or(0) as u32,
    );
    let rails = json.get("rails").and_then(|v| v.as_array());
    for i in 0..3 {
        let rail = rails.and_then(|a| a.get(i));
        let voltage = rail
            .and_then(|v| v.get("voltageV").or_else(|| v.get("voltage_v")))
            .and_then(|v| v.as_f64())
            .unwrap_or(-1.0);
        pw.put_f32(voltage as f32);
    }
    pw.buf
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
    // Stored admin token so the long-lived scope SSE client can carry the
    // auth header (the per-request `client` headers aren't otherwise reused).
    admin_token: Option<String>,
}

impl HttpTransport {
    /// Connect to the device via HTTP. Verifies connectivity with /api/device/info.
    /// Returns (Self, mac_address) on success.
    ///
    /// `candidate_tokens` is the list of all stored admin tokens to try when the
    /// device responds with 403 (auth required but no token header sent yet).
    pub async fn connect(base_url: &str, candidate_tokens: &[String]) -> Result<(Self, String)> {
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

        // Parse response to confirm it's a BugBuster and extract MAC.
        // If the device returns 403, iterate stored tokens and retry with each
        // as X-BugBuster-Admin-Token until one succeeds.
        let info: Value = if resp.status() == reqwest::StatusCode::FORBIDDEN {
            let mut authed_info: Option<Value> = None;
            for token in candidate_tokens {
                let r = client
                    .get(&url)
                    .header("X-BugBuster-Admin-Token", token)
                    .send()
                    .await?;
                if r.status().is_success() {
                    authed_info = Some(r.json().await?);
                    break;
                }
            }
            match authed_info {
                Some(i) => i,
                None => {
                    return Err(anyhow!(
                        "Device at {} requires authentication and no stored token matched — \
                     connect via USB once to pair",
                        base_url
                    ))
                }
            }
        } else if !resp.status().is_success() {
            return Err(anyhow!("Device returned HTTP {}", resp.status()));
        } else {
            resp.json().await?
        };
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
            admin_token: None,
        };

        Ok((transport, mac))
    }

    /// Inject a Tauri AppHandle for emitting streaming events (e.g.
    /// `scope-data` from the HTTP scope polling task).
    pub fn set_app_handle(&mut self, app: AppHandle) {
        self.app_handle = Some(app);
    }

    /// Start the background scope streaming task. Idempotent — repeat calls
    /// while it is active are a no-op (Bug 2).
    ///
    /// Consumes the firmware Server-Sent-Events stream `/api/scope/stream` —
    /// the same path the on-device web UI uses. This is preferred over polling
    /// `/api/scope` because (a) opening the stream makes the firmware enter
    /// scope-sampling mode, and (b) it is a single long-lived connection rather
    /// than 10 Hz fresh GETs that churned the ESP's tiny httpd socket pool and
    /// produced `httpd_sock_err recv 104` resets / 0 SPS.
    fn start_scope_polling(&self) -> Result<()> {
        if self.scope_polling.swap(true, Ordering::AcqRel) {
            return Ok(()); // already running
        }
        let app = self
            .app_handle
            .clone()
            .ok_or_else(|| anyhow!("HttpTransport missing AppHandle for scope streaming"))?;
        let base_url = self.base_url.clone();
        let polling = self.scope_polling.clone();
        let token = self.admin_token.clone();

        tokio::spawn(async move {
            // Dedicated client with NO overall timeout (SSE is long-lived).
            let mut builder =
                Client::builder().connect_timeout(std::time::Duration::from_secs(5));
            if let Some(tok) = token.as_ref() {
                if let Ok(val) = reqwest::header::HeaderValue::from_str(tok) {
                    let mut h = reqwest::header::HeaderMap::new();
                    h.insert("X-BugBuster-Admin-Token", val);
                    builder = builder.default_headers(h);
                }
            }
            let client = match builder.build() {
                Ok(c) => c,
                Err(e) => {
                    log::error!("scope SSE client build failed: {}", e);
                    polling.store(false, Ordering::Release);
                    return;
                }
            };
            let url = format!("{}/api/scope/stream", base_url);
            let mut seq_counter: u32 = 0;
            log::info!("scope SSE task starting, url={}", url);

            // Reconnect loop: if the stream drops while still requested, retry.
            while polling.load(Ordering::Acquire) {
                let mut resp = match client.get(&url).send().await {
                    Ok(r) if r.status().is_success() => {
                        log::info!("scope SSE connected ({})", r.status());
                        r
                    }
                    Ok(r) => {
                        log::warn!("scope SSE HTTP {}", r.status());
                        tokio::time::sleep(std::time::Duration::from_millis(1000)).await;
                        continue;
                    }
                    Err(e) => {
                        log::warn!("scope SSE connect failed: {}", e);
                        tokio::time::sleep(std::time::Duration::from_millis(1000)).await;
                        continue;
                    }
                };

                let mut buf = String::new();
                loop {
                    if !polling.load(Ordering::Acquire) {
                        return;
                    }
                    let chunk = match resp.chunk().await {
                        Ok(Some(c)) => c,
                        Ok(None) => break, // server closed the stream — reconnect
                        Err(e) => {
                            log::warn!("scope SSE read error: {}", e);
                            break;
                        }
                    };
                    buf.push_str(&String::from_utf8_lossy(&chunk));

                    // SSE frames are delimited by a blank line ("\n\n").
                    while let Some(idx) = buf.find("\n\n") {
                        let frame: String = buf.drain(..idx + 2).collect();
                        for line in frame.lines() {
                            let Some(data) = line.trim().strip_prefix("data:") else {
                                continue;
                            };
                            let json: Value = match serde_json::from_str(data.trim()) {
                                Ok(j) => j,
                                Err(_) => continue,
                            };
                            let Some(samples) = json.get("samples").and_then(|v| v.as_array())
                            else {
                                continue;
                            };
                            for bucket in samples {
                                let Some(arr) =
                                    bucket.as_array().filter(|a| a.len() >= 13)
                                else {
                                    continue;
                                };
                                // [t, ch0avg..ch3avg, ch0min,ch0max,ch1min,ch1max,...]
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

                                // EVT_SCOPE_DATA-compatible payload (parse_scope_event
                                // needs >=58 bytes; avg at pos 10/22/34/46 in 12-byte
                                // [avg f32, min f32, max f32] blocks).
                                let mut payload = Vec::with_capacity(58);
                                payload.extend_from_slice(&seq_counter.to_le_bytes());
                                seq_counter = seq_counter.wrapping_add(1);
                                payload.extend_from_slice(&t_ms.to_le_bytes());
                                payload.extend_from_slice(&1u16.to_le_bytes());
                                for ch in 0..4 {
                                    payload.extend_from_slice(&avg[ch].to_le_bytes());
                                    payload.extend_from_slice(&min[ch].to_le_bytes());
                                    payload.extend_from_slice(&max[ch].to_le_bytes());
                                }
                                let _ = app.emit("scope-data", &payload);
                                if seq_counter % 50 == 1 {
                                    log::info!("scope SSE emitted {} buckets", seq_counter);
                                }
                            }
                        }
                    }
                    // Defend against a malformed stream that never yields "\n\n".
                    if buf.len() > 65536 {
                        buf.clear();
                    }
                }

                if polling.load(Ordering::Acquire) {
                    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                }
            }
            log::info!("scope SSE task exited");
        });
        Ok(())
    }

    fn stop_scope_polling(&self) {
        self.scope_polling.store(false, Ordering::Release);
    }

    /// Set the admin token for future POST requests.
    pub fn set_admin_token(&mut self, token: &str) -> Result<()> {
        self.admin_token = Some(token.to_string());
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

            bbp::CMD_SELFTEST_SUPPLY_VOLTAGES_CACHED => {
                let json = self.get_json("/api/selftest/supplies/cached").await?;
                Ok(encode_selftest_supplies_cached_payload(&json))
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
                Ok(encode_gpio_status_payload(
                    json.get("gpios")
                        .and_then(|v| v.as_array())
                        .map(|v| v.as_slice()),
                ))
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
                    // Only trust polyValid when calPoly actually carries 4 coefficients —
                    // some firmware revisions ship `polyValid:true` with an empty calPoly,
                    // which would otherwise decode to a 0 V (vMin-clamped) reading.
                    let poly_valid = ch
                        .and_then(|c| c.get("polyValid").and_then(|v| v.as_bool()))
                        .unwrap_or(false)
                        && poly.map(|a| a.len() >= 4).unwrap_or(false);
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
                let ssid_len = r
                    .get_u8()
                    .ok_or_else(|| anyhow!("Malformed payload for CMD_WIFI_CONNECT (ssid_len)"))?
                    as usize;
                if r.remaining() < ssid_len + 1 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ssid =
                    String::from_utf8_lossy(&payload[r.pos()..r.pos() + ssid_len]).to_string();
                r.skip(ssid_len);
                let pass_len = r
                    .get_u8()
                    .ok_or_else(|| anyhow!("Malformed payload for CMD_WIFI_CONNECT (pass_len)"))?
                    as usize;
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
                let json: Value = resp.json().await.unwrap_or_else(|_| serde_json::json!({}));
                let status_byte: u8 = if status_code.is_success()
                    && json.get("ok").and_then(|v| v.as_bool()).unwrap_or(false)
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
                let json: Value = resp.json().await.unwrap_or_else(|_| serde_json::json!({}));
                // BBP status: 0 = deleted, 1 = was not present.
                let existed = json
                    .get("deleted")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                Ok(vec![if existed { 0 } else { 1 }])
            }

            // =================================================================
            // HAT v2 commands
            // =================================================================
            bbp::CMD_HAT_GET_STATUS => {
                let j = self.get_json("/api/hat").await?;
                let detected = j.get("detected").and_then(|v| v.as_bool()).unwrap_or(false);
                let connected = j
                    .get("connected")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                let hat_type = j.get("type").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let detect_voltage = j
                    .get("detectVoltage")
                    .and_then(|v| v.as_f64())
                    .unwrap_or(0.0) as f32;
                let fw_major = j.get("fwMajor").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let fw_minor = j.get("fwMinor").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let config_confirmed = j
                    .get("configConfirmed")
                    .or_else(|| j.get("config_confirmed"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                let pin_config: Vec<u8> = j
                    .get("pinConfig")
                    .and_then(|v| v.as_array())
                    .map(|arr| {
                        arr.iter()
                            .take(4)
                            .map(|e| e.get("function").and_then(|v| v.as_u64()).unwrap_or(0) as u8)
                            .collect()
                    })
                    .unwrap_or_else(|| vec![0u8; 4]);
                let dap_connected = j
                    .get("dapConnected")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                let target_detected = j
                    .get("targetDetected")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                let target_dpidr =
                    j.get("targetDpidr").and_then(|v| v.as_u64()).unwrap_or(0) as u32;

                let mut buf = Vec::new();
                buf.push(detected as u8);
                buf.push(connected as u8);
                buf.push(hat_type);
                buf.extend_from_slice(&detect_voltage.to_le_bytes());
                buf.push(fw_major);
                buf.push(fw_minor);
                buf.push(config_confirmed as u8);
                for i in 0..4usize {
                    buf.push(pin_config.get(i).copied().unwrap_or(0));
                }
                // 2 connector slots (enabled, current_ma, fault) — not in REST, use 0
                for _ in 0..2 {
                    buf.push(0);
                    buf.extend_from_slice(&0.0f32.to_le_bytes());
                    buf.push(0);
                }
                buf.extend_from_slice(&0u16.to_le_bytes()); // io_voltage_mv
                buf.push(dap_connected as u8);
                buf.push(target_detected as u8);
                buf.extend_from_slice(&target_dpidr.to_le_bytes());
                Ok(buf)
            }

            bbp::CMD_HAT_DETECT => {
                let j = self
                    .post_json("/api/hat/detect", &serde_json::json!({}))
                    .await?;
                let detected = j.get("detected").and_then(|v| v.as_bool()).unwrap_or(false);
                let hat_type = j.get("type").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let detect_voltage = j
                    .get("detectVoltage")
                    .and_then(|v| v.as_f64())
                    .unwrap_or(0.0) as f32;
                let connected = j
                    .get("connected")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                let mut buf = vec![detected as u8, hat_type];
                buf.extend_from_slice(&detect_voltage.to_le_bytes());
                buf.push(connected as u8);
                Ok(buf)
            }

            bbp::CMD_HAT_GET_CAPS => {
                let j = self.get_json("/api/hat/v2/caps").await?;
                let hw_revision = j.get("hwRevision").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let flags = j.get("flags").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
                let rail_count = j.get("railCount").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let led_count = j.get("ledCount").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let shifted_io_count = j
                    .get("shiftedIoCount")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(0) as u8;
                let la_route_count =
                    j.get("laRouteCount").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let fw_major = j.get("fwMajor").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let fw_minor = j.get("fwMinor").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let mut buf = Vec::new();
                buf.push(hw_revision);
                buf.extend_from_slice(&flags.to_le_bytes());
                buf.push(rail_count);
                buf.push(led_count);
                buf.push(shifted_io_count);
                buf.push(la_route_count);
                buf.push(fw_major);
                buf.push(fw_minor);
                Ok(buf)
            }

            bbp::CMD_HAT_GET_RAIL_STATUS => {
                let j = self.get_json("/api/hat/v2/rails").await?;
                let rails = j
                    .get("rails")
                    .and_then(|v| v.as_array())
                    .cloned()
                    .unwrap_or_default();
                let count = rails.len() as u8;
                let mut buf = vec![count];
                for rail in &rails {
                    let rail_id = rail.get("railId").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                    let enabled = rail
                        .get("enabled")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    let voltage_mv =
                        rail.get("voltageMv").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                    let current_ma =
                        rail.get("currentMa").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                    let status = rail.get("status").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                    let target_mv = rail
                        .get("targetVoltageMv")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(voltage_mv as u64) as u16;
                    buf.push(rail_id);
                    buf.push(enabled as u8);
                    buf.extend_from_slice(&voltage_mv.to_le_bytes());
                    buf.extend_from_slice(&current_ma.to_le_bytes());
                    buf.push(status);
                    buf.extend_from_slice(&target_mv.to_le_bytes());
                }
                Ok(buf)
            }

            bbp::CMD_HAT_SET_RAIL_ENABLE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_RAIL_ENABLE"));
                }
                let rail_id = payload[0];
                let enable = payload[1] != 0;
                let j = self
                    .post_json(
                        "/api/hat/v2/rail/enable",
                        &serde_json::json!({
                            "railId": rail_id,
                            "enable": enable,
                        }),
                    )
                    .await?;
                let r_rail_id = j
                    .get("railId")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(rail_id as u64) as u8;
                let r_enabled = j.get("enabled").and_then(|v| v.as_bool()).unwrap_or(enable);
                let r_voltage_mv = j.get("voltageMv").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                let r_current_ma = j.get("currentMa").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                let r_status = j.get("status").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let r_target_mv = j
                    .get("targetVoltageMv")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(r_voltage_mv as u64) as u16;
                let mut buf = vec![1u8]; // count = 1
                buf.push(r_rail_id);
                buf.push(r_enabled as u8);
                buf.extend_from_slice(&r_voltage_mv.to_le_bytes());
                buf.extend_from_slice(&r_current_ma.to_le_bytes());
                buf.push(r_status);
                buf.extend_from_slice(&r_target_mv.to_le_bytes());
                Ok(buf)
            }

            bbp::CMD_HAT_SET_RAIL_VOLTAGE => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_RAIL_VOLTAGE"));
                }
                let rail_id = payload[0];
                let voltage_mv = u16::from_le_bytes([payload[1], payload[2]]);
                let j = self
                    .post_json(
                        "/api/hat/v2/rail/voltage",
                        &serde_json::json!({
                            "railId": rail_id,
                            "voltageMv": voltage_mv,
                        }),
                    )
                    .await?;
                let r_rail_id = j
                    .get("railId")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(rail_id as u64) as u8;
                let r_enabled = j.get("enabled").and_then(|v| v.as_bool()).unwrap_or(false);
                let r_voltage_mv = j.get("voltageMv").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                let r_current_ma = j.get("currentMa").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                let r_status = j.get("status").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let r_target_mv = j
                    .get("targetVoltageMv")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(voltage_mv as u64) as u16;
                let mut buf = vec![1u8]; // count = 1
                buf.push(r_rail_id);
                buf.push(r_enabled as u8);
                buf.extend_from_slice(&r_voltage_mv.to_le_bytes());
                buf.extend_from_slice(&r_current_ma.to_le_bytes());
                buf.push(r_status);
                buf.extend_from_slice(&r_target_mv.to_le_bytes());
                Ok(buf)
            }

            bbp::CMD_HAT_CALIBRATE_START => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload for CMD_HAT_CALIBRATE_START"));
                }
                let rail_id = payload[0];
                let j = self
                    .post_json(
                        "/api/hat/v2/calibrate/start",
                        &serde_json::json!({
                            "railId": rail_id,
                        }),
                    )
                    .await?;
                let status = j.get("status").and_then(|v| v.as_u64()).unwrap_or(1) as u8;
                Ok(vec![status])
            }

            bbp::CMD_HAT_CALIBRATE_STATUS => {
                let j = self.get_json("/api/hat/v2/calibrate/status").await?;
                let state = j.get("state").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let progress = j.get("progress").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let rail_id = j.get("railId").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let last_error = j.get("lastError").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let persist_state =
                    j.get("persistState").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let stage = j.get("stage").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let point = j.get("point").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let code = j.get("code").and_then(|v| v.as_i64()).unwrap_or(0) as i8 as u8;
                let measured_mv = j.get("measuredMv").and_then(|v| v.as_i64()).unwrap_or(-1) as i32;
                let min_mv = j.get("minMv").and_then(|v| v.as_i64()).unwrap_or(-1) as i32;
                let max_mv = j.get("maxMv").and_then(|v| v.as_i64()).unwrap_or(-1) as i32;
                let max_gap_mv = j.get("maxGapMv").and_then(|v| v.as_i64()).unwrap_or(-1) as i32;
                let max_error_mv =
                    j.get("maxErrorMv").and_then(|v| v.as_i64()).unwrap_or(-1) as i32;
                let validation_flags = j
                    .get("validationFlags")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(0) as u16;
                let mut buf = Vec::new();
                buf.push(state);
                buf.push(progress);
                buf.push(rail_id);
                buf.push(last_error);
                buf.push(persist_state);
                buf.push(stage);
                buf.push(point);
                buf.push(code);
                buf.extend_from_slice(&measured_mv.to_le_bytes());
                buf.extend_from_slice(&min_mv.to_le_bytes());
                buf.extend_from_slice(&max_mv.to_le_bytes());
                buf.extend_from_slice(&max_gap_mv.to_le_bytes());
                buf.extend_from_slice(&max_error_mv.to_le_bytes());
                buf.extend_from_slice(&validation_flags.to_le_bytes());
                Ok(buf)
            }

            bbp::CMD_HAT_CALIBRATE_IMPORT => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_CALIBRATE_IMPORT"));
                }
                let rail_id = payload[0];
                let count = payload[1] as usize;
                let mut points = Vec::new();
                let mut pos = 2usize;
                for _ in 0..count {
                    if pos + 5 > payload.len() {
                        break;
                    }
                    let dac_code = payload[pos] as i8;
                    let measured_v = f32::from_le_bytes([
                        payload[pos + 1],
                        payload[pos + 2],
                        payload[pos + 3],
                        payload[pos + 4],
                    ]);
                    pos += 5;
                    points.push(serde_json::json!({"dacCode": dac_code, "measuredV": measured_v}));
                }
                self.post_json(
                    "/api/hat/v2/calibrate/import",
                    &serde_json::json!({
                        "railId": rail_id,
                        "points": points,
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_SET_IO_BANK => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_IO_BANK"));
                }
                self.post_json(
                    "/api/hat/v2/io_bank",
                    &serde_json::json!({
                        "dirs": payload[0],
                        "ups": payload[1],
                        "dns": payload[2],
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_SET_LEVEL_SHIFT => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_LEVEL_SHIFT"));
                }
                let oe = payload[0] != 0;
                let dir = payload[1] != 0;
                let j = self
                    .post_json(
                        "/api/hat/v2/level_shift",
                        &serde_json::json!({
                            "oe": oe,
                            "dir": dir,
                        }),
                    )
                    .await?;
                let r_oe = j.get("oe").and_then(|v| v.as_bool()).unwrap_or(oe);
                let r_dir = j.get("dir").and_then(|v| v.as_bool()).unwrap_or(dir);
                Ok(vec![r_oe as u8, r_dir as u8])
            }

            bbp::CMD_HAT_SET_IO_VOLTAGE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_IO_VOLTAGE"));
                }
                let voltage_mv = u16::from_le_bytes([payload[0], payload[1]]);
                self.post_json(
                    "/api/hat/v2/io_voltage",
                    &serde_json::json!({
                        "voltageMv": voltage_mv,
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_SETUP_SWD => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SETUP_SWD"));
                }
                let target_voltage_mv = u16::from_le_bytes([payload[0], payload[1]]);
                let connector = payload[2];
                self.post_json(
                    "/api/hat/v2/swd/setup",
                    &serde_json::json!({
                        "targetVoltageMv": target_voltage_mv,
                        "connector": connector,
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_DETECT_TARGET => {
                let j = self
                    .post_json("/api/hat/v2/swd/detect", &serde_json::json!({}))
                    .await?;
                let detected = j.get("detected").and_then(|v| v.as_bool()).unwrap_or(false);
                let dpidr = j.get("dpidr").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
                let mut buf = vec![detected as u8];
                buf.extend_from_slice(&dpidr.to_le_bytes());
                Ok(buf)
            }

            bbp::CMD_HAT_SET_LED_STATE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_LED_STATE"));
                }
                self.post_json(
                    "/api/hat/v2/led",
                    &serde_json::json!({
                        "ledId": payload[0],
                        "colorCode": payload[1],
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_LA_SET_ROUTE => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload for CMD_HAT_LA_SET_ROUTE"));
                }
                let route = payload[0];
                let j = self
                    .post_json(
                        "/api/hat/v2/la/route",
                        &serde_json::json!({
                            "route": route,
                        }),
                    )
                    .await?;
                let r_route = j
                    .get("route")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(route as u64) as u8;
                Ok(vec![r_route])
            }

            bbp::CMD_HAT_LA_LOG_ENABLE => {
                if payload.is_empty() {
                    return Err(anyhow!("Invalid payload for CMD_HAT_LA_LOG_ENABLE"));
                }
                self.post_json(
                    "/api/hat/v2/la/log/enable",
                    &serde_json::json!({
                        "enable": payload[0] != 0,
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_LA_STATUS => {
                let j = self.get_json("/api/hat/la/status").await?;
                let state = j.get("state").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let channels = j.get("channels").and_then(|v| v.as_u64()).unwrap_or(4) as u8;
                let captured = j
                    .get("samplesCaptured")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(0) as u32;
                let total = j.get("totalSamples").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
                let rate = j.get("actualRateHz").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
                let usb_conn = j
                    .get("usbConnected")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                let usb_mnt = j
                    .get("usbMounted")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                let stop_reason = j.get("stopReason").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let mut buf = Vec::new();
                buf.push(state);
                buf.push(channels);
                buf.extend_from_slice(&captured.to_le_bytes());
                buf.extend_from_slice(&total.to_le_bytes());
                buf.extend_from_slice(&rate.to_le_bytes());
                buf.push(usb_conn as u8);
                buf.push(usb_mnt as u8);
                buf.push(stop_reason);
                Ok(buf)
            }

            bbp::CMD_HAT_LA_LOG_GET => {
                let j = self.get_json("/api/hat/v2/la/log").await?;
                let lines: Vec<&str> = j
                    .get("lines")
                    .and_then(|v| v.as_array())
                    .map(|arr| arr.iter().filter_map(|v| v.as_str()).collect())
                    .unwrap_or_default();
                // Encode as newline-separated UTF-8 bytes so commands.rs can split them back
                Ok(lines.join("\n").into_bytes())
            }

            bbp::CMD_HAT_SET_PIN => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_PIN"));
                }
                self.post_json(
                    "/api/hat/config",
                    &serde_json::json!({
                        "pin": payload[0],
                        "function": payload[1],
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_SET_ALL_PINS => {
                if payload.len() < 4 {
                    return Err(anyhow!("Invalid payload for CMD_HAT_SET_ALL_PINS"));
                }
                self.post_json(
                    "/api/hat/config",
                    &serde_json::json!({
                        "pins": [payload[0], payload[1], payload[2], payload[3]],
                    }),
                )
                .await?;
                Ok(vec![])
            }

            bbp::CMD_HAT_RESET => {
                self.post_json("/api/hat/reset", &serde_json::json!({}))
                    .await?;
                Ok(vec![])
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

#[cfg(test)]
mod tests {
    use super::{encode_gpio_status_payload, encode_selftest_supplies_cached_payload};
    use serde_json::json;

    #[test]
    fn encode_gpio_status_payload_serializes_all_twelve_pins() {
        let gpios = (0..12)
            .map(|i| {
                json!({
                    "id": i,
                    "mode": i % 5,
                    "output": i % 2 == 0,
                    "input": i % 3 == 0,
                    "pulldown": i % 4 == 0,
                })
            })
            .collect::<Vec<_>>();

        let buf = encode_gpio_status_payload(Some(&gpios));

        assert_eq!(buf.len(), 60);
        assert_eq!(&buf[0..5], &[0, 0, 1, 1, 1]);
        assert_eq!(&buf[55..60], &[11, 1, 0, 0, 0]);
    }

    #[test]
    fn encode_selftest_supplies_cached_payload_serializes_three_rails() {
        let json = json!({
            "available": true,
            "timestampMs": 12345,
            "rails": [
                {"rail": 0, "name": "VADJ1", "voltageV": 12.0},
                {"rail": 1, "name": "VADJ2", "voltageV": 5.0},
                {"rail": 2, "name": "VLOGIC", "voltageV": 3.3},
            ],
        });

        let buf = encode_selftest_supplies_cached_payload(&json);

        assert_eq!(buf.len(), 17);
        assert_eq!(buf[0], 1);
        assert_eq!(u32::from_le_bytes([buf[1], buf[2], buf[3], buf[4]]), 12345);
        assert!((f32::from_le_bytes([buf[5], buf[6], buf[7], buf[8]]) - 12.0).abs() < 1e-6);
        assert!((f32::from_le_bytes([buf[9], buf[10], buf[11], buf[12]]) - 5.0).abs() < 1e-6);
        assert!((f32::from_le_bytes([buf[13], buf[14], buf[15], buf[16]]) - 3.3).abs() < 1e-6);
    }
}
