"""Generate the golden DAQ STATUS fixture from the firmware header.

gcc compiles ``usb_proto.h`` itself, fills every ``usb_status_payload_t`` field
with a distinct value, and dumps the packed bytes plus each field's real
``offsetof`` and value. Every host decoder (Python ``daq_stream.py``,
``tests/lib/daq_records.py``, Rust ``daq_proto.rs``) is tested against this one
file, so a decoder can only pass by agreeing with the firmware layout.

    python -m tests.firmware_host.gen_daq_status_fixture     # regenerate

``tests/synthetic/test_fixture_freshness.py`` fails when the header changes
without a regeneration.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run
from tests.lib.srcread import REPO_ROOT, read_source

HEADER = "Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h"
STRUCT = "usb_status_payload_t"
OUT_DIR = REPO_ROOT / "tests" / "fixtures" / "daq"
BIN = OUT_DIR / "status.bin"
META = OUT_DIR / "status.json"

_C_TYPES = {"uint8_t": "u8", "int8_t": "i8", "uint16_t": "u16", "int16_t": "i16",
            "uint32_t": "u32", "int32_t": "i32", "float": "f32"}


def header_sha256() -> str:
    return hashlib.sha256(read_source(HEADER).encode("utf-8")).hexdigest()


def struct_fields() -> list[tuple[str, str]]:
    src = read_source(HEADER)
    end = re.search(r"\}\s*" + STRUCT + r"\s*;", src)
    if not end:
        raise LookupError(f"{STRUCT} not found in {HEADER}")
    start = src.rfind("typedef struct", 0, end.start())
    body = re.sub(r"//[^\n]*", "", src[src.index("{", start) + 1:end.start()])
    fields = []
    for ctype, name in re.findall(r"\b(u?int(?:8|16|32)_t|float)\s+(\w+)\s*(?:\[\d+\])?\s*;", body):
        if not name.startswith("_pad"):
            fields.append((ctype, name))
    return fields


def _value(i: int, ctype: str):
    kind = _C_TYPES[ctype]
    if kind == "f32":
        return 1.5 + i * 0.25          # exactly representable
    if kind == "i16":
        return -1000 - i               # signed: catches unsigned decoding
    if kind == "u8":
        return 0x10 + i
    if kind == "u16":
        return 0x1000 + i * 0x11
    return 0x01000000 + i * 0x010101   # u32: distinct in every byte


def generate() -> tuple[bytes, dict]:
    fields = struct_fields()
    assigns = "\n".join(f"    s.{n} = ({t}){_value(i, t)!r};" for i, (t, n) in enumerate(fields))
    offs = "\n".join(f'    printf("{n} %zu\\n", offsetof({STRUCT}, {n}));' for _t, n in fields)
    out = compile_and_run(f"""
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "usb_proto.h"
int main(void) {{
    {STRUCT} s; memset(&s, 0, sizeof s);
{assigns}
    printf("size %zu\\n", sizeof s);
{offs}
    const unsigned char *p = (const unsigned char *)&s;
    printf("hex ");
    for (size_t k = 0; k < sizeof s; k++) printf("%02x", p[k]);
    printf("\\n");
    return 0;
}}
""", include_dirs=[Path(HEADER).parent])
    lines = dict(line.split(" ", 1) for line in out.strip().splitlines())
    raw = bytes.fromhex(lines.pop("hex"))
    size = int(lines.pop("size"))
    assert len(raw) == size
    meta = {
        "source": HEADER,
        "source_sha256": header_sha256(),
        "struct": STRUCT,
        "size": size,
        "fields": {n: {"type": _C_TYPES[t], "offset": int(lines[n]), "value": _value(i, t)}
                   for i, (t, n) in enumerate(fields)},
    }
    return raw, meta


def main() -> None:
    raw, meta = generate()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    BIN.write_bytes(raw)
    META.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {BIN.relative_to(REPO_ROOT)} ({len(raw)} bytes), {len(meta['fields'])} fields")


if __name__ == "__main__":
    main()
