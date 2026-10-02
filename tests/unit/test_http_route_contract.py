"""Contract: every HTTP path the Python package sends resolves to a firmware
handler that actually serves it, and every JSON body key it sends is read there.

``tests/synthetic/test_http_route_existence.py`` only checks registration, so a
path that lands on a wildcard dispatcher (``POST /api/hat/*``) passes there even
when that dispatcher answers 404. This file follows the dispatch chain down to a
leaf handler:

1. ``httpd_register_uri_handler`` order in ``webserver.cpp`` (first match wins,
   trailing ``*`` matches any suffix, as ``httpd_uri_match_wildcard``).
2. Dispatchers: ``if (strstr(req->uri, "...")) return h(req);`` and
   ``if (strcmp(suffix, "...") == 0) return h(req);`` (suffix = text after the
   wildcard prefix with the numeric id and its slash stripped).
3. Delegation into ``api_core_handle()`` (``net/api_core.cpp``), whose own
   ``strcmp(path, ...)`` / prefix-block dispatch is followed the same way.

Body keys are what the leaf (plus the helpers it calls, one level) reads with
``cJSON_GetObjectItem``, ``body_get`` or ``VALIDATE_JSON_FIELD``.

Python side: AST of every module under ``python/bugbuster`` - ``_http_get/
_http_post/_http_delete`` on any receiver, ``get/post/delete`` on ``self._t`` or
``self`` (HTTPTransport), ``self._session.get/post`` with a ``{self._base}``
prefix, and the OTA ``_upload``/``_push_daq`` helpers.
"""

from __future__ import annotations

import ast
import re
from dataclasses import dataclass, field
from pathlib import Path

import pytest

from tests.lib.srcread import REPO_ROOT, read_source

WEBSERVER = "Firmware/ESP32/src/web/webserver.cpp"
ADAPTER = "Firmware/ESP32/src/web/http_adapter.cpp"
API_CORE = "Firmware/ESP32/src/net/api_core.cpp"
PY_PKG = REPO_ROOT / "python" / "bugbuster"

# Concrete stand-in for an f-string path parameter (/channel/{ch}/..). "0" is
# valid for every parameterised route the client builds (channel, bridge, slot,
# fault channel, gpio, dio).
SAMPLE_PARAM = "0"


# ---------------------------------------------------------------------------
# Allow-lists. Real bugs are xfail(strict=True); intentional exceptions are
# skipped with the reason recorded here. Delete an entry when it is fixed - the
# strict xfail will flag it.
# ---------------------------------------------------------------------------

# (METHOD, path template) -> reason. Path resolves to no leaf handler.
KNOWN_UNRESOLVED_BUGS: dict[tuple[str, str], str] = {}

# (METHOD, path template, key) -> reason. Key sent but never read (real bug).
KNOWN_UNREAD_KEY_BUGS: dict[tuple[str, str, str], str] = {}

# (METHOD, path template, key) -> reason. Intentionally unread; not a bug.
INTENTIONAL_UNREAD_KEYS: dict[tuple[str, str, str], str] = {}


# ---------------------------------------------------------------------------
# C source helpers
# ---------------------------------------------------------------------------

def _skip_literal(text: str, i: int) -> int:
    """Index just past the string/char literal or comment starting at *i*,
    or *i* if none starts there."""
    c = text[i]
    if c in "\"'":
        j = i + 1
        while j < len(text) and text[j] != c:
            j += 2 if text[j] == "\\" else 1
        return j + 1
    if text.startswith("//", i):
        j = text.find("\n", i)
        return len(text) if j < 0 else j
    if text.startswith("/*", i):
        j = text.find("*/", i + 2)
        return len(text) if j < 0 else j + 2
    return i


def _match_brace(text: str, open_idx: int) -> int:
    """Index of the '}' closing the '{' at *open_idx*."""
    depth = 0
    i = open_idx
    while i < len(text):
        j = _skip_literal(text, i)
        if j != i:
            i = j
            continue
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced braces")


_FUNC_DEF = re.compile(r"^(?:static\s+)?[\w\s\*]+?\b(\w+)\s*\([^;{}()]*\)\s*\{", re.MULTILINE)


def c_functions(text: str) -> dict[str, str]:
    out = {}
    for m in _FUNC_DEF.finditer(text):
        name = m.group(1)
        if name in ("if", "for", "while", "switch"):
            continue
        open_idx = m.end() - 1
        out.setdefault(name, text[open_idx:_match_brace(text, open_idx) + 1])
    return out


_KEY_READ = re.compile(
    r'\b(?:cJSON_GetObjectItem(?:CaseSensitive)?|body_get|VALIDATE_JSON_FIELD|json_get_\w+)'
    r'\(\s*\w+\s*,\s*"([^"]+)"')


@dataclass
class Firmware:
    routes: list[tuple[str, str, str]]          # (METHOD, uri, handler) in registration order
    web: dict[str, str]                          # webserver.cpp + http_adapter.cpp functions
    core: dict[str, str]                         # api_core.cpp functions
    core_dispatch: str = ""

    @classmethod
    def load(cls) -> "Firmware":
        web_src = read_source(WEBSERVER)
        adapter_src = read_source(ADAPTER)
        core_src = read_source(API_CORE)
        routes = [(m, u, h) for u, m, h in re.findall(
            r'\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_([A-Z]+)\s*,\s*\.handler\s*=\s*(\w+)',
            web_src)]
        # http_adapter_register() runs after every webserver.cpp registration
        # except the browser fallback, which is "/*" and never an /api route.
        routes += [(m, u, h) for u, m, h in re.findall(
            r'\{\s*"(/api/[^"]+)"\s*,\s*HTTP_([A-Z]+)\s*,\s*(\w+)', adapter_src)]
        routes = [r for r in routes if r[1].startswith("/api/")]
        web = c_functions(web_src)
        web.update(c_functions(adapter_src))
        core = c_functions(core_src)
        return cls(routes, web, core, core.get("api_core_handle", ""))


@dataclass
class Resolution:
    chain: list[str] = field(default_factory=list)
    keys: set[str] = field(default_factory=set)
    error: str = ""


def _match_route(fw: Firmware, method: str, path: str):
    for m, uri, handler in fw.routes:
        if m != method:
            continue
        if uri.endswith("*") and path.startswith(uri[:-1]):
            return uri, handler
        if uri == path:
            return uri, handler
    return None, None


def _suffix_after(prefix: str, path: str) -> str:
    rest = path[len(prefix):]
    rest = rest.lstrip("0123456789")
    return rest[1:] if rest.startswith("/") else rest


def _keys_of(funcs: dict[str, str], name: str) -> set[str]:
    body = funcs[name]
    keys = set(_KEY_READ.findall(body))
    for callee in set(re.findall(r"\b(\w+)\s*\(", body)) - {name}:
        if callee in funcs:
            keys |= set(_KEY_READ.findall(funcs[callee]))
    return keys


_STRSTR_DISPATCH = re.compile(
    r'if\s*\(\s*strstr\(\s*req->uri\s*,\s*"([^"]+)"\s*\)\s*\)\s*(?:return\s+(\w+)\s*\(\s*req\s*\)|\{)')
_SUFFIX_DISPATCH = re.compile(
    r'if\s*\(\s*strcmp\(\s*(?:suffix|p)\s*,\s*"([^"]*)"\s*\)\s*==\s*0\s*\)\s*return\s+(\w+)\s*\(\s*req\s*\)')
_CORE_CALL = re.compile(r'api_core_handle\(\s*"(\w+)"\s*,\s*(?:"([^"]+)"|req->uri)')


def _resolve_core(fw: Firmware, method: str, path: str, res: Resolution) -> None:
    text = fw.core_dispatch
    for lit, fn in re.findall(
            r'strcmp\(\s*path\s*,\s*"([^"]+)"\s*\)\s*==\s*0\s*\)\s*return\s+(\w+)\s*\(', text):
        if lit == path:
            res.chain.append(f"api_core:{fn}")
            res.keys |= _keys_of(fw.core, fn)
            return
    for m in re.finditer(r'if\s*\(\s*strncmp\(\s*path\s*,\s*"([^"]+)"\s*,\s*\d+\s*\)\s*==\s*0\s*\)\s*'
                         r'(return\s+(\w+)\s*\(|\{)', text):
        prefix, fn = m.group(1), m.group(3)
        if not path.startswith(prefix):
            continue
        if fn:
            res.chain.append(f"api_core:{fn}")
            res.keys |= _keys_of(fw.core, fn)
            return
        block = text[m.end() - 1:_match_brace(text, m.end() - 1) + 1]
        if "if (is_post)" in block:
            post_part, _, get_part = block.partition("} else {")
            block = post_part if method == "POST" else get_part
        sfx = _suffix_after(prefix, path)
        for lit, sub in re.findall(
                r'strcmp\(\s*sfx\s*,\s*"([^"]+)"\s*\)\s*==\s*0\s*\)\s*return\s+(\w+)\s*\(', block):
            if lit == sfx:
                fn = sub
                break
        else:
            if sfx == "":
                hit = re.search(r"\*sfx\s*==\s*'\\0'\s*\)\s*return\s+(\w+)\s*\(", block)
                fn = hit.group(1) if hit else None
            for needle, sub in re.findall(
                    r'strstr\(\s*path\s*,\s*"([^"]+)"\s*\)\s*!=\s*NULL\s*\)\s*return\s+(\w+)\s*\(', block):
                if not fn and needle in path:
                    fn = sub
        if fn:
            res.chain.append(f"api_core:{fn}")
            res.keys |= _keys_of(fw.core, fn)
            return
    res.error = f"api_core_handle() has no branch for {path}"


def resolve(fw: Firmware, method: str, path: str) -> Resolution:
    """Follow *path* (no query string) from registration to a leaf handler."""
    res = Resolution()
    uri, handler = _match_route(fw, method, path)
    if handler is None:
        res.error = f"no {method} registration matches {path}"
        return res
    prefix = uri[:-1] if uri.endswith("*") else uri
    seen = set()
    while True:
        if handler in seen or handler not in fw.web:
            res.error = f"handler {handler} not found"
            return res
        seen.add(handler)
        res.chain.append(handler)
        body = fw.web[handler]
        strstr = _STRSTR_DISPATCH.findall(body)
        suffix = _SUFFIX_DISPATCH.findall(body)
        if strstr:
            nxt = next(((needle, h) for needle, h in strstr if needle in path), None)
            if nxt is None:
                res.error = f"{handler} has no branch for {path} (dispatcher 404)"
                return res
            if not nxt[1]:
                break  # handled inline in the dispatcher
            handler = nxt[1]
            continue
        if suffix:
            sfx = _suffix_after(prefix, path)
            nxt = next((h for lit, h in suffix if lit == sfx), None)
            if nxt is None:
                res.error = f"{handler} has no branch for suffix {sfx!r} (dispatcher 404)"
                return res
            handler = nxt
            continue
        break
    res.keys |= _keys_of(fw.web, handler)
    call = _CORE_CALL.search(fw.web[handler])
    if call:
        _resolve_core(fw, call.group(1), call.group(2) or path, res)
    return res


# ---------------------------------------------------------------------------
# Python client collector
# ---------------------------------------------------------------------------

_HELPER_METHODS = {"_http_get": "GET", "_http_post": "POST", "_http_delete": "DELETE",
                   "_http_put": "PUT", "_upload": "POST", "_push_daq": "POST"}
_VERB_METHODS = {"get": "GET", "post": "POST", "delete": "DELETE", "put": "PUT"}


@dataclass(frozen=True)
class ClientCall:
    method: str
    template: str            # /api/... with {} for path parameters
    body_keys: frozenset | None
    where: str

    @property
    def concrete(self) -> str:
        return self.template.replace("{}", SAMPLE_PARAM)


def _is_self_base(node: ast.expr) -> bool:
    return (isinstance(node, ast.FormattedValue) and isinstance(node.value, ast.Attribute)
            and node.value.attr == "_base")


def _path_template(node: ast.expr) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if not isinstance(node, ast.JoinedStr):
        return None
    values = list(node.values)
    if values and _is_self_base(values[0]):
        values = values[1:]
    parts: list[str] = []
    for i, v in enumerate(values):
        if isinstance(v, ast.Constant):
            parts.append(str(v.value))
        elif i == len(values) - 1 and parts and (parts[-1].endswith("?") or not parts[-1].endswith("/")):
            break  # trailing query string: f"/scripts/run-file?{qs}", f"/scripts/eval{qs}"
        elif not parts:
            return None  # f"{self._base}{endpoint}" - not a literal path
        else:
            parts.append("{}")
    return "".join(parts)


def _receiver_ok(func: ast.Attribute) -> bool:
    if func.attr in _HELPER_METHODS:
        return True
    recv = func.value
    if isinstance(recv, ast.Name) and recv.id == "self":
        return True                       # HTTPTransport.get/post on itself
    return isinstance(recv, ast.Attribute) and recv.attr in ("_t", "_session")


def client_calls() -> list[ClientCall]:
    out = []
    for py in sorted(PY_PKG.rglob("*.py")):
        tree = ast.parse(read_source(py))
        rel = py.relative_to(REPO_ROOT).as_posix()
        # Per-function locals: url = f"{self._base}/pairing/verify" and
        # body = {...} passed by name instead of inline.
        local_urls: dict[tuple[int, str], ast.expr] = {}
        owner: dict[int, int] = {}
        for fn in ast.walk(tree):
            if isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
                for sub in ast.walk(fn):
                    owner[id(sub)] = id(fn)  # innermost wins: walk order is outer-first
        for node in ast.walk(tree):
            if (isinstance(node, ast.Assign) and len(node.targets) == 1
                    and isinstance(node.targets[0], ast.Name)
                    and (isinstance(node.value, ast.Dict) or _path_template(node.value) is not None)):
                local_urls[(owner.get(id(node), 0), node.targets[0].id)] = node.value
        for node in ast.walk(tree):
            if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.args):
                continue
            attr = node.func.attr
            method = _HELPER_METHODS.get(attr) or _VERB_METHODS.get(attr)
            if method is None or not _receiver_ok(node.func):
                continue
            arg0 = node.args[0]
            if isinstance(arg0, ast.Name):
                arg0 = local_urls.get((owner.get(id(node), 0), arg0.id), arg0)
            tpl = _path_template(arg0)
            if tpl is None or not tpl.startswith("/"):
                continue
            tpl = tpl.split("?")[0]
            if not tpl.startswith("/api/"):
                tpl = "/api" + tpl
            keys = None
            body = node.args[1] if len(node.args) > 1 else None
            if isinstance(body, ast.Name):
                body = local_urls.get((owner.get(id(node), 0), body.id), body)
            if isinstance(body, ast.Dict):
                d = body
                if all(isinstance(k, ast.Constant) and isinstance(k.value, str) for k in d.keys):
                    keys = frozenset(k.value for k in d.keys)  # type: ignore[union-attr]
            out.append(ClientCall(method, tpl, keys, f"{rel}:{node.lineno}"))
    return out


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

FW = Firmware.load()
CALLS = client_calls()
UNIQUE = sorted({(c.method, c.template) for c in CALLS})


def test_collectors_find_both_sides():
    assert len(FW.routes) > 120
    assert len(FW.core) > 50
    assert len(UNIQUE) > 100
    files = {c.where.split(":")[0] for c in CALLS}
    for expected in ("python/bugbuster/client.py", "python/bugbuster/ota.py",
                     "python/bugbuster/bus.py", "python/bugbuster/transport/http.py"):
        assert expected in files, f"collector found no HTTP calls in {expected}"


@pytest.mark.parametrize("method,path,expect_ok", [
    ("POST", "/api/channel/0/dac", True),
    ("GET", "/api/channel/0/dac/readback", True),
    ("POST", "/api/quicksetup/0/apply", True),
    ("GET", "/api/quicksetup/0", True),
    ("POST", "/api/usbpd/caps", True),
    ("POST", "/api/channel/0/bogus", False),
    ("POST", "/api/hat/bogus", False),
    ("GET", "/api/definitely/not/a/route", False),
    ("DELETE", "/api/status", False),
    ("POST", "/api/uart/0/bogus", False),
])
def test_resolver_controls(method, path, expect_ok):
    res = resolve(FW, method, path)
    assert (res.error == "") is expect_ok, res.error or res.chain


def test_resolver_follows_api_core_delegation():
    res = resolve(FW, "POST", "/api/channel/0/dac")
    assert res.chain[-1] == "api_core:api_channel_dac"
    assert {"code", "voltage", "bipolar", "current_mA"} <= res.keys
    res = resolve(FW, "POST", "/api/quicksetup/0")
    assert res.chain[-1] == "api_core:api_quicksetup_save"
    res = resolve(FW, "GET", "/api/quicksetup/0")
    assert res.chain[-1] == "api_core:api_quicksetup_get"
    res = resolve(FW, "POST", "/api/gpio/0/set")
    assert res.chain[-1] == "api_core:api_gpio_set" and "value" in res.keys


def _route_params():
    out = []
    for method, tpl in UNIQUE:
        marks = []
        if (method, tpl) in KNOWN_UNRESOLVED_BUGS:
            marks.append(pytest.mark.xfail(strict=True, reason=KNOWN_UNRESOLVED_BUGS[(method, tpl)]))
        out.append(pytest.param(method, tpl, marks=marks, id=f"{method} {tpl}"))
    return out


@pytest.mark.parametrize("method,template", _route_params())
def test_client_path_reaches_a_firmware_handler(method, template):
    res = resolve(FW, method, template.replace("{}", SAMPLE_PARAM))
    where = sorted(c.where for c in CALLS if (c.method, c.template) == (method, template))
    assert res.error == "", f"{method} {template} ({', '.join(where)}): {res.error}"


def _key_params():
    out = []
    seen = set()
    for c in CALLS:
        if c.body_keys is None or (c.method, c.template) in KNOWN_UNRESOLVED_BUGS:
            continue
        for key in sorted(c.body_keys):
            ident = (c.method, c.template, key)
            if ident in seen or ident in INTENTIONAL_UNREAD_KEYS:
                continue
            seen.add(ident)
            marks = []
            if ident in KNOWN_UNREAD_KEY_BUGS:
                marks.append(pytest.mark.xfail(strict=True, reason=KNOWN_UNREAD_KEY_BUGS[ident]))
            out.append(pytest.param(c.method, c.template, key, marks=marks,
                                    id=f"{c.method} {c.template} [{key}]"))
    return out


@pytest.mark.parametrize("method,template,key", _key_params())
def test_client_body_key_is_read_by_the_handler(method, template, key):
    res = resolve(FW, method, template.replace("{}", SAMPLE_PARAM))
    assert res.error == "", res.error
    assert key in res.keys, (
        f"{method} {template}: firmware ({' -> '.join(res.chain)}) never reads {key!r}; "
        f"it reads {sorted(res.keys)}")


def test_allow_lists_reference_real_client_calls():
    """A stale allow-list entry would silently stop guarding anything."""
    unique = set(UNIQUE)
    for method, tpl in KNOWN_UNRESOLVED_BUGS:
        assert (method, tpl) in unique, f"{method} {tpl} is no longer called"
    sent = {(c.method, c.template, k) for c in CALLS if c.body_keys for k in c.body_keys}
    for ident in {**KNOWN_UNREAD_KEY_BUGS, **INTENTIONAL_UNREAD_KEYS}:
        assert ident in sent, f"{ident} is no longer sent"


# ---------------------------------------------------------------------------
# The routes this task audited: each is either called by the Python package
# (and so covered above + in test_http_client_routes.py) or listed here as
# having no Python caller. Adding a caller means removing it from this set.
# ---------------------------------------------------------------------------

AUDITED_ROUTES = [
    "/api/adc/dsp/start", "/api/adc/dsp/stop", "/api/adgs/routes", "/api/board/select",
    "/api/channel/{}", "/api/debug", "/api/diagnostics", "/api/diagnostics/config",
    "/api/hat/calibration", "/api/hat/power", "/api/hat/v2/calibrate/import",
    "/api/hat/v2/calibrate/status", "/api/hat/v2/caps", "/api/hat/v2/io_bank",
    "/api/hat/v2/io_voltage", "/api/hat/v2/la/log/enable", "/api/hat/v2/la/route",
    "/api/hat/v2/led", "/api/hat/v2/level_shift", "/api/hat/v2/rail/voltage",
    "/api/hat/v2/rails", "/api/idac/cal/clear", "/api/ioexp/rail_up", "/api/lshift/oe",
    "/api/ota/rollback", "/api/ota/upload_rp2040", "/api/pairing/info",
    "/api/pairing/rotate", "/api/pairing/verify", "/api/quicksetup/{}",
    "/api/scripts/autorun/disable", "/api/scripts/autorun/enable",
    "/api/scripts/autorun/status", "/api/scripts/lint", "/api/scripts/reset",
    "/api/scripts/run-file", "/api/scripts/stop", "/api/scripts/storage",
    "/api/selftest/calibrate", "/api/uart/{}", "/api/uart/config", "/api/uart/pins",
    "/api/usbpd/caps", "/api/wavegen/start", "/api/wavegen/stop", "/api/wifi/hostname",
]

NO_PYTHON_CALLER = {
    "/api/adgs/routes",
    "/api/hat/calibration",
    "/api/ota/upload_rp2040",
    "/api/pairing/rotate",
    "/api/scripts/lint",
    "/api/scripts/storage",
    "/api/usbpd/caps",
    "/api/wifi/hostname",
}


def _called(route: str) -> bool:
    if route.endswith("{}"):
        prefix = route[:-2]
        return any(t.startswith(prefix) and t != prefix.rstrip("/") for _, t in UNIQUE)
    return any(t == route for _, t in UNIQUE)


@pytest.mark.parametrize("route", AUDITED_ROUTES)
def test_audited_route_caller_inventory(route):
    route_exists = any(
        uri == route or (uri.endswith("*") and route.replace("{}", SAMPLE_PARAM).startswith(uri[:-1]))
        for _, uri, _ in FW.routes)
    assert route_exists, f"{route} is no longer registered by the firmware"
    assert _called(route) is (route not in NO_PYTHON_CALLER), (
        f"{route}: Python caller present={_called(route)}; update NO_PYTHON_CALLER")


def test_paths_are_relative_to_the_api_prefix():
    """HTTPTransport prepends /api; a literal "/api/..." would double it."""
    for py in sorted(Path(PY_PKG).rglob("*.py")):
        for i, line in enumerate(read_source(py).splitlines(), 1):
            if re.search(r'_http_(?:get|post|delete)\(\s*f?"/api/', line):
                pytest.fail(f"{py.name}:{i} passes an /api-prefixed path")
