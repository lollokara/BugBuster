// =============================================================================
// SystemHero — compact device status strip at the top of the System tab
// =============================================================================
import { usePoll as useInterval } from "../../hooks/usePoll";
import { api } from "../../api/client";
import { deviceInfo, deviceMac, boardState } from "../../state/signals";
export function SystemHero() {
    const info = deviceInfo.value;
    const mac = deviceMac.value;
    const board = boardState.value;
    // Live fault count for the hero strip
    const faults = useInterval(() => api.faults(), 5000);
    // HAT presence
    const hat = useInterval(() => api.hat(), 4000);
    const faultCount = Array.isArray(faults?.faults) ? faults.faults.length : 0;
    const hatDetected = hat?.detected ?? hat?.present ?? false;
    const pills = [
        {
            label: "MAC",
            value: mac ? mac.toUpperCase() : "—",
        },
        {
            label: "SPI",
            value: info?.spiOk === true ? "OK" : info?.spiOk === false ? "FAIL" : "—",
            color: info?.spiOk === false ? "#ef4444" : info?.spiOk === true ? "#10b981" : undefined,
        },
        {
            label: "HAT",
            value: hatDetected ? "Connected" : "Absent",
            color: hatDetected ? "#10b981" : "var(--text-muted)",
        },
        {
            label: "Faults",
            value: String(faultCount),
            color: faultCount > 0 ? "#f59e0b" : "var(--text-muted)",
        },
        {
            label: "Board",
            value: board?.active ?? "—",
        },
    ];
    return (<div style={{
            display: "flex",
            flexWrap: "wrap",
            gap: "8px",
            padding: "10px 12px",
            background: "rgba(6,10,20,0.45)",
            border: "1px solid var(--border)",
            borderRadius: "8px",
            marginBottom: "16px",
        }}>
      {pills.map(p => (<div key={p.label} style={{
                display: "flex",
                flexDirection: "column",
                alignItems: "flex-start",
                minWidth: "80px",
                padding: "4px 8px",
                borderRadius: "5px",
                background: "var(--bg2)",
            }}>
          <span style={{ fontSize: "8px", color: "var(--text-dim)", textTransform: "uppercase", letterSpacing: "0.06em", marginBottom: "2px" }}>
            {p.label}
          </span>
          <span class="mono" style={{ fontSize: "11px", fontWeight: "600", color: p.color ?? "var(--text)" }}>
            {p.value}
          </span>
        </div>))}
    </div>);
}
