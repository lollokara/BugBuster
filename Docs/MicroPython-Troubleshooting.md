# MicroPython Troubleshooting

On-device MicroPython runs a FreeRTOS task on Core 0 with a 1 MB GC heap
allocated from PSRAM. Each script submission goes through a depth-4 queue;
output is routed to an in-memory log ring and simultaneously forwarded to the
browser REPL WebSocket. This guide covers the most common failure patterns.

See also: [`Docs/MicroPython Examples/`](MicroPython%20Examples/) for
annotated working examples.

---

## 1. Script won't eval — "queue_full" or no response

**Symptom:** `POST /api/scripts/eval` returns `{"ok": false, "err": "queue_full"}`.

**Cause:** The eval queue has a fixed depth of 4. If 4 scripts are already
queued (or one long-running script is blocking the task), new submissions are
rejected immediately.

**What to do:**

1. Call `POST /api/scripts/stop` to request a `KeyboardInterrupt` in the
   running script.
2. Wait for `GET /api/scripts/status` → `"running": false`.
3. Re-submit your script.

The queue is not a FIFO pipeline for serial execution — it is a small buffer
to absorb bursts. For sequential workflows, wait for `"running": false` before
submitting the next eval.

---

## 2. Heap watermark warning — what 80% means

**Symptom:** `GET /api/scripts/status` returns `"watermarkSoftHit": true`, or
you see `"mode"` flip from `"PERSISTENT"` back to `"EPHEMERAL"` unexpectedly.

**Thresholds:**

| Threshold | Level | Action taken by firmware |
|---|---|---|
| 80% of heap used | Soft | `gc_collect()` triggered after eval; `watermarkSoftHit: true` in status |
| 95% of heap used | Hard | VM torn down and restarted; `autoResetCount` incremented |

The GC heap is 1 MB from PSRAM. 80% = ~820 KB used.

**Where to view:**

- `GET /api/scripts/status` → `globalsBytes`, `globalsCount`, `watermarkSoftHit`
- `GET /api/scripts/storage` → SPIFFS partition usage (separate from GC heap)

**What to do:**

- In persistent mode: delete unused globals explicitly (`del large_array`) or
  call `gc.collect()` from your script.
- If the hard watermark is repeatedly hit, split your work across multiple
  ephemeral evals rather than keeping everything in one persistent session.
- Check `autoResetCount` in the status response — non-zero means the VM has
  been force-reset at least once this boot.

---

## 3. Import errors — ModuleNotFoundError

**Symptom:** Script raises `ModuleNotFoundError: No module named 'foo'`.

**Causes and fixes:**

| Cause | Fix |
|---|---|
| Module not frozen into firmware | Use only modules listed in `Docs/MicroPython Examples/11_frozen_helpers.py` |
| VFS not mounted (ephemeral eval, cold start) | The VM mounts `vfs_posix` on `/` at each init — if you see this on first eval, try again; cold-start timing can occasionally lose the mount |
| Importing from SPIFFS path wrong | Files uploaded via `POST /api/scripts/files?name=foo.py` are stored in SPIFFS. Use `import foo` only in persistent mode with `vfs_posix` mounted; see example `14_vfs_import.py` |
| Circular import | Split into smaller modules |

The standard library subset available is frozen into the firmware image. There
is no pip or package manager on device.

---

## 4. REPL won't connect / disconnects immediately (close code 4002)

**Symptom:** The browser REPL tab shows a connection error or closes
immediately after the WebSocket handshakes.

**Cause:** The REPL WebSocket enforces a single-session lock. A second client
connecting while a session is active receives WebSocket close code **4002**
(`SESSION_IN_USE`) and is dropped.

**Fix:**

1. Close any other browser tab that has the REPL open.
2. If no other tab is open, the previous session may not have cleaned up.
   Refreshing the page or waiting ~30 seconds for the server-side idle timeout
   will release the slot.
3. On reconnect the session context is freshly initialized — there is no
   stale state from the previous connection.

The REPL WebSocket is at `ws://<device>/api/scripts/repl`.

---

## 5. Persistence vs. ephemeral mode

BugBuster supports two scripting modes:

| Mode | Behaviour |
|---|---|
| **EPHEMERAL** (default) | VM is torn down and re-initialized after every eval. Globals do not survive between evals. Clean and predictable; no heap accumulation. |
| **PERSISTENT** | VM stays alive across evals. Globals, imported modules, and open file handles persist. Switched to by passing `?persist=true` on `POST /api/scripts/eval`. |

To enter persistent mode:

```
POST /api/scripts/eval?persist=true
Body: x = 42
```

A subsequent eval in the same session can read `x`. The mode is sticky until
the VM is reset (idle timeout, hard watermark, or `POST /api/scripts/reset`).

To explicitly reset a persistent session:

```
POST /api/scripts/reset
X-BugBuster-Admin-Token: <token>
```

See example [`10_persistent_session.py`](MicroPython%20Examples/10_persistent_session.py).

---

## 6. stdout goes missing — the vfs_posix / fd 1 gotcha

**Symptom:** `print()` output does not appear in logs or the REPL, but the
script runs without error.

**Cause:** When MicroPython's `vfs_posix` is mounted (which happens at every
VM init), POSIX file descriptors 1 and 2 are captured by the VFS layer. Any
code that writes directly to the underlying libc `fd 1` (e.g., C extensions
calling `write(1, ...)` directly) bypasses MicroPython's output routing and
never reaches `mp_hal_stdout_tx_strn` — the function that feeds the log ring
and the REPL WebSocket.

**Fixes:**

- Use `print()` or `sys.stdout.write()` from Python — these route through
  `mp_hal_stdout_tx_strn` correctly.
- If you are writing a C extension: call `scripting_log_push()` (the internal
  C API) rather than writing to fd 1 directly.
- Never call `os.dup2()` or redirect `sys.stdout` to a VFS file object and
  expect the output to appear in the log ring.

---

## 7. Log polling with /api/scripts/logs

The log endpoint supports two modes:

**Destructive drain (no cursor):**

```
GET /api/scripts/logs
X-BugBuster-Admin-Token: <token>
```

Returns up to the entire log ring as `text/plain` and removes those bytes from
the ring. Use only when you own the only log consumer.

**Non-destructive cursor polling (recommended):**

```
GET /api/scripts/logs?since=0
X-BugBuster-Admin-Token: <token>
```

Returns bytes starting at absolute offset `since`. The response header
`X-BugBuster-Log-Next` contains the next cursor value to pass on the following
request. Bytes are never removed from the ring by cursor reads.

```
< HTTP/1.1 200 OK
< Content-Type: text/plain
< X-BugBuster-Log-Next: 4096
< Access-Control-Expose-Headers: X-BugBuster-Log-Next

<log text since offset 0>
```

Poll loop pattern:

```python
import requests, time

cursor = 0
TOKEN = "your-token"
BASE  = "http://bugbuster.local"

while True:
    r = requests.get(f"{BASE}/api/scripts/logs?since={cursor}",
                     headers={"X-BugBuster-Admin-Token": TOKEN})
    if r.text:
        print(r.text, end="")
    cursor = int(r.headers.get("X-BugBuster-Log-Next", cursor))
    time.sleep(0.2)
```

The ring wraps at a fixed size; if your script produces output faster than you
poll, older bytes are overwritten. `GET /api/scripts/status` will show the
total bytes ever accepted via `lastEvalAtMs` but there is no ring-overflow
counter exposed over HTTP.

---

## 8. Upload and autorun a script

**Step 1 — Upload the file:**

```bash
curl -X POST "http://bugbuster.local/api/scripts/files?name=blink.py" \
     -H "X-BugBuster-Admin-Token: $TOKEN" \
     -H "Content-Type: text/plain" \
     --data-binary @blink.py
```

Response: `{"ok": true}`

File names must match `[a-zA-Z0-9_.-]+\.py` and be at most 32 characters.
Maximum file size is 32 768 bytes.

**Step 2 — Run it once:**

```bash
curl -X POST "http://bugbuster.local/api/scripts/run-file?name=blink.py" \
     -H "X-BugBuster-Admin-Token: $TOKEN"
```

**Step 3 — Set it as autorun:**

```bash
curl -X POST "http://bugbuster.local/api/scripts/autorun/enable?name=blink.py" \
     -H "X-BugBuster-Admin-Token: $TOKEN"
```

The script runs automatically at boot. Check status:

```bash
curl "http://bugbuster.local/api/scripts/autorun/status" \
     -H "X-BugBuster-Admin-Token: $TOKEN"
```

Response:

```json
{
  "enabled": true,
  "has_script": true,
  "io12_high": false,
  "last_run_ok": true,
  "last_run_id": 3
}
```

Disable autorun:

```bash
curl -X POST "http://bugbuster.local/api/scripts/autorun/disable" \
     -H "X-BugBuster-Admin-Token: $TOKEN"
```

See example [`08_autorun_blink.py`](MicroPython%20Examples/08_autorun_blink.py).

---

## 9. Stopping a runaway script

If a script enters an infinite loop or hangs, send a cooperative stop request:

```bash
curl -X POST "http://bugbuster.local/api/scripts/stop" \
     -H "X-BugBuster-Admin-Token: $TOKEN"
```

This sets a volatile flag that the VM checks at every back-edge (the
`MICROPY_VM_HOOK_LOOP` hook). When the flag is seen while an `nlr_buf_t` is on
the stack, a `KeyboardInterrupt` is raised inside the script.

**Caveats:**

- The stop is **cooperative** — a script blocked inside a C extension that
  never returns to the Python VM will not be interrupted.
- C-level sleep calls go through `mp_hal_delay_ms()`, which checks the stop
  flag and will interrupt those too.
- After stopping, poll `GET /api/scripts/status` until `"running": false`
  before submitting a new eval.

BBP equivalent: `0xF8 SCRIPT_STOP`.

---

## 10. Channel API and common board gotchas

**Importing the bugbuster module:**

The `bugbuster` module is frozen into the MicroPython firmware image. Import it
directly:

```python
import bugbuster as bb
```

Do not attempt to `pip install` or `import bugbuster` from SPIFFS — the frozen
module takes precedence and the SPIFFS version will be shadowed.

**Channel numbering:** Channels are 0-indexed in the Python API (0–3), matching
the AD74416H physical channels.

**IO terminal numbering:** Digital IOs are 1-indexed in the DIO API (1–12),
matching the silkscreen labels. Do not pass 0.

**EFUSE silkscreen swap:** The PCA9535 HAL silently corrects the EFUSE3/EFUSE4
silkscreen cross on the PCB. The Python/MCP API uses natural order (1–4) with
no swap needed. See the project memory note on this.

**MUX routing before GPIO:** When configuring a digital IO as input or output
through a script, the firmware automatically routes the terminal through the
MUX (`bus_planner_route_digital_input`). If the MUX is already locked by
another path, a warning is logged but the GPIO configuration still proceeds.
Always check the log output after configuring IOs.

**ADC readings return raw codes, not voltages:** `get_adc_value()` returns a
raw 24-bit code unless you pass `convert=True` or use the higher-level
`measure_voltage()` / `measure_current()` helpers. See
[`02_channel_voltage_sweep.py`](MicroPython%20Examples/02_channel_voltage_sweep.py).
