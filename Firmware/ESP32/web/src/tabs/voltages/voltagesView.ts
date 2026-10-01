// Pure mapping for the Voltages tab IDAC card (tested against /api/idac).

export interface IdacRow {
  ch: number;
  code: number;
  voltage: number;
  enabled: boolean;
}

export function idacRows(idac: any): IdacRow[] {
  return (idac?.channels ?? []).map((c: any, i: number) => ({
    ch: i,
    code: c.code ?? 0,
    voltage: c.voltage ?? 0,
    enabled: c.enabled ?? false,
  }));
}
