# Desktop scripting parity matrix (iOS vs desktop, USB and Wi-Fi)

Status date: 2026-10-05. Worktree `.worktrees/desktop-scripting`, nothing committed.
Checkpoint update: backend `01e95a21` is committed. The Wi-Fi WebSocket REPL was implemented
after the repaired mock run; local-server session tests pass, but socket fixtures and the full
post-integration UI suite are still pending. Use `desktop-scripting-handoff.md` for remaining work.
This is a map, not a contract: the owning sources are named in every row. Re-derive counts
and behaviour from the sources and the harness result, never from this page.

## How to read the evidence column

| Tag | Meaning | Where it lives |
|---|---|---|
| `MOCK-UI` | A step of the interaction suite passed against the stateful `script_request` mock. Browser only: no Tauri runtime, no CDP, no device. | `DesktopApp/BugBuster/tests/ui-harness/scripting.mjs`, step names quoted |
| `MOCK-SELF` | The mock itself, driven without a browser. | `scripting_mock_selftest.mjs` |
| `EDITOR` | CodeMirror and catalogue behaviour on a static test page. | `tests/ui-harness/editor_e2e.mjs`, `editor/test/*.test.mjs` |
| `UNIT` | Rust model tests (`script_state::tests`) or backend tests (`scripts_tests`, `scripts::model_tests`). | `src/script_state.rs`, `src-tauri/src/scripts*.rs` |
| `HOST-FW` | Firmware logic compiled on the host. Not an S3 build, not a flash. | `tests/firmware_host/test_script_*.py` |
| `BUILD` | Compiles only. Never counts as behaviour evidence. | `cargo check`, `trunk serve` |
| `LIVE-PENDING` | Needs the real Tauri WebView and/or hardware. Not run. | - |
| `OPEN-DEFECT` | The harness step fails today; the precise selector is in `scratch/desktop-scripting/harness-report.md`. | - |

Nothing in this matrix is `LIVE`. No row claims USB or Wi-Fi hardware behaviour.

## Transport map (current desktop, verified in source)

| Operation (IPC `script_request`) | USB path (BBP `SCRIPT_AUTORUN` 0xFD sub 6 CALL / sub 7 FETCH, tunnelled op) | Wi-Fi path (HTTP, admin token) |
|---|---|---|
| files | op 4 files | `GET /api/scripts/files` |
| storage | op 5 storage | `GET /api/scripts/storage` |
| get | op 6 files/get, paged | `GET /api/scripts/files/get` |
| save | op 8 files/chunk, 1536-byte raw chunks, final flag, temp file then copy | `POST /api/scripts/files?name=` raw body |
| delete | op 7 files/delete | `POST /api/scripts/files/delete` |
| lint (source or name) | op 11, JSON `{src}` or `{name}` | `POST /api/scripts/lint` raw text; for a stored name the file is read first, then its text is linted |
| run / replace / background | op 9 run-file | `POST /api/scripts/run-file` |
| eval (persistent) | op 10 eval | `POST /api/scripts/eval?persist=1` |
| logs (cursor, non draining) | op 2 logs | `GET /api/scripts/logs?since=`; cursor in `X-BugBuster-Log-Next` |
| status / stop / reset | ops 1 / 3 / 16 | `/api/scripts/status`, `/stop`, `/reset` |
| autorun status / enable / disable | ops 12 / 13 / 14 | `/api/scripts/autorun/status`, `/enable`, `/disable` |
| autorun run-now | op 15; backend only, **no UI control** | none: backend answers `unsupported` |

Old firmware: sub 6/7 are refused, the backend answers `kind:"unsupported"` and never falls back to the other transport.
The iOS app uses Wi-Fi HTTP and a BLE tunnel; the desktop has no BLE. USB is desktop only.

## Matrix

Reference symbols are in `iOSApp/Sources` and tests in `iOSApp/Tests` unless noted. iOS test counts are not repeated here:
`Select-String -Path iOSApp\Tests\*Script*.swift -Pattern 'func test'` lists them.

### Connection, identity, offline

| # | Capability | iOS reference | Desktop component | USB | Wi-Fi | Evidence and limit |
|---|---|---|---|---|---|---|
| 1 | Per-device workspace, stable across transports | `ScriptRunManager` `testDeviceChangeResetsLog`, `testAttachFollowsConnectionState` | `script_state.rs` `DeviceIdent::key` (`mac:<mac>`), backend `device{transport,address,mac}` | yes | yes | `MOCK-UI` identity: "Wi-Fi: same MAC keeps files and the dirty buffer"; `UNIT`. Without a MAC the key is `transport:address`, so USB and Wi-Fi then differ (no harness case). |
| 2 | Reply guard on device swap / disconnect | `testRestartPollingWhileWireRequestSuspendedDoesNotDuplicateLogs` | link epoch in `ScriptStore::request`, backend `device_changed` | yes | yes | `MOCK-UI` identity: "device swap: B shows only B... stale A status is ignored" (mock set to deliver the old reply late), "disconnect while saving". |
| 3 | Dirty buffers survive disconnect and return per device | `ScriptEditorModelTests` | `Scripts` model per device | yes | yes | `MOCK-UI` same steps. iOS clears status on disconnect; the desktop keeps it stale ("last seen"), an intentional difference from the brief. |
| 4 | Editor, docs and scratch buffer without a device | `FirmwareAPICatalogue.bundled` | offline shell in `app.rs`, `bbScriptCatalogue` | n/a | n/a | `MOCK-UI` identity: "offline: editor, docs and scratch work with no device and no IPC" (zero `script_request`), docs-failure. |
| 5 | Catalogue failure is explicit and retryable | none | `sc-docs-state` | n/a | n/a | `MOCK-UI` docs-failure. iOS bundles the catalogue, so there is no failure state to compare. |
| 6 | Click a stored script before the link is verified | n/a | `ScriptStore::open_file` | yes | yes | `MOCK-UI` identity step 6 (verified 2026-10-05): a connected but unverified link verifies itself with a status request, then loads the file. If the device still has not identified itself the notice says so instead of "Connect to a device". |

### Files and editor

| # | Capability | iOS reference | Desktop component | USB | Wi-Fi | Evidence and limit |
|---|---|---|---|---|---|---|
| 7 | List, storage usage, refresh | `testLoadFilesSortsCaseInsensitivelyAndReadsStorage` | `sc-list`, `sc-storage`, `sc-refresh` | yes | yes | `MOCK-UI` identity connect step. Case-insensitive ordering is not exercised (mock names are lowercase). |
| 8 | Create: name normalisation, invalid, duplicate | `testCreateNormalizesTheNameAndOpensIt`, `testCreateRejectsInvalidAndDuplicateNames` | `Dialog::New`, `ScriptStore::create_file` | yes | yes | `MOCK-UI` editing: "new script: name validation, duplicates..." (duplicate is refused at confirm time with a notice, no upload). |
| 9 | Save then compile-only lint; lint failure never undoes the save | `testSaveThenLintOK`, `testLintFailureStillSaves` | `save_named` + `lint_stored` | yes | yes | `MOCK-UI` editing + lint steps. |
| 10 | Failed upload keeps the editor dirty | `testSaveFailureKeepsTheEditorDirty` | `save_named` | yes | yes | `MOCK-UI` editing: "failed upload...", "mid-upload failure..." (old file unchanged, progress bar). |
| 11 | Edits made during upload stay dirty | `ScriptEditorModelTests` | revision-tagged snapshot | yes | yes | `MOCK-UI` editing: "typing during upload..." (also: lint result not attributed to the newer text). |
| 12 | 32 KiB UTF-8 byte limit | `testScriptNameMirrorsFirmwareValidation` | `check_source` | yes | yes | `MOCK-UI` editing: "oversize script..." (0 save commands sent). |
| 13 | Delete with confirmation; deleting the open file closes the editor | `testDeletingTheOpenFileClosesTheEditor` | `Dialog::Delete` | yes | yes | `MOCK-UI` editing: "dirty protection..." and "deleting the open file...". |
| 14 | Run saves the dirty open file first; another file saves nothing | `testRunSavesTheDirtyOpenFileFirst`, `testRunOfAnotherFileDoesNotSave` | `run_flow` | yes | yes | `MOCK-UI` runs: "Run saves the dirty open file first...", "edit during the save-before-run upload cancels the run". |
| 15 | Revert to device copy with confirmation | none | `Dialog::Revert` | yes | yes | Desktop addition. `MOCK-UI` editing. |
| 16 | Lossy (non UTF-8) file warning | none | `sc-lossy`, `Dialog::SaveLossy` | yes | yes | Desktop addition. `MOCK-UI` editing: "lossy...". |
| 17 | Smart-punctuation and CRLF sanitising of editor input | `testSanitizerReplacesSmartPunctuation`, `testSanitizerNormalizesCRLFAndLoneCRToLF` | **not applied** (it would corrupt valid UTF-8) | n/a | n/a | Shipped difference. The REPL input is sanitised. |
| 18 | Undo / redo | `ScriptKeyBar` | CodeMirror history, `Ctrl+Z` / `Ctrl+Y` | n/a | n/a | `MOCK-UI` editing: "undo / redo / search & replace"; `EDITOR`. |
| 19 | Indent / outdent / symbol key bar | `ScriptKeyBar`, `testIndent*`, `testOutdent*` | `Tab`, `Shift+Tab`, auto-indent | n/a | n/a | `EDITOR` only (no key bar on a desktop keyboard). |
| 20 | Find and replace | none | CodeMirror search panel | n/a | n/a | Desktop addition. `MOCK-UI` editing. |
| 21 | Long lines scroll inside the editor | n/a | `.cm-scroller` | n/a | n/a | `MOCK-UI` editing: "long source line and long file name stay inside their panes". |

### Completion and docs

| # | Capability | iOS reference | Desktop component | USB | Wi-Fi | Evidence and limit |
|---|---|---|---|---|---|---|
| 22 | Members after `.`, module / class / instance, import and alias, `with ... as`, return-annotation instances, kwargs, builtins, keywords | `ScriptCompletion.swift`, `ScriptCompletionTests` | `editor/src` completion on `bbScriptCatalogue` | n/a | n/a | `EDITOR` ports the iOS cases; `MOCK-UI` completion: alias, from-import + class instance, return-annotation instance, class call, builtins, mouse / keyboard / `Ctrl+Space`. |
| 23 | Accept inserts a call and selects the first placeholder; `Tab` walks placeholders | `testAcceptingACallSelectsTheFirstPlaceholder`, `testTabWalksThePlaceholdersThenLeavesTheCall` | CodeMirror snippet-style placeholders | n/a | n/a | `MOCK-UI` completion ("caret lands on the first placeholder"); `EDITOR`. |
| 24 | Popup placement | `testPopoverClampsRightAndFlipsAbove` | CodeMirror tooltip | n/a | n/a | `MOCK-UI` completion: "popup anchoring..." and every evidence shot (inside the viewport, not over the caret). |
| 25 | Local variable / def names are **not** completed | not shipped | not shipped | n/a | n/a | `MOCK-UI` completion asserts absence. This is a shipped limit, not a gap. |
| 26 | Signature help | none (iOS shows kwargs only) | `.cm-tooltip-signature` | n/a | n/a | Desktop addition. `MOCK-UI` completion. F3 fixed 2026-10-05: tooltips are confined to the editor box (no overlap with the toolbar) and the hint hides while the completion menu is open; checked in one screenshot. |
| 27 | Docs browser: search, kind filter, detail (params, returns, raises, safety), examples | `ScriptDocsBrowser`, `FirmwareDocs.sections` | `DocsPanel` | n/a | n/a | `MOCK-UI` docs steps. F10 fixed 2026-10-05: the detail signature wraps (`sc-code-sig`). |
| 28 | Insert adds a missing import once; an alias does not count; examples go in verbatim only into an empty script | `testInsertAtCaretAddsAMissingImport`, `testInsertAtCaretKeepsAnExistingImport`, `testInsertIntoAnEmptyDocument` | `planInsert`, `insertDoc` | n/a | n/a | `MOCK-UI` docs. |
| 29 | One catalogue for completion and docs | `FirmwareAPICatalogue` | `public/firmware-api.json`, `npm run check` in `editor/` | n/a | n/a | `EDITOR`; `BUILD`. |

### Syntax checking

| # | Capability | iOS reference | Desktop component | USB | Wi-Fi | Evidence and limit |
|---|---|---|---|---|---|---|
| 30 | Device compile-only check on save and on demand | `ScriptsClient.lint` | `lint_stored`, `check_syntax` | yes | yes | `MOCK-UI` lint: "valid text..."; "explicit check sends source... never executes". |
| 31 | Five outcomes stay distinct: syntax, busy, unsupported firmware, transport, unavailable | `testLintBusyWhileAScriptRunsIsUnavailableNotFailed`, `testSaveOverBLEOldFirmwareReportsLintNeedsWiFi` | `LintBanner` | yes | yes | `MOCK-UI` lint (all five wordings compared). |
| 32 | Stale lint reply cannot attach to newer text | none | revision-tagged `LintRecord` | yes | yes | `MOCK-UI` lint: "stale lint reply...". Desktop addition. |
| 33 | Device diagnostics shown in the editor, local advisory kept separate | none | `setDiagnostics(source:"device syntax")` | n/a | n/a | `MOCK-UI` lint (red squiggle). No local advisory source is wired. |

### Runs, status, logs

| # | Capability | iOS reference | Desktop component | USB | Wi-Fi | Evidence and limit |
|---|---|---|---|---|---|---|
| 34 | Foreground run opens the log, background does not | `testForegroundRunOpensConsoleAndMarksLog`, `testBackgroundRunKeepsConsoleClosed` | `run_flow` | yes | yes | `MOCK-UI` runs: "background run sends background:true and keeps the log closed; foreground opens it". |
| 35 | Single script slot: busy, confirm replace, cancel sends nothing, replace keeps the background flag | `testBusyPromptUsesTheHolderFromThe409Body`, `testConfirmReplaceKeepsBackgroundFlag`, `testCancelReplaceSendsNothing` | `sc-replace`, `replace:true` | yes | yes | `MOCK-UI` runs: "busy -> replace dialog...", "stop-and-replace keeps the background flag". Keyboard handling (focus on open, Escape, Tab trap, focus restore) fixed in the Dialogs component and verified by the editing and runs steps 2026-10-05 (F5). |
| 36 | No duplicate launch from double click or retry | `testRapidRunThenStopDiscardsLateRunResponse` | backend `in_flight`, no retry of run / eval / reset | yes | yes | `MOCK-UI` runs: "double click Run sends exactly one run", "unknown outcome: no automatic retry". The late-response-after-stop race is not driven by the harness (`UNIT` only). |
| 37 | Stop, final status, final traceback | `testStopRefreshesStatus`, `testFinishedRunDrainsFinalLogs` | `ScriptStore::stop`, pending-run settle | yes | yes | `MOCK-UI` runs: quick script, long run + Stop, failing script (red traceback, "failed" marker). |
| 38 | Status chip on every view | `ScriptPillRule`, `testPillShowsOnEveryTabWhileActive` | `ScriptRunChip` | yes | yes | `MOCK-UI` runs: "long run: global chip survives navigation"; evidence shots `ev-global-*` for daq, la, none. |
| 39 | Runs started elsewhere / by autorun are observed, never started by the UI | `testAttachFollowsConnectionState` | announce marker, `Source` label | yes | yes | `MOCK-UI` runs: "run started by another client or boot autorun is observed..."; disconnect / reconnect step. |
| 40 | Elapsed: exact when known, `>=` when only observed | `testElapsedFromEpochStart`, `testElapsedFromUptimeAnchor` | `Elapsed` | yes | yes | `UNIT`. UI shows `>= Ns` for external runs (seen in shots). Epoch clock path not driven by the harness. |
| 41 | Poll cadence by visibility | `testConsoleVisiblePollsStatusAndLogsEverySecond`, `testPillCadenceIsFiveSeconds`, `testIdleAndHiddenPollsOnlyStatus` | `PollPolicy` | yes | yes | `UNIT`; `MOCK-UI` runs: "polling cadence..." (relative only). |
| 42 | Device reboot detected | `testDeviceLogRestartMarkerAndCursorReset` | `LogPage.restarted` | yes | yes | `MOCK-UI` runs ("device reboot during a run...") and logs ("device reboot... restart marker"). Detection needs the counter to go backwards; a reboot that out-logs the old cursor is invisible on the device too. |
| 43 | Non-draining cursor logs, independent readers | `ScriptsClient` logs | `logs {since}` | op 2 | `?since=` | `MOCK-UI` logs: "independent reader does not drain the UI". Device side: `HOST-FW`. |
| 44 | Split UTF-8, CRLF, partial line | `testAssemblerKeepsSplitUTF8Intact`, `testAssemblerStripsCR`, `testAssemblerCarriesPartialLine` | `LogAssembler` | yes | yes | `MOCK-UI` logs: split at the 4096-byte page boundary, CRLF, partial line. |
| 45 | Gap marker only after first attach; capacity keeps newest | `testDroppedMarkerOnlyAfterFirstAttach`, `testCapacityKeepsNewest` | `LogStore` (5000 lines) | yes | yes | `MOCK-UI` logs: "ring overflow shows a gap marker". The 5000-line cap itself is `UNIT`. |
| 46 | Filter by level and text; markers rule | `testFilterByLevelKeepsMarkers` | `LogFilter` | n/a | n/a | `MOCK-UI` logs: "level + text filter...". |
| 47 | Local clear keeps the cursor | `testClearKeepsCursorResetDoesNot` | `sc-log-clear` | n/a | n/a | `MOCK-UI` logs. |
| 48 | Follow / pause-scroll, Latest button | `ScriptLogConsoleView` | `sc-log-pause`, `sc-log-latest` | n/a | n/a | `MOCK-UI` logs. Pause stops auto-scroll, it does not stop ingesting (as on iOS). |
| 49 | Copy, export | `UIPasteboard`, `ScriptLogExport` | `sc-log-copy`, `sc-log-export` | n/a | n/a | `MOCK-UI` logs (clipboard text, downloaded `.txt`). Blob download and clipboard in the real WebView are `LIVE-PENDING`. |

### REPL

| # | Capability | iOS reference | Desktop component | USB | Wi-Fi | Evidence and limit |
|---|---|---|---|---|---|---|
| 50 | Persistent evaluation | `WebSocketREPL` (Wi-Fi), BLE eval | `eval {persist:true}` | tunnel op 10 | `POST /api/scripts/eval` | `MOCK-UI` repl. Shipped difference: **no WebSocket REPL on the desktop**; the badge reads "USB tunnel eval" / "Wi-Fi REST eval". |
| 51 | Read-only while a file script holds the slot, with Stop | `testREPLReadOnlyWhileFileSlotHeld`, `testREPLGate` | `sc-repl-banner` | yes | yes | `MOCK-UI` repl: "file script holds the slot...", "race: slot taken between check and send". |
| 52 | Reset VM with confirmation | none in the REPL view | `Dialog::Reset` | op 16 | `/api/scripts/reset` | `MOCK-UI` repl: "Reset VM needs confirmation..." (`NameError` afterwards). |
| 53 | History recall, multiline | not on the WebSocket REPL surface | `sc-repl-input` | yes | yes | Desktop addition. `MOCK-UI` repl. |
| 54 | Copy REPL output | `repl_copy_button` | `sc-repl-copy` | - | - | Built 2026-10-05 (shared `copy_text`); compiles, not yet exercised by a harness step. |

### Autorun

| # | Capability | iOS reference | Desktop component | USB | Wi-Fi | Evidence and limit |
|---|---|---|---|---|---|---|
| 55 | Status and refresh | `ScriptAutorunPanel`, `testAutorunRefreshAndDisable` | `sc-autorun` | op 12 | `/autorun/status` | `MOCK-UI` autorun. A run observed on the status poll (boot autorun, another client) or its end now refreshes the panel; verified 2026-10-05 (F7). |
| 56 | Enable / change / disable | `onEnable`, `onDisable` | `Dialog::Autorun` (confirmation added) | ops 13 / 14 | `/autorun/enable`, `/disable` | `MOCK-UI` autorun. iOS has no confirmation; the desktop adds one. |
| 57 | Failure keeps state and shows the error | `testAutorunEnableDisableFailureKeepsStateAndShowsError` | `ScriptStore` | yes | yes | `MOCK-UI` autorun: "enable failure keeps the previous state...". |
| 58 | Never enabled implicitly by save or run; never restarted by the UI on reconnect | brief | n/a | yes | yes | `MOCK-UI` autorun. |
| 59 | Run-now | **not shipped on iOS** | **no UI control** (backend op 15 exists, USB only) | backend only | `unsupported` | Harness asserts `autorun_run` is never sent and no control exists. Matches the brief: do not invent it. |
| 60 | IO12 gate wording | `testAutorunRows` | `AutorunStatus::rows` | yes | yes | `MOCK-UI` autorun ("Suppressed: IO12 held low"). |

### Shell, layout, keyboard

| # | Capability | iOS reference | Desktop component | Evidence and limit |
|---|---|---|---|---|
| 61 | Light and dark | `testThemeColours` | tokens, `data-theme` | `MOCK-UI` matrix at 1440 / 1100 / 800 in both themes, HAT daq / la / none; screenshots in `scratch/desktop-scripting/harness-shots/ev-*`. |
| 62 | Narrow layouts: list and docs as drawers | `ScriptLogColumnLayout` | `data-narrow`, `data-compact`, `.sc-drawer-scrim` | `MOCK-UI` matrix (verified 2026-10-05, all HATs x themes x 800/1100/1440): the list drawer closes when a file or the REPL opens (F8), and the empty state shows a Stored scripts button whenever the list is hidden (F9). |
| 63 | Keyboard: save, run, background run, stop, check, log toggle | none (touch) | `on_key` in the view | `MOCK-UI` editing / runs. Modal dialogs take focus and trap Tab (F5). |
| 64 | WASM stability | n/a | `Console` component | F6 fixed 2026-10-05: delayed scroll callbacks in `Console`, `ReplPane` and `Dialogs` use `try_get_untracked`; no console or page errors in the runs, editing, autorun and matrix groups. |

## Explicit gaps and shipped limits (no placeholders)

- No live evidence: the real Tauri WebView (CDP), USB tunnel on hardware, Wi-Fi on hardware, clipboard / download in the WebView, and the Tauri event `script-upload-progress` are `LIVE-PENDING`.
- S3 firmware full-TU build is not part of this matrix (backend report: host-compiled extractions only).
- No WebSocket REPL, no BLE, no local (non-catalogue) completions, no sanitiser on editor input.
- iOS has no autorun run-now and neither does the desktop UI; Wi-Fi has no route for it.
- Wi-Fi `get` returns the whole file in one reply (backend report).
