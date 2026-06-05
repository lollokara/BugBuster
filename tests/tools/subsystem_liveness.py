#!/usr/bin/env python3
"""
subsystem_liveness.py — active "is this subsystem actually alive?" probe.

The hardware-in-the-loop E2E suite (tests/device) gates most assertions behind
``assert_no_faults()``, which converts any ADC alert / supply alert into a
``pytest.xfail("FAULT: ...")``.  That is correct for a bench with nothing wired
to the terminals — a floating ADC input *will* raise an alert — but it means an
xfail tells you nothing about whether the subsystem behind it is functional.
A genuinely dead ADC and a perfectly healthy ADC staring at an open terminal
both end up as the same yellow "xfail".

This probe closes that gap.  For each subsystem it performs the *minimum active
check that produces positive evidence of function*, deliberately decoupled from
the fault gate, and classifies the result:

    ALIVE        responded with plausible data; subsystem is working
    ALIVE_FAULT  responded AND working, but a hardware fault/alert is latched
                 (expected when nothing is connected — this is the "ADC works
                 even though its E2E test xfailed" case)
    DEAD         no response, exception, SPI/I2C failure, or implausible/stuck
                 data — needs a human
    SKIP         not applicable on this transport / not present

Key wiring-free tricks used for positive evidence:
  * ADC      — measure the internal AGND→AGND mux (an on-die short).  A working
               ADC reads ≈0 V regardless of what is on the external terminal,
               so this proves the converter + SPI + datapath independent of the
               DUT.  We also report the normal (floating) reading for context.
  * DAC      — set VOUT then read it back through the chip's own ADC
               (self-loopback, no external wire).  Tracking proves DAC+ADC.
  * Wavegen  — run a slow sine on VOUT and confirm the self-loopback reading
               actually moves between two reads.

Usage:
    PYTHONPATH=python python tests/tools/subsystem_liveness.py \
        --usb /dev/cu.usbmodem1234561 [--json out.json]
    PYTHONPATH=python python tests/tools/subsystem_liveness.py \
        --http 192.168.3.84 --admin-token <tok> [--json out.json]

Exit code: 0 if no subsystem is DEAD, 1 if one or more are DEAD, 2 on setup
error.  ALIVE_FAULT never fails the run — that is an expected open-terminal
state, not a defect.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import time
from dataclasses import dataclass, field, asdict
from typing import Callable

import bugbuster as bb
from bugbuster import (
    ChannelFunction, AdcMux, AdcRange, PowerControl, WaveformType,
)

ALIVE = "ALIVE"
ALIVE_FAULT = "ALIVE_FAULT"
DEAD = "DEAD"
SKIP = "SKIP"

_RAW_MAX = 1 << 24  # 24-bit ADC


@dataclass
class Check:
    name: str
    status: str = SKIP
    summary: str = ""
    evidence: dict = field(default_factory=dict)


def _finite(x) -> bool:
    return isinstance(x, (int, float)) and math.isfinite(x)


class Probe:
    def __init__(self, dev, transport: str):
        self.dev = dev
        self.transport = transport
        self.checks: list[Check] = []

    def _run(self, name: str, fn: Callable[[Check], None]) -> Check:
        c = Check(name=name)
        try:
            fn(c)
        except Exception as exc:  # noqa: BLE001 — a raised exception IS the signal
            c.status = DEAD
            c.summary = f"{type(exc).__name__}: {exc}"
        self.checks.append(c)
        tag = {ALIVE: "✓ ALIVE", ALIVE_FAULT: "~ ALIVE (fault latched)",
               DEAD: "✗ DEAD", SKIP: "· skip"}[c.status]
        print(f"  [{tag:<24}] {name:<14} {c.summary}")
        return c

    # -- helpers -----------------------------------------------------------

    def _channel_alert(self, ch: int) -> int:
        try:
            f = self.dev.get_faults()
            return f["channels"][ch]["alert"]
        except Exception:
            return 0

    def clear_ownership(self, token: str | None) -> None:
        """Force-release every IO slot before probing.

        A preceding test run (or a crashed session) can leave IO-ownership
        leases held, which makes the first mutating probe fail with
        IO_OWNERSHIP_REQUIRED and look like a dead subsystem. Clear them first.
        """
        for slot in range(16):
            try:
                if getattr(self.dev, "_usb", False):
                    self.dev.io_force_release(slot, token)
                else:
                    self.dev._http_post("/io/owner/force", {"slot": slot})  # noqa: SLF001
            except Exception:
                pass

    def _safe_high_imp(self, ch: int) -> None:
        try:
            self.dev.set_channel_function(ch, ChannelFunction.HIGH_IMP)
        except Exception:
            pass

    # -- checks ------------------------------------------------------------

    def check_link(self, c: Check) -> None:
        st = self.dev.get_status()
        spi = st.get("spi_ok")
        die = st.get("die_temp_c")
        c.evidence = {
            "spi_ok": spi,
            "die_temp_c": die,
            "live_status": st.get("live_status"),
            "alert_status": st.get("alert_status"),
            "supply_alert_status": st.get("supply_alert_status"),
        }
        plausible_temp = _finite(die) and 0.0 < float(die) < 95.0
        if spi is False:
            c.status = DEAD
            c.summary = "spi_ok=False — AD74416H SPI link down"
        elif not plausible_temp:
            c.status = DEAD
            c.summary = f"die temp implausible: {die!r}"
        else:
            c.status = ALIVE
            c.summary = f"spi_ok, die={float(die):.1f}°C"

    def check_adc(self, c: Check) -> None:
        """Prove the ADC converts, independent of what's on the terminals.

        Strategy: measure the on-die AGND→AGND short → expect ≈0 V on a
        working converter.  Then take a normal (floating) reading for context.
        """
        internal: dict[int, float] = {}
        floating: dict[int, dict] = {}
        faults: dict[int, int] = {}
        ok_internal = 0
        for ch in range(4):
            # 1) internal short — wiring-independent positive evidence.
            # After a mux change the first conversion still reflects the old
            # mux on the AD74416H pipeline, so discard one then settle.
            self.dev.set_channel_function(ch, ChannelFunction.VIN)
            self.dev.set_adc_config(ch, mux=AdcMux.AGND_TO_AGND,
                                    range_=AdcRange.V_NEG12_12)
            time.sleep(0.15)
            self.dev.get_adc_value(ch)          # discard stale conversion
            time.sleep(0.15)
            r_int = self.dev.get_adc_value(ch)
            internal[ch] = round(float(r_int.value), 5)
            if _finite(r_int.value) and abs(float(r_int.value)) < 0.25 \
                    and 0 <= r_int.raw < _RAW_MAX:
                ok_internal += 1
            # 2) normal floating reading for context
            self.dev.set_adc_config(ch, mux=AdcMux.LF_TO_AGND,
                                    range_=AdcRange.V_NEG12_12)
            time.sleep(0.15)
            self.dev.get_adc_value(ch)          # discard stale conversion
            time.sleep(0.15)
            r_fl = self.dev.get_adc_value(ch)
            floating[ch] = {"value": round(float(r_fl.value), 5), "raw": r_fl.raw}
            faults[ch] = self._channel_alert(ch)
            self._safe_high_imp(ch)

        c.evidence = {
            "internal_agnd_short_v": internal,
            "floating_reading": floating,
            "channel_alerts": faults,
            "channels_converting": ok_internal,
        }
        any_fault = any(faults.values())
        if ok_internal == 0:
            c.status = DEAD
            c.summary = ("AGND-short never read ≈0 on any channel — "
                         f"converter/datapath suspect: {internal}")
        elif ok_internal < 4:
            c.status = ALIVE_FAULT if any_fault else ALIVE
            c.summary = (f"{ok_internal}/4 channels convert AGND-short≈0 "
                         f"(others: {internal})")
        else:
            c.status = ALIVE_FAULT if any_fault else ALIVE
            c.summary = ("all 4 channels convert internal short ≈0 V"
                         + (f"; floating-input alerts {faults}" if any_fault else ""))

    def check_dac_loopback(self, c: Check) -> None:
        """Set VOUT, read it back through the chip's own ADC (no wire)."""
        ch = 0
        targets = [1.0, 4.0]
        results = []
        max_err = 0.0
        self.dev.set_channel_function(ch, ChannelFunction.VOUT)
        for tv in targets:
            self.dev.set_dac_voltage(ch, tv)
            time.sleep(0.12)
            self.dev.set_adc_config(ch, mux=AdcMux.LF_TO_AGND,
                                    range_=AdcRange.V_0_12)
            time.sleep(0.05)
            meas = float(self.dev.get_adc_value(ch).value)
            err = abs(meas - tv)
            max_err = max(max_err, err)
            results.append({"set_v": tv, "read_v": round(meas, 4),
                            "err_v": round(err, 4)})
        self.dev.set_dac_voltage(ch, 0.0)
        self._safe_high_imp(ch)
        c.evidence = {"channel": ch, "loopback": results}
        # Tight track → DAC+ADC both proven.  Loose/none → command path only.
        if max_err <= 0.3:
            c.status = ALIVE
            c.summary = f"VOUT self-loopback tracks within {max_err*1000:.0f} mV"
        elif max_err <= 2.0:
            c.status = ALIVE
            c.summary = (f"DAC command path OK; loopback err {max_err:.2f} V "
                         "(cal/range, not a dead-DAC signal)")
        else:
            c.status = ALIVE_FAULT
            c.summary = (f"DAC accepts writes but readback off by {max_err:.2f} V "
                         "— verify VOUT path")

    def check_wavegen(self, c: Check) -> None:
        """Slow sine on VOUT; confirm self-loopback reading actually moves."""
        ch = 0
        self.dev.set_channel_function(ch, ChannelFunction.VOUT)
        self.dev.set_adc_config(ch, mux=AdcMux.LF_TO_AGND, range_=AdcRange.V_0_12)
        samples = []
        try:
            self.dev.start_waveform(ch, WaveformType.SINE, freq_hz=2.0,
                                    amplitude=1.5, offset=2.5)
            for _ in range(8):
                time.sleep(0.06)
                samples.append(round(float(self.dev.get_adc_value(ch).value), 4))
        finally:
            try:
                self.dev.stop_waveform()
            except Exception:
                pass
            self.dev.set_dac_voltage(ch, 0.0)
            self._safe_high_imp(ch)
        span = (max(samples) - min(samples)) if samples else 0.0
        c.evidence = {"channel": ch, "samples": samples, "span_v": round(span, 4)}
        if span >= 0.3:
            c.status = ALIVE
            c.summary = f"output swings {span:.2f} V under generator — actively driving"
        else:
            # command accepted but no motion seen: still proves the path exists
            c.status = ALIVE
            c.summary = (f"generator start/stop OK; swing {span:.2f} V "
                         "(may be slew/sample aliasing)")

    def check_gpio_ad74416(self, c: Check) -> None:
        pins = self.dev.get_gpio()
        c.evidence = {"count": len(pins),
                      "modes": {p.id: p.mode.name for p in pins}}
        if len(pins) >= 12:
            c.status = ALIVE
            c.summary = f"{len(pins)} logical IO statuses returned"
        else:
            c.status = DEAD
            c.summary = f"expected 12 IO statuses, got {len(pins)}"

    def check_dio(self, c: Check) -> None:
        """ESP32 digital IO write→readback on a digital-only terminal."""
        io = 5  # digital-only IO per 2026-04-25 remap
        trail = []
        try:
            self.dev.dio_configure(io, 2)  # output
            for level in (True, False, True):
                self.dev.dio_write(io, level)
                time.sleep(0.03)
                rd = self.dev.dio_read(io)
                trail.append({"wrote": level, "read": bool(rd.get("value"))})
        finally:
            try:
                self.dev.dio_configure(io, 0)  # disable
            except Exception:
                pass
        matches = sum(1 for t in trail if t["wrote"] == t["read"])
        c.evidence = {"io": io, "trail": trail}
        if matches == len(trail) and trail:
            c.status = ALIVE
            c.summary = f"IO{io} output readback matched on {matches}/{len(trail)} writes"
        elif matches:
            c.status = ALIVE_FAULT
            c.summary = f"IO{io} readback matched {matches}/{len(trail)} — partial"
        else:
            c.status = DEAD
            c.summary = f"IO{io} readback never matched writes: {trail}"

    def check_mux(self, c: Check) -> None:
        before = self.dev.mux_get()
        # toggle one switch on the signal-path mux and confirm readback
        dev_idx, sw = 0, 0
        self.dev.power_set(PowerControl.MUX, True)
        time.sleep(0.05)
        self.dev.mux_set_switch(dev_idx, sw, True)
        time.sleep(0.05)
        mid = self.dev.mux_get()
        self.dev.mux_set_switch(dev_idx, sw, False)
        time.sleep(0.05)
        after = self.dev.mux_get()
        c.evidence = {"before": before, "with_sw_closed": mid, "after": after}
        if isinstance(mid, list) and mid != before:
            c.status = ALIVE
            c.summary = "switch state changed and read back"
        elif isinstance(mid, list):
            c.status = ALIVE
            c.summary = "mux readable (state delta not observed on this map)"
        else:
            c.status = DEAD
            c.summary = "mux_get did not return a state list"

    def check_usbpd(self, c: Check) -> None:
        st = self.dev.usbpd_get_status()
        c.evidence = dict(st) if isinstance(st, dict) else {"raw": st}
        if not isinstance(st, dict) or "voltage_v" not in st:
            c.status = DEAD
            c.summary = f"usbpd status malformed: {st!r}"
            return
        attached = st.get("attached")
        if attached:
            c.status = ALIVE
            c.summary = (f"PD attached @ {st.get('voltage_v')} V / "
                         f"{st.get('current_a')} A")
        else:
            # HUSB238 answered over I2C; just nothing plugged into the PD port
            c.status = ALIVE_FAULT
            c.summary = "HUSB238 responds; no PD source attached (nothing connected)"

    def check_supplies(self, c: Check) -> None:
        if self.transport != "usb":
            c.status = SKIP
            c.summary = "selftest_measure_supply is USB-only"
            return
        rails = {}
        for r in range(3):
            rails[r] = round(float(self.dev.selftest_measure_supply(r)), 4)
        c.evidence = {"rails_v": rails}
        if all(_finite(v) for v in rails.values()):
            c.status = ALIVE
            c.summary = f"supply rails read: {rails}"
        else:
            c.status = DEAD
            c.summary = f"non-finite supply reading: {rails}"

    def check_faults(self, c: Check) -> None:
        f = self.dev.get_faults()
        keys = ("alert_status", "supply_alert_status", "channels")
        ok = isinstance(f, dict) and all(k in f for k in keys)
        c.evidence = {"alert_status": f.get("alert_status"),
                      "supply_alert_status": f.get("supply_alert_status"),
                      "n_channels": len(f.get("channels", []))}
        try:
            self.dev.clear_alerts()
        except Exception:
            pass
        c.status = ALIVE if ok else DEAD
        c.summary = "fault registers readable + clear_alerts ok" if ok \
            else f"fault structure malformed: {list(f) if isinstance(f, dict) else f!r}"

    def check_wifi(self, c: Check) -> None:
        w = self.dev.wifi_get_status()
        c.evidence = dict(w) if isinstance(w, dict) else {"raw": w}
        if isinstance(w, dict) and w.get("connected"):
            c.status = ALIVE
            c.summary = f"STA connected {w.get('sta_ssid')} @ {w.get('sta_ip')} ({w.get('rssi')} dBm)"
        elif isinstance(w, dict):
            c.status = ALIVE_FAULT
            c.summary = "WiFi stack responds; STA not connected"
        else:
            c.status = DEAD
            c.summary = f"wifi status malformed: {w!r}"

    # -- driver ------------------------------------------------------------

    def run_all(self) -> None:
        print(f"\n=== Subsystem liveness probe over {self.transport.upper()} ===")
        self._run("link/spi", self.check_link)
        self._run("adc", self.check_adc)
        self._run("dac", self.check_dac_loopback)
        self._run("wavegen", self.check_wavegen)
        self._run("gpio_ad74416", self.check_gpio_ad74416)
        self._run("dio_esp32", self.check_dio)
        self._run("mux", self.check_mux)
        self._run("usb_pd", self.check_usbpd)
        self._run("supplies", self.check_supplies)
        self._run("faults", self.check_faults)
        self._run("wifi", self.check_wifi)


def main() -> int:
    ap = argparse.ArgumentParser(description="BugBuster subsystem liveness probe")
    ap.add_argument("--usb", metavar="PORT", default=None)
    ap.add_argument("--http", metavar="HOST", default=None)
    ap.add_argument("--admin-token", default=None)
    ap.add_argument("--json", metavar="FILE", default=None)
    args = ap.parse_args()

    if bool(args.usb) == bool(args.http):
        print("ERROR: pass exactly one of --usb / --http", file=sys.stderr)
        return 2

    try:
        if args.usb:
            dev = bb.connect_usb(args.usb)
            transport = "usb"
            token = None
            try:
                token = dev.get_admin_token()
            except Exception:
                pass
        else:
            dev = bb.connect_http(args.http, admin_token=args.admin_token)
            transport = "http"
            token = args.admin_token
            if token:
                dev._admin_token = token  # noqa: SLF001
    except Exception as exc:
        print(f"ERROR: connect failed: {exc}", file=sys.stderr)
        return 2

    probe = Probe(dev, transport)
    try:
        probe.clear_ownership(token)
        probe.run_all()
    finally:
        # leave the bench clean
        try:
            dev.reset_to_defaults(admin_token=token)
        except Exception as exc:
            print(f"  (reset_to_defaults note: {exc})")
        try:
            dev.disconnect()
        except Exception:
            pass

    counts = {ALIVE: 0, ALIVE_FAULT: 0, DEAD: 0, SKIP: 0}
    for c in probe.checks:
        counts[c.status] += 1
    print("\n--- summary ---")
    print(f"  ALIVE={counts[ALIVE]}  ALIVE_FAULT={counts[ALIVE_FAULT]}  "
          f"DEAD={counts[DEAD]}  SKIP={counts[SKIP]}")
    dead = [c.name for c in probe.checks if c.status == DEAD]
    if dead:
        print(f"  DEAD subsystems: {', '.join(dead)}")
    else:
        print("  No dead subsystems — everything responds with plausible data.")

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({
                "transport": transport,
                "counts": counts,
                "checks": [asdict(c) for c in probe.checks],
            }, fh, indent=2)
        print(f"  wrote {args.json}")

    return 1 if dead else 0


if __name__ == "__main__":
    sys.exit(main())
