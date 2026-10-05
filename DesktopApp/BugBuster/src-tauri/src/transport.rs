// =============================================================================
// transport.rs - Transport trait abstracting USB (BBP) vs HTTP communication
// =============================================================================

use anyhow::Result;
use async_trait::async_trait;
use std::collections::HashMap;
use std::time::Duration;

use crate::state::DeviceState;

/// One raw request to the device's REST API (HTTP transport only).
pub struct HttpExchange {
    pub method: &'static str,
    pub path: String,
    pub query: Vec<(String, String)>,
    /// Body bytes and their content type.
    pub body: Option<(Vec<u8>, &'static str)>,
    pub timeout: Duration,
}

/// Raw reply: header names are lower-cased.
pub struct HttpReply {
    pub status: u16,
    pub headers: HashMap<String, String>,
    pub body: Vec<u8>,
}

/// Abstraction over USB (BBP binary protocol) and HTTP (REST API) transports.
/// Both implement the same device operations; the connection manager picks
/// whichever is available (USB preferred).
#[async_trait]
pub trait Transport: Send + Sync {
    /// Send a raw BBP command and return the response payload.
    /// For HTTP transport, this maps to the equivalent REST endpoint.
    async fn send_command(&self, cmd_id: u8, payload: &[u8]) -> Result<Vec<u8>>;

    /// Poll full device status. Returns parsed DeviceState.
    async fn get_status(&self) -> Result<DeviceState>;

    /// Check if the transport is still connected.
    fn is_connected(&self) -> bool;

    /// Disconnect and clean up resources.
    async fn disconnect(&self) -> Result<()>;

    /// Transport type name for display.
    fn transport_name(&self) -> &str;

    /// HTTP base URL (only for HTTP transport, None for USB).
    fn base_url(&self) -> Option<String> {
        None
    }

    /// Raw REST exchange for features without a BBP mapping (scripting). USB has none:
    /// callers must tunnel over BBP instead of falling back to Wi-Fi.
    async fn http_exchange(&self, _req: HttpExchange) -> Result<HttpReply> {
        Err(anyhow::anyhow!("not an HTTP transport"))
    }
}
