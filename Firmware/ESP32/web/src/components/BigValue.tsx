// =============================================================================
// BigValue — large numeric readout with optional unit badge.
// =============================================================================

export interface BigValueProps {
  value: number | string;
  unit?: string;
  precision?: number;
  color?: string;
  label?: string;
  /** Draws attention to a value that should not be read as authoritative. */
  highlight?: boolean;
}

export function BigValue({ value, unit, precision = 3, color, label, highlight }: BigValueProps) {
  let text: string;
  if (typeof value === "number") {
    if (!Number.isFinite(value)) text = "—";
    else text = value.toFixed(precision);
  } else {
    text = value;
  }
  const style = color ? { color } : highlight ? { color: "#f59e0b" } : undefined;
  return (
    <div class={"big-value-block" + (highlight ? " highlight" : "")}>
      {label && <div class="big-value-label text-dim">{label}</div>}
      <div class="big-value" style={style}>
        {text}
        {unit && <span class="unit">{unit}</span>}
      </div>
    </div>
  );
}
