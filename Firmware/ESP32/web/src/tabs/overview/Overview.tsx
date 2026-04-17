// =============================================================================
// Overview tab — 2x2 channel tiles + pairing card + supply rails strip.
// =============================================================================

import { useEffect } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { BigValue } from "../../components/BigValue";
import { BarGauge } from "../../components/BarGauge";
import { Sparkline } from "../../components/Sparkline";
import { CH_COLORS } from "../../scope/ScopeCanvas";
import {
  deviceStatus,
  deviceInfo,
  pairingInfo,
  channelSparks,
  pushChannelSamples,
} from "../../state/signals";

const CH_NAMES = ["A", "B", "C", "D"] as const;

// AD74416H CH_FUNC codes (subset). Keep labels short.
// TODO: verify shape from firmware
const CH_FUNC_LABEL: Record<number, string> = {
  0: "HI-Z",
  1: "VOUT",
  2: "IOUT",
  3: "VIN",
  4: "IIN-LOOP",
  5: "EXT-RTD-2W",
  6: "EXT-RTD-3W",
  7: "IIN-EXT-PWR",
  8: "RES-MEAS",
  9: "DIN-LOGIC",
  10: "DIN-LOOP",
};

function funcLabel(code: number | undefined, fallback?: string): string {
  if (typeof code === "number" && CH_FUNC_LABEL[code]) return CH_FUNC_LABEL[code]!;
  if (fallback) return fallback;
  if (typeof code === "number") return `CH_FUNC_${code}`;
  return "—";
}

export function Overview() {
  const status = deviceStatus.value;
  const info = deviceInfo.value;
  const pairing = pairingInfo.value;

  // Feed sparkline ring whenever status updates with fresh ADC values.
  useEffect(() => {
    if (!status || !Array.isArray(status.channels)) return;
    const arr = status.channels;
    const vals: [number, number, number, number] = [
      Number(arr[0]?.adcValue ?? arr[0]?.adc_value ?? 0),
      Number(arr[1]?.adcValue ?? arr[1]?.adc_value ?? 0),
      Number(arr[2]?.adcValue ?? arr[2]?.adc_value ?? 0),
      Number(arr[3]?.adcValue ?? arr[3]?.adc_value ?? 0),
    ];
    pushChannelSamples(vals);
  }, [status]);

  const channels = Array.isArray(status?.channels) ? status.channels : [];
  const diagnostics = status?.diagnostics ?? {};
  const dieTemp = Number(diagnostics.dieTemp ?? diagnostics.die_temp ?? status?.dieTemp ?? NaN);

  // Supply rails — tolerate various shapes from firmware.
  // TODO: verify shape from firmware
  const vadj1 = Number(
    diagnostics.vadj1 ?? status?.vadj1 ?? status?.rails?.vadj1 ?? NaN,
  );
  const vadj2 = Number(
    diagnostics.vadj2 ?? status?.vadj2 ?? status?.rails?.vadj2 ?? NaN,
  );
  const vbus = Number(
    diagnostics.vbus ?? diagnostics.usbVbus ?? status?.usbpd?.vbus ?? NaN,
  );

  const fwStr = info
    ? (info as any).fwMajor !== undefined
      ? `${(info as any).fwMajor}.${(info as any).fwMinor}.${(info as any).fwPatch}`
      : `silicon ${info.siliconRev}`
    : "—";

  const sparks = channelSparks.value;

  return (
    <div class="overview-grid">
      <div class="overview-channels">
        {[0, 1, 2, 3].map((i) => {
          const ch = channels[i] ?? {};
          const funcCode = Number(ch.functionCode ?? ch.function_code ?? ch.function ?? -1);
          const funcName = funcLabel(funcCode, ch.function);
          const adc = Number(ch.adcValue ?? ch.adc_value ?? NaN);
          return (
            <GlassCard key={i} title={`Channel ${CH_NAMES[i]}`}>
              <div class="ch-tile-head">
                <span class="ch-swatch" style={{ color: CH_COLORS[i], background: CH_COLORS[i] }} />
                <span class="uppercase-tag">{funcName}</span>
              </div>
              <BigValue value={adc} unit="V" precision={3} />
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
