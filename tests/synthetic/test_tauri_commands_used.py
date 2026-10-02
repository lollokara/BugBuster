"""DESK-REFAC: every Tauri command the backend registers must be invoked by the
frontend (Rust `try_invoke("name"` / `invoke("name"` or index.html). Dead
commands were removed instead of carried as untested surface.

A: 25 registered commands had no caller."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "DesktopApp" / "BugBuster"


def _registered() -> list:
    lib = (ROOT / "src-tauri" / "src" / "lib.rs").read_text(encoding="utf-8")
    block = lib[lib.index("generate_handler!["):]
    block = block[: block.index("])")]
    block = re.sub(r"//[^\n]*", "", block)
    return [p.split("::")[-1] for p in re.findall(r"[a-z_][\w:]*", block) if p != "generate_handler"]


def _invoked() -> set:
    # Commands are invoked through several wrappers (try_invoke, invoke_and_log,
    # send_*), always with the name as a string literal.
    text = "\n".join(p.read_text(encoding="utf-8") for p in (ROOT / "src").rglob("*.rs"))
    text += (ROOT / "index.html").read_text(encoding="utf-8")
    return set(re.findall(r"""["']([a-z0-9_]+)["']""", text))


def test_every_registered_command_has_a_frontend_caller():
    dead = [c for c in _registered() if c not in _invoked()]
    assert dead == []
