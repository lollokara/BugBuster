// =============================================================================
// HatCard — HAT Expansion Board v2 status, rails, routing, IO bank, calibration
// =============================================================================
import { useEffect, useState } from "preact/hooks";
import { usePoll as useInterval } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { api, PairingRequiredError } from "../../api/client";
import { deviceMac } from "../../state/signals";
import { HatStatusPanel } from "./HatStatusPanel";
import { HatPowerRailsPanel } from "./HatPowerRailsPanel";
import { HatRoutingPanel } from "./HatRoutingPanel";
import { HatLogsPanel } from "./HatLogsPanel";
import { HatIoBankPanel } from "./HatIoBankPanel";
import { HatCalibrationPanel } from "./HatCalibrationPanel";
export function HatCard() {
    const mac = deviceMac.value;
    const hat = useInterval(() => api.hat(), 2000);
    const la = useInterval(() => api.hatLaStatus(), 1000);
    const rails = useInterval(() => {
        if (hat && (hat.detected ?? hat.present)) {
            return api.hatV2Rails();
        }
        return Promise.resolve(null);
    }, 2000);
    const [caps, setCaps] = useState(null);
    useEffect(() => {
        if (hat && (hat.detected ?? hat.present)) {
            api.hatV2Caps()
                .then(setCaps)
                .catch(err => console.warn("Failed to get HAT caps", err));
        }
        else {
            setCaps(null);
        }
    }, [hat?.detected, hat?.present]);
    const [busy, setBusy] = useState(null);
    const [statusText, setStatusText] = useState(null);
    const [localRails, setLocalRails] = useState(null);
    const [hatSeen, setHatSeen] = useState(false);
    const [calActive, setCalActive] = useState(false);
    const [laRouteSig, setLaRouteSig] = useState(null);
    useEffect(() => {
        if (rails?.rails)
            setLocalRails(null);
    }, [rails]);
    useEffect(() => {
        const present = hat?.detected ?? hat?.present;
        if (present === true)
            setHatSeen(true);
        if (present === false)
            setHatSeen(false);
    }, [hat]);
    const applyRailUpdate = (res) => {
        if (!res || res.railId == null)
            return;
        const base = localRails ?? railList;
        let matched = false;
        const update = {
            railId: res.railId,
            enabled: !!res.enabled,
            voltageMv: res.voltageMv ?? res.voltage_mv ?? 0,
            currentMa: res.currentMa ?? res.current_ma ?? 0,
            status: res.status ?? 0,
        };
        const updated = base.map((r) => {
            if (r.railId !== res.railId)
                return r;
            matched = true;
            return { ...r, ...update };
        });
        setLocalRails(matched ? updated : [...updated, update]);
    };
    const patchRailEnabled = (railId, enabled) => {
        const base = localRails ?? railList;
        let matched = false;
        const updated = base.map((r) => {
            if (r.railId !== railId)
                return r;
            matched = true;
            return { ...r, enabled };
        });
        setLocalRails(matched
            ? updated
            : [...updated, { railId, enabled, voltageMv: 0, currentMa: 0, status: 0 }]);
    };
    const toggleRail = async (railId, enabled, busyKey) => {
        const previous = localRails;
        setBusy(busyKey);
        patchRailEnabled(railId, enabled);
        try {
            const res = await api.hatV2SetRailEnable(mac, railId, enabled);
            if (res?.ok)
                applyRailUpdate(res);
        }
        catch (e) {
            setLocalRails(previous);
            setStatusText(e instanceof Error ? e.message : "Rail command failed");
        }
        finally {
            setBusy(null);
        }
    };
    const setRailVoltage = async (railId, voltageMv, busyKey) => {
        const previous = localRails;
        setBusy(busyKey);
        try {
            const res = await api.hatV2SetRailVoltage(mac, railId, voltageMv);
            if (res?.ok)
                applyRailUpdate(res);
        }
        catch (e) {
            setLocalRails(previous);
            setStatusText(e instanceof Error ? e.message : "Rail command failed");
        }
        finally {
            setBusy(null);
        }
    };
    const detect = async () => {
        if (!mac)
            return;
        setBusy("detect");
        setStatusText(null);
        try {
            await api.hatDetect(mac);
            setStatusText("Detection requested");
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatusText(e instanceof Error ? e.message : "Command failed");
            }
        }
        finally {
            setBusy(null);
        }
    };
    const reset = async () => {
        if (!mac)
            return;
        setBusy("reset");
        setStatusText(null);
        try {
            await api.hatReset(mac);
            setStatusText("HAT Reset requested");
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatusText(e instanceof Error ? e.message : "Command failed");
            }
        }
        finally {
            setBusy(null);
        }
    };
    const railStatusMeta = (rail) => {
        const status = Number(rail?.status ?? 0);
        switch (status) {
            case 0: return { label: "OK", color: "#10b981" };
            case 1: return { label: "FAULT", color: "#ef4444" };
            case 2: return { label: "CAL INVALID", color: "#f59e0b" };
            case 3: return { label: "BUSY", color: "#3b82f6" };
            default: return { label: `STATUS ${status}`, color: "#f59e0b" };
        }
    };
    const presentValue = hat?.detected ?? hat?.present;
    const detected = presentValue === true || (presentValue == null && hatSeen);
    const explicitlyAbsent = presentValue === false;
    const connected = !!hat?.connected;
    const degraded = !!hat?.degraded;
    const dapConnected = !!hat?.dapConnected;
    const targetDetected = !!hat?.targetDetected;
    const targetDpidr = hat?.targetDpidr || 0;
    const version = hat?.fwMajor != null ? `${hat.fwMajor}.${hat.fwMinor ?? 0}` : "—";
    const resolvedRoute = laRouteSig !== null ? laRouteSig : (hat?.laRoute ?? 0);
    const railList = localRails ?? (rails?.rails || []);
    return (<GlassCard title="HAT Expansion Board v2">
      <HatStatusPanel detected={detected} connected={connected} degraded={degraded} dapConnected={dapConnected} targetDetected={targetDetected} hwRevision={caps ? caps.hwRevision : "-"} version={version}/>

      {explicitlyAbsent ? (<div style={{ textAlign: "center", padding: "32px 16px", color: "var(--text-dim)" }}>
          <div style={{ fontSize: "20px", marginBottom: "8px" }}>No HAT Expansion Board Detected</div>
          <div style={{ fontSize: "12px", marginBottom: "16px" }}>Connect a HAT board to the expansion header.</div>
          <div style={{ display: "flex", justifyContent: "center", gap: "8px" }}>
            <button class="btn" onClick={detect} disabled={busy === "detect"}>
              {busy === "detect" ? "Detecting..." : "Detect"}
            </button>
            <button class="btn" onClick={reset} disabled={busy === "reset"}>
              {busy === "reset" ? "Resetting..." : "Reset"}
            </button>
          </div>
        </div>) : (<>
          <div style={{ display: "flex", gap: "8px", flexWrap: "wrap", marginBottom: "12px" }}>
            <span style={{ fontSize: "10px", color: "var(--text-dim)", alignSelf: "center" }}>Capabilities:</span>
            {caps && (<>
                {caps.flags & 1 ? <span style={{ fontSize: "9px", background: "rgba(16,185,129,0.1)", color: "#10b981", border: "1px solid rgba(16,185,129,0.3)", padding: "2px 6px", borderRadius: "4px" }}>Rails Control</span> : null}
                {caps.flags & 2 ? <span style={{ fontSize: "9px", background: "rgba(59,130,246,0.1)", color: "#3b82f6", border: "1px solid rgba(59,130,246,0.3)", padding: "2px 6px", borderRadius: "4px" }}>RGB LEDs</span> : null}
                {caps.flags & 4 ? <span style={{ fontSize: "9px", background: "rgba(139,92,246,0.1)", color: "#8b5cf6", border: "1px solid rgba(139,92,246,0.3)", padding: "2px 6px", borderRadius: "4px" }}>LA Route Low-Speed</span> : null}
                {caps.flags & 8 ? <span style={{ fontSize: "9px", background: "rgba(168,85,247,0.1)", color: "#a855f7", border: "1px solid rgba(168,85,247,0.3)", padding: "2px 6px", borderRadius: "4px" }}>LA Route High-Speed</span> : null}
                {caps.flags & 16 ? <span style={{ fontSize: "9px", background: "rgba(236,72,153,0.1)", color: "#ec4899", border: "1px solid rgba(236,72,153,0.3)", padding: "2px 6px", borderRadius: "4px" }}>Shifted I/O Bank</span> : null}
              </>)}
          </div>

          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "16px", marginBottom: "16px" }}>
            <HatPowerRailsPanel rails={railList} calActive={calActive} busy={busy} toggleRail={toggleRail} setRailVoltage={setRailVoltage} railStatusMeta={railStatusMeta}/>

            <HatRoutingPanel mac={mac} la={la} resolvedRoute={resolvedRoute} setLaRouteSig={setLaRouteSig} targetDetected={targetDetected} targetDpidr={targetDpidr} setBusy={setBusy} setStatusText={setStatusText}/>
          </div>

          <HatLogsPanel mac={mac} busy={busy} setBusy={setBusy} setStatusText={setStatusText}/>

          <HatIoBankPanel mac={mac} busy={busy} setBusy={setBusy} setStatusText={setStatusText}/>

          <HatCalibrationPanel mac={mac} busy={busy} setBusy={setBusy} setStatusText={setStatusText} calActive={calActive} setCalActive={setCalActive}/>
        </>)}

      {/* General actions / detection / reset */}
      <div style={{ display: "flex", justifyContent: "flex-end", gap: "8px", marginTop: "12px" }}>
        {statusText && <span style={{ fontSize: "10px", color: "var(--text-dim)", alignSelf: "center", marginRight: "auto" }}>{statusText}</span>}
        <button class="btn" onClick={detect} disabled={busy === "detect"}>
          {busy === "detect" ? "Detecting..." : "Detect"}
        </button>
        <button class="btn" onClick={reset} disabled={busy === "reset"}>
          {busy === "reset" ? "Resetting..." : "Reset"}
        </button>
      </div>
    </GlassCard>);
}
