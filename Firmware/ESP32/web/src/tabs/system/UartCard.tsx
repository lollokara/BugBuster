import { useEffect, useState } from "preact/hooks";
import { usePoll as useInterval } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { api, PairingRequiredError } from "../../api/client";
import {
  UART_BAUD_OPTIONS,
  UART_DATA_BITS_OPTIONS,
  UART_PARITY_OPTIONS,
  UART_STOP_BITS_OPTIONS,
} from "../../config/options";
import { deviceMac } from "../../state/signals";
import { ioLabelForGpio } from "./systemUtils";

export function UartCard() {
  const mac = deviceMac.value;
  const cfg = useInterval(() => api.uartConfig(), 2000) as any;
  const pins = useInterval(() => api.uartPins(), 10000) as any;
  const [bridgeId, setBridgeId] = useState<number>(0);
  const [form, setForm] = useState<any>(null);
  const [formDirty, setFormDirty] = useState(false);
  const [status, setStatus] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const bridges = Array.isArray(cfg?.bridges) ? cfg.bridges : [];
  const availablePins = Array.isArray(pins?.available) ? pins.available.map((v: any) => Number(v)) : [];

  useEffect(() => {
    if (bridges.length === 0) return;
    if (formDirty) return;
    const exists = bridges.some((b: any) => Number(b?.id) === bridgeId);
    const activeId = exists ? bridgeId : Number(bridges[0]?.id ?? 0);
    const active = bridges.find((b: any) => Number(b?.id) === activeId) ?? bridges[0];
    setBridgeId(activeId);
    setForm({
      uartNum: Number(active?.uartNum ?? 1),
      txPin: Number(active?.txPin ?? 1),
      rxPin: Number(active?.rxPin ?? 2),
      baudrate: Number(active?.baudrate ?? 115200),
      dataBits: Number(active?.dataBits ?? 8),
      parity: Number(active?.parity ?? 0),
      stopBits: Number(active?.stopBits ?? 0),
      enabled: !!active?.enabled,
    });
  }, [cfg, bridgeId, formDirty]);

  const apply = async () => {
    if (!mac || !form) return;
    setBusy(true);
    setStatus(null);
    try {
      await api.uartSetConfig(mac, bridgeId, form);
      setStatus(`Bridge ${bridgeId} updated`);
      setFormDirty(false);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(false);
    }
  };

  return (
    <GlassCard title="UART Bridge">
      <div class="kv-row">
        <span class="uppercase-tag">Bridge</span>
        <select
          class="input"
          value={String(bridgeId)}
          onChange={(e) => {
            setFormDirty(false);
            setBridgeId(parseInt((e.currentTarget as HTMLSelectElement).value, 10));
          }}
        >
          {bridges.map((b: any) => (
            <option key={b?.id} value={String(b?.id)}>Bridge {b?.id}</option>
          ))}
        </select>
      </div>
      {form && (
        <>
          <div class="analog-row"><label>UART Num</label>
            <select class="input" value={String(form.uartNum)} onChange={(e) => { setFormDirty(true); setForm({ ...form, uartNum: parseInt((e.currentTarget as HTMLSelectElement).value, 10) }); }}>
              {[0, 1, 2].map((v) => <option key={v} value={String(v)}>{v}</option>)}
            </select>
          </div>
          <div class="analog-row"><label>TX IO</label>
            <select class="input" value={String(form.txPin)} onChange={(e) => { setFormDirty(true); setForm({ ...form, txPin: parseInt((e.currentTarget as HTMLSelectElement).value, 10) }); }}>
              {availablePins.map((v: number) => <option key={v} value={String(v)}>{ioLabelForGpio(v)}</option>)}
            </select>
          </div>
          <div class="analog-row"><label>RX IO</label>
            <select class="input" value={String(form.rxPin)} onChange={(e) => { setFormDirty(true); setForm({ ...form, rxPin: parseInt((e.currentTarget as HTMLSelectElement).value, 10) }); }}>
              {availablePins.map((v: number) => <option key={v} value={String(v)}>{ioLabelForGpio(v)}</option>)}
            </select>
          </div>
          <div class="analog-row"><label>Baud</label>
            <select class="input" value={String(form.baudrate)} onChange={(e) => { setFormDirty(true); setForm({ ...form, baudrate: parseInt((e.currentTarget as HTMLSelectElement).value, 10) }); }}>
              {UART_BAUD_OPTIONS.map((v) => <option key={v} value={String(v)}>{v}</option>)}
            </select>
          </div>
          <div class="analog-row"><label>Data Bits</label>
            <select class="input" value={String(form.dataBits)} onChange={(e) => { setFormDirty(true); setForm({ ...form, dataBits: parseInt((e.currentTarget as HTMLSelectElement).value, 10) }); }}>
              {UART_DATA_BITS_OPTIONS.map((v) => <option key={v} value={String(v)}>{v}</option>)}
            </select>
          </div>
          <div class="analog-row"><label>Parity</label>
            <select class="input" value={String(form.parity)} onChange={(e) => { setFormDirty(true); setForm({ ...form, parity: parseInt((e.currentTarget as HTMLSelectElement).value, 10) }); }}>
              {UART_PARITY_OPTIONS.map((v) => <option key={v.code} value={String(v.code)}>{v.label}</option>)}
            </select>
          </div>
          <div class="analog-row"><label>Stop Bits</label>
            <select class="input" value={String(form.stopBits)} onChange={(e) => { setFormDirty(true); setForm({ ...form, stopBits: parseInt((e.currentTarget as HTMLSelectElement).value, 10) }); }}>
              {UART_STOP_BITS_OPTIONS.map((v) => <option key={v.code} value={String(v.code)}>{v.label}</option>)}
            </select>
          </div>
          <div class="analog-row"><label>Enabled</label>
            <input type="checkbox" checked={!!form.enabled} onChange={(e) => { setFormDirty(true); setForm({ ...form, enabled: (e.currentTarget as HTMLInputElement).checked }); }} />
          </div>
          <button class="btn" disabled={!mac || busy} onClick={apply}>{busy ? "Applying…" : "Apply UART"}</button>
          {status && <div class="text-dim">{status}</div>}
        </>
      )}
    </GlassCard>
  );
}

