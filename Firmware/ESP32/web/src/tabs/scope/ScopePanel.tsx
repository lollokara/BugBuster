// =============================================================================
// ScopePanel — toolbar + left rail + ScopeCanvas.
// =============================================================================

import { useMemo } from "preact/hooks";
import { ScopeCanvas, CH_COLORS } from "../../scope/ScopeCanvas";
import {
  scopeBuffer,
  scopeChannelEnabled,
  scopeChannelInvert,
  scopeChannelOffset,
  scopePlotMode,
  scopeRunning,
  scopeSeq,
  scopeTimeBase,
  scopeTriggerLevel,
} from "../../state/signals";
import {
  ADMIN_TOKEN_HEADER,
  getCachedToken,
} from "../../api/client";
import { deviceMac } from "../../state/signals";

const CH_LETTERS = ["A", "B", "C", "D"] as const;
const TIME_BASES: { label: string; seconds: number }[] = [
  { label: "10ms", seconds: 0.01 },
  { label: "100ms", seconds: 0.1 },
  { label: "1s", seconds: 1 },
  { label: "10s", seconds: 10 },
  { label: "60s", seconds: 60 },
];

interface ChStats {
  last: number;
  vpp: number;
  mean: number;
  min: number;
  max: number;
}

function computeStats(ch: number): ChStats {
  const buf = scopeBuffer.value;
  const enabled = scopeChannelEnabled.value[ch];
  if (!enabled || buf.length === 0) {
    return { last: NaN, vpp: 0, mean: 0, min: 0, max: 0 };
  }
  const offset = scopeChannelOffset.value[ch] ?? 0;
  const invert = scopeChannelInvert.value[ch] ?? false;
  let mn = Infinity;
  let mx = -Infinity;
  let sum = 0;
  let n = 0;
  let last = 0;
  const windowSize = Math.min(buf.length, 2048);
  for (let i = buf.length - windowSize; i < buf.length; i++) {
    let v = buf[i]!.v[ch] ?? 0;
    if (invert) v = -v;
    v += offset;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    sum += v;
    n++;
    last = v;
  }
  if (n === 0) return { last: NaN, vpp: 0, mean: 0, min: 0, max: 0 };
  return {
    last,
    vpp: mx - mn,
    mean: sum / n,
    min: mn,
    max: mx,
  };
}

async function wavegenStart(type: string) {
  const mac = deviceMac.value;
  const headers: Record<string, string> = { "Content-Type": "application/json" };
  if (mac) {
    const tok = getCachedToken(mac);
    if (tok) headers[ADMIN_TOKEN_HEADER] = tok;
  }
  try {
    await fetch("/api/wavegen/start", {
      method: "POST",
      headers,
      body: JSON.stringify({ type, freq: 100, amplitude: 1, offset: 0 }),
    });
  } catch {
    /* ignore — UI-only */
  }
}

async function wavegenStop() {
  const mac = deviceMac.value;
  const headers: Record<string, string> = {};
  if (mac) {
    const tok = getCachedToken(mac);
    if (tok) headers[ADMIN_TOKEN_HEADER] = tok;
  }
  try {
    await fetch("/api/wavegen/stop", { method: "POST", headers });
  } catch {
    /* ignore */
  }
}

function ChannelRailCard({ index }: { index: number }) {
  const stats = useMemo(() => computeStats(index), [
    scopeBuffer.value,
    scopeChannelEnabled.value,
    scopeChannelOffset.value,
    scopeChannelInvert.value,
    index,
  ]);
  const color = CH_COLORS[index]!;
  const enabled = scopeChannelEnabled.value[index];
  const offset = scopeChannelOffset.value[index] ?? 0;
  const invert = scopeChannelInvert.value[index] ?? false;

  const toggleEnabled = () => {
    const next = [...scopeChannelEnabled.value] as [boolean, boolean, boolean, boolean];
    next[index] = !enabled;
    scopeChannelEnabled.value = next;
  };
  const toggleInvert = () => {
    const next = [...scopeChannelInvert.value] as [boolean, boolean, boolean, boolean];
    next[index] = !invert;
    scopeChannelInvert.value = next;
  };
  const setOffset = (v: number) => {
    const next = [...scopeChannelOffset.value] as [number, number, number, number];
    next[index] = v;
    scopeChannelOffset.value = next;
  };

  return (
    <div class="scope-rail-card" style={{ borderTop: `2px solid ${color}` }}>
      <div class="scope-rail-head">
        <span class="ch-swatch" style={{ color, background: color }} />
        <span class="scope-rail-letter">{CH_LETTERS[index]}</span>
        <label class="scope-rail-enable">
          <input type="checkbox" checked={enabled} onChange={toggleEnabled} />
          <span>On</span>
        </label>
      </div>
      <div class="scope-rail-value mono">
        {Number.isFinite(stats.last) ? stats.last.toFixed(3) : "—"}
        <span class="unit"> V</span>
      </div>
      <div class="scope-rail-stats">
        <div><span class="uppercase-tag">Vp-p</span><span class="mono">{stats.vpp.toFixed(3)}</span></div>
        <div><span class="uppercase-tag">Mean</span><span class="mono">{stats.mean.toFixed(3)}</span></div>
        <div><span class="uppercase-tag">Min</span><span class="mono">{stats.min.toFixed(3)}</span></div>
        <div><span class="uppercase-tag">Max</span><span class="mono">{stats.max.toFixed(3)}</span></div>
      </div>
      <div class="scope-rail-offset">
        <span class="uppercase-tag">Offset</span>
        <input
          type="range"
          min={-10}
          max={10}
          step={0.1}
          value={offset}
          onInput={(e) => setOffset(parseFloat((e.currentTarget as HTMLInputElement).value))}
        />
        <span class="mono">{offset.toFixed(1)}</span>
      </div>
      <label class="scope-rail-invert">
        <input type="checkbox" checked={invert} onChange={toggleInvert} />
        <span>Invert</span>
      </label>
    </div>
  );
}

export function ScopePanel() {
  const running = scopeRunning.value;
  const mode = scopePlotMode.value;
  const tb = scopeTimeBase.value;
  const trig = scopeTriggerLevel.value;

  const clearBuffer = () => {
    scopeSeq.value = 0;
    // mutate via assignment (signals detect new reference)
    scopeBuffer.value = [];
  };

  return (
    <div class="scope-layout">
      <aside class="scope-rail">
        {[0, 1, 2, 3].map((i) => <ChannelRailCard index={i} key={i} />)}
      </aside>

      <div class="scope-main">
        <div class="scope-toolbar">
          <button class={"pill" + (running ? " active" : "")} onClick={() => (scopeRunning.value = !running)}>
            {running ? "Pause" : "Run"}
          </button>
          <button class="pill" onClick={clearBuffer}>Clear</button>

          <div class="segmented" role="tablist">
            <button
              class={mode === "overlay" ? "active" : ""}
              onClick={() => (scopePlotMode.value = "overlay")}
            >Overlay</button>
            <button
              class={mode === "stacked" ? "active" : ""}
              onClick={() => (scopePlotMode.value = "stacked")}
            >Stacked</button>
          </div>

          <div class="scope-timebase">
            {TIME_BASES.map((tbOpt) => (
              <button
                key={tbOpt.label}
                class={"pill" + (Math.abs(tb - tbOpt.seconds) < 1e-6 ? " active" : "")}
                onClick={() => (scopeTimeBase.value = tbOpt.seconds)}
              >{tbOpt.label}</button>
            ))}
          </div>

          <label class="scope-trigger">
            <span class="uppercase-tag">Trig</span>
            <input
              type="number"
              step={0.1}
              value={trig}
              class="input"
              style={{ width: "80px" }}
              onInput={(e) => (scopeTriggerLevel.value = parseFloat((e.currentTarget as HTMLInputElement).value) || 0)}
            />
          </label>
        </div>

        <ScopeCanvas />

        <div class="scope-wavegen">
          <span class="uppercase-tag">WaveGen @100 Hz</span>
          <button class="pill" onClick={() => wavegenStart("sine")}>Sine</button>
          <button class="pill" onClick={() => wavegenStart("triangle")}>Triangle</button>
          <button class="pill" onClick={() => wavegenStart("square")}>Square</button>
          <button class="pill" onClick={() => wavegenStart("sawtooth")}>Sawtooth</button>
          <button class="pill" onClick={wavegenStop}>Stop</button>
        </div>
      </div>
    </div>
  );
}
