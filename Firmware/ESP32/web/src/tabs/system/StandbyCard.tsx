import { useState } from "preact/hooks";
import { usePoll } from "../../hooks/usePoll";
import { GlassCard } from "../../components/GlassCard";
import { api, PairingRequiredError } from "../../api/client";
import type { StandbyStatus } from "../../api/client";
import { deviceMac } from "../../state/signals";
import { isUnsupported, standbyPresence, standbyStatus } from "../../state/standby";
import { TIMEOUT_OPTIONS, idleLine, stateLabel } from "./standbyView";

export function StandbyCard() {
  const mac = deviceMac.value;
  const [unsupported, setUnsupported] = useState(false);
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const [released, setReleased] = useState(false);

  const polled = usePoll<StandbyStatus>(() => api.standby.status(), 2000, {
    enabled: !unsupported,
    onError: (e) => { if (isUnsupported(e)) setUnsupported(true); },
  });
  const status = polled ?? standbyStatus.value;

  const run = async (fn: () => Promise<StandbyStatus>) => {
    setBusy(true);
    setNote(null);
    try {
      standbyStatus.value = await fn();
    } catch (e) {
      if (isUnsupported(e)) setUnsupported(true);
      else if (!(e instanceof PairingRequiredError)) setNote(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  const setTimeoutSeconds = (seconds: number) => {
    if (!mac) return;
    void run(() => api.standby.setPolicy(mac, seconds));
  };

  const wake = () => {
    standbyPresence.attach();
    setReleased(false);
    void run(() => api.standby.wake());
  };

  const sleep = () => {
    if (!mac) return;
    if (!confirm("Enter standby now? VADJ1/VADJ2, VDUT and analog rails switch off and all MUX routes open. Outputs stay off after wake.")) return;
    standbyPresence.detach();
    setReleased(true);
    void run(() => api.standby.sleep(mac));
  };

  if (unsupported) {
    return (
      <GlassCard title="Standby">
        <div class="text-dim">This firmware has no system standby.</div>
      </GlassCard>
    );
  }

  return (
    <GlassCard title="Standby">
      <div class="kv-row">
        <span class="uppercase-tag">State</span>
        <span class="mono">{stateLabel(status)}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Timer</span>
        <span class="mono">{idleLine(status)}</span>
      </div>
      <div class="kv-row" style={{ gap: "8px" }}>
        <span class="uppercase-tag">Auto standby</span>
        {TIMEOUT_OPTIONS.map((o) => (
          <button
            key={o.seconds}
            class={"pill" + (status?.timeoutSeconds === o.seconds ? " active" : "")}
            disabled={!mac || busy}
            onClick={() => setTimeoutSeconds(o.seconds)}
          >
            {o.label}
          </button>
        ))}
      </div>
      <div class="kv-row" style={{ gap: "8px" }}>
        <button class="btn" disabled={busy} onClick={wake}>Wake</button>
        <button class="btn" disabled={!mac || busy} onClick={sleep}>Sleep now</button>
      </div>
      {released && (
        <div class="text-dim">This tab released its connection and will not reconnect until you press Wake.</div>
      )}
      {note && <div class="text-dim">{note}</div>}
    </GlassCard>
  );
}
