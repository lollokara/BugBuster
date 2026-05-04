# BugBuster — Release checklist

Three components ship under independent tags, each validated by a dedicated
CI workflow. **All three validators must pass locally before pushing tags** —
otherwise the workflow fails before building any artifacts.

---

## Tag scheme

| Component | Tag prefix | CI workflow | Version-source script |
|---|---|---|---|
| RP2040 HAT firmware | `hat-fw-v*` | `.github/workflows/rp2040-firmware.yml` | `Firmware/tools/firmware_version.py rp2040` |
| ESP32 BBP firmware | `esp-fw-v*` | `.github/workflows/esp32-firmware.yml` | `Firmware/tools/firmware_version.py esp32` |
| Desktop Tauri app | `desktop-v*` | `.github/workflows/desktop-release.yml` | `DesktopApp/BugBuster/scripts/desktop_version.py --check` |

The CI step *Validate tag matches firmware version* strips the prefix from the
pushed tag and asserts the helper returns the same string.
**Mismatch → workflow fails → no release artifacts produced.**

---

## Files to edit per component

### RP2040 (single file — automated)

1. **`Firmware/RP2040/CMakeLists.txt`** — `set(PROBE_VERSION "bb-hat-X.Y")`.
   This is the **only** file you need to edit. It is the canonical source that:
   - CI reads and validates against the pushed tag.
   - Gets written into the generated `version.h` (USB product string / debugprobe).
   - Is parsed by CMake to inject `BB_HAT_FW_MAJOR` / `BB_HAT_FW_MINOR` as
     compile definitions, so `bb_main.c` picks them up automatically via
     `#ifndef` fallback guards — no manual edit required in that file.

`Firmware/RP2040/src/bb_main.c` retains `#ifndef BB_HAT_FW_MAJOR/MINOR`
fallback guards for IDE / out-of-CMake builds. Do **not** add bare
(non-guarded) `#define` lines there — `firmware_version.py rp2040` will
detect and reject a mismatch.

Run to verify before tagging:
```bash
python3 Firmware/tools/firmware_version.py rp2040 --expect X.Y
```

### Combined release (all three components)

A `release-v*` tag triggers `.github/workflows/release.yml`, which orchestrates
the three component CI workflows in parallel and publishes a single GitHub Release
bundling all Desktop installers, ESP32 firmware images, RP2040 UF2 artifacts, and
a top-level `SHA256SUMS.txt`.

### ESP32 (single file)

1. **`Firmware/ESP32/src/bbp.h`** — `BBP_FW_VERSION_MAJOR` /
   `_MINOR` / `_PATCH`. CI joins them with dots.

### Desktop (three files, validated in lockstep)

1. **`DesktopApp/BugBuster/Cargo.toml`** — top-level Leptos UI crate.
2. **`DesktopApp/BugBuster/src-tauri/Cargo.toml`** — Tauri backend crate.
3. **`DesktopApp/BugBuster/src-tauri/tauri.conf.json`** — JSON, no trailing
   commas.

Use `scripts/desktop_version.py X.Y.Z` (positional) to set all three at once
instead of editing by hand:

```bash
python3 DesktopApp/BugBuster/scripts/desktop_version.py 0.5.0
```

---

## Pre-push checklist

Run from repo root. All three must print the expected version and exit 0:

```bash
python Firmware/tools/firmware_version.py rp2040 --expect 2.0
python Firmware/tools/firmware_version.py esp32  --expect 3.0.0
python DesktopApp/BugBuster/scripts/desktop_version.py --check --expect 0.5.0
```

Only then create and push the tags:

```bash
git tag -a hat-fw-v2.0    -m "RP2040 HAT firmware v2.0"    <commit>
git tag -a esp-fw-v3.0.0  -m "ESP32 BBP firmware v3.0.0"   <commit>
git tag -a desktop-v0.5.0 -m "BugBuster Desktop v0.5.0"    <commit>
git push origin hat-fw-v2.0 esp-fw-v3.0.0 desktop-v0.5.0
```

---

## Protocol version sync (BBP wire)

`BBP_PROTO_VERSION` / `PROTO_VERSION` is **separate** from the firmware
release version. Bump it whenever the BBP wire protocol changes, in lockstep
across three files:

| File | Constant |
|---|---|
| `Firmware/ESP32/src/bbp.h` | `BBP_PROTO_VERSION` |
| `python/bugbuster/protocol.py` | `BBP_PROTO_VERSION` |
| `DesktopApp/BugBuster/src-tauri/src/bbp.rs` | `PROTO_VERSION` |

A CI gate enforces sync on every push and PR
(`.github/workflows/proto-version-check.yml`). Run the same check locally
before bumping the version:

```bash
python Firmware/tools/check_proto_version.py --verbose
```

If the check fails, update all three files to the same integer, then re-run
to confirm before pushing.

Current value: **`4`** (introduced 2026-04 with 14-byte handshake + MAC).

---

## Recovering from a failed tag-validation CI run

If a tag was pushed before the version files were synced:

1. Fix the missing version bump on `main`, commit, push.
2. Force-move the failing tag:
   ```bash
   git tag -f <tag> <new-commit>
   git push --force origin <tag>
   ```
3. CI re-fires automatically because GitHub treats the force-pushed tag as a
   new tag-push event.

Force-moving a tag is normally destructive, but for release tags whose CI
failed *before* producing any draft release, no one depends on the old SHA
yet. **Do not** force-move a tag whose draft release has been edited or whose
binaries have been distributed.

---

## RP2040 version — single source of truth (resolved 2026-05-04)

Previously, `PROBE_VERSION` in `CMakeLists.txt` and the `BB_HAT_FW_MAJOR/MINOR`
literals in `bb_main.c` had to be kept in sync manually. This is now automated:

- `CMakeLists.txt` parses `PROBE_VERSION "bb-hat-X.Y"` with CMake
  `string(REGEX MATCH ...)` and injects `BB_HAT_FW_MAJOR=X` / `BB_HAT_FW_MINOR=Y`
  as compile definitions via `target_compile_definitions`.
- `bb_main.c` uses `#ifndef` guards as fallback values for non-CMake builds.
- `firmware_version.py rp2040` detects any stale bare `#define` in `bb_main.c`
  that disagrees with CMakeLists and exits non-zero with a clear error message.

**To bump the version:** edit only `PROBE_VERSION` in `CMakeLists.txt`.
