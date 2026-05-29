import { GlassCard } from "../../components/GlassCard";

export function DesktopOnlyCard() {
  return (
    <GlassCard title="Desktop-Only / Transport-Limited">
      <div class="kv-row">
        <span class="uppercase-tag">Logic Analyzer Stream</span>
        <span class="text-dim">USB vendor-bulk only (desktop app)</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Scope Recording / Export</span>
        <span class="text-dim">Desktop workflow (file picker + BBSC/CSV export)</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Calibration Deep Flows</span>
        <span class="text-dim">Partially exposed over HTTP; advanced path remains desktop</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Guidance</span>
        <span class="mono">Use desktop app for USB-only flows</span>
      </div>
    </GlassCard>
  );
}
