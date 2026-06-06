import { api, PairingRequiredError } from "../../api/client";

interface HatRoutingPanelProps {
  mac: string | null;
  la: any;
  resolvedRoute: number;
  setLaRouteSig: (route: number | null) => void;
  targetDetected: boolean;
  targetDpidr: number;
  setBusy: (busy: string | null) => void;
  setStatusText: (status: string | null) => void;
}

export function HatRoutingPanel({
  mac,
  la,
  resolvedRoute,
  setLaRouteSig,
  targetDetected,
  targetDpidr,
  setBusy,
  setStatusText,
}: HatRoutingPanelProps) {
  return (
    <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px" }}>
      <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>Routing & SWD</div>

      <div style={{ marginBottom: "10px", padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
        <div style={{ display: "flex", justifyContent: "space-between", fontSize: "11px", color: "var(--text-dim)", marginBottom: "4px" }}>
          <span>LA Capture Status:</span>
          <span class="mono" style={{ color: "var(--text)" }}>
            {la?.stateName ?? "—"} ({la?.samplesCaptured ?? 0}/{la?.totalSamples ?? 0})
          </span>
        </div>
      </div>

      {/* Route selector */}
      <div style={{ padding: "8px", borderRadius: "6px", background: "var(--bg2)", marginBottom: "10px" }}>
        <span style={{ fontSize: "11px", fontWeight: "700", display: "block", marginBottom: "6px" }}>LA Route Selector</span>
        <div style={{ display: "flex", gap: "6px" }}>
          <button
            class="btn"
            style={{ fontSize: "10px", padding: "4px 8px", flex: 1, background: resolvedRoute === 0 ? "rgba(59,130,246,0.15)" : "var(--glass)", color: resolvedRoute === 0 ? "#3b82f6" : "var(--text)", borderColor: resolvedRoute === 0 ? "#3b82f650" : "var(--border-bright)" }}
            onClick={async () => {
              try {
                const res = await api.hatV2SetLaRoute(mac!, 0);
                if (res && res.ok) setLaRouteSig(0);
              } catch(e) {
                if (!(e instanceof PairingRequiredError)) {
                  setStatusText(e instanceof Error ? e.message : "Command failed");
                }
              }
            }}
          >
            Low-Speed (Conn2)
          </button>
          <button
            class="btn"
            style={{ fontSize: "10px", padding: "4px 8px", flex: 1, background: resolvedRoute === 1 ? "rgba(59,130,246,0.15)" : "var(--glass)", color: resolvedRoute === 1 ? "#3b82f6" : "var(--text)", borderColor: resolvedRoute === 1 ? "#3b82f650" : "var(--border-bright)" }}
            onClick={async () => {
              try {
                const res = await api.hatV2SetLaRoute(mac!, 1);
                if (res && res.ok) setLaRouteSig(1);
              } catch(e) {
                if (!(e instanceof PairingRequiredError)) {
                  setStatusText(e instanceof Error ? e.message : "Command failed");
                }
              }
            }}
          >
            High-Speed (Conn1)
          </button>
        </div>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", marginTop: "4px" }}>
          {resolvedRoute === 0 ? "EXP_EXT pins (up to 4 channels @ 1MHz max)." : "Low-skew buffered Conn1 connector (max 3 channels)."}
        </div>
      </div>

      {/* SWD setup */}
      <div style={{ padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
        <span style={{ fontSize: "11px", fontWeight: "700", display: "block", marginBottom: "6px" }}>SWD Target (Dedicated Header)</span>
        <div style={{ display: "flex", alignSelf: "center", gap: "6px", marginBottom: "6px", fontSize: "11px" }}>
          <div style={{ width: 8, height: 8, borderRadius: "50%", background: targetDetected ? "#f59e0b" : "var(--text-muted)", boxShadow: targetDetected ? "0 0 6px #f59e0b" : "none", alignSelf: "center" }} />
          <span>
            {targetDetected ? `Target: DPIDR 0x${targetDpidr.toString(16).toUpperCase().padStart(8, "0")}` : "No target detected"}
          </span>
        </div>
        <div style={{ display: "flex", gap: "6px" }}>
          <button
            class="btn"
            style={{ fontSize: "10px", padding: "4px 8px", background: "rgba(139,92,246,0.12)", color: "#a855f7", borderColor: "#a855f750" }}
            onClick={async () => {
              setBusy("swd");
              try {
                await api.hatV2SetupSwd(mac!, 3300, 0);
                setStatusText("SWD target setup at 3.3V requested");
              } catch(e) {
                if (!(e instanceof PairingRequiredError)) {
                  setStatusText(e instanceof Error ? e.message : "Command failed");
                }
              }
              setBusy(null);
            }}
          >
            Setup SWD 3.3V
          </button>
          <button
            class="btn"
            style={{ fontSize: "10px", padding: "4px 8px", background: "rgba(139,92,246,0.12)", color: "#a855f7", borderColor: "#a855f750" }}
            onClick={async () => {
              setBusy("swd");
              try {
                await api.hatV2SetupSwd(mac!, 1800, 0);
                setStatusText("SWD target setup at 1.8V requested");
              } catch(e) {
                if (!(e instanceof PairingRequiredError)) {
                  setStatusText(e instanceof Error ? e.message : "Command failed");
                }
              }
              setBusy(null);
            }}
          >
            Setup SWD 1.8V
          </button>
        </div>
      </div>
    </div>
  );
}
