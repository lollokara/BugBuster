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
pub const PROTO_VERSION: u8 = 1;
pub const FRAME_HEADER_LEN: usize = 12;
pub const FRAME_CRC_LEN: usize = 2;
pub const FRAME_OVERHEAD: usize = FRAME_HEADER_LEN + FRAME_CRC_LEN;
pub const MAX_PAYLOAD: usize = 4096;

// Record / frame types. 0x00..0x7F = device->PC data, 0x80+ = PC->device control.
pub const REC_WAVEFORM: u8 = 0x01;
pub const REC_STATS: u8 = 0x02;
pub const REC_ENERGY: u8 = 0x03;
pub const REC_FFT: u8 = 0x04;
pub const REC_MARKER: u8 = 0x05;
pub const REC_STATUS: u8 = 0x06;

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

/// One fused waveform sample (decoded from the 16-byte on-wire struct).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WaveSample {
    pub i: f32,
    pub v: f32,
    pub p: f32,
    pub range: u8,
    pub source: u8,
    pub flags: u8,
}

#[derive(Debug, Clone, PartialEq)]
pub struct WaveformRecord {
    pub start_seq: u32,
    pub sample_rate: u32,
    pub decimation: u8,
    pub samples: Vec<WaveSample>,
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
    /// Extension v2 (bytes 28-35): FINE ADC health.
    /// `adaq_ok_bits`: bit0=FINE ok, bit1=COARSE ok, bit2=VOLT ok. 0 = old firmware.
    pub adaq_ok_bits: u8,
    /// Percentage of FINE samples that carried a STATUS_ERR bit (0-100).
    /// 100 means the fused stream is running on COARSE only.
    pub fine_err_pct: u8,
    /// FINE pairing-resync drop counter (saturates at 65535).
    pub drop_fine: u16,
    /// COARSE pairing-resync drop counter (saturates at 65535).
    pub drop_coarse: u16,
    /// OR of all MASTER_STATUS (0x2D) bytes seen on the FINE ADAQ since boot.
    /// 0xFF = FINE ADAQ did not initialise. Bit map: 7=MASTER_ERR 6=ADC_ERR
    /// 5=DIG_ERR 4=CLK_QUAL 3=FILT_SAT 2=FILT_UNSETTLED 1=SPI_ERR 0=POR.
    pub fine_diag_sticky: u8,
}

/// One digital event marker (flag or trigger), decoded from the 16-byte wire
/// struct usb_marker_payload_t. Markers carry the fused-sample index the event
/// aligns to, so they survive the host's multi-resolution decimation untouched.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MarkerRecord {
    pub sample_index: u32,
    pub timestamp_us: u64,
    pub channel: u8, // S3 IO number (1..12)
    pub edge: u8,    // 0 = falling, 1 = rising
    pub kind: u8,    // MARK_KIND_FLAG / MARK_KIND_TRIGGER
}

#[derive(Debug, Clone, PartialEq)]
pub enum DaqRecord {
    Waveform(WaveformRecord),
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
    let got_crc = u16::from_le_bytes([buf[total - 2], buf[total - 1]]);
    let expected = crc16(&buf[2..FRAME_HEADER_LEN + payload_len], 0xFFFF);
    if got_crc != expected {
        return Err(FrameError::BadCrc {
            expected,
            got: got_crc,
        });
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
        REC_WAVEFORM => {
            if p.len() < 12 {
                return DaqRecord::Other(rec_type);
            }
            let start_seq = rd_u32(p, 0);
            let sample_rate = rd_u32(p, 4);
            let count = rd_u16(p, 8) as usize;
            let decimation = p[10];
            let mut samples = Vec::with_capacity(count);
            let mut o = 12;
            for _ in 0..count {
                if o + 16 > p.len() {
                    break;
                }
                samples.push(WaveSample {
                    i: rd_f32(p, o),
                    v: rd_f32(p, o + 4),
                    p: rd_f32(p, o + 8),
                    range: p[o + 12],
                    source: p[o + 13],
                    flags: p[o + 14],
                });
                o += 16;
            }
            DaqRecord::Waveform(WaveformRecord {
                start_seq,
                sample_rate,
                decimation,
                samples,
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
            };
            // Extension v1 (bytes 20-27): input-rail sense.
            if p.len() >= 28 {
                s.in_voltage = rd_f32(p, 20);
                s.in_current = rd_f32(p, 24);
            }
            // Extension v2 (bytes 28-35): FINE ADC health.
            if p.len() >= 36 {
                s.adaq_ok_bits     = p[28];
                s.fine_err_pct     = p[29];
                s.drop_fine        = rd_u16(p, 30);
                s.drop_coarse      = rd_u16(p, 32);
                s.fine_diag_sticky = p[34];
            }
            DaqRecord::Status(s)
        }
        REC_MARKER => {
            // usb_marker_payload_t: sample_index u32, timestamp_us u64,
            // channel u8, edge u8, kind u8, _pad u8 (16 bytes).
            if p.len() < 15 {
                return DaqRecord::Other(rec_type);
            }
            DaqRecord::Marker(MarkerRecord {
                sample_index: rd_u32(p, 0),
                timestamp_us: rd_u64(p, 4),
                channel: p[12],
                edge: p[13],
                kind: p[14],
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
        let frame = frame_record(REC_STATUS, &p);
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
    fn parse_waveform_samples() {
        let mut p = Vec::new();
        p.extend_from_slice(&0u32.to_le_bytes()); // start_seq
        p.extend_from_slice(&250_000u32.to_le_bytes()); // sample_rate
        p.extend_from_slice(&2u16.to_le_bytes()); // count
        p.push(1); // decimation
        p.push(0); // pad
        for k in 0..2u8 {
            p.extend_from_slice(&(k as f32).to_le_bytes()); // i
            p.extend_from_slice(&3.3f32.to_le_bytes()); // v
            p.extend_from_slice(&(k as f32 * 3.3).to_le_bytes()); // p
            p.push(RANGE_LO);
            p.push(SRC_COARSE);
            p.push(0);
            p.push(0);
        }
        let frame = frame_record(REC_WAVEFORM, &p);
        let (rec, _n) = parse_frame(&frame).unwrap();
        match rec {
            DaqRecord::Waveform(w) => {
                assert_eq!(w.samples.len(), 2);
                assert_eq!(w.sample_rate, 250_000);
                assert_eq!(w.samples[1].source, SRC_COARSE);
            }
            _ => panic!("wrong record"),
        }
    }

    #[test]
    fn truncated_then_complete() {
        let frame = frame_record(REC_RESET_MARKER(), &[]);
        let short = &frame[..frame.len() - 1];
        assert!(matches!(
            parse_frame(short),
            Err(FrameError::Truncated { .. })
        ));
    }

    #[allow(non_snake_case)]
    fn REC_RESET_MARKER() -> u8 {
        REC_MARKER
    }

    #[test]
    fn bad_crc_detected() {
        let mut frame = frame_record(REC_ENERGY, &[0u8; 52]);
        let last = frame.len() - 1;
        frame[last] ^= 0xFF;
        assert!(matches!(parse_frame(&frame), Err(FrameError::BadCrc { .. })));
    }

    #[test]
    fn parse_marker_flag_and_trigger() {
        let mut p = Vec::new();
        p.extend_from_slice(&1234u32.to_le_bytes()); // sample_index
        p.extend_from_slice(&987_654u64.to_le_bytes()); // timestamp_us
        p.push(6); // channel = IO6
        p.push(1); // edge = rising
        p.push(MARK_KIND_TRIGGER); // kind
        p.push(0); // pad
        let frame = frame_record(REC_MARKER, &p);
        let (rec, n) = parse_frame(&frame).unwrap();
        assert_eq!(n, frame.len());
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
}
