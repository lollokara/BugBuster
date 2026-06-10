// =============================================================================
// BigValue — large numeric readout with optional unit badge.
// =============================================================================
export function BigValue({ value, unit, precision = 3, color }) {
    let text;
    if (typeof value === "number") {
        if (!Number.isFinite(value))
            text = "—";
        else
            text = value.toFixed(precision);
    }
    else {
        text = value;
    }
    const style = color ? { color } : undefined;
    return (<div class="big-value" style={style}>
      {text}
      {unit && <span class="unit">{unit}</span>}
    </div>);
}
