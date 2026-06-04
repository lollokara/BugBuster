#!/usr/bin/env python3
"""
MeasureCurrent.py — Live current measurement via AD8411A + BugBuster.

Hardware: AD8411A current-sense amplifier (gain=50) connected to a BugBuster
IO_Block (configured for 5V supply). DUT powered from a different voltage
domain (other VADJ or HAT rail).

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

# Allow running from repo root or from Scripts/
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

AD8411A_GAIN      = 50.0
SAMPLE_RATE_HZ    = 20          # ADC polling rate (ADC at SPS_200_H → fresh data every 5 ms)
WINDOW_SECONDS    = 180         # 3-minute rolling display window
WINDOW_SAMPLES    = SAMPLE_RATE_HZ * WINDOW_SECONDS   # 3600 samples

IO_BLOCK_LABELS = {1: "A", 2: "B", 3: "C", 4: "D"}

# Public channel APIs are logical/user-facing A/B/C/D. Firmware maps logical
# C/D to the swapped physical AD74416H registers internally.
ADC_CH_LABELS = {0: "A", 1: "B", 2: "C", 3: "D"}


def _build_io_block_map() -> dict[int, dict]:
    """Build IO_Block resources from the HAL routing table to avoid drift."""
    result = {}
    for block in range(1, 5):
        analog_io = block * 3
        rt = DEFAULT_ROUTING[analog_io]
        result[block] = {
            "label": IO_BLOCK_LABELS[block],
            "analog_io": analog_io,
            "adc_ch": rt.channel,
            "adc_label": ADC_CH_LABELS[rt.channel],
            "efuse": rt.efuse,
            "supply": rt.supply,
            "idac_ch": rt.supply_idac,
            "vadj_domain": rt.block,
            "selftest_rail": 0 if rt.block == 1 else 1,
        }
    return result


# IO_Block -> hardware resources derived from hal.py DEFAULT_ROUTING.
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
# Autorange
# ---------------------------------------------------------------------------

_RANGE_RES_NV = {
    AdcRange.V_0_312MV:    18.6,   # 312.5 mV / 2^24
    AdcRange.V_0_625MV:    37.3,   # 625 mV / 2^24
    AdcRange.V_NEG2_5_2_5: 298.0,  # 5 V span / 2^24
    AdcRange.V_0_12:       715.0,  # 12 V / 2^24
}


def select_adc_range(vout_max: float) -> tuple[AdcRange, str]:
    """Return the tightest ADC range that covers vout_max (unidirectional, 0 V baseline)."""
    if vout_max <= 0.312:
        return AdcRange.V_0_312MV,    "V_0_312MV  (0–312 mV)"
    if vout_max <= 0.625:
        return AdcRange.V_0_625MV,    "V_0_625MV  (0–625 mV)"
    if vout_max <= 2.5:
        return AdcRange.V_NEG2_5_2_5, "V_NEG2_5_2_5 (±2.5 V, positive half)"
    return AdcRange.V_0_12,           "V_0_12     (0–12 V)"


def autorange(amps: float) -> tuple[float, str]:
    abs_v = abs(amps)
    if abs_v < 1e-6:
        return amps * 1e9, "nA"
    if abs_v < 1e-3:
        return amps * 1e6, "µA"
    if abs_v < 1.0:
        return amps * 1e3, "mA"
    return amps, "A"


def _energy_autorange(wh: float) -> tuple[float, str]:
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
    """Return the host IP/hostname to connect to, via mDNS auto-discovery."""
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

    # Multiple boards — let user pick
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
        # Auto-detect USB port
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
    """Configure AD8411A IO_Block: 5V supply, EFUSE on, ADC at 10 SPS hi-res."""
    info       = IO_BLOCK_MAP[block]
    analog_io  = info["analog_io"]
    adc_ch     = info["adc_ch"]

    print(
        f"\n  IO_Block {block} ({info['label']}) | IO{analog_io} "
        f"→ logical ADC ch{adc_ch} (CH {info['adc_label']}) | "
        f"VADJ{info['vadj_domain']} → 5.0 V"
    )

    # HAL must see 5V as the supply voltage before it powers up this IO_Block
    hal._supply_v = 5.0
    hal.configure(analog_io, PortMode.ANALOG_IN)

    # Initial safe config: V_0_12 covers any 0–5 V output until Rsense+I_max are known.
    # Rate SPS_200_H: 200 SPS, -90 dB 50/60 Hz rejection; 5 ms/conv always fresh at 20 Hz poll.
    # Range is overridden in main() once I_max is known (unidirectional range optimisation).
    bb.set_adc_config(adc_ch, AdcMux.LF_TO_AGND, AdcRange.V_0_12, AdcRate.SPS_200_H)

    print(f"  AD8411A ready: 5.0 V supply, CH {info['adc_label']} at 200 SPS")
    return info


# ---------------------------------------------------------------------------
# Phase 3 — DUT power
# ---------------------------------------------------------------------------

def setup_dut_bb_ioblock(bb, block: int, voltage: float) -> None:
    """Power a BB IO_Block for the DUT: set VADJ, enable rail + EFUSE."""
    info = IO_BLOCK_MAP[block]
    print(f"\n  DUT on IO_Block {block} | VADJ{info['vadj_domain']} → {voltage} V")

    bb.idac_set_voltage(info["idac_ch"], voltage)
    bb.power_set(info["supply"], on=True)
    time.sleep(0.5)
    bb.power_set(info["efuse"], on=True)

    print(f"  DUT IO_Block {block}: {voltage} V, EFUSE enabled")


def setup_dut_hat(bb, rail_id: int, voltage: float) -> None:
    """Power a HAT rail (VADJ3=rail_id 1, VADJ4=rail_id 2) for the DUT."""
    rail_name = {1: "VADJ3", 2: "VADJ4"}.get(rail_id, f"rail {rail_id}")
    print(f"\n  DUT on HAT {rail_name} → {voltage} V")

    bb.hat_set_rail_voltage(rail_id, int(voltage * 1000))
    bb.hat_set_rail_enable(rail_id, True)

    print(f"  HAT {rail_name}: {voltage} V enabled")


# ---------------------------------------------------------------------------
# Phase 6 — Zero-current calibration
# ---------------------------------------------------------------------------

def calibrate_zero(bb, adc_ch: int, n_samples: int = 20) -> float:
    print("\n── Zero-Current Calibration ──────────────────────────────────────")
    print("  Ensure the DUT is OFF and not drawing any current.")
    input("  Press Enter when ready…")

    samples = []
    for i in range(n_samples):
        samples.append(bb.get_adc_value(adc_ch).value)
        print(f"  \r  Sampling {i+1}/{n_samples}…", end="", flush=True)
        time.sleep(0.05)   # 50 ms — ADC at 200 SPS produces a new result every 5 ms

    voffset = sum(samples) / len(samples)
    spread  = max(samples) - min(samples)
    print(f"\n  Voffset = {voffset:.6f} V  (spread {spread*1000:.3f} mV)")
    return voffset


# ---------------------------------------------------------------------------
# Phase 7 — Selftest supply measurement
# ---------------------------------------------------------------------------

def measure_ad8411a_supply(bb, ad8411a_info: dict) -> float:
    """Measure actual AD8411A 5V supply via selftest, if channel 3 is free."""
    if ad8411a_info["adc_ch"] != 3:
        rail = ad8411a_info["selftest_rail"]   # 0=VADJ1, 1=VADJ2
        vs = bb.selftest_measure_supply(rail)
        if vs > 0:
            print(f"  AD8411A supply (measured): {vs:.4f} V")
            return vs
    print("  AD8411A supply (nominal): 5.0000 V")
    return 5.0


# ---------------------------------------------------------------------------
# Measurement state (shared between poll thread and animation)
# ---------------------------------------------------------------------------

class Measurement:
    def __init__(self):
        self.timestamps = collections.deque(maxlen=WINDOW_SAMPLES)
        self.currents   = collections.deque(maxlen=WINDOW_SAMPLES)
        self.powers     = collections.deque(maxlen=WINDOW_SAMPLES)
        self.live_amps  = 0.0
        self.live_watts = 0.0
        self.running    = False
        self._csv_file  = None
        self._lock      = threading.Lock()

    def open_csv(self, path: str, supply_v: float, rsense_mohm: float,
                 ad8411a_block: int, dut_label: str) -> None:
        self._csv_file = open(path, "w", newline="", buffering=1)
        hdr = (
            f"# BugBuster Current Measurement Log\n"
            f"# AD8411A supply: {supply_v:.4f} V\n"
            f"# Rsense: {rsense_mohm:.3f} mOhm  (gain={AD8411A_GAIN})\n"
            f"# AD8411A IO_Block: {ad8411a_block} | DUT: {dut_label}\n"
            f"# timestamp_unix,current_A,power_W\n"
        )
        self._csv_file.write(hdr)

    def close_csv(self) -> None:
        if self._csv_file:
            self._csv_file.close()
            self._csv_file = None

    def poll(self, bb, adc_ch: int, voffset: float,
             rsense_ohm: float, dut_supply_v: float) -> None:
        """Background thread: poll ADC → compute I/P → append to deques + CSV."""
        self.running = True
        interval = 1.0 / SAMPLE_RATE_HZ

        while self.running:
            t0 = time.monotonic()
            try:
                vout   = bb.get_adc_value(adc_ch).value
                i_amps = (vout - voffset) / (AD8411A_GAIN * rsense_ohm)
                p_watt = i_amps * dut_supply_v
                ts     = time.time()

                with self._lock:
                    self.timestamps.append(ts)
                    self.currents.append(i_amps)
                    self.powers.append(p_watt)
                    self.live_amps  = i_amps
                    self.live_watts = p_watt

                if self._csv_file:
                    self._csv_file.write(
                        f"{ts:.3f},{i_amps:.9e},{p_watt:.6e}\n"
                    )
            except Exception as exc:
                sys.stderr.write(f"\r[poll error: {exc}]")

            elapsed = time.monotonic() - t0
            time.sleep(max(0.0, interval - elapsed))


# ---------------------------------------------------------------------------
# Phase 8 — Matplotlib UI
# ---------------------------------------------------------------------------

def run_ui(meas: Measurement, rsense_mohm: float,
           log_path: str, dut_supply_v: float) -> None:

    fig = plt.figure(figsize=(12, 7), facecolor="#1a1a2e")
    ax_graph = fig.add_axes([0.09, 0.38, 0.87, 0.54])
    ax_hud   = fig.add_axes([0.09, 0.05, 0.87, 0.27])

    for ax in (ax_graph, ax_hud):
        ax.set_facecolor("#16213e")
        for spine in ax.spines.values():
            spine.set_color("#0f3460")

    ax_graph.tick_params(colors="#c0c0d0", labelsize=8)
    ax_graph.yaxis.label.set_color("#c0c0d0")
    ax_graph.xaxis.label.set_color("#c0c0d0")
    ax_graph.set_xlabel("Time (s ago)", fontsize=8)
    ax_graph.set_ylabel("Current", fontsize=8)
    ax_graph.set_title(
        f"Current Monitor  ·  Rsense {rsense_mohm:.3f} mΩ  ·  {os.path.basename(log_path)}",
        color="#e0e0e0", fontsize=9, pad=6,
    )
    ax_graph.grid(True, color="#0f3460", linewidth=0.5, alpha=0.7)

    (line,) = ax_graph.plot([], [], color="#00d4ff", linewidth=1.0, antialiased=True)

    ax_hud.axis("off")
    hud = ax_hud.text(
        0.5, 0.5, "Waiting for samples…",
        transform=ax_hud.transAxes,
        ha="center", va="center", fontsize=15,
        color="#00ff88", fontfamily="monospace",
        bbox=dict(boxstyle="round,pad=0.6", facecolor="#0f3460",
                  edgecolor="#00d4ff", alpha=0.85),
    )

    _current_unit = [None]   # mutable slot so closure can update it

    def _update(_frame):
        with meas._lock:
            ts_snap = list(meas.timestamps)
            i_snap  = list(meas.currents)
            p_snap  = list(meas.powers)
            live_a  = meas.live_amps
            live_w  = meas.live_watts

        if not ts_snap:
            return line, hud

        # ── Rolling graph ────────────────────────────────────────────
        now    = ts_snap[-1]
        x_data = [t - now for t in ts_snap]

        peak = max(abs(v) for v in i_snap) if i_snap else abs(live_a)
        _, unit = autorange(peak if peak > 0 else 1e-12)

        if unit != _current_unit[0]:
            ax_graph.set_ylabel(f"Current ({unit})", fontsize=8, color="#c0c0d0")
            _current_unit[0] = unit

        scale = {"nA": 1e9, "µA": 1e6, "mA": 1e3, "A": 1.0}[unit]
        y_data = [v * scale for v in i_snap]

        line.set_data(x_data, y_data)
        ax_graph.relim()
        ax_graph.autoscale_view()

        # ── HUD ──────────────────────────────────────────────────────
        live_val, live_unit = autorange(live_a)
        live_mw = live_w * 1e3

        if len(p_snap) > 1 and len(ts_snap) > 1:
            dt_avg   = (ts_snap[-1] - ts_snap[0]) / (len(ts_snap) - 1)
            energy_wh = sum(p_snap) * dt_avg / 3600.0
        else:
            energy_wh = 0.0

        e_val, e_unit = _energy_autorange(energy_wh)
        n_sec = len(ts_snap) / SAMPLE_RATE_HZ

        hud.set_text(
            f"Live: {live_val:+.3f} {live_unit:<3}   │   Power: {live_mw:.4f} mW\n"
            f"3-min window energy: {e_val:.4f} {e_unit}   │   "
            f"DUT supply: {dut_supply_v:.3f} V   │   "
            f"Samples: {len(ts_snap)} ({n_sec:.0f} s)"
        )

        return line, hud

    ani = animation.FuncAnimation(       # noqa: F841 (kept alive by plt.show)
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

        # Hat detect must be called early (required before any HAT API)
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
        if hat_present:
            if is_usb:
                options += [("HAT VADJ3 (1.8–36 V)", "hat", 1),
                            ("HAT VADJ4 (1.8–36 V)", "hat", 2)]
            else:
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

        # ── ADC range optimisation (unidirectional) ──────────────────
        print("\n── ADC Range Optimisation ───────────────────────────────────")
        print("  Unidirectional mode: Vout = 0 V at zero current.")
        print(f"  Enter max expected current to select tightest range (≤ {abs_max_i:.3g} A).")
        max_current_a = _prompt_float(
            "Max expected current (A)", min_val=1e-9, max_val=abs_max_i,
            default=round(abs_max_i / 10, 9),
        )
        vout_max = AD8411A_GAIN * rsense_ohm * max_current_a
        adc_range, range_label = select_adc_range(vout_max)
        res_nv = _RANGE_RES_NV[adc_range]
        print(f"  Vout_max = {vout_max*1000:.2f} mV → {range_label}  (~{res_nv:.0f} nV/LSB)")
        bb.set_adc_config(adc_ch, AdcMux.LF_TO_AGND, adc_range, AdcRate.SPS_200_H)

        # ── Phase 5: Log filename ────────────────────────────────────
        print("\n── Log File ─────────────────────────────────────────────────")
        log_dir     = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
        default_log = next_log_filename(log_dir)
        log_input   = _prompt("Log filename", default_log)
        if not os.path.isabs(log_input):
            log_path = os.path.join(log_dir, log_input)
        else:
            log_path = log_input
        if not log_path.endswith(".csv"):
            log_path += ".csv"
        print(f"  Writing to: {log_path}")

        # ── Phase 7: Supply voltage measurement ─────────────────────
        vs_actual = measure_ad8411a_supply(bb, ad8411a_info)

        # ── Phase 6: Zero-current calibration ───────────────────────
        voffset = calibrate_zero(bb, adc_ch)

        # ── Phase 8: Measurement loop ────────────────────────────────
        print("\n── Starting Measurement ─────────────────────────────────────")
        print("  Enable your DUT load now.")
        print("  Close the plot window or press Ctrl+C to stop.\n")

        meas = Measurement()
        meas.open_csv(log_path, vs_actual, rsense_mohm, ad8411a_block, dut_label)

        poll_thread = threading.Thread(
            target=meas.poll,
            args=(bb, adc_ch, voffset, rsense_ohm, dut_supply_v),
            daemon=True,
        )
        poll_thread.start()

        try:
            run_ui(meas, rsense_mohm, log_path, dut_supply_v)
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
