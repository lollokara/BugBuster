// =============================================================================
// PairingModal — collects the 64-hex-char admin token, verifies via API.
// On success closes itself; token is cached (sessionStorage by default,
// localStorage if "Remember on this device" is checked).
// =============================================================================

import { useState } from "preact/hooks";
import {
  api,
  setCachedToken,
  isPersistentlyRemembered,
} from "../api/client";
import { pairingInfo, pairingRequired, deviceMac } from "../state/signals";

const TOKEN_RE = /^[0-9a-f]{64}$/i;

export function PairingModal() {
  const [token, setToken] = useState("");
  const [busy, setBusy] = useState(false);
  const [rotating, setRotating] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const info = pairingInfo.value;
  const mac = deviceMac.value ?? info?.macAddress ?? "—";
  const fp = info?.tokenFingerprint;
  const transport = info?.transport ?? "http";

  // Default the "remember" toggle to whatever this device already has so the
  // user's existing preference is sticky if they re-pair.
  const [remember, setRemember] = useState(() =>
    mac && mac !== "—" ? isPersistentlyRemembered(mac) : false,
  );

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
      const ok = await api.pairingVerify(mac, t, { remember });
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

  const onRotate = async () => {
    setError(null);
    if (!mac || mac === "—") {
      setError("Device MAC unknown — cannot rotate.");
      return;
    }
    if (
      !window.confirm(
        "Rotating will generate a fresh admin token on the device. " +
          "Any previously paired client must re-pair using the new token. " +
          "Continue?",
      )
    ) {
      return;
    }
    setRotating(true);
    try {
      const resp = await api.pairingRotate(mac);
      if (resp?.ok && typeof resp.token === "string" && TOKEN_RE.test(resp.token)) {
        // Cache the freshly-minted token so subsequent admin requests
        // succeed without re-pairing the current browser session.
        setCachedToken(mac, resp.token, { remember });
        // Re-fetch pairing info so the displayed fingerprint matches.
        try {
          pairingInfo.value = await api.pairingInfo();
        } catch {
          /* non-fatal — UI just keeps showing the previous fingerprint */
        }
        pairingRequired.value = false;
        setToken("");
      } else {
        setError("Rotation succeeded but the response was malformed.");
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setRotating(false);
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
              <span class="mono">921600&nbsp;8N1</span>.
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
            disabled={busy || rotating}
          />

          <label
            style={{
              display: "flex",
              alignItems: "center",
              gap: "8px",
              marginTop: "10px",
              fontSize: "0.82rem",
            }}
          >
            <input
              type="checkbox"
              checked={remember}
              disabled={busy || rotating}
              onChange={(e) =>
                setRemember((e.currentTarget as HTMLInputElement).checked)
              }
            />
            <span>
              Remember on this device
              <span class="text-dim" style={{ marginLeft: "6px" }}>
                (persists across browser-close — leave off for shared
                machines)
              </span>
            </span>
          </label>

          {error && (
            <div class="text-err" style={{ fontSize: "0.8rem", marginTop: "8px" }}>
              {error}
            </div>
          )}

          <div class="modal-actions">
            <button
              type="button"
              class="btn"
              onClick={onCancel}
              disabled={busy || rotating}
            >
              Cancel
            </button>
            <button
              type="button"
              class="btn"
              onClick={onRotate}
              disabled={busy || rotating || !fp /* requires existing pairing */}
              title={
                fp
                  ? "Generate a fresh token on the device"
                  : "Rotate is only available when the device already holds a paired token"
              }
            >
              {rotating ? "Rotating…" : "Rotate token"}
            </button>
            <button
              type="submit"
              class="btn primary"
              disabled={busy || rotating}
            >
              {busy ? "Verifying…" : "Verify"}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
