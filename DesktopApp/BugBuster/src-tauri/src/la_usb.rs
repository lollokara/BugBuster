// =============================================================================
// la_usb.rs — Logic Analyzer USB bulk data reader
//
// Discovers the RP2040 BugBuster HAT on USB, opens the LA vendor bulk
// interface (interface 3), and reads captured waveform data.
//
// USB device identification:
//   VID: 0x2E8A (Raspberry Pi)
//   PID: 0x000C (Debug Probe / BugBuster HAT)
//   Interface 3: BugBuster Logic Analyzer (vendor bulk)
//   EP 0x87 IN: LA data stream
//   EP 0x06 OUT: LA commands (not used — commands go via ESP32 UART)
// =============================================================================

use anyhow::{anyhow, Result};
use log::{info, warn};
use nusb::transfer::RequestBuffer;
use futures::executor::block_on;

const BB_HAT_VID: u16 = 0x2E8A;
const BB_HAT_PID: u16 = 0x000C;
const LA_INTERFACE_NUM: u8 = 3;
const LA_EP_IN: u8 = 0x87;
const LA_EP_OUT: u8 = 0x06;

/// Captured LA data with metadata
#[derive(Debug, Clone, Default, serde::Serialize, serde::Deserialize)]
pub struct LaCaptureData {
    pub channels: u8,
    pub sample_rate_hz: u32,
    pub total_samples: u32,
    pub raw_data: Vec<u8>,
    /// Decoded per-channel sample arrays (0/1 values)
    pub channel_data: Vec<Vec<u8>>,
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

    /// Find and open the BugBuster HAT LA interface
    pub fn connect(&mut self) -> Result<()> {
        let devices = nusb::list_devices()
            .map_err(|e| anyhow!("USB enumeration failed: {}", e))?;

        for dev_info in devices {
            if dev_info.vendor_id() == BB_HAT_VID && dev_info.product_id() == BB_HAT_PID {
                info!("Found BugBuster HAT USB device: bus={} addr={}",
                      dev_info.bus_number(), dev_info.device_address());

                let device = dev_info.open()
                    .map_err(|e| anyhow!("Failed to open USB device: {}", e))?;

                // Claim interface 3 (LA vendor bulk)
                let iface = device.claim_interface(LA_INTERFACE_NUM)
                    .map_err(|e| anyhow!("Failed to claim LA interface {}: {}", LA_INTERFACE_NUM, e))?;

                info!("LA USB interface claimed (interface {})", LA_INTERFACE_NUM);
                self.interface = Some(iface);
                self.connected = true;
                return Ok(());
            }
        }

        Err(anyhow!("BugBuster HAT not found on USB (VID={:04X} PID={:04X})", BB_HAT_VID, BB_HAT_PID))
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }

    /// Read LA data from USB bulk endpoint (blocking).
    /// The RP2040 sends: [total_len:u32 LE] [data bytes...]
    /// Call from a blocking task context (not from async directly).
    pub fn read_capture_blocking(&mut self) -> Result<Vec<u8>> {
        let iface = self.interface.as_ref()
            .ok_or_else(|| anyhow!("LA USB not connected"))?;

        // First read: 4-byte header with total length
        let header_completion = block_on(iface.bulk_in(LA_EP_IN, RequestBuffer::new(64)));
        let header_result = header_completion.into_result()
            .map_err(|e| anyhow!("USB bulk read header failed: {}", e))?;

        if header_result.len() < 4 {
            return Err(anyhow!("Short header: {} bytes", header_result.len()));
        }

        let total_len = u32::from_le_bytes([
            header_result[0], header_result[1],
            header_result[2], header_result[3],
        ]) as usize;

        info!("LA USB: reading {} bytes of capture data", total_len);

        // Collect all data (header may contain data after the 4-byte length)
        let mut data = Vec::with_capacity(total_len);
        if header_result.len() > 4 {
            data.extend_from_slice(&header_result[4..]);
        }

        // Read remaining data in 64-byte chunks
        while data.len() < total_len {
            let completion = block_on(iface.bulk_in(LA_EP_IN, RequestBuffer::new(64)));
            let result = completion.into_result()
                .map_err(|e| anyhow!("USB bulk read failed at offset {}: {}", data.len(), e))?;

            if result.is_empty() {
                warn!("USB bulk read returned 0 bytes at offset {}/{}", data.len(), total_len);
                break;
            }
            data.extend_from_slice(&result);
        }

        data.truncate(total_len);
        info!("LA USB: received {} bytes", data.len());
        Ok(data)
    }

    pub fn disconnect(&mut self) {
        self.interface = None;
        self.connected = false;
    }
}

/// Decode raw LA capture data into per-channel arrays
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

/// Check if a BugBuster HAT is present on USB (without opening)
pub fn hat_usb_present() -> bool {
    if let Ok(devices) = nusb::list_devices() {
        devices.into_iter().any(|d| {
            d.vendor_id() == BB_HAT_VID && d.product_id() == BB_HAT_PID
        })
    } else {
        false
    }
}
