import { useState, useEffect } from "preact/hooks";
export function HatPowerRailsPanel({ rails, calActive, busy, toggleRail, setRailVoltage, railStatusMeta, }) {
    const railAdj = rails.find(r => r.railId === 0);
    const railV3 = rails.find(r => r.railId === 1);
    const railV4 = rails.find(r => r.railId === 2);
    const adjEn = railAdj?.enabled || false;
    const v3En = railV3?.enabled || false;
    const v4En = railV4?.enabled || false;
    const adjMv = railAdj?.voltageMv || 0;
    const v3Mv = railV3?.voltageMv || 0;
    const v4Mv = railV4?.voltageMv || 0;
    const v3Ma = railV3?.currentMa || 0;
    const v4Ma = railV4?.currentMa || 0;
    const adjStatus = railStatusMeta(railAdj);
    const v3Status = railStatusMeta(railV3);
    const v4Status = railStatusMeta(railV4);
    const [v3v3TargetMv, setV3v3TargetMv] = useState(3300);
    const [vadj3TargetMv, setVadj3TargetMv] = useState(3300);
    const [vadj4TargetMv, setVadj4TargetMv] = useState(3300);
    // Sync target inputs to actual rail voltage if it changes (when enabled)
    useEffect(() => {
        if (adjEn && adjMv > 0)
            setV3v3TargetMv(adjMv);
    }, [adjMv, adjEn]);
    useEffect(() => {
        if (v3En && v3Mv > 0)
            setVadj3TargetMv(v3Mv);
    }, [v3Mv, v3En]);
    useEffect(() => {
        if (v4En && v4Mv > 0)
            setVadj4TargetMv(v4Mv);
    }, [v4Mv, v4En]);
    return (<div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px" }}>
      <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>Power Rails</div>

      {/* 3V3_ADJ Rail */}
      <div style={{ marginBottom: "10px", padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "6px" }}>
          <span style={{ fontSize: "11px", fontWeight: "700", color: "#10b981" }}>3V3_ADJ Rail (1.7–5.0V)</span>
          <span class="mono" style={{ fontSize: "8px", color: adjStatus.color }}>{adjStatus.label}</span>
          <button class="btn" style={{ fontSize: "10px", padding: "2px 8px", background: adjEn ? "rgba(16,185,129,0.15)" : "var(--glass)", color: adjEn ? "#10b981" : "var(--text)", borderColor: adjEn ? "#10b98150" : "var(--border-bright)" }} onClick={() => toggleRail(0, !adjEn, "rail0")} disabled={calActive || busy === "rail0"}>
            {adjEn ? "ON" : "OFF"}
          </button>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr", gap: "6px", alignItems: "start", marginBottom: "8px" }}>
          <div>
            <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>
              {adjEn ? "Voltage" : "Preview Voltage"}
            </div>
            <span class="mono" style={{ fontSize: "11px", fontWeight: "600", color: "#10b981" }}>
              {adjEn
            ? `${(adjMv / 1000.0).toFixed(3)} V`
            : `${(v3v3TargetMv / 1000.0).toFixed(2)} V (Preview)`}
            </span>
          </div>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr auto", gap: "8px", alignItems: "center" }}>
          <input type="range" min="1700" max="5000" step="100" value={v3v3TargetMv} onInput={(e) => setV3v3TargetMv(Number(e.currentTarget.value) || 0)} disabled={calActive}/>
          <div style={{ display: "flex", alignItems: "center", gap: "8px" }}>
            <span class="mono" style={{ fontSize: "10px", color: "var(--text-dim)", minWidth: "50px", textAlign: "right" }}>
              {(v3v3TargetMv / 1000.0).toFixed(2)} V
            </span>
            <button class="btn" style={{ fontSize: "10px", padding: "4px 10px" }} onClick={() => setRailVoltage(0, v3v3TargetMv, "v3v3-voltage")} disabled={calActive || busy === "v3v3-voltage"}>
              Confirm
            </button>
          </div>
        </div>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginTop: "6px" }}>
          Required for level shifter Outputs Enable (OE). Hard interlocked.
        </div>
      </div>

      {/* VADJ3 Rail */}
      <div style={{ marginBottom: "10px", padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "6px" }}>
          <span style={{ fontSize: "11px", fontWeight: "700", color: "#06b6d4" }}>VADJ3 Rail (1.8–36V)</span>
          <span class="mono" style={{ fontSize: "8px", color: v3Status.color }}>{v3Status.label}</span>
          <button class="btn" style={{ fontSize: "10px", padding: "2px 8px", background: v3En ? "rgba(16,185,129,0.15)" : "var(--glass)", color: v3En ? "#10b981" : "var(--text)", borderColor: v3En ? "#10b98150" : "var(--border-bright)" }} onClick={() => toggleRail(1, !v3En, "rail1")} disabled={calActive || busy === "rail1"}>
            {v3En ? "ON" : "OFF"}
          </button>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "6px", alignItems: "center" }}>
          <div>
            <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>
              {v3En ? "Voltage" : "Preview Voltage"}
            </div>
            <span class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>
              {v3En
            ? `${(v3Mv / 1000.0).toFixed(3)} V`
            : `${(vadj3TargetMv / 1000.0).toFixed(2)} V (Preview)`}
            </span>
          </div>
          <div>
            <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>Current</div>
            <span class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>
              {v3Ma} mA
            </span>
          </div>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr auto", gap: "8px", alignItems: "center", marginTop: "8px" }}>
          <input type="range" min="0" max="36000" step="100" value={vadj3TargetMv} onInput={(e) => setVadj3TargetMv(Number(e.currentTarget.value) || 0)} disabled={calActive}/>
          <div style={{ display: "flex", alignItems: "center", gap: "8px" }}>
            <span class="mono" style={{ fontSize: "10px", color: "var(--text-dim)", minWidth: "72px", textAlign: "right" }}>
              {(vadj3TargetMv / 1000.0).toFixed(2)} V
            </span>
            <button class="btn" style={{ fontSize: "10px", padding: "4px 10px" }} onClick={() => setRailVoltage(1, vadj3TargetMv, "vadj3-voltage")} disabled={calActive || busy === "vadj3-voltage"}>
              Confirm
            </button>
          </div>
        </div>
      </div>

      {/* VADJ4 Rail */}
      <div style={{ padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "6px" }}>
          <span style={{ fontSize: "11px", fontWeight: "700", color: "#ff4d6a" }}>VADJ4 Rail (1.8–36V)</span>
          <span class="mono" style={{ fontSize: "8px", color: v4Status.color }}>{v4Status.label}</span>
          <button class="btn" style={{ fontSize: "10px", padding: "2px 8px", background: v4En ? "rgba(16,185,129,0.15)" : "var(--glass)", color: v4En ? "#10b981" : "var(--text)", borderColor: v4En ? "#10b98150" : "var(--border-bright)" }} onClick={() => toggleRail(2, !v4En, "rail2")} disabled={calActive || busy === "rail2"}>
            {v4En ? "ON" : "OFF"}
          </button>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "8px" }}>
          <div>
            <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>
              {v4En ? "Voltage" : "Preview Voltage"}
            </div>
            <span class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>
              {v4En
            ? `${(v4Mv / 1000.0).toFixed(3)} V`
            : `${(vadj4TargetMv / 1000.0).toFixed(2)} V (Preview)`}
            </span>
          </div>
          <div>
            <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>Current</div>
            <span class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>
              {v4Ma} mA
            </span>
          </div>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr auto", gap: "8px", alignItems: "center", marginTop: "8px" }}>
          <input type="range" min="0" max="36000" step="100" value={vadj4TargetMv} onInput={(e) => setVadj4TargetMv(Number(e.currentTarget.value) || 0)} disabled={calActive}/>
          <div style={{ display: "flex", alignItems: "center", gap: "8px" }}>
            <span class="mono" style={{ fontSize: "10px", color: "var(--text-dim)", minWidth: "72px", textAlign: "right" }}>
              {(vadj4TargetMv / 1000.0).toFixed(2)} V
            </span>
            <button class="btn" style={{ fontSize: "10px", padding: "4px 10px" }} onClick={() => setRailVoltage(2, vadj4TargetMv, "vadj4-voltage")} disabled={calActive || busy === "vadj4-voltage"}>
              Confirm
            </button>
          </div>
        </div>
      </div>
    </div>);
}
