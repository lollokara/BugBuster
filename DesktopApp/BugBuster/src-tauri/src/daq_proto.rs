// =============================================================================
// daq_proto.rs — ESP32-P4 DAQ USB-HS measurement-stream wire protocol.
//
// Mirrors Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h. Frames are pushed
// device -> PC over the P4 USB-HS vendor-bulk endpoint; control commands flow
// PC -> device. Every frame is:
//
//   offset  size  field
//   0       2     magic = 0xBB 0x50
//   2       1     version (USB_PROTO_VERSION)
//   3       1     type    (record / command id)
//   4       1     flags
//   5       1     reserved (0)
//   6       4     seq     (monotonic per stream)
//   10      2     payload_len
//   12      N     payload
//   12+N    2     crc16-ccitt over bytes [2 .. 12+N)
//
// All multi-byte integers and floats are little-endian.
// =============================================================================

use serde::{Deserialize, Serialize};

pub const MAGIC0: u8 = 0xBB;
pub const MAGIC1: u8 = 0x50;
pub const PROTO_VERSION: u8 = 2;
pub const FRAME_HEADER_LEN: usize = 12;
pub const FRAME_CRC_LEN: usize = 2;
pub const FRAME_OVERHEAD: usize = FRAME_HEADER_LEN + FRAME_CRC_LEN;
pub const MAX_PAYLOAD: usize = 16384;

// Record / frame types. 0x00..0x7F = device->PC data, 0x80+ = PC->device control.
pub const REC_WAVE_I: u8 = 0x01;
pub const REC_STATS: u8 = 0x02;
pub const REC_ENERGY: u8 = 0x03;
pub const REC_FFT: u8 = 0x04;
pub const REC_MARKER: u8 = 0x05;
pub const REC_STATUS: u8 = 0x06;
pub const REC_WAVE_V: u8 = 0x07;

pub const CMD_START: u8 = 0x80;
pub const CMD_STOP: u8 = 0x81;
pub const CMD_SET_RATE: u8 = 0x82;
pub const CMD_RANGE_LOCK: u8 = 0x83;
pub const CMD_RESET_ENERGY: u8 = 0x84;
pub const CMD_RESET_STATS: u8 = 0x85;
pub const CMD_FFT_CONFIG: u8 = 0x86;
pub const CMD_SET_SOURCE: u8 = 0x87;
pub const CMD_ARM: u8 = 0x88;

// MARKER kind codes (usb_marker_payload_t.kind).
pub const MARK_KIND_FLAG: u8 = 0;
pub const MARK_KIND_TRIGGER: u8 = 1;

// Current-range codes (current_range_t).
pub const RANGE_HI: u8 = 0; // 51 Ω shunt — FINE, nA..~1.4 mA
pub const RANGE_MID: u8 = 1; // 2 Ω shunt — FINE, ~1.4..37 mA
pub const RANGE_LO: u8 = 2; // 50 mΩ shunt — COARSE, ~37 mA..3 A

// Fusion source codes (fuse_source_t).
pub const SRC_FINE: u8 = 0;
pub const SRC_COARSE: u8 = 1;
pub const SRC_BLEND: u8 = 2;

/// meta byte accessors (WaveIRecord.meta[i]): bits 0-1 = range, bits 2-3 =
/// source, bit 4 = saturated, bit 5 = settling.
pub fn meta_range(m: u8) -> u8 {
    m & 0x03
}
pub fn meta_source(m: u8) -> u8 {
    (m >> 2) & 0x03
}
pub const META_SATURATED: u8 = 0x10;
pub const META_SETTLING: u8 = 0x20;

/// WAVE_I record: struct-of-arrays fused-current waveform.
/// Wire payload (24-byte header): u64 start_index | u64 timestamp_us |
/// u32 sample_rate | u16 count | u8 decimation | u8 pad
/// | count x f32 i[] | count x u8 meta[]
#[derive(Debug, Clone, PartialEq)]
pub struct WaveIRecord {
    pub start_index: u64,
    pub timestamp_us: u64,
    pub sample_rate: u32,
    pub decimation: u8,
    pub i: Vec<f32>,
    pub meta: Vec<u8>,
}

/// WAVE_V record: struct-of-arrays voltage waveform.
/// Wire payload (24-byte header): u64 start_index | u64 timestamp_us |
/// u32 sample_rate | u16 count | u16 pad | count x f32 v[]
#[derive(Debug, Clone, PartialEq)]
pub struct WaveVRecord {
    pub start_index: u64,
    pub timestamp_us: u64,
    pub sample_rate: u32,
    pub v: Vec<f32>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct StatBlock {
    pub min: f32,
    pub max: f32,
    pub mean: f32,
    pub rms: f32,
    pub std: f32,
    pub count: u32,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct StatsRecord {
    pub i: StatBlock,
    pub v: StatBlock,
    pub p: StatBlock,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EnergyRecord {
    pub energy_mwh: f64,
    pub energy_j: f64,
    pub charge_mah: f64,
    pub charge_c: f64,
    pub elapsed_s: f64,
    pub last_i: f32,
    pub last_v: f32,
    pub last_p: f32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FftRecord {
    pub sample_rate: u32,
    pub source: u8, // 0 = current, 1 = power
    pub window: u8,
    pub bins: Vec<f32>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StatusRecord {
    pub sample_rate: u32,
    pub overflow_count: u32,
    pub range: u8,
    pub streaming: bool,
    pub range_locked: bool,
    pub source_enabled: bool,
    pub vdut_set: f32,
    pub ilimit_set: f32,
    /// Extension v1 (bytes 20-27): SMU input-rail sense. 0 when not reported.
    pub in_voltage: f32,
    pub in_current: f32,
    /// Extension v2 (bytes 28-39): FINE ADC health.
    /// `adaq_ok_bits`: bit0=FINE ok, bit1=COARSE ok, bit2=VOLT ok. 0 = old firmware.
    pub adaq_ok_bits: u8,
    /// Percentage of FINE samples that carried a STATUS_ERR bit (0-100).
    /// 100 means the fused stream is running on COARSE only.
    pub fine_err_pct: u8,
    /// FINE pairing-resync drop counter (widened v3: uint32, was uint16).
    pub drop_fine: u32,
    /// COARSE pairing-resync drop counter (widened v3: uint32, was uint16).
    pub drop_coarse: u32,
    /// OR of all MASTER_STATUS (0x2D) bytes seen on the FINE ADAQ since boot.
    /// 0xFF = FINE ADAQ did not initialise. Bit map: 7=MASTER_ERR 6=ADC_ERR
    /// 5=DIG_ERR 4=CLK_QUAL 3=FILT_SAT 2=FILT_UNSETTLED 1=SPI_ERR 0=POR.
    pub fine_diag_sticky: u8,
    /// Extension v3 (bytes 40-59): USB streaming performance counters.
    /// 0 when not reported (payload < 60 bytes).
    pub frames_tx: u32,
    pub bytes_per_sec: u32,
    pub fifo_drop_frames: u32,
    pub ring_high_water: u32,
    pub wave_i_index_lo: u32,
    /// Extension v7 (bytes 100-103): board temperatures, 0.1 C. None when sensor absent.
    pub board_temp_analog_c: Option<f32>,
    pub board_temp_power_c: Option<f32>,
    /// Extension v8 (byte 104): per-range current calibration validity.
    /// bit0=HI calibrated, bit1=MID calibrated, bit2=LO calibrated.
    pub cal_have_hi: bool,
    pub cal_have_mid: bool,
    pub cal_have_lo: bool,
}

/// One digital event marker (flag or trigger), decoded from the 16-byte wire
/// struct usb_marker_payload_t. Markers carry the fused-sample index the event
/// aligns to, so they survive the host's multi-resolution decimation untouched.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MarkerRecord {
    pub sample_index: u64,
    pub timestamp_us: u64,
    pub channel: u8, // S3 IO number (1..12)
    pub edge: u8,    // 0 = falling, 1 = rising
    pub kind: u8,    // MARK_KIND_FLAG / MARK_KIND_TRIGGER
}

#[derive(Debug, Clone, PartialEq)]
pub enum DaqRecord {
    WaveI(WaveIRecord),
    WaveV(WaveVRecord),
    Stats(StatsRecord),
    Energy(EnergyRecord),
    Fft(FftRecord),
    Status(StatusRecord),
    Marker(MarkerRecord),
    /// A record type we recognised but do not decode.
    Other(u8),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FrameError {
    ShortHeader(usize),
    BadMagic,
    BadVersion(u8),
    PayloadTooLong(usize),
    Truncated { need: usize, have: usize },
    BadCrc { expected: u16, got: u16 },
}

/// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Matches usb_proto_crc16.
pub fn crc16(data: &[u8], init: u16) -> u16 {
    let mut crc = init;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            if crc & 0x8000 != 0 {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    crc
}

/// Parse exactly one frame starting at the front of `buf`. On success returns
/// the decoded record plus the total number of bytes consumed (header + payload
/// + crc). `Truncated` means more bytes are needed; the caller should keep the
/// buffer and read more from USB.
pub fn parse_frame(buf: &[u8]) -> Result<(DaqRecord, usize), FrameError> {
    if buf.len() < FRAME_HEADER_LEN {
        return Err(FrameError::ShortHeader(buf.len()));
    }
    if buf[0] != MAGIC0 || buf[1] != MAGIC1 {
        return Err(FrameError::BadMagic);
    }
    if buf[2] != PROTO_VERSION {
        return Err(FrameError::BadVersion(buf[2]));
    }
    let rec_type = buf[3];
    let payload_len = u16::from_le_bytes([buf[10], buf[11]]) as usize;
    if payload_len > MAX_PAYLOAD {
        return Err(FrameError::PayloadTooLong(payload_len));
    }
    let total = FRAME_HEADER_LEN + payload_len + FRAME_CRC_LEN;
    if buf.len() < total {
        return Err(FrameError::Truncated {
            need: total,
            have: buf.len(),
        });
    }
    let payload = &buf[FRAME_HEADER_LEN..FRAME_HEADER_LEN + payload_len];
    // Data-direction frames (rec_type < 0x80) carry a zeroed CRC slot on the
    // wire and are not checked. Control-direction frames (>= 0x80, i.e.
    // PC->device commands, which this parser also validates for loopback /
    // test purposes) still carry a real CRC and are enforced.
    if rec_type >= 0x80 {
        let got_crc = u16::from_le_bytes([buf[total - 2], buf[total - 1]]);
        let expected = crc16(&buf[2..FRAME_HEADER_LEN + payload_len], 0xFFFF);
        if got_crc != expected {
            return Err(FrameError::BadCrc {
                expected,
                got: got_crc,
            });
        }
    }
    let record = decode_payload(rec_type, payload);
    Ok((record, total))
}

fn rd_u16(p: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([p[o], p[o + 1]])
}
fn rd_u32(p: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([p[o], p[o + 1], p[o + 2], p[o + 3]])
}
fn rd_u64(p: &[u8], o: usize) -> u64 {
    u64::from_le_bytes([
        p[o], p[o + 1], p[o + 2], p[o + 3], p[o + 4], p[o + 5], p[o + 6], p[o + 7],
    ])
}
fn rd_f32(p: &[u8], o: usize) -> f32 {
    f32::from_le_bytes([p[o], p[o + 1], p[o + 2], p[o + 3]])
}
fn rd_f64(p: &[u8], o: usize) -> f64 {
    f64::from_le_bytes([
        p[o], p[o + 1], p[o + 2], p[o + 3], p[o + 4], p[o + 5], p[o + 6], p[o + 7],
    ])
}

fn decode_payload(rec_type: u8, p: &[u8]) -> DaqRecord {
    match rec_type {
        REC_WAVE_I => {
            if p.len() < 24 {
                return DaqRecord::Other(rec_type);
            }
            let start_index = rd_u64(p, 0);
            let timestamp_us = rd_u64(p, 8);
            let sample_rate = rd_u32(p, 16);
            let mut count = rd_u16(p, 20) as usize;
            let decimation = p[22];
            // clamp count so the f32 + u8 arrays fit in the payload.
            while count > 0 && 24 + count * 5 > p.len() {
                count -= 1;
            }
            let mut i = Vec::with_capacity(count);
            let mut o = 24;
            for _ in 0..count {
                i.push(rd_f32(p, o));
                o += 4;
            }
            let meta = p[o..o + count].to_vec();
            DaqRecord::WaveI(WaveIRecord {
                start_index,
                timestamp_us,
                sample_rate,
                decimation,
                i,
                meta,
            })
        }
        REC_WAVE_V => {
            if p.len() < 24 {
                return DaqRecord::Other(rec_type);
            }
            let start_index = rd_u64(p, 0);
            let timestamp_us = rd_u64(p, 8);
            let sample_rate = rd_u32(p, 16);
            let mut count = rd_u16(p, 20) as usize;
            // clamp count so the f32 array fits in the payload.
            while count > 0 && 24 + count * 4 > p.len() {
                count -= 1;
            }
            let mut v = Vec::with_capacity(count);
            let mut o = 24;
            for _ in 0..count {
                v.push(rd_f32(p, o));
                o += 4;
            }
            DaqRecord::WaveV(WaveVRecord {
                start_index,
                timestamp_us,
                sample_rate,
                v,
            })
        }
        REC_STATS => {
            let block = |o: usize| -> StatBlock {
                if o + 24 > p.len() {
                    return StatBlock::default();
                }
                StatBlock {
                    min: rd_f32(p, o),
                    max: rd_f32(p, o + 4),
                    mean: rd_f32(p, o + 8),
                    rms: rd_f32(p, o + 12),
                    std: rd_f32(p, o + 16),
                    count: rd_u32(p, o + 20),
                }
            };
            DaqRecord::Stats(StatsRecord {
                i: block(0),
                v: block(24),
                p: block(48),
            })
        }
        REC_ENERGY => {
            if p.len() < 52 {
                return DaqRecord::Other(rec_type);
            }
            DaqRecord::Energy(EnergyRecord {
                energy_mwh: rd_f64(p, 0),
                energy_j: rd_f64(p, 8),
                charge_mah: rd_f64(p, 16),
                charge_c: rd_f64(p, 24),
                elapsed_s: rd_f64(p, 32),
                last_i: rd_f32(p, 40),
                last_v: rd_f32(p, 44),
                last_p: rd_f32(p, 48),
            })
        }
        REC_FFT => {
            if p.len() < 8 {
                return DaqRecord::Other(rec_type);
            }
            let sample_rate = rd_u32(p, 0);
            let nbins = rd_u16(p, 4) as usize;
            let source = p[6];
            let window = p[7];
            let mut bins = Vec::with_capacity(nbins);
            let mut o = 8;
            for _ in 0..nbins {
                if o + 4 > p.len() {
                    break;
                }
                bins.push(rd_f32(p, o));
                o += 4;
            }
            DaqRecord::Fft(FftRecord {
                sample_rate,
                source,
                window,
                bins,
            })
        }
        REC_STATUS => {
            if p.len() < 20 {
                return DaqRecord::Other(rec_type);
            }
            let mut s = StatusRecord {
                sample_rate: rd_u32(p, 0),
                overflow_count: rd_u32(p, 4),
                range: p[8],
                streaming: p[9] != 0,
                range_locked: p[10] != 0,
                source_enabled: p[11] != 0,
                vdut_set: rd_f32(p, 12),
                ilimit_set: rd_f32(p, 16),
                in_voltage: 0.0,
                in_current: 0.0,
                adaq_ok_bits: 0,
                fine_err_pct: 0,
                drop_fine: 0,
                drop_coarse: 0,
                fine_diag_sticky: 0,
                frames_tx: 0,
                bytes_per_sec: 0,
                fifo_drop_frames: 0,
                ring_high_water: 0,
                wave_i_index_lo: 0,
                board_temp_analog_c: None,
                board_temp_power_c: None,
                cal_have_hi: false,
                cal_have_mid: false,
                cal_have_lo: false,
            };
            // Extension v1 (bytes 20-27): input-rail sense.
            if p.len() >= 28 {
                s.in_voltage = rd_f32(p, 20);
                s.in_current = rd_f32(p, 24);
            }
            // Extension v2 (bytes 28-39): FINE ADC health, widened drop counters (v3).
            if p.len() >= 40 {
                s.adaq_ok_bits     = p[28];
                s.fine_err_pct     = p[29];
                s.drop_fine        = rd_u32(p, 30);
                s.drop_coarse      = rd_u32(p, 34);
                s.fine_diag_sticky = p[38];
            }
            // Extension v3 (bytes 40-59): USB streaming performance counters.
            if p.len() >= 60 {
                s.frames_tx = rd_u32(p, 40);
                s.bytes_per_sec = rd_u32(p, 44);
                s.fifo_drop_frames = rd_u32(p, 48);
                s.ring_high_water = rd_u32(p, 52);
                s.wave_i_index_lo = rd_u32(p, 56);
            }
            // Extension v7 (bytes 100-103): board temperatures, 0.1 C.
            if p.len() >= 104 {
                let t0 = rd_u16(p, 100) as i16;
                let t1 = rd_u16(p, 102) as i16;
                s.board_temp_analog_c = if t0 == 0x7FFF { None } else { Some(t0 as f32 / 10.0) };
                s.board_temp_power_c  = if t1 == 0x7FFF { None } else { Some(t1 as f32 / 10.0) };
            }
            // Extension v8 (byte 104): per-range calibration validity.
            if p.len() >= 105 {
                let cal_have = p[104];
                s.cal_have_hi  = (cal_have & 0x01) != 0;
                s.cal_have_mid = (cal_have & 0x02) != 0;
                s.cal_have_lo  = (cal_have & 0x04) != 0;
            }
            DaqRecord::Status(s)
        }
        REC_MARKER => {
            // usb_marker_payload_t v2: sample_index u64, timestamp_us u64,
            // channel u8, edge u8, kind u8, _pad u8 (20 bytes).
            if p.len() < 19 {
                return DaqRecord::Other(rec_type);
            }
            DaqRecord::Marker(MarkerRecord {
                sample_index: rd_u64(p, 0),
                timestamp_us: rd_u64(p, 8),
                channel: p[16],
                edge: p[17],
                kind: p[18],
            })
        }
        other => DaqRecord::Other(other),
    }
}

/// Build a PC->device control frame with the given type and payload.
pub fn encode_command(seq: u32, cmd_type: u8, payload: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(FRAME_OVERHEAD + payload.len());
    frame.push(MAGIC0);
    frame.push(MAGIC1);
    frame.push(PROTO_VERSION);
    frame.push(cmd_type);
    frame.push(0); // flags
    frame.push(0); // reserved
    frame.extend_from_slice(&seq.to_le_bytes());
    frame.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    frame.extend_from_slice(payload);
    let crc = crc16(&frame[2..], 0xFFFF);
    frame.extend_from_slice(&crc.to_le_bytes());
    frame
}

/// usb_cmd_rate_t payload.
pub fn rate_payload(current_sps: u32, voltage_sps: u32, decimation: u8) -> Vec<u8> {
    let mut p = Vec::with_capacity(12);
    p.extend_from_slice(&current_sps.to_le_bytes());
    p.extend_from_slice(&voltage_sps.to_le_bytes());
    p.push(decimation);
    p.extend_from_slice(&[0, 0, 0]);
    p
}

/// usb_cmd_fft_t payload.
pub fn fft_payload(nbins: u16, source: u8, window: u8, enabled: bool) -> Vec<u8> {
    let mut p = Vec::with_capacity(8);
    p.extend_from_slice(&nbins.to_le_bytes());
    p.push(source);
    p.push(window);
    p.push(if enabled { 1 } else { 0 });
    p.extend_from_slice(&[0, 0, 0]);
    p
}

/// usb_cmd_source_t payload.
pub fn source_payload(vdut: f32, ilimit: f32, enable: bool) -> Vec<u8> {
    let mut p = Vec::with_capacity(12);
    p.extend_from_slice(&vdut.to_le_bytes());
    p.extend_from_slice(&ilimit.to_le_bytes());
    p.push(if enable { 1 } else { 0 });
    p.extend_from_slice(&[0, 0, 0]);
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame_record(rec_type: u8, payload: &[u8]) -> Vec<u8> {
        encode_command(7, rec_type, payload)
    }

    /// Device->PC data frame: CRC slot is zero and must NOT be checked.
    fn data_frame(rec_type: u8, payload: &[u8]) -> Vec<u8> {
        let mut f = vec![MAGIC0, MAGIC1, PROTO_VERSION, rec_type, 0, 0];
        f.extend_from_slice(&7u32.to_le_bytes());
        f.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        f.extend_from_slice(payload);
        f.extend_from_slice(&[0, 0]); // zero CRC
        f
    }

    #[test]
    fn parse_wave_i_soa() {
        let mut p = Vec::new();
        p.extend_from_slice(&1_000_000u64.to_le_bytes()); // start_index
        p.extend_from_slice(&123_456_789u64.to_le_bytes()); // timestamp_us
        p.extend_from_slice(&256_000u32.to_le_bytes());
        p.extend_from_slice(&3u16.to_le_bytes());
        p.push(1);
        p.push(0);
        for k in 0..3 {
            p.extend_from_slice(&(k as f32 * 0.001).to_le_bytes());
        }
        p.extend_from_slice(&[
            RANGE_HI, // range=0 source=FINE
            RANGE_MID | (SRC_BLEND << 2) | META_SETTLING,
            RANGE_LO | (SRC_COARSE << 2) | META_SATURATED,
        ]);
        let (rec, n) = parse_frame(&data_frame(REC_WAVE_I, &p)).unwrap();
        assert_eq!(n, FRAME_OVERHEAD + p.len());
        match rec {
            DaqRecord::WaveI(w) => {
                assert_eq!(w.start_index, 1_000_000);
                assert_eq!(w.timestamp_us, 123_456_789);
                assert_eq!(w.sample_rate, 256_000);
                assert_eq!(w.i.len(), 3);
                assert_eq!(meta_range(w.meta[2]), RANGE_LO);
                assert_eq!(meta_source(w.meta[1]), SRC_BLEND);
                assert_ne!(w.meta[2] & META_SATURATED, 0);
            }
            _ => panic!("wrong record"),
        }
    }

    #[test]
    fn parse_wave_v() {
        let mut p = Vec::new();
        p.extend_from_slice(&500u64.to_le_bytes());
        p.extend_from_slice(&42u64.to_le_bytes());
        p.extend_from_slice(&64_000u32.to_le_bytes());
        p.extend_from_slice(&2u16.to_le_bytes());
        p.extend_from_slice(&0u16.to_le_bytes());
        p.extend_from_slice(&3.3f32.to_le_bytes());
        p.extend_from_slice(&3.29f32.to_le_bytes());
        let (rec, _) = parse_frame(&data_frame(REC_WAVE_V, &p)).unwrap();
        match rec {
            DaqRecord::WaveV(w) => {
                assert_eq!(w.start_index, 500);
                assert_eq!(w.sample_rate, 64_000);
                assert_eq!(w.v, vec![3.3, 3.29]);
            }
            _ => panic!("wrong record"),
        }
    }

    #[test]
    fn data_frame_crc_ignored_control_crc_enforced() {
        // Data frame with garbage CRC still parses.
        let mut f = data_frame(REC_WAVE_V, &{
            let mut p = Vec::new();
            p.extend_from_slice(&0u64.to_le_bytes());
            p.extend_from_slice(&0u64.to_le_bytes());
            p.extend_from_slice(&64_000u32.to_le_bytes());
            p.extend_from_slice(&0u16.to_le_bytes());
            p.extend_from_slice(&0u16.to_le_bytes());
            p
        });
        let last = f.len() - 1;
        f[last] = 0xAB;
        assert!(parse_frame(&f).is_ok());
        // Control-direction frame (encode_command) still carries a valid CRC.
        let c = encode_command(1, CMD_START, &[]);
        assert_eq!(
            u16::from_le_bytes([c[c.len() - 2], c[c.len() - 1]]),
            crc16(&c[2..c.len() - 2], 0xFFFF)
        );
    }

    #[test]
    fn marker_u64_index() {
        let mut p = Vec::new();
        p.extend_from_slice(&5_000_000_000u64.to_le_bytes()); // > u32::MAX
        p.extend_from_slice(&987_654u64.to_le_bytes());
        p.push(6);
        p.push(1);
        p.push(MARK_KIND_TRIGGER);
        p.push(0);
        let (rec, _) = parse_frame(&data_frame(REC_MARKER, &p)).unwrap();
        match rec {
            DaqRecord::Marker(m) => assert_eq!(m.sample_index, 5_000_000_000),
            _ => panic!("wrong record"),
        }
    }

    #[test]
    fn parse_marker_flag_and_trigger() {
        let mut p = Vec::new();
        p.extend_from_slice(&1234u64.to_le_bytes()); // sample_index
        p.extend_from_slice(&987_654u64.to_le_bytes()); // timestamp_us
        p.push(6); // channel = IO6
        p.push(1); // edge = rising
        p.push(MARK_KIND_TRIGGER); // kind
        p.push(0); // pad
        let (rec, n) = parse_frame(&data_frame(REC_MARKER, &p)).unwrap();
        assert_eq!(n, FRAME_OVERHEAD + p.len());
        match rec {
            DaqRecord::Marker(m) => {
                assert_eq!(m.sample_index, 1234);
                assert_eq!(m.timestamp_us, 987_654);
                assert_eq!(m.channel, 6);
                assert_eq!(m.edge, 1);
                assert_eq!(m.kind, MARK_KIND_TRIGGER);
            }
            _ => panic!("wrong record"),
        }
    }

    #[test]
    fn status_perf_extension() {
        let mut p = vec![0u8; 56];
        p[36..40].copy_from_slice(&777u32.to_le_bytes()); // frames_tx
        p[40..44].copy_from_slice(&1_600_000u32.to_le_bytes()); // bytes_per_sec
        p[44..48].copy_from_slice(&3u32.to_le_bytes()); // fifo_drop_frames
        let (rec, _) = parse_frame(&data_frame(REC_STATUS, &p)).unwrap();
        match rec {
            DaqRecord::Status(s) => {
                assert_eq!(s.frames_tx, 777);
                assert_eq!(s.bytes_per_sec, 1_600_000);
                assert_eq!(s.fifo_drop_frames, 3);
            }
            _ => panic!("wrong record"),
        }
    }

    #[test]
    fn version_1_rejected() {
        let mut f = data_frame(REC_STATUS, &[0u8; 20]);
        f[2] = 1;
        assert!(matches!(parse_frame(&f), Err(FrameError::BadVersion(1))));
    }

    #[test]
    fn crc_roundtrip_and_parse_status() {
        let mut p = Vec::new();
        p.extend_from_slice(&250_000u32.to_le_bytes()); // sample_rate
        p.extend_from_slice(&3u32.to_le_bytes()); // overflow_count
        p.push(RANGE_MID);
        p.push(1); // streaming
        p.push(0); // range_locked
        p.push(1); // source_enabled
        p.extend_from_slice(&3.3f32.to_le_bytes());
        p.extend_from_slice(&0.5f32.to_le_bytes());
        let frame = data_frame(REC_STATUS, &p);
        let (rec, n) = parse_frame(&frame).unwrap();
        assert_eq!(n, frame.len());
        match rec {
            DaqRecord::Status(s) => {
                assert_eq!(s.sample_rate, 250_000);
                assert_eq!(s.overflow_count, 3);
                assert_eq!(s.range, RANGE_MID);
                assert!(s.streaming);
                assert!(!s.range_locked);
                assert!(s.source_enabled);
                assert!((s.vdut_set - 3.3).abs() < 1e-5);
            }
            _ => panic!("wrong record"),
        }
    }

    #[test]
    fn truncated_then_complete() {
        let frame = data_frame(REC_MARKER, &[]);
        let short = &frame[..frame.len() - 1];
        assert!(matches!(
            parse_frame(short),
            Err(FrameError::Truncated { .. })
        ));
    }

    #[test]
    fn bad_crc_detected_on_control_frame() {
        // Control-direction (>= 0x80) frames still enforce CRC.
        let mut frame = frame_record(CMD_RESET_ENERGY, &[0u8; 4]);
        let last = frame.len() - 1;
        frame[last] ^= 0xFF;
        assert!(matches!(parse_frame(&frame), Err(FrameError::BadCrc { .. })));
    }

    #[test]
    fn parse_stats_and_energy_and_fft() {
        let mut sp = Vec::new();
        for _ in 0..3 {
            sp.extend_from_slice(&(-1.0f32).to_le_bytes()); // min
            sp.extend_from_slice(&1.0f32.to_le_bytes()); // max
            sp.extend_from_slice(&0.0f32.to_le_bytes()); // mean
            sp.extend_from_slice(&0.5f32.to_le_bytes()); // rms
            sp.extend_from_slice(&0.1f32.to_le_bytes()); // std
            sp.extend_from_slice(&100u32.to_le_bytes()); // count
        }
        let (rec, _) = parse_frame(&data_frame(REC_STATS, &sp)).unwrap();
        match rec {
            DaqRecord::Stats(s) => assert_eq!(s.i.count, 100),
            _ => panic!("wrong record"),
        }

        let mut ep = Vec::new();
        ep.extend_from_slice(&1.0f64.to_le_bytes());
        ep.extend_from_slice(&3.6f64.to_le_bytes());
        ep.extend_from_slice(&2.0f64.to_le_bytes());
        ep.extend_from_slice(&7.2f64.to_le_bytes());
        ep.extend_from_slice(&10.0f64.to_le_bytes());
        ep.extend_from_slice(&0.5f32.to_le_bytes());
        ep.extend_from_slice(&3.3f32.to_le_bytes());
        ep.extend_from_slice(&1.65f32.to_le_bytes());
        let (rec, _) = parse_frame(&data_frame(REC_ENERGY, &ep)).unwrap();
        match rec {
            DaqRecord::Energy(e) => assert!((e.energy_mwh - 1.0).abs() < 1e-9),
            _ => panic!("wrong record"),
        }

        let mut fp = Vec::new();
        fp.extend_from_slice(&64_000u32.to_le_bytes());
        fp.extend_from_slice(&2u16.to_le_bytes());
        fp.push(0); // source
        fp.push(0); // window
        fp.extend_from_slice(&1.0f32.to_le_bytes());
        fp.extend_from_slice(&2.0f32.to_le_bytes());
        let (rec, _) = parse_frame(&data_frame(REC_FFT, &fp)).unwrap();
        match rec {
            DaqRecord::Fft(f) => assert_eq!(f.bins, vec![1.0, 2.0]),
            _ => panic!("wrong record"),
        }
    }
}
