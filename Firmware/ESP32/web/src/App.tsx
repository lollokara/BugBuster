import { useEffect } from "preact/hooks";
import { signal } from "@preact/signals";
import { api, PairingRequiredError, getCachedToken } from "./api/client";
import {
  deviceInfo,
  pairingInfo,
  pairingRequired,
  deviceStatus,
  deviceMac,
} from "./state/signals";
import { PairingModal } from "./components/PairingModal";
import { Overview } from "./tabs/overview/Overview";
import { ScopePanel } from "./tabs/scope/ScopePanel";
import { Analog } from "./tabs/analog/Analog";
import { Digital } from "./tabs/digital/Digital";
import { SignalPath } from "./tabs/signal/SignalPath";
import { System } from "./tabs/system/System";

type TabId =
  | "overview"
  | "scope"
  | "analog"
  | "digital"
  | "signal"
  | "system";

const TABS: { id: TabId; label: string }[] = [
  { id: "overview", label: "Overview" },
  { id: "scope", label: "Scope" },
  { id: "analog", label: "Analog" },
  { id: "digital", label: "Digital" },
  { id: "signal", label: "Signal Path" },
  { id: "system", label: "System" },
];

const activeTab = signal<TabId>("overview");

export function App() {
  // Bootstrap: fetch device + pairing info once, then open the pairing modal
  // if we don't already have a cached admin token for this device's MAC.
  useEffect(() => {
    const boot = async () => {
      try {
        const [info, pairing] = await Promise.all([
          api.deviceInfo(),
          api.pairingInfo(),
        ]);
        deviceInfo.value = info;
        pairingInfo.value = pairing;
        const mac = pairing?.macAddress ?? info?.macAddress ?? null;
        if (mac && !getCachedToken(mac)) {
          pairingRequired.value = true;
        }
      } catch (e) {
        console.warn("bootstrap failed", e);
      }
    };
    boot();

    const onPairingRequired = () => {
      pairingRequired.value = true;
    };
    window.addEventListener("bb:pairing-required", onPairingRequired);
    return () => window.removeEventListener("bb:pairing-required", onPairingRequired);
  }, []);

  // Poll /api/status at 2 Hz for the header status + Overview sparklines.
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const s = await api.status();
        deviceStatus.value = s;
      } catch (e) {
        if (!(e instanceof PairingRequiredError)) {
          console.warn("status poll failed", e);
        }
      }
      if (alive) setTimeout(tick, 500);
    };
    tick();
    return () => { alive = false; };
  }, []);

  const spiOk = deviceStatus.value?.spiOk ?? deviceStatus.value?.spi_ok ?? false;
  const mac = deviceMac.value ?? "--";
  const fp = pairingInfo.value?.tokenFingerprint;

  return (
    <div class="app-shell">
      <header class="header">
        <div class="header-left">
          <div class="logo-text">BugBuster</div>
          <div class="subtitle">ESP32-S3 · on-device UI</div>
        </div>
        <div class="header-right">
          <span class={"status-dot " + (spiOk ? "ok" : "err")} />
          <span class="text-dim mono">{mac}</span>
          {fp && <span class="uppercase-tag">#{fp}</span>}
        </div>
      </header>

      <nav class="tab-bar">
        {TABS.map((t) => (
          <button
            key={t.id}
            class={"tab-item" + (activeTab.value === t.id ? " active" : "")}
            onClick={() => (activeTab.value = t.id)}
          >
            {t.label}
          </button>
        ))}
      </nav>

      <main class="content">
        {activeTab.value === "overview" && <Overview />}
        {activeTab.value === "scope" && <ScopePanel />}
        {activeTab.value === "analog" && <Analog />}
        {activeTab.value === "digital" && <Digital />}
        {activeTab.value === "signal" && <SignalPath />}
        {activeTab.value === "system" && <System />}
      </main>

      {pairingRequired.value && <PairingModal />}
    </div>
  );
}
