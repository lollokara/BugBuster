# ESP32 Web UI Refactor Plan

Date: 2026-05-29
Scope: `Firmware/ESP32/web/` on-device ESP32 UI, with visual and workflow parity toward `DesktopApp/BugBuster/`.
Mode: Orchestrated planning pass with three parallel subagents:

1. ESP32 web UI structure inventory
2. Desktop visual/style parity audit
3. System tab and HTTP API feature-gap audit

## Executive Summary

The ESP32 web UI already has the right foundation: Vite + Preact + Signals, lazy-loaded tabs, glass styling tokens, and several reusable instrument-style components. The refactor should not replace the app shell. The main work is to make the UI feel as clean and navigable as the desktop app by:

- Splitting the overloaded `System.tsx` file and card into domain-focused modules.
- Re-grouping System features into clear operator mental models: Device, Connectivity, Power & Protection, HAT, Diagnostics & Service, Advanced.
- Extracting reusable polling/form/status primitives to remove repeated ad hoc state logic.
- Modularizing the monolithic API client without breaking existing imports.
- Moving repeated camel/snake normalization into typed model helpers.
- Preserving ESP32 firmware constraints: small asset names, lazy chunks, SPIFFS/LittleFS footprint, safe admin-token handling, and explicit warnings on power/service actions.

## Current Hotspots

### 1. `tabs/system/System.tsx`

Approximate size: 1,895 LOC.

Current responsibilities in one file:

- Board profile selection
- HAT detection/status/caps
- HAT rails, LEDs, IO bank, level shifter, LA route, SWD setup, LA logs, calibration
- USB-PD status and PDO voltage selection
- UART bridge config
- WiFi STA scan/connect
- IO expander power/protection/fault config
- Fault masks and clear actions
- Selftest/service actions
- OTA card integration
- IO ownership table
- Raw debug JSON
- Desktop-only/transport-limited guidance

Problems:

- Too many unrelated hardware domains share one render tree.
- `HatCard` is a card-within-a-tab bottleneck and mixes at least five independent tools.
- Polling loops and error handling are local, inconsistent, and often silent.
- Edit-state protection is repeated and implicit.
- Domain helpers such as `parseMask`, `maskToHex`, `UART_IO_MAP`, and `ioLabelForGpio` live beside rendering.
- Heavy `any` usage makes API drift hard to catch.

### 2. `api/client.ts`

Approximate size: 1,085 LOC.

Current role:

- Fetch wrapper and auth cache
- Errors
- Endpoint methods for almost every API domain
- Type definitions for many unrelated payloads

Problems:

- The file is no longer navigable.
- Endpoint ownership is unclear.
- Type definitions are mixed with transport concerns.
- Incremental changes risk unrelated regressions.

### 3. `styles/components.css`

Approximate size: 1,431 LOC.

Current role:

- Shell, tabs, primitives, scope styles, analog/digital/system/script styles.

Problems:

- Style ownership is not modular.
- System-specific layout styles are mixed with base primitives.
- Future visual parity work will make this file harder to maintain.

## Target UX Structure

Keep the current seven top-level web tabs. Do not expand the constrained on-device UI to match all desktop tabs one-for-one.

| Web tab | Desktop app surfaces to emulate |
|---|---|
| Overview | Overview, Board, Voltages |
| Scope | Scope, WaveGen, Logic Analyzer guidance |
| Analog | ADC, VDAC, IDAC, Current Input, HV IO, Diagnostics |
| Digital | GPIO, DIN, DOUT |
| Signal Path | Signal Path, IO Expander route context, Voltages |
| System | Board, UART, USB PD, IO Expander, HAT, Faults, Diagnostics, Calibration, OTA |
| Scripts | ESP32-specific developer surface |

## New System Information Architecture

Refactor the System tab into a layout shell plus grouped sections.

```tsx
export function System() {
  return (
    <div class="tab-stack system-tab">
      <SystemHero />

      <SystemSection title="Device">
        <DeviceIdentityCard />
        <BoardCard />
        <OtaCard />
      </SystemSection>

      <SystemSection title="Connectivity">
        <WifiCard />
        <UartCard />
      </SystemSection>

      <SystemSection title="Power & Protection">
        <UsbPdCard />
        <MainboardPowerCard />
        <SupplyDiagnosticsCard />
      </SystemSection>

      <SystemSection title="HAT Expansion">
        <HatSummaryCard />
        <HatRailsCard />
        <HatIoBankCard />
        <HatLogicAnalyzerCard />
        <HatCalibrationCard />
      </SystemSection>

      <SystemSection title="Diagnostics & Service">
        <FaultsCard />
        <SelftestCard />
        <ServiceActionsCard />
        <IoOwnershipCard />
        <RawDebugCard />
      </SystemSection>

      <AdvancedLimitationsCard />
    </div>
  );
}
```

### Group definitions

#### Device

Purpose: identity, firmware, pairing, board profile, update lifecycle.

Cards:

- `DeviceIdentityCard`
  - Use `/api/device/info`, `/api/device/version`, `/api/pairing/info`.
  - Show MAC, token fingerprint, silicon rev/ID, SPI OK, firmware version, transport.
- `BoardCard`
  - Existing board profile selection, cleaned up.
- `OtaCard`
  - Keep existing `OtaCard.tsx`, adjust placement and danger styling.

#### Connectivity

Purpose: communication into or out of the device.

Cards:

- `WifiCard`
  - Existing status/scan/connect.
  - Add hostname and AP password controls if firmware endpoints are stable.
  - Clearly warn that AP password changes disconnect clients.
- `UartCard`
  - Existing bridge config, but with compact desktop-style form rows.

#### Power & Protection

Purpose: rails, power source, e-fuses, level-shifter and supply diagnostics.

Cards:

- `UsbPdCard`
  - Add PDO/current budget context and warning copy.
- `MainboardPowerCard`
  - Rename current “IOExp Power & EFuse” surface.
  - Split power enables, e-fuse latches, and level-shifter OE visually.
- `SupplyDiagnosticsCard`
  - Use `/api/selftest/supplies`, cached supplies, and per-rail probe endpoints.

#### HAT Expansion

Purpose: HAT status and HAT-only capabilities.

Cards:

- `HatSummaryCard`: presence, fw, caps, target voltage, high-level health.
- `HatRailsCard`: 3V3_ADJ, VADJ3, VADJ4 enable/voltage/target/current.
- `HatIoBankCard`: shifted IO masks, pull-ups, pull-downs, level-shift direction/OE, HAT LEDs.
- `HatLogicAnalyzerCard`: LA route/status/log enable/log view. Mark USB-only LA streaming clearly.
- `HatCalibrationCard`: calibration rail select, progress, last measured voltage, stage/point/code/persist status.

#### Diagnostics & Service

Purpose: status first, actions second.

Cards:

- `FaultsCard`: faults, masks, clear actions.
- `SelftestCard`: boot selftest, worker, supply monitor, calibration status.
- `ServiceActionsCard`: device reset and other mutating service actions with danger styling.
- `IoOwnershipCard`: owner table, force release.
- `RawDebugCard`: `/api/debug` JSON, copy button, endpoint label.

#### Advanced / Developer

Purpose: capabilities present in firmware but not safe as primary workflows.

Potential future cards:

- External bus status and guarded bus tools: `/api/bus/status`, `/api/bus/*`.
- ADGS route inspector: `/api/adgs/routes`, likely better placed in Signal Path with a System link.
- Desktop-only and transport-limited guidance card.

## Proposed File Structure

```text
Firmware/ESP32/web/src/
  api/
    core.ts                 # request(), errors, auth-token cache
    client.ts               # compatibility facade re-exporting api object
    board.ts
    device.ts
    hat.ts
    power.ts
    connectivity.ts
    diagnostics.ts
    scripts.ts
    ota.ts
    io_lease.ts             # existing boundary, keep
  components/
    AsyncActionButton.tsx
    Field.tsx               # SelectField, NumberField, ToggleRow if kept together
    KeyValue.tsx            # KeyValueRow, KeyValueTable
    ResultBanner.tsx
    SectionHeader.tsx
    SystemSection.tsx       # or tabs/system/SystemSection.tsx
  hooks/
    useAsyncAction.ts
    usePoll.ts
    useEditableResource.ts  # optional, for polling + local editing protection
  models/
    status.ts
    hat.ts
    system.ts
    normalize.ts
  styles/
    tokens.css
    components.css          # primitives only
    layout.css
    forms.css
    system.css
    scope.css
    analog.css
    digital.css
    scripts.css
  tabs/system/
    System.tsx              # layout only
    DeviceIdentityCard.tsx
    BoardCard.tsx
    WifiCard.tsx
    UartCard.tsx
    UsbPdCard.tsx
    MainboardPowerCard.tsx
    SupplyDiagnosticsCard.tsx
    FaultsCard.tsx
    SelftestCard.tsx
    ServiceActionsCard.tsx
    IoOwnershipCard.tsx
    RawDebugCard.tsx
    AdvancedLimitationsCard.tsx
    OtaCard.tsx             # existing
    hat/
      HatSummaryCard.tsx
      HatRailsCard.tsx
      HatIoBankCard.tsx
      HatLogicAnalyzerCard.tsx
      HatCalibrationCard.tsx
      hatUtils.ts
```

## Implementation Plan

### Phase 0: Baseline and guardrails

Goal: make refactor measurable and reversible.

Tasks:

1. Capture current web build output size and chunk list.
2. Run baseline validation:
   - `cd Firmware/ESP32/web && pnpm build`
   - `PYTHONPATH=python python -m pytest tests/unit/test_http_transport.py -q`
3. Add or update a short web refactor checklist under `Firmware/ESP32/web/docs/` if needed.
4. Ensure no firmware HTTP endpoint changes are needed for the first structural pass.

Acceptance:

- Baseline build and HTTP unit tests are known before edits.
- New UI chunks do not violate SPIFFS short-path assumptions.

### Phase 1: Extract primitives and hooks

Goal: reduce repeated code without changing behavior.

Tasks:

1. Add `hooks/usePoll.ts` with:
   - interval polling
   - alive guard
   - optional immediate tick
   - optional error callback
   - no console spam by default
2. Add `hooks/useAsyncAction.ts` for busy/error/result state around mutating actions.
3. Add `components/KeyValue.tsx`.
4. Add `components/ResultBanner.tsx`.
5. Add `components/AsyncActionButton.tsx` if it improves repeated button states.
6. Move `parseMask`, `maskToHex`, `UART_IO_MAP`, and `ioLabelForGpio` into local utility files.

Acceptance:

- `System.tsx` behavior unchanged.
- Existing cards can still import from the single file while helpers are extracted.
- `pnpm build` passes.

### Phase 2: Split System tab cards

Goal: break the 1.9k LOC hotspot into domain files with no feature loss.

Recommended subagent lanes:

- Agent A: Device + OTA placement
- Agent B: Connectivity cards
- Agent C: Power & Protection cards
- Agent D: HAT card split
- Agent E: Diagnostics & Service cards

Tasks:

1. Create `tabs/system/SystemSection.tsx` and update `System.tsx` to be layout only.
2. Extract `BoardCard` to `BoardCard.tsx` with no logic changes.
3. Extract `WifiCard` and `UartCard`.
4. Extract `UsbPdCard`, `MainboardPowerCard`, and `SupplyDiagnosticsCard`.
5. Split `HatCard` into summary, rails, IO bank, LA, and calibration cards.
6. Extract `FaultsCard`, `SelftestCard`, `ServiceActionsCard`, `IoOwnershipCard`, and `RawDebugCard`.
7. Keep `OtaCard.tsx` as existing module.

Acceptance:

- System tab renders all existing capabilities.
- No card loses a current action or status field.
- Mutating actions retain admin-token behavior and pairing errors.
- `pnpm build` passes after each lane.

### Phase 3: UX cleanup and desktop parity

Goal: make the UI cleaner, more like the desktop app, and less crowded.

Tasks:

1. Add a `SystemHero` status strip with:
   - MAC/fingerprint
   - firmware version
   - SPI OK
   - HAT detected
   - fault count
   - active board profile
2. Apply desktop card hierarchy:
   - section header
   - compact status pills
   - glass cards
   - monospace telemetry
   - clear warning/danger styling
3. Convert HAT advanced areas to collapsible subcards or compact grouped grids.
4. Rename confusing labels:
   - “IOExp Power & EFuse” -> “Mainboard Power & Protection”
   - “Debug” -> “Raw Diagnostics JSON”
   - “Selftest / Service” -> separate “Diagnostics” and “Service Actions”
5. Add missing endpoint-backed controls where low-risk:
   - `/api/device/version` in Device Identity
   - `/api/wifi/hostname` in WiFi card
   - `/api/wifi/ap_password` with confirmation warning
   - `/api/adgs/routes` as read-only route inspector or Signal Path link
   - `/api/bus/status` as read-only External Bus status

Acceptance:

- System tab can be understood without scrolling through unrelated controls.
- Dangerous actions are visually separated and require deliberate clicks.
- The first viewport shows identity, health, and key actions.

### Phase 4: Modularize API and models

Goal: make future feature work safer.

Tasks:

1. Move `request()`, auth cache, and error classes to `api/core.ts`.
2. Move endpoint groups into domain files.
3. Keep `api/client.ts` as a compatibility facade:

   ```ts
   export * from "./core";
   export const api = {
     ...deviceApi,
     ...boardApi,
     ...hatApi,
     ...powerApi,
     ...connectivityApi,
     ...diagnosticsApi,
     ...scriptsApi,
     ...otaApi,
   };
   ```

4. Move domain types to `models/*` or colocated `api/*` files.
5. Add normalization helpers for camelCase/snake_case payload drift.

Acceptance:

- Existing imports from `../../api/client` keep working.
- TypeScript catches more System/HAT fields than before.
- Build output remains within firmware asset constraints.

### Phase 5: CSS split

Goal: keep styling maintainable while preserving chunk behavior.

Tasks:

1. Keep `tokens.css` as source of truth.
2. Keep `components.css` for app shell and reusable primitives only.
3. Add `layout.css`, `forms.css`, `system.css`, and domain CSS files only where needed.
4. Prefer static imports from `main.tsx` unless CSS code-splitting is verified against firmware packaging.

Acceptance:

- Visual output remains consistent.
- CSS is easier to navigate.
- Vite output still produces firmware-safe asset names.

### Phase 6: Validation and smoke testing

Build validation:

```bash
cd Firmware/ESP32/web && pnpm build
PYTHONPATH=python python -m pytest tests/unit/test_http_transport.py -q
```

Optional firmware build if static assets are regenerated:

```bash
cd Firmware/ESP32 && pio run -e esp32s3
```

Manual or scripted browser smoke:

- Load Overview and all lazy tabs.
- Open System and verify all sections render.
- Pairing modal still opens on 401.
- Mutating actions still include admin token.
- System cards do not overwrite in-progress form edits while polling.
- HAT absent state is clean.
- HAT present state shows caps/rails/LA/calibration.
- WiFi scan/connect error handling is visible.
- OTA card still accepts firmware and SPIFFS image flows.
- Dangerous service actions are clearly styled.

Hardware smoke checklist:

- Use `Firmware/ESP32/tools/web_parity_smoke.sh` read-only mode first.
- Run mutating checks only with `RUN_MUTATING=1` on a safe bench setup.

## Suggested Agent Assignment for Coding

Use five coding subagents working from this plan, one lane at a time, with the orchestrator integrating and running builds between lanes.

### Lane A: Foundations

Files:

- `src/hooks/usePoll.ts`
- `src/hooks/useAsyncAction.ts`
- `src/components/KeyValue.tsx`
- `src/components/ResultBanner.tsx`
- `src/tabs/system/systemUtils.ts`

Deliverable:

- Shared primitives and utilities, no visible behavior change.

### Lane B: System tab extraction

Files:

- `src/tabs/system/System.tsx`
- `src/tabs/system/BoardCard.tsx`
- `src/tabs/system/WifiCard.tsx`
- `src/tabs/system/UartCard.tsx`
- `src/tabs/system/UsbPdCard.tsx`
- `src/tabs/system/FaultsCard.tsx`
- `src/tabs/system/SelftestCard.tsx`
- `src/tabs/system/IoOwnershipCard.tsx`
- `src/tabs/system/RawDebugCard.tsx`

Deliverable:

- Same UI features, split files, cleaner layout shell.

### Lane C: HAT split

Files:

- `src/tabs/system/hat/*`

Deliverable:

- HAT functionality split into five domain cards without losing controls.

### Lane D: Visual polish and missing low-risk endpoints

Files:

- `src/tabs/system/*`
- `src/styles/system.css`
- `src/api/client.ts` or new domain API files if Phase 4 has started

Deliverable:

- Desktop-like visual grouping, Device Identity card, hostname/AP password if endpoint wrappers are verified.

### Lane E: API/CSS modularization

Files:

- `src/api/*`
- `src/models/*`
- `src/styles/*`

Deliverable:

- Smaller API and style modules with `api/client.ts` compatibility preserved.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Lazy chunks/assets exceed firmware path or size assumptions | Compare `dist/assets` before/after every phase; preserve Vite short chunk config |
| Refactor drops a System action | Use current card inventory as checklist; verify every current API method remains reachable |
| Polling overwrites forms while user edits | Add explicit editable resource pattern and pause/merge poll updates during dirty state |
| Dangerous controls become easier to click | Separate Service Actions, use danger styling, preserve confirmations |
| HAT absent state breaks cards | HAT cards must accept `null`/absent state and render disabled guidance |
| API modularization breaks imports | Keep `api/client.ts` compatibility facade until all tabs are migrated |
| CSS split changes visuals unexpectedly | Split after structural work and keep token names/classes stable |

## Completion Criteria

The refactor is complete when:

- `System.tsx` is a layout shell, not a feature implementation file.
- HAT functionality is split into focused cards.
- The System tab is grouped by operator intent, not implementation module.
- At least `/api/device/version`, WiFi hostname/AP password, bus status, and ADGS route status have deliberate UI decisions: surfaced, linked, or documented as deferred.
- `api/client.ts` is either modularized or reduced to a compatibility facade.
- CSS has a clear primitive-vs-domain boundary.
- `pnpm build` passes.
- HTTP transport unit tests pass.
- Existing System feature checklist passes with no regressions.
