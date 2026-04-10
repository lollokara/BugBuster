//! Transport abstraction for LA USB bulk communication.
//!
//! The `LaTransport` trait decouples the streaming loop from the concrete
//! USB implementation, enabling unit testing without hardware.

use anyhow::Result;
use std::sync::{Arc, Mutex};

use crate::la_usb::{LaStreamPacket, LaUsbConnection};

/// Transport abstraction for LA USB bulk.
///
/// # Contract
/// Implementors MUST NOT hold external locks across calls. Each call must
/// acquire and release its resources independently so that concurrent callers
/// (e.g. `la_stream_usb_stop`) can acquire the same lock without deadlocking.
pub trait LaTransport: Send + 'static {
    /// Send a single-byte command to the bulk OUT endpoint.
    fn send_command(&mut self, cmd: u8) -> Result<()>;

    /// Block until one stream packet is received from the bulk IN endpoint.
    ///
    /// Returns the parsed packet or an error (USB disconnect, timeout, parse failure).
    fn read_packet(&mut self) -> Result<LaStreamPacket>;
}

/// Real transport: wraps `Arc<Mutex<LaUsbConnection>>` with lock-per-call semantics.
///
/// Each call to `send_command` or `read_packet` acquires the mutex, performs
/// the operation, and immediately releases it. This allows `la_stream_usb_stop`
/// to send a STOP command between read iterations without deadlocking.
pub struct LockedLaTransport {
    pub usb: Arc<Mutex<LaUsbConnection>>,
}

impl LaTransport for LockedLaTransport {
    fn send_command(&mut self, cmd: u8) -> Result<()> {
        self.usb
            .lock()
            .map_err(|e| anyhow::anyhow!("USB mutex poisoned: {e}"))?
            .send_command(cmd)
    }

    fn read_packet(&mut self) -> Result<LaStreamPacket> {
        self.usb
            .lock()
            .map_err(|e| anyhow::anyhow!("USB mutex poisoned: {e}"))?
            .read_stream_packet_blocking()
    }
}

/// Reason why a streaming session terminated.
#[derive(Debug, PartialEq)]
pub enum StreamStopReason {
    /// Device sent a STOP packet normally.
    Normal,
    /// Host requested stop (sent STOP command, device acknowledged).
    HostStopped,
    /// USB read/write error (disconnect, timeout, etc.).
    UsbError(String),
    /// Sequence number gap detected — firmware may have dropped packets.
    SeqMismatch { expected: u8, got: u8 },
    /// Device sent an ERROR packet with the given info byte.
    FirmwareError(u8),
}

/// Mock transport for unit tests — driven by a pre-loaded packet queue.
///
/// Instantiate with `MockLaTransport::new(packets)` where packets is a
/// `Vec<Result<LaStreamPacket>>`. `read_packet()` pops from the front;
/// when the queue is empty it returns an error to terminate the loop.
#[cfg(test)]
pub struct MockLaTransport {
    pub packets: std::collections::VecDeque<Result<LaStreamPacket>>,
    pub commands_sent: Vec<u8>,
}

#[cfg(test)]
impl MockLaTransport {
    pub fn new(packets: Vec<Result<LaStreamPacket>>) -> Self {
        Self {
            packets: packets.into_iter().collect(),
            commands_sent: Vec::new(),
        }
    }
}

#[cfg(test)]
impl LaTransport for MockLaTransport {
    fn send_command(&mut self, cmd: u8) -> Result<()> {
        self.commands_sent.push(cmd);
        Ok(())
    }

    fn read_packet(&mut self) -> Result<LaStreamPacket> {
        self.packets
            .pop_front()
            .unwrap_or_else(|| Err(anyhow::anyhow!("MockLaTransport: packet queue exhausted")))
    }
}
