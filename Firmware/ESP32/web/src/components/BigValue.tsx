// =============================================================================
// BigValue — large numeric readout with optional unit badge.
// =============================================================================

export interface BigValueProps {
  value: number | string;
  unit?: string;
  precision?: number;
  color?: string;
}

export function BigValue({ value, unit, precision = 3, color }: BigValueProps) {
  let text: string;
  if (typeof value === "number") {
    if (!Number.isFinite(value)) text = "—";
    else text = value.toFixed(precision);
  } else {
    text = value;
  }
  const style = color ? { color } : undefined;
  return (
    <div class="big-value" style={style}>
      {text}
      {unit && <span class="unit">{unit}</span>}
    </div>
  );
}
