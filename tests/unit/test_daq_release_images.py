"""DAQ HAT release-image format tests (sub-project 2: CI / manifest).

The ESP32-P4 and ESP32-C6 take different image formats and swapping them is the
most damaging mistake this pipeline can make:

  * A merged image published as the P4 asset is streamed into
    ``esp_ota_write()`` and lands in an A/B slot that will not boot.
  * An app-only image published as the C6 asset is written from flash offset 0
    by the ESP-ROM loader (``relay_c6_push()`` -> ``c6_flasher_begin(size, 0)``),
    laying app bytes over the C6 bootloader. There is no A/B slot and no second
    MCU to recover it: the chip is bricked.

Both formats begin with the same 0xE9 magic byte, so these tests exist above all
to pin down that the verifier discriminates on something stronger than magic.
"""
from __future__ import annotations

import importlib.util
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "Firmware" / "tools"
WORKFLOWS = REPO_ROOT / ".github" / "workflows"


def _load_tool():
    spec = importlib.util.spec_from_file_location(
        "esp_image_tool", TOOLS / "esp_image_tool.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = _load_tool()


# --------------------------------------------------------------------------
# Synthetic images
# --------------------------------------------------------------------------

APP_LEN = 1_200_000  # a realistic DAQ HAT app: comfortably larger than 0x10000


def make_app() -> bytes:
    return b"\xe9" + b"\x5a" * (APP_LEN - 1)


def make_bootloader() -> bytes:
    return b"\xe9" + b"\x11" * (0x7000 - 1)


def make_partition_table() -> bytes:
    return b"\xaa\x50" + b"\x22" * (0xC00 - 2)


def make_merged(app: bytes | None = None) -> bytes:
    app = app if app is not None else make_app()
    bl, pt = make_bootloader(), make_partition_table()
    image = bytearray(b"\xff" * (tool.APP_OFFSET + len(app)))
    image[0:len(bl)] = bl
    image[tool.PARTITIONS_OFFSET:tool.PARTITIONS_OFFSET + len(pt)] = pt
    image[tool.APP_OFFSET:tool.APP_OFFSET + len(app)] = app
    return bytes(image)


# --------------------------------------------------------------------------
# The discriminator
# --------------------------------------------------------------------------

def test_app_only_and_merged_share_the_same_magic_byte():
    """The premise: a magic-byte check cannot tell the two formats apart."""
    app_only = make_app()
    merged = make_merged()
    assert app_only[0] == merged[0] == tool.ESP_IMAGE_MAGIC


def test_length_alone_does_not_discriminate():
    """A real app-only image is far larger than the 0x10000 app offset.

    So `len(data) > 0x10000` is necessary for a merged image but proves
    nothing on its own -- if this ever becomes false the verifier has been
    weakened to a test that a realistic bad input would pass.
    """
    app_only = make_app()
    assert len(app_only) > tool.APP_OFFSET
    assert not tool.looks_merged(app_only)


def test_merged_image_is_recognised():
    assert tool.looks_merged(make_merged())
    tool.verify_merged(make_merged())


def test_app_only_image_is_recognised():
    tool.verify_app_only(make_app())


# --------------------------------------------------------------------------
# The two catastrophic swaps
# --------------------------------------------------------------------------

def test_app_only_image_rejected_as_c6_merged_asset():
    """The brick case: app-only published as the C6 asset."""
    with pytest.raises(tool.ImageError, match="brick"):
        tool.verify_merged(make_app())


def test_short_app_rejected_as_c6_merged_asset():
    """A sub-0x10000 image can never be merged; the message must say why."""
    with pytest.raises(tool.ImageError, match="app offset"):
        tool.verify_merged(b"\xe9" + b"\x00" * (tool.APP_OFFSET - 1))


def test_merged_image_rejected_as_p4_app_only_asset():
    with pytest.raises(tool.ImageError, match="MERGED image"):
        tool.verify_app_only(make_merged())


def test_truncated_app_rejected():
    with pytest.raises(tool.ImageError, match="Truncated"):
        tool.verify_app_only(b"\xe9" + b"\x00" * 1024)


def test_non_esp_image_rejected_both_ways():
    junk = b"\x00" * (APP_LEN)
    with pytest.raises(tool.ImageError):
        tool.verify_app_only(junk)
    with pytest.raises(tool.ImageError):
        tool.verify_merged(junk)


def test_merged_image_missing_partition_table_rejected():
    """Length and both magic bytes right, partition table absent."""
    broken = bytearray(make_merged())
    broken[tool.PARTITIONS_OFFSET:tool.PARTITIONS_OFFSET + 2] = b"\xff\xff"
    with pytest.raises(tool.ImageError, match="ESP_PARTITION_MAGIC"):
        tool.verify_merged(bytes(broken))


# --------------------------------------------------------------------------
# Merge layout
# --------------------------------------------------------------------------

def test_make_merged_image_places_each_part_at_its_flash_offset(tmp_path):
    bl, pt, app = make_bootloader(), make_partition_table(), make_app()
    (tmp_path / "bootloader.bin").write_bytes(bl)
    (tmp_path / "partitions.bin").write_bytes(pt)
    (tmp_path / "firmware.bin").write_bytes(app)

    image = tool.make_merged_image(tmp_path)

    assert image[0:len(bl)] == bl
    assert image[tool.PARTITIONS_OFFSET:tool.PARTITIONS_OFFSET + len(pt)] == pt
    assert image[tool.APP_OFFSET:] == app
    # Gaps are erased flash, not zeroes.
    assert set(image[len(bl):tool.PARTITIONS_OFFSET]) == {0xFF}
    tool.verify_merged(image)


def test_make_merged_image_reports_missing_build_outputs(tmp_path):
    (tmp_path / "firmware.bin").write_bytes(make_app())
    with pytest.raises(tool.ImageError, match="bootloader"):
        tool.make_merged_image(tmp_path)


# --------------------------------------------------------------------------
# Workflow wiring
#
# Source-scanning: these guard the wiring that the image tests cannot reach.
# --------------------------------------------------------------------------

def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_daq_workflow_builds_both_chips():
    text = _read(WORKFLOWS / "daq-hat-firmware.yml")
    assert "pio run -d Firmware/DAQ_HAT/ESP32P4 -e esp32p4" in text
    assert "pio run -d Firmware/DAQ_HAT/ESP32C6 -e esp32c6" in text


def test_daq_workflow_publishes_the_correct_format_per_chip():
    text = _read(WORKFLOWS / "daq-hat-firmware.yml")
    # P4 asset is the raw app binary; C6 asset goes through the merge step.
    assert re.search(
        r'cp "\$p4_build/firmware\.bin" .*bugbuster-daq-p4-v\$\{p4_version\}-ota\.bin',
        text,
    ), "P4 asset must be the app-only firmware.bin, copied unmodified"
    assert re.search(
        r"esp_image_tool\.py merge\s+\\\s*\n\s*--build-dir \"\$c6_build\"\s+\\\s*\n\s*"
        r"--out .*bugbuster-daq-c6-v\$\{c6_version\}-merged\.bin",
        text,
    ), "C6 asset must be produced by the merge step, not copied from firmware.bin"


def test_daq_workflow_verifies_both_assets_before_upload():
    text = _read(WORKFLOWS / "daq-hat-firmware.yml")
    assert "verify --format app-only" in text
    assert "verify --format merged" in text
    # Verification must precede the upload, or a bad asset still ships.
    assert text.index("Verify image formats") < text.index("Upload DAQ HAT firmware artifacts")


def test_c6_asset_name_carries_the_merged_marker():
    """`-merged` in the name is a second line of defence for hand-uploads."""
    text = _read(WORKFLOWS / "daq-hat-firmware.yml")
    assert "bugbuster-daq-c6-v${c6_version}-merged.bin" in text
    release = _read(WORKFLOWS / "release.yml")
    assert "bugbuster-daq-c6-v*-merged.bin" in release


def test_release_workflow_gates_publish_on_the_daq_job():
    text = _read(WORKFLOWS / "release.yml")
    assert "needs: [prep, rp2040, esp32, daq, desktop]" in text
    assert "needs.daq.result == 'success' || needs.daq.result == 'skipped'" in text


def test_release_workflow_detects_daq_changes():
    text = _read(WORKFLOWS / "release.yml")
    assert "Firmware/DAQ_HAT/" in text
    assert 'build_daq="false"' in text
    assert 'echo "build_daq=$build_daq" >> "$GITHUB_OUTPUT"' in text


def test_manifest_omits_daq_keys_when_no_asset_is_staged():
    """Absence must omit the key, never carry a stale URL forward.

    A manifest entry whose asset is not in this release points the S3 at a
    404 mid-update; `p4`/`c6` are optional in the schema precisely so the key
    can be dropped instead.
    """
    text = _read(WORKFLOWS / "release.yml")
    assert "def pick_optional(pattern):" in text
    assert 'p4 = pick_optional("bugbuster-daq-p4-v*-ota.bin")' in text
    assert 'c6 = pick_optional("bugbuster-daq-c6-v*-merged.bin")' in text
    assert 'omitting the manifest \'p4\' key' in text
    assert 'omitting the manifest \'c6\' key' in text


def test_manifest_reverifies_daq_asset_formats():
    """Staged assets may be carried forward or hand-replaced, so re-check."""
    text = _read(WORKFLOWS / "release.yml")
    assert "esp_image_tool.verify_app_only(open(p4" in text
    assert "esp_image_tool.verify_merged(open(c6" in text


def test_archive_manifest_rewrites_every_component_url():
    """The archived nightly must not link back to the rolling `nightly` tag.

    Its assets are deleted on the next run, so any component left out of this
    rewrite loop yields an archived manifest with a dead URL.
    """
    text = _read(WORKFLOWS / "release.yml")
    assert 'for component in ("rp2040", "esp32", "spiffs", "p4", "c6"):' in text


def test_firmware_version_tool_knows_both_daq_chips():
    text = _read(TOOLS / "firmware_version.py")
    assert '"p4":' in text and '"c6":' in text
    assert (REPO_ROOT / "Firmware/DAQ_HAT/ESP32C6/include/version.h").is_file()
