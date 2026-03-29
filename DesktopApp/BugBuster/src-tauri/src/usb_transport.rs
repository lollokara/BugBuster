// =============================================================================
// usb_transport.rs - USB CDC transport using BBP binary protocol
// =============================================================================

use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{anyhow, Result};
use async_trait::async_trait;
use tokio::sync::oneshot;

use crate::bbp::{self, FrameAccumulator, HandshakeInfo, Message};
use crate::state::DeviceState;
use crate::transport::Transport;

/// Pending command awaiting a response, keyed by sequence number.
struct PendingCommand {
    seq: u16,
    tx: oneshot::Sender<Message>,
}

pub struct UsbTransport {
    connected: Arc<AtomicBool>,
    seq_counter: AtomicU16,
    port_name: String,
    handshake_info: Option<HandshakeInfo>,
    // Serial port writer (shared with reader thread)
    writer: Arc<Mutex<Option<Box<dyn serialport::SerialPort>>>>,
    // Pending responses
    pending: Arc<Mutex<Vec<PendingCommand>>>,
    // Event callback for streaming data
    event_tx: Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<Message>>>>,
    // Reader thread handle
    _reader_handle: Option<std::thread::JoinHandle<()>>,
}

impl UsbTransport {
    /// Connect to a serial port and perform BBP handshake.
    pub fn connect(
        port_name: &str,
        event_tx: tokio::sync::mpsc::UnboundedSender<Message>,
    ) -> Result<Self> {
        // Open serial port
        let port = serialport::new(port_name, 115200) // Baud rate ignored for CDC
            .timeout(Duration::from_millis(100))
            .open()?;

        let mut writer_port = port.try_clone()?;
        let mut reader_port = port;

        // Drain any pending data (boot messages, CLI prompt, etc.)
        {
            let mut drain = [0u8; 1024];
            loop {
                match reader_port.read(&mut drain) {
                    Ok(n) if n > 0 => {
                        log::debug!("Drained {} bytes: {:?}", n, std::str::from_utf8(&drain[..n]).unwrap_or("(binary)"));
                    }
                    _ => break,
                }
            }
        }

        // Send handshake magic
        log::info!("Sending BBP handshake magic: {:02X?}", bbp::MAGIC);
        writer_port.write_all(&bbp::MAGIC)?;
        writer_port.flush()?;

        // Read response bytes, scanning for the magic pattern.
        // The CLI may echo some of our magic bytes before the real response arrives.
        let mut ring = Vec::with_capacity(64);
        let deadline = std::time::Instant::now() + Duration::from_millis(2000);
        let mut found_offset: Option<usize> = None;

        while std::time::Instant::now() < deadline {
            let mut tmp = [0u8; 32];
            match reader_port.read(&mut tmp) {
                Ok(n) if n > 0 => {
                    ring.extend_from_slice(&tmp[..n]);
                    // Search for magic pattern in accumulated bytes
                    if ring.len() >= bbp::HANDSHAKE_RSP_LEN {
                        for i in 0..=(ring.len() - bbp::HANDSHAKE_RSP_LEN) {
                            if ring[i..i+4] == bbp::MAGIC {
                                found_offset = Some(i);
                                break;
                            }
                        }
                        if let Some(off) = found_offset {
                            // We might need more bytes after the magic
                            if ring.len() >= off + bbp::HANDSHAKE_RSP_LEN {
                                break;
                            }
                        }
                    }
                }
                Ok(_) => {}
                Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => continue,
                Err(e) => return Err(e.into()),
            }
        }

        log::info!("Handshake raw stream ({} bytes): {:02X?}", ring.len(), ring);

        let off = found_offset.ok_or_else(|| {
            anyhow!("Handshake magic not found in response: {:02X?}", ring)
        })?;
        let rsp = &ring[off..off + bbp::HANDSHAKE_RSP_LEN];
        let handshake_info = HandshakeInfo::parse(rsp)
            .ok_or_else(|| anyhow!("Invalid handshake at offset {}: {:02X?}", off, rsp))?;

        log::info!(
            "BBP handshake OK: proto v{}, fw v{}.{}.{}",
            handshake_info.proto_version,
            handshake_info.fw_major,
            handshake_info.fw_minor,
            handshake_info.fw_patch,
        );

        let connected = Arc::new(AtomicBool::new(true));
        let writer = Arc::new(Mutex::new(
            Some(writer_port) as Option<Box<dyn serialport::SerialPort>>
        ));
        let pending: Arc<Mutex<Vec<PendingCommand>>> = Arc::new(Mutex::new(Vec::new()));
        let event_tx = Arc::new(Mutex::new(Some(event_tx)));

        // Spawn background reader thread
        let reader_connected = connected.clone();
        let reader_pending = pending.clone();
        let reader_event_tx = event_tx.clone();

        let reader_handle = std::thread::Builder::new()
            .name("bbp-reader".into())
            .spawn(move || {
                let mut acc = FrameAccumulator::new();
                let mut buf = [0u8; 1024];

                while reader_connected.load(Ordering::Relaxed) {
                    match reader_port.read(&mut buf) {
                        Ok(n) if n > 0 => {
                            let messages = acc.feed(&buf[..n]);
                            for msg in messages {
                                if msg.is_response() || msg.is_error() {
                                    // Match to pending command by seq
                                    if let Ok(mut pend) = reader_pending.lock() {
                                        if let Some(idx) = pend.iter().position(|p| p.seq == msg.seq) {
                                            let p = pend.remove(idx);
                                            let _ = p.tx.send(msg);
                                        }
                                    }
                                } else if msg.is_event() {
                                    // Forward events
                                    if let Ok(guard) = reader_event_tx.lock() {
                                        if let Some(tx) = guard.as_ref() {
                                            let _ = tx.send(msg);
                                        }
                                    }
                                }
                            }
                        }
                        Ok(_) => {} // 0 bytes
                        Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                        Err(e) => {
                            log::error!("Serial read error: {}", e);
                            reader_connected.store(false, Ordering::Relaxed);
                            break;
                        }
                    }
                }
                log::info!("BBP reader thread exiting");
            })?;

        Ok(Self {
            connected,
            seq_counter: AtomicU16::new(1),
            port_name: port_name.to_string(),
            handshake_info: Some(handshake_info),
            writer,
            pending,
            event_tx,
            _reader_handle: Some(reader_handle),
        })
    }

    pub fn handshake_info(&self) -> Option<&HandshakeInfo> {
        self.handshake_info.as_ref()
    }

    fn next_seq(&self) -> u16 {
        self.seq_counter.fetch_add(1, Ordering::Relaxed)
    }
}

#[async_trait]
impl Transport for UsbTransport {
    async fn send_command(&self, cmd_id: u8, payload: &[u8]) -> Result<Vec<u8>> {
        if !self.connected.load(Ordering::Relaxed) {
            return Err(anyhow!("Not connected"));
        }

        let seq = self.next_seq();
        let frame = Message::build_frame(seq, cmd_id, payload);

        // Register pending response
        let (tx, rx) = oneshot::channel();
        {
            let mut pend = self.pending.lock().map_err(|_| anyhow!("Pending lock poisoned"))?;
            pend.push(PendingCommand { seq, tx });
        }

        // Write frame to serial port
        {
            let mut writer_lock = self.writer.lock().map_err(|_| anyhow!("Writer lock poisoned"))?;
            if let Some(ref mut port) = *writer_lock {
                port.write_all(&frame)?;
                port.flush()?;
            } else {
                return Err(anyhow!("Port closed"));
            }
        }

        // Wait for response with timeout (2s for MUX commands that need SPI bus yield)
        let response = tokio::time::timeout(Duration::from_millis(2000), rx).await;

        match response {
            Ok(Ok(msg)) => {
                if msg.is_error() {
                    let err_code = msg.error_code().unwrap_or(0);
                    Err(anyhow!("Device error 0x{:02X} for cmd 0x{:02X}", err_code, cmd_id))
                } else {
                    Ok(msg.payload)
                }
            }
            Ok(Err(_)) => Err(anyhow!("Response channel closed")),
            Err(_) => {
                // Remove from pending on timeout
                if let Ok(mut pend) = self.pending.lock() {
                    pend.retain(|p| p.seq != seq);
                }
                Err(anyhow!("Command timeout (cmd=0x{:02X})", cmd_id))
            }
        }
    }

    async fn get_status(&self) -> Result<DeviceState> {
        let payload = self.send_command(bbp::CMD_GET_STATUS, &[]).await?;
        DeviceState::from_status_payload(&payload)
            .ok_or_else(|| anyhow!("Failed to parse status response"))
    }

    fn is_connected(&self) -> bool {
        self.connected.load(Ordering::Relaxed)
    }

    async fn disconnect(&self) -> Result<()> {
        if self.connected.load(Ordering::Relaxed) {
            // Send DISCONNECT command (best effort)
            let _ = self.send_command(bbp::CMD_DISCONNECT, &[]).await;
        }
        self.connected.store(false, Ordering::Relaxed);

        // Close the port
        if let Ok(mut writer_lock) = self.writer.lock() {
            *writer_lock = None;
        }

        // Drop event sender
        if let Ok(mut evt) = self.event_tx.lock() {
            *evt = None;
        }

        log::info!("USB transport disconnected from {}", self.port_name);
        Ok(())
    }

    fn transport_name(&self) -> &str {
        "USB"
    }
}

impl Drop for UsbTransport {
    fn drop(&mut self) {
        self.connected.store(false, Ordering::Relaxed);
        if let Ok(mut writer_lock) = self.writer.lock() {
            *writer_lock = None;
        }
    }
}
