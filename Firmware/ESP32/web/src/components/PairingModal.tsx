// =============================================================================
// PairingModal — collects the 64-hex-char admin token, verifies via API.
// On success closes itself; token is cached in localStorage by the API.
// =============================================================================

import { useState } from "preact/hooks";
import { api } from "../api/client";
import { pairingInfo, pairingRequired, deviceMac } from "../state/signals";

const TOKEN_RE = /^[0-9a-f]{64}$/i;

export function PairingModal() {
  const [token, setToken] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const info = pairingInfo.value;
  const mac = deviceMac.value ?? info?.macAddress ?? "—";
  const fp = info?.tokenFingerprint;
  const transport = info?.transport ?? "http";

  const onSubmit = async (e: Event) => {
    e.preventDefault();
    setError(null);
    const t = token.trim().toLowerCase();
    if (!TOKEN_RE.test(t)) {
      setError("Token must be exactly 64 hex characters.");
      return;
    }
    if (!mac || mac === "—") {
      setError("Device MAC unknown — cannot verify.");
      return;
    }
    setBusy(true);
    try {
      const ok = await api.pairingVerify(mac, t);
      if (ok) {
        pairingRequired.value = false;
        setToken("");
      } else {
        setError("Token rejected by device (401).");
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  };

  const onCancel = () => {
    pairingRequired.value = false;
    setError(null);
  };

  return (
    <div class="modal-backdrop" role="dialog" aria-modal="true">
      <div class="modal">
        <h2>Pair with device</h2>
        <p class="text-dim" style={{ fontSize: "0.82rem", marginBottom: "12px" }}>
          Mutating endpoints require a 64-character admin token. Read-only
          panels (status, scope, board list) remain available without it.
        </p>

        <div class="pair-meta">
          <div class="pair-meta-row">
            <span class="uppercase-tag">MAC</span>
            <span class="mono">{mac}</span>
          </div>
          <div class="pair-meta-row">
            <span class="uppercase-tag">Fingerprint</span>
            <span class="mono">{fp ?? "—"}</span>
          </div>
          <div class="pair-meta-row">
            <span class="uppercase-tag">Transport</span>
            <span class="mono">{transport}</span>
          </div>
        </div>

        <div class="pair-howto">
          <div class="uppercase-tag" style={{ marginBottom: "6px" }}>
            How to get your token
          </div>
          <ol>
            <li>
              Plug the ESP32 into your computer via USB and open any serial
              terminal (<span class="mono">screen</span>,{" "}
              <span class="mono">minicom</span>,{" "}
              <span class="mono">pio device monitor</span>, Arduino Serial
              Monitor…) on the <span class="mono">CDC0</span> port at{" "}
              <span class="mono">115200&nbsp;8N1</span>.
            </li>
            <li>
              At the <span class="mono">&gt;</span> prompt, type{" "}
              <span class="mono pair-cmd">token</span> and press{" "}
              <span class="mono">Enter</span>.
            </li>
            <li>
              Copy the 64-character hex string and paste it below. The
              fingerprint above must match the{" "}
              <span class="mono">#&hellip;</span> printed by the command.
            </li>
          </ol>
        </div>

        <form onSubmit={onSubmit}>
          <input
            class="input"
            type="password"
            autocomplete="off"
            spellcheck={false}
            placeholder="64 hex characters"
            value={token}
            onInput={(e) => setToken((e.currentTarget as HTMLInputElement).value)}
            disabled={busy}
          />
          {error && (
            <div class="text-err" style={{ fontSize: "0.8rem", marginTop: "8px" }}>
              {error}
            </div>
          )}

          <div class="modal-actions">
            <button type="button" class="btn" onClick={onCancel} disabled={busy}>
              Cancel
            </button>
            <button type="submit" class="btn primary" disabled={busy}>
              {busy ? "Verifying…" : "Verify"}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
