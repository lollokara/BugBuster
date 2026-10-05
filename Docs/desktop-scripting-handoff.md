# Desktop Scripting Integration Handoff

Date: 2026-10-05. Branch: `feat/desktop-scripting`, worktree `.worktrees/desktop-scripting`.
This is an implementation checkpoint, not a completed acceptance report. No device was flashed,
no active user script was replaced, and no live USB/Wi-Fi or actual Tauri WebView test was run by Agent 2.

## Commits and Ownership

- Backend/firmware transport and work inhibitor: `01e95a21`; Agent 1 imported it as `4b597e82`.
- The subsequent frontend/editor/harness/WebSocket checkpoint contains the remaining implementation.
  Its commit hash is recorded in the canonical root `scratch/low-power-agent-coordination.md`.
- Agent 1 owns final integration after this handoff. Preserve both branches' shell/registry/connection changes.
- Standalone standby module supplied by Agent 1: `538547ba`. It is NOT imported into this scripting branch yet.
- No push, release tag, release-version bump, or firmware-image selection is authorized by this checkpoint.

## Implemented

- USB scripting through existing SCRIPT_AUTORUN CALL/FETCH suboperations, selected-transport-only HTTP,
  stored-file readback and chunked save, compile-only lint, rich status, non-draining cursor logs,
  run/background/replace/stop, persistent eval/reset and autorun status/settings.
- Backend serialization of staged scripting requests and connection-epoch checks on each transfer frame.
- CodeMirror 6, offline esbuild bundle and the canonical generated firmware catalogue shared by
  autocomplete and docs. Catalogue/import/class/instance/keyword/builtin completion, placeholders,
  signatures, undo/redo, indentation, search/replace, docs and example insertion.
- Leptos file browser/editor/docs inspector/console/REPL, app-wide runtime store/status chip,
  dirty buffers and save snapshots, stale reply/lint guards, device-isolated workspaces,
  cursor/UTF-8/gap/reboot logs, local clear/filter/follow/export, autorun confirmations.
- Selected-connection authenticated Wi-Fi WebSocket REPL with bounded session/output queues,
  auth/banner gating, busy/unauthorized handling, epoch/session guards and explicit reconnect without replay.
- Strict stateful IPC mock, editor tests, interaction suite, and `Docs/desktop-scripting-parity.md`.

## Remaining Implementation

1. Import/register Agent 1's `standby_commands.rs`; add bridge DTOs/functions and source-derived BBP constant.
2. Wire random nonzero connection-epoch presence: open/refresh every 10 s, close on logical disconnect,
   45 s expiry, selected USB or HTTP only. Do not register presence for offline editor/docs.
3. Surface standby readiness/waking and inhibit device actions until ready. Preserve buffers;
   never automatically replay a run/eval/reset after waking or an ambiguous timeout.
4. Integrate direct P4 USB logical presence/ack handling from Agent 1's published 0x94/record-0x09 ABI
   in the desktop DAQ transport/decoder with reconnect epochs; USB mounted is not client presence.
5. Integrate the minimal existing System sleep-timeout control requested by Agent 1.
6. Verify Agent 1's combined S3 branch exposes the reset/autorun-run HTTP routes through existing owners.
   Wi-Fi autorun-run currently returns unsupported in `scripts.rs`; no run-now UI exists because iOS has none.
7. Extend `mock_ipc.js` for the new `script_repl_request` command/events and WebSocket error scenarios.
   Exact shapes/scenarios are in root `scratch/desktop-scripting/repl-report.md`. Add REPL copy verification.
8. Refresh parity documentation after the WebSocket integration. The previous mock suite tested REST eval,
   not the new socket session. No arbitrary local-name completion is implemented (also absent on iOS).

## Acceptance Still Required

- Rerun the COMPLETE committed interaction suite after WebSocket/presence integration, not just repaired groups.
- Run the actual Tauri/WebView via CDP, test editor popups/focus, clipboard and export, collect browser errors,
  capture/open screenshots in both themes at representative widths. Do not use browser mocks as WebView evidence.
- Serialize full S3/P4/C6 builds through Agent 1's granted PlatformIO window. Build the combined source;
  agree source inputs and binary SHA-256 before controlled flashing. No isolated image may overwrite standby.
- Use MCP device_status first; inspect running script and autorun before any live test. Coordinate the sole CDC0
  handle between MCP and Tauri. Never stop/replace the user's active script or change calibration.
- Real USB-only and Wi-Fi-only save/readback/lint/run/status/log/stop/REPL workflows using bounded harmless
  temporary scripts; test compile-only lint with a side-effect sentinel. Clean only test-created files.
- Live boot-autorun/reboot testing still needs specific safe-script/action consent; do not enable it implicitly.
- Update owning `.mex` manifests/router/context/patterns and root TODO/CHANGELOG/README, protocol docs;
  run protocol/doc-count/catalogue/desktop-version and applicable Python lint/type gates. `.mex` is separate git.

## Evidence at the Checkpoint

- Main rerun: 31 focused Rust scripting tests, 14 compiled firmware-host tests, 17 local-server WebSocket tests
  passed; frontend WASM check and editor bundle/catalogue freshness check passed.
- Delegate reports: 188 native backend tests and 42 frontend model tests passed after WebSocket implementation;
  56 editor Node tests and 21 standalone editor browser interactions passed (browser suite predates cosmetic fix).
- Latest scoped repaired mock run: 284 passed, 0 failed for identity/editing/runs/autorun/layout matrix.
  This is NOT a final full-suite result after WebSocket changes. Original full run had failures that were repaired.
- Screenshot inspection was performed by delegates, not by the integrating agent. Root evidence paths:
  `scratch/desktop-scripting/harness-shots/`, `shots/`, `editor-*.png`; see ui-repair-report/editor-report.
- No actual Tauri, live USB, live Wi-Fi, or full firmware build evidence from Agent 2.
- Dev server left running at `http://127.0.0.1:1462` (Trunk). It serves the browser frontend, not native Tauri IPC.
  Keep probes/screenshots in root scratch to avoid hot-reload disconnects.

## Commands

From `DesktopApp/BugBuster`: `cargo check --target wasm32-unknown-unknown`,
`cargo test --bin bugbuster-ui`, and `cargo test --manifest-path src-tauri/Cargo.toml --lib`.
From `editor`: `npm ci`, `npm run check`, `npm test`, `npm run test:e2e`.
From `tests/ui-harness`: `node scripting_mock_selftest.mjs`, then
`node scripting.mjs --url http://127.0.0.1:1462 --out <ROOT>/scratch/desktop-scripting`.
Playwright can be resolved via `BB_PLAYWRIGHT_DIR`; see the committed harness README.