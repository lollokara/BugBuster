// =============================================================================
// HatCard — HAT Expansion Board v2 status, rails, routing, IO bank, calibration
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { usePoll as useInterval } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { api, PairingRequiredError } from "../../api/client";
import { deviceMac } from "../../state/signals";

export function HatCard() {
  const mac = deviceMac.value;
  const hat = useInterval(() => api.hat(), 2000) as any;
  const la = useInterval(() => api.hatLaStatus(), 1000) as any;
  const rails = useInterval(() => {
    if (hat && (hat.detected ?? hat.present)) {
      return api.hatV2Rails();
    }
    return Promise.resolve(null);
  }, 2000) as any;

  const [caps, setCaps] = useState<any>(null);
  useEffect(() => {
    if (hat && (hat.detected ?? hat.present)) {
      api.hatV2Caps()
        .then(setCaps)
        .catch(err => console.warn("Failed to get HAT caps", err));
    } else {
      setCaps(null);
    }
  }, [hat?.detected, hat?.present]);

  const [busy, setBusy] = useState<string | null>(null);
  const [statusText, setStatusText] = useState<string | null>(null);

  const [localRails, setLocalRails] = useState<any[] | null>(null);
  const [laLogEnabled, setLaLogEnabled] = useState<boolean>(false);
  const [laLogLines, setLaLogLines] = useState<string[]>([]);

  // Shifted IO Bank States
  const [ioDirs, setIoDirs] = useState<number>(0);
  const [ioUps, setIoUps] = useState<number>(0);
  const [ioDns, setIoDns] = useState<number>(0);
  const [ioVals, setIoVals] = useState<number>(0);

  // Level Shifter States
  const [lsOe, setLsOe] = useState<boolean>(false);
  const [lsDir, setLsDir] = useState<boolean>(false);

  // LA Route State
  const [laRouteSig, setLaRouteSig] = useState<number | null>(null);
  const [hatSeen, setHatSeen] = useState<boolean>(false);

  // Calibration States
  const [calActive, setCalActive] = useState<boolean>(false);
  const [calProgress, setCalProgress] = useState<number>(0);
  const [calRailId, setCalRailId] = useState<number>(1); // 1 = VADJ3, 2 = VADJ4
  const [calStage, setCalStage] = useState<number>(0);
  const [calPoint, setCalPoint] = useState<number>(0);
  const [calCode, setCalCode] = useState<number>(0);
  const [calMeasuredMv, setCalMeasuredMv] = useState<number>(-1);
  const [calPersistState, setCalPersistState] = useState<number>(0);
  const [calMinMv, setCalMinMv] = useState<number>(-1);
  const [calMaxMv, setCalMaxMv] = useState<number>(-1);
  const [calMaxGapMv, setCalMaxGapMv] = useState<number>(-1);
  const [calMaxErrorMv, setCalMaxErrorMv] = useState<number>(-1);
  const [calValidationFlags, setCalValidationFlags] = useState<number>(0);
  const [laLogError, setLaLogError] = useState<string | null>(null);
  const [vadj3TargetMv, setVadj3TargetMv] = useState<number>(3300);
  const [vadj4TargetMv, setVadj4TargetMv] = useState<number>(3300);
  const [v3v3TargetMv, setV3v3TargetMv] = useState<number>(3300);

  useEffect(() => {
    if (rails?.rails) setLocalRails(null);
  }, [rails]);

  useEffect(() => {
    const present = hat?.detected ?? hat?.present;
    if (present === true) setHatSeen(true);
    if (present === false) setHatSeen(false);
  }, [hat]);

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

  const applyRailUpdate = (res: any) => {
    if (!res || res.railId == null) return;
    const base = localRails ?? railList;
    let matched = false;
    const update = {
      railId: res.railId,
      enabled: !!res.enabled,
      voltageMv: res.voltageMv ?? res.voltage_mv ?? 0,
      currentMa: res.currentMa ?? res.current_ma ?? 0,
      status: res.status ?? 0,
    };
    const updated = base.map((r: any) => {
      if (r.railId !== res.railId) return r;
      matched = true;
      return { ...r, ...update };
    });
    setLocalRails(matched ? updated : [...updated, update]);
  };

  const patchRailEnabled = (railId: number, enabled: boolean) => {
    const base = localRails ?? railList;
    let matched = false;
    const updated = base.map((r: any) => {
      if (r.railId !== railId) return r;
      matched = true;
      return { ...r, enabled };
    });
    setLocalRails(
      matched
        ? updated
        : [...updated, { railId, enabled, voltageMv: 0, currentMa: 0, status: 0 }]
    );
  };

  const toggleRail = async (railId: number, enabled: boolean, busyKey: string) => {
    const previous = localRails;
    setBusy(busyKey);
    patchRailEnabled(railId, enabled);
    try {
      const res = await api.hatV2SetRailEnable(mac!, railId, enabled);
      if (res?.ok) applyRailUpdate(res);
    } catch(e) {
      setLocalRails(previous);
      setStatusText(e instanceof Error ? e.message : "Rail command failed");
    } finally {
      setBusy(null);
    }
  };

  const setRailVoltage = async (railId: number, voltageMv: number, busyKey: string) => {
    const previous = localRails;
    setBusy(busyKey);
    try {
      const res = await api.hatV2SetRailVoltage(mac!, railId, voltageMv);
      if (res?.ok) applyRailUpdate(res);
    } catch(e) {
      setLocalRails(previous);
      setStatusText(e instanceof Error ? e.message : "Rail command failed");
    } finally {
      setBusy(null);
    }
  };

  useEffect(() => {
    if (!calActive) return;
    const interval = setInterval(async () => {
      try {
        const res = await api.hatV2CalibrateStatus();
        if (res) {
          setCalProgress(res.progress);
          setCalStage(res.stage ?? 0);
          setCalPoint(res.point ?? 0);
          setCalCode(res.code ?? 0);
          setCalMeasuredMv(res.measuredMv ?? -1);
          setCalPersistState(res.persistState ?? 0);
          setCalMinMv(res.minMv ?? -1);
          setCalMaxMv(res.maxMv ?? -1);
          setCalMaxGapMv(res.maxGapMv ?? -1);
          setCalMaxErrorMv(res.maxErrorMv ?? -1);
          setCalValidationFlags(res.validationFlags ?? 0);
          if (res.state === 2) {
            setCalActive(false);
            setStatusText("Calibration completed successfully!");
          } else if (res.state === 3) {
            setCalActive(false);
            setStatusText(`Calibration failed (Error ${res.lastError})`);
          }
        }
      } catch (e) {
        setCalActive(false);
        setStatusText("Failed to read calibration status");
      }
    }, 500);
    return () => clearInterval(interval);
  }, [calActive]);

  const detect = async () => {
    if (!mac) return;
    setBusy("detect");
    setStatusText(null);
    try {
      await api.hatDetect(mac!);
      setStatusText("Detection requested");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatusText(e instanceof Error ? e.message : "Command failed");
      }
    } finally {
      setBusy(null);
    }
  };

  const calStageLabel = (stage: number) => {
    switch (stage) {
      case 1: return "prepare";
      case 2: return "step";
      case 3: return "settle";
      case 4: return "measure";
      case 5: return "done";
      case 8: return "error";
      default: return "idle";
    }
  };

  const calPersistLabel = (state: number) => {
    switch (state) {
      case 0: return { label: "RAM only", color: "#f59e0b" };
      case 1: return { label: "Save pending", color: "#3b82f6" };
      case 2: return { label: "Saving", color: "#3b82f6" };
      case 3: return { label: "Save failed", color: "#ef4444" };
      default: return { label: `State ${state}`, color: "var(--text-dim)" };
    }
  };

  const railStatusMeta = (rail: any) => {
    const status = Number(rail?.status ?? 0);
    switch (status) {
      case 0: return { label: "OK", color: "#10b981" };
      case 1: return { label: "FAULT", color: "#ef4444" };
      case 2: return { label: "CAL INVALID", color: "#f59e0b" };
      case 3: return { label: "BUSY", color: "#3b82f6" };
      default: return { label: `STATUS ${status}`, color: "#f59e0b" };
    }
  };

  const reset = async () => {
    if (!mac) return;
    setBusy("reset");
    setStatusText(null);
    try {
      await api.hatReset(mac!);
      setStatusText("HAT Reset requested");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatusText(e instanceof Error ? e.message : "Command failed");
      }
    } finally {
      setBusy(null);
    }
  };

  const startCal = async () => {
    if (!mac) return;
    setBusy("cal");
    setStatusText(null);
    try {
      const res = await api.hatV2CalibrateStart(mac!, calRailId);
      if (res && res.ok) {
        setCalActive(true);
        setCalProgress(0);
        setStatusText("Calibration sweep started...");
      } else {
        setStatusText(`Failed to start calibration: ${res?.error || "unknown error"}`);
      }
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatusText(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
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
  const railV3 = railList.find((r: any) => r.railId === 1); // VADJ3
  const railV4 = railList.find((r: any) => r.railId === 2); // VADJ4
  const railAdj = railList.find((r: any) => r.railId === 0); // 3V3_ADJ

  const v3En = railV3?.enabled || false;
  const v4En = railV4?.enabled || false;
  const adjEn = railAdj?.enabled || false;

  const v3Mv = railV3?.voltageMv || 0;
  const v4Mv = railV4?.voltageMv || 0;
  const adjMv = railAdj?.voltageMv || 0;

  const v3Ma = railV3?.currentMa || 0;
  const v4Ma = railV4?.currentMa || 0;
  const adjStatus = railStatusMeta(railAdj);
  const v3Status = railStatusMeta(railV3);
  const v4Status = railStatusMeta(railV4);
  const persistMeta = calPersistLabel(calPersistState);

  return (
    <GlassCard title="HAT Expansion Board v2">
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
          <div class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>{caps ? `v${caps.hwRevision}` : "-"}</div>
        </div>
        <div style={{ textAlign: "center", padding: "6px", borderRadius: "6px", background: "var(--bg2)" }}>
          <div style={{ fontSize: "9px", color: "var(--text-dim)", marginBottom: "3px" }}>Firmware</div>
          <div class="mono" style={{ fontSize: "11px", fontWeight: "600" }}>v{version}</div>
        </div>
      </div>

      {explicitlyAbsent ? (
        <div style={{ textAlign: "center", padding: "32px 16px", color: "var(--text-dim)" }}>
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
        </div>
      ) : (
        <>
          <div style={{ display: "flex", gap: "8px", flexWrap: "wrap", marginBottom: "12px" }}>
            <span style={{ fontSize: "10px", color: "var(--text-dim)", alignSelf: "center" }}>Capabilities:</span>
            {caps && (
              <>
                {caps.flags & 1 ? <span style={{ fontSize: "9px", background: "rgba(16,185,129,0.1)", color: "#10b981", border: "1px solid rgba(16,185,129,0.3)", padding: "2px 6px", borderRadius: "4px" }}>Rails Control</span> : null}
                {caps.flags & 2 ? <span style={{ fontSize: "9px", background: "rgba(59,130,246,0.1)", color: "#3b82f6", border: "1px solid rgba(59,130,246,0.3)", padding: "2px 6px", borderRadius: "4px" }}>RGB LEDs</span> : null}
                {caps.flags & 4 ? <span style={{ fontSize: "9px", background: "rgba(139,92,246,0.1)", color: "#8b5cf6", border: "1px solid rgba(139,92,246,0.3)", padding: "2px 6px", borderRadius: "4px" }}>LA Route Low-Speed</span> : null}
                {caps.flags & 8 ? <span style={{ fontSize: "9px", background: "rgba(168,85,247,0.1)", color: "#a855f7", border: "1px solid rgba(168,85,247,0.3)", padding: "2px 6px", borderRadius: "4px" }}>LA Route High-Speed</span> : null}
                {caps.flags & 16 ? <span style={{ fontSize: "9px", background: "rgba(236,72,153,0.1)", color: "#ec4899", border: "1px solid rgba(236,72,153,0.3)", padding: "2px 6px", borderRadius: "4px" }}>Shifted I/O Bank</span> : null}
              </>
            )}
          </div>

          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "16px", marginBottom: "16px" }}>
            {/* Power Rails section */}
            <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px" }}>
              <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>Power Rails</div>

              {/* 3V3_ADJ Rail */}
              <div style={{ marginBottom: "10px", padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "6px" }}>
                  <span style={{ fontSize: "11px", fontWeight: "700", color: "#10b981" }}>3V3_ADJ Rail (1.7–5.0V)</span>
                  <span class="mono" style={{ fontSize: "8px", color: adjStatus.color }}>{adjStatus.label}</span>
                  <button
                    class="btn"
                    style={{ fontSize: "10px", padding: "2px 8px", background: adjEn ? "rgba(16,185,129,0.15)" : "var(--glass)", color: adjEn ? "#10b981" : "var(--text)", borderColor: adjEn ? "#10b98150" : "var(--border-bright)" }}
                    onClick={async () => {
                      await toggleRail(0, !adjEn, "rail0");
                    }}
                    disabled={calActive || busy === "rail0"}
                  >
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
                  <input
                    type="range"
                    min="1700"
                    max="5000"
                    step="100"
                    value={v3v3TargetMv}
                    onInput={(e) => setV3v3TargetMv(Number((e.currentTarget as HTMLInputElement).value) || 0)}
                    disabled={calActive}
                  />
                  <div style={{ display: "flex", alignItems: "center", gap: "8px" }}>
                    <span class="mono" style={{ fontSize: "10px", color: "var(--text-dim)", minWidth: "50px", textAlign: "right" }}>
                      {(v3v3TargetMv / 1000.0).toFixed(2)} V
                    </span>
                    <button class="btn" style={{ fontSize: "10px", padding: "4px 10px" }}
                      onClick={() => setRailVoltage(0, v3v3TargetMv, "v3v3-voltage")}
                      disabled={calActive || busy === "v3v3-voltage"}
                    >
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
                  <button
                    class="btn"
                    style={{ fontSize: "10px", padding: "2px 8px", background: v3En ? "rgba(16,185,129,0.15)" : "var(--glass)", color: v3En ? "#10b981" : "var(--text)", borderColor: v3En ? "#10b98150" : "var(--border-bright)" }}
                    onClick={async () => {
                      await toggleRail(1, !v3En, "rail1");
                    }}
                    disabled={calActive || busy === "rail1"}
                  >
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
                  <input
                    type="range"
                    min="0"
                    max="36000"
                    step="100"
                    value={vadj3TargetMv}
                    onInput={(e) => setVadj3TargetMv(Number((e.currentTarget as HTMLInputElement).value) || 0)}
                    disabled={calActive}
                  />
                  <div style={{ display: "flex", alignItems: "center", gap: "8px" }}>
                    <span class="mono" style={{ fontSize: "10px", color: "var(--text-dim)", minWidth: "72px", textAlign: "right" }}>
                      {(vadj3TargetMv / 1000.0).toFixed(2)} V
                    </span>
                    <button class="btn" style={{ fontSize: "10px", padding: "4px 10px" }}
                      onClick={() => setRailVoltage(1, vadj3TargetMv, "vadj3-voltage")}
                      disabled={calActive || busy === "vadj3-voltage"}
                    >
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
                  <button
                    class="btn"
                    style={{ fontSize: "10px", padding: "2px 8px", background: v4En ? "rgba(16,185,129,0.15)" : "var(--glass)", color: v4En ? "#10b981" : "var(--text)", borderColor: v4En ? "#10b98150" : "var(--border-bright)" }}
                    onClick={async () => {
                      await toggleRail(2, !v4En, "rail2");
                    }}
                    disabled={calActive || busy === "rail2"}
                  >
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
                  <input
                    type="range"
                    min="0"
                    max="36000"
                    step="100"
                    value={vadj4TargetMv}
                    onInput={(e) => setVadj4TargetMv(Number((e.currentTarget as HTMLInputElement).value) || 0)}
                    disabled={calActive}
                  />
                  <div style={{ display: "flex", alignItems: "center", gap: "8px" }}>
                    <span class="mono" style={{ fontSize: "10px", color: "var(--text-dim)", minWidth: "72px", textAlign: "right" }}>
                      {(vadj4TargetMv / 1000.0).toFixed(2)} V
                    </span>
                    <button class="btn" style={{ fontSize: "10px", padding: "4px 10px" }}
                      onClick={() => setRailVoltage(2, vadj4TargetMv, "vadj4-voltage")}
                      disabled={calActive || busy === "vadj4-voltage"}
                    >
                      Confirm
                    </button>
                  </div>
                </div>
              </div>
            </div>

            {/* Routing & SWD Section */}
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
          </div>

          {/* LA Debug Logs section */}
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

          <div style={{ display: "grid", gridTemplateColumns: "1.2fr 0.8fr", gap: "16px", marginBottom: "16px" }}>

            {/* Shifted I/O Bank */}
            <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px" }}>
              <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>Shifted I/O Bank Configuration (GPIO 10-15, 20-21)</div>

              <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "8px", marginBottom: "10px" }}>
                {Array.from({ length: 8 }).map((_, i) => {
                  const isOut = (ioDirs & (1 << i)) !== 0;
                  const isUp = (ioUps & (1 << i)) !== 0;
                  const isDn = (ioDns & (1 << i)) !== 0;
                  const isHigh = (ioVals & (1 << i)) !== 0;
                  return (
                    <div key={i} style={{ padding: "6px", borderRadius: "6px", background: "var(--bg2)", border: "1px solid var(--border)" }}>
                      <div style={{ fontSize: "9px", fontWeight: "700", color: "#3b82f6", marginBottom: "3px" }}>SH_IO_{i + 1}</div>
                      <div style={{ display: "flex", gap: "2px", marginBottom: "4px" }}>
                        <button
                          class="btn"
                          style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: !isOut ? "rgba(59,130,246,0.25)" : "var(--glass)", color: !isOut ? "#3b82f6" : "var(--text)", border: "none" }}
                          onClick={() => setIoDirs(d => d & ~(1 << i))}
                        >
                          IN
                        </button>
                        <button
                          class="btn"
                          style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: isOut ? "rgba(59,130,246,0.25)" : "var(--glass)", color: isOut ? "#3b82f6" : "var(--text)", border: "none" }}
                          onClick={() => setIoDirs(d => d | (1 << i))}
                        >
                          OUT
                        </button>
                      </div>
                      {/* Output value toggle — only relevant when pin is OUT */}
                      {isOut && (
                        <div style={{ display: "flex", gap: "2px", marginBottom: "4px" }}>
                          <button
                            class="btn"
                            style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: !isHigh ? "rgba(239,68,68,0.2)" : "var(--glass)", color: !isHigh ? "#ef4444" : "var(--text-dim)", border: "none" }}
                            onClick={() => setIoVals(v => v & ~(1 << i))}
                          >
                            LOW
                          </button>
                          <button
                            class="btn"
                            style={{ padding: "1px 2px", fontSize: "8px", flex: 1, background: isHigh ? "rgba(34,197,94,0.2)" : "var(--glass)", color: isHigh ? "#22c55e" : "var(--text-dim)", border: "none" }}
                            onClick={() => setIoVals(v => v | (1 << i))}
                          >
                            HIGH
                          </button>
                        </div>
                      )}
                      <div style={{ display: "flex", flexDirection: "column", gap: "2px" }}>
                        <label style={{ fontSize: "8px", display: "flex", alignItems: "center", gap: "2px", cursor: "pointer" }}>
                          <input
                            type="checkbox"
                            checked={isUp}
                            onChange={(e) => {
                              const chk = e.currentTarget.checked;
                              if (chk) {
                                setIoUps(u => u | (1 << i));
                                setIoDns(d => d & ~(1 << i));
                              } else {
                                setIoUps(u => u & ~(1 << i));
                              }
                            }}
                          />
                          Pull-Up
                        </label>
                        <label style={{ fontSize: "8px", display: "flex", alignItems: "center", gap: "2px", cursor: "pointer" }}>
                          <input
                            type="checkbox"
                            checked={isDn}
                            onChange={(e) => {
                              const chk = e.currentTarget.checked;
                              if (chk) {
                                setIoDns(d => d | (1 << i));
                                setIoUps(u => u & ~(1 << i));
                              } else {
                                setIoDns(d => d & ~(1 << i));
                              }
                            }}
                          />
                          Pull-Down
                        </label>
                      </div>
                    </div>
                  );
                })}
              </div>

              <button
                class="btn primary"
                style={{ width: "100%", fontSize: "10px", padding: "4px" }}
                onClick={async () => {
                  setBusy("io_bank");
                  try {
                    await api.hatV2SetIoBank(mac!, ioDirs, ioUps, ioDns, ioVals);
                    setStatusText("I/O bank configuration applied!");
                  } catch(e) {
                    if (!(e instanceof PairingRequiredError)) {
                      setStatusText(e instanceof Error ? e.message : "Command failed");
                    }
                  }
                  setBusy(null);
                }}
                disabled={busy !== null}
              >
                Apply I/O Bank Config
              </button>
            </div>

            {/* Level Shifter Overrides */}
            <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px" }}>
              <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>Level Shifter Overrides</div>

              <div style={{ marginBottom: "8px", padding: "8px", borderRadius: "6px", background: "var(--bg2)" }}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "4px" }}>
                  <span style={{ fontSize: "10px", fontWeight: "700" }}>Outputs Enable (OE)</span>
                  <button
                    class="btn"
                    style={{ fontSize: "9px", padding: "1px 6px", background: lsOe ? "rgba(239,68,68,0.15)" : "var(--glass)", color: lsOe ? "#ef4444" : "var(--text)", borderColor: lsOe ? "#ef444450" : "var(--border-bright)" }}
                    onClick={async () => {
                      const next = !lsOe;
                      setBusy("ls");
                      try {
                        const res = await api.hatV2SetLevelShift(mac!, next, lsDir);
                        if (res && res.ok) {
                          setLsOe(res.oe);
                          setLsDir(res.dir);
                          setStatusText(res.oe ? "Outputs Enabled!" : "Outputs Tri-stated!");
                        }
                      } catch(e) {
                        if (!(e instanceof PairingRequiredError)) {
                          setStatusText(e instanceof Error ? e.message : "Command failed");
                        }
                      }
                      setBusy(null);
                    }}
                  >
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
                  <button
                    class="btn"
                    style={{ fontSize: "9px", padding: "1px 6px" }}
                    onClick={async () => {
                      const next = !lsDir;
                      setBusy("ls");
                      try {
                        const res = await api.hatV2SetLevelShift(mac!, lsOe, next);
                        if (res && res.ok) {
                          setLsOe(res.oe);
                          setLsDir(res.dir);
                        }
                      } catch(e) {
                        if (!(e instanceof PairingRequiredError)) {
                          setStatusText(e instanceof Error ? e.message : "Command failed");
                        }
                      }
                      setBusy(null);
                    }}
                  >
                    {lsDir ? "A → B (Output)" : "B → A (Input)"}
                  </button>
                </div>
                <div style={{ fontSize: "8px", color: "var(--text-dim)" }}>
                  A → B drives target. B → A sets EXP_EXT as inputs.
                </div>
              </div>
            </div>
          </div>

          {/* Calibration */}
          <div style={{ background: "rgba(6,10,20,0.25)", border: "1px solid var(--border)", borderRadius: "8px", padding: "12px", marginBottom: "8px" }}>
            <div class="card-title" style={{ marginBottom: "10px", fontSize: "10px" }}>DS4424 Auto-Calibration Sweep</div>

            {calActive ? (
              <div style={{ padding: "8px", textAlign: "center" }}>
                <div style={{ fontSize: "11px", fontWeight: "700", color: "#3b82f6", marginBottom: "4px" }}>
                  Calibrating Rail {calRailId === 1 ? "VADJ3" : "VADJ4"} ...
                </div>
                <div style={{ width: "100%", height: "8px", background: "var(--bg2)", borderRadius: "4px", overflow: "hidden", marginBottom: "6px" }}>
                  <div style={{ width: `${calProgress}%`, height: "100%", background: "#3b82f6", transition: "width 0.3s ease" }} />
                </div>
                <div style={{ fontSize: "9px", color: "var(--text-dim)", display: "flex", gap: "10px", justifyContent: "center", flexWrap: "wrap" }}>
                  <span>Progress: {calProgress}% complete</span>
                  <span>Stage: {calStageLabel(calStage)}</span>
                  <span>Code: {calCode}</span>
                  <span>{calMeasuredMv >= 0 ? `Measured: ${(calMeasuredMv / 1000.0).toFixed(3)} V` : "Measured: —"}</span>
                  <span>Point: {calPoint}</span>
                  <span>Persist: <span style={{ color: persistMeta.color }}>{persistMeta.label}</span></span>
                  <span>{calMinMv >= 0 ? `Range: ${(calMinMv / 1000.0).toFixed(2)}–${(calMaxMv / 1000.0).toFixed(2)} V` : "Range: —"}</span>
                  <span>{calMaxGapMv >= 0 ? `Max gap: ${calMaxGapMv} mV` : "Max gap: —"}</span>
                  <span>{calMaxErrorMv >= 0 ? `Max error: ${calMaxErrorMv} mV` : "Max error: —"}</span>
                  <span style={{ color: calValidationFlags ? "#f59e0b" : "#10b981" }}>Flags: 0x{calValidationFlags.toString(16)}</span>
                </div>
              </div>
            ) : (
              <div style={{ display: "flex", gap: "10px", alignItems: "center" }}>
                <span style={{ fontSize: "10px", color: "var(--text-dim)" }}>Select rail to calibrate:</span>
                <select
                  class="input"
                  style={{ background: "var(--bg2)", border: "1px solid var(--border-bright)", color: "var(--text)", borderRadius: "4px", padding: "2px 4px", fontSize: "10px", width: "auto", height: "24px" }}
                  value={calRailId}
                  onChange={(e) => setCalRailId(parseInt(e.currentTarget.value, 10))}
                >
                  <option value={1}>VADJ3 (1.8–36V, midpoint 18V)</option>
                  <option value={2}>VADJ4 (1.8–36V, midpoint 18V)</option>
                </select>
                <button
                  class="btn primary"
                  style={{ fontSize: "10px", padding: "4px 12px" }}
                  onClick={startCal}
                  disabled={busy !== null}
                >
                  Start Calibration Sweep
                </button>
              </div>
            )}
          </div>
        </>
      )}

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
    </GlassCard>
  );
}
