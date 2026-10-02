#!/usr/bin/env python3
"""Capture golden HTTP JSON fixtures from a live board - GET only, read-only.

The route list comes from the firmware sources (tests/lib/fw_routes.py), not
from docs. Routes with side effects are skipped by name, with the reason kept
next to them. Secrets are scrubbed before anything touches disk.

    python tests/tools/http_fixture_dump.py --host 192.168.3.51 [--token-env BB_TOKEN]

Writes tests/fixtures/http/<route>.json plus _index.json (route -> status,
firmware version). The token is read from an environment variable only, so it
never appears on a command line or in shell history.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from tests.lib.fw_routes import firmware_routes  # noqa: E402

OUT = REPO / "tests" / "fixtures" / "http"

SKIP = {
    "/api/scope/stream": "SSE loop occupies the only httpd task (WEB-23)",
    "/api/ws/stream": "WebSocket",
    "/api/scripts/repl/ws": "WebSocket",
    "/api/wifi/scan": "radio scan disturbs the WiFi link",
    "/api/update/check": "contacts GitHub and starts a worker",
    "/api/selftest": "may start a measurement",
    "/api/selftest/supplies": "runs a measurement, switches the U23 MUX",
    "/api/selftest/supplies/cached": "runs a monitor step in the handler (PWR-10)",
    "/api/selftest/efuse_imon": "runs a measurement, switches the U23 MUX",
    "/api/scripts/logs": "draining read: consumes the log buffer",
    "/api/hat/v2/la/log": "draining read",
    "/api/scripts/files/get": "needs a ?name= parameter",
}

_SECRET_PARTS = ("ssid", "pass", "psk", "secret")
_MAC = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")
_IPV4 = re.compile(r"^\d{1,3}(\.\d{1,3}){3}$")
_DEFAULT_AP_IP = "192.168.4.1"


def _is_secret(key: str) -> bool:
    k = key.lower()
    return any(p in k for p in _SECRET_PARTS) or k.endswith("token")


def _anonymise(v: str) -> str:
    # Format-preserving, so consumers that parse these still see valid values.
    if _MAC.match(v):
        return "02:00:00:00:00:01"
    if _IPV4.match(v) and v not in (_DEFAULT_AP_IP, "0.0.0.0"):
        return "10.0.0.2"
    return v


def scrub(obj):
    if isinstance(obj, dict):
        return {k: ("<scrubbed>" if _is_secret(k) and isinstance(v, str) else scrub(v))
                for k, v in obj.items()}
    if isinstance(obj, str):
        return _anonymise(obj)
    if isinstance(obj, list):
        return [scrub(v) for v in obj]
    return obj


def fetch(host: str, path: str, token: str | None, timeout: float):
    req = urllib.request.Request(f"http://{host}{path}")
    if token:
        req.add_header("X-BugBuster-Admin-Token", token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--token-env", default="BB_TOKEN",
                    help="environment variable holding the admin token")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--gap", type=float, default=0.25, help="seconds between requests")
    args = ap.parse_args()
    token = os.environ.get(args.token_env)

    routes = sorted({u for m, u in firmware_routes() if m == "GET" and not u.endswith("*")})
    OUT.mkdir(parents=True, exist_ok=True)
    index = {"host_role": "live board", "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
             "routes": {}}
    for path in routes:
        if path in SKIP:
            index["routes"][path] = {"skipped": SKIP[path]}
            continue
        try:
            status, body = fetch(args.host, path, token, args.timeout)
        except Exception as exc:  # timeout / connection reset: record and move on
            index["routes"][path] = {"error": type(exc).__name__}
            print(f"  ERR  {path}: {exc}")
            time.sleep(args.gap)
            continue
        entry = {"status": status}
        try:
            data = scrub(json.loads(body))
            name = path.removeprefix("/api/").replace("/", "__") + ".json"
            (OUT / name).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                                    encoding="utf-8")
            entry["file"] = name
        except ValueError:
            entry["non_json_bytes"] = len(body)
        index["routes"][path] = entry
        print(f"  {status}  {path}")
        time.sleep(args.gap)

    info = OUT / "device__info.json"
    if info.exists():
        index["device_info"] = json.loads(info.read_text(encoding="utf-8"))
    (OUT / "_index.json").write_text(json.dumps(index, indent=2, sort_keys=True) + "\n",
                                     encoding="utf-8")
    ok = sum(1 for e in index["routes"].values() if e.get("status") == 200)
    print(f"{ok} routes captured, {len(SKIP)} skipped by design -> {OUT.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
