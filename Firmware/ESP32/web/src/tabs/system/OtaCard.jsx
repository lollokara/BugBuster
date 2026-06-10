// =============================================================================
// OtaCard.tsx — Firmware + filesystem updates from the on-device web UI.
//
// Talks to /api/ota/{info,upload,uploadfs,rollback}. SHA-256 of the firmware
// is computed in the browser and verified by the firmware before the boot
// partition is switched, so a corrupted upload cannot brick the device.
// =============================================================================
import { useEffect, useRef, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { api, HttpError } from "../../api/client";
import { deviceMac } from "../../state/signals";
function fmtBytes(n) {
    if (!n || n <= 0)
        return "—";
    if (n < 1024)
        return `${n} B`;
    if (n < 1024 * 1024)
        return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / 1024 / 1024).toFixed(2)} MB`;
}
export function GitOtaCard() {
    const mac = deviceMac.value;
    const [gitStage, setGitStage] = useState("idle");
    const [checkResult, setCheckResult] = useState(null);
    const [applyRp, setApplyRp] = useState(false);
    const [applyEsp, setApplyEsp] = useState(false);
    const [status, setStatus] = useState(null);
    const [msg, setMsg] = useState("");
    const pollRef = useRef(null);
    const stopPoll = () => {
        if (pollRef.current) {
            clearInterval(pollRef.current);
            pollRef.current = null;
        }
    };
    useEffect(() => () => stopPoll(), []);
    const onCheck = async () => {
        if (!mac) {
            setMsg("Pair first");
            return;
        }
        setGitStage("checking");
        setMsg("");
        setCheckResult(null);
        try {
            const r = await api.update.check();
            setCheckResult(r);
            setApplyRp(r.rp2040.newer);
            setApplyEsp(r.esp32.newer);
            setGitStage("idle");
            if (!r.rp2040.newer && !r.esp32.newer)
                setMsg("Both components are up to date.");
        }
        catch (e) {
            setGitStage("error");
            setMsg(e.message);
        }
    };
    const onApply = async () => {
        if (!mac || (!applyRp && !applyEsp))
            return;
        setGitStage("applying");
        setMsg("");
        try {
            await api.update.apply(applyRp, applyEsp);
            // Poll status until idle/error
            pollRef.current = setInterval(async () => {
                try {
                    const s = await api.update.status();
                    setStatus(s);
                    if (s.step === "idle" || s.lastError) {
                        stopPoll();
                        setGitStage(s.lastError ? "error" : "done");
                        setMsg(s.lastError || "Update applied. Device may reboot.");
                    }
                }
                catch {
                    stopPoll();
                    setGitStage("error");
                    setMsg("Status poll failed.");
                }
            }, 1500);
        }
        catch (e) {
            stopPoll();
            setGitStage("error");
            setMsg(e.message);
        }
    };
    const busy = gitStage === "checking" || gitStage === "applying";
    const pct = status?.progressTotal
        ? Math.min(100, Math.round((status.progressDone / status.progressTotal) * 100))
        : 0;
    return (<GlassCard title="GitHub Firmware Update">
      {/* Current versions row */}
      {status && (<div class="kv-row">
          <span class="text-dim">ESP32:</span>
          <span class="mono">{status.currentEsp32 || "—"}</span>
          <span class="text-dim">RP2040:</span>
          <span class="mono">{status.currentRp2040 || "—"}</span>
        </div>)}

      {/* Check result */}
      {checkResult && (<div style={{ display: "flex", flexDirection: "column", gap: "6px", margin: "6px 0" }}>
          {["esp32", "rp2040"].map((key) => {
                const c = checkResult[key];
                const sel = key === "esp32" ? applyEsp : applyRp;
                const setSel = key === "esp32" ? setApplyEsp : setApplyRp;
                return (<div class="kv-row" key={key} style={{ flexWrap: "wrap", gap: "6px" }}>
                <span class="uppercase-tag">{key === "esp32" ? "ESP32" : "RP2040"}</span>
                {c.available ? (<>
                    <span class="mono">{c.version || c.availableBuildId}</span>
                    <span class={c.newer ? "pill active" : "pill"} style={{ fontSize: "0.7rem" }}>
                      {c.newer ? "Update available" : "Up to date"}
                    </span>
                    {c.newer && (<label style={{ display: "flex", alignItems: "center", gap: "4px", cursor: "pointer" }}>
                        <input type="checkbox" checked={sel} disabled={busy} onChange={(e) => setSel(e.currentTarget.checked)}/>
                        <span style={{ fontSize: "0.78rem" }}>Install</span>
                      </label>)}
                  </>) : (<span class="text-dim">Not in nightly manifest</span>)}
              </div>);
            })}
        </div>)}

      {/* Progress */}
      {gitStage === "applying" && status && (status.progressTotal ?? 0) > 0 && (<div class="kv-row">
          <progress max={100} value={pct} style="width:100%"/>
          <span class="mono text-dim">{status?.step} {pct}%</span>
        </div>)}

      {msg && <div class={gitStage === "error" ? "text-warn" : "text-dim"}>{msg}</div>}

      <div class="kv-row" style={{ marginTop: "6px" }}>
        <button class="btn" onClick={onCheck} disabled={busy || !mac}>
          {gitStage === "checking" ? "Checking…" : "Check for updates"}
        </button>
        {checkResult && (applyRp || applyEsp) && (<button class="btn primary" onClick={onApply} disabled={busy || !mac || (!applyRp && !applyEsp)}>
            {gitStage === "applying" ? `${status?.step ?? "Applying"}…` : "Apply update"}
          </button>)}
      </div>
    </GlassCard>);
}
export function OtaCard() {
    const mac = deviceMac.value;
    const [info, setInfo] = useState(null);
    const [stage, setStage] = useState("idle");
    const [progress, setProgress] = useState({ sent: 0, total: 0 });
    const [message, setMessage] = useState("");
    const [target, setTarget] = useState("firmware");
    const fileRef = useRef(null);
    // Refresh the info snapshot on mount and after every action.
    const refresh = async () => {
        try {
            setInfo(await api.ota.info());
        }
        catch (e) {
            // Older firmware: no /api/ota/info endpoint. Surface a one-shot warning.
            setMessage(e instanceof HttpError && e.status === 404
                ? "Firmware predates /api/ota/info — upload still works"
                : `info: ${e.message}`);
        }
    };
    useEffect(() => { refresh(); }, []);
    const onUpload = async () => {
        if (!mac) {
            setMessage("Pair first");
            setStage("error");
            return;
        }
        const f = fileRef.current?.files?.[0];
        if (!f) {
            setMessage("Pick a .bin file");
            setStage("error");
            return;
        }
        setMessage("");
        setProgress({ sent: 0, total: f.size });
        setStage(target === "firmware" ? "hashing" : "uploading");
        try {
            const fn = target === "firmware"
                ? api.ota.uploadFirmware
                : api.ota.uploadSpiffs;
            // For firmware, the api wrapper computes SHA-256 first (the "hashing"
            // stage) then transitions to streaming bytes — we just flip the stage
            // when the first progress event lands.
            const r = await fn(mac, f, (sent, total) => {
                if (stage !== "uploading")
                    setStage("uploading");
                setProgress({ sent, total });
            });
            if (target === "firmware") {
                setStage("rebooting");
                setMessage(`Wrote ${fmtBytes(r.bytesWritten)} to ${r.partition ?? "?"}` +
                    (r.sha256Verified ? " (SHA-256 verified)" : "") +
                    " — device is rebooting…");
                // Give the device ~12 s to reboot before re-fetching info.
                setTimeout(() => { refresh(); setStage("done"); }, 12000);
            }
            else {
                setStage("done");
                setMessage(`SPIFFS: wrote ${fmtBytes(f.size)}.`);
                refresh();
            }
        }
        catch (e) {
            setStage("error");
            setMessage(e.message);
        }
    };
    const onRollback = async () => {
        if (!mac) {
            setMessage("Pair first");
            setStage("error");
            return;
        }
        if (!confirm("Roll back to the previous OTA slot? Device will reboot."))
            return;
        setMessage("");
        setStage("rebooting");
        try {
            await api.ota.rollback(mac);
            setMessage("Rollback initiated — device is rebooting…");
            setTimeout(() => { refresh(); setStage("done"); }, 12000);
        }
        catch (e) {
            setStage("error");
            setMessage(e.message);
        }
    };
    const pct = progress.total
        ? Math.min(100, Math.round((progress.sent / progress.total) * 100))
        : 0;
    return (<GlassCard title="Firmware Update (OTA)">
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
        <select class="input" value={target} onChange={(e) => setTarget(e.currentTarget.value)}>
          <option value="firmware">firmware.bin (app partition)</option>
          <option value="spiffs">spiffs.bin (web UI)</option>
        </select>
        <input ref={fileRef} class="input" type="file" accept=".bin" disabled={stage === "uploading" || stage === "hashing" || stage === "rebooting"}/>
        <button class="btn primary" onClick={onUpload} disabled={!mac || stage === "uploading" || stage === "hashing" || stage === "rebooting"}>
          {stage === "hashing"
            ? "Hashing…"
            : stage === "uploading"
                ? `Uploading ${pct}%`
                : stage === "rebooting"
                    ? "Rebooting…"
                    : "Upload"}
        </button>
        <button class="btn" onClick={onRollback} disabled={!mac || !info?.canRollback || stage === "rebooting"} title={info?.canRollback ? "Roll back to the previous OTA slot" : "No rollback target"}>
          Rollback
        </button>
      </div>

      {(stage === "uploading" || stage === "hashing") && (<div class="kv-row">
          <progress max={100} value={pct} style="width:100%"/>
          <span class="mono text-dim">
            {fmtBytes(progress.sent)} / {fmtBytes(progress.total)}
          </span>
        </div>)}

      {message && (<div class={stage === "error" ? "text-warn" : "text-dim"}>{message}</div>)}
    </GlassCard>);
}
