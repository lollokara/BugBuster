"""PY-21 characterisation: the public ``BugBuster`` surface (every public member
and its signature) must not change during the client refactor. Golden file:
tests/fixtures/public_api.json. Regenerate ONLY for an intended API change:

    python -m tests.unit.test_public_api_snapshot --write
"""

import inspect
import json
import sys
from pathlib import Path

GOLDEN = Path(__file__).resolve().parents[1] / "fixtures" / "public_api.json"


def snapshot() -> dict:
    from bugbuster.client import BugBuster

    out = {}
    for name, member in sorted(inspect.getmembers(BugBuster)):
        if name.startswith("_"):
            continue
        if isinstance(inspect.getattr_static(BugBuster, name), property):
            out[name] = "property"
            continue
        if callable(member):
            try:
                out[name] = str(inspect.signature(member))
            except (TypeError, ValueError):
                out[name] = "callable"
        else:
            out[name] = type(member).__name__
    return out


def test_public_api_matches_golden():
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    current = snapshot()
    removed = sorted(set(golden) - set(current))
    added = sorted(set(current) - set(golden))
    changed = sorted(k for k in set(golden) & set(current) if golden[k] != current[k])
    assert not (removed or added or changed), (
        f"public API drift: removed={removed} added={added} changed={changed}")


if __name__ == "__main__" and "--write" in sys.argv:
    GOLDEN.write_text(json.dumps(snapshot(), indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {GOLDEN}")
