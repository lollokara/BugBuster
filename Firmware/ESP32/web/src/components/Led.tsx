// =============================================================================
// Led — colored indicator with optional label.
// =============================================================================

export type LedState = "off" | "on" | "warn" | "err";

export interface LedProps {
  state: LedState;
  label?: string;
}

export function Led({ state, label }: LedProps) {
  const cls = state === "off" ? "led" : "led " + state;
  return (
    <span class="led-wrap">
      <span class={cls} />
      {label && <span class="led-label">{label}</span>}
    </span>
  );
}
