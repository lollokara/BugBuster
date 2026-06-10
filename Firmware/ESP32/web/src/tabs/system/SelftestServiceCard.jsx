// =============================================================================
// SelftestServiceCard — Selftest summary, supply probes, calibration, device reset
// =============================================================================
import { useEffect, useState } from "preact/hooks";
import { usePoll as useInterval } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { Led } from "../../components/Led";
import { api, PairingRequiredError } from "../../api/client";
import { deviceMac, selftestWorkerEnabled, supplyMonitorActive, setSelftestStatus, } from "../../state/signals";
export function SelftestServiceCard() {
    const mac = deviceMac.value;
    const summary = useInterval(() => api.selftest(), 3000);
    const suppliesCached = useInterval(() => api.selftestSuppliesCached(), 2000);
    const [railValues, setRailValues] = useState({});
    const [calChannel, setCalChannel] = useState(0);
    const [busy, setBusy] = useState(null);
    const [status, setStatus] = useState(null);
    useEffect(() => {
        if (summary)
            setSelftestStatus(summary);
    }, [summary]);
    const probeRail = async (rail) => {
        setBusy(`probe${rail}`);
        setStatus(null);
        try {
            const r = await api.selftestSupply(rail);
            const v = Number(r?.voltage ?? NaN);
            if (Number.isFinite(v)) {
                setRailValues((prev) => ({ ...prev, [rail]: v }));
            }
        }
        catch (e) {
            setStatus(e instanceof Error ? e.message : String(e));
        }
        finally {
            setBusy(null);
        }
    };
    const startCalibration = async () => {
        if (!mac)
            return;
        setBusy("cal");
        setStatus(null);
        try {
            const r = await api.selftestCalibrate(mac, calChannel);
            const points = Number(r?.points ?? 0);
            const err = Number(r?.errorMv ?? NaN);
            setStatus(`Calibration started (ch=${calChannel}, points=${points}, error=${Number.isFinite(err) ? err.toFixed(1) : "?"}mV)`);
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatus(e instanceof Error ? e.message : String(e));
            }
        }
        finally {
            setBusy(null);
        }
    };
    const resetDevice = async () => {
        if (!mac)
            return;
        if (!window.confirm("Reset the device now? The web session will disconnect briefly."))
            return;
        setBusy("reset");
        setStatus(null);
        try {
            await api.deviceReset(mac);
            setStatus("Reset command sent.");
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatus(e instanceof Error ? e.message : String(e));
            }
        }
        finally {
            setBusy(null);
        }
    };
    const toggleWorker = async () => {
        if (!mac)
            return;
        setBusy("worker");
        setStatus(null);
        try {
            setSelftestStatus(await api.selftestWorker(mac, !selftestWorkerEnabled.value));
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatus(e instanceof Error ? e.message : String(e));
            }
        }
        finally {
            setBusy(null);
        }
    };
    const cal = summary?.calibration ?? {};
    const boot = summary?.boot ?? {};
    return (<GlassCard title="Selftest / Service">
      <div class="kv-row"><span class="uppercase-tag">Boot Selftest</span><span class="mono">{boot?.ran ? (boot?.passed ? "PASS" : "FAIL") : "N/A"}</span></div>
      <div class="kv-row"><span class="uppercase-tag">Cal Status</span><span class="mono">{String(cal?.status ?? "—")}</span></div>
      <div class="kv-row"><span class="uppercase-tag">Cal Error</span><span class="mono">{Number.isFinite(Number(cal?.errorMv)) ? `${Number(cal.errorMv).toFixed(1)} mV` : "—"}</span></div>
      <div class="kv-row">
        <span class="uppercase-tag">Supply monitor (opt-in)</span>
        <button class={"pill" + (selftestWorkerEnabled.value ? " active" : "")} disabled={!mac || busy !== null} onClick={toggleWorker}>
          {busy === "worker" ? "..." : selftestWorkerEnabled.value ? "ON" : "OFF"}
        </button>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Monitor Active</span>
        <Led state={supplyMonitorActive.value ? "on" : "off"} label={supplyMonitorActive.value ? "Active" : "Idle"}/>
      </div>

      <details>
        <summary class="uppercase-tag">Supply Probes</summary>
        <div class="kv-row" style={{ gap: "8px", marginTop: "8px" }}>
          <button class="btn" disabled={busy !== null} onClick={() => probeRail(0)}>Probe VADJ1</button>
          <button class="btn" disabled={busy !== null} onClick={() => probeRail(1)}>Probe VADJ2</button>
          <button class="btn" disabled={busy !== null} onClick={() => probeRail(2)}>Probe 3V3</button>
        </div>
        <div class="kv-row"><span class="uppercase-tag">VADJ1</span><span class="mono">{Number.isFinite(railValues[0]) ? `${railValues[0].toFixed(3)} V` : "—"}</span></div>
        <div class="kv-row"><span class="uppercase-tag">VADJ2</span><span class="mono">{Number.isFinite(railValues[1]) ? `${railValues[1].toFixed(3)} V` : "—"}</span></div>
        <div class="kv-row"><span class="uppercase-tag">3V3_ADJ</span><span class="mono">{Number.isFinite(railValues[2]) ? `${railValues[2].toFixed(3)} V` : "—"}</span></div>
        <div class="uppercase-tag" style={{ marginTop: "10px", marginBottom: "4px" }}>Live cache</div>
        {suppliesCached && !suppliesCached.available && (<div class="text-dim" style={{ color: "#f59e0b", marginBottom: "4px" }}>interlock blocked</div>)}
        {(suppliesCached?.rails ?? []).map((r) => (<div class="kv-row" key={r.rail} style={{ opacity: suppliesCached?.available === false ? 0.5 : 1 }}>
            <span class="uppercase-tag">{r.name}</span>
            <span class="mono">
              {r.voltageV < 0 ? <span class="text-dim">disabled</span> : `${r.voltageV.toFixed(3)} V`}
            </span>
          </div>))}
        {!suppliesCached && <div class="text-dim" style={{ fontSize: "11px" }}>—</div>}
      </details>

      <details>
        <summary class="uppercase-tag">Internal Supplies</summary>
        <pre class="debug-dump mono">{JSON.stringify(suppliesCached, null, 2)}</pre>
      </details>

      <div class="analog-row" style={{ marginTop: "8px" }}>
        <label>Auto-calibrate channel</label>
        <select class="input" value={String(calChannel)} onChange={(e) => setCalChannel(parseInt(e.currentTarget.value, 10))}>
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
    </GlassCard>);
}
