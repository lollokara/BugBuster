// =============================================================================
// Digital tab — DIN/DOUT/GPIO/DIO/IOexp control parity
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { Led } from "../../components/Led";
import { api, PairingRequiredError } from "../../api/client";
import { deviceMac, deviceStatus } from "../../state/signals";
import {
  DIN_DEBOUNCE_OPTIONS,
  DO_MODE_OPTIONS,
} from "../../config/options";

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
    return () => {
      alive = false;
    };
  }, []);
  return value;
}

function DinDoutCard() {
  const status = deviceStatus.value;
  const mac = deviceMac.value;
  const channels = Array.isArray(status?.channels) ? status.channels : [];
  const [dinCfg, setDinCfg] = useState(
    Array.from({ length: 4 }, () => ({ thresh: 64, debounce: 0, ocDet: false, scDet: false })),
  );
  const [doCfg, setDoCfg] = useState(
    Array.from({ length: 4 }, () => ({ mode: 0, srcSelGpio: false, t1: 0, t2: 0 })),
  );

  const pushDin = async (ch: number) => {
    if (!mac) return;
    try {
      await api.channel.setDinConfig(mac, ch, dinCfg[ch]!);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setDinConfig failed", e);
    }
  };

  const pushDo = async (ch: number) => {
    if (!mac) return;
    try {
      await api.channel.setDoConfig(mac, ch, doCfg[ch]!);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setDoConfig failed", e);
    }
  };

  const setDoState = async (ch: number, on: boolean) => {
    if (!mac) return;
    try {
      await api.channel.setDoState(mac, ch, on);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setDoState failed", e);
    }
  };

  return (
    <GlassCard title="AD74416H DIN / DOUT">
      <div class="analog-grid">
        {[0, 1, 2, 3].map((i) => {
          const c = channels[i] ?? {};
          const dinState = !!(c.dinState ?? c.din_state);
          const counter = Number(c.dinCounter ?? c.din_counter ?? 0);
          const doState = !!(c.doState ?? c.do_state ?? c.doutState ?? c.dout_state);
          const din = dinCfg[i]!;
          const dout = doCfg[i]!;
          return (
            <div class="analog-item" key={i}>
              <div class="uppercase-tag">CH {CH_NAMES[i]}</div>
              <div class="kv-row">
                <span class="uppercase-tag">DIN</span>
                <Led state={dinState ? "on" : "off"} label={dinState ? "HIGH" : "LOW"} />
                <span class="mono">#{counter}</span>
              </div>
              <div class="kv-row">
                <span class="uppercase-tag">DOUT</span>
                <button class={"pill" + (doState ? " active" : "")} onClick={() => setDoState(i, !doState)}>
                  {doState ? "ON" : "OFF"}
                </button>
              </div>
              <details>
                <summary class="uppercase-tag">DIN Config</summary>
                <div class="analog-row">
                  <label>Debounce</label>
                  <select
                    class="input"
                    value={String(din.debounce)}
                    onChange={(e) => {
                      const next = [...dinCfg];
                      next[i] = { ...next[i]!, debounce: parseInt((e.currentTarget as HTMLSelectElement).value, 10) };
                      setDinCfg(next);
                    }}
                  >
                    {DIN_DEBOUNCE_OPTIONS.map((opt) => (
                      <option key={opt.code} value={String(opt.code)}>
                        {opt.label}
                      </option>
                    ))}
                  </select>
                </div>
                <div class="analog-row">
                  <label>Threshold</label>
                  <input
                    class="input"
                    type="number"
                    min={0}
                    max={127}
                    value={String(din.thresh)}
                    onInput={(e) => {
                      const next = [...dinCfg];
                      next[i] = { ...next[i]!, thresh: parseInt((e.currentTarget as HTMLInputElement).value || "64", 10) };
                      setDinCfg(next);
                    }}
                  />
                </div>
                <div class="analog-row">
                  <label>OC Detect</label>
                  <input
                    type="checkbox"
                    checked={din.ocDet}
                    onChange={(e) => {
                      const next = [...dinCfg];
                      next[i] = { ...next[i]!, ocDet: (e.currentTarget as HTMLInputElement).checked };
                      setDinCfg(next);
                    }}
                  />
                </div>
                <div class="analog-row">
                  <label>SC Detect</label>
                  <input
                    type="checkbox"
                    checked={din.scDet}
                    onChange={(e) => {
                      const next = [...dinCfg];
                      next[i] = { ...next[i]!, scDet: (e.currentTarget as HTMLInputElement).checked };
                      setDinCfg(next);
                    }}
                  />
                </div>
                <button class="btn" disabled={!mac} onClick={() => pushDin(i)}>
                  Apply DIN
                </button>
              </details>
              <details>
                <summary class="uppercase-tag">DOUT Config</summary>
                <div class="analog-row">
                  <label>Mode</label>
                  <select
                    class="input"
                    value={String(dout.mode)}
                    onChange={(e) => {
                      const next = [...doCfg];
                      next[i] = { ...next[i]!, mode: parseInt((e.currentTarget as HTMLSelectElement).value, 10) };
                      setDoCfg(next);
                    }}
                  >
                    {DO_MODE_OPTIONS.map((opt) => (
                      <option key={opt.code} value={String(opt.code)}>
                        {opt.label}
                      </option>
                    ))}
                  </select>
                </div>
                <div class="analog-row">
                  <label>Source GPIO</label>
                  <input
                    type="checkbox"
                    checked={dout.srcSelGpio}
                    onChange={(e) => {
                      const next = [...doCfg];
                      next[i] = { ...next[i]!, srcSelGpio: (e.currentTarget as HTMLInputElement).checked };
                      setDoCfg(next);
                    }}
                  />
                </div>
                <div class="analog-row">
                  <label>T1</label>
                  <input
                    class="input"
                    type="number"
                    min={0}
                    max={15}
                    value={String(dout.t1)}
                    onInput={(e) => {
                      const next = [...doCfg];
                      next[i] = { ...next[i]!, t1: parseInt((e.currentTarget as HTMLInputElement).value || "0", 10) };
                      setDoCfg(next);
                    }}
                  />
                </div>
                <div class="analog-row">
                  <label>T2</label>
                  <input
                    class="input"
                    type="number"
                    min={0}
                    max={255}
                    value={String(dout.t2)}
                    onInput={(e) => {
                      const next = [...doCfg];
                      next[i] = { ...next[i]!, t2: parseInt((e.currentTarget as HTMLInputElement).value || "0", 10) };
                      setDoCfg(next);
                    }}
                  />
                </div>
                <button class="btn" disabled={!mac} onClick={() => pushDo(i)}>
                  Apply DOUT
                </button>
              </details>
            </div>
          );
        })}
      </div>
    </GlassCard>
  );
}

function GpioCard() {
  const mac = deviceMac.value;
  const data = useInterval(() => api.gpio(), 1000);
  const pins = Array.isArray(data) ? data : [];

  const setConfig = async (gpio: number, mode: number, pulldown: boolean) => {
    if (!mac) return;
    try {
      await api.gpioSetConfig(mac, gpio, mode, pulldown);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("gpioSetConfig failed", e);
    }
  };

  const setOutput = async (gpio: number, value: boolean) => {
    if (!mac) return;
    try {
      await api.gpioSetValue(mac, gpio, value);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("gpioSetValue failed", e);
    }
  };

  return (
    <GlassCard title="ESP32 GPIO (12)">
      <div class="dio-grid-compact">
        {pins.length === 0 && <span class="text-dim">No data</span>}
        {pins.map((p: any, idx: number) => {
          const gpio = Number(p.id ?? idx);
          const mode = Number(p.mode ?? 0);
          const output = !!p.output;
          const input = !!p.input;
          const pulldown = !!p.pulldown;
          return (
            <div class="dio-cell" key={idx}>
              <Led state={input ? "on" : "off"} />
              <span class="mono">IO {gpio + 1}</span>
              <select class="input" value={String(mode)} onChange={(e) => setConfig(gpio, parseInt((e.currentTarget as HTMLSelectElement).value, 10), pulldown)}>
                <option value="0">HIGH_IMP</option>
                <option value="1">OUTPUT</option>
                <option value="2">INPUT</option>
              </select>
              <button class={"pill" + (output ? " active" : "")} onClick={() => setOutput(gpio, !output)}>
                {output ? "HIGH" : "LOW"}
              </button>
            </div>
          );
        })}
      </div>
    </GlassCard>
  );
}

function IoExpCard() {
  const data = useInterval(() => api.ioexp(), 1000);
  const faultLog = useInterval(() => api.ioexp.faults(), 1500);
  const enables = data?.enables ?? {};
  const efuses = Array.isArray(data?.efuses) ? data.efuses : [];
  return (
    <GlassCard title="PCA9535 IO Expander">
      <div class="dio-grid-compact">
        <div class="dio-cell">
          <span class="uppercase-tag">VADJ1</span>
          <Led state={enables.vadj1 ? "on" : "off"} />
        </div>
        <div class="dio-cell">
          <span class="uppercase-tag">VADJ2</span>
          <Led state={enables.vadj2 ? "on" : "off"} />
        </div>
        <div class="dio-cell">
          <span class="uppercase-tag">15V</span>
          <Led state={enables.analog15v ? "on" : "off"} />
        </div>
        <div class="dio-cell">
          <span class="uppercase-tag">MUX</span>
          <Led state={enables.mux ? "on" : "off"} />
        </div>
        <div class="dio-cell">
          <span class="uppercase-tag">USB HUB</span>
          <Led state={enables.usbHub ? "on" : "off"} />
        </div>
      </div>
      <div class="dio-grid-compact" style={{ marginTop: "8px" }}>
        {efuses.map((e: any, idx: number) => (
          <div class="dio-cell" key={idx}>
            <span class="mono">EF{e.id ?? idx + 1}</span>
            <Led state={e.enabled ? "on" : "off"} />
            <Led state={e.fault ? "err" : "off"} label={e.fault ? "fault" : "ok"} />
          </div>
        ))}
      </div>
      <pre class="debug-dump mono">{JSON.stringify(faultLog, null, 2)}</pre>
    </GlassCard>
  );
}

export function Digital() {
  return (
    <div class="tab-stack">
      <DinDoutCard />
      <GpioCard />
      <IoExpCard />
    </div>
  );
}
