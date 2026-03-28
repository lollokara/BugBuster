// =============================================================================
// http_transport.rs - HTTP REST API transport (fallback when USB not available)
// =============================================================================

use std::sync::atomic::{AtomicBool, Ordering};

use anyhow::{anyhow, Result};
use async_trait::async_trait;
use reqwest::Client;
use serde_json::Value;

use crate::bbp;
use crate::state::{ChannelState, DeviceState};
use crate::transport::Transport;

pub struct HttpTransport {
    client: Client,
    base_url: String,
    connected: AtomicBool,
}

impl HttpTransport {
    /// Connect to the device via HTTP. Verifies connectivity with /api/device/info.
    pub async fn connect(base_url: &str) -> Result<Self> {
        let client = Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .build()?;

        // Verify the device is reachable
        let url = format!("{}/api/device/info", base_url);
        let resp = client.get(&url).send().await?;
        if !resp.status().is_success() {
            return Err(anyhow!("Device returned HTTP {}", resp.status()));
        }

        // Parse response to confirm it's a BugBuster
        let info: Value = resp.json().await?;
        if info.get("spiOk").is_none() {
            return Err(anyhow!("Not a BugBuster device"));
        }

        log::info!("HTTP transport connected to {}", base_url);

        Ok(Self {
            client,
            base_url: base_url.to_string(),
            connected: AtomicBool::new(true),
        })
    }

    async fn get_json(&self, path: &str) -> Result<Value> {
        let url = format!("{}{}", self.base_url, path);
        let resp = self.client.get(&url).send().await?;
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

    /// Parse /api/status JSON into DeviceState
    fn parse_status_json(json: &Value) -> Option<DeviceState> {
        let mut state = DeviceState::default();

        state.spi_ok = json.get("spiOk")?.as_bool()?;
        state.die_temperature = json.get("dieTemp")?.as_f64()? as f32;
        state.alert_status = json.get("alertStatus")?.as_u64()? as u16;
        state.alert_mask = json.get("alertMask")?.as_u64()? as u16;
        state.supply_alert_status = json.get("supplyAlertStatus")?.as_u64()? as u16;
        state.supply_alert_mask = json.get("supplyAlertMask")?.as_u64()? as u16;
        state.live_status = json.get("liveStatus")?.as_u64()? as u16;

        let channels = json.get("channels")?.as_array()?;
        for (i, ch_json) in channels.iter().enumerate().take(4) {
            state.channels[i] = ChannelState {
                function: ch_json.get("function")?.as_u64()? as u8,
                adc_raw: ch_json.get("adcRaw")?.as_u64()? as u32,
                adc_value: ch_json.get("adcValue")?.as_f64()? as f32,
                adc_range: ch_json.get("adcRange")?.as_u64()? as u8,
                adc_rate: ch_json.get("adcRate")?.as_u64()? as u8,
                adc_mux: ch_json.get("adcMux")?.as_u64()? as u8,
                dac_code: ch_json.get("dacCode")?.as_u64()? as u16,
                dac_value: ch_json.get("dacValue")?.as_f64()? as f32,
                din_state: ch_json.get("dinState")?.as_bool().unwrap_or(false),
                din_counter: ch_json.get("dinCounter")?.as_u64().unwrap_or(0) as u32,
                do_state: ch_json.get("doState")?.as_bool().unwrap_or(false),
                channel_alert: ch_json.get("channelAlert")?.as_u64()? as u16,
            };
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
            bbp::CMD_SET_CH_FUNC => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let func = payload[1];
                let body = serde_json::json!({"function": func});
                self.post_json(&format!("/api/channel/{}/function", ch), &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DAC_CODE => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let code = u16::from_le_bytes([payload[1], payload[2]]);
                let body = serde_json::json!({"code": code});
                self.post_json(&format!("/api/channel/{}/dac", ch), &body).await?;
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
                self.post_json(&format!("/api/channel/{}/dac", ch), &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DAC_CURRENT => {
                if payload.len() < 5 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let current = f32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                let body = serde_json::json!({"current_mA": current});
                self.post_json(&format!("/api/channel/{}/dac", ch), &body).await?;
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
                self.post_json(&format!("/api/channel/{}/adc/config", ch), &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_CLEAR_ALL_ALERTS => {
                self.post_json("/api/faults/clear", &serde_json::json!({})).await?;
                Ok(vec![])
            }

            bbp::CMD_DEVICE_RESET => {
                self.post_json("/api/device/reset", &serde_json::json!({})).await?;
                Ok(vec![])
            }

            bbp::CMD_SET_GPIO_CONFIG => {
                if payload.len() < 3 {
                    return Err(anyhow!("Invalid payload"));
                }
                let gpio = payload[0];
                let body = serde_json::json!({"mode": payload[1], "pulldown": payload[2] != 0});
                self.post_json(&format!("/api/gpio/{}/config", gpio), &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_GPIO_VALUE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let gpio = payload[0];
                let body = serde_json::json!({"value": payload[1] != 0});
                self.post_json(&format!("/api/gpio/{}/set", gpio), &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_DO_STATE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({"on": payload[1] != 0});
                self.post_json(&format!("/api/channel/{}/do/set", ch), &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_VOUT_RANGE => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({"bipolar": payload[1] != 0});
                self.post_json(&format!("/api/channel/{}/vout/range", ch), &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_SET_ILIMIT => {
                if payload.len() < 2 {
                    return Err(anyhow!("Invalid payload"));
                }
                let ch = payload[0];
                let body = serde_json::json!({"limit8mA": payload[1] != 0});
                self.post_json(&format!("/api/channel/{}/ilimit", ch), &body).await?;
                Ok(payload.to_vec())
            }

            // DS4424 IDAC (HTTP passthrough via JSON API)
            bbp::CMD_IDAC_GET_STATUS => {
                let json = self.get_json("/api/idac").await?;
                Ok(serde_json::to_vec(&json).unwrap_or_default())
            }

            bbp::CMD_IDAC_SET_CODE => {
                if payload.len() < 2 { return Err(anyhow!("Invalid payload")); }
                let body = serde_json::json!({"ch": payload[0], "code": payload[1] as i8});
                self.post_json("/api/idac/code", &body).await?;
                Ok(payload.to_vec())
            }

            bbp::CMD_IDAC_SET_VOLTAGE => {
                if payload.len() < 5 { return Err(anyhow!("Invalid payload")); }
                let ch = payload[0];
                let voltage = f32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                let body = serde_json::json!({"ch": ch, "voltage": voltage});
                self.post_json("/api/idac/voltage", &body).await?;
                Ok(payload.to_vec())
            }

            // PCA9535 GPIO Expander
            bbp::CMD_PCA_GET_STATUS => {
                let json = self.get_json("/api/ioexp").await?;
                Ok(serde_json::to_vec(&json).unwrap_or_default())
            }

            bbp::CMD_PCA_SET_CONTROL => {
                if payload.len() < 2 { return Err(anyhow!("Invalid payload")); }
                let ctrl_names = ["vadj1","vadj2","15v","mux","usb","efuse1","efuse2","efuse3","efuse4"];
                let idx = payload[0] as usize;
                let name = if idx < ctrl_names.len() { ctrl_names[idx] } else { "?" };
                let body = serde_json::json!({"control": name, "on": payload[1] != 0});
                self.post_json("/api/ioexp/control", &body).await?;
                Ok(payload.to_vec())
            }

            // HUSB238 USB PD
            bbp::CMD_USBPD_GET_STATUS => {
                let json = self.get_json("/api/usbpd").await?;
                Ok(serde_json::to_vec(&json).unwrap_or_default())
            }

            bbp::CMD_USBPD_SELECT_PDO => {
                if payload.is_empty() { return Err(anyhow!("Invalid payload")); }
                let v_map = [0, 5, 9, 12, 15, 18, 20];
                let v = if (payload[0] as usize) < v_map.len() { v_map[payload[0] as usize] } else { 0 };
                let body = serde_json::json!({"voltage": v});
                self.post_json("/api/usbpd/select", &body).await?;
                Ok(payload.to_vec())
            }

            // Streaming not supported over HTTP
            bbp::CMD_START_ADC_STREAM | bbp::CMD_STOP_ADC_STREAM |
            bbp::CMD_START_SCOPE_STREAM | bbp::CMD_STOP_SCOPE_STREAM => {
                Err(anyhow!("Streaming not supported over HTTP"))
            }

            _ => {
                Err(anyhow!("Command 0x{:02X} not implemented for HTTP transport", cmd_id))
            }
        }
    }

    async fn get_status(&self) -> Result<DeviceState> {
        let json = self.get_json("/api/status").await?;
        Self::parse_status_json(&json)
            .ok_or_else(|| anyhow!("Failed to parse HTTP status response"))
    }

    fn is_connected(&self) -> bool {
        self.connected.load(Ordering::Relaxed)
    }

    async fn disconnect(&self) -> Result<()> {
        self.connected.store(false, Ordering::Relaxed);
        log::info!("HTTP transport disconnected from {}", self.base_url);
        Ok(())
    }

    fn transport_name(&self) -> &str {
        "HTTP"
    }
}
