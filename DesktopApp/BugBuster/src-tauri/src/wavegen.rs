// =============================================================================
// wavegen.rs - Waveform generator (drives DAC from a tokio task)
// =============================================================================

use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use anyhow::Result;
use crate::bbp::{self, PayloadWriter};
use crate::connection_manager::ConnectionManager;

pub struct WavegenState {
    running: Arc<AtomicBool>,
}

impl WavegenState {
    pub fn new() -> Self {
        Self { running: Arc::new(AtomicBool::new(false)) }
    }

    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::Relaxed)
    }

    pub fn start(
        &self,
        mgr: Arc<ConnectionManager>,
        channel: u8,
        waveform: String,
        freq_hz: f64,
        amplitude: f64,
        offset: f64,
    ) {
        self.running.store(true, Ordering::Relaxed);
        let running = self.running.clone();

        tokio::spawn(async move {
            let update_rate = 100.0f64; // Updates per second (limited by command latency)
            let dt = 1.0 / update_rate;
            let mut t = 0.0f64;

            while running.load(Ordering::Relaxed) {
                let phase = (t * freq_hz).fract();
                let value = match waveform.as_str() {
                    "sine" => offset + amplitude * (phase * std::f64::consts::TAU).sin(),
                    "square" => offset + if phase < 0.5 { amplitude } else { -amplitude },
                    "triangle" => {
                        let v = if phase < 0.25 { phase * 4.0 }
                            else if phase < 0.75 { 2.0 - phase * 4.0 }
                            else { phase * 4.0 - 4.0 };
                        offset + amplitude * v
                    }
                    "sawtooth" => offset + amplitude * (2.0 * phase - 1.0),
                    _ => offset,
                };

                let voltage = value.clamp(-12.0, 12.0) as f32;
                let bipolar = voltage < 0.0;
                let mut pw = PayloadWriter::new();
                pw.put_u8(channel);
                pw.put_f32(voltage);
                pw.put_bool(bipolar);
                let _ = mgr.send_command(bbp::CMD_SET_DAC_VOLTAGE, &pw.buf).await;

                t += dt;
                tokio::time::sleep(std::time::Duration::from_millis((dt * 1000.0) as u64)).await;
            }

            log::info!("Wavegen stopped on channel {}", channel);
        });
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::Relaxed);
    }
}
