"""
Script engine handlers for SimulatedDevice.

Handles: SCRIPT_EVAL, SCRIPT_STATUS, SCRIPT_LOGS, SCRIPT_STOP,
         SCRIPT_UPLOAD, SCRIPT_LIST, SCRIPT_RUN_FILE, SCRIPT_DELETE.

State fields added to device in simulated_device.py:
  script_running      bool
  script_id           int   (monotonically incremented on each successful eval)
  script_total_runs   int
  script_total_errors int
  script_last_error   str
  script_log_ring     str   (drained by SCRIPT_LOGS)
  script_files        dict  (name -> bytes, in-memory SPIFFS store)
"""

import struct
from bugbuster.constants import CmdId

# Maximum accepted script body in the simulator (matches firmware 32 KB gate)
_SCRIPT_MAX_SRC = 32 * 1024
_SCRIPT_NAME_MAX = 32


def register(device) -> None:
    # Initialise in-memory file store if not already present
    if not hasattr(device, "script_files"):
        device.script_files = {}

    device.register_handler(CmdId.SCRIPT_EVAL,     _script_eval(device))
    device.register_handler(CmdId.SCRIPT_STATUS,   _script_status(device))
    device.register_handler(CmdId.SCRIPT_LOGS,     _script_logs(device))
    device.register_handler(CmdId.SCRIPT_STOP,     _script_stop(device))
    device.register_handler(CmdId.SCRIPT_UPLOAD,   _script_upload(device))
    device.register_handler(CmdId.SCRIPT_LIST,     _script_list(device))
    device.register_handler(CmdId.SCRIPT_RUN_FILE, _script_run_file(device))
    device.register_handler(CmdId.SCRIPT_DELETE,   _script_delete(device))
    device.register_handler(CmdId.SCRIPT_AUTORUN,  _script_autorun(device))


# ---------------------------------------------------------------------------
# SCRIPT_EVAL (0xF5)
# payload: u16 src_len, char[src_len] src
# resp:    u8 enqueued, u32 script_id
# ---------------------------------------------------------------------------

def _script_eval(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            return struct.pack('<BI', 0, 0)
        src_len, = struct.unpack_from('<H', payload, 0)
        if src_len > _SCRIPT_MAX_SRC or len(payload) < 2 + src_len:
            return struct.pack('<BI', 0, 0)

        # Enqueue: bump script_id, mark running
        device.script_id += 1
        device.script_running = True
        device.script_total_runs += 1
        device.script_last_error = ""
        # Append a synthetic log line so tests can verify SCRIPT_LOGS
        src = payload[2:2 + src_len].decode("utf-8", errors="replace")
        device.script_log_ring += f"[eval:{device.script_id}] {src[:40]}\n"

        return struct.pack('<BI', 1, device.script_id)
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_STATUS (0xF6)
# payload: (none)
# resp:    u8 is_running, u32 script_id, u32 total_runs, u32 total_errors,
#          u8 err_len, char[err_len] last_error
# ---------------------------------------------------------------------------

def _script_status(device):
    def handler(payload: bytes) -> bytes:
        err = device.script_last_error.encode("utf-8")[:64]
        buf  = struct.pack('<B', int(device.script_running))
        buf += struct.pack('<I', device.script_id)
        buf += struct.pack('<I', device.script_total_runs)
        buf += struct.pack('<I', device.script_total_errors)
        buf += struct.pack('<B', len(err))
        buf += err
        return buf
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_LOGS (0xF7)
# payload: (none)
# resp:    u16 count, char[count] log_bytes
# ---------------------------------------------------------------------------

def _script_logs(device):
    def handler(payload: bytes) -> bytes:
        chunk = device.script_log_ring[:1020].encode("utf-8", errors="replace")
        device.script_log_ring = device.script_log_ring[1020:]
        return struct.pack('<H', len(chunk)) + chunk
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_STOP (0xF8)
# payload: (none)
# resp:    (none)
# ---------------------------------------------------------------------------

def _script_stop(device):
    def handler(payload: bytes) -> bytes:
        device.script_running = False
        return b''
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_UPLOAD (0xF9)
# payload: u8 name_len, char[name_len] name, u16 body_len, u8[body_len] body
# resp:    u8 ok, u8 err_len, char[err_len] err
# ---------------------------------------------------------------------------

def _script_upload(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            err = b'bad payload'
            return struct.pack('<BB', 0, len(err)) + err
        name_len = payload[0]
        if name_len == 0 or name_len > _SCRIPT_NAME_MAX or len(payload) < 1 + name_len + 2:
            err = b'bad name_len'
            return struct.pack('<BB', 0, len(err)) + err
        name = payload[1:1 + name_len].decode('utf-8', errors='replace')
        pos = 1 + name_len
        body_len, = struct.unpack_from('<H', payload, pos)
        pos += 2
        if body_len > _SCRIPT_MAX_SRC or len(payload) < pos + body_len:
            err = b'body too large'
            return struct.pack('<BB', 0, len(err)) + err
        body = payload[pos:pos + body_len]
        device.script_files[name] = body
        return struct.pack('<BB', 1, 0)
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_LIST (0xFA)
# payload: (none)
# resp:    u8 count, [count × (u8 name_len, char[name_len] name)]
# ---------------------------------------------------------------------------

def _script_list(device):
    def handler(payload: bytes) -> bytes:
        names = list(device.script_files.keys())
        buf = struct.pack('<B', len(names))
        for n in names:
            nb = n.encode('utf-8')[:_SCRIPT_NAME_MAX]
            buf += struct.pack('<B', len(nb)) + nb
        return buf
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_RUN_FILE (0xFB)
# payload: u8 name_len, char[name_len] name
# resp:    u8 enqueued, u32 script_id
# ---------------------------------------------------------------------------

def _script_run_file(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            return struct.pack('<BI', 0, 0)
        name_len = payload[0]
        if name_len == 0 or name_len > _SCRIPT_NAME_MAX or len(payload) < 1 + name_len:
            return struct.pack('<BI', 0, 0)
        name = payload[1:1 + name_len].decode('utf-8', errors='replace')
        if name not in device.script_files:
            return struct.pack('<BI', 0, 0)
        # Simulate enqueue
        device.script_id += 1
        device.script_running = True
        device.script_total_runs += 1
        device.script_last_error = ""
        device.script_log_ring += f"[run_file:{device.script_id}] {name}\n"
        return struct.pack('<BI', 1, device.script_id)
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_DELETE (0xFC)
# payload: u8 name_len, char[name_len] name
# resp:    u8 ok, u8 err_len, char[err_len] err
# ---------------------------------------------------------------------------

def _script_delete(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            err = b'bad payload'
            return struct.pack('<BB', 0, len(err)) + err
        name_len = payload[0]
        if name_len == 0 or name_len > _SCRIPT_NAME_MAX or len(payload) < 1 + name_len:
            err = b'bad name_len'
            return struct.pack('<BB', 0, len(err)) + err
        name = payload[1:1 + name_len].decode('utf-8', errors='replace')
        if name not in device.script_files:
            err = b'not found'
            return struct.pack('<BB', 0, len(err)) + err
        del device.script_files[name]
        return struct.pack('<BB', 1, 0)
    return handler


# ---------------------------------------------------------------------------
# SCRIPT_AUTORUN (0xFD)
# payload: u8 sub [, ...]
#   sub=0  STATUS   resp: u8 enabled, u8 has_script, u8 io12_high,
#                         u8 last_run_ok, u32 last_run_id
#   sub=1  ENABLE   payload: u8 sub, u8 name_len, char[name_len] name
#                   resp: u8 ok, u8 err_len, char[err_len] err
#   sub=2  DISABLE  resp: u8 ok, u8 err_len, char[err_len] err
#   sub=3  RUN_NOW  resp: u8 ok, u32 script_id, u8 err_len, char[err_len] err
# ---------------------------------------------------------------------------

def _script_autorun(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            return struct.pack('<BB', 0, 0)
        sub = payload[0]

        if sub == 0:
            # STATUS
            has_script = (device.autorun_script_name is not None and
                          device.autorun_script_name in device.script_files)
            buf  = struct.pack('<B', int(device.autorun_enabled))
            buf += struct.pack('<B', int(has_script))
            buf += struct.pack('<B', int(device.autorun_io12_high))
            buf += struct.pack('<B', int(device.autorun_last_run_ok))
            buf += struct.pack('<I', device.autorun_last_run_id)
            return buf

        if sub == 1:
            # ENABLE
            if len(payload) < 3:
                err = b'bad payload'
                return struct.pack('<BB', 0, len(err)) + err
            name_len = payload[1]
            if name_len == 0 or name_len > _SCRIPT_NAME_MAX or len(payload) < 2 + name_len:
                err = b'bad name_len'
                return struct.pack('<BB', 0, len(err)) + err
            name = payload[2:2 + name_len].decode('utf-8', errors='replace')
            if name not in device.script_files:
                err = b'script not found'
                return struct.pack('<BB', 0, len(err)) + err
            device.autorun_enabled = True
            device.autorun_script_name = name
            return struct.pack('<BB', 1, 0)

        if sub == 2:
            # DISABLE
            device.autorun_enabled = False
            return struct.pack('<BB', 1, 0)

        if sub == 3:
            # RUN_NOW
            has_script = (device.autorun_script_name is not None and
                          device.autorun_script_name in device.script_files)
            if not has_script:
                err = b'no autorun script'
                return struct.pack('<BIB', 0, 0, len(err)) + err
            device.script_id += 1
            device.script_running = True
            device.script_total_runs += 1
            device.autorun_last_run_id = device.script_id
            device.autorun_last_run_ok = True
            device.script_log_ring += f"[autorun:{device.script_id}] {device.autorun_script_name}\n"
            return struct.pack('<BIB', 1, device.script_id, 0)

        err = b'bad sub'
        return struct.pack('<BB', 0, len(err)) + err
    return handler
