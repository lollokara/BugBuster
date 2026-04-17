// =============================================================================
// Overview tab — 2x2 channel tiles + pairing card + supply rails strip.
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { BigValue } from "../../components/BigValue";
import { BarGauge } from "../../components/BarGauge";
import { Sparkline } from "../../components/Sparkline";
import { CH_COLORS } from "../../scope/ScopeCanvas";
import { api, PairingRequiredError } from "../../api/client";
import {
  CHANNEL_FUNCTION_LABELS,
  CHANNEL_FUNCTION_OPTIONS,
} from "../../config/options";
import {
  deviceStatus,
  deviceInfo,
  pairingInfo,
  deviceMac,
  channelSparks,
  pushChannelSamples,
} from "../../state/signals";

const CH_NAMES = ["A", "B", "C", "D"] as const;
const OUTPUT_FUNCTION_CODES = new Set([1, 2, 10]);

function functionCodeFromChannel(ch: any): number {
  const raw = ch?.functionCode ?? ch?.function_code;
  if (typeof raw === "number" && Number.isFinite(raw)) return raw;
  const fn = String(ch?.function ?? "").toUpperCase();
  if (fn === "VOUT") return 1;
  if (fn === "IOUT") return 2;
  if (fn === "IOUT_HART") return 10;
  return -1;
}

function displayValueFromChannel(ch: any, funcCode: number): number {
  const adc = Number(ch?.adcValue ?? ch?.adc_value ?? NaN);
  const dac = Number(ch?.dacValue ?? ch?.dac_value ?? NaN);
  if (OUTPUT_FUNCTION_CODES.has(funcCode)) {
    return Number.isFinite(dac) ? dac : adc;
  }
  return adc;
}

function funcLabel(code: number | undefined, fallback?: string): string {
  if (typeof code === "number" && CHANNEL_FUNCTION_LABELS[code]) {
    return CHANNEL_FUNCTION_LABELS[code]!;
  }
  if (fallback) return fallback;
  if (typeof code === "number") return `CH_FUNC_${code}`;
  return "—";
}

export function Overview() {
  const status = deviceStatus.value;
  const info = deviceInfo.value;
  const pairing = pairingInfo.value;
  const mac = deviceMac.value;
  const [busy, setBusy] = useState<number | null>(null);
  const [vadj1, setVadj1] = useState<number>(NaN);
  const [vadj2, setVadj2] = useState<number>(NaN);
  const [vbus, setVbus] = useState<number>(NaN);

  // Feed sparkline ring whenever status updates with fresh ADC values.
  useEffect(() => {
    if (!status || !Array.isArray(status.channels)) return;
    const arr = status.channels;
    const vals: [number, number, number, number] = [
      displayValueFromChannel(arr[0], functionCodeFromChannel(arr[0])),
      displayValueFromChannel(arr[1], functionCodeFromChannel(arr[1])),
      displayValueFromChannel(arr[2], functionCodeFromChannel(arr[2])),
      displayValueFromChannel(arr[3], functionCodeFromChannel(arr[3])),
    ];
    pushChannelSamples(vals);
  }, [status]);

  const channels = Array.isArray(status?.channels) ? status.channels : [];
  const diagnostics = status?.diagnostics ?? {};
  const dieTemp = Number(diagnostics.dieTemp ?? diagnostics.die_temp ?? status?.dieTemp ?? NaN);

  // Supply rails are fetched from dedicated endpoints.
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const [s0, s1, pd] = await Promise.allSettled([
          api.selftestSupply(0),
          api.selftestSupply(1),
          api.usbpd(),
        ]);
        if (!alive) return;
        if (s0.status === "fulfilled") {
          setVadj1(Number((s0.value as any)?.voltage ?? NaN));
        }
        if (s1.status === "fulfilled") {
          setVadj2(Number((s1.value as any)?.voltage ?? NaN));
        }
        if (pd.status === "fulfilled") {
          setVbus(Number((pd.value as any)?.voltageV ?? NaN));
        }
      } catch {
        /* ignore transient poll failures */
      }
      if (alive) setTimeout(tick, 2500);
    };
    tick();
    return () => {
      alive = false;
    };
  }, []);

  const fwStr = info
    ? (info as any).fwMajor !== undefined
      ? `${(info as any).fwMajor}.${(info as any).fwMinor}.${(info as any).fwPatch}`
      : `silicon ${info.siliconRev}`
    : "—";

  const sparks = channelSparks.value;
  const setFunction = async (ch: number, func: number) => {
    if (!mac) return;
    setBusy(ch);
    try {
      await api.channel.setFunction(mac, ch, func);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        console.warn("setFunction failed", e);
      }
    } finally {
      setBusy(null);
    }
  };

  return (
    <div class="overview-grid">
      <div class="overview-channels">
        {[0, 1, 2, 3].map((i) => {
          const ch = channels[i] ?? {};
          const funcCode = functionCodeFromChannel(ch);
          const funcName = funcLabel(funcCode, ch.function);
          const displayValue = displayValueFromChannel(ch, funcCode);
          return (
            <GlassCard key={i} title={`Channel ${CH_NAMES[i]}`}>
              <div class="ch-tile-head">
                <span class="ch-swatch" style={{ color: CH_COLORS[i], background: CH_COLORS[i] }} />
                <span class="uppercase-tag">{funcName}</span>
              </div>
              <div class="analog-row" style={{ marginTop: "6px" }}>
                <label class="uppercase-tag">Function</label>
                <select
                  class="input"
                  value={String(Number.isFinite(funcCode) ? funcCode : 0)}
                  disabled={!mac || busy === i}
                  onChange={(e) =>
                    setFunction(i, parseInt((e.currentTarget as HTMLSelectElement).value, 10))
                  }
                >
                  {CHANNEL_FUNCTION_OPTIONS.map((opt) => (
                    <option key={opt.code} value={String(opt.code)}>
                      {opt.label}
                    </option>
                  ))}
                </select>
              </div>
              <BigValue value={displayValue} unit="V" precision={3} />
              <div style={{ marginTop: "10px" }}>
                <Sparkline values={sparks[i] ?? []} color={CH_COLORS[i]} height={56} />
              </div>
            </GlassCard>
          );
        })}
      </div>

      <GlassCard title="Pairing" class="overview-pairing">
        <div class="kv-row">
          <span class="uppercase-tag">MAC</span>
          <span class="mono">{pairing?.macAddress ?? info?.macAddress ?? "—"}</span>
        </div>
        <div class="kv-row">
          <span class="uppercase-tag">FW</span>
          <span class="mono">{fwStr}</span>
        </div>
        <div class="kv-row">
          <span class="uppercase-tag">Fingerprint</span>
          <span class="mono">{pairing?.tokenFingerprint ?? "—"}</span>
        </div>
        <div class="kv-row">
          <span class="uppercase-tag">Transport</span>
          <span class="mono">HTTP</span>
        </div>
        <div class="kv-row">
          <span class="uppercase-tag">SPI</span>
          <span class={info?.spiOk ? "text-ok" : "text-err"}>
            {info?.spiOk ? "OK" : "FAIL"}
          </span>
        </div>
        <div class="kv-row">
          <span class="uppercase-tag">Die °C</span>
          <span class="mono">{Number.isFinite(dieTemp) ? dieTemp.toFixed(1) : "—"}</span>
        </div>
      </GlassCard>

      <div class="overview-rails">
        <GlassCard title="VADJ1">
          <BigValue value={vadj1} unit="V" precision={3} />
          <BarGauge value={Number.isFinite(vadj1) ? vadj1 : 0} min={0} max={24} color="var(--blue)" />
        </GlassCard>
        <GlassCard title="VADJ2">
          <BigValue value={vadj2} unit="V" precision={3} />
          <BarGauge value={Number.isFinite(vadj2) ? vadj2 : 0} min={0} max={24} color="var(--green)" />
        </GlassCard>
        <GlassCard title="USB-PD VBUS">
          <BigValue value={vbus} unit="V" precision={2} />
          <BarGauge value={Number.isFinite(vbus) ? vbus : 0} min={0} max={20} color="var(--purple)" />
        </GlassCard>
      </div>
    </div>
  );
}
