// =============================================================================
// BarGauge — horizontal fill showing value within [min, max].
// =============================================================================

export interface BarGaugeProps {
  value: number;
  min: number;
  max: number;
  color?: string;
}

export function BarGauge({ value, min, max, color }: BarGaugeProps) {
  const span = max - min;
  let pct = 0;
  if (span > 0 && Number.isFinite(value)) {
    pct = ((value - min) / span) * 100;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
  }
  const style: Record<string, string> = { width: pct.toFixed(1) + "%" };
  if (color) style.background = color;
  return (
    <div class="bar-gauge">
      <div class="bar-fill" style={style} />
    </div>
  );
}
