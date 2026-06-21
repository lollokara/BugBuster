// =============================================================================
// daq_usb.rs — ESP32-P4 DAQ USB-HS vendor-bulk transport + synthetic source.
//
// The P4 exposes its own dedicated USB-HS port to the PC (independent of the
// S3 CDC/BBP link): VID 0x303A, PID 0x4001, vendor interface 0, bulk IN 0x81 /
// bulk OUT 0x01, 512-byte HS packets. Measurement frames stream device->PC;
// control commands flow PC->device. See daq_proto.rs for the byte layout.
//
// `MockDaqTransport` synthesises the same record stream so the desktop app can
// run as a "Demo / Mock device" with no hardware attached.
// =============================================================================

use crate::daq_proto::{
    self, DaqRecord, EnergyRecord, FftRecord, StatBlock, StatsRecord, StatusRecord, WaveSample,
    WaveformRecord, RANGE_HI, RANGE_LO, RANGE_MID, SRC_BLEND, SRC_COARSE, SRC_FINE,
};
use anyhow::{anyhow, Result};
use nusb::transfer::RequestBuffer;
use std::time::Instant;

pub const DAQ_VID: u16 = 0x303A;
pub const DAQ_PID: u16 = 0x4001;
const DAQ_IFACE: u8 = 0;
const DAQ_EP_IN: u8 = 0x81;
const DAQ_EP_OUT: u8 = 0x01;

/// Common interface implemented by the real USB transport and the mock source.
pub trait DaqTransport: Send {
    /// Read and decode whatever frames are currently available. Blocks until at
    /// least one record is produced or a timeout/error occurs.
    fn read_records(&mut self) -> Result<Vec<DaqRecord>>;
    /// Send a control command (type + payload); the transport frames it.
    fn send(&mut self, cmd_type: u8, payload: &[u8]) -> Result<()>;
}

/// True if a P4 DAQ device is present on USB.
pub fn daq_usb_present() -> bool {
    nusb::list_devices()
        .map(|devs| {
            devs.into_iter()
                .any(|d| d.vendor_id() == DAQ_VID && d.product_id() == DAQ_PID)
        })
        .unwrap_or(false)
}

pub struct DaqUsbConnection {
    interface: Option<nusb::Interface>,
    connected: bool,
    rx: Vec<u8>,
    out_seq: u32,
}

impl Default for DaqUsbConnection {
    fn default() -> Self {
        Self::new()
    }
}

impl DaqUsbConnection {
    pub fn new() -> Self {
        Self {
            interface: None,
            connected: false,
            rx: Vec::new(),
            out_seq: 0,
        }
    }

    pub fn connect(&mut self) -> Result<()> {
        let devices =
            nusb::list_devices().map_err(|e| anyhow!("USB enumeration failed: {}", e))?;
        for info in devices {
            if info.vendor_id() == DAQ_VID && info.product_id() == DAQ_PID {
                let device = info
                    .open()
                    .map_err(|e| anyhow!("Failed to open DAQ USB device: {}", e))?;
                let iface = device
                    .claim_interface(DAQ_IFACE)
                    .map_err(|e| anyhow!("Failed to claim DAQ interface {}: {}", DAQ_IFACE, e))?;
                log::info!("DAQ USB interface claimed (VID={DAQ_VID:04X} PID={DAQ_PID:04X})");
                self.interface = Some(iface);
                self.connected = true;
                self.rx.clear();
                return Ok(());
            }
        }
        Err(anyhow!(
            "DAQ HAT not found on USB (VID={:04X} PID={:04X})",
            DAQ_VID,
            DAQ_PID
        ))
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn close(&mut self) {
        self.interface = None;
        self.connected = false;
        self.rx.clear();
    }

    /// Parse as many complete frames as the buffer holds, resyncing on garbage.
    fn drain_frames(&mut self) -> Vec<DaqRecord> {
        let mut out = Vec::new();
        loop {
            match daq_proto::parse_frame(&self.rx) {
                Ok((rec, consumed)) => {
                    self.rx.drain(0..consumed);
                    out.push(rec);
                }
                Err(daq_proto::FrameError::Truncated { .. })
                | Err(daq_proto::FrameError::ShortHeader(_)) => break,
                Err(daq_proto::FrameError::BadMagic)
                | Err(daq_proto::FrameError::BadVersion(_))
                | Err(daq_proto::FrameError::BadCrc { .. })
                | Err(daq_proto::FrameError::PayloadTooLong(_)) => {
                    // Resync: drop one byte and retry.
                    if self.rx.is_empty() {
                        break;
                    }
                    self.rx.remove(0);
                }
            }
        }
        out
    }
}

impl DaqTransport for DaqUsbConnection {
    fn read_records(&mut self) -> Result<Vec<DaqRecord>> {
        let iface = self
            .interface
            .as_ref()
            .ok_or_else(|| anyhow!("DAQ USB not connected"))?
            .clone();
        let rt = tokio::runtime::Handle::current();
        loop {
            // Parse anything already buffered first.
            let ready = self.drain_frames();
            if !ready.is_empty() {
                return Ok(ready);
            }
            let completion = rt
                .block_on(tokio::time::timeout(
                    std::time::Duration::from_secs(5),
                    iface.bulk_in(DAQ_EP_IN, RequestBuffer::new(16384)),
                ))
                .map_err(|_| anyhow!("DAQ USB read timed out (5 s)"))?;
            let data = completion
                .into_result()
                .map_err(|e| anyhow!("DAQ bulk IN failed: {}", e))?;
            if data.is_empty() {
                return Err(anyhow!("DAQ bulk IN returned 0 bytes"));
            }
            self.rx.extend_from_slice(&data);
        }
    }

    fn send(&mut self, cmd_type: u8, payload: &[u8]) -> Result<()> {
        let iface = self
            .interface
            .as_ref()
            .ok_or_else(|| anyhow!("DAQ USB not connected"))?
            .clone();
        let frame = daq_proto::encode_command(self.out_seq, cmd_type, payload);
        self.out_seq = self.out_seq.wrapping_add(1);
        let rt = tokio::runtime::Handle::current();
        let completion = rt.block_on(iface.bulk_out(DAQ_EP_OUT, frame));
        completion
            .into_result()
            .map_err(|e| anyhow!("DAQ bulk OUT failed: {}", e))?;
        Ok(())
    }
}

// =============================================================================
// Mock / synthetic DAQ source
// =============================================================================

/// Synthesises a realistic power-profile stream: a low-power sleep baseline with
/// periodic active bursts and the occasional inrush spike that exercises every
/// current range and the FINE/COARSE/BLEND fusion path.
pub struct MockDaqTransport {
    sample_rate: u32,
    decimation: u8,
    running: bool,
    started: Instant,
    sample_idx: u64,
    seq: u32,
    // SMU / source
    vdut: f32,
    source_enabled: bool,
    range_lock: u8, // 0xFF = auto
    // DSP
    fft_enabled: bool,
    fft_nbins: u16,
    fft_source: u8,
    fft_window: u8,
    // accumulators
    energy_j: f64,
    charge_c: f64,
    last_aux: Instant,
}

impl Default for MockDaqTransport {
    fn default() -> Self {
        Self::new()
    }
}

impl MockDaqTransport {
    pub fn new() -> Self {
        Self {
            sample_rate: 250_000,
            decimation: 1,
            running: false,
            started: Instant::now(),
            sample_idx: 0,
            seq: 0,
            vdut: 3.3,
            source_enabled: true,
            range_lock: 0xFF,
            fft_enabled: true,
            fft_nbins: 256,
            fft_source: 0,
            fft_window: 1,
            energy_j: 0.0,
            charge_c: 0.0,
            last_aux: Instant::now(),
        }
    }

    /// Effective current at absolute time `t` seconds — a duty-cycled load.
    fn current_at(&self, t: f64) -> f32 {
        if !self.source_enabled {
            return 0.0;
        }
        // Sleep baseline ~80 µA.
        let mut i = 80e-6_f64;
        // Active window: 40 ms burst every 200 ms at ~45 mA with ripple.
        let phase = (t % 0.200) / 0.200;
        if phase < 0.20 {
            let ripple = 1.0 + 0.15 * (2.0 * std::f64::consts::PI * 2_000.0 * t).sin();
            i = 0.045 * ripple;
        }
        // Inrush spike once every ~2 s, ~1.6 A for 1 ms — pushes into RANGE_LO.
        if (t % 2.0) < 0.001 {
            i = 1.6;
        }
        // A little broadband noise.
        let noise = 0.002 * ((t * 91_193.0).sin() + (t * 53_201.0).cos());
        (i + noise).max(0.0) as f32
    }

    fn range_for(&self, i: f32) -> u8 {
        if self.range_lock != 0xFF {
            return self.range_lock;
        }
        if i < 1.4e-3 {
            RANGE_HI
        } else if i < 37e-3 {
            RANGE_MID
        } else {
            RANGE_LO
        }
    }

    fn synth_waveform(&mut self, count: usize) -> WaveformRecord {
        let dt = 1.0_f64 / self.sample_rate as f64;
        let start_seq = self.sample_idx as u32;
        let mut samples = Vec::with_capacity(count);
        let mut prev_range = if self.sample_idx == 0 {
            RANGE_HI
        } else {
            self.range_for(self.current_at(self.sample_idx as f64 * dt))
        };
        for k in 0..count {
            let n = self.sample_idx + k as u64;
            let t = n as f64 * dt;
            let i = self.current_at(t);
            let range = self.range_for(i);
            // During a range transition the FINE path is settling, so COARSE
            // carries the signal (BLEND at the seams) — gap is filled, never lost.
            let source = if range != prev_range {
                SRC_BLEND
            } else if range == RANGE_LO {
                SRC_COARSE
            } else {
                SRC_FINE
            };
            prev_range = range;
            // Voltage droops slightly under load.
            let v = (self.vdut - (i * 0.05)).max(0.0);
            let p = i * v;
            let flags = if i > 2.4 { 1 } else { 0 };
            samples.push(WaveSample {
                i,
                v,
                p,
                range,
                source,
                flags,
            });
            // Accumulate energy / charge.
            self.energy_j += p as f64 * dt;
            self.charge_c += i as f64 * dt;
        }
        self.sample_idx += count as u64;
        WaveformRecord {
            start_seq,
            sample_rate: self.sample_rate,
            decimation: self.decimation,
            samples,
        }
    }

    fn synth_stats(&self) -> StatsRecord {
        // Cheap representative block; the front-end mostly shows last values.
        let blk = |mean: f32, max: f32| StatBlock {
            min: 0.0,
            max,
            mean,
            rms: mean * 1.1,
            std: mean * 0.3,
            count: self.sample_rate,
        };
        StatsRecord {
            i: blk(0.010, 1.6),
            v: blk(self.vdut, self.vdut),
            p: blk(0.033, self.vdut * 1.6),
        }
    }

    fn synth_energy(&self) -> EnergyRecord {
        let elapsed = self.started.elapsed().as_secs_f64();
        let last_i = self.current_at(elapsed);
        let last_v = (self.vdut - last_i * 0.05).max(0.0);
        EnergyRecord {
            energy_mwh: self.energy_j / 3.6,
            energy_j: self.energy_j,
            charge_mah: self.charge_c / 3.6,
            charge_c: self.charge_c,
            elapsed_s: elapsed,
            last_i,
            last_v,
            last_p: last_i * last_v,
        }
    }

    fn synth_fft(&self) -> FftRecord {
        let n = self.fft_nbins.max(2) as usize;
        let mut bins = Vec::with_capacity(n);
        for k in 0..n {
            let f = k as f32 / n as f32;
            // DC-heavy with a peak near the 5 Hz burst rate and 2 kHz ripple.
            let burst = 0.6 * (-((f - 0.001).abs() * 400.0)).exp();
            let ripple = 0.3 * (-((f - 0.016).abs() * 120.0)).exp();
            let floor = 0.02 / (1.0 + 50.0 * f);
            bins.push(burst + ripple + floor);
        }
        FftRecord {
            sample_rate: self.sample_rate,
            source: self.fft_source,
            window: self.fft_window,
            bins,
        }
    }

    fn synth_status(&self) -> StatusRecord {
        StatusRecord {
            sample_rate: self.sample_rate,
            overflow_count: 0,
            range: self.range_for(self.current_at(self.started.elapsed().as_secs_f64())),
            streaming: self.running,
            range_locked: self.range_lock != 0xFF,
            source_enabled: self.source_enabled,
            vdut_set: self.vdut,
            ilimit_set: 2.5,
        }
    }
}

impl DaqTransport for MockDaqTransport {
    fn read_records(&mut self) -> Result<Vec<DaqRecord>> {
        // Pace at ~30 ms cadence to mimic a live stream without pegging a core.
        std::thread::sleep(std::time::Duration::from_millis(30));
        if !self.running {
            // Idle: still emit a heartbeat so the UI shows "connected, stopped".
            return Ok(vec![DaqRecord::Status(self.synth_status())]);
        }
        let mut out = Vec::new();
        // ~30 ms of samples, decimated for the waveform view, capped per frame.
        let raw = (self.sample_rate as f64 * 0.030) as usize;
        let dec = self.decimation.max(1) as usize;
        let count = (raw / dec).clamp(1, 2048);
        out.push(DaqRecord::Waveform(self.synth_waveform(count * dec)));

        // Aux records ~5 Hz.
        if self.last_aux.elapsed().as_millis() >= 200 {
            self.last_aux = Instant::now();
            out.push(DaqRecord::Stats(self.synth_stats()));
            out.push(DaqRecord::Energy(self.synth_energy()));
            out.push(DaqRecord::Status(self.synth_status()));
            if self.fft_enabled {
                out.push(DaqRecord::Fft(self.synth_fft()));
            }
        }
        Ok(out)
    }

    fn send(&mut self, cmd_type: u8, payload: &[u8]) -> Result<()> {
        match cmd_type {
            daq_proto::CMD_START => {
                self.running = true;
                self.started = Instant::now();
                self.sample_idx = 0;
                self.energy_j = 0.0;
                self.charge_c = 0.0;
            }
            daq_proto::CMD_STOP => self.running = false,
            daq_proto::CMD_SET_RATE if payload.len() >= 9 => {
                let sps = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
                if sps > 0 {
                    self.sample_rate = sps;
                }
                self.decimation = payload[8].max(1);
            }
            daq_proto::CMD_RANGE_LOCK if !payload.is_empty() => {
                self.range_lock = payload[0];
            }
            daq_proto::CMD_RESET_ENERGY => {
                self.energy_j = 0.0;
                self.charge_c = 0.0;
            }
            daq_proto::CMD_FFT_CONFIG if payload.len() >= 5 => {
                self.fft_nbins = u16::from_le_bytes([payload[0], payload[1]]);
                self.fft_source = payload[2];
                self.fft_window = payload[3];
                self.fft_enabled = payload[4] != 0;
            }
            daq_proto::CMD_SET_SOURCE if payload.len() >= 9 => {
                self.vdut = f32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
                self.source_enabled = payload[8] != 0;
            }
            _ => {}
        }
        let _ = self.seq.wrapping_add(1);
        Ok(())
    }
}
