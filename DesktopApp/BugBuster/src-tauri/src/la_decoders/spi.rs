// =============================================================================
// la_decoders/spi.rs — SPI protocol decoder
// =============================================================================

use serde::{Serialize, Deserialize};
use super::{Annotation, AnnotationType};
use crate::la_store::LaStore;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SpiConfig {
    pub mosi_channel: u8,
    pub miso_channel: u8,
    pub sclk_channel: u8,
    pub cs_channel: u8,
    pub cpol: u8,               // Clock polarity: 0 or 1
    pub cpha: u8,               // Clock phase: 0 or 1
    pub bit_order: String,      // "msb" or "lsb"
    pub word_size: u8,          // Bits per word (default 8)
    pub cs_active_low: bool,    // true = CS active low (standard)
}

impl Default for SpiConfig {
    fn default() -> Self {
        Self {
            mosi_channel: 0,
            miso_channel: 1,
            sclk_channel: 2,
            cs_channel: 3,
            cpol: 0,
            cpha: 0,
            bit_order: "msb".into(),
            word_size: 8,
            cs_active_low: true,
        }
    }
}

pub fn decode(cfg: &SpiConfig, store: &LaStore, start: u64, end: u64) -> Vec<Annotation> {
    let mut annotations = Vec::new();

    let cs_active = if cfg.cs_active_low { 0u8 } else { 1u8 };

    // Find clock edges
    let sclk_trans = store.get_visible(cfg.sclk_channel, start, end, None);

    // Determine which clock edge samples data
    // CPHA=0: data sampled on first (leading) edge
    // CPHA=1: data sampled on second (trailing) edge
    // CPOL=0: idle LOW, leading = rising, trailing = falling
    // CPOL=1: idle HIGH, leading = falling, trailing = rising
    let sample_on_rising = (cfg.cpol == 0 && cfg.cpha == 0) || (cfg.cpol == 1 && cfg.cpha == 1);

    // Collect sampling edges
    let mut sample_edges: Vec<u64> = Vec::new();
    for i in 1..sclk_trans.len() {
        let is_rising = sclk_trans[i - 1].1 == 0 && sclk_trans[i].1 == 1;
        let is_falling = sclk_trans[i - 1].1 == 1 && sclk_trans[i].1 == 0;
        if (sample_on_rising && is_rising) || (!sample_on_rising && is_falling) {
            let s = sclk_trans[i].0;
            if s >= start && s <= end {
                // 0xFF = no CS line — always active
                let cs_ok = cfg.cs_channel == 0xFF
                    || store.get_value_at(cfg.cs_channel, s) == cs_active;
                if cs_ok {
                    sample_edges.push(s);
                }
            }
        }
    }

    // Group edges into words
    let ws = cfg.word_size as usize;
    if ws == 0 { return annotations; }

    let mut edge_idx = 0;
    while edge_idx + ws <= sample_edges.len() {
        let word_start = sample_edges[edge_idx];
        let word_end = sample_edges[edge_idx + ws - 1];

        // Read MOSI word
        let mut mosi_val: u64 = 0;
        let mut miso_val: u64 = 0;
        for b in 0..ws {
            let sample = sample_edges[edge_idx + b];
            let mosi_bit = store.get_value_at(cfg.mosi_channel, sample);
            let miso_bit = store.get_value_at(cfg.miso_channel, sample);

            if cfg.bit_order == "msb" {
                mosi_val = (mosi_val << 1) | mosi_bit as u64;
                miso_val = (miso_val << 1) | miso_bit as u64;
            } else {
                mosi_val |= (mosi_bit as u64) << b;
                miso_val |= (miso_bit as u64) << b;
            }
        }

        // MOSI annotation
        let mosi_text = if ws <= 8 {
            format!("0x{:02X}", mosi_val)
        } else if ws <= 16 {
            format!("0x{:04X}", mosi_val)
        } else {
            format!("0x{:X}", mosi_val)
        };

        annotations.push(Annotation {
            start_sample: word_start,
            end_sample: word_end + 5,
            text: mosi_text.clone(),
            detail: format!("SPI MOSI: {} ({})", mosi_text, mosi_val),
            ann_type: AnnotationType::Data,
            row: 0,
            channel: cfg.mosi_channel,
        });

        // MISO annotation (skip if same channel as MOSI — would be identical)
        if cfg.miso_channel != cfg.mosi_channel {
            let miso_text = if ws <= 8 {
                format!("0x{:02X}", miso_val)
            } else if ws <= 16 {
                format!("0x{:04X}", miso_val)
            } else {
                format!("0x{:X}", miso_val)
            };

            annotations.push(Annotation {
                start_sample: word_start,
                end_sample: word_end + 5,
                text: miso_text.clone(),
                detail: format!("SPI MISO: {} ({})", miso_text, miso_val),
                ann_type: AnnotationType::Info,
                row: 0,
                channel: cfg.miso_channel,
            });
        }

        edge_idx += ws;
    }

    annotations
}
