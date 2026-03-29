// =============================================================================
// transport.rs - Transport trait abstracting USB (BBP) vs HTTP communication
// =============================================================================

use anyhow::Result;
use async_trait::async_trait;

use crate::state::DeviceState;

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
    fn base_url(&self) -> Option<String> { None }
}
