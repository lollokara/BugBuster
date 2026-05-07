// =============================================================================
// OtaCard.tsx — Firmware + filesystem updates from the on-device web UI.
//
// Talks to /api/ota/{info,upload,uploadfs,rollback}. SHA-256 of the firmware
// is computed in the browser and verified by the firmware before the boot
// partition is switched, so a corrupted upload cannot brick the device.
// =============================================================================

import { useEffect, useRef, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { api, HttpError, OtaInfo } from "../../api/client";
import { deviceMac } from "../../state/signals";

type Stage = "idle" | "hashing" | "uploading" | "rebooting" | "error" | "done";

function fmtBytes(n: number | undefined): string {
  if (!n || n <= 0) return "—";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

export function OtaCard() {
  const mac = deviceMac.value;
  const [info, setInfo] = useState<OtaInfo | null>(null);
  const [stage, setStage] = useState<Stage>("idle");
  const [progress, setProgress] = useState({ sent: 0, total: 0 });
  const [message, setMessage] = useState<string>("");
  const [target, setTarget] = useState<"firmware" | "spiffs">("firmware");
  const fileRef = useRef<HTMLInputElement>(null);

  // Refresh the info snapshot on mount and after every action.
  const refresh = async () => {
    try {
      setInfo(await api.ota.info());
    } catch (e) {
      // Older firmware: no /api/ota/info endpoint. Surface a one-shot warning.
      setMessage(e instanceof HttpError && e.status === 404
        ? "Firmware predates /api/ota/info — upload still works"
        : `info: ${(e as Error).message}`);
    }
  };
  useEffect(() => { refresh(); }, []);

  const onUpload = async () => {
    if (!mac) { setMessage("Pair first"); setStage("error"); return; }
    const f = fileRef.current?.files?.[0];
    if (!f) { setMessage("Pick a .bin file"); setStage("error"); return; }

    setMessage(""); setProgress({ sent: 0, total: f.size });
    setStage(target === "firmware" ? "hashing" : "uploading");

    try {
      const fn = target === "firmware"
        ? api.ota.uploadFirmware
        : api.ota.uploadSpiffs;
      // For firmware, the api wrapper computes SHA-256 first (the "hashing"
      // stage) then transitions to streaming bytes — we just flip the stage
      // when the first progress event lands.
      const r = await fn(mac, f, (sent, total) => {
        if (stage !== "uploading") setStage("uploading");
        setProgress({ sent, total });
      });
      if (target === "firmware") {
        setStage("rebooting");
        setMessage(
          `Wrote ${fmtBytes(r.bytesWritten)} to ${r.partition ?? "?"}` +
          (r.sha256Verified ? " (SHA-256 verified)" : "") +
          " — device is rebooting…",
        );
        // Give the device ~12 s to reboot before re-fetching info.
        setTimeout(() => { refresh(); setStage("done"); }, 12000);
      } else {
        setStage("done");
        setMessage(`SPIFFS: wrote ${fmtBytes(f.size)}.`);
        refresh();
      }
    } catch (e) {
      setStage("error");
      setMessage((e as Error).message);
    }
  };

  const onRollback = async () => {
    if (!mac) { setMessage("Pair first"); setStage("error"); return; }
    if (!confirm("Roll back to the previous OTA slot? Device will reboot."))
      return;
    setMessage(""); setStage("rebooting");
    try {
      await api.ota.rollback(mac);
      setMessage("Rollback initiated — device is rebooting…");
      setTimeout(() => { refresh(); setStage("done"); }, 12000);
    } catch (e) {
      setStage("error");
      setMessage((e as Error).message);
    }
  };

  const pct = progress.total
    ? Math.min(100, Math.round((progress.sent / progress.total) * 100))
    : 0;

  return (
    <GlassCard title="Firmware Update (OTA)">
      <div class="kv-row">
        <span class="text-dim">Running:</span>
        <span class="mono">
          {info?.running?.label ?? "—"}
          {info?.running?.state ? ` · ${info.running.state}` : ""}
        </span>
        <span class="text-dim">FW:</span>
        <span class="mono">
          {info ? `${info.fwMajor}.${info.fwMinor}.${info.fwPatch}` : "—"}
        </span>
      </div>
      <div class="kv-row">
        <span class="text-dim">Next slot:</span>
        <span class="mono">
          {info?.next?.label ?? "—"}
          {info?.next?.size ? ` (${fmtBytes(info.next.size)})` : ""}
        </span>
        <span class="text-dim">Rollback:</span>
        <span class="mono">{info?.canRollback ? "available" : "—"}</span>
      </div>

      <div class="kv-row" style="margin-top:0.5rem;">
        <select
          class="input"
          value={target}
          onChange={(e) =>
            setTarget((e.currentTarget as HTMLSelectElement).value as "firmware" | "spiffs")
          }
        >
          <option value="firmware">firmware.bin (app partition)</option>
          <option value="spiffs">spiffs.bin (web UI)</option>
        </select>
        <input
          ref={fileRef}
          class="input"
          type="file"
          accept=".bin"
          disabled={stage === "uploading" || stage === "hashing" || stage === "rebooting"}
        />
        <button
          class="btn primary"
          onClick={onUpload}
          disabled={!mac || stage === "uploading" || stage === "hashing" || stage === "rebooting"}
        >
          {stage === "hashing"
            ? "Hashing…"
            : stage === "uploading"
              ? `Uploading ${pct}%`
              : stage === "rebooting"
                ? "Rebooting…"
                : "Upload"}
        </button>
        <button
          class="btn"
          onClick={onRollback}
          disabled={!mac || !info?.canRollback || stage === "rebooting"}
          title={info?.canRollback ? "Roll back to the previous OTA slot" : "No rollback target"}
        >
          Rollback
        </button>
      </div>

      {(stage === "uploading" || stage === "hashing") && (
        <div class="kv-row">
          <progress max={100} value={pct} style="width:100%" />
          <span class="mono text-dim">
            {fmtBytes(progress.sent)} / {fmtBytes(progress.total)}
          </span>
        </div>
      )}

      {message && (
        <div class={stage === "error" ? "text-warn" : "text-dim"}>{message}</div>
      )}
    </GlassCard>
  );
}
