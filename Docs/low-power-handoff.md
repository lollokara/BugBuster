# System Standby Checkpoint

Date: 2026-10-05. Status: implementation checkpoint, NOT release-ready or live-verified.
No firmware was flashed, no calibration was changed, and no commits were pushed.

## Integration

- Worktree: `.worktrees/system-standby`, branch `feat/system-standby`.
- Standalone desktop standby backend: `538547ba`.
- Agent 2 scripting backend `01e95a21` is already imported as `4b597e82`.
- Agent 2 frontend/editor checkpoint: `8e734805`; merge this committed checkpoint,
  not Agent 2's dirty working files. Shared desktop ownership has been handed over.
- Root `scratch/low-power-agent-coordination.md` is the canonical lease/handoff file.
- Root main still contains user changes. Do not overwrite or revert them during integration.
- `.mex` is a separate repository. Update and commit its knowledge independently.

## Implemented

- S3 authoritative ACTIVE / PREPARING / ASLEEP / WAKING / FAULT_SAFE coordinator,
  monotonic timing, generation-tagged acknowledgements, bounded barriers and recovery.
- Persisted timeout choices 0 / 60 / 300 / 900 seconds, default 300. Persistence
  errors are reported, not claimed successful. Connected clients inhibit manual sleep too.
- Explicit logical presence leases and owner-derived script, stream, trigger, bus,
  UART, waveform, calibration, self-test, OTA, battery-simulation and HAT inhibitors.
- S3 MUX disconnection/readback before e-fuses, VADJ1/VADJ2 and VANALOG shutdown;
  worker/SPI availability gates and unavailable analog values; genuine faults retained.
- P4 MUX disconnection before switched analog 3V3, +/-26 V and +/-24 V shutdown.
  Retained `3V3_ESP`, controllers, communications and buttons remain powered.
- P4 wake resets and identifies the ADAQs and restores saved configuration and
  register calibration values without NVS/calibration changes. Outputs and routes stay off.
- LA HAT guarded rail/OE/pin shutdown, logical DAP/LA inhibitors and safe wake.
- C6 logo/progress UI from real milestones, timeout menu, panel/backlight hard-off,
  transient LED blanking and complete wake-gesture consumption.
- Python/MCP, web and iOS presence/settings slices; standalone desktop backend.

Code families: `Firmware/ESP32/src/power/standby_*`, `net/api_standby.*`,
`bbp/cmds/cmd_standby.cpp`, DAQ `standby/`, C6 `standby_c6*` / `splash*`,
RP2040 `bb_standby*`, Python `standby.py`, and the focused standby tests.
Use `git show --stat` on the checkpoint commit for the complete changed-file list.

## Wire Contract

- BBP `STANDBY` is additive command `0x78`, with status/presence/policy/wake/sleep
  suboperations and a 28-byte schema-1 status. Definitions: `standby_api.h`.
- HTTP/BLE JSON routes: `/api/standby/{status,presence,policy,wake,sleep}`.
- HAT command/reply `0x7C` / `0x9C`, DDP `0x1F`, policy mailbox `0x0A`.
  Canonical packed 16-byte envelope: `Firmware/DAQ_HAT/common/standby_wire.h`.
- P4 direct USB lease command `0x94`, 12-byte `<BBHII>` request; ACK record `0x09`,
  16-byte schema-1 reply. Full capacity refuses rather than evicting clients.
- Existing protocol and release versions are unchanged; lockstep gate passed.

## Evidence

- Focused checkpoint run: 108 passed, 3 failed in the route-contract scanner.
  The scanner was then updated to follow the real standby parser; rerun evidence
  is root `scratch/standby-contract-check.txt`.
- Firmware-host tests execute the real C policy/participants with fake hardware,
  including repeated cycles, ordering, cancellation, failed rails, stale replies,
  and wake gesture consumption. These are NOT physical tests.
- Initial P4/C6 builds succeeded before subsequent safety-review changes;
  the latest reviewed sources need serial rebuilds. S3/RP2040 final builds are pending.
- Python/web/Rust slice tests were run by delegates; reports are in root
  `scratch/standby-*-report.md`. iOS was source-reviewed only, not built.
- No live MCU, physical display/backlight/LED, power-savings or wake-latency evidence.
- Bench was confirmed DUT/IO-disconnected. Only a DAQ HAT was available; LA live
  testing, an external system input-power meter and physical visual evidence are unavailable.

## Remaining Gates

1. Complete desktop registration/bridge, connection-epoch presence/close/readiness,
   direct P4 USB leases/ACKs and minimal existing System timeout control.
2. Complete P4 ACK decoding in Python and every owning host decoder; integrate
   standby simulator handlers and remaining route/protocol fixture expectations.
3. Wire Agent 2 reset/autorun-run owner helpers into S3 HTTP/api_core dispatch.
4. Run full host/simulator tests, ruff, mypy, doc-count, protocol and sdkconfig gates.
   No failing gate may be described as an expected pass.
5. Build final S3, P4, C6 and RP2040 images; check PIO processes and exclusive lease
   before each serialized build. Confirm RP2040 DAP linker wrappers actually link.
6. Update all owning manifests, cross-surface links, CI common-header path filters,
   documentation counts and `.mex` state. Preserve the scripting handoff entries.
7. Agree combined source/image SHA-256 and device/CDC lease with Agent 2 before
   any supported-format OTA. Preserve NVS, settings and calibration fingerprints.
8. Perform bounded repeated DAQ live transitions and race/failure tests, held-button
   wake, real USB/Wi-Fi requests, startup without Wi-Fi and physical visual checks.
9. Obtain LA HAT coverage and measure comparable system input power and wake latency;
   otherwise retain those explicit acceptance gaps.

Known hardware issue: `C6_RST_PIN` aliases GPIO54 in existing P4 source, while the
schematic maps GPIO54 to `26V_EN`. Standby uses the verified analog-enable net;
existing flasher/CLI reset aliases were not repaired in this task. Investigate before
relying on those reset paths or claiming physical isolation/backfeed verification.