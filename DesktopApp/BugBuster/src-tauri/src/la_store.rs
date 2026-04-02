// =============================================================================
// la_store.rs — Logic Analyzer capture data model
//
// Stores captures as run-length encoded transitions (value changes only).
// Efficient for digital signals where values are stable most of the time.
// Supports viewport extraction for rendering and VCD export.
// =============================================================================

use serde::{Serialize, Deserialize};

/// A single transition: (sample_index, new_value)
pub type Transition = (u64, u8);

/// Run-length encoded capture storage
#[derive(Debug, Clone, Default)]
pub struct LaStore {
    pub channels: u8,
    pub sample_rate_hz: u32,
    /// Per-channel transition lists, sorted by sample index
    pub transitions: Vec<Vec<Transition>>,
    pub total_samples: u64,
    pub trigger_sample: Option<u64>,
}

/// Serializable view data for a viewport — sent to frontend for rendering
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LaViewData {
    pub channels: u8,
    pub sample_rate_hz: u32,
    pub total_samples: u64,
    pub view_start: u64,
    pub view_end: u64,
    pub trigger_sample: Option<u64>,
    /// Per-channel: list of (sample_index, value) for the visible range
    /// Includes one transition before view_start for initial state
    pub channel_transitions: Vec<Vec<(u64, u8)>>,
}

impl LaStore {
    /// Create from raw packed bitstream (from RP2040 capture)
    pub fn from_raw(raw: &[u8], channels: u8, sample_rate_hz: u32) -> Self {
        let bits_per_sample = channels as usize;
        let mut transitions: Vec<Vec<Transition>> = (0..channels).map(|_| Vec::new()).collect();
        let mut prev_values = vec![0xFFu8; channels as usize]; // impossible initial value to force first transition

        let mut sample_idx: u64 = 0;

        for &byte in raw {
            let mut bit_pos = 0;
            while bit_pos + bits_per_sample <= 8 {
                for ch in 0..channels as usize {
                    let val = (byte >> (bit_pos + ch)) & 1;
                    if val != prev_values[ch] {
                        transitions[ch].push((sample_idx, val));
                        prev_values[ch] = val;
                    }
                }
                sample_idx += 1;
                bit_pos += bits_per_sample;
            }
        }

        LaStore {
            channels,
            sample_rate_hz,
            transitions,
            total_samples: sample_idx,
            trigger_sample: None,
        }
    }

    /// Get transitions visible in a sample range for a channel.
    /// Includes one transition before `start` to establish initial value.
    pub fn get_visible(&self, ch: u8, start: u64, end: u64) -> Vec<Transition> {
        if ch as usize >= self.transitions.len() {
            return vec![];
        }
        let trans = &self.transitions[ch as usize];

        // Binary search for the first transition >= start
        let first_idx = trans.partition_point(|&(s, _)| s < start);

        let mut result = Vec::new();

        // Include one transition before start for initial value
        if first_idx > 0 {
            result.push(trans[first_idx - 1]);
        } else if !trans.is_empty() {
            // No transition before start — signal starts at its first known value
            // Insert a synthetic transition at sample 0
            result.push((0, trans[0].1 ^ 1)); // opposite of first change (implies initial state)
        }

        // Collect transitions in range
        for &t in &trans[first_idx..] {
            if t.0 > end { break; }
            result.push(t);
        }

        result
    }

    /// Convert sample index to time in seconds
    pub fn sample_to_time(&self, sample: u64) -> f64 {
        if self.sample_rate_hz == 0 { return 0.0; }
        sample as f64 / self.sample_rate_hz as f64
    }

    /// Total capture duration in seconds
    pub fn total_duration_sec(&self) -> f64 {
        self.sample_to_time(self.total_samples)
    }

    /// Extract viewport data for frontend rendering
    pub fn to_view_data(&self, start: u64, end: u64) -> LaViewData {
        let channel_transitions: Vec<Vec<(u64, u8)>> = (0..self.channels)
            .map(|ch| self.get_visible(ch, start, end))
            .collect();

        LaViewData {
            channels: self.channels,
            sample_rate_hz: self.sample_rate_hz,
            total_samples: self.total_samples,
            view_start: start,
            view_end: end,
            trigger_sample: self.trigger_sample,
            channel_transitions,
        }
    }

    /// Export as VCD (Value Change Dump) format string
    pub fn export_vcd(&self) -> String {
        let mut vcd = String::new();

        // Header
        vcd.push_str("$date\n  BugBuster Logic Analyzer capture\n$end\n");
        vcd.push_str(&format!("$version\n  BugBuster HAT LA v1.0\n$end\n"));

        // Timescale: derive from sample rate
        let timescale_ns = if self.sample_rate_hz > 0 {
            1_000_000_000u64 / self.sample_rate_hz as u64
        } else {
            1000 // default 1µs
        };
        vcd.push_str(&format!("$timescale\n  {}ns\n$end\n", timescale_ns));

        // Variable definitions
        vcd.push_str("$scope module top $end\n");
        for ch in 0..self.channels {
            let id = (b'!' + ch) as char; // VCD identifier characters
            vcd.push_str(&format!("$var wire 1 {} ch{} $end\n", id, ch));
        }
        vcd.push_str("$upscope $end\n");
        vcd.push_str("$enddefinitions $end\n");

        // Initial values
        vcd.push_str("#0\n");
        for ch in 0..self.channels {
            let id = (b'!' + ch) as char;
            let initial = if !self.transitions[ch as usize].is_empty() {
                // Value before first transition (opposite of first change)
                self.transitions[ch as usize][0].1 ^ 1
            } else {
                0
            };
            vcd.push_str(&format!("{}{}\n", initial, id));
        }

        // Merge all transitions and sort by time
        let mut all_events: Vec<(u64, u8, char)> = Vec::new();
        for ch in 0..self.channels {
            let id = (b'!' + ch) as char;
            for &(sample, val) in &self.transitions[ch as usize] {
                all_events.push((sample, val, id));
            }
        }
        all_events.sort_by_key(|&(s, _, _)| s);

        // Write events
        let mut last_time = 0u64;
        for (sample, val, id) in all_events {
            if sample != last_time {
                vcd.push_str(&format!("#{}\n", sample));
                last_time = sample;
            }
            vcd.push_str(&format!("{}{}\n", val, id));
        }

        vcd
    }

    /// Get value of a channel at a specific sample index
    pub fn get_value_at(&self, ch: u8, sample: u64) -> u8 {
        if ch as usize >= self.transitions.len() { return 0; }
        let trans = &self.transitions[ch as usize];

        // Find the last transition at or before this sample
        let idx = trans.partition_point(|&(s, _)| s <= sample);
        if idx == 0 {
            // No transition before this point — assume initial state
            if trans.is_empty() { return 0; }
            return trans[0].1 ^ 1; // opposite of first change
        }
        trans[idx - 1].1
    }
}
