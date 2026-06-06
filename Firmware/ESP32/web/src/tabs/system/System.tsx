// =============================================================================
// System tab — board profile, HAT, USB-PD, UART, WiFi, faults, IOExp, debug.
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { SystemHero } from "./SystemHero";
import { BoardCard } from "./BoardCard";
import { OtaCard } from "./OtaCard";
import { HatCard } from "./HatCard";
import { UsbPdCard } from "./UsbPdCard";
import { UartCard } from "./UartCard";
import { IoExpControlCard } from "./IoExpControlCard";
import { WifiCard } from "./WifiCard";
import { FaultsCard } from "./FaultsCard";
import { SelftestServiceCard } from "./SelftestServiceCard";
import { IoOwnershipCard } from "./IoOwnershipCard";
import { DebugCard } from "./DebugCard";
import { DesktopOnlyCard } from "./DesktopOnlyCard";
import { startSelftestStatusPolling } from "../../state/signals";

export function System() {
  useEffect(() => startSelftestStatusPolling(), []);
  const [activeSection, setActiveSection] = useState<string>("device");

  const sections = [
    { id: "device", label: "Device" },
    { id: "conn", label: "Connectivity" },
    { id: "power", label: "Power" },
    { id: "hat", label: "HAT Expansion" },
    { id: "diag", label: "Diagnostics" },
  ];

  return (
    <div class="tab-stack">
      <SystemHero />

      {/* Sub-navigation Pills */}
      <div style={{ display: "flex", gap: "6px", flexWrap: "wrap", marginBottom: "8px", borderBottom: "1px solid var(--border)", paddingBottom: "12px" }}>
        {sections.map(s => (
          <button
            key={s.id}
            onClick={() => setActiveSection(s.id)}
            class={"pill" + (activeSection === s.id ? " active" : "")}
            style={{
              padding: "6px 12px",
              fontSize: "0.8rem",
              fontWeight: 600,
              textTransform: "uppercase",
              letterSpacing: "0.5px",
            }}
          >
            {s.label}
          </button>
        ))}
      </div>

      <div style={{ display: "flex", flexDirection: "column", gap: "12px" }}>
        {activeSection === "device" && (
          <>
            <BoardCard />
            <OtaCard />
          </>
        )}
        {activeSection === "conn" && (
          <>
            <WifiCard />
            <UartCard />
          </>
        )}
        {activeSection === "power" && (
          <>
            <UsbPdCard />
            <IoExpControlCard />
          </>
        )}
        {activeSection === "hat" && (
          <HatCard />
        )}
        {activeSection === "diag" && (
          <>
            <FaultsCard />
            <SelftestServiceCard />
            <IoOwnershipCard />
            <DebugCard />
            <DesktopOnlyCard />
          </>
        )}
      </div>
    </div>
  );
}
