// =============================================================================
// la_usb.rs — Logic Analyzer USB vendor-bulk transport
//
// Interface 3 is the BugBuster HAT LA vendor interface.
// One-shot capture readout keeps the legacy `[total_len:u32][raw bytes...]`
// format. Live streaming uses packetized vendor-bulk frames:
//   [packet_type:u8][seq:u8][payload_len:u8][info:u8][payload bytes...]
// =============================================================================

use anyhow::{anyhow, Result};
use futures::executor::block_on;
use log::{info, warn};
use nusb::transfer::RequestBuffer;

const BB_HAT_VID: u16 = 0x2E8A;
const BB_HAT_PID: u16 = 0x000C;
const LA_INTERFACE_NUM: u8 = 3;
const LA_EP_IN: u8 = 0x87;
const LA_EP_OUT: u8 = 0x06;

const STREAM_CMD_START: u8 = 0x01;

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

/// LA USB connection state
pub struct LaUsbConnection {
    interface: Option<nusb::Interface>,
    connected: bool,
}

impl LaUsbConnection {
    pub fn new() -> Self {
        Self {
            interface: None,
            connected: false,
        }
    }

    pub fn connect(&mut self) -> Result<()> {
        let devices = nusb::list_devices().map_err(|e| anyhow!("USB enumeration failed: {}", e))?;

        for dev_info in devices {
            if dev_info.vendor_id() == BB_HAT_VID && dev_info.product_id() == BB_HAT_PID {
                info!(
                    "Found BugBuster HAT USB device: bus={} addr={}",
                    dev_info.bus_number(),
                    dev_info.device_address()
                );

                let device = dev_info
                    .open()
                    .map_err(|e| anyhow!("Failed to open USB device: {}", e))?;

                let iface = device.claim_interface(LA_INTERFACE_NUM).map_err(|e| {
                    anyhow!("Failed to claim LA interface {}: {}", LA_INTERFACE_NUM, e)
                })?;

                info!("LA USB interface claimed (interface {})", LA_INTERFACE_NUM);

                if let Ok(config) = device.active_configuration() {
                    for iface_group in config.interface_alt_settings() {
                        if iface_group.interface_number() == LA_INTERFACE_NUM {
                            info!(
                                "LA iface {} alt={} class={:?} subclass={} protocol={}",
                                iface_group.interface_number(),
                                iface_group.alternate_setting(),
                                iface_group.class(),
                                iface_group.subclass(),
                                iface_group.protocol()
                            );
                            for ep in iface_group.endpoints() {
                                info!(
                                    "  EP 0x{:02X} dir={:?} type={:?} max_packet={}",
                                    ep.address(),
                                    ep.direction(),
                                    ep.transfer_type(),
                                    ep.max_packet_size()
                                );
                            }
                        }
                    }
                }

                self.interface = Some(iface);
                self.connected = true;
                return Ok(());
            }
        }

        Err(anyhow!(
            "BugBuster HAT not found on USB (VID={:04X} PID={:04X})",
            BB_HAT_VID,
            BB_HAT_PID
        ))
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn read_capture_blocking(&mut self) -> Result<Vec<u8>> {
        let iface = self
            .interface
            .as_ref()
            .ok_or_else(|| anyhow!("LA USB not connected"))?;

        let header_completion = block_on(iface.bulk_in(LA_EP_IN, RequestBuffer::new(64)));
        let header_result = header_completion
            .into_result()
            .map_err(|e| anyhow!("USB bulk read header failed: {}", e))?;

        if header_result.len() < 4 {
            return Err(anyhow!("Short header: {} bytes", header_result.len()));
        }

        let total_len = u32::from_le_bytes([
            header_result[0],
            header_result[1],
            header_result[2],
            header_result[3],
        ]) as usize;

        info!("LA USB: reading {} bytes of capture data", total_len);

        let mut data = Vec::with_capacity(total_len);
        if header_result.len() > 4 {
            data.extend_from_slice(&header_result[4..]);
        }

        while data.len() < total_len {
            let completion = block_on(iface.bulk_in(LA_EP_IN, RequestBuffer::new(64)));
            let result = completion
                .into_result()
                .map_err(|e| anyhow!("USB bulk read failed at offset {}: {}", data.len(), e))?;

            if result.is_empty() {
                warn!(
                    "USB bulk read returned 0 bytes at offset {}/{}",
                    data.len(),
                    total_len
                );
                break;
            }
            data.extend_from_slice(&result);
        }

        data.truncate(total_len);
        info!("LA USB: received {} bytes", data.len());
        Ok(data)
    }

    pub fn read_stream_packet_blocking(&mut self) -> Result<LaStreamPacket> {
        let iface = self
            .interface
            .as_ref()
            .ok_or_else(|| anyhow!("LA USB not connected"))?;

        let completion = block_on(iface.bulk_in(LA_EP_IN, RequestBuffer::new(64)));
        let result = completion
            .into_result()
            .map_err(|e| anyhow!("USB live bulk read failed: {}", e))?;

        if result.is_empty() {
            return Err(anyhow!("USB live bulk read returned 0 bytes"));
        }

        parse_stream_packet(&result)
            .map_err(|e| anyhow!("Invalid live bulk packet {:?}: {:02X?}", e, result))
    }

    pub fn send_command(&self, cmd: u8) -> Result<()> {
        let iface = self
            .interface
            .as_ref()
            .ok_or_else(|| anyhow!("LA USB not connected"))?;

        let completion = block_on(iface.bulk_out(LA_EP_OUT, vec![cmd]));
        completion
            .into_result()
            .map_err(|e| anyhow!("USB bulk OUT failed: {}", e))?;
        Ok(())
    }

    pub fn send_stream_start(&self) -> Result<()> {
        info!("LA USB: sending stream start command");
        self.send_command(STREAM_CMD_START)
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
    if let Ok(devices) = nusb::list_devices() {
        devices
            .into_iter()
            .any(|d| d.vendor_id() == BB_HAT_VID && d.product_id() == BB_HAT_PID)
    } else {
        false
    }
}

#[cfg(test)]
mod tests {
    use super::{
        parse_stream_packet, LaStreamPacketError, LaStreamPacketKind, STREAM_INFO_START_REJECTED,
        STREAM_PKT_DATA, STREAM_PKT_ERROR, STREAM_PKT_START, STREAM_PKT_STOP,
    };

    #[test]
    fn parses_start_packet() {
        let pkt = parse_stream_packet(&[STREAM_PKT_START, 0x00, 0x00, 0x00]).unwrap();
        assert_eq!(pkt.kind, LaStreamPacketKind::Start);
        assert!(pkt.payload.is_empty());
    }

    #[test]
    fn parses_data_packet() {
        let pkt =
            parse_stream_packet(&[STREAM_PKT_DATA, 0x05, 0x03, 0x00, 0xAA, 0xBB, 0xCC]).unwrap();
        assert_eq!(pkt.kind, LaStreamPacketKind::Data);
        assert_eq!(pkt.seq, 0x05);
        assert_eq!(pkt.payload, vec![0xAA, 0xBB, 0xCC]);
    }

    #[test]
    fn parses_stop_packet() {
        let pkt = parse_stream_packet(&[STREAM_PKT_STOP, 0x09, 0x00, 0x01]).unwrap();
        assert_eq!(pkt.kind, LaStreamPacketKind::Stop);
        assert_eq!(pkt.info, 0x01);
    }

    #[test]
    fn parses_error_packet() {
        let pkt = parse_stream_packet(&[STREAM_PKT_ERROR, 0x00, 0x00, STREAM_INFO_START_REJECTED])
            .unwrap();
        assert_eq!(pkt.kind, LaStreamPacketKind::Error);
        assert_eq!(pkt.info, STREAM_INFO_START_REJECTED);
    }

    #[test]
    fn rejects_short_header() {
        let err = parse_stream_packet(&[STREAM_PKT_START, 0x00]).unwrap_err();
        assert_eq!(err, LaStreamPacketError::ShortHeader(2));
    }

    #[test]
    fn rejects_unknown_packet_type() {
        let err = parse_stream_packet(&[0xFF, 0x00, 0x00, 0x00]).unwrap_err();
        assert_eq!(err, LaStreamPacketError::InvalidPacketType(0xFF));
    }

    #[test]
    fn rejects_invalid_payload_length() {
        let err = parse_stream_packet(&[STREAM_PKT_DATA, 0x00, 61, 0x00]).unwrap_err();
        assert_eq!(err, LaStreamPacketError::InvalidPayloadLength(61));
    }

    #[test]
    fn rejects_truncated_payload() {
        let err =
            parse_stream_packet(&[STREAM_PKT_DATA, 0x00, 0x04, 0x00, 0x11, 0x22]).unwrap_err();
        assert_eq!(
            err,
            LaStreamPacketError::TruncatedPayload {
                announced: 4,
                actual: 2,
            }
        );
    }
}
