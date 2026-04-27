# Script Management Reference

This document covers every way to load, list, run, stop, persist, and remove scripts on a BugBuster device over HTTP, USB, or BBP.

---

## 1. Authentication

All HTTP routes under `/api/scripts/*` require the `X-BugBuster-Admin-Token` header with a 64-character admin token.

**USB / BBP (binary protocol):** Cable = trust. No token required.

**Obtaining the token** (USB connection):

```bash
TOKEN=$(python -c "from bugbuster import connect_usb; b=connect_usb('/dev/cu.usbmodem...'); print(b.get_admin_token())")
export TOKEN
```

Or fetch via HTTP pairing flow in the web UI.

Then use in curl:

```bash
curl -H "X-BugBuster-Admin-Token: $TOKEN" http://device.local/api/scripts/...
```

---

## 2. Inline Eval (Ephemeral)

Submit Python source directly for immediate execution. Each eval initializes and tears down the interpreter independently.

### HTTP (curl)

```bash
curl -X POST http://device.local/api/scripts/eval \
  -H "X-BugBuster-Admin-Token: $TOKEN" \
  -H "Content-Type: text/plain" \
  --data-binary 'print("hello")'
```

Response:

```json
{"ok": true, "id": 42}
```

Or on error:

```json
{"ok": false, "err": "SyntaxError: invalid syntax"}
```

### USB / Python

```python
from bugbuster import connect_usb
bb = connect_usb('/dev/cu.usbmodem...')
result = bb.script_eval('print("hello")')
import time; time.sleep(0.1)
print(bb.script_logs())  # "hello\n"
```

### Retrieving logs and status

```bash
# Get logs (up to 1 KB per call)
curl http://device.local/api/scripts/logs \
  -H "X-BugBuster-Admin-Token: $TOKEN"

# Get status (running, ID, error count)
curl http://device.local/api/scripts/status \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

Python:

```python
logs = bb.script_logs()
status = bb.script_status()  # dict with is_running, script_id, total_runs, etc.
```

### Stopping a running script

```bash
curl -X POST http://device.local/api/scripts/stop \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

Python:

```python
bb.script_stop()
```

Note: Stop injects `KeyboardInterrupt` at the next `bugbuster.sleep()` call. Tight C loops do not interrupt immediately.

---

## 3. Persistent Eval (V2-A)

Keep the VM alive across multiple calls. Globals persist; state is shared.

### HTTP: Add `?persist=true` query parameter

```bash
curl -X POST http://device.local/api/scripts/eval?persist=true \
  -H "X-BugBuster-Admin-Token: $TOKEN" \
  --data-binary 'x = 5'

curl -X POST http://device.local/api/scripts/eval?persist=true \
  -H "X-BugBuster-Admin-Token: $TOKEN" \
  --data-binary 'print(x)'  # prints 5
```

### USB: Pass `persist=True`

```python
bb.script_eval('x = 5', persist=True)
time.sleep(0.1)
bb.script_eval('print(x)', persist=True)
time.sleep(0.1)
logs = bb.script_logs()  # "5\n"
```

### Context manager (auto-reset on exit)

```python
with bb.script_session() as s:
    s.eval("x = 10")
    s.eval("y = 20")
    logs = s.eval("print(x + y)")
    # VM auto-resets on __exit__
```

### Reset the VM

Tear down the persistent session and revert to ephemeral mode:

```bash
curl -X POST http://device.local/api/scripts/reset \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

Python:

```python
bb.script_reset()
```

### Memory and auto-reset policy

**Status fields** (returned by `GET /api/scripts/status`):

- `mode` — 0=ephemeral, 1=persistent
- `globalsBytes` — estimated heap used by globals
- `globalsCount` — number of global variables
- `watermarkSoftHit` — soft limit (80%) reached, `gc.collect()` ran
- `autoResetCount` — number of times VM auto-reset due to threshold or idle

**Auto-reset triggers:**

1. **Soft watermark (≥80% heap):** runs `gc.collect()` automatically
2. **Hard watermark (≥95% heap):** tears down the VM, reverts to ephemeral mode
3. **Idle timeout:** if ≥10 minutes have passed with no new `script_eval` call, VM resets

Example (Python):

```python
bb.script_eval('huge = [i for i in range(50000)]', persist=True)
time.sleep(0.1)
status = bb.script_status()
if status.get('watermarkSoftHit'):
    print("Soft watermark hit, gc.collect() ran")
if status.get('mode') == 0:
    print("VM reset due to hard watermark or idle")
```

---

## 4. Stored Scripts (SPIFFS)

Upload Python files to the device and run them by name. Files are stored at `/spiffs/scripts/<name>`.

### Upload a script

**HTTP:**

```bash
curl -X POST 'http://device.local/api/scripts/files?name=blink.py' \
  -H "X-BugBuster-Admin-Token: $TOKEN" \
  --data-binary @blink.py
```

Response: `{"ok": true}` or `{"ok": false, "err": "..."}`

**Python:**

```python
with open('blink.py', 'r') as f:
    src = f.read()
bb.script_upload('blink.py', src)
```

### List stored scripts

**HTTP:**

```bash
curl http://device.local/api/scripts/files \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

Response:

```json
{"files": [{"name": "blink.py", "size": 256}, {"name": "test.py", "size": 512}]}
```

**Python:**

```python
files = bb.script_list()
for f in files:
    print(f"  {f['name']}: {f['size']} bytes")
```

### Get a script file

**HTTP:**

```bash
curl 'http://device.local/api/scripts/files/get?name=blink.py' \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

Returns raw Python source.

**Python:**

```python
src = bb.script_get('blink.py')
print(src)
```

### Delete a script file

**HTTP:**

```bash
curl -X DELETE 'http://device.local/api/scripts/files?name=blink.py' \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

**Python:**

```python
bb.script_delete('blink.py')
```

### Run a stored script by name

**HTTP:**

```bash
curl -X POST 'http://device.local/api/scripts/run-file?name=blink.py' \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

**Python:**

```python
bb.script_run_file('blink.py')
time.sleep(0.1)
logs = bb.script_logs()
```

**Limitation:** Files use a flat namespace; `MICROPY_VFS` is disabled, so scripts cannot `import` each other. Each script is hermetic.

---

## 5. Autorun at Boot (V2-B)

Enable a stored script to run automatically on every power-on or reset. Three safety gates must all pass:

1. **Sentinel file exists:** `/spiffs/.autorun_enabled` (created by enable route)
2. **5-second grace window:** if any HTTP/BBP/CLI activity in the first 5 s after boot, autorun is cancelled
3. **IO12 reads HIGH:** pin is sampled at boot; if LOW (pulled to GND), autorun is disabled

### Enable autorun

**HTTP:**

```bash
curl -X POST 'http://device.local/api/scripts/autorun/enable?name=boot_script.py' \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

**Python:**

```python
bb.script_autorun_enable('boot_script.py')
```

### Check autorun status

**HTTP:**

```bash
curl http://device.local/api/scripts/autorun/status \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

Response:

```json
{
  "enabled": true,
  "name": "boot_script.py",
  "has_script": true,
  "io12_high": true
}
```

**Python:**

```python
status = bb.script_autorun_status()
print(f"Enabled: {status['enabled']}, Script: {status['name']}, IO12: {status['io12_high']}")
```

### Disable autorun

**HTTP:**

```bash
curl -X POST http://device.local/api/scripts/autorun/disable \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

Removes the sentinel file but keeps the script file itself.

**Python:**

```python
bb.script_autorun_disable()
```

### Run the autorun script on-demand (USB only)

```python
bb.script_autorun_run_now()
time.sleep(0.1)
logs = bb.script_logs()
```

### Recovery

If autorun crashes the device:

- **Pull IO12 LOW** at boot (temporary jumper to GND) to bypass autorun
- **Connect within 5 seconds** of power-on to trigger the grace window
- **OTA bootloader rolls back** automatically within ~30 seconds if firmware never marks itself valid

---

## 6. WebSocket REPL (V2-B.1)

BugBuster exposes an interactive REPL over WebSocket for line-by-line evaluation with persistent globals. Full details at `repl.md`.

**Quick summary:**

- URL: `ws://device.local/api/scripts/repl/ws` (or `wss://` for HTTPS)
- Auth: send 64-char token as first text frame
- Single session lock: only one REPL client at a time
- Line-based eval: each CR-terminated line is evaluated in persistent mode
- Ctrl-C injects `KeyboardInterrupt`

---

## 7. Host-Side CLI (`python -m bugbuster.script`)

Convenient command-line interface for script operations.

### Global connection flags

```
--usb /dev/cu.usbmodem...    USB connection
--host IP --token TOKEN       HTTP connection
```

### Subcommands

**Run a script file:**

```bash
python -m bugbuster.script run path.py [--persist]
```

**Fetch logs (with optional tail mode):**

```bash
python -m bugbuster.script logs [--tail]
```

**Set autorun:**

```bash
python -m bugbuster.script autorun-set <name>
```

**Disable autorun:**

```bash
python -m bugbuster.script autorun-disable
```

**Reset persistent VM:**

```bash
python -m bugbuster.script reset
```

**Get status:**

```bash
python -m bugbuster.script status
```

### Example workflow

```bash
# Upload and set as autorun
python -m bugbuster.script run blink.py --usb /dev/cu.usbmodem...

# Check status
python -m bugbuster.script status --usb /dev/cu.usbmodem...

# Get logs
python -m bugbuster.script logs --usb /dev/cu.usbmodem...
```

---

## 8. Web UI Scripts Tab (V2-F)

The device web UI provides a full GUI for script management:

1. **Scripts tab** in the web UI (open `http://device.local/`)
2. **Left pane:** file tree of stored scripts
3. **Center pane:** CodeMirror editor for viewing/editing uploaded scripts
4. **Right pane:** log viewer and status dashboard
5. **Toolbar:** run, upload, delete, autorun toggle buttons
6. **Embedded REPL:** interactive terminal in the same tab

All operations route through the same HTTP `/api/scripts/*` endpoints documented above.

---

## 9. Wire / BBP Reference

BugBuster Protocol (BBP v4) script commands are multiplexed over USB CDC. No token required (cable = trust).

| Opcode | Command | Purpose | Payload | Response |
|--------|---------|---------|---------|----------|
| 0xF5 | SCRIPT_EVAL | Submit source inline | flags(1) + len(2) + src | ok(1) + id(4) |
| 0xF6 | SCRIPT_STATUS | Get engine state | — | running(1) + id(4) + runs(4) + errors(4) + err_len(1) + error(...) |
| 0xF7 | SCRIPT_LOGS | Drain log ring | — | len(2) + data |
| 0xF8 | SCRIPT_STOP | Request stop | — | ok(1) |
| 0xF9 | SCRIPT_UPLOAD | Store file to SPIFFS | name_len(2) + name + src_len(4) + src | ok(1) + err_len(1) + error(...) |
| 0xFA | SCRIPT_LIST | List stored files | — | count(2) + [name_len(2) + name + size(4)]* |
| 0xFB | SCRIPT_RUN_FILE | Run stored file | name_len(2) + name | ok(1) + id(4) + err_len(1) + error(...) |
| 0xFC | SCRIPT_DELETE | Delete stored file | name_len(2) + name | ok(1) + err_len(1) + error(...) |
| 0xFD | SCRIPT_AUTORUN | Autorun control (multiplexed) | op(1) + [name_len(2) + name]* | op-specific |

**Notes:**

- `SCRIPT_EVAL` flags bit 0: persist (1 = persistent mode)
- `SCRIPT_RESET` is HTTP/Python-only; no BBP opcode exists
- WebSocket REPL is HTTP-only; no BBP path
- All opcodes are defined in `Firmware/ESP32/src/bbp/bbp.h`

---

## 10. Troubleshooting

**"queue_full" response**

The script queue (depth 4) is exhausted. A previous script is still running.

**Fix:** Call `POST /api/scripts/stop` first, wait a moment, then retry.

**KeyboardInterrupt not raised in tight C loops**

The VM does not interrupt during blocking operations in C (e.g., `spi.transfer()`, I2C transaction).

**Fix:** Sprinkle `bugbuster.sleep(1)` into long Python loops to yield and check the stop flag.

**"Source too large" error**

Script source exceeds 32 KB.

**Fix:** Split into multiple files. Upload one, run it with `run-file`, then run the next sequentially.

**Persistent globals leaked (watermark hit)**

Status shows `watermarkSoftHit=true` or `autoResetCount` has incremented.

**Fix:** Call `POST /api/scripts/reset` (HTTP) or `bb.script_reset()` (Python) to tear down the VM. Or wait for the 10-minute idle timeout.

**Autorun won't fire at boot**

Check all three gates via `GET /api/scripts/autorun/status`:

1. Is `enabled=true`?
2. Is `has_script=true`?
3. Is `io12_high=true`?

If `io12_high=false`, you have a pull-down on IO12 or a button held. Wire a pull-up or remove the external load.

**WebSocket REPL closes immediately**

- Close code **4001:** token mismatch — re-pair and retry
- Close code **4002:** another REPL session is active — close the other tab first
