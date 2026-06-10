// =============================================================================
// Led — colored indicator with optional label.
// =============================================================================
export function Led({ state, label }) {
    const cls = state === "off" ? "led" : "led " + state;
    return (<span class="led-wrap">
      <span class={cls}/>
      {label && <span class="led-label">{label}</span>}
    </span>);
}
