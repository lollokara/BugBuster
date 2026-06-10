import { useEffect, useState } from "preact/hooks";
import { api } from "../../api/client";
const RAIL_NAMES = {
    0: "VADJ1",
    1: "VADJ2",
    2: "3V3_ADJ",
    3: "VADJ3",
    4: "VADJ4",
};
function voltageColor(mv) {
    if (mv <= 0)
        return "var(--text-dim)";
    if (mv > 15000)
        return "var(--rose)";
    return "var(--green)";
}
function RailRow({ label, voltageMv, currentMa }) {
    const v = (voltageMv / 1000).toFixed(3);
    return (<div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "6px 0", borderBottom: "1px solid var(--border)" }}>
      <span style={{ fontSize: "0.8rem", color: "var(--text-dim)", fontFamily: "monospace" }}>{label}</span>
      <div style={{ display: "flex", gap: "12px", alignItems: "center" }}>
        {currentMa !== undefined && (<span style={{ fontSize: "0.75rem", color: "var(--text-dim)" }}>{currentMa.toFixed(0)} mA</span>)}
        <span style={{ fontFamily: "monospace", fontWeight: 600, color: voltageColor(voltageMv) }}>
          {voltageMv > 0 ? `${v} V` : "—"}
        </span>
      </div>
    </div>);
}
export function Voltages() {
    const [overviewRails, setOverviewRails] = useState([]);
    const [idacChannels, setIdacChannels] = useState([]);
    const [hatRails, setHatRails] = useState([]);
    const [hatDetected, setHatDetected] = useState(false);
    const [loading, setLoading] = useState(true);
    const refresh = async () => {
        try {
            const [ov, idac] = await Promise.all([api.overview(), api.idac()]);
            // Supply rails from overview
            const rails = (ov.rails ?? []).map((r) => ({
                name: r.name ?? `Rail ${r.id}`,
                voltageMv: Math.round((r.voltage ?? 0) * 1000),
            }));
            setOverviewRails(rails);
            // IDAC channels
            const channels = (idac.channels ?? []).map((c, i) => ({
                ch: i,
                code: c.code ?? 0,
                voltage: c.voltage ?? 0,
                enabled: c.enabled ?? false,
            }));
            setIdacChannels(channels);
            // HAT rails (optional)
            try {
                const caps = await api.hatV2Caps();
                if (caps && caps.railCount > 0) {
                    setHatDetected(true);
                    const hr = await api.hatV2Rails();
                    setHatRails(hr.rails ?? []);
                }
            }
            catch {
                setHatDetected(false);
            }
        }
        catch (e) {
            console.warn("voltages fetch failed", e);
        }
        finally {
            setLoading(false);
        }
    };
    useEffect(() => {
        refresh();
        const id = setInterval(refresh, 2000);
        return () => clearInterval(id);
    }, []);
    if (loading) {
        return <div style={{ padding: "24px", color: "var(--text-dim)", textAlign: "center" }}>Loading…</div>;
    }
    return (<div class="tab-stack" style={{ gap: "12px" }}>
      {/* Supply Rails */}
      <div class="card">
        <div class="card-header">
          <span class="card-title">Supply Rails</span>
        </div>
        <div style={{ padding: "0 4px" }}>
          {overviewRails.length === 0 ? (<div style={{ color: "var(--text-dim)", fontSize: "0.8rem", padding: "8px 0" }}>No supply rail data</div>) : overviewRails.map(r => (<RailRow key={r.name} label={r.name} voltageMv={r.voltageMv}/>))}
        </div>
      </div>

      {/* IDAC Channels */}
      {idacChannels.length > 0 && (<div class="card">
          <div class="card-header">
            <span class="card-title">IDAC Channels (DS4424)</span>
          </div>
          <div style={{ padding: "0 4px" }}>
            {idacChannels.map(c => (<RailRow key={c.ch} label={`CH ${c.ch}  (code ${c.code > 0 ? "+" : ""}${c.code})`} voltageMv={Math.round(c.voltage * 1000)}/>))}
          </div>
        </div>)}

      {/* HAT Rails */}
      {hatDetected && hatRails.length > 0 && (<div class="card">
          <div class="card-header">
            <span class="card-title">HAT Power Rails</span>
          </div>
          <div style={{ padding: "0 4px" }}>
            {hatRails.map(r => (<RailRow key={r.railId} label={RAIL_NAMES[r.railId] ?? `Rail ${r.railId}`} voltageMv={r.voltageMv} currentMa={r.currentMa}/>))}
          </div>
        </div>)}
    </div>);
}
