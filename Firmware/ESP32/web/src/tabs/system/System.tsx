// =============================================================================
// System tab — board profile, HAT, USB-PD, UART, WiFi, faults, raw debug.
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { Led } from "../../components/Led";
import {
  api,
  PairingRequiredError,
  type BoardProfile,
  type BoardState,
} from "../../api/client";
import { boardState, deviceMac } from "../../state/signals";

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
    return () => { alive = false; };
  }, []);
  return value;
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
    return () => { alive = false; };
  }, []);

  const state = boardState.value;
  const active: BoardProfile | undefined = state?.available.find(
    (b) => b.id === state.active,
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
          onChange={(e) => setSelected((e.currentTarget as HTMLSelectElement).value || null)}
        >
          <option value="">(none)</option>
          {state?.available.map((b) => (
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
                  <td>{rail.locked ? <Led state="warn" label="locked" /> : <Led state="off" label="free" />}</td>
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
  const hat = useInterval(() => api.hat(), 2000);
  const la = useInterval(() => api.hatLaStatus(), 1000) as any;

  const present = !!hat?.present;
  const type = hat?.type ?? "—";
  const version = hat?.version ?? "—";
  const laArmed = !!la?.armed;
  const laTriggered = !!la?.triggered;
  const laFilled = !!la?.filled;

  return (
    <GlassCard title="HAT">
      <div class="kv-row">
        <span class="uppercase-tag">Present</span>
        <Led state={present ? "on" : "off"} label={present ? "Yes" : "No"} />
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Type</span>
        <span class="mono">{String(type)}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Version</span>
        <span class="mono">{String(version)}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">LA</span>
        <Led state={laArmed ? "warn" : "off"} label={laArmed ? "armed" : "idle"} />
        <Led state={laTriggered ? "on" : "off"} label="trig" />
        <Led state={laFilled ? "on" : "off"} label="full" />
      </div>
    </GlassCard>
  );
}

function UsbPdCard() {
  const pd = useInterval(() => api.usbpd(), 2000) as any;
  const pdos = Array.isArray(pd?.pdos) ? pd.pdos : Array.isArray(pd) ? pd : [];
  return (
    <GlassCard title="USB-PD">
      <table class="kv-table">
        <thead>
          <tr><th>#</th><th>Voltage</th><th>Current</th><th></th></tr>
        </thead>
        <tbody>
          {pdos.map((p: any, i: number) => (
            <tr key={i}>
              <td class="mono">{i}</td>
              <td class="mono">{Number(p.voltage ?? p.v ?? 0).toFixed(2)} V</td>
              <td class="mono">{Number(p.current ?? p.i ?? 0).toFixed(2)} A</td>
              <td>
                <button class="pill" disabled={!p.selectable}>Select</button>
              </td>
            </tr>
          ))}
          {pdos.length === 0 && (
            <tr><td colSpan={4} class="text-dim">No PDOs reported</td></tr>
          )}
        </tbody>
      </table>
    </GlassCard>
  );
}

function UartCard() {
  const cfg = useInterval(() => api.uartConfig(), 2000) as any;
  const [baud, setBaud] = useState<number>(115200);
  useEffect(() => {
    if (cfg?.baud) setBaud(Number(cfg.baud));
  }, [cfg]);
  return (
    <GlassCard title="UART Bridge">
      <div class="kv-row">
        <span class="uppercase-tag">Baud</span>
        <select
          class="input"
          value={String(baud)}
          onChange={(e) => setBaud(parseInt((e.currentTarget as HTMLSelectElement).value, 10))}
        >
          {[9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600].map((b) => (
            <option key={b} value={String(b)}>{b}</option>
          ))}
        </select>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Data bits</span>
        <span class="mono">{cfg?.dataBits ?? 8}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Parity</span>
        <span class="mono">{cfg?.parity ?? "none"}</span>
      </div>
    </GlassCard>
  );
}

function WifiCard() {
  const [nets, setNets] = useState<any[]>([]);
  const [scanning, setScanning] = useState(false);
  const [ssid, setSsid] = useState("");
  const [pass, setPass] = useState("");

  const scan = async () => {
    setScanning(true);
    try {
      const r = await api.wifi();
      setNets(Array.isArray(r?.networks) ? r.networks : Array.isArray(r) ? r : []);
    } catch {
      /* ignore */
    } finally {
      setScanning(false);
    }
  };

  return (
    <GlassCard title="WiFi">
      <div class="kv-row">
        <button class="btn" onClick={scan} disabled={scanning}>
          {scanning ? "Scanning…" : "Scan"}
        </button>
      </div>
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
        <button class="btn primary" disabled={!ssid}>Connect</button>
      </div>
    </GlassCard>
  );
}

function FaultsCard() {
  const faults = useInterval(() => api.faults(), 2000) as any;
  const list = Array.isArray(faults?.faults)
    ? faults.faults
    : Array.isArray(faults)
      ? faults
      : [];
  return (
    <GlassCard
      title="Faults"
      actions={<button class="btn" disabled={list.length === 0}>Clear all</button>}
    >
      {list.length === 0
        ? <div class="text-ok">No active faults</div>
        : (
          <ul class="fault-list">
            {list.map((f: any, i: number) => (
              <li key={i}>
                <Led state="err" />
                <span class="mono">{f.code ?? f.id ?? i}</span>
                <span>{f.message ?? String(f)}</span>
              </li>
            ))}
          </ul>
        )
      }
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

export function System() {
  return (
    <div class="tab-stack">
      <BoardCard />
      <HatCard />
      <UsbPdCard />
      <UartCard />
      <WifiCard />
      <FaultsCard />
      <DebugCard />
    </div>
  );
}
