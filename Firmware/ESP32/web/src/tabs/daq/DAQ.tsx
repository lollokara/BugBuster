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

  // Seed the setpoint inputs from the device once, instead of the hard-coded
  // 3.3 V / 500 mA defaults.
  const [seeded, setSeeded] = useState(false);
  useEffect(() => {
    if (seeded || !vdutStatus.value) return;
    if (view.vdut.setpointV !== null) setVdutVoltage(view.vdut.setpointV);
    if (view.vdut.currentLimitMa !== null) setVdutCurrentLimit(view.vdut.currentLimitMa);
    setSeeded(true);
  }, [vdutStatus.value, seeded]);

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
        {view.calibration === "unknown" && (
          <div class="text-dim" style="margin-top: 0.5rem; font-size: 0.875rem;">
            Current-range calibration state is not reported over HTTP.
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
            label="Current"
            value={vdutIMeas.toFixed(6)}
            unit="A"
          />
          <BigValue
            label="Power"
            value={vdutPower.toFixed(6)}
            unit="W"
          />
        </div>
        <div class="kv-row" style="margin-top: 1rem;">
          <span>Fault</span>
          <span class="mono" style={view.vdut.fault ? "color: #ef4444;" : ""}>
            {view.vdut.fault ? "FAULT" : "none"}
          </span>
        </div>
      </GlassCard>
    </div>
  );
}
