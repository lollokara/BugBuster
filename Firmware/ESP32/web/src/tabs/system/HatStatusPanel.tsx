
interface HatStatusPanelProps {
  detected: boolean;
  connected: boolean;
  degraded: boolean;
  dapConnected: boolean;
  targetDetected: boolean;
  hwRevision: string | number;
  version: string;
}

export function HatStatusPanel({
  detected,
  connected,
  degraded,
  dapConnected,
  targetDetected,
  hwRevision,
  version,
}: HatStatusPanelProps) {
  return (
    <div style={{ display: "grid", gridTemplateColumns: "repeat(6, 1fr)", gap: "10px", marginBottom: "12px" }}>
      <div style={{ textAlign: "center", padding: "6px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginBottom: "3px" }}>Detected</div>
        <div style={{ width: 10, height: 10, borderRadius: "50%", display: "inline-block", background: detected ? "#10b981" : "var(--text-muted)", boxShadow: detected ? "0 0 6px #10b981" : "none" }} />
      </div>
      <div style={{ textAlign: "center", padding: "6px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginBottom: "3px" }}>UART</div>
        <div style={{ width: 10, height: 10, borderRadius: "50%", display: "inline-block", background: connected && !degraded ? "#3b82f6" : degraded ? "#f59e0b" : "var(--text-muted)", boxShadow: connected && !degraded ? "0 0 6px #3b82f6" : degraded ? "0 0 6px #f59e0b" : "none" }} />
      </div>
      <div style={{ textAlign: "center", padding: "6px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginBottom: "3px" }}>DAP</div>
        <div style={{ width: 10, height: 10, borderRadius: "50%", display: "inline-block", background: dapConnected ? "#8b5cf6" : "var(--text-muted)", boxShadow: dapConnected ? "0 0 6px #8b5cf6" : "none" }} />
      </div>
      <div style={{ textAlign: "center", padding: "6px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginBottom: "3px" }}>Target</div>
        <div style={{ width: 10, height: 10, borderRadius: "50%", display: "inline-block", background: targetDetected ? "#f59e0b" : "var(--text-muted)", boxShadow: targetDetected ? "0 0 6px #f59e0b" : "none" }} />
      </div>
      <div style={{ textAlign: "center", padding: "6px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginBottom: "3px" }}>Revision</div>
        <div class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>{hwRevision !== "-" ? `v${hwRevision}` : "-"}</div>
      </div>
      <div style={{ textAlign: "center", padding: "6px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginBottom: "3px" }}>Firmware</div>
        <div class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>v{version}</div>
      </div>
    </div>
  );
}
