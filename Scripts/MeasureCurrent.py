#!/usr/bin/env python3
"""
MeasureCurrent.py — Live current measurement via AD8411A + BugBuster.

Hardware: AD8411A current-sense amplifier (gain=50) connected to a BugBuster
IO_Block (configured for 5V supply). DUT powered from a different voltage
domain (other VADJ or HAT rail).

Two modes
─────────
  DSP  (default) — 9.6 kSPS on-device pipeline: Hann-windowed 256-pt FFT,
                    spike detection, running stats. ~37 windows/sec.
                    Shows mean/max envelope, spike markers (▼), live FFT.

  Poll (legacy)  — 20 Hz single-sample polling. No sub-50 ms spike visibility.

Run: python Scripts/MeasureCurrent.py
"""

import os
import sys
import glob
import time
import threading
import collections
import re

import matplotlib
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import matplotlib.gridspec as gridspec

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from python.bugbuster import connect_usb, connect_http
from python.bugbuster.hal import PortMode, DEFAULT_ROUTING
from python.bugbuster.constants import (
    AdcMux, AdcRange, AdcRate, PowerControl,
)
from python.bugbuster.discovery import discover_mdns, DiscoveredDevice

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

AD8411A_GAIN   = 50.0
WINDOW_SECONDS = 180      # rolling display window (both modes)

# Poll mode
POLL_RATE_HZ   = 20
POLL_DEQUE     = POLL_RATE_HZ * WINDOW_SECONDS   # 3600 samples

# DSP mode
DSP_SAMPLE_RATE = 9600
DSP_WIN_SAMPLES = 256
DSP_WIN_RATE    = DSP_SAMPLE_RATE / DSP_WIN_SAMPLES   # ~37.5 windows/sec
DSP_WIN_DEQUE   = int(DSP_WIN_RATE * WINDOW_SECONDS)  # ~6750 windows
DSP_SPIKE_DEQUE = 4000     # max retained spike records
DSP_N_FFT_PEAKS = 8

IO_BLOCK_LABELS = {1: "A", 2: "B", 3: "C", 4: "D"}
ADC_CH_LABELS   = {0: "A", 1: "B", 2: "C", 3: "D"}


def _build_io_block_map() -> dict:
    result = {}
    for block in range(1, 5):
        analog_io = block * 3
        rt = DEFAULT_ROUTING[analog_io]
        result[block] = {
            "label":        IO_BLOCK_LABELS[block],
            "analog_io":    analog_io,
            "adc_ch":       rt.channel,
            "adc_label":    ADC_CH_LABELS[rt.channel],
            "efuse":        rt.efuse,
            "supply":       rt.supply,
            "idac_ch":      rt.supply_idac,
            "vadj_domain":  rt.block,
            "selftest_rail": 0 if rt.block == 1 else 1,
        }
    return result


IO_BLOCK_MAP = _build_io_block_map()

RSENSE_GUIDANCE = """
  Rsense selection guide for µA–mA measurements (AD8411A gain=50):
  ─────────────────────────────────────────────────────────────────
  Current range     Suggested Rsense   Resolution (~1 µV ADC floor)
  ─────────────────────────────────────────────────────────────────
  < 1 µA            400 Ω – 1 kΩ      ~20 nA          (sweet spot 1 kΩ)
  1 µA – 100 µA     10 Ω  – 400 Ω     ~2 nA – 200 nA  (sweet spot 100 Ω)
  100 µA – 10 mA    100 mΩ – 10 Ω     ~0.2 µA – 2 µA  (sweet spot 1 Ω)
  10 mA – 100 mA    10 mΩ – 100 mΩ    ~2 µA – 20 µA
  > 100 mA          ≤ 10 mΩ           ~20 µA

  ⚠  A single Rsense cannot cover nA AND 1A simultaneously.
     For nA floor + mA peaks → 100 Ω
     For mA floor + 1A peaks → 10 mΩ
"""

# ---------------------------------------------------------------------------
# Terminal helpers
# ---------------------------------------------------------------------------

def _prompt(msg: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    try:
        ans = input(f"  {msg}{suffix}: ").strip()
    except EOFError:
        raise KeyboardInterrupt
    return ans if ans else default


def _prompt_int(msg: str, valid: list, default: int = None) -> int:
    while True:
        raw = _prompt(msg, str(default) if default is not None else "")
        try:
            val = int(raw)
            if val in valid:
                return val
            print(f"    Must be one of {valid}")
        except ValueError:
            print("    Enter a whole number.")


def _prompt_float(msg: str, min_val: float = None, max_val: float = None,
                  default: float = None) -> float:
    while True:
        raw = _prompt(msg, str(default) if default is not None else "")
        try:
            val = float(raw)
            if min_val is not None and val < min_val:
                print(f"    Must be ≥ {min_val}")
                continue
            if max_val is not None and val > max_val:
                print(f"    Must be ≤ {max_val}")
                continue
            return val
        except ValueError:
            print("    Enter a number.")


# ---------------------------------------------------------------------------
# Autorange helpers
# ---------------------------------------------------------------------------

_RANGE_RES_NV = {
    AdcRange.V_0_312MV:    18.6,
    AdcRange.V_0_625MV:    37.3,
    AdcRange.V_NEG2_5_2_5: 298.0,
    AdcRange.V_0_12:       715.0,
}


def select_adc_range(vout_max: float) -> tuple:
    if vout_max <= 0.312:
        return AdcRange.V_0_312MV,    "V_0_312MV  (0–312 mV)"
    if vout_max <= 0.625:
        return AdcRange.V_0_625MV,    "V_0_625MV  (0–625 mV)"
    if vout_max <= 2.5:
        return AdcRange.V_NEG2_5_2_5, "V_NEG2_5_2_5 (±2.5 V, positive half)"
    return AdcRange.V_0_12,           "V_0_12     (0–12 V)"


def autorange(amps: float) -> tuple:
    abs_v = abs(amps)
    if abs_v < 1e-6:
        return amps * 1e9, "nA"
    if abs_v < 1e-3:
        return amps * 1e6, "µA"
    if abs_v < 1.0:
        return amps * 1e3, "mA"
    return amps, "A"


def _scale_for(unit: str) -> float:
    return {"nA": 1e9, "µA": 1e6, "mA": 1e3, "A": 1.0}[unit]


def _energy_autorange(wh: float) -> tuple:
    abs_v = abs(wh)
    if abs_v < 1e-6:
        return wh * 1e9, "nWh"
    if abs_v < 1e-3:
        return wh * 1e6, "µWh"
    if abs_v < 1.0:
        return wh * 1e3, "mWh"
    return wh, "Wh"


# ---------------------------------------------------------------------------
# Log filename
# ---------------------------------------------------------------------------

def next_log_filename(log_dir: str) -> str:
    os.makedirs(log_dir, exist_ok=True)
    existing = glob.glob(os.path.join(log_dir, "Log*.csv"))
    indices = []
    for f in existing:
        m = re.search(r"Log(\d+)\.csv$", os.path.basename(f))
        if m:
            indices.append(int(m.group(1)))
    return os.path.join(log_dir, f"Log{max(indices, default=0) + 1:03d}.csv")


# ---------------------------------------------------------------------------
# Phase 1 — Connection
# ---------------------------------------------------------------------------

def _discover_wifi() -> str:
    print("  Scanning for BugBuster boards via mDNS…", end="", flush=True)
    try:
        devices = discover_mdns(timeout=2.0)
    except ImportError:
        print(" (zeroconf not installed, skipping)")
        return _prompt("BugBuster host (IP or hostname)", "192.168.4.1")

    if not devices:
        print(" none found.")
        return _prompt("BugBuster host (IP or hostname)", "192.168.4.1")

    if len(devices) == 1:
        d = devices[0]
        label = f"{d.instance or d.hostname}  {d.ip}"
        if d.firmware:
            label += f"  fw={d.firmware}"
        print(f" found: {label}")
        return d.ip

    print(f" found {len(devices)}:")
    for i, d in enumerate(devices, 1):
        fw = f"  fw={d.firmware}" if d.firmware else ""
        print(f"    {i}) {d.instance or d.hostname:<24} {d.ip}{fw}")
    choice = _prompt_int("Select board", list(range(1, len(devices) + 1)), default=1)
    return devices[choice - 1].ip


def setup_connection():
    print("\n── BugBuster Connection ─────────────────────────────────────────")
    conn_type = _prompt("Connection type (wifi/usb)", "wifi").lower()

    if conn_type.startswith("u"):
        candidates = (glob.glob("/dev/tty.usbmodem*") +
                      glob.glob("/dev/ttyACM*") +
                      glob.glob("COM*"))
        default_port = candidates[0] if candidates else "/dev/ttyACM0"
        port = _prompt("Serial port", default_port)
        print(f"  Connecting to {port}…")
        bb = connect_usb(port)
        is_usb = True
    else:
        host  = _discover_wifi()
        token = _prompt("Admin token (leave blank if none)", "")
        print(f"  Connecting to {host}…")
        bb = connect_http(host, admin_token=token if token else None)
        is_usb = False

    print("  Connected.")
    return bb, is_usb


# ---------------------------------------------------------------------------
# Phase 2 — AD8411A IO_Block
# ---------------------------------------------------------------------------

def setup_ad8411a(bb, hal, block: int) -> dict:
    info      = IO_BLOCK_MAP[block]
    analog_io = info["analog_io"]
    adc_ch    = info["adc_ch"]

    print(
        f"\n  IO_Block {block} ({info['label']}) | IO{analog_io} "
        f"→ logical ADC ch{adc_ch} (CH {info['adc_label']}) | "
        f"VADJ{info['vadj_domain']} → 5.0 V"
    )

    hal._supply_v = 5.0
    hal.configure(analog_io, PortMode.ANALOG_IN)

    bb.set_adc_config(adc_ch, AdcMux.LF_TO_AGND, AdcRange.V_0_12, AdcRate.SPS_200_H)
    print(f"  AD8411A ready: 5.0 V supply, CH {info['adc_label']} at 200 SPS")
    return info


# ---------------------------------------------------------------------------
# Phase 3 — DUT power
# ---------------------------------------------------------------------------

def setup_dut_bb_ioblock(bb, block: int, voltage: float) -> None:
    info = IO_BLOCK_MAP[block]
    print(f"\n  DUT on IO_Block {block} | VADJ{info['vadj_domain']} → {voltage} V")
    bb.idac_set_voltage(info["idac_ch"], voltage)
    bb.power_set(info["supply"], on=True)
    time.sleep(0.5)
    bb.power_set(info["efuse"], on=True)
    print(f"  DUT IO_Block {block}: {voltage} V, EFUSE enabled")


def setup_dut_hat(bb, rail_id: int, voltage: float) -> None:
    rail_name = {1: "VADJ3", 2: "VADJ4"}.get(rail_id, f"rail {rail_id}")
    print(f"\n  DUT on HAT {rail_name} → {voltage} V")
    bb.hat_set_rail_voltage(rail_id, int(voltage * 1000))
    bb.hat_set_rail_enable(rail_id, True)
    print(f"  HAT {rail_name}: {voltage} V enabled")


# ---------------------------------------------------------------------------
# Zero-current calibration
# ---------------------------------------------------------------------------
# Supply measurement
# ---------------------------------------------------------------------------

def measure_ad8411a_supply(bb, ad8411a_info: dict) -> float:
    if ad8411a_info["adc_ch"] != 3:
        rail = ad8411a_info["selftest_rail"]
        vs = bb.selftest_measure_supply(rail)
        if vs > 0:
            print(f"  AD8411A supply (measured): {vs:.4f} V")
            return vs
    print("  AD8411A supply (nominal): 5.0000 V")
    return 5.0


# ---------------------------------------------------------------------------
# DSP measurement state
# ---------------------------------------------------------------------------

class DspMeasurement:
    """Thread-safe rolling state fed by AdcDspWindow callbacks."""

    def __init__(self, gain: float, rsense_ohm: float,
                 dut_v: float, sample_rate: int = 9600):
        self._gain   = gain
        self._rsense = rsense_ohm
        self._dut_v  = dut_v
        self._fs     = sample_rate
        self._lock      = threading.Lock()
        self._csv_file  = None

        # Per-window rolling deques
        self.win_times = collections.deque(maxlen=DSP_WIN_DEQUE)
        self.mean_I    = collections.deque(maxlen=DSP_WIN_DEQUE)
        self.max_I     = collections.deque(maxlen=DSP_WIN_DEQUE)
        self.min_I     = collections.deque(maxlen=DSP_WIN_DEQUE)
        self.rms_I     = collections.deque(maxlen=DSP_WIN_DEQUE)

        # Spike log: (wall_time_s, current_A)
        self.spikes = collections.deque(maxlen=DSP_SPIKE_DEQUE)

        # Latest FFT: list of (freq_hz, magnitude_A)
        self.fft_latest: list = []

        # Live stats
        self.live_mean_I     = 0.0
        self.live_max_I      = 0.0
        self.live_rms_I      = 0.0
        self.n_spikes_window = 0
        self.n_spikes_total  = 0

    def _v2i(self, v: float) -> float:
        return v / (self._gain * self._rsense)

    def on_window(self, win) -> None:
        """Called from DSP stream callback thread."""
        now          = time.time()
        win_dur      = DSP_WIN_SAMPLES / self._fs  # seconds

        mean_i = self._v2i(win.mean_v)
        max_i  = self._v2i(win.max_v)
        min_i  = self._v2i(win.min_v)
        # RMS: subtract offset in voltage then divide
        rms_i  = win.rms_v / (self._gain * self._rsense)

        # FFT peaks: voltage magnitude → current magnitude (no offset subtraction for magnitude)
        fft_scale = 1.0 / (self._gain * self._rsense)
        fft = [(b * self._fs / DSP_WIN_SAMPLES, mag * fft_scale)
               for b, mag in win.fft_peaks]

        # Spikes: absolute wall time estimated from window end - remaining time
        spike_entries = []
        for offset_us, v in win.spikes:
            spike_wall = now - win_dur + offset_us / 1e6
            spike_i    = self._v2i(v)
            spike_entries.append((spike_wall, spike_i))

        with self._lock:
            self.win_times.append(now)
            self.mean_I.append(mean_i)
            self.max_I.append(max_i)
            self.min_I.append(min_i)
            self.rms_I.append(rms_i)
            self.fft_latest = fft
            self.spikes.extend(spike_entries)
            self.live_mean_I     = mean_i
            self.live_max_I      = max_i
            self.live_rms_I      = rms_i
            self.n_spikes_window = len(win.spikes)
            self.n_spikes_total += len(win.spikes)

        if self._csv_file:
            p = mean_i * self._dut_v
            self._csv_file.write(
                f"{now:.3f},{mean_i:.9e},{max_i:.9e},{rms_i:.9e},"
                f"{len(win.spikes)},{p:.6e}\n"
            )

    def open_csv(self, path: str, supply_v: float, rsense_mohm: float,
                 block: int, dut_label: str) -> None:
        self._csv_file = open(path, "w", newline="", buffering=1)
        self._csv_file.write(
            f"# BugBuster DSP Current Measurement Log\n"
            f"# AD8411A supply: {supply_v:.4f} V\n"
            f"# Rsense: {rsense_mohm:.3f} mOhm  (gain={self._gain})\n"
            f"# AD8411A IO_Block: {block} | DUT: {dut_label}\n"
            f"# timestamp_unix,mean_A,max_A,rms_A,n_spikes,power_W\n"
        )

    def close_csv(self) -> None:
        if self._csv_file:
            self._csv_file.close()
            self._csv_file = None


# ---------------------------------------------------------------------------
# DSP UI  (3-panel: current envelope + spike markers / FFT / HUD)
# ---------------------------------------------------------------------------

_C_BG    = "#1a1a2e"
_C_PANEL = "#16213e"
_C_SPINE = "#0f3460"
_C_GRID  = "#0f3460"
_C_TEXT  = "#c0c0d0"
_C_MEAN  = "#00d4ff"   # mean current line
_C_ENV   = "#00d4ff"   # min/max fill
_C_SPIKE = "#ff4444"   # spike markers
_C_FFT   = "#00ff88"   # FFT bars
_C_TITLE = "#e0e0e0"


def _style_ax(ax):
    ax.set_facecolor(_C_PANEL)
    for sp in ax.spines.values():
        sp.set_color(_C_SPINE)
    ax.tick_params(colors=_C_TEXT, labelsize=8)


def run_ui_dsp(dsp: DspMeasurement, rsense_mohm: float,
               log_path: str, dut_supply_v: float) -> None:

    fig = plt.figure(figsize=(13, 9), facecolor=_C_BG)
    gs  = gridspec.GridSpec(
        3, 1, figure=fig,
        height_ratios=[5, 3, 1.5],
        hspace=0.38,
        left=0.09, right=0.95, top=0.95, bottom=0.05,
    )

    ax_cur = fig.add_subplot(gs[0])
    ax_fft = fig.add_subplot(gs[1])
    ax_hud = fig.add_subplot(gs[2])

    for ax in (ax_cur, ax_fft):
        _style_ax(ax)
        ax.grid(True, color=_C_GRID, linewidth=0.5, alpha=0.7)
    _style_ax(ax_hud)
    ax_hud.axis("off")

    # ── Current panel ────────────────────────────────────────────────
    log_name = os.path.basename(log_path)
    ax_cur.set_xlabel("Time (s ago)", fontsize=8, color=_C_TEXT)
    ax_cur.set_ylabel("Current", fontsize=8, color=_C_TEXT)
    ax_cur.set_title(
        f"DSP Current Monitor  ·  Rsense {rsense_mohm:.3f} mΩ  ·  {log_name}",
        color=_C_TITLE, fontsize=9, pad=5,
    )
    ax_cur.xaxis.label.set_color(_C_TEXT)
    ax_cur.yaxis.label.set_color(_C_TEXT)

    line_mean, = ax_cur.plot([], [], color=_C_MEAN, linewidth=1.0,
                             label="mean", zorder=3, antialiased=True)
    scat_spk   = ax_cur.scatter([], [], color=_C_SPIKE, s=30, zorder=5,
                                marker="v", label="spike", linewidths=0)
    ax_cur.legend(loc="upper left", fontsize=7, facecolor=_C_SPINE,
                  labelcolor=_C_TEXT, framealpha=0.8)

    # ── FFT panel ────────────────────────────────────────────────────
    ax_fft.set_xlabel("Frequency (Hz)", fontsize=8, color=_C_TEXT)
    ax_fft.set_ylabel("Magnitude", fontsize=8, color=_C_TEXT)
    ax_fft.set_title("FFT Spectrum — latest 256-sample window",
                     color=_C_TITLE, fontsize=8, pad=4)
    ax_fft.xaxis.label.set_color(_C_TEXT)
    ax_fft.yaxis.label.set_color(_C_TEXT)

    # ── HUD ─────────────────────────────────────────────────────────
    hud = ax_hud.text(
        0.5, 0.5, "Waiting for DSP windows…",
        transform=ax_hud.transAxes,
        ha="center", va="center", fontsize=11,
        color="#00ff88", fontfamily="monospace",
        bbox=dict(boxstyle="round,pad=0.5", facecolor=_C_SPINE,
                  edgecolor=_C_MEAN, alpha=0.9),
    )

    # Mutable state for the animation closure
    _fill_handle = [None]
    _cur_unit    = [None]
    _last_n_spk  = [0]

    def _update(_frame):
        with dsp._lock:
            t_snap   = list(dsp.win_times)
            mean_snap = list(dsp.mean_I)
            max_snap  = list(dsp.max_I)
            min_snap  = list(dsp.min_I)
            rms_snap  = list(dsp.rms_I)
            spk_snap  = list(dsp.spikes)
            fft_snap  = list(dsp.fft_latest)
            lm = dsp.live_mean_I
            lx = dsp.live_max_I
            lr = dsp.live_rms_I
            n_spk_win = dsp.n_spikes_window
            n_spk_tot = dsp.n_spikes_total

        if not t_snap:
            return

        now = t_snap[-1]

        # ── Unit selection (based on max current seen) ────────────────
        peak = max((abs(v) for v in max_snap), default=abs(lx))
        _, unit = autorange(peak if peak > 0 else 1e-12)
        sc = _scale_for(unit)

        if unit != _cur_unit[0]:
            ax_cur.set_ylabel(f"Current ({unit})", fontsize=8, color=_C_TEXT)
            _cur_unit[0] = unit

        x  = [t - now for t in t_snap]
        ym = [v * sc for v in mean_snap]
        yx = [v * sc for v in max_snap]
        yn = [v * sc for v in min_snap]

        line_mean.set_data(x, ym)

        # Envelope fill (replace each frame)
        if _fill_handle[0] is not None:
            _fill_handle[0].remove()
        _fill_handle[0] = ax_cur.fill_between(
            x, yn, yx, alpha=0.15, color=_C_ENV, zorder=2,
        )

        # Axis limits from data
        if ym:
            ylo = min(yn) - (max(yx) - min(yn)) * 0.08
            yhi = max(yx) + (max(yx) - min(yn)) * 0.08
            margin = (yhi - ylo) * 0.05 or 0.01
            ax_cur.set_ylim(ylo - margin, yhi + margin)
        ax_cur.set_xlim((x[0] if x else -WINDOW_SECONDS), 0.5)

        # Spike scatter — only within the rolling window
        recent = [(t, v) for t, v in spk_snap if t >= now - WINDOW_SECONDS]
        if recent:
            sx = [t - now for t, _ in recent]
            sy = [v * sc  for _, v in recent]
            scat_spk.set_offsets(list(zip(sx, sy)))
            scat_spk.set_visible(True)
        else:
            scat_spk.set_visible(False)

        # Title flash when new spikes arrive
        new_spikes = n_spk_tot != _last_n_spk[0] and n_spk_win > 0
        _last_n_spk[0] = n_spk_tot
        ax_cur.set_title(
            f"DSP Current Monitor  ·  Rsense {rsense_mohm:.3f} mΩ  ·  {log_name}"
            + ("  ⚡" if new_spikes else ""),
            color="#ff6666" if new_spikes else _C_TITLE,
            fontsize=9, pad=5,
        )

        # ── FFT panel ─────────────────────────────────────────────────
        ax_fft.cla()
        _style_ax(ax_fft)
        ax_fft.grid(True, axis="y", color=_C_GRID, linewidth=0.5, alpha=0.6)
        ax_fft.set_title("FFT Spectrum — latest 256-sample window",
                         color=_C_TITLE, fontsize=8, pad=4)

        if fft_snap:
            freqs = [f for f, _ in fft_snap]
            mags  = [m for _, m in fft_snap]
            fp    = max(mags, default=1e-12)
            _, fu = autorange(fp)
            fsc   = _scale_for(fu)
            bw    = dsp._fs / DSP_WIN_SAMPLES * 0.7   # bar width ~70% of bin spacing

            bars = ax_fft.bar(
                freqs, [m * fsc for m in mags], width=bw,
                color=_C_FFT, alpha=0.85, zorder=3,
            )
            # Annotate top bars with frequency
            for bar, (freq, mag) in zip(bars, zip(freqs, mags)):
                bh = bar.get_height()
                if bh > 0:
                    ax_fft.text(
                        bar.get_x() + bar.get_width() / 2, bh * 1.02,
                        f"{freq:.0f}",
                        ha="center", va="bottom", fontsize=6.5,
                        color=_C_TEXT,
                    )

            ax_fft.set_xlabel("Frequency (Hz)", fontsize=8, color=_C_TEXT)
            ax_fft.set_ylabel(f"Magnitude ({fu})", fontsize=8, color=_C_TEXT)
            ax_fft.xaxis.label.set_color(_C_TEXT)
            ax_fft.yaxis.label.set_color(_C_TEXT)
            ax_fft.tick_params(colors=_C_TEXT, labelsize=8)
        else:
            ax_fft.text(0.5, 0.5, "No FFT data yet",
                        transform=ax_fft.transAxes,
                        ha="center", va="center",
                        color=_C_TEXT, fontsize=9)

        # ── HUD ───────────────────────────────────────────────────────
        lm_v, lm_u = autorange(lm)
        lr_v, lr_u = autorange(lr)
        lx_v, lx_u = autorange(lx)
        pw_mw      = lm * dut_supply_v * 1e3

        if len(mean_snap) > 1 and len(t_snap) > 1:
            dt = (t_snap[-1] - t_snap[0]) / max(len(t_snap) - 1, 1)
            e_wh = sum(v * dut_supply_v for v in mean_snap) * dt / 3600.0
        else:
            e_wh = 0.0
        e_v, e_u = _energy_autorange(e_wh)

        spk_note = f"  ⚡ {n_spk_win} in window" if n_spk_win else ""
        hud.set_text(
            f"Live mean: {lm_v:+.3f} {lm_u:<3}   "
            f"RMS: {lr_v:.3f} {lr_u:<3}   "
            f"Peak: {lx_v:.3f} {lx_u:<3}   "
            f"Power: {pw_mw:.4f} mW\n"
            f"Energy: {e_v:.4f} {e_u}   "
            f"Windows: {len(t_snap)}   "
            f"Spikes: {n_spk_tot} total{spk_note}"
        )

    animation.FuncAnimation(
        fig, _update, interval=200, blit=False, cache_frame_data=False,
    )
    plt.show()


# ---------------------------------------------------------------------------
# Poll measurement state (legacy)
# ---------------------------------------------------------------------------

class Measurement:
    def __init__(self):
        self.timestamps = collections.deque(maxlen=POLL_DEQUE)
        self.currents   = collections.deque(maxlen=POLL_DEQUE)
        self.powers     = collections.deque(maxlen=POLL_DEQUE)
        self.live_amps  = 0.0
        self.live_watts = 0.0
        self.running    = False
        self._csv_file  = None
        self._lock      = threading.Lock()

    def open_csv(self, path: str, supply_v: float, rsense_mohm: float,
                 ad8411a_block: int, dut_label: str) -> None:
        self._csv_file = open(path, "w", newline="", buffering=1)
        self._csv_file.write(
            f"# BugBuster Current Measurement Log\n"
            f"# AD8411A supply: {supply_v:.4f} V\n"
            f"# Rsense: {rsense_mohm:.3f} mOhm  (gain={AD8411A_GAIN})\n"
            f"# AD8411A IO_Block: {ad8411a_block} | DUT: {dut_label}\n"
            f"# timestamp_unix,current_A,power_W\n"
        )

    def close_csv(self) -> None:
        if self._csv_file:
            self._csv_file.close()
            self._csv_file = None

    def poll(self, bb, adc_ch: int, rsense_ohm: float, dut_supply_v: float) -> None:
        self.running = True
        interval = 1.0 / POLL_RATE_HZ
        while self.running:
            t0 = time.monotonic()
            try:
                vout   = bb.get_adc_value(adc_ch).value
                i_amps = vout / (AD8411A_GAIN * rsense_ohm)
                p_watt = i_amps * dut_supply_v
                ts     = time.time()
                with self._lock:
                    self.timestamps.append(ts)
                    self.currents.append(i_amps)
                    self.powers.append(p_watt)
                    self.live_amps  = i_amps
                    self.live_watts = p_watt
                if self._csv_file:
                    self._csv_file.write(f"{ts:.3f},{i_amps:.9e},{p_watt:.6e}\n")
            except Exception as exc:
                sys.stderr.write(f"\r[poll error: {exc}]")
            elapsed = time.monotonic() - t0
            time.sleep(max(0.0, interval - elapsed))


# ---------------------------------------------------------------------------
# Poll UI (legacy single-panel)
# ---------------------------------------------------------------------------

def run_ui_poll(meas: Measurement, rsense_mohm: float,
                log_path: str, dut_supply_v: float) -> None:

    fig = plt.figure(figsize=(12, 7), facecolor=_C_BG)
    ax_graph = fig.add_axes([0.09, 0.38, 0.87, 0.54])
    ax_hud   = fig.add_axes([0.09, 0.05, 0.87, 0.27])

    for ax in (ax_graph, ax_hud):
        ax.set_facecolor(_C_PANEL)
        for spine in ax.spines.values():
            spine.set_color(_C_SPINE)

    ax_graph.tick_params(colors=_C_TEXT, labelsize=8)
    ax_graph.yaxis.label.set_color(_C_TEXT)
    ax_graph.xaxis.label.set_color(_C_TEXT)
    ax_graph.set_xlabel("Time (s ago)", fontsize=8)
    ax_graph.set_ylabel("Current", fontsize=8)
    ax_graph.set_title(
        f"Current Monitor  ·  Rsense {rsense_mohm:.3f} mΩ  ·  {os.path.basename(log_path)}",
        color=_C_TITLE, fontsize=9, pad=6,
    )
    ax_graph.grid(True, color=_C_GRID, linewidth=0.5, alpha=0.7)

    (line,) = ax_graph.plot([], [], color=_C_MEAN, linewidth=1.0, antialiased=True)

    ax_hud.axis("off")
    hud = ax_hud.text(
        0.5, 0.5, "Waiting for samples…",
        transform=ax_hud.transAxes,
        ha="center", va="center", fontsize=15,
        color="#00ff88", fontfamily="monospace",
        bbox=dict(boxstyle="round,pad=0.6", facecolor=_C_SPINE,
                  edgecolor=_C_MEAN, alpha=0.85),
    )

    _cur_unit = [None]

    def _update(_frame):
        with meas._lock:
            ts_snap = list(meas.timestamps)
            i_snap  = list(meas.currents)
            p_snap  = list(meas.powers)
            live_a  = meas.live_amps
            live_w  = meas.live_watts

        if not ts_snap:
            return line, hud

        now    = ts_snap[-1]
        x_data = [t - now for t in ts_snap]

        peak = max(abs(v) for v in i_snap) if i_snap else abs(live_a)
        _, unit = autorange(peak if peak > 0 else 1e-12)
        if unit != _cur_unit[0]:
            ax_graph.set_ylabel(f"Current ({unit})", fontsize=8, color=_C_TEXT)
            _cur_unit[0] = unit

        sc = _scale_for(unit)
        line.set_data(x_data, [v * sc for v in i_snap])
        ax_graph.relim()
        ax_graph.autoscale_view()

        live_val, live_unit = autorange(live_a)
        live_mw = live_w * 1e3
        if len(p_snap) > 1 and len(ts_snap) > 1:
            dt_avg    = (ts_snap[-1] - ts_snap[0]) / (len(ts_snap) - 1)
            energy_wh = sum(p_snap) * dt_avg / 3600.0
        else:
            energy_wh = 0.0
        e_val, e_unit = _energy_autorange(energy_wh)

        hud.set_text(
            f"Live: {live_val:+.3f} {live_unit:<3}   │   Power: {live_mw:.4f} mW\n"
            f"3-min window energy: {e_val:.4f} {e_unit}   │   "
            f"DUT supply: {dut_supply_v:.3f} V   │   "
            f"Samples: {len(ts_snap)} ({len(ts_snap) / POLL_RATE_HZ:.0f} s)"
        )
        return line, hud

    animation.FuncAnimation(
        fig, _update, interval=200, blit=False, cache_frame_data=False,
    )
    plt.show()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    print("╔══════════════════════════════════════════════════════════════╗")
    print("║       BugBuster — AD8411A Current Measurement               ║")
    print("╚══════════════════════════════════════════════════════════════╝")

    log_path = None
    bb = None
    hal = None

    try:
        # ── Phase 1: Connection ──────────────────────────────────────
        bb, is_usb = setup_connection()

        hat_info    = bb.hat_detect()
        hat_present = bool(hat_info.get("detected", False))
        if hat_present:
            print(f"  HAT detected (type={hat_info.get('type', '?')}, "
                  f"detect_v={hat_info.get('detect_voltage', 0):.2f}V)")
        else:
            print("  No HAT detected.")

        hal = bb.hal
        hal.begin()

        # ── Phase 2: AD8411A IO_Block ────────────────────────────────
        print("\n── AD8411A Setup ────────────────────────────────────────────")
        ad8411a_block = _prompt_int(
            "IO_Block with AD8411A board (1–4)", [1, 2, 3, 4]
        )
        ad8411a_info = setup_ad8411a(bb, hal, ad8411a_block)
        adc_ch       = ad8411a_info["adc_ch"]
        ad8411a_vadj = ad8411a_info["vadj_domain"]

        # ── Phase 3: DUT power ───────────────────────────────────────
        print("\n── DUT Power Setup ──────────────────────────────────────────")
        print(f"  AD8411A is on VADJ{ad8411a_vadj} — choose a different domain.")

        options = []
        if ad8411a_vadj == 1:
            options += [("BB IO_Block 3 (VADJ2, 3–15 V)", "bb", 3),
                        ("BB IO_Block 4 (VADJ2, 3–15 V)", "bb", 4)]
        else:
            options += [("BB IO_Block 1 (VADJ1, 3–15 V)", "bb", 1),
                        ("BB IO_Block 2 (VADJ1, 3–15 V)", "bb", 2)]
        if hat_present and is_usb:
            options += [("HAT VADJ3 (1.8–36 V)", "hat", 1),
                        ("HAT VADJ4 (1.8–36 V)", "hat", 2)]
        elif hat_present:
            print("  (HAT rails require USB — not offered over WiFi)")

        print("  Available DUT power sources:")
        for i, (label, _, _) in enumerate(options, 1):
            print(f"    {i}) {label}")

        dut_choice = _prompt_int(
            f"Select DUT source (1–{len(options)})", list(range(1, len(options) + 1))
        )
        dut_label_str, dut_type, dut_id = options[dut_choice - 1]
        dut_voltage = _prompt_float(
            "DUT supply voltage (V)", min_val=1.8, max_val=36.0, default=12.5
        )

        if dut_type == "bb":
            setup_dut_bb_ioblock(bb, dut_id, dut_voltage)
            dut_label = f"BB IO_Block {dut_id} ({dut_voltage:.2f} V)"
        else:
            setup_dut_hat(bb, dut_id, dut_voltage)
            dut_label = f"HAT VADJ{2 + dut_id} ({dut_voltage:.2f} V)"

        dut_supply_v = dut_voltage

        # ── Phase 4: Rsense ──────────────────────────────────────────
        print("\n── Sense Resistor ───────────────────────────────────────────")
        print(RSENSE_GUIDANCE)
        rsense_mohm = _prompt_float("Rsense in mΩ (milliohms)", min_val=0.001)
        rsense_ohm  = rsense_mohm / 1000.0
        abs_max_i   = 5.0 / (AD8411A_GAIN * rsense_ohm)
        print(f"  Rsense = {rsense_mohm:.3f} mΩ → absolute max I ≈ {abs_max_i:.3g} A")

        # ── ADC range ────────────────────────────────────────────────
        print("\n── ADC Range Optimisation ───────────────────────────────────")
        print("  Unidirectional mode: Vout = 0 V at zero current.")
        max_current_a = _prompt_float(
            "Max expected current (A)", min_val=1e-9, max_val=abs_max_i,
            default=round(abs_max_i / 10, 9),
        )
        vout_max = AD8411A_GAIN * rsense_ohm * max_current_a
        adc_range, range_label = select_adc_range(vout_max)
        res_nv = _RANGE_RES_NV[adc_range]
        print(f"  Vout_max = {vout_max*1000:.2f} mV → {range_label}  (~{res_nv:.0f} nV/LSB)")

        # ── Mode selection ───────────────────────────────────────────
        print("\n── Measurement Mode ─────────────────────────────────────────")
        print("  DSP  — 9.6 kSPS on-device FFT + spike detection (~37 win/sec)")
        print("  Poll — 20 Hz single-sample (legacy, no transient visibility)")
        use_dsp = _prompt("Mode (dsp/poll)", "dsp").lower().startswith("d")

        # ── DSP path ─────────────────────────────────────────────────
        if use_dsp:
            print("\n── Spike Detection Threshold ────────────────────────────────")
            mi_v, mi_u = autorange(max_current_a)
            print(f"  Max expected current: {mi_v:.3g} {mi_u}")
            print("  Spikes are flagged when |I - window_mean| exceeds this value.")
            spike_thresh_a = _prompt_float(
                "Spike threshold (A)", min_val=0.0, max_val=abs_max_i,
                default=round(max_current_a * 0.1, 12),
            )
            v_spike_thresh = spike_thresh_a * AD8411A_GAIN * rsense_ohm
            st_v, st_u = autorange(spike_thresh_a)
            print(f"  Threshold: {st_v:.3g} {st_u}  →  {v_spike_thresh*1000:.3f} mV ADC")

            # Configure ADC: final range + 9.6 kSPS for DSP
            bb.set_adc_config(adc_ch, AdcMux.LF_TO_AGND, adc_range, AdcRate.SPS_200_H)

            # ── Log file ──────────────────────────────────────────────
            print("\n── Log File ─────────────────────────────────────────────────")
            log_dir   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
            log_path  = _prompt("Log filename", next_log_filename(log_dir))
            if not os.path.isabs(log_path):
                log_path = os.path.join(log_dir, log_path)
            if not log_path.endswith(".csv"):
                log_path += ".csv"
            print(f"  Writing to: {log_path}")

            vs_actual = measure_ad8411a_supply(bb, ad8411a_info)

            print("\n── Starting DSP Stream ──────────────────────────────────────")
            print("  Enable your DUT load now.")
            print("  Close the plot window or press Ctrl+C to stop.\n")

            dsp = DspMeasurement(AD8411A_GAIN, rsense_ohm, dut_supply_v,
                                 sample_rate=DSP_SAMPLE_RATE)
            dsp.open_csv(log_path, vs_actual, rsense_mohm, ad8411a_block, dut_label)

            try:
                bb.start_adc_dsp_stream(
                    channel        = adc_ch,
                    rate           = AdcRate.SPS_9600,
                    window_samples = DSP_WIN_SAMPLES,
                    spike_threshold= v_spike_thresh,
                    n_fft_peaks    = DSP_N_FFT_PEAKS,
                    callback       = dsp.on_window,
                )
                try:
                    run_ui_dsp(dsp, rsense_mohm, log_path, dut_supply_v)
                except KeyboardInterrupt:
                    pass
            finally:
                bb.stop_adc_dsp_stream()
                dsp.close_csv()

        # ── Poll path (legacy) ────────────────────────────────────────
        else:
            bb.set_adc_config(adc_ch, AdcMux.LF_TO_AGND, adc_range, AdcRate.SPS_200_H)

            print("\n── Log File ─────────────────────────────────────────────────")
            log_dir   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
            log_path  = _prompt("Log filename", next_log_filename(log_dir))
            if not os.path.isabs(log_path):
                log_path = os.path.join(log_dir, log_path)
            if not log_path.endswith(".csv"):
                log_path += ".csv"
            print(f"  Writing to: {log_path}")

            vs_actual = measure_ad8411a_supply(bb, ad8411a_info)

            print("\n── Starting Measurement ─────────────────────────────────────")
            print("  Enable your DUT load now.")
            print("  Close the plot window or press Ctrl+C to stop.\n")

            meas = Measurement()
            meas.open_csv(log_path, vs_actual, rsense_mohm, ad8411a_block, dut_label)

            poll_thread = threading.Thread(
                target=meas.poll,
                args=(bb, adc_ch, rsense_ohm, dut_supply_v),
                daemon=True,
            )
            poll_thread.start()
            try:
                run_ui_poll(meas, rsense_mohm, log_path, dut_supply_v)
            except KeyboardInterrupt:
                pass
            finally:
                meas.running = False
                poll_thread.join(timeout=3.0)
                meas.close_csv()

    finally:
        print("\n── Shutting down ────────────────────────────────────────────")
        if hal is not None:
            try:
                hal.shutdown()
                print("  Hardware powered down.")
            except Exception as exc:
                print(f"  HAL shutdown error: {exc}")
        if log_path:
            print(f"  Log: {log_path}")
        print("  Done.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
