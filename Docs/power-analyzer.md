# Power analyzer - architecture

Why the Power Profiler Pro HAT is built the way it is: the ranging scheme, the
two-ADC fusion trick, where the timebase comes from, and what gets calibrated.

For build instructions, module layout and the USB record formats, see
[Firmware/DAQ_HAT/README.md](../Firmware/DAQ_HAT/README.md). For pin- and
net-level facts, see [daq-hat-hardware.md](daq-hat-hardware.md).

## Topology

```
        ┌──────────────────────── Power Profiler Pro HAT ────────────────────┐
        │                                                                     │
 PC ◀──USB-HS bulk (measurement stream)──▶  ESP32-P4  ──DDP──▶  ESP32-C6      │
        │                                  (acquire+DSP)      (display/wireless)
        └───────────────────────────────────────┼─────────────────────────────┘
                                                │ HAT UART + SYNC line
 PC ◀──USB CDC (BBP control)──▶  ESP32-S3 mainboard  ── HV-tolerant world IO,
                                 (BBP host, IO owner)   digital event markers
```

The split matters:

- **Control plane** - BBP arrives at the ESP32-S3; the DAQ subset is forwarded
  to the P4 over the HAT UART.
- **Data plane** - the P4 streams high-rate measurement data out of its *own*
  USB-HS port, straight to the PC. The S3 is not in this path, so stream
  throughput is independent of control traffic.
- **World IO and markers** - the S3 owns the HV-tolerant IO and the digital
  event-marker channels. Markers correlate to P4 sample indices through a shared
  sync epoch.

The Logic Analyzer HAT is a *different* HAT and is never present at the same
time.

## Ranging

A series shunt ladder - **51 Ω + 2 Ω + 50 mΩ**. As current rises, an analog
feedback loop closes bypass switches that short the larger shunts. Each sense
node is read through an AD8411A current-sense amplifier (gain 50 V/V, −2…+70 V
common mode).

| Range | Active shunt | ADC path | Span | Role |
|---|---|---|---|---|
| HI | 51 Ω | FINE (muxed) | nA – ~2 mA | precision low current |
| MID | 2 Ω (51 Ω bypassed) | FINE (muxed) | ~2 – 50 mA | precision mid |
| LO | 50 mΩ | COARSE (dedicated) | ~50 mA – 3 A | high current and gap fill |

With the AD8411A's ×50 gain:

- 51 Ω → 2550 V/A → 5 V at ~2 mA
- 2 Ω → 100 V/A → 5 V at 50 mA
- 50 mΩ → 2.5 V/A → 7.5 V at 3 A

Three ADAQ7769-1 converters, assigned deliberately:

- **FINE** - multiplexed across the 51 Ω and 2 Ω nodes. Supplies the two
  precision sub-ranges.
- **COARSE** - permanently on the 50 mΩ node. Always valid, which is what makes
  it usable as a continuity reference.
- **VOLTAGE** - differential, fed by 4-wire Kelvin sense lines at the DUT
  terminals, giving true `V_dut` rather than a supply-side approximation.

**Autorange authority is split.** The analog loop does the fast switching; the
P4 only *observes* the active range by reading bypass-switch state through
1 kΩ-tapped GPIOs, and can force an override. A per-sample range tag travels
with the data so the DSP applies the right shunt value and calibration
coefficients.

## Fusion - why two ADCs

```
 FINE (51Ω|2Ω) ─┐
 COARSE (50mΩ) ─┼─▶ fusion + hysteresis + cross-fade ─▶ i[n] (seamless)
 range state ───┘
```

A single auto-ranging ADC has a hole in its output every time it switches range:
the Σ-Δ filter has to re-settle. COARSE is always valid, so it covers exactly
that window.

- Use FINE whenever its range is settled and unsaturated.
- Switch to COARSE during and just after a transition, until FINE re-settles - 
  the settling time per ODR and filter is known from the ADAQ7769-1 datasheet
  tables.
- Hysteresis on the range boundaries prevents chatter; a short cross-fade across
  the transition keeps the waveform glitch-free.

FINE and COARSE share one MCLK and one SYNC line, so their sample instants align
deterministically. **COARSE and VOLTAGE share SPI bus B and that same SYNC
line** - they cannot be phase-staggered, so any acquisition mode that starves one
starves the other.

## Timebase and power

Current and voltage run at independent, configurable rates, both SYNC-aligned.
Voltage - a slowly-moving supply rail - is held and linearly interpolated up to
the current rate, then `p[n] = v[n] · i[n]` using the Kelvin-sensed `V_dut`.

Marker correlation across the two MCUs works through a shared epoch:

1. A pre-acquisition handshake plus a hardware SYNC pulse zeroes both timebases.
2. The S3 timestamps its digital-marker events in that epoch, which maps to
   exact P4 sample indices.
3. A re-sync after the run bounds any clock drift across the capture.

## Bandwidth budget

Two channels at 250 kSPS × 4 B ≈ **2 MB/s**. USB High-Speed gives roughly
40 MB/s, so there is ample headroom - including for full-rate raw bursts. This
is why the DSP results, not the raw samples, are the normal payload: it keeps
the PC-side application simple without the wire being the constraint.

## Calibration

| Path | What is trimmed |
|---|---|
| Current, per range | ADAQ offset and gain registers plus an NVS calibration table |
| Shunts | Actual 51 Ω / 2 Ω / 50 mΩ values, and AD8411A offset |
| Voltage | Sense divider ratio and Kelvin offset |
| Source | DS4424 voltage and current-limit curves - same approach as the ESP32 DS4424 calibration |

## Source / SMU

A programmable 0–20 V supply trimmed by the DS4424 IDAC, with a 2.5 A current
limit that is also trimmable downward.

Two modes, one measurement chain:

- **Ammeter** - an external supply powers the DUT; the instrument measures
  inline.
- **Source-measure** - the instrument powers the DUT.

Set points and limits belong to the control plane; the acquisition path is
identical either way.

## Where it sits

| Feature | PPK2 | Otii | Joulescope | Power Profiler Pro |
|---|:---:|:---:|:---:|:---:|
| Seamless autorange | ✓ | ✓ | ✓ | ✓ (3 HW ranges, dual-ADC gap fill) |
| Source / SMU | ✓ | ✓ | – | ✓ (0–20 V, 2.5 A) |
| Energy and charge | ✓ | ✓ | ✓ | ✓ (mWh / J / mAh / C) |
| Statistics | ✓ | ✓ | ✓ | ✓ |
| Multi-resolution zoom | ✓ | ✓ | ✓ | ✓ |
| FFT / spectrum | – | – | ✓ | ✓ (continuous Welch) |
| Digital event markers | ✓ (8) | ✓ | ✓ | ✓ (via the S3) |
| Live streaming | ✓ | ✓ | ✓ | ✓ (USB-HS) |
| Network and OTA | – | – | – | ✓ (via the S3) |
| Export / logging | ✓ | ✓ | ✓ | not yet |

## Open

- Which FFT lengths and window functions to expose to the host.
- Export format - CSV, and possibly a JLS-like container.
- Trigger taxonomy: level, edge, event, external.
- The ×32 (256 kSPS) settings measure roughly 300× worse in noise *density* than
  the best settings. Unexplained; see
  [noise-characterisation.md](noise-characterisation.md).
