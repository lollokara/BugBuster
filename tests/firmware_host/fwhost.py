"""Compile and run pure firmware logic on the host with gcc/g++ (or clang).

Firmware translation units mostly cannot be built off-target as a whole: every
``cmd_*.cpp`` pulls ``bbp.h`` -> ``ad74416h.h`` and FreeRTOS. So tests either
compile a whole file against the small stub set in ``stubs/``, or extract the
exact function text from the firmware source and compile that. Either way the
code under test is the real firmware text, never a Python re-implementation.

Locally a missing compiler skips the test; under CI (``CI`` set) it fails, so
the gate can never be silently disabled.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable

import pytest

from tests.lib.srcread import REPO_ROOT, read_source

STUBS_DIR = Path(__file__).resolve().parent / "stubs"


def find_compiler(cxx: bool = False) -> str | None:
    names = ("g++", "clang++") if cxx else ("gcc", "clang")
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return None


def require_compiler(cxx: bool = False) -> str:
    cc = find_compiler(cxx)
    if cc:
        return cc
    msg = f"no {'C++' if cxx else 'C'} compiler on PATH (gcc/clang)"
    if os.environ.get("CI"):
        pytest.fail(msg + " - firmware_host tests must run in CI")
    pytest.skip(msg)


def _skip_literal(src: str, i: int, quote: str) -> int:
    i += 1
    while i < len(src):
        if src[i] == "\\":
            i += 2
            continue
        if src[i] == quote:
            return i + 1
        i += 1
    raise ValueError("unterminated literal")


def _match_brace(src: str, open_idx: int) -> int:
    """Index just past the brace matching ``src[open_idx] == '{'``, ignoring
    braces inside comments, string and char literals."""
    depth = 0
    i = open_idx
    while i < len(src):
        c = src[i]
        if src.startswith("//", i):
            nl = src.find("\n", i)
            i = len(src) if nl < 0 else nl
            continue
        if src.startswith("/*", i):
            i = src.index("*/", i + 2) + 2
            continue
        if c in "\"'":
            i = _skip_literal(src, i, c)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced braces")


def extract_function(path: str | Path, signature_regex: str) -> str:
    """Return the full text of the function whose signature matches
    ``signature_regex`` (searched with re.M), from the start of that line to
    its closing brace."""
    src = read_source(path)
    m = re.search(signature_regex, src, re.M)
    if not m:
        raise LookupError(f"{signature_regex!r} not found in {path}")
    start = src.rfind("\n", 0, m.start()) + 1
    open_idx = src.index("{", m.end())
    return src[start:_match_brace(src, open_idx)]


def extract_defines(path: str | Path, names: Iterable[str]) -> str:
    """Return the ``#define`` lines for ``names`` from a header, in file order,
    so the compiler (not Python) evaluates any expression they contain."""
    src = read_source(path)
    wanted = set(names)
    lines = []
    for line in src.splitlines():
        m = re.match(r"\s*#define\s+(\w+)\b", line)
        if m and m.group(1) in wanted:
            lines.append(line.split("//")[0].rstrip())
            wanted.discard(m.group(1))
    if wanted:
        raise LookupError(f"defines {sorted(wanted)} not found in {path}")
    return "\n".join(lines)


def compile_and_run(
    main_src: str,
    *,
    sources: Iterable[str | Path] = (),
    cxx: bool = False,
    include_dirs: Iterable[str | Path] = (),
    defines: Iterable[str] = (),
    extra_flags: Iterable[str] = (),
    args: Iterable[str] = (),
) -> str:
    """Compile ``main_src`` (plus any repo ``sources``) and return the
    program's stdout. A compile or runtime failure raises AssertionError
    carrying the compiler/program output."""
    cc = require_compiler(cxx)
    flags = ["-O0", "-g", "-Wall", "-Wextra", "-Wno-unused-function",
             "-Wno-unused-parameter"]
    if sys.platform.startswith("linux"):
        flags += ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
    flags += list(extra_flags)
    incs = [STUBS_DIR, *include_dirs]
    with tempfile.TemporaryDirectory(prefix="fwhost_") as tmp:
        tmpdir = Path(tmp)
        main_file = tmpdir / ("main.cpp" if cxx else "main.c")
        main_file.write_text(main_src, encoding="utf-8")
        exe = tmpdir / ("prog.exe" if os.name == "nt" else "prog")
        cmd = [cc, *flags, *(f"-I{Path(p) if Path(p).is_absolute() else REPO_ROOT / p}" for p in incs),
               *(f"-D{d}" for d in defines), str(main_file),
               *(str(REPO_ROOT / s) for s in sources), "-o", str(exe)]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError(f"compile failed:\n{' '.join(cmd)}\n{build.stderr}")
        # Harness output is UTF-8; decode it explicitly (Windows defaults to cp1252).
        run = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=30,
                             encoding="utf-8", errors="replace")
        if run.returncode != 0:
            raise AssertionError(
                f"program exited {run.returncode}\nstdout:\n{run.stdout}\nstderr:\n{run.stderr}")
        return run.stdout
