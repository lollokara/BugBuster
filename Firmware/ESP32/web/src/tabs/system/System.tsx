// =============================================================================
// System tab — board profile, HAT, USB-PD, UART, WiFi, faults, IOExp, debug.
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { Led } from "../../components/Led";
import {
  api,
  PairingRequiredError,
  type BoardProfile,
  type BoardState,
  type SelftestSuppliesCached,
} from "../../api/client";
import {
  HAT_PIN_FUNCTION_OPTIONS,
  UART_BAUD_OPTIONS,
  UART_DATA_BITS_OPTIONS,
  UART_PARITY_OPTIONS,
  UART_STOP_BITS_OPTIONS,
  USBPD_VOLTAGE_OPTIONS,
} from "../../config/options";
import {
  boardState,
  deviceMac,
  selftestWorkerEnabled,
  supplyMonitorActive,
  setSelftestStatus,
  startSelftestStatusPolling,
} from "../../state/signals";

function useInterval<T>(fn: () => Promise<T>, ms: number) {
  const [value, setValue] = useState<T | null>(null);
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const r = await fn();
        if (alive) setValue(r);
      } catch {
        /* ignore */
      }
      if (alive) setTimeout(tick, ms);
    };
    tick();
    return () => {
      alive = false;
    };
  }, []);
  return value;
}

function parseMask(value: string): number {
  const v = value.trim().toLowerCase();
  if (!v) return 0;
  const n = v.startsWith("0x") ? parseInt(v.slice(2), 16) : parseInt(v, 10);
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.min(0xffff, n));
}

function maskToHex(mask: number | undefined): string {
  const safe = Number.isFinite(mask) ? (mask as number) : 0;
  return `0x${(safe & 0xffff).toString(16).toUpperCase().padStart(4, "0")}`;
}

const UART_IO_MAP: ReadonlyArray<readonly [number, number]> = [
  [1, 4], [2, 2], [3, 1],
  [4, 7], [5, 6], [6, 5],
  [7, 8], [8, 9], [9, 10],
  [10, 11], [11, 12], [12, 13],
];

function ioLabelForGpio(gpio: number): string {
  const entry = UART_IO_MAP.find(([, g]) => g === gpio);
  return entry ? `IO${entry[0]} (GPIO${entry[1]})` : `GPIO${gpio}`;
}

function BoardCard() {
  const [selected, setSelected] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    const load = async () => {
      try {
        const r: BoardState = await api.board();
        if (alive) {
          boardState.value = r;
          setSelected(r.active);
        }
      } catch (e) {
        if (alive) setErr(e instanceof Error ? e.message : String(e));
      }
    };
    load();
    return () => {
      alive = false;
    };
  }, []);

  const state = boardState.value;
  const available = Array.isArray(state?.available) ? state!.available : [];
  const active: BoardProfile | undefined = available.find(
    (b) => b.id === state!.active,
  );

  const apply = async () => {
    const mac = deviceMac.value;
    if (!mac || !selected) return;
    setBusy(true);
    setErr(null);
    try {
      await api.boardSelect(mac, selected);
      const r = await api.board();
      boardState.value = r;
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setErr(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(false);
    }
  };

  return (
    <GlassCard title="Board Profile">
      {err && <div class="text-err" style={{ fontSize: "0.8rem" }}>{err}</div>}
      <div class="kv-row">
        <span class="uppercase-tag">Active</span>
        <span class="mono">{state?.active ?? "—"}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Select</span>
        <select
          class="input"
          value={selected ?? ""}
          onChange={(e) =>
            setSelected((e.currentTarget as HTMLSelectElement).value || null)
          }
        >
          <option value="">(none)</option>
          {available.map((b) => (
            <option key={b.id} value={b.id}>{b.name}</option>
          ))}
        </select>
        <button class="btn primary" onClick={apply} disabled={busy || !selected}>
          {busy ? "…" : "Apply"}
        </button>
      </div>

      {active && (
        <table class="kv-table">
          <thead>
            <tr>
              <th>Rail</th>
              <th>Value</th>
              <th>Lock</th>
            </tr>
          </thead>
          <tbody>
            {(["vlogic", "vadj1", "vadj2"] as const).map((k) => {
              const rail = active.rails[k];
              return (
                <tr key={k}>
                  <td class="uppercase-tag">{k}</td>
                  <td class="mono">{rail.value.toFixed(3)} V</td>
                  <td>
                    {rail.locked
                      ? <Led state="warn" label="locked" />
                      : <Led state="off" label="free" />}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </GlassCard>
  );
}

function HatCard() {
  const mac = deviceMac.value;
  const hat = useInterval(() => api.hat(), 2000) as any;
  const la = useInterval(() => api.hatLaStatus(), 1000) as any;
  const [busy, setBusy] = useState<"detect" | "reset" | "pins" | null>(null);
  const [pinCfg, setPinCfg] = useState<number[]>([0, 0, 0, 0]);
  const [pinDirty, setPinDirty] = useState(false);

  useEffect(() => {
    const arr = Array.isArray(hat?.pin_config)
      ? hat.pin_config
      : Array.isArray(hat?.pinConfig)
        ? hat.pinConfig.map((p: any) => Number(p?.function ?? 0))
        : null;
    if (!pinDirty && arr && arr.length === 4) {
      setPinCfg(arr.map((v: any) => Number(v) || 0));
    }
  }, [hat?.pin_config, hat?.pinConfig, pinDirty]);

  const applyPins = async () => {
    if (!mac) return;
    setBusy("pins");
    try {
      await api.hatSetPins(mac, pinCfg.slice(0, 4));
      setPinDirty(false);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("hatSetPins failed", e);
    } finally {
      setBusy(null);
    }
  };

  const detect = async () => {
    if (!mac) return;
    setBusy("detect");
    try {
      await api.hatDetect(mac);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("hatDetect failed", e);
    } finally {
      setBusy(null);
    }
  };

  const reset = async () => {
    if (!mac) return;
    setBusy("reset");
    try {
      await api.hatReset(mac);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("hatReset failed", e);
    } finally {
      setBusy(null);
    }
  };

  const detected = !!(hat?.detected ?? hat?.present);
  const connected = !!hat?.connected;
  const version = hat?.fwMajor != null ? `${hat.fwMajor}.${hat.fwMinor ?? 0}` : "—";

  return (
    <GlassCard title="HAT">
      <div class="kv-row">
        <span class="uppercase-tag">Detected</span>
        <Led state={detected ? "on" : "off"} label={detected ? "Yes" : "No"} />
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Connected</span>
        <Led state={connected ? "on" : "off"} label={connected ? "Yes" : "No"} />
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Type</span>
        <span class="mono">{hat?.typeName ?? "—"}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">FW</span>
        <span class="mono">{version}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">LA</span>
        <span class="mono">{la?.stateName ?? "—"}</span>
        <span class="mono text-dim">{la?.samplesCaptured ?? 0}/{la?.totalSamples ?? 0}</span>
      </div>
      <div class="kv-row" style={{ gap: "8px" }}>
        <button class="btn" disabled={!mac || busy !== null} onClick={detect}>Detect</button>
        <button class="btn" disabled={!mac || busy !== null} onClick={reset}>Reset</button>
      </div>
      <details>
        <summary class="uppercase-tag">Pin Mapping</summary>
        <div class="analog-grid" style={{ marginTop: "8px" }}>
          {[0, 1, 2, 3].map((i) => (
            <div class="analog-item" key={i}>
              <div class="uppercase-tag">EXT {i + 1}</div>
              <select
                class="input"
                value={String(pinCfg[i] ?? 0)}
                onChange={(e) => {
                  const next = [...pinCfg];
                  next[i] = parseInt((e.currentTarget as HTMLSelectElement).value, 10);
                  setPinCfg(next);
                  setPinDirty(true);
                }}
              >
                {HAT_PIN_FUNCTION_OPTIONS.map((opt) => (
                  <option key={opt.code} value={String(opt.code)}>
                    {opt.label}
                  </option>
                ))}
              </select>
            </div>
          ))}
        </div>
        <button class="btn" disabled={!mac || busy !== null} onClick={applyPins}>
          {busy === "pins" ? "Applying…" : "Apply Pins"}
        </button>
      </details>
    </GlassCard>
  );
}

function UsbPdCard() {
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

function UartCard() {
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

function WifiCard() {
  const mac = deviceMac.value;
  const [nets, setNets] = useState<any[]>([]);
  const [scanning, setScanning] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [ssid, setSsid] = useState("");
  const [pass, setPass] = useState("");
  const [status, setStatus] = useState<string | null>(null);

  const scan = async () => {
    setScanning(true);
    setStatus(null);
    try {
      const r = await api.wifiScan();
      setNets(Array.isArray(r?.networks) ? r.networks : Array.isArray(r) ? r : []);
      if (!Array.isArray(r?.networks) && !Array.isArray(r)) {
        setStatus("No scan results");
      }
    } catch (e) {
      setStatus(e instanceof Error ? e.message : String(e));
    } finally {
      setScanning(false);
    }
  };

  const connect = async () => {
    if (!mac || !ssid) return;
    setConnecting(true);
    setStatus(null);
    try {
      const r = await api.wifiConnect(mac, ssid, pass);
      if (r?.success) {
        setStatus(`Connected${r.ip ? ` (${r.ip})` : ""}`);
      } else {
        setStatus("Connection failed");
      }
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setConnecting(false);
    }
  };

  return (
    <GlassCard title="WiFi">
      <div class="kv-row">
        <button class="btn" onClick={scan} disabled={scanning}>
          {scanning ? "Scanning…" : "Scan"}
        </button>
      </div>
      {status && <div class="text-dim">{status}</div>}
      <ul class="wifi-list">
        {nets.map((n, i) => (
          <li key={i} onClick={() => setSsid(n.ssid ?? "")}>
            <span class="mono">{n.ssid ?? "—"}</span>
            <span class="text-dim">{n.rssi ?? ""} dBm</span>
          </li>
        ))}
        {nets.length === 0 && <li class="text-dim">No networks yet</li>}
      </ul>
      <div class="kv-row">
        <input
          class="input"
          placeholder="SSID"
          value={ssid}
          onInput={(e) => setSsid((e.currentTarget as HTMLInputElement).value)}
        />
        <input
          class="input"
          type="password"
          placeholder="password"
          value={pass}
          onInput={(e) => setPass((e.currentTarget as HTMLInputElement).value)}
        />
        <button class="btn primary" onClick={connect} disabled={!mac || !ssid || connecting}>
          {connecting ? "Connecting…" : "Connect"}
        </button>
      </div>
    </GlassCard>
  );
}

function IoExpControlCard() {
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

function FaultsCard() {
  const mac = deviceMac.value;
  const faults = useInterval(() => api.faults(), 2000) as any;
  const [alertMask, setAlertMask] = useState("0xFFFF");
  const [supplyMask, setSupplyMask] = useState("0xFFFF");
  const [channelMask, setChannelMask] = useState<Record<number, string>>({});
  const [masksDirty, setMasksDirty] = useState(false);

  useEffect(() => {
    if (masksDirty) return;
    const am = Number(faults?.alertMask ?? faults?.alert_mask ?? 0);
    const sm = Number(faults?.supplyAlertMask ?? faults?.supply_alert_mask ?? 0);
    setAlertMask(maskToHex(am));
    setSupplyMask(maskToHex(sm));

    const next: Record<number, string> = {};
    const channels = Array.isArray(faults?.channels) ? faults.channels : [];
    for (const ch of channels) {
      const id = Number(ch?.id ?? 0);
      const mask = Number(ch?.channelAlertMask ?? ch?.mask ?? 0);
      next[id] = maskToHex(mask);
    }
    setChannelMask(next);
  }, [faults, masksDirty]);

  const clearAll = async () => {
    if (!mac) return;
    try {
      await api.faultsClearAll(mac);
      setMasksDirty(false);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("faultsClearAll failed", e);
    }
  };

  const clearChannel = async (id: number) => {
    if (!mac) return;
    try {
      await api.faultsClearChannel(mac, id);
      setMasksDirty(false);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        console.warn("faultsClearChannel failed", e);
      }
    }
  };

  const applyGlobalMasks = async () => {
    if (!mac) return;
    try {
      await api.faultsSetMasks(mac, parseMask(alertMask), parseMask(supplyMask));
      setMasksDirty(false);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("faultsSetMasks failed", e);
    }
  };

  const applyChannelMask = async (id: number) => {
    if (!mac) return;
    try {
      await api.faultsSetChannelMask(mac, id, parseMask(channelMask[id] ?? "0"));
      setMasksDirty(false);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        console.warn("faultsSetChannelMask failed", e);
      }
    }
  };

  const alertStatus = Number(faults?.alertStatus ?? faults?.alert_status ?? 0);
  const supplyStatus = Number(
    faults?.supplyAlertStatus ?? faults?.supply_alert_status ?? 0,
  );
  const channels = Array.isArray(faults?.channels) ? faults.channels : [];

  return (
    <GlassCard
      title="Faults"
      actions={<button class="btn" disabled={!mac} onClick={clearAll}>Clear all</button>}
    >
      <div class="kv-row">
        <span class="uppercase-tag">Alert Status</span>
        <span class="mono">{maskToHex(alertStatus)}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Supply Status</span>
        <span class="mono">{maskToHex(supplyStatus)}</span>
      </div>

      <details>
        <summary class="uppercase-tag">Global Masks</summary>
        <div class="analog-row">
          <label>Alert mask</label>
          <input class="input" value={alertMask} onInput={(e) => { setMasksDirty(true); setAlertMask((e.currentTarget as HTMLInputElement).value); }} />
        </div>
        <div class="analog-row">
          <label>Supply mask</label>
          <input class="input" value={supplyMask} onInput={(e) => { setMasksDirty(true); setSupplyMask((e.currentTarget as HTMLInputElement).value); }} />
        </div>
        <button class="btn" disabled={!mac} onClick={applyGlobalMasks}>Apply masks</button>
      </details>

      <table class="kv-table">
        <thead>
          <tr><th>CH</th><th>Status</th><th>Mask</th><th></th><th></th></tr>
        </thead>
        <tbody>
          {channels.map((ch: any) => {
            const id = Number(ch?.id ?? 0);
            const status = Number(ch?.channelAlert ?? ch?.alert ?? 0);
            return (
              <tr key={id}>
                <td class="mono">{id}</td>
                <td class="mono">{maskToHex(status)}</td>
                <td>
                  <input
                    class="input"
                    value={channelMask[id] ?? "0x0000"}
                    onInput={(e) =>
                      { setMasksDirty(true); setChannelMask({
                        ...channelMask,
                        [id]: (e.currentTarget as HTMLInputElement).value,
                      }); }
                    }
                  />
                </td>
                <td>
                  <button class="pill" disabled={!mac} onClick={() => applyChannelMask(id)}>Apply</button>
                </td>
                <td>
                  <button class="pill" disabled={!mac} onClick={() => clearChannel(id)}>Clear</button>
                </td>
              </tr>
            );
          })}
          {channels.length === 0 && (
            <tr><td colSpan={5} class="text-dim">No channel fault data</td></tr>
          )}
        </tbody>
      </table>
    </GlassCard>
  );
}

function SelftestServiceCard() {
  const mac = deviceMac.value;
  const summary = useInterval(() => api.selftest(), 3000) as any;
  const supplies = useInterval(() => api.selftestSupplies(), 5000) as any;
  const suppliesCached = useInterval(() => api.selftestSuppliesCached(), 2000) as SelftestSuppliesCached | null;
  const [railValues, setRailValues] = useState<Record<number, number>>({});
  const [calChannel, setCalChannel] = useState(0);
  const [busy, setBusy] = useState<null | "probe0" | "probe1" | "probe2" | "cal" | "reset" | "worker">(null);
  const [status, setStatus] = useState<string | null>(null);

  useEffect(() => {
    if (summary) setSelftestStatus(summary);
  }, [summary]);

  const probeRail = async (rail: 0 | 1 | 2) => {
    setBusy(`probe${rail}` as "probe0" | "probe1" | "probe2");
    setStatus(null);
    try {
      const r = await api.selftestSupply(rail);
      const v = Number((r as any)?.voltage ?? NaN);
      if (Number.isFinite(v)) {
        setRailValues((prev) => ({ ...prev, [rail]: v }));
      }
    } catch (e) {
      setStatus(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(null);
    }
  };

  const startCalibration = async () => {
    if (!mac) return;
    setBusy("cal");
    setStatus(null);
    try {
      const r = await api.selftestCalibrate(mac, calChannel);
      const points = Number((r as any)?.points ?? 0);
      const err = Number((r as any)?.errorMv ?? NaN);
      setStatus(`Calibration started (ch=${calChannel}, points=${points}, error=${Number.isFinite(err) ? err.toFixed(1) : "?"}mV)`);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const resetDevice = async () => {
    if (!mac) return;
    if (!window.confirm("Reset the device now? The web session will disconnect briefly.")) return;
    setBusy("reset");
    setStatus(null);
    try {
      await api.deviceReset(mac);
      setStatus("Reset command sent.");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const toggleWorker = async () => {
    if (!mac) return;
    setBusy("worker");
    setStatus(null);
    try {
      setSelftestStatus(await api.selftestWorker(mac, !selftestWorkerEnabled.value));
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const cal = summary?.calibration ?? {};
  const boot = summary?.boot ?? {};

  return (
    <GlassCard title="Selftest / Service">
      <div class="kv-row"><span class="uppercase-tag">Boot Selftest</span><span class="mono">{boot?.ran ? (boot?.passed ? "PASS" : "FAIL") : "N/A"}</span></div>
      <div class="kv-row"><span class="uppercase-tag">Cal Status</span><span class="mono">{String(cal?.status ?? "—")}</span></div>
      <div class="kv-row"><span class="uppercase-tag">Cal Error</span><span class="mono">{Number.isFinite(Number(cal?.errorMv)) ? `${Number(cal.errorMv).toFixed(1)} mV` : "—"}</span></div>
      <div class="kv-row">
        <span class="uppercase-tag">Supply monitor (opt-in)</span>
        <button
          class={"pill" + (selftestWorkerEnabled.value ? " active" : "")}
          disabled={!mac || busy !== null}
          onClick={toggleWorker}
        >
          {busy === "worker" ? "..." : selftestWorkerEnabled.value ? "ON" : "OFF"}
        </button>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Monitor Active</span>
        <Led state={supplyMonitorActive.value ? "on" : "off"} label={supplyMonitorActive.value ? "Active" : "Idle"} />
      </div>

      <details>
        <summary class="uppercase-tag">Supply Probes</summary>
        <div class="kv-row" style={{ gap: "8px", marginTop: "8px" }}>
          <button class="btn" disabled={busy !== null} onClick={() => probeRail(0)}>Probe VADJ1</button>
          <button class="btn" disabled={busy !== null} onClick={() => probeRail(1)}>Probe VADJ2</button>
          <button class="btn" disabled={busy !== null} onClick={() => probeRail(2)}>Probe 3V3</button>
        </div>
        <div class="kv-row"><span class="uppercase-tag">VADJ1</span><span class="mono">{Number.isFinite(railValues[0]) ? `${railValues[0]!.toFixed(3)} V` : "—"}</span></div>
        <div class="kv-row"><span class="uppercase-tag">VADJ2</span><span class="mono">{Number.isFinite(railValues[1]) ? `${railValues[1]!.toFixed(3)} V` : "—"}</span></div>
        <div class="kv-row"><span class="uppercase-tag">3V3_ADJ</span><span class="mono">{Number.isFinite(railValues[2]) ? `${railValues[2]!.toFixed(3)} V` : "—"}</span></div>
        <div class="uppercase-tag" style={{ marginTop: "10px", marginBottom: "4px" }}>Live cache</div>
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
      </details>

      <details>
        <summary class="uppercase-tag">Internal Supplies</summary>
        <pre class="debug-dump mono">{JSON.stringify(supplies, null, 2)}</pre>
      </details>

      <div class="analog-row" style={{ marginTop: "8px" }}>
        <label>Auto-calibrate channel</label>
        <select class="input" value={String(calChannel)} onChange={(e) => setCalChannel(parseInt((e.currentTarget as HTMLSelectElement).value, 10))}>
          {[0, 1, 2, 3].map((ch) => <option key={ch} value={String(ch)}>CH {ch}</option>)}
        </select>
        <button class="btn" disabled={!mac || busy !== null} onClick={startCalibration}>
          {busy === "cal" ? "Starting..." : "Start Cal"}
        </button>
      </div>

      <div class="kv-row" style={{ marginTop: "8px" }}>
        <button class="btn" disabled={!mac || busy !== null} onClick={resetDevice}>
          {busy === "reset" ? "Resetting..." : "Device Reset"}
        </button>
      </div>
      {status && <div class="text-dim">{status}</div>}
    </GlassCard>
  );
}

function DebugCard() {
  const dbg = useInterval(() => api.debug(), 2000);
  return (
    <GlassCard title="Debug (raw)">
      <pre class="debug-dump mono">
        {JSON.stringify(dbg, null, 2)}
      </pre>
    </GlassCard>
  );
}

function DesktopOnlyCard() {
  return (
    <GlassCard title="Desktop-Only / Transport-Limited">
      <div class="kv-row">
        <span class="uppercase-tag">Logic Analyzer Stream</span>
        <span class="text-dim">USB vendor-bulk only (desktop app)</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Scope Recording / Export</span>
        <span class="text-dim">Desktop workflow (file picker + BBSC/CSV export)</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Calibration Deep Flows</span>
        <span class="text-dim">Partially exposed over HTTP; advanced path remains desktop</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Guidance</span>
        <span class="mono">Use desktop app for USB-only flows</span>
      </div>
    </GlassCard>
  );
}

export function System() {
  useEffect(() => startSelftestStatusPolling(), []);

  return (
    <div class="tab-stack">
      <BoardCard />
      <HatCard />
      <UsbPdCard />
      <UartCard />
      <IoExpControlCard />
      <WifiCard />
      <FaultsCard />
      <SelftestServiceCard />
      <DebugCard />
      <DesktopOnlyCard />
    </div>
  );
}
