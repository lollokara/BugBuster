// =============================================================================
// la_decoders/i2c.rs — I2C protocol decoder
// =============================================================================

use serde::{Serialize, Deserialize};
use super::{Annotation, AnnotationType};
use crate::la_store::LaStore;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct I2cConfig {
    pub sda_channel: u8,
    pub scl_channel: u8,
}

impl Default for I2cConfig {
    fn default() -> Self {
        Self { sda_channel: 0, scl_channel: 1 }
    }
}

pub fn decode(cfg: &I2cConfig, store: &LaStore, start: u64, end: u64) -> Vec<Annotation> {
    let mut annotations = Vec::new();

    let sda_trans = store.get_visible(cfg.sda_channel, start, end, None);
    let scl_trans = store.get_visible(cfg.scl_channel, start, end, None);

    // Build a list of SCL rising edges (where data is sampled)
    let mut scl_rising: Vec<u64> = Vec::new();
    for i in 1..scl_trans.len() {
        if scl_trans[i - 1].1 == 0 && scl_trans[i].1 == 1 {
            scl_rising.push(scl_trans[i].0);
        }
    }

    // Detect START/STOP conditions: SDA changes while SCL is HIGH
    // START: SDA falls while SCL high
    // STOP:  SDA rises while SCL high
    let mut events: Vec<(u64, &str)> = Vec::new(); // (sample, "START" or "STOP")

    for i in 1..sda_trans.len() {
        let sample = sda_trans[i].0;
        if sample < start || sample > end { continue; }

        let scl_val = store.get_value_at(cfg.scl_channel, sample);
        if scl_val == 1 {
            if sda_trans[i - 1].1 == 1 && sda_trans[i].1 == 0 {
                events.push((sample, "START"));
            } else if sda_trans[i - 1].1 == 0 && sda_trans[i].1 == 1 {
                events.push((sample, "STOP"));
            }
        }
    }

    // Process: between START and STOP, read bytes on SCL rising edges
    let mut ev_idx = 0;
    while ev_idx < events.len() {
        let (start_sample, evt_type) = events[ev_idx];
        if evt_type != "START" {
            // Annotate STOP
            annotations.push(Annotation {
                start_sample,
                end_sample: start_sample + 10,
                text: "P".into(),
                detail: "I2C STOP".into(),
                ann_type: AnnotationType::Control,
                row: 0,
                channel: cfg.sda_channel,
            });
            ev_idx += 1;
            continue;
        }

        // START condition
        annotations.push(Annotation {
            start_sample,
            end_sample: start_sample + 10,
            text: "S".into(),
            detail: "I2C START".into(),
            ann_type: AnnotationType::Control,
            row: 0,
            channel: cfg.sda_channel,
        });

        // Find SCL rising edges after START until next START/STOP
        let next_event_sample = if ev_idx + 1 < events.len() { events[ev_idx + 1].0 } else { end };

        let edges: Vec<u64> = scl_rising.iter()
            .filter(|&&s| s > start_sample && s < next_event_sample)
            .copied()
            .collect();

        // Read bytes (8 data bits + 1 ACK/NAK per byte)
        let mut bit_idx = 0;
        let mut is_first_byte = true;

        while bit_idx + 8 < edges.len() {
            let byte_start = edges[bit_idx];
            let mut byte_val: u8 = 0;
            for b in 0..8 {
                let sda = store.get_value_at(cfg.sda_channel, edges[bit_idx + b]);
                byte_val = (byte_val << 1) | sda;
            }

            // ACK/NAK bit
            let ack_sample = if bit_idx + 8 < edges.len() { edges[bit_idx + 8] } else { break };
            let ack_val = store.get_value_at(cfg.sda_channel, ack_sample);
            let ack_str = if ack_val == 0 { "ACK" } else { "NAK" };

            let byte_end = ack_sample + 10;

            if is_first_byte {
                // Address byte: 7-bit address + R/W
                let addr = byte_val >> 1;
                let rw = if byte_val & 1 == 1 { "R" } else { "W" };
                annotations.push(Annotation {
                    start_sample: byte_start,
                    end_sample: byte_end,
                    text: format!("0x{:02X} {}", addr, rw),
                    detail: format!("I2C Address 0x{:02X} {}", addr, if rw == "R" { "Read" } else { "Write" }),
                    ann_type: AnnotationType::Address,
                    row: 0,
                    channel: cfg.sda_channel,
                });
                is_first_byte = false;
            } else {
                // Data byte
                annotations.push(Annotation {
                    start_sample: byte_start,
                    end_sample: byte_end,
                    text: format!("0x{:02X}", byte_val),
                    detail: format!("I2C Data 0x{:02X} ({})", byte_val, byte_val),
                    ann_type: AnnotationType::Data,
                    row: 0,
                    channel: cfg.sda_channel,
                });
            }

            // ACK/NAK annotation
            annotations.push(Annotation {
                start_sample: ack_sample,
                end_sample: ack_sample + 5,
                text: ack_str.into(),
                detail: format!("I2C {}", ack_str),
                ann_type: if ack_val == 0 { AnnotationType::Info } else { AnnotationType::Error },
                row: 0,
                channel: cfg.scl_channel,
            });

            bit_idx += 9; // 8 data + 1 ack
        }

        ev_idx += 1;
    }

    annotations
}
