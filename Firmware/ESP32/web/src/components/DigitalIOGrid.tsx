import { useEffect, useState } from "preact/hooks";
import { api } from "../api/client";
import { Led } from "./Led";

function pinsFromResponse(data: unknown): any[] {
  if (Array.isArray(data)) return data;
  const obj = data as any;
  if (Array.isArray(obj?.pins)) return obj.pins;
  if (Array.isArray(obj?.gpios)) return obj.gpios;
  if (Array.isArray(obj?.ios)) return obj.ios;
  return [];
}

function modeLabel(mode: number): string {
  if (mode === 1) return "OUT";
  if (mode === 2) return "IN";
  return "HI-Z";
}

function levelValue(pin: any): boolean {
  const raw = pin?.input ?? pin?.level ?? pin?.value ?? pin?.output ?? false;
  return raw === true || raw === 1 || raw === "1" || String(raw).toUpperCase() === "HIGH";
}

export function DigitalIOGrid() {
  const [pins, setPins] = useState<any[]>([]);

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const r = await api.gpio();
        if (alive) setPins(pinsFromResponse(r).slice(0, 12));
      } catch {
        if (alive) setPins([]);
      }
      if (alive) window.setTimeout(tick, 2000);
    };
    tick();
    return () => {
      alive = false;
    };
  }, []);

  const cells = Array.from({ length: 12 }, (_, i) => pins[i] ?? { id: i, mode: 0 });

  return (
    <div class="dio-grid-compact">
      {cells.map((pin, idx) => {
        const mode = Number(pin?.mode ?? 0);
        const high = levelValue(pin);
        const gpio = Number(pin?.id ?? pin?.gpio ?? idx);
        return (
          <div
            class="dio-cell"
            key={idx}
            style={{ minHeight: "64px", flexDirection: "column", alignItems: "stretch" }}
          >
            <div class="kv-row" style={{ padding: 0 }}>
              <span class="mono">IO{idx + 1}</span>
              <span class="uppercase-tag">{modeLabel(mode)}</span>
            </div>
            <div class="kv-row" style={{ padding: 0 }}>
              <Led state={high ? "on" : "off"} label={high ? "HIGH" : "LOW"} />
              <span class="mono text-dim">GPIO{gpio}</span>
            </div>
          </div>
        );
      })}
    </div>
  );
}
