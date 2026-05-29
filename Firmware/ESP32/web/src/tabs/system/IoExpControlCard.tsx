// =============================================================================
// IoExpControlCard — IOExp Power & EFuse controls
// =============================================================================

import { useState } from "preact/hooks";
import { usePoll as useInterval } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { api, PairingRequiredError, type SelftestSuppliesCached } from "../../api/client";
import { deviceMac } from "../../state/signals";

export function IoExpControlCard() {
  const mac = deviceMac.value;
  const data = useInterval(() => api.ioexp(), 1500) as any;
  const faultLog = useInterval(() => api.ioexp.faults(), 3000) as any;
  const suppliesCached = useInterval(() => api.selftestSuppliesCached(), 2000) as SelftestSuppliesCached | null;
  const [faultCfg, setFaultCfg] = useState({ auto_disable: true, log_events: true });
  const [busyControl, setBusyControl] = useState<string | null>(null);

  const enables = data?.enables ?? {};
  const efuses = Array.isArray(data?.efuses) ? data.efuses : [];

  const controls: Array<{ key: string; on: boolean; label: string }> = [
    { key: "vadj1", on: !!enables.vadj1, label: "VADJ1" },
    { key: "vadj2", on: !!enables.vadj2, label: "VADJ2" },
    { key: "15v", on: !!enables.analog15v, label: "±15V" },
    { key: "mux", on: !!enables.mux, label: "MUX" },
    { key: "usb", on: !!enables.usbHub, label: "USB Hub" },
  ];

  for (let i = 0; i < 4; i++) {
    controls.push({
      key: `efuse${i + 1}`,
      on: !!efuses[i]?.enabled,
      label: `EFuse ${i + 1}`,
    });
  }

  const toggle = async (control: string, on: boolean) => {
    if (!mac) return;
    setBusyControl(control);
    try {
      await api.ioexp.setControl(mac, control, on);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("ioexp.setControl failed", e);
    } finally {
      setBusyControl(null);
    }
  };

  const applyFaultConfig = async () => {
    if (!mac) return;
    try {
      await api.ioexp.setFaultConfig(
        mac,
        faultCfg.auto_disable,
        faultCfg.log_events,
      );
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        console.warn("ioexp.setFaultConfig failed", e);
      }
    }
  };

  return (
    <GlassCard title="IOExp Power & EFuse">
      <div class="dio-grid-compact">
        {controls.map((c) => (
          <div class="dio-cell" key={c.key}>
            <span class="mono">{c.label}</span>
            <button
              class={`pill${c.on ? " active" : ""}`}
              disabled={!mac || busyControl !== null}
              onClick={() => toggle(c.key, !c.on)}
            >
              {busyControl === c.key ? "..." : c.on ? "ON" : "OFF"}
            </button>
          </div>
        ))}
      </div>
      <div class="analog-row" style={{ marginTop: "10px" }}>
        <label>Auto-disable EFuse</label>
        <input
          type="checkbox"
          checked={faultCfg.auto_disable}
          onChange={(e) =>
            setFaultCfg({
              ...faultCfg,
              auto_disable: (e.currentTarget as HTMLInputElement).checked,
            })
          }
        />
      </div>
      <div class="analog-row">
        <label>Log fault events</label>
        <input
          type="checkbox"
          checked={faultCfg.log_events}
          onChange={(e) =>
            setFaultCfg({
              ...faultCfg,
              log_events: (e.currentTarget as HTMLInputElement).checked,
            })
          }
        />
      </div>
      <button class="btn" disabled={!mac} onClick={applyFaultConfig}>Apply fault config</button>
      <details>
        <summary class="uppercase-tag">Fault Log</summary>
        <pre class="debug-dump mono">{JSON.stringify(faultLog, null, 2)}</pre>
      </details>
      <div style={{ marginTop: "10px" }}>
        <div class="uppercase-tag" style={{ marginBottom: "6px" }}>Live Supply Voltages</div>
        {suppliesCached && !suppliesCached.available && (
          <div class="text-dim" style={{ color: "#f59e0b", marginBottom: "4px" }}>interlock blocked</div>
        )}
        {(suppliesCached?.rails ?? []).map((r) => (
          <div class="kv-row" key={r.rail} style={{ opacity: suppliesCached?.available === false ? 0.5 : 1 }}>
            <span class="uppercase-tag">{r.name}</span>
            <span class="mono">
              {r.voltageV < 0 ? <span class="text-dim">disabled</span> : `${r.voltageV.toFixed(3)} V`}
            </span>
          </div>
        ))}
        {!suppliesCached && <div class="text-dim" style={{ fontSize: "11px" }}>—</div>}
      </div>
    </GlassCard>
  );
}
