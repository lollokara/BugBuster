import { useState, useEffect } from "preact/hooks";
import { api, PairingRequiredError } from "../../api/client";

interface HatCalibrationPanelProps {
  mac: string | null;
  busy: string | null;
  setBusy: (busy: string | null) => void;
  setStatusText: (status: string | null) => void;
  calActive: boolean;
  setCalActive: (active: boolean) => void;
}

export function HatCalibrationPanel({
  mac,
  busy,
  setBusy,
  setStatusText,
  calActive,
  setCalActive,
}: HatCalibrationPanelProps) {
  // Calibration States
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

  const persistMeta = calPersistLabel(calPersistState);

  return (
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
  );
}
