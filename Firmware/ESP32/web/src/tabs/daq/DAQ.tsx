// =============================================================================
// DAQ tab — WEB-1 & WEB-2: DAQ HAT power analyzer UI and DUT supply control.
// =============================================================================

import { useState, useEffect } from "preact/hooks";
import { signal } from "@preact/signals";
import { GlassCard } from "../../components/GlassCard";
import { BigValue } from "../../components/BigValue";
import { api, PairingRequiredError } from "../../api/client";
import { deviceMac } from "../../state/signals";
import { daqView } from "./daqView";

const daqStatus = signal<any>(null);
const vdutStatus = signal<any>(null);

export function DAQ() {
  const mac = deviceMac.value;
  const [vdutVoltage, setVdutVoltage] = useState(3.3);
  const [vdutCurrentLimit, setVdutCurrentLimit] = useState(500);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);

  // Poll DAQ status
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const [daq, vdut] = await Promise.all([
          api.daq.status(),
          api.daq.vdutStatus(),
        ]);
        daqStatus.value = daq;
        vdutStatus.value = vdut;
      } catch {
        /* Device may not have DAQ HAT */
      }
      if (alive) setTimeout(tick, 1000);
    };
    tick();
    return () => { alive = false; };
  }, []);

  const view = daqView(daqStatus.value, vdutStatus.value);
  const present = view.present;
  const vdutEnabled = view.vdut.enabled;
  const vdutVMeas = view.vdut.measuredV;
  const vdutIMeas = view.vdut.measuredA;
  const vdutPower = view.vdut.powerW;
  const range = view.vdut.range;
  const rangeCalibrated = view.calibration === "calibrated";

  const toggleEnable = async () => {
    if (!mac) return;
    setBusy(true);
    setStatus(null);
    try {
      await api.daq.vdutEnable(mac, !vdutEnabled);
      setStatus(vdutEnabled ? "DUT supply disabled" : "DUT supply enabled");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(false);
    }
  };

  const applySetpoint = async () => {
    if (!mac) return;
    setBusy(true);
    setStatus(null);
    try {
      await api.daq.vdutSetpoint(mac, vdutVoltage, vdutCurrentLimit);
      setStatus("Setpoint applied");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(false);
    }
  };

  if (!present) {
    return (
      <div class="tab-container">
        <h2>DAQ Power Analyzer</h2>
        <GlassCard title="DAQ HAT">
          <div class="text-dim">No DAQ HAT detected</div>
        </GlassCard>
      </div>
    );
  }

  return (
    <div class="tab-container">
      <h2>DAQ Power Analyzer</h2>

      <GlassCard title="DAQ HAT Status">
        <div class="kv-row">
          <span>Type</span>
          <span class="mono">{view.typeLabel}</span>
        </div>
        <div class="kv-row">
          <span>Version</span>
          <span class="mono">{view.version}</span>
        </div>
        {view.calibration === "uncalibrated" && (
          <div style="margin-top: 1rem; padding: 0.5rem; background: rgba(245, 158, 11, 0.1); border-left: 3px solid #f59e0b; color: #f59e0b;">
            ⚠ One or more current ranges are UNCALIBRATED. Current and energy readings carry an uncompensated offset.
          </div>
        )}
      </GlassCard>

      <GlassCard title="DUT Supply (VDUT)">
        <div class="kv-row">
          <span>Enable</span>
          <button
            class={"btn" + (vdutEnabled ? " active" : "")}
            onClick={toggleEnable}
            disabled={busy}
          >
            {vdutEnabled ? "ON" : "OFF"}
          </button>
        </div>
        <div class="kv-row">
          <label>
            <span>Voltage</span>
            <input
              class="input"
              type="number"
              step={0.1}
              min={1.8}
              max={12}
              value={vdutVoltage}
              onInput={(e) => setVdutVoltage(Number((e.currentTarget as HTMLInputElement).value))}
            />
            <span class="text-dim">V</span>
          </label>
        </div>
        <div class="kv-row">
          <label>
            <span>Current Limit</span>
            <input
              class="input"
              type="number"
              step={10}
              min={100}
              max={2500}
              value={vdutCurrentLimit}
              onInput={(e) => setVdutCurrentLimit(Number((e.currentTarget as HTMLInputElement).value))}
            />
            <span class="text-dim">mA</span>
          </label>
        </div>
        <button class="btn primary" onClick={applySetpoint} disabled={busy || !mac}>
          {busy ? "Applying…" : "Apply Setpoint"}
        </button>
        {status && <div class="text-dim" style="margin-top: 0.5rem;">{status}</div>}
      </GlassCard>

      <GlassCard title="Live Measurements">
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 1rem;">
          <BigValue
            label="Voltage"
            value={vdutVMeas.toFixed(3)}
            unit="V"
          />
          <BigValue
            label={rangeCalibrated ? "Current" : "Current (UNCALIBRATED)"}
            value={vdutIMeas.toFixed(6)}
            unit="A"
            highlight={!rangeCalibrated}
          />
          <BigValue
            label={rangeCalibrated ? "Power" : "Power (UNCALIBRATED)"}
            value={vdutPower.toFixed(6)}
            unit="W"
            highlight={!rangeCalibrated}
          />
        </div>
        <div class="kv-row" style="margin-top: 1rem;">
          <span>Current Range</span>
          <span class="mono">
            {range}
            {!rangeCalibrated && <span style="color: #f59e0b;"> ⚠ UNCAL</span>}
          </span>
        </div>
      </GlassCard>
    </div>
  );
}
