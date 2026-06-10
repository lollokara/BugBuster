import { useState } from "preact/hooks";
import { api, PairingRequiredError } from "../../api/client";
export function HatIoBankPanel({ mac, busy, setBusy, setStatusText, }) {
    // Shifted IO Bank States
    const [ioDirs, setIoDirs] = useState(0);
    const [ioUps, setIoUps] = useState(0);
    const [ioDns, setIoDns] = useState(0);
    const [ioVals, setIoVals] = useState(0);
    // Level Shifter States
    const [lsOe, setLsOe] = useState(false);
    const [lsDir, setLsDir] = useState(false);
    return (<div style={{ display: "grid", gridTemplateColumns: "1.2fr 0.8fr", gap: "16px", marginBottom: "16px" }}>
      {/* Shifted I/O Bank */}
      <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px" }}>
        <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>Shifted I/O Bank Configuration (GPIO 10-15, 20-21)</div>

        <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "8px", marginBottom: "10px" }}>
          {Array.from({ length: 8 }).map((_, i) => {
            const isOut = (ioDirs & (1 << i)) !== 0;
            const isUp = (ioUps & (1 << i)) !== 0;
            const isDn = (ioDns & (1 << i)) !== 0;
            const isHigh = (ioVals & (1 << i)) !== 0;
            return (<div key={i} style={{ padding: "6px", borderRadius: "6px", background: "var(--bg2)", border: "1px solid var(--border)" }}>
                <div style={{ fontSize: "9px", fontWeight: "700", color: "#3b82f6", marginBottom: "3px" }}>SH_IO_{i + 1}</div>
                <div style={{ display: "flex", gap: "2px", marginBottom: "4px" }}>
                  <button class="btn" style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: !isOut ? "rgba(59,130,246,0.25)" : "var(--glass)", color: !isOut ? "#3b82f6" : "var(--text)", border: "none" }} onClick={() => setIoDirs(d => d & ~(1 << i))}>
                    IN
                  </button>
                  <button class="btn" style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: isOut ? "rgba(59,130,246,0.25)" : "var(--glass)", color: isOut ? "#3b82f6" : "var(--text)", border: "none" }} onClick={() => setIoDirs(d => d | (1 << i))}>
                    OUT
                  </button>
                </div>
                {/* Output value toggle — only relevant when pin is OUT */}
                {isOut && (<div style={{ display: "flex", gap: "2px", marginBottom: "4px" }}>
                    <button class="btn" style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: !isHigh ? "rgba(239,68,68,0.2)" : "var(--glass)", color: !isHigh ? "#ef4444" : "var(--text-dim)", border: "none" }} onClick={() => setIoVals(v => v & ~(1 << i))}>
                      LOW
                    </button>
                    <button class="btn" style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: isHigh ? "rgba(34,197,94,0.2)" : "var(--glass)", color: isHigh ? "#22c55e" : "var(--text-dim)", border: "none" }} onClick={() => setIoVals(v => v | (1 << i))}>
                      HIGH
                    </button>
                  </div>)}
                <div style={{ display: "flex", flexDirection: "column", gap: "2px" }}>
                  <label style={{ fontSize: "8px", display: "flex", alignItems: "center", gap: "2px", cursor: "pointer" }}>
                    <input type="checkbox" checked={isUp} onChange={(e) => {
                    const chk = e.currentTarget.checked;
                    if (chk) {
                        setIoUps(u => u | (1 << i));
                        setIoDns(d => d & ~(1 << i));
                    }
                    else {
                        setIoUps(u => u & ~(1 << i));
                    }
                }}/>
                    Pull-Up
                  </label>
                  <label style={{ fontSize: "8px", display: "flex", alignItems: "center", gap: "2px", cursor: "pointer" }}>
                    <input type="checkbox" checked={isDn} onChange={(e) => {
                    const chk = e.currentTarget.checked;
                    if (chk) {
                        setIoDns(d => d | (1 << i));
                        setIoUps(u => u & ~(1 << i));
                    }
                    else {
                        setIoDns(d => d & ~(1 << i));
                    }
                }}/>
                    Pull-Down
                  </label>
                </div>
              </div>);
        })}
        </div>

        <button class="btn primary" style={{ width: "100%", fontSize: "10px", padding: "4px" }} onClick={async () => {
            setBusy("io_bank");
            try {
                await api.hatV2SetIoBank(mac, ioDirs, ioUps, ioDns, ioVals);
                setStatusText("I/O bank configuration applied!");
            }
            catch (e) {
                if (!(e instanceof PairingRequiredError)) {
                    setStatusText(e instanceof Error ? e.message : "Command failed");
                }
            }
            setBusy(null);
        }} disabled={busy !== null}>
          Apply I/O Bank Config
        </button>
      </div>

      {/* Level Shifter Overrides */}
      <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px" }}>
        <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>Level Shifter Overrides</div>

        <div style={{ marginBottom: "8px", padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "4px" }}>
            <span style={{ fontSize: "10px", fontWeight: "700" }}>Outputs Enable (OE)</span>
            <button class="btn" style={{ fontSize: "9px", padding: "1px 6px", background: lsOe ? "rgba(239,68,68,0.15)" : "var(--glass)", color: lsOe ? "#ef4444" : "var(--text)", borderColor: lsOe ? "#ef444450" : "var(--border-bright)" }} onClick={async () => {
            const next = !lsOe;
            setBusy("ls");
            try {
                const res = await api.hatV2SetLevelShift(mac, next, lsDir);
                if (res && res.ok) {
                    setLsOe(res.oe);
                    setLsDir(res.dir);
                    setStatusText(res.oe ? "Outputs Enabled!" : "Outputs Tri-stated!");
                }
            }
            catch (e) {
                if (!(e instanceof PairingRequiredError)) {
                    setStatusText(e instanceof Error ? e.message : "Command failed");
                }
            }
            setBusy(null);
        }}>
              {lsOe ? "ACTIVE" : "TRI-STATE"}
            </button>
          </div>
          <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>
            OE requires the 3V3_ADJ rail to be enabled first. Interlocked.
          </div>
        </div>

        <div style={{ padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
          <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "4px" }}>
            <span style={{ fontSize: "10px", fontWeight: "700" }}>Direction (DIR)</span>
            <button class="btn" style={{ fontSize: "9px", padding: "1px 6px" }} onClick={async () => {
            const next = !lsDir;
            setBusy("ls");
            try {
                const res = await api.hatV2SetLevelShift(mac, lsOe, next);
                if (res && res.ok) {
                    setLsOe(res.oe);
                    setLsDir(res.dir);
                }
            }
            catch (e) {
                if (!(e instanceof PairingRequiredError)) {
                    setStatusText(e instanceof Error ? e.message : "Command failed");
                }
            }
            setBusy(null);
        }}>
              {lsDir ? "A → B (Output)" : "B → A (Input)"}
            </button>
          </div>
          <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>
            A → B drives target. B → A sets EXP_EXT as inputs.
          </div>
        </div>
      </div>
    </div>);
}
