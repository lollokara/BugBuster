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
    /// Density histogram: total transitions per bucket across all channels (for minimap)
    pub density: Vec<u16>,
    /// True if the data was decimated (LOD) for this viewport
    pub decimated: bool,
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

    /// Append raw packed bitstream data to the existing store (for stream mode).
    /// New samples are indexed starting from `self.total_samples`.
    pub fn append_raw(&mut self, raw: &[u8]) {
        let bits_per_sample = self.channels as usize;
        if bits_per_sample == 0 { return; }

        // Get the last known value per channel to detect transitions
        let mut prev_values: Vec<u8> = (0..self.channels as usize).map(|ch| {
            self.transitions[ch].last().map(|&(_, v)| v)
                .unwrap_or(0xFF) // impossible value forces first transition if store was empty
        }).collect();

        let mut sample_idx = self.total_samples;

        for &byte in raw {
            let mut bit_pos = 0;
            while bit_pos + bits_per_sample <= 8 {
                for ch in 0..self.channels as usize {
                    let val = (byte >> (bit_pos + ch)) & 1;
                    if val != prev_values[ch] {
                        // Limit total transitions per channel to avoid memory explosion (1M per channel)
                        if self.transitions[ch].len() < 1_000_000 {
                            self.transitions[ch].push((sample_idx, val));
                            prev_values[ch] = val;
                        }
                    }
                }
                sample_idx += 1;
                bit_pos += bits_per_sample;
            }
        }

        self.total_samples = sample_idx;
    }

    /// Get transitions visible in a sample range for a channel.
    /// Includes one transition before `start` to establish initial value.
    /// If `max_points` is provided, decimates data using a min/max envelope.
    pub fn get_visible(&self, ch: u8, start: u64, end: u64, max_points: Option<usize>) -> Vec<Transition> {
        if ch as usize >= self.transitions.len() {
            return vec![];
        }
        let trans = &self.transitions[ch as usize];

        // Binary search for the first transition >= start
        let first_idx = trans.partition_point(|&(s, _)| s < start);

        let mut result = Vec::new();

        // 1. Include one transition before start for initial value
        if first_idx > 0 {
            result.push(trans[first_idx - 1]);
        } else if !trans.is_empty() && trans[0].0 > start {
            result.push((start, trans[0].1 ^ 1));
        }

        // 2. Collect transitions in range
        let mut raw_points = Vec::new();
        for &t in &trans[first_idx..] {
            if t.0 > end { break; }
            raw_points.push(t);
        }

        if raw_points.is_empty() {
            return result;
        }

        // 3. Decimate if needed
        if let Some(limit) = max_points {
            if raw_points.len() > limit {
                let bucket_size = (end - start) / limit as u64;
                if bucket_size > 1 {
                    let mut current_bucket = start + bucket_size;
                    let mut i = 0;
                    
                    while i < raw_points.len() {
                        let bucket_start_idx = i;
                        let mut has_0 = false;
                        let mut has_1 = false;
                        
                        while i < raw_points.len() && raw_points[i].0 < current_bucket {
                            if raw_points[i].1 == 0 { has_0 = true; }
                            else { has_1 = true; }
                            i += 1;
                        }
                        
                        if i > bucket_start_idx {
                            // If bucket contains both 0 and 1, it's an "activity" block.
                            // We must ensure it's visible AND ends on the correct logic level.
                            if has_0 && has_1 {
                                let first_val = raw_points[bucket_start_idx].1;
                                let last_val = raw_points[i - 1].1;
                                
                                result.push((raw_points[bucket_start_idx].0, first_val));
                                if last_val == first_val {
                                    // Ending same as starting: need a middle toggle to show activity
                                    result.push((raw_points[bucket_start_idx].0 + 1, first_val ^ 1));
                                    result.push((raw_points[i - 1].0, last_val));
                                } else {
                                    // Ending different: already shows activity, just ensure last transition is there
                                    result.push((raw_points[i - 1].0, last_val));
                                }
                            } else {
                                // Uniform bucket, just take the first transition
                                result.push(raw_points[bucket_start_idx]);
                            }
                        }
                        current_bucket += bucket_size;
                    }
                    return result;
                }
            }
        }

        result.extend(raw_points);
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

    /// Build a density histogram for the entire capture (for minimap heatmap)
    pub fn density_histogram(&self, buckets: usize) -> Vec<u16> {
        if buckets == 0 || self.total_samples == 0 { return vec![0; buckets.max(1)]; }
        let mut hist = vec![0u16; buckets];
        let bucket_size = (self.total_samples as f64) / buckets as f64;
        for ch_trans in &self.transitions {
            for &(sample, _) in ch_trans {
                let b = ((sample as f64 / bucket_size) as usize).min(buckets - 1);
                hist[b] = hist[b].saturating_add(1);
            }
        }
        hist
    }

    /// Extract viewport data for frontend rendering
    pub fn to_view_data(&self, start: u64, end: u64, max_points: Option<usize>) -> LaViewData {
        let mut decimated = false;
        let channel_transitions: Vec<Vec<(u64, u8)>> = (0..self.channels)
            .map(|ch| {
                let v = self.get_visible(ch, start, end, max_points);
                // Check if any channel was decimated (count of raw points would have been > limit)
                if let Some(limit) = max_points {
                    let raw_count = self.transitions[ch as usize].partition_point(|&(s, _)| s <= end) 
                                  - self.transitions[ch as usize].partition_point(|&(s, _)| s < start);
                    if raw_count > limit { decimated = true; }
                }
                v
            })
            .collect();

        let density = self.density_histogram(800);

        LaViewData {
            channels: self.channels,
            sample_rate_hz: self.sample_rate_hz,
            total_samples: self.total_samples,
            view_start: start,
            view_end: end,
            trigger_sample: self.trigger_sample,
            channel_transitions,
            density,
            decimated,
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

    /// Delete all samples in [start, end] and shift subsequent samples left.
    pub fn delete_range(&mut self, start: u64, end: u64) -> u64 {
        if end < start { return 0; }
        let removed = end - start + 1;
        for ch in 0..self.channels {
            let ch_idx = ch as usize;
            if ch_idx >= self.transitions.len() { continue; }

            // 1. Capture signal states before and after the cut
            let val_before = self.get_value_at(ch, start.saturating_sub(1));
            let val_after = self.get_value_at(ch, end + 1);

            let ch_trans = &mut self.transitions[ch_idx];

            // 2. Remove transitions within the range
            ch_trans.retain(|&(s, _)| s < start || s > end);

            // 3. Shift remaining transitions
            for t in ch_trans.iter_mut() {
                if t.0 > end {
                    t.0 -= removed;
                }
            }

            // 4. Insert bridge transition at start if signal level changed
            if val_after != val_before {
                let splice_idx = ch_trans.partition_point(|&(s, _)| s < start);
                ch_trans.insert(splice_idx, (start, val_after));
            }

            // 5. Restore RLE invariant
            ch_trans.dedup_by(|a, b| a.1 == b.1);
        }
        self.total_samples = self.total_samples.saturating_sub(removed);
        removed
    }
}
