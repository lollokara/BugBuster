// =============================================================================
// System tab — board profile, HAT, USB-PD, UART, WiFi, faults, IOExp, debug.
// =============================================================================

import { useEffect } from "preact/hooks";
import { SystemHero } from "./SystemHero";
import { SystemSection } from "./SystemSection";
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

  return (
    <div class="tab-stack">
      <SystemHero />

      <SystemSection title="Device">
        <BoardCard />
        <OtaCard />
      </SystemSection>

      <SystemSection title="Connectivity">
        <WifiCard />
        <UartCard />
      </SystemSection>

      <SystemSection title="Power & Protection">
        <UsbPdCard />
        <IoExpControlCard />
      </SystemSection>

      <SystemSection title="HAT Expansion">
        <HatCard />
      </SystemSection>

      <SystemSection title="Diagnostics & Service">
        <FaultsCard />
        <SelftestServiceCard />
        <IoOwnershipCard />
        <DebugCard />
        <DesktopOnlyCard />
      </SystemSection>
    </div>
  );
}
