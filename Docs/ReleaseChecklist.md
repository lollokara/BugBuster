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

### RP2040 (two files — keep them synchronised)

1. **`Firmware/RP2040/CMakeLists.txt`** — `PROBE_VERSION "bb-hat-X.Y"`.
   This is the **canonical** source that CI reads and that ends up in the
   USB product string / debugprobe version report.
2. **`Firmware/RP2040/src/bb_main.c`** — `BB_HAT_FW_MAJOR` / `BB_HAT_FW_MINOR`.
   What the RP2040 reports to the ESP32 over the HAT UART `GET_INFO` response.
   Two integers, no patch field.

CI only checks (1). A stale (2) means `hat_get_status()` reports a wrong
firmware string, which is still a regression even if CI is happy. Always
update both.

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

There is **no CI gate** for this constant — a mismatch is silent and manifests
as confusing stream / handshake failures. Always grep all three when changing
the protocol version:

```bash
grep -n "PROTO_VERSION" \
  Firmware/ESP32/src/bbp.h \
  python/bugbuster/protocol.py \
  DesktopApp/BugBuster/src-tauri/src/bbp.rs
```

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

## Why two RP2040 files exist

The `PROBE_VERSION` string in `CMakeLists.txt` was inherited from the upstream
debugprobe project — it ends up in the USB descriptor as part of the product
string and is what host tools see. The `BB_HAT_FW_MAJOR/MINOR` constants in
`bb_main.c` are BugBuster-specific and live in the HAT UART `GET_INFO`
response. They were added separately and never unified. Future cleanup:
define `BB_HAT_FW_MAJOR` / `BB_HAT_FW_MINOR` in a shared header (e.g.
`bb_version.h`) generated from CMakeLists, so a single CMake change updates
both. Low-priority TODO.
