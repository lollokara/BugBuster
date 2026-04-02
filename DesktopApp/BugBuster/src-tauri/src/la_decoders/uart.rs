// =============================================================================
// la_decoders/uart.rs — UART protocol decoder
// =============================================================================

use serde::{Serialize, Deserialize};
use super::{Annotation, AnnotationType};
use crate::la_store::LaStore;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UartConfig {
    pub tx_channel: u8,         // Channel index for TX line
    pub baud_rate: u32,         // Baud rate (e.g. 115200)
    pub data_bits: u8,          // 5-9 (default 8)
    pub parity: String,         // "none", "even", "odd"
    pub stop_bits: f32,         // 1.0, 1.5, 2.0
    pub idle_high: bool,        // true = idle HIGH (standard), false = inverted
}

impl Default for UartConfig {
    fn default() -> Self {
        Self {
            tx_channel: 0,
            baud_rate: 115200,
            data_bits: 8,
            parity: "none".into(),
            stop_bits: 1.0,
            idle_high: true,
        }
    }
}

pub fn decode(cfg: &UartConfig, store: &LaStore, start: u64, end: u64) -> Vec<Annotation> {
    let mut annotations = Vec::new();
    let sr = store.sample_rate_hz as f64;
    if sr == 0.0 || cfg.baud_rate == 0 { return annotations; }

    let samples_per_bit = sr / cfg.baud_rate as f64;
    let ch = cfg.tx_channel;
    let idle_val: u8 = if cfg.idle_high { 1 } else { 0 };
    let active_val: u8 = 1 - idle_val;

    // Walk through transitions looking for start bits
    let transitions = store.get_visible(ch, start, end);
    let mut i = 0;

    while i < transitions.len() {
        let (sample, val) = transitions[i];
        if sample > end { break; }

        // Look for start bit: transition from idle to active
        if val == active_val {
            let start_bit_sample = sample;

            // Sample data bits at center of each bit period
            let mut byte_val: u8 = 0;
            let mut valid = true;

            for bit in 0..cfg.data_bits {
                let bit_center = start_bit_sample as f64 + (1.5 + bit as f64) * samples_per_bit;
                let bit_sample = bit_center as u64;
                if bit_sample > end {
                    valid = false;
                    break;
                }
                let bit_val = store.get_value_at(ch, bit_sample);
                let data_bit = if cfg.idle_high { 1 - bit_val } else { bit_val };
                byte_val |= data_bit << bit;
            }

            if !valid {
                i += 1;
                continue;
            }

            // Check parity if configured
            let mut parity_ok = true;
            let parity_bits = if cfg.parity != "none" { 1 } else { 0 };
            if parity_bits > 0 {
                let parity_center = start_bit_sample as f64 + (1.5 + cfg.data_bits as f64) * samples_per_bit;
                let parity_val = store.get_value_at(ch, parity_center as u64);
                let data_parity = byte_val.count_ones() as u8 & 1;
                let expected = if cfg.parity == "even" { data_parity } else { 1 - data_parity };
                let actual = if cfg.idle_high { 1 - parity_val } else { parity_val };
                parity_ok = actual == expected;
            }

            // Stop bit
            let stop_center = start_bit_sample as f64
                + (1.5 + cfg.data_bits as f64 + parity_bits as f64) * samples_per_bit;
            let stop_val = store.get_value_at(ch, stop_center as u64);
            let frame_ok = stop_val == idle_val;

            let end_sample = start_bit_sample + ((1.0 + cfg.data_bits as f64 + parity_bits as f64 + cfg.stop_bits as f64) * samples_per_bit) as u64;

            // Format display
            let text = if byte_val >= 0x20 && byte_val <= 0x7E {
                format!("'{}'", byte_val as char)
            } else {
                format!("0x{:02X}", byte_val)
            };

            let detail = format!("UART: {} ({})", text,
                if !frame_ok { "FRAME ERR" }
                else if !parity_ok { "PARITY ERR" }
                else { "OK" });

            let ann_type = if !frame_ok || !parity_ok {
                AnnotationType::Error
            } else {
                AnnotationType::Data
            };

            annotations.push(Annotation {
                start_sample: start_bit_sample,
                end_sample,
                text,
                detail,
                ann_type,
                row: 0,
            });

            // Skip past this frame
            let next_sample = end_sample;
            while i < transitions.len() && transitions[i].0 < next_sample {
                i += 1;
            }
            continue;
        }

        i += 1;
    }

    annotations
}
