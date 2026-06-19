# BugBuster IoT Power Analyzer — Architecture (ESP32-P4 DAQ HAT)

Status: Design / planning · Last updated: 2026-06-19

A Joulescope / Otii / Nordic PPK2-class precision power analyzer built on the
BugBuster DAQ HAT. Measures DUT current (nA–3A, seamless hardware autorange) and
voltage (true 4-wire Kelvin sense), can also **source** the DUT (0–20 V SMU),
computes power / energy / spectrum on-device, and streams results to a PC app
over USB High-Speed.

---

## 1. System topology

```
        ┌──────────────────────────── DAQ HAT ────────────────────────────┐
        │                                                                  │
 PC ◀──USB HS bulk (measurement stream)──▶  ESP32-P4  ──DDP──▶  ESP32-C6   │
        │                                  (acquire+DSP)        (display/  │
        │                                       ▲                wireless) │
        └───────────────────────────────────────┼──────────────────────────┘
                                                 │ inter-proc UART + SYNC line
 PC ◀──USB CDC (BBP control)──▶  ESP32-S3 mainboard  ─ HV-tolerant world IO,
                                 (BBP host, IO owner)   digital event markers
```

- **Control plane:** BBP arrives at the **ESP32-S3** mainboard; the DAQ-specific
  subset is forwarded to the **ESP32-P4** over the inter-processor UART.
- **Data plane:** the P4 streams the high-rate measurement data out its **own
  USB HS** port directly to the PC, independent of BBP.
- **World IO / markers:** the **S3** owns the HV-tolerant IO and the digital
  event-marker channels. Markers are correlated to P4 samples through a shared
  **sync epoch** established before each acquisition (see §8).
- The RP2040 vendor-bulk HAT is a *different* HAT and is **not present** when
  this DAQ HAT is installed.

---

## 2. Analog front-end and ranging

Series-shunt ladder **51 Ω + 2 Ω + 50 mΩ**. As current rises, an **analog
feedback loop** closes bypass switches that short the larger shunts. Each sense
node is read through an **AD8411A** current-sense amplifier (gain 50 V/V,
−2…+70 V common-mode).

| Range | Active shunt | ADC path | Approx. span | Role |
|------|--------------|----------|--------------|------|
| HI  | 51 Ω  | FINE (muxed) | nA – ~2 mA  | precision low-current |
| MID | 2 Ω (51 bypassed) | FINE (muxed) | ~2 – 50 mA | precision mid |
| LO  | 50 mΩ | COARSE (dedicated) | ~50 mA – 3 A | high-current + gap-fill |

ADC assignment (3× ADAQ7769-1):

- **FINE** — one ADAQ multiplexed across the 51 Ω and 2 Ω nodes (the 51 Ω and
  2 Ω can be bypassed). Provides the two precision sub-ranges.
- **COARSE** — a dedicated ADAQ permanently on the 50 mΩ node. Always valid, so
  it is the continuity reference that fills the gap whenever FINE switches range.
- **VOLTAGE** — one ADAQ, **differential**, fed by 4-wire **Kelvin sense** lines
  at the DUT terminals → true `V_dut`.

Range math (AD8411A ×50):

- 51 Ω → 2550·I V/A → 5 V at ~2 mA → nA…2 mA.
- 2 Ω → 100·I V/A → 5 V at 50 mA → ~2…50 mA.
- 50 mΩ → 2.5·I V/A → 7.5 V at 3 A → ~50 mA…3 A.

**Autorange authority:** the analog loop performs the fast switching. The P4
*observes* the active range by reading the bypass-switch states through the
1 kΩ-tapped GPIOs, and can **force/override** a range. A per-sample **range tag**
travels with the data so the DSP applies the correct shunt value and calibration.

---

## 3. Current reconstruction (fusion)

```
 FINE (51Ω|2Ω) ─┐
 COARSE (50mΩ) ─┼─▶ fusion + hysteresis + cross-fade ─▶ i[n] (seamless, calibrated)
 range state ──┘
```

- Use **FINE** when its range is settled and unsaturated; switch to **COARSE**
  during/after a range transition until FINE re-settles (the ADAQ filter settle
  time per ODR/filter is known from the datasheet settling tables).
- **Hysteresis** on the range boundaries prevents chatter; a short cross-fade
  across transitions yields a glitch-free waveform.
- FINE and COARSE ADAQ are **SYNC'd to the same MCLK** (the firmware sync
  engine), so their sample instants align deterministically.

---

## 4. Time alignment and power

- Current **250 kSPS** (configurable), voltage **50 kSPS** (configurable), all
  SYNC-aligned. Voltage (slow supply rail) is **held / linearly interpolated**
  up to the current rate.
- Instantaneous power: `p[n] = v[n] · i[n]` using Kelvin-sensed `V_dut`.

---

## 5. On-device DSP pipeline (ESP32-P4 HP core + esp-dsp)

```
 i[n] ─┬─▶ multi-resolution (min/max/mean decimation, zoom levels)
       │
 v[n] ─┴─▶ p = v·i ─┬─▶ energy mWh / J, charge mAh / C   (double accumulators)
                    ├─▶ stats: min / max / mean / RMS / std (rolling windows)
                    └─▶ Welch FFT: current + power spectrum (continuous rolling)
```

- **Energy / charge** accumulated in `double` (trapezoidal integration) to avoid
  long-run drift; resettable and windowable between markers.
- **Multi-resolution reductions** (Joulescope-style) let the PC pan/zoom without
  transferring full-rate data.
- **FFT/Welch** runs **continuously** (rolling spectrum), selectable length and
  window, on current and power. esp-dsp uses the P4 FPU + DSP/SIMD extension.

---

## 6. P4 data path

`adaq_stream` (DRDY → DMA → PSRAM ring, already implemented) → fusion → DSP →
USB framer. Bus map matches the implemented topology: **FINE on the dedicated
SPI bus A**; **COARSE + slow VOLTAGE on shared bus B** (50 kSPS voltage leaves
most of bus B to COARSE). 32 MB PSRAM is available for the ring, multi-resolution
buffers, and deep capture.

---

## 7. USB HS streaming protocol (new vendor-bulk)

A compact framed stream, separate from BBP. Typed records:

| Record | Payload |
|--------|---------|
| `WAVEFORM` | decimated i / v / p block + range mask + seq + sample-epoch timestamp |
| `STATS`    | min / max / mean / RMS / std over the window |
| `ENERGY`   | accumulated mWh / J, mAh / C, elapsed time |
| `FFT_BINS` | spectrum magnitude bins (current and/or power) |
| `MARKER`   | digital event index + S3 timestamp mapped to sample index |

Control (start/stop, sample rate, range lock/override, reset energy, FFT config,
trigger config) flows over a control endpoint or via BBP-through-S3. Bandwidth
budget: 2 ch × 250 kSPS × 4 B ≈ **2 MB/s**, well within USB HS (~40 MB/s), with
ample headroom for full-rate raw bursts.

---

## 8. P4 ↔ S3 coordination and markers

- **Pre-acquisition handshake + hardware SYNC pulse** zeroes both timebases.
- The **S3** timestamps its digital-marker events in the shared epoch; those map
  to exact P4 sample indices.
- **Re-sync after** the run bounds any clock drift across the capture.

---

## 9. Source / SMU integration

- Programmable supply **0–20 V**, trimmed via the **DS4424** IDAC.
- Current limit **2.5 A**, trimmable down via the DS4424.
- Modes: **ammeter** (external supply, measure inline) and **source-measure**
  (instrument supplies the DUT). Source set-points and limits are part of the
  control plane; the measurement chain is identical in both modes.

---

## 10. Proposed module layout (extends the current P4 tree)

```
src/range/range_manager.{c,h}     read range GPIOs, transition detect, per-range cal
src/fusion/current_fusion.{c,h}   FINE+COARSE → seamless i[n]
src/dsp/power_dsp.{c,h}           p, energy, charge, stats
src/dsp/spectrum.{c,h}            Welch FFT (esp-dsp)
src/dsp/multires.{c,h}            min/max/mean decimation reductions
src/stream/usb_stream.{c,h}       TinyUSB HS vendor-bulk + framing
src/link/s3_link.{c,h}            P4↔S3 UART + sync epoch
src/board/daq_board.{c,h}         extend existing integration
```

Already implemented: `adaq7769/*` (regs, low-level SPI+CRC, HAL, streaming),
`ad741x/*` (temp), `ds4424/*` (IDAC), `board/daq_board`.

---

## 11. Calibration

- Per-range gain/offset (ADAQ offset/gain registers + an NVS cal table).
- Shunt value trim (51 Ω / 2 Ω / 50 mΩ actuals), AD8411A offset.
- Voltage path: sense divider + Kelvin offset.
- Source path: DS4424 voltage and current-limit curves (reuse the ESP32
  DS4424 calibration approach).

---

## 12. Market-parity feature matrix

| Feature | PPK2 | Otii | Joulescope | This design |
|---------|:----:|:----:|:----------:|:-----------:|
| Seamless autorange | ✓ | ✓ | ✓ | ✓ (3 HW ranges, dual-ADC gap-fill) |
| Source / SMU | ✓ | ✓ | – | ✓ (0–20 V, 2.5 A) |
| Energy / charge | ✓ | ✓ | ✓ | ✓ mWh / J / mAh / C |
| Statistics (min/max/mean/RMS/std) | ✓ | ✓ | ✓ | ✓ |
| Multi-resolution zoom | ✓ | ✓ | ✓ | ✓ |
| FFT / spectrum | – | – | ✓ | ✓ (continuous rolling) |
| Digital event markers | ✓ (8) | ✓ | ✓ | ✓ (via S3) |
| Triggers + windowed markers | ✓ | ✓ | ✓ | ✓ |
| Continuous live streaming | ✓ | ✓ | ✓ | ✓ |
| Export / logging | ✓ | ✓ | ✓ | planned |

---

## 13. Phased implementation plan

1. Range manager + per-range calibration (read range GPIOs, tag samples).
2. Current fusion (seamless `i[n]`), validated against bench references.
3. Power + energy/charge accumulators + statistics.
4. USB HS vendor-bulk streaming + PC handshake (continuous live).
5. Multi-resolution decimation + continuous Welch FFT.
6. P4 ↔ S3 sync epoch + digital-marker correlation.
7. Source / SMU control integration (DS4424 0–20 V / 2.5 A).
8. Triggers, logging / export.

---

## 14. Resolved design parameters

- Current range nA–3A, 3 HW ranges (51 / 2 / 0.05 Ω), analog autorange with P4
  override + readback.
- Rates: current 250 kSPS, voltage 50 kSPS (both configurable).
- On-device DSP (Otii/PPK2 model); results streamed live.
- Source 0–20 V, 2.5 A via DS4424 trim.
- Continuous live streaming; continuous rolling FFT.
- 32 MB PSRAM available.

## 15. Open items for next iteration

- Exact FFT lengths / window set to expose to the PC (e.g. 1024 / 4096, Hann /
  Blackman-Harris).
- Export file format(s) (CSV and/or a Joulescope-JLS-like container).
- Trigger taxonomy (level / edge / event / external).
- Final GPIO pin map (still placeholders in `include/config.h`).
