# WebSocket REPL

BugBuster exposes an interactive MicroPython REPL over WebSocket at
`/api/scripts/repl/ws`.  The on-device web UI connects automatically from
the **Scripts** tab; the notes below are for direct / scripted use.

## URL

```
ws://<device-ip>/api/scripts/repl/ws
```

If the device is reached over HTTPS, use `wss://` instead.

## Authentication

The WebSocket handshake itself is unauthenticated (HTTP does not allow
standard headers on the `GET /...` upgrade request in most browsers).
Authentication happens via the **first text frame** sent after the handshake
completes:

1. Open the WebSocket.
2. Wait for `onopen`.
3. Send the 64-char admin bearer token as a plain text frame.

If the token is invalid the server responds with WebSocket close code **4001**
and drops the connection immediately.  Re-pair the device and retry.

## Single-session lock

Only one REPL session is allowed at a time.  If a second client connects while
a session is active it receives close code **4002** ("session in use") and is
disconnected.  Close the existing browser tab before opening a new one.

## Input

After authentication, each keystroke or paste is sent as a text frame.
The server echoes the characters back so the terminal renders them.

| Key / byte | Effect |
|---|---|
| Printable chars | Accumulated in a line buffer (max 511 chars). |
| `CR` or `LF` (0x0D / 0x0A) | Submits the current line to MicroPython for evaluation. |
| `Backspace` (0x08 or 0x7F) | Removes the last character from the line buffer. |
| `Ctrl-C` (0x03) | Calls `scripting_stop()` — injects `KeyboardInterrupt` into the running VM. |

Each submitted line is evaluated in **persistent mode** (`persist=true`), so
globals are retained between lines exactly as in a standard REPL.

## Output

MicroPython `print()` output, tracebacks, and the `>>>` prompt are forwarded to
the WebSocket in real time.  Output from concurrently running scripts (submitted
via `/api/scripts/eval`) is also forwarded to the active REPL session.

## Ctrl-C behaviour

Sending byte `0x03` while a long-running script is executing causes a
`KeyboardInterrupt` to be raised at the next VM back-edge poll (typically within
a few milliseconds).  This is the same mechanism used by the `/api/scripts/stop`
REST endpoint.

## Example — raw WebSocket client (Python)

```python
import asyncio
import websockets

TOKEN = "your-64-char-admin-token-here"

async def repl():
    uri = "ws://192.168.1.42/api/scripts/repl/ws"
    async with websockets.connect(uri) as ws:
        # Step 1: authenticate
        await ws.send(TOKEN)
        # Step 2: read welcome banner
        banner = await ws.recv()
        print(banner, end="")
        # Step 3: send a line and read the response
        await ws.send("print('hello from REPL')\r")
        while True:
            data = await asyncio.wait_for(ws.recv(), timeout=2.0)
            print(data, end="", flush=True)

asyncio.run(repl())
```

## Limitations

- **Single session**: only one client at a time.
- **Line-based eval**: each CR-terminated line is submitted as a standalone
  `scripting_run_string()` call.  Multi-line constructs (e.g. `for` loops,
  `def`) must be entered on a single line using `;` or sent as a complete block
  via `/api/scripts/eval`.  A full pyexec REPL continuation mode is planned
  for a future phase.
- **Output latency**: output is forwarded via a 4 KB ring buffer drained by a
  dedicated FreeRTOS task.  Under heavy output (e.g. tight `print` loops) the
  ring may fill and drop bytes; use `time.sleep_ms(1)` in tight loops to yield.
