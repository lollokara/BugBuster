import { useEffect } from "preact/hooks";
import { signal } from "@preact/signals";
import { lazy, Suspense } from "preact/compat";
import { api, PairingRequiredError, getCachedToken } from "./api/client";
import {
  deviceInfo,
  pairingInfo,
  pairingRequired,
  deviceStatus,
  deviceMac,
  deviceFailureStreak,
} from "./state/signals";
import { PairingModal } from "./components/PairingModal";
import { ErrorBoundary } from "./components/ErrorBoundary"; // WEB-8
import { Overview } from "./tabs/overview/Overview";

// Tabs other than Overview are dynamically imported so their JS only ships
// to the browser when the tab is first opened. Overview is the landing tab
// and stays in the entry bundle. Vite emits each lazy() target as its own
// chunk under assets/c-[hash].js (filename pattern in vite.config.ts is kept
// short to fit SPIFFS's 32-char path limit).
const ScopePanel = lazy(() => import("./tabs/scope/ScopePanel").then((m) => ({ default: m.ScopePanel })));
const Analog = lazy(() => import("./tabs/analog/Analog").then((m) => ({ default: m.Analog })));
const Digital = lazy(() => import("./tabs/digital/Digital").then((m) => ({ default: m.Digital })));
const SignalPath = lazy(() => import("./tabs/signal/SignalPath").then((m) => ({ default: m.SignalPath })));
const System = lazy(() => import("./tabs/system/System").then((m) => ({ default: m.System })));
const Scripts = lazy(() => import("./tabs/scripts/Scripts").then((m) => ({ default: m.Scripts })));
const Voltages = lazy(() => import("./tabs/voltages/Voltages").then((m) => ({ default: m.Voltages })));
const DAQ = lazy(() => import("./tabs/daq/DAQ").then((m) => ({ default: m.DAQ }))); // WEB-1

type TabId =
  | "overview"
  | "scope"
  | "analog"
  | "digital"
  | "signal"
  | "voltages"
  | "daq"
  | "system"
  | "scripts";

const TABS: { id: TabId; label: string }[] = [
  { id: "overview", label: "Overview" },
  { id: "scope", label: "Scope" },
  { id: "analog", label: "Analog" },
  { id: "digital", label: "Digital" },
  { id: "signal", label: "Signal Path" },
  { id: "voltages", label: "Voltages" },
  { id: "daq", label: "DAQ" }, // WEB-1
  { id: "system", label: "System" },
  { id: "scripts", label: "Scripts" },
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
  // On consecutive failures, back off so a disconnected device doesn't burn
  // 1200 doomed requests per 10-min idle window. The cadence resets to the
  // hot 500 ms loop on the next successful response.
  // WEB-6: Also back off to 30s when page is hidden.
  useEffect(() => {
    let alive = true;
    let consecutiveFailures = 0;
    const HOT_MS = 500;
    const HIDDEN_MS = 30000;  // WEB-6: slow poll when hidden
    const BACKOFF_STAGES = [1000, 2500, 5000, 10000, 30000] as const;
    const nextDelay = () => {
      // WEB-6: Always use slow poll when page is hidden
      if (typeof document !== "undefined" && document.visibilityState === "hidden") {
        return HIDDEN_MS;
      }
      if (consecutiveFailures === 0) return HOT_MS;
      const idx = Math.min(consecutiveFailures - 1, BACKOFF_STAGES.length - 1);
      return BACKOFF_STAGES[idx]!;
    };
    const tick = async () => {
      if (!alive) return;
      try {
        const s = await api.status();
        deviceStatus.value = s;
        consecutiveFailures = 0;
        deviceFailureStreak.value = 0;
      } catch (e) {
        consecutiveFailures += 1;
        // Publish the streak so secondary pollers can back off too, instead of
        // hammering a device this loop already knows is unreachable (S2-9).
        deviceFailureStreak.value = consecutiveFailures;
        // PairingRequiredError fires its own modal; don't spam the console.
        // Other errors warn at most once per backoff stage transition.
        const stage = Math.min(consecutiveFailures, BACKOFF_STAGES.length);
        if (
          (!(e instanceof PairingRequiredError)) &&
          (consecutiveFailures === 1 || consecutiveFailures === stage)
        ) {
          console.warn(
            `status poll failed (#${consecutiveFailures}, next retry in ${nextDelay()} ms)`,
            e,
          );
        }
      }
      if (alive) setTimeout(tick, nextDelay());
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
        {activeTab.value === "overview" && (
          <ErrorBoundary key="overview">
            <Overview />
          </ErrorBoundary>
        )}
        <Suspense fallback={<div class="text-dim" style="padding:1rem">Loading…</div>}>
          {activeTab.value === "scope" && (
            <ErrorBoundary key="scope">
              <ScopePanel />
            </ErrorBoundary>
          )}
          {activeTab.value === "analog" && (
            <ErrorBoundary key="analog">
              <Analog />
            </ErrorBoundary>
          )}
          {activeTab.value === "digital" && (
            <ErrorBoundary key="digital">
              <Digital />
            </ErrorBoundary>
          )}
          {activeTab.value === "signal" && (
            <ErrorBoundary key="signal">
              <SignalPath />
            </ErrorBoundary>
          )}
          {activeTab.value === "voltages" && (
            <ErrorBoundary key="voltages">
              <Voltages />
            </ErrorBoundary>
          )}
          {activeTab.value === "daq" && (
            <ErrorBoundary key="daq">
              <DAQ />
            </ErrorBoundary>
          )}
          {activeTab.value === "system" && (
            <ErrorBoundary key="system">
              <System />
            </ErrorBoundary>
          )}
          {activeTab.value === "scripts" && (
            <ErrorBoundary key="scripts">
              <Scripts />
            </ErrorBoundary>
          )}
        </Suspense>
      </main>

      {pairingRequired.value && <PairingModal />}
    </div>
  );
}
