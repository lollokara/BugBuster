// JSON -> view mapping for the DAQ tab, kept pure so it can be tested against
// the captured /api/daq and /api/daq/vdut/status fixtures.

export type CalState = "calibrated" | "uncalibrated" | "unknown";

export interface DaqView {
  present: boolean;
  typeLabel: string;
  version: string;
  vdut: {
    enabled: boolean;
    measuredV: number;
    measuredA: number;
    powerW: number;
    range: string;
  };
  calibration: CalState;
}

export function daqView(daq: any, vdut: any): DaqView {
  const present = daq?.present ?? false;
  const hatType = daq?.type ?? 0;
  const measuredV = vdut?.voltage_v ?? 0;
  const measuredA = vdut?.current_a ?? 0;
  const calHaveHi = vdut?.cal_have_hi ?? false;
  const calHaveMid = vdut?.cal_have_mid ?? false;
  const calHaveLo = vdut?.cal_have_lo ?? false;
  return {
    present,
    typeLabel: hatType === 0x10 ? "DAQ HAT" : `0x${hatType.toString(16)}`,
    version: daq?.version ?? "—",
    vdut: {
      enabled: vdut?.enabled ?? false,
      measuredV,
      measuredA,
      powerW: measuredV * measuredA,
      range: vdut?.range ?? "unknown",
    },
    calibration: calHaveHi && calHaveMid && calHaveLo ? "calibrated" : "uncalibrated",
  };
}
