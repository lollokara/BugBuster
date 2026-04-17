// =============================================================================
// Digital tab — DIN/DOUT/GPIO/DIO/IOexp.
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { Led } from "../../components/Led";
import { api } from "../../api/client";
import { deviceStatus } from "../../state/signals";

const CH_NAMES = ["A", "B", "C", "D"] as const;

function useInterval<T>(fn: () => Promise<T>, ms: number) {
  const [value, setValue] = useState<T | null>(null);
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const r = await fn();
        if (alive) setValue(r);
      } catch {
        /* ignore */
      }
      if (alive) setTimeout(tick, ms);
    };
    tick();
    return () => { alive = false; };
  }, []);
  return value;
}

function DinDoutCard() {
  const status = deviceStatus.value;
  const channels = Array.isArray(status?.channels) ? status.channels : [];
  return (
    <GlassCard title="AD74416H Digital IO">
      <div class="dio-grid">
        <div>
          <div class="uppercase-tag">DIN (inputs)</div>
          <div class="dio-row">
            {[0, 1, 2, 3].map((i) => {
              const c = channels[i] ?? {};
              const state = !!(c.dinState ?? c.din_state);
              const counter = Number(c.counter ?? 0);
              return (
                <div class="dio-cell" key={i}>
                  <Led state={state ? "on" : "off"} />
                  <span class="mono">{CH_NAMES[i]}</span>
                  <span class="mono text-dim">#{counter}</span>
                </div>
              );
            })}
          </div>
        </div>
        <div>
          <div class="uppercase-tag">DOUT (outputs)</div>
          <div class="dio-row">
            {[0, 1, 2, 3].map((i) => {
              const c = channels[i] ?? {};
              const state = !!(c.doutState ?? c.dout_state);
              return (
                <button
                  class={"pill" + (state ? " active" : "")}
                  key={i}
                >
                  <Led state={state ? "on" : "off"} /> {CH_NAMES[i]}
                </button>
              );
            })}
          </div>
        </div>
      </div>
    </GlassCard>
  );
}

function GpioCard() {
  const data = useInterval(() => api.gpio(), 1000);
  const pins = Array.isArray(data?.pins) ? data.pins : [];
  return (
    <GlassCard title="ESP32 GPIO">
      <div class="dio-grid-compact">
        {pins.length === 0 && <span class="text-dim">No data</span>}
        {pins.map((p: any, idx: number) => (
          <div class="dio-cell" key={idx}>
            <Led state={p.level ? "on" : "off"} />
            <span class="mono">GP{p.pin ?? idx}</span>
            <select class="input" value={String(p.mode ?? "input")}>
              <option value="input">input</option>
              <option value="output">output</option>
              <option value="input_pulldown">pulldn</option>
            </select>
          </div>
        ))}
      </div>
    </GlassCard>
  );
}

function DioCard() {
  const data = useInterval(() => api.dio(), 1000);
  const pins = Array.isArray(data?.pins) ? data.pins : [];
  return (
    <GlassCard title="ESP32 DIO (12)">
      <div class="dio-grid-compact">
        {pins.length === 0 && <span class="text-dim">No data</span>}
        {pins.map((p: any, idx: number) => (
          <div class="dio-cell" key={idx}>
            <Led state={p.level ? "on" : "off"} />
            <span class="mono">D{p.pin ?? idx}</span>
            <span class="uppercase-tag">{p.mode ?? "—"}</span>
          </div>
        ))}
      </div>
    </GlassCard>
  );
}

function IoExpCard() {
  const data = useInterval(() => api.ioexp(), 1000);
  const pins = Array.isArray(data?.pins) ? data.pins : [];
  return (
    <GlassCard title="PCA9535 IO Expander">
      <div class="dio-grid-compact">
        {pins.length === 0 && <span class="text-dim">No data</span>}
        {pins.map((p: any, idx: number) => (
          <div class="dio-cell" key={idx}>
            <Led state={p.level ? "on" : "off"} />
            <span class="mono">P{p.pin ?? idx}</span>
            <span class="uppercase-tag">{p.direction ?? p.dir ?? "—"}</span>
          </div>
        ))}
      </div>
    </GlassCard>
  );
}

export function Digital() {
  return (
    <div class="tab-stack">
      <DinDoutCard />
      <GpioCard />
      <DioCard />
      <IoExpCard />
    </div>
  );
}
