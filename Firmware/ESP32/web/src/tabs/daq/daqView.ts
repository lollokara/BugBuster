// JSON -> view mapping for the DAQ tab, kept pure so it can be tested against
// the captured /api/daq and /api/daq/vdut/status fixtures. Key names follow
// api_core.cpp (api_daq, api_daq_vdut_status).

export type CalState = "calibrated" | "uncalibrated" | "unknown";

export interface DaqView {
  present: boolean;
  typeLabel: string;
  version: string;
  vdut: {
    enabled: boolean;
    fault: boolean;
    measuredV: number;
    measuredA: number;
    powerW: number;
    setpointV: number | null;
    currentLimitMa: number | null;
  };
  // The firmware exposes no per-range calibration flags yet, so this is
  // "unknown" rather than a plausible but false "uncalibrated".
  calibration: CalState;
}

const num = (v: unknown): number | null => (typeof v === "number" && Number.isFinite(v) ? v : null);

export function daqView(daq: any, vdut: any): DaqView {
  const measuredV = num(vdut?.measuredVoltageV) ?? 0;
  const measuredA = (num(vdut?.measuredCurrentMa) ?? 0) / 1000;
  const major = num(daq?.fwMajor);
  const minor = num(daq?.fwMinor);
  return {
    present: daq?.present ?? false,
    typeLabel: typeof daq?.typeName === "string" && daq.typeName ? daq.typeName : "—",
    version: major !== null && minor !== null ? `${major}.${minor}` : "—",
    vdut: {
      enabled: vdut?.enabled ?? false,
      fault: vdut?.fault ?? false,
      measuredV,
      measuredA,
      powerW: measuredV * measuredA,
      setpointV: num(vdut?.voltageSetpointV),
      currentLimitMa: num(vdut?.currentLimitMa),
    },
    calibration: "unknown",
  };
}
