// =============================================================================
// la_decoders/mod.rs — Protocol decoder framework
// =============================================================================

pub mod uart;
pub mod i2c;
pub mod spi;

use serde::{Serialize, Deserialize};
use crate::la_store::LaStore;

/// Annotation type for color coding
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum AnnotationType {
    Data,       // Blue — data bytes
    Address,    // Green — address fields
    Control,    // Amber — START, STOP, ACK, NAK
    Error,      // Red — parity errors, framing errors
    Info,       // Cyan — informational
}

/// A decoded annotation to render on the waveform
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Annotation {
    pub start_sample: u64,
    pub end_sample: u64,
    pub text: String,           // Short label: "0x4A", "ACK", "S"
    pub detail: String,         // Longer detail: "Write to addr 0x4A"
    pub ann_type: AnnotationType,
    pub row: u8,                // Display row (0 = primary, 1 = secondary)
    pub channel: u8,            // Which channel track to display this annotation on
}

/// Decoder configuration — serializable for UI
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
#[serde(tag = "type")]
pub enum DecoderConfig {
    Uart(uart::UartConfig),
    I2c(i2c::I2cConfig),
    Spi(spi::SpiConfig),
}

/// Run a decoder on a capture store for a sample range
pub fn decode(
    config: &DecoderConfig,
    store: &LaStore,
    start: u64,
    end: u64,
) -> Vec<Annotation> {
    match config {
        DecoderConfig::Uart(cfg) => uart::decode(cfg, store, start, end),
        DecoderConfig::I2c(cfg)  => i2c::decode(cfg, store, start, end),
        DecoderConfig::Spi(cfg)  => spi::decode(cfg, store, start, end),
    }
}
