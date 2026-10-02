"""WEB-NULL gate: no cJSON_GetObjectItem(...)->value* dereference without a
guard for the same key. The audit suspected 13 of 17 sites were unguarded;
re-reading showed every one is preceded by VALIDATE_JSON_FIELD (which returns
400 on a missing or mistyped key) or uses a ternary null check. This keeps it
that way."""

import re

from tests.lib.srcread import read_source

FILES = ("Firmware/ESP32/src/web/webserver.cpp", "Firmware/ESP32/src/net/api_core.cpp")
_DEREF = re.compile(r'cJSON_GetObjectItem\w*\(\s*(\w+)\s*,\s*"(\w+)"\s*\)\s*->\s*value')


def _unguarded():
    for path in FILES:
        lines = read_source(path).splitlines()
        for n, line in enumerate(lines):
            for obj, key in _DEREF.findall(line):
                if re.search(rf'cJSON_GetObjectItem\w*\(\s*{obj}\s*,\s*"{key}"\s*\)\s*\?', line):
                    continue
                window = "\n".join(lines[max(0, n - 12):n])
                if re.search(rf'VALIDATE_JSON_FIELD\(\s*{obj}\s*,\s*"{key}"', window):
                    continue
                yield f"{path}:{n + 1}: {line.strip()}"


def test_gate_sees_the_known_sites():
    total = sum(len(_DEREF.findall(read_source(p))) for p in FILES)
    assert total >= 17


def test_no_unguarded_cjson_dereference():
    assert list(_unguarded()) == []
