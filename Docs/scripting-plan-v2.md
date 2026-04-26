# BugBuster On-Device Scripting — V2 Plan

> **v1 status:** Phases 0–7 done and bench-verified 2026-04-26 (see
> `Docs/scripting-progress.md`). v1 ships hermetic per-eval scripting with
> HTTP + BBP transports, channel + I2C + SPI bindings, SPIFFS-stored scripts,
> and `autorun.py` with three-gate safety.
>
> **Scope of this doc:** V2 turns scripting from "useful feature" into "an
> interactive, integrated developer surface." V3 (platform: sandboxing,
> debugger, fleet) is referenced at the bottom — full V3 spec stays in
> `Docs/scripting-plan.md` § "v3 — Platform".
>
> **Companion docs:**
> - `Docs/scripting-plan.md` — original v1+v2+v3 roadmap (canonical for V3).
> - `Docs/scripting-progress.md` — v1 implementation state.
> - `Docs/MicroPython Examples/README.md` — user-facing API guide.

---

## Why V2

v1 answers "can the device run a Python script?" with **yes**. V2 answers
the next user question — "can I iterate on it without round-tripping a full
eval each time?" — and removes the most-felt friction:

- **No persistent state** between `script_eval` calls (each `mp_init` /
  `mp_deinit` cycle wipes globals). Iterative tuning loops re-pay setup.
- **No REPL.** Bench debugging means "edit, paste into curl, observe logs."
- **No frozen helpers.** Common boilerplate (channel sweep wrappers, sensor
  drivers) has to be re-pasted into every eval.
- **No web UI.** All script work is curl/Python-client. Non-developer users
  can't drive it.
- **No rich network bindings.** Scripts can't HTTP-fetch a config or POST a
  measurement to a host.

V2 doesn't add new transports (USB, HTTP, BBP cover the access matrix). It
makes those transports more useful.

---

## Architectural decisions carried from v1

These are **not** revisited in V2 unless explicitly noted in a phase. Refer
to `Docs/scripting-progress.md` § "Key technical decisions" for the canonical
list.

| Locked-in | Why it stays in V2 |
|---|---|
| MicroPython 1.24.1 submodule | LTS for Xtensa LX7; no compelling V2 driver to bump |
| `MICROPY_NLR_SETJMP=1` | LX7 has no native asm NLR |
| `MICROPY_EMIT_XTENSA=0` (bytecode only) | V2-D explicitly revisits |
| 1 MB PSRAM GC heap | V2-A may grow per-slot; V3-A formalises quotas |
| `taskMicroPython` Core 0 prio 1, queue depth 4 | V2-A may grow queue; V2-G replaces with multi-task |
| Per-eval `gc_init→mp_init→run→mp_deinit` (hermetic) | V2-A makes this conditional |
| `MP_STATE_PORT MP_STATE_VM` (no port slots; statics in scripting.cpp) | V2-A may need port slots for persistent globals |
| qstr/moduledefs/root_pointers via `execute_process()` at CMake configure | works under PIO/SCons; preserve |
| `MICROPY_VFS=0` | re-enabling pulls in `vfs_fat.c` (needs `FFCONF_H`) and `vfs_posix.c` (needs `<poll.h>`); revisit only with selective MP source-list exclusion + poll.h shim |
| Bindings stay C; HW access C++ via `extern "C"` bridge | `modbugbuster*.c` pattern; new V2 modules follow it |
| BBP wire version v4 | bump only when frame format changes |

### Deferred items v1 left for V2

- **MICROPY_VFS / `import` from /spiffs/** — first attempt in Phase 6a
  pulled in vfs_fat.c which can't compile under our ESP-IDF newlib. Re-enable
  needs (a) MP source-list exclusion of `vfs_fat*.c`, (b) `<poll.h>` shim or
  port file with the few `struct pollfd` symbols `vfs_posix.c` references.
  V2-A is a clean place to reattempt.
- **BBP CRC handshake fragility** — transient queue-desync after a CRC
  mismatch on USB CDC. Workaround today: reconnect or use HTTP. V2-H may
  formalise client-side recovery.
- **`pullups='internal'` warns silently.** Add `mp_warning` when V2 lands.
- **`allow_split_supplies` kwarg** for I2C/SPI not exposed (host MCP has it).

---

## V2 phase ordering

Each phase is **independent unless noted.** Recommended sequence: A → H → C
→ B → F → D → E → G. The dependency graph:

```
v1 ┐
   ├─ V2-A persistent state ────────────────┐
   ├─ V2-H host ergonomics ─────────────┐    │
   ├─ V2-C frozen modules ──────────────┼────┤
   ├─ V2-B REPL ────────────────────────┼────┤── V3-A sandboxing
   ├─ V2-F web UI ─ depends on V2-B.1 ──┘    │   (multi-VM contexts)
   ├─ V2-D native emitter ────────────────────│
   ├─ V2-E network ──────────────────────────┤
   └─ V2-G threading (NOT default-shipping) ─┘
```

`bus_planner` (v1 Phase 4) and `script_storage` (v1 Phase 6a) are reusable
for any V2 binding that needs hardware routing or file persistence.

---

# V2-A — Persistent interpreter state across eval calls

**Goal:** keep MicroPython initialised between calls so tuning loops keep
their globals, modules, and import cache.

**Surface:**
- `POST /api/scripts/eval?persist=true` — keeps interpreter alive after this
  eval. Subsequent `eval` calls (with or without `persist`) see the prior
  globals.
- `POST /api/scripts/reset` — explicit teardown back to hermetic mode.
- `GET /api/scripts/status` reports new fields: `mode: 'ephemeral' | 'persistent'`,
  `globals_count`, `globals_bytes_est`, `last_eval_at_ms`, `idle_for_ms`.
- BBP: new opcode `BBP_CMD_SCRIPT_RESET = 0xFD` … wait — 0xFD is taken by
  `SCRIPT_AUTORUN`. **Allocate `BBP_CMD_SCRIPT_RESET = 0xFE`**. (`0xFE` is
  PING per `bbp.h:226`.) Pick from the next free contiguous slot — currently
  none in 0xC0..0xFF after v1 fully fills 0xF5..0xFD plus PING/DISCONNECT at
  0xFE/0xFF. Either:
  - (a) reuse `BBP_CMD_SCRIPT_AUTORUN = 0xFD` with a new `sub` value
    (`sub=4 RESET_VM`, `sub=5 STATUS_PERSISTED` etc.), or
  - (b) bump wire version to v5 and refactor opcode space.
  **(a) is recommended** — opcode multiplexing already works for autorun.
- `python/bugbuster/client.py`:
  - `script_eval(src, persist: bool = False)` — kw arg.
  - `script_reset()` — torches the persistent VM.

**Implementation:**
- `scripting.cpp` adds a state field `enum ScriptingMode { EPHEMERAL,
   PERSISTENT }` and `int64_t s_last_eval_us`.
- Per-eval lifecycle becomes:
  - Mode `EPHEMERAL` (default): unchanged from v1 — `gc_init → mp_init → run
    → mp_deinit`.
  - Mode `PERSISTENT`: on first `eval?persist=true`, do `gc_init + mp_init`
    once; subsequent evals reuse the VM (run only). `mp_deinit` only on
    explicit reset, idle-timeout, or watermark trigger.
- **Auto-reset triggers** (defensive — persistent VMs leak):
  - **Soft heap watermark**: 80% of `MP_HEAP_SIZE`. When crossed during an
    eval, schedule `gc.collect()` immediately after the eval returns. Track
    in status.
  - **Hard watermark**: 95%. Force `mp_deinit` + auto-revert to `EPHEMERAL`.
    Status records `auto_reset_count`.
  - **Idle timeout**: 10 min default (configurable via
    `MP_PERSISTENT_IDLE_MS`). Auto-reset to free PSRAM.
- `MP_STATE_PORT` may need root-pointer slots for persistent state. Per v1
  decision #11 we use `MP_STATE_VM`; persistence may require revisiting.
  **Spike first** before committing.

**Files:**
- MOD `Firmware/ESP32/src/scripting.{h,cpp}` — mode field + state machine.
- MOD `Firmware/ESP32/src/webserver.cpp` — parse `?persist=true`, add reset
  route.
- MOD `Firmware/ESP32/src/cmds/cmd_script.cpp` — new sub-codes on 0xFD or new
  opcode.
- MOD `python/bugbuster/client.py` — `persist` kw, `script_reset()`.
- MOD `tests/mock/handlers/scripts.py` — track persistent state.
- NEW `tests/unit/test_scripts_persistent.py`.
- MOD `Firmware/BugBusterProtocol.md` — sub-code table.
- MOD `Docs/MicroPython Examples/` — new example: incremental DAC sweep
  using `bb.script_eval(..., persist=True)`.

**Verification gate:**
1. Two evals with `persist=true`: first sets `x = 5`, second reads `x` →
   prints 5.
2. After `script_reset()`, third eval reads `x` → `NameError`.
3. Eval that allocates 850 KB blob → status reports watermark hit + GC.
4. Eval that leaks 1.2 MB → auto-reset back to `EPHEMERAL`; status shows
   `auto_reset_count == 1`.
5. After 10 min idle → auto-reset.

**Risks:**
- Persistent globals may pin objects that hold C pointers to freed memory
  if our bindings forget their cleanup. Audit `modbugbuster_*` for stale
  pointers across evals (especially `bugbuster.I2C` / `bugbuster.SPI`
  instances — they hold no firmware-side handles, so should be safe).
- VFS-disabled MP doesn't reload modules from disk. Persistent `import` is
  fine for built-ins; user-added `.py` files won't be reachable until
  V2-C-frozen or VFS comes online.

---

# V2-B — Interactive REPL

**Goal:** an interactive `>>>` prompt accessible from a host, with line
editing and persistent VM state (V2-A is the substrate).

Two transport options — pick one. **B.1 recommended.**

## V2-B.1 — WebSocket REPL via xterm.js

**Surface:**
- `GET /api/scripts/repl/ws` — WebSocket upgrade, admin-auth via initial
  bearer-token frame.
- Browser client: `Firmware/ESP32/web/src/tabs/scripts/Repl.tsx` using
  xterm.js. Connects, displays prompt, sends keystrokes, renders output.

**Implementation:**
- ESP-IDF httpd has a WebSocket server (since v4.4). One `httpd_ws_handler`
  per session. Buffer keystrokes into a per-session line; on `\r` push the
  line into `mp_repl_continue_with_input()`.
- Enable `MICROPY_HELPER_REPL=1`, `MICROPY_REPL_AUTO_INDENT=1`,
  `MICROPY_KBD_EXCEPTION=1` (already set).
- One persistent VM per WS session. Sessions are mutually exclusive (one
  REPL at a time). Future V3-A formalises multi-slot.

**Cost:** xterm.js bundle ~150 KB compressed → SPIFFS asset (gated behind a
sdkconfig flag for 4 MB bench boards, per `feedback_flash_budget_testing`).

## V2-B.2 — Third TinyUSB CDC interface

**Surface:** any serial terminal connected to a third CDC-ACM endpoint sees
a MicroPython REPL.

**Implementation:**
- `CONFIG_TINYUSB_CDC_COUNT=3` (currently 2). Add `src/repl_cdc.cpp` that
  registers a CDC RX handler routing into MP REPL.
- No browser dependency, no auth gate (cable = trust).

**Risk:** Windows USB descriptor cache. Adding a third interface re-numbers
descriptors; existing Windows hosts that paired with a 2-CDC device may need
"forget device" + reinstall. Mac/Linux unaffected.

**Files (B.1):**
- NEW `Firmware/ESP32/src/repl_ws.cpp` — WS handler + line buffering.
- MOD `Firmware/ESP32/src/webserver.cpp` — register `/api/scripts/repl/ws`.
- NEW `Firmware/ESP32/web/src/tabs/scripts/Repl.tsx` — xterm.js client.
- MOD `Firmware/ESP32/web/package.json` — add xterm dep.
- MOD `mpconfigport.h` — REPL macros (mostly already on).

**Verification gate (B.1):**
1. Browser connects, sees `>>> ` prompt within 1 s.
2. `1+1` → `2`. `import bugbuster` → silent. `bugbuster.Channel(0)`
   instantiates. Auto-indent works after `if x:`.
3. `Ctrl-C` injects `KeyboardInterrupt`.
4. Disconnect mid-session — server cleans up VM; next connect starts fresh.
5. Two browsers: second sees `409 Conflict` (or queues; design choice).

**Depends on:** V2-A (persistent VM is the REPL substrate).

---

# V2-C — Frozen modules in firmware

**Goal:** ship `bb_helpers`, `bb_devices`, `bb_logging` as pre-compiled
`.mpy` modules baked into firmware. `import bb_helpers` works without VFS.

**Surface:**
- New built-in importable names: `bb_helpers` (channel sweep, ramp, settle),
  `bb_devices` (TMP102, BMP280, MCP3008, common SPI flash IDs), `bb_logging`
  (formatted log helpers).
- A starter library shipped in the firmware tree: `python/firmware_modules/`.

**Implementation:**
- `MICROPY_MODULE_FROZEN_MPY=1` in `mpconfigport.h`.
- `mpy-cross` invocation at CMake configure time (alongside qstr pipeline)
  produces `frozen_content.c`.
- ~50 KB flash budget per `Docs/scripting-plan.md` § "Memory & flash budget".
- Disk-loaded modules (post-V2 VFS) of same name shadow frozen — standard
  MicroPython behaviour.

**Files:**
- NEW `python/firmware_modules/bb_helpers.py`
- NEW `python/firmware_modules/bb_devices.py`
- NEW `python/firmware_modules/bb_logging.py`
- MOD `Firmware/ESP32/components/micropython/CMakeLists.txt` — `mpy-cross`
  step + freeze-list generation.
- MOD `mpconfigport.h` — `MICROPY_MODULE_FROZEN_MPY=1`.

**Verification gate:**
1. `import bb_helpers; bb_helpers.dac_ramp(0, 0.0, 5.0, 1.0)` → drives the
   DAC.
2. `bb_devices.TMP102(i2c, 0x48).read_celsius()` → returns a float.
3. Flash size delta < 80 KB.
4. `bb_helpers` is overridable: `import bb_helpers` after uploading a
   `bb_helpers.py` to /spiffs/scripts/ shadows the frozen one (post-V2
   VFS — defer this last test until VFS lands).

---

# V2-D — Native emitter (`@micropython.native`, `@micropython.viper`)

**Goal:** allow tight loops in scripts to compile to native LX7 instructions
for ~5–10× speedup.

**Implementation:**
- Flip `MICROPY_EMIT_XTENSA=1`.
- Native emitter requires the emitted code to live in `MALLOC_CAP_INTERNAL |
  MALLOC_CAP_EXEC` regions. ESP32-S3 has IRAM but it's tight; PSRAM is NOT
  executable. Plan: a small static IRAM pool sized e.g. 16 KB for native
  code blobs, with per-eval reset.
- Risk: LX7 emitter has historical edge-case bugs. **Re-spike 2 days before
  committing** — try compiling a representative `@native` block, run on
  hardware, verify behaviour.

**Verification gate:**
1. `@micropython.native\ndef sum(n):\n  s=0\n  for i in range(n): s+=i\n
   return s` runs and returns correct value for n=1000.
2. Microbenchmark: same loop without `@native` vs with → 5× speedup minimum.
3. 100-iteration eval loop with `@native` block — no IRAM exhaustion.

**Risk:** `@viper` adds more aggressive lowering (raw int math). Defer
`@viper` to a follow-up if `@native` lands cleanly.

---

# V2-E — Network bindings (selective)

**Goal:** scripts can fetch a config or POST a measurement without manual
HTTP plumbing.

**Surface:**
```python
import bugbuster
r = bugbuster.http_get("https://api.example.com/cfg", headers={"X-Key": "..."})
print(r.status, r.body)
bugbuster.http_post("https://logs.example.com/m", body=b'{"v":3.3}',
                    headers={"Content-Type":"application/json"})
bugbuster.mqtt_publish("bench/temp", b"23.5", host="mqtt.local")
```

**Implementation:**
- `bugbuster.http_get/http_post` wrap `esp_http_client`. TLS via Mozilla CA
  bundle on SPIFFS.
- `bugbuster.mqtt_publish` wraps ESP-IDF MQTT component (mqtt5 if available).
- **Deliberately no raw `socket`/`select` exposure.** Scripts that need that
  level of control are an anti-pattern for this device.

**Files:**
- NEW `Firmware/ESP32/src/modbugbuster_net.c` — type bindings.
- MOD `Firmware/ESP32/src/modbugbuster_bridge.{h,cpp}` — `extern "C"`
  forwards.
- NEW `Firmware/ESP32/src/net_bridge.cpp` — esp_http_client / MQTT wrappers.
- MOD `mpconfigport.h` — qstrs.
- NEW `data/cacert.pem` (Mozilla bundle) — uploaded to SPIFFS during
  manufacturing or on first WiFi connect.

**Verification gate:**
1. `http_get('https://www.howsmyssl.com/a/check')` → 200, body contains
   "rating".
2. `http_post` to a netcat listener — host receives the body intact.
3. `mqtt_publish` against `mqtt://test.mosquitto.org:1883` — `mosquitto_sub`
   on host receives the message.
4. Bad CA bundle → cleanly raises `OSError(MP_EIO)` with a useful message,
   doesn't crash the script task.

**Risk:**
- TLS handshakes are heap-hungry (mbedTLS). Soft watermark may trigger;
  audit. PSRAM is preferred for buffers.
- Slow networks — HTTP calls block the script for seconds. Cooperative
  cancellation must wake from inside the http_client wait. Use a separate
  worker task that polls a stop flag, not a synchronous call.

---

# V2-F — Web UI Scripts tab

**Goal:** non-developer users can edit, run, save, and watch logs from a
browser.

**Surface:** new tab in the existing web UI:
- File tree of `/spiffs/scripts/`.
- CodeMirror 6 editor (~70 KB compressed) with Python mode.
- Toolbar: Run / Stop / Save / Set-as-autorun / Disable-autorun.
- Log pane streaming `/api/scripts/logs` via SSE.
- Status badge: mode (ephemeral/persistent), heap watermark, last error.

**Implementation:**
- `Firmware/ESP32/web/src/tabs/scripts/Scripts.tsx`.
- Feature-flag behind `scriptingUiEnabled` toggle (System tab) so 4 MB
  bench boards can opt out.

**Depends on:** V2-A (status fields), V2-B.1 (REPL pane in same tab —
optional sub-component).

**Verification gate:**
1. UI loads, file tree shows existing scripts.
2. Edit + Save → file appears in `script_list()` from CLI/HTTP.
3. Run → log pane streams output live.
4. Set-as-autorun → `autorun_status.enabled == true`.

---

# V2-G — Threaded MicroPython (`_thread`)

> **Highest risk; default is to NOT ship.**

**Goal:** scripts can spawn cooperative threads, e.g. one polling I2C,
another driving DAC.

**Implementation:**
- Each Python thread maps to a FreeRTOS task on Core 0.
- `MICROPY_PY_THREAD=1` (currently 0 per `mpconfigport.h`).
- GIL holds except inside `mp_hal_delay_ms` and `bugbuster.sleep`.
- **All `bugbuster.*` bindings need re-audit** for deadlock between MP GIL
  + `g_spi_bus_mutex` + `g_stateMutex` + `s_planner_mutex`. Lock ordering
  must be documented and enforced.

**Risk:** historical evidence (per `feedback_rp2040_dcd_bugs`-style
incidents) is that small ordering bugs become bench-time hangs under load.
Default to **NOT shipping** unless a concrete user need forces it. V3-A's
sandboxing is a cleaner path to "multiple things running at once."

---

# V2-H — Host-side ergonomics

**Goal:** Python client and CLI tooling improvements that don't require
firmware changes.

**Surface:**
```python
# Context manager keeps a persistent VM warm
with bb.script_session() as s:
    s.eval("x = 5")
    print(s.eval("x * 10"))   # 50

# Decorator style
@bb.on_device
def sweep():
    import bugbuster
    for v in range(0, 50, 5):
        bugbuster.Channel(0).set_voltage(v / 10)
        bugbuster.sleep(50)

sweep()  # transparently posts to /api/scripts/eval

# CLI
$ python -m bugbuster.script run path/to/script.py --persist
$ python -m bugbuster.script logs --tail
$ python -m bugbuster.script autorun-set blink.py
```

Plus an MCP tool `run_device_script(src, persist=False)` for AI-assisted
workflows.

**Implementation:**
- `python/bugbuster/script.py` — context manager + decorator.
- `python/bugbuster/__main__.py` — CLI entry.
- `python/bugbuster_mcp/tools/scripting.py` — MCP tool.

**Also lands here (carries from v1):**
- **BBP CRC fragility recovery.** When a `TimeoutError` is raised waiting
  for response, drain the input buffer and retry once. If the second attempt
  also fails, surface the timeout. This is the "BBP CRC" known limitation
  from v1.
- `pullups='internal'` warning surfaces in client (host-side) since firmware
  doesn't `mp_warning` today.

**Verification gate:**
1. `with bb.script_session() as s` round-trips — `x = 5` then `x` reads 5.
2. `@bb.on_device` decorated function runs on device, returns to host (via
   logs scrape or explicit return-channel — design choice).
3. `python -m bugbuster.script run` and `logs --tail` work end-to-end.
4. Inject a forced CRC mismatch — client transparently recovers within 1 s.

---

# V2 cross-cutting

## Memory & flash budget (incremental on top of v1)

Updated based on v1 actuals (v1 final: 1,470,924 B flash, 31.9% RAM).

| Phase | Flash delta | DRAM delta | PSRAM delta |
|---|---|---|---|
| V2-A persistent | +20 KB | 0 | +0–500 KB depending on user globals (idle reuse) |
| V2-B.1 REPL + xterm | +80 KB (+150 KB SPIFFS asset) | +20 KB | +50 KB session |
| V2-B.2 alt: 3rd CDC | +10 KB | +6 KB (extra TinyUSB EP buffers) | 0 |
| V2-C frozen modules | +50 KB | 0 | 0 |
| V2-D native emitter | +30 KB + 16 KB IRAM exec pool | +16 KB IRAM | 0 |
| V2-E network | +60 KB (mbedTLS already linked) | 0 | +20 KB TLS buffers per call |
| V2-F web UI | +200 KB SPIFFS (CodeMirror + UI) | 0 | 0 |
| V2-G threading | +20 KB | per-thread TCB+stack | per-thread |
| V2-H host-side | 0 (host-only) | 0 | 0 |
| **V2 total (typical)** | **~+440 KB** | **~+40 KB** | **~+0.6 MB** |

Still comfortably within 6 MB OTA slot + 4.8 MB free PSRAM (per v1 budget
math in `Docs/scripting-plan.md` § "Memory & flash budget").

## Risks log (V2)

| Risk | Phase | Mitigation |
|---|---|---|
| Persistent VM root-pointer audit gaps | V2-A | Spike `MP_STATE_PORT` migration before committing |
| Persistent globals leak across evals (user code) | V2-A | Soft + hard watermark + idle reset; documented |
| Re-attempting MICROPY_VFS pulls vfs_fat | V2-A or V2-C | Selective MP source list + poll.h shim |
| WS REPL session conflict with concurrent eval | V2-B.1 | Single-session lock; future V3-A multi-slot |
| Windows CDC re-enumeration after 3rd CDC | V2-B.2 | Document; provide "forget device" recovery |
| LX7 native emitter edge cases | V2-D | 2-day spike; bench microbench |
| TLS heap exhaustion under load | V2-E | PSRAM buffers; per-call timeout; cooperative cancel |
| MP GIL + SPI bus mutex + state mutex deadlock | V2-G | Re-audit all bindings; lock ordering doc; default-off |
| Web UI bloats SPIFFS past 4 MB bench limit | V2-F | sdkconfig flag; gated build |

## Out of scope for V2 (revisit in V3)

- Multiple concurrent script slots with quotas (V3-A).
- HAT co-processing (V3-B).
- Live debugger / DAP (V3-C).
- Fleet orchestration (V3-D).
- Plugin slots (V3-F).
- Real-time data viz (V3-G).

---

## Looking ahead — V3

V3 is "scripting as a platform." Each V3 phase is significantly larger than
a V2 phase and depends on **V2-A (persistent VM) being shipped**. Don't
start V3 until V2 has delivered value to real users. Full spec lives in
`Docs/scripting-plan.md` § "v3 — Platform"; one-line summaries:

| V3 phase | Goal | Key prereq |
|---|---|---|
| **V3-A — Sandboxing & per-script quotas** | Multiple scripts run safely with independent heap, CPU-ms-per-min, allowed-modules whitelist, wallclock caps. New `/api/scripts/spawn`, `/slots`, `/slot/{id}/kill`. Per-slot `mp_state_ctx_t`. | V2-A (multi-context state mgmt) |
| **V3-B — HAT co-processing** | `bugbuster.hat.*` module: offload tight loops, PIO programs, LA capture, SWD reads to the RP2040 via the existing 0x48/0x4C mailbox. Round-trip ≤ 100 ms for 1024 LA samples. | v1 HAT subsystem; no V2 dep |
| **V3-C — Live debugging (DAP)** | `sys.settrace`-backed Debug Adapter Protocol over WebSocket. VS Code extension sets breakpoints, steps, watches. `MICROPY_PY_SYS_SETTRACE=1` (~10× slowdown when active; opt-in). | V2-A + V2-B.1 (WS infra) |
| **V3-D — OTA fleet orchestration** | Host-side `bb_fleet push/run/aggregate --tag X`. mDNS discovery via `_bugbuster._tcp` TXT records. Per-device + fleet-token auth. SQLite result store. | v1 OTA path; no V2 dep |
| **V3-E — Script library / marketplace** | Curated `python/bugbuster_library/` of test recipes; MCP `find_script(intent)` for semantic discovery. | V2-H (MCP tool path) |
| **V3-F — Cross-language plugin slots** | Static-linked-but-runtime-selectable Rust/C plugins exposing `bugbuster.*` extensions. Manifest-gated allowed imports. | V3-A (quotas to bound plugin damage) |
| **V3-G — Real-time data viz** | `bugbuster.viz.scope(channel=0, samples=512)` opens a host-side viz panel via WebSocket. Live scope traces from scripts. | V2-F (web UI), V3-A (CPU quota for viz streamers) |

V3 is intentionally far. Most BugBuster users will be well-served by v1 +
V2-A through V2-F. Reach for V3 only when fleet-scale, sandboxing, or
debugger-class workflows become real user demands.

---

## Next steps when picking up V2

1. **Read `Docs/scripting-progress.md`** for the v1 final state and the
   "MICROPY_VFS attempt failed" lessons.
2. **Pick V2-A first** unless a specific user need overrides — every other
   V2 phase improves when persistent state exists.
3. **Spike before committing** any V2 phase that touches the build pipeline
   (V2-C frozen modules, V2-D native emitter) — both have historical
   landmines under our PIO/SCons pipeline.
4. **Update `Docs/scripting-progress.md`** as each phase lands. Mirror the
   v1 phase-status table format.
5. **Update `Docs/MicroPython Examples/`** with a new example per shipped
   V2 phase. Persistent state, REPL, frozen modules, network — all merit
   their own runnable script.
