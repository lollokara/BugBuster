import { useState, useEffect } from "preact/hooks";
import { api } from "../../api/client";

interface HatLogsPanelProps {
  mac: string | null;
  busy: string | null;
  setBusy: (busy: string | null) => void;
  setStatusText: (status: string | null) => void;
}

export function HatLogsPanel({
  mac,
  busy,
  setBusy,
  setStatusText,
}: HatLogsPanelProps) {
  const [laLogEnabled, setLaLogEnabled] = useState<boolean>(false);
  const [laLogLines, setLaLogLines] = useState<string[]>([]);
  const [laLogError, setLaLogError] = useState<string | null>(null);

  useEffect(() => {
    if (!laLogEnabled) return;
    const interval = setInterval(async () => {
      try {
        const res = await api.hatV2LaLogPoll();
        if (res?.lines && res.lines.length > 0) {
          setLaLogLines(prev => {
            const combined = [...prev, ...res.lines];
            return combined.length > 200 ? combined.slice(combined.length - 200) : combined;
          });
        }
      } catch(e) {
        setLaLogError(e instanceof Error ? e.message : "Failed to poll logs");
      }
    }, 500);
    return () => clearInterval(interval);
  }, [laLogEnabled]);

  return (
    <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px", marginTop: "16px" }}>
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", marginBottom: "8px" }}>
        <div class="card-title" style={{ fontSize: "10px" }}>RP2040 Debug Logs</div>
        <div style={{ display: "flex", gap: "8px", alignItems: "center" }}>
          <button
            class="btn"
            style={{ fontSize: "10px", padding: "2px 10px",
              background: laLogEnabled ? "rgba(16,185,129,0.15)" : "var(--glass)",
              color: laLogEnabled ? "#10b981" : "var(--text)",
              borderColor: laLogEnabled ? "#10b98150" : "var(--border-bright)" }}
            onClick={async () => {
              const newVal = !laLogEnabled;
              setBusy("lalog");
              try {
                setLaLogError(null);
                await api.hatV2LaLogEnable(mac!, newVal);
                setLaLogEnabled(newVal);
              } catch(e) {
                setStatusText(e instanceof Error ? e.message : "Failed to toggle log relay");
              }
              setBusy(null);
            }}
            disabled={busy === "lalog"}
          >
            {laLogEnabled ? "Enabled" : "Disabled"}
          </button>
          <button
            class="btn"
            style={{ fontSize: "10px", padding: "2px 10px" }}
            onClick={() => setLaLogLines([])}
          >
            Clear
          </button>
        </div>
      </div>
      <div style={{ fontSize: "9px", color: laLogEnabled ? "#10b981" : "var(--text-dim)", marginBottom: "6px" }}>
        Relay {laLogEnabled ? "enabled" : "disabled"}. Showing last {laLogLines.length}/200 buffered lines.
        {laLogError ? <span style={{ color: "#f59e0b", marginLeft: "8px" }}>Poll error: {laLogError}</span> : null}
      </div>
      <pre style={{ fontSize: "10px", fontFamily: "'JetBrains Mono', monospace", background: "var(--bg2)", borderRadius: "4px", padding: "8px", maxHeight: "180px", overflowY: "auto", whiteSpace: "pre-wrap", wordBreak: "break-all", color: "var(--text-dim)", margin: 0 }}>
        {laLogLines.length === 0 ? (laLogEnabled ? "(waiting for RP2040 log output…)" : "(log relay disabled)") : laLogLines.join("\n")}
      </pre>
    </div>
  );
}
