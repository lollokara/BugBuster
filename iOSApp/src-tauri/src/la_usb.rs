// =============================================================================
// la_usb.rs — Logic Analyzer USB types (iOS stub)
// =============================================================================

use anyhow::Result;

pub const STREAM_PKT_START: u8 = 0x01;
pub const STREAM_PKT_DATA: u8 = 0x02;
pub const STREAM_PKT_STOP: u8 = 0x03;
pub const STREAM_PKT_ERROR: u8 = 0x04;

pub const STREAM_INFO_START_REJECTED: u8 = 0x80;
pub const STREAM_MAX_PAYLOAD: usize = 60;

/// Captured LA data with metadata
#[derive(Debug, Clone, Default, serde::Serialize, serde::Deserialize)]
pub struct LaCaptureData {
    pub channels: u8,
    pub sample_rate_hz: u32,
    pub total_samples: u32,
    pub raw_data: Vec<u8>,
    pub channel_data: Vec<Vec<u8>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LaStreamPacketKind {
    Start,
    Data,
    Stop,
    Error,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaStreamPacket {
    pub kind: LaStreamPacketKind,
    pub seq: u8,
    pub info: u8,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LaStreamPacketError {
    ShortHeader(usize),
    InvalidPacketType(u8),
    InvalidPayloadLength(usize),
    TruncatedPayload { announced: usize, actual: usize },
}

pub fn parse_stream_packet(buf: &[u8]) -> Result<LaStreamPacket, LaStreamPacketError> {
    if buf.len() < 4 {
        return Err(LaStreamPacketError::ShortHeader(buf.len()));
    }

    let kind = match buf[0] {
        STREAM_PKT_START => LaStreamPacketKind::Start,
        STREAM_PKT_DATA => LaStreamPacketKind::Data,
        STREAM_PKT_STOP => LaStreamPacketKind::Stop,
        STREAM_PKT_ERROR => LaStreamPacketKind::Error,
        other => return Err(LaStreamPacketError::InvalidPacketType(other)),
    };
    let seq = buf[1];
    let payload_len = buf[2] as usize;
    let info = buf[3];

    if payload_len > STREAM_MAX_PAYLOAD {
        return Err(LaStreamPacketError::InvalidPayloadLength(payload_len));
    }

    let frame_len = 4 + payload_len;
    if buf.len() < frame_len {
        return Err(LaStreamPacketError::TruncatedPayload {
            announced: payload_len,
            actual: buf.len().saturating_sub(4),
        });
    }

    Ok(LaStreamPacket {
        kind,
        seq,
        info,
        payload: buf[4..frame_len].to_vec(),
    })
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub enum DeviceSelector {
    Any,
    SerialNumber(String),
}

pub struct LaUsbConnection {
    connected: bool,
}

impl LaUsbConnection {
    pub fn new() -> Self {
        Self { connected: false }
    }
    pub fn connect(&mut self, _selector: Option<DeviceSelector>) -> Result<()> {
        Err(anyhow::anyhow!("USB not supported on iOS"))
    }
    pub fn is_connected(&self) -> bool {
        false
    }
    pub fn close(&mut self) -> Result<()> {
        Ok(())
    }
    pub fn disconnect(&mut self) -> Result<()> {
        Ok(())
    }
    pub fn read_capture_blocking(&mut self) -> Result<Vec<u8>> {
        Err(anyhow::anyhow!("USB not supported on iOS"))
    }
    pub fn read_stream_packet_blocking(&mut self) -> Result<LaStreamPacket> {
        Err(anyhow::anyhow!("USB not supported on iOS"))
    }
}

pub fn decode_capture(raw: &[u8], channels: u8) -> Vec<Vec<u8>> {
    let mut result: Vec<Vec<u8>> = (0..channels).map(|_| Vec::new()).collect();
    let bits_per_sample = channels as usize;

    for &byte in raw {
        let mut bit_pos = 0;
        while bit_pos < 8 {
            for ch in 0..channels as usize {
                if bit_pos + ch < 8 {
                    result[ch].push((byte >> (bit_pos + ch)) & 1);
                }
            }
            bit_pos += bits_per_sample;
        }
    }

    result
}

pub fn hat_usb_present() -> bool {
    false
}
