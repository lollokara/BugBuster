import { useEffect, useMemo, useState } from "preact/hooks";
import { api, PairingRequiredError } from "../api/client";
import { BarGauge } from "./BarGauge";
import { BigValue } from "./BigValue";
import { GlassCard } from "./GlassCard";
function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
}
function estimateCode(ch, voltage) {
    const midpoint = Number(ch?.midpointV ?? (Number(ch?.vMin) + Number(ch?.vMax)) / 2);
    const step = Number(ch?.stepMv ?? 0) / 1000;
    if (!Number.isFinite(midpoint) || !Number.isFinite(step) || step <= 0) {
        return Number(ch?.code ?? 0);
    }
    // DS4424 mapping on BugBuster rails is inverted:
    // negative DAC codes increase output voltage, positive codes decrease it.
    return clamp(Math.round((midpoint - voltage) / step), -127, 127);
}
function evalPoly(poly, code) {
    const arr = Array.isArray(poly) ? poly.map((v) => Number(v)) : [];
    if (arr.length < 4 || arr.some((v) => !Number.isFinite(v)))
        return NaN;
    // Firmware reports polynomial over normalized code cn = code / 127.
    const cn = code / 127.0;
    return arr[0] + arr[1] * cn + arr[2] * cn * cn + arr[3] * cn * cn * cn;
}
export function SupplySliderCard({ title, idacChannel, controlKey, enabled, measuredVoltage, idacChannelStatus, min, max, color, mac, invertSlider = false, }) {
    const currentTarget = Number(idacChannelStatus?.targetV ?? idacChannelStatus?.target ?? min);
    const [value, setValue] = useState(Number.isFinite(currentTarget) ? currentTarget : min);
    const [isDirty, setIsDirty] = useState(false);
    const [busy, setBusy] = useState(null);
    const [status, setStatus] = useState(null);
    // Only sync server state when the user is not actively editing the slider.
    useEffect(() => {
        if (!isDirty && Number.isFinite(currentTarget))
            setValue(clamp(currentTarget, min, max));
    }, [currentTarget, min, max, isDirty]);
    const previewState = useMemo(() => {
        if (!idacChannelStatus?.calibrated)
            return { kind: "uncal" };
        if (!idacChannelStatus?.polyValid)
            return { kind: "no-poly" };
        const code = estimateCode(idacChannelStatus, value);
        const v = evalPoly(idacChannelStatus?.calPoly, code);
        return Number.isFinite(v) ? { kind: "ok", v } : { kind: "no-poly" };
    }, [idacChannelStatus, value]);
    const apply = async () => {
        if (!mac)
            return;
        setBusy("apply");
        setStatus(null);
        try {
            await api.idacSetVoltage(mac, idacChannel, value);
            setStatus("Applied");
            setIsDirty(false);
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatus(e instanceof Error ? e.message : String(e));
            }
        }
        finally {
            setBusy(null);
        }
    };
    const toggleEnable = async () => {
        if (!mac)
            return;
        setBusy("enable");
        setStatus(null);
        try {
            await api.ioexp.setControl(mac, controlKey, !enabled);
        }
        catch (e) {
            if (!(e instanceof PairingRequiredError)) {
                setStatus(e instanceof Error ? e.message : String(e));
            }
        }
        finally {
            setBusy(null);
        }
    };
    const sliderValue = invertSlider ? min + max - value : value;
    const onSliderInput = (e) => {
        const raw = parseFloat(e.currentTarget.value);
        if (!Number.isFinite(raw))
            return;
        const mapped = invertSlider ? min + max - raw : raw;
        setValue(clamp(mapped, min, max));
        setIsDirty(true);
    };
    return (<GlassCard title={title} actions={<button class={"pill" + (enabled ? " active" : "")} disabled={!mac || busy !== null} onClick={toggleEnable}>
          {busy === "enable" ? "..." : enabled ? "ON" : "OFF"}
        </button>}>
      <BigValue value={enabled ? measuredVoltage : value} unit={enabled ? "V" : "V (Preview)"} precision={3}/>
      <BarGauge value={Number.isFinite(measuredVoltage) ? measuredVoltage : value} min={min} max={max} color={color}/>
      <div class="analog-row">
        <input type="range" min={min} max={max} step="0.001" value={String(sliderValue)} onInput={onSliderInput}/>
        <input class="input" type="number" min={min} max={max} step="0.001" value={value.toFixed(3)} style={{ maxWidth: "96px" }} onInput={(e) => { setValue(clamp(parseFloat(e.currentTarget.value || "0"), min, max)); setIsDirty(true); }}/>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">
          {previewState.kind === "no-poly" ? "Target" : "Preview"}
        </span>
        <span class={previewState.kind === "uncal" ? "text-warn" : "mono"}>
          {previewState.kind === "ok"
            ? `${previewState.v.toFixed(3)} V`
            : previewState.kind === "no-poly"
                ? `${value.toFixed(3)} V`
                : "Not calibrated"}
        </span>
      </div>
      <button class="btn primary" disabled={!mac || busy !== null} onClick={apply}>
        {busy === "apply" ? "Applying..." : "Apply"}
      </button>
      {status && <div class="text-dim" style={{ marginTop: "8px", fontSize: "0.78rem" }}>{status}</div>}
    </GlassCard>);
}
