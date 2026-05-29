import { useState } from "preact/hooks";
import { usePoll as useInterval } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { Led } from "../../components/Led";
import { api, PairingRequiredError } from "../../api/client";
import { USBPD_VOLTAGE_OPTIONS } from "../../config/options";
import { deviceMac } from "../../state/signals";

export function UsbPdCard() {
  const mac = deviceMac.value;
  const pd = useInterval(() => api.usbpd(), 2000) as any;
  const [busyVoltage, setBusyVoltage] = useState<number | null>(null);
  const [status, setStatus] = useState<string | null>(null);

  const selectVoltage = async (voltage: 5 | 9 | 12 | 15 | 18 | 20) => {
    if (!mac) return;
    setBusyVoltage(voltage);
    setStatus(null);
    try {
      await api.usbpdSelect(mac, voltage);
      setStatus(`Negotiating ${voltage}V`);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusyVoltage(null);
    }
  };

  const requestCaps = async () => {
    if (!mac) return;
    try {
      await api.usbpdRequestCaps(mac);
      setStatus("Source capabilities requested");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    }
  };

  const pdos = Array.isArray(pd?.sourcePdos) ? pd.sourcePdos : [];
  const selected = Number(pd?.selectedPdo ?? -1);

  return (
    <GlassCard title="USB-PD">
      <div class="kv-row">
        <span class="uppercase-tag">Attached</span>
        <Led state={pd?.attached ? "on" : "off"} label={pd?.attached ? "Yes" : "No"} />
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Contract</span>
        <span class="mono">{Number(pd?.voltageV ?? 0).toFixed(1)}V / {Number(pd?.currentA ?? 0).toFixed(2)}A</span>
      </div>
      <div class="kv-row" style={{ gap: "8px" }}>
        <button class="btn" disabled={!mac} onClick={requestCaps}>Request Caps</button>
        {USBPD_VOLTAGE_OPTIONS.map((v) => (
          <button
            key={v}
            class={`pill${busyVoltage === v || Number(pd?.voltageV ?? 0) === v ? " active" : ""}`}
            disabled={!mac || busyVoltage !== null}
            onClick={() => selectVoltage(v)}
          >
            {v}V
          </button>
        ))}
      </div>
      {status && <div class="text-dim">{status}</div>}
      <table class="kv-table">
        <thead>
          <tr><th>PDO</th><th>Detected</th><th>Max A</th><th>Max W</th><th></th></tr>
        </thead>
        <tbody>
          {pdos.map((p: any, i: number) => {
            const voltage = parseInt(String(p?.voltage ?? "").replace(/[^0-9]/g, ""), 10);
            const detected = !!p?.detected;
            const selectable = detected && [5, 9, 12, 15, 18, 20].includes(voltage);
            return (
              <tr key={i}>
                <td class="mono">{String(p?.voltage ?? "—")}{selected === i + 1 ? " (selected)" : ""}</td>
                <td>{detected ? "Yes" : "No"}</td>
                <td class="mono">{Number(p?.maxCurrentA ?? 0).toFixed(2)}</td>
                <td class="mono">{Number(p?.maxPowerW ?? 0).toFixed(1)}</td>
                <td>
                  <button
                    class="pill"
                    disabled={!mac || !selectable || busyVoltage !== null}
                    onClick={() => selectVoltage(voltage as 5 | 9 | 12 | 15 | 18 | 20)}
                  >
                    Select
                  </button>
                </td>
              </tr>
            );
          })}
          {pdos.length === 0 && (
            <tr><td colSpan={5} class="text-dim">No PDOs reported</td></tr>
          )}
        </tbody>
      </table>
    </GlassCard>
  );
}
