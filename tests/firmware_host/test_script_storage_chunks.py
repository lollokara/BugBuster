"""Spec 2026-10-03 §2: BLE uploads arrive as `files/chunk {name, off, b64, final}`
writes into a temp file that becomes /scripts/<name> on `final`.
script_storage.cpp is plain POSIX, so it is compiled on the host against a
temporary SCRIPTS_BASE directory."""

from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

SRC = "Firmware/ESP32/src/mp/script_storage.cpp"
INC = ["Firmware/ESP32/src/mp"]

MAIN = r"""
#include <cstdio>
#include <cstring>
#include "script_storage.h"
static void show(const char *tag, bool ok, uint32_t total, const char *err) {
    std::printf("%s %d %u %s\n", tag, ok ? 1 : 0, (unsigned)total, ok ? "-" : err);
}
int main() {
    char err[96]; uint32_t total = 0; bool ok;
    const uint8_t a[] = "print(1)\n", b[] = "print(2)\n";

    ok = script_storage_chunk_write("up.py", 0, a, 9, false, &total, err, sizeof err); show("c0", ok, total, err);
    ok = script_storage_chunk_write("up.py", 5, b, 9, false, &total, err, sizeof err); show("bad_off", ok, total, err);
    ok = script_storage_chunk_write("other.py", 9, b, 9, false, &total, err, sizeof err); show("bad_name", ok, total, err);

    char names[8][SCRIPT_NAME_MAX + 1];
    std::printf("listed %d\n", script_storage_list(names, 8));    /* temp file hidden */

    ok = script_storage_chunk_write("up.py", 9, b, 9, true, &total, err, sizeof err); show("final", ok, total, err);
    std::printf("listed %d %s\n", script_storage_list(names, 8), names[0]);

    /* restart from 0 after an abandoned upload */
    ok = script_storage_chunk_write("r.py", 0, a, 4, false, &total, err, sizeof err); show("r0", ok, total, err);
    ok = script_storage_chunk_write("r.py", 0, b, 9, true, &total, err, sizeof err); show("r_restart", ok, total, err);

    /* oversize */
    static uint8_t big[SCRIPT_BODY_MAX];
    std::memset(big, 'x', sizeof big);
    ok = script_storage_chunk_write("big.py", 0, big, sizeof big, false, &total, err, sizeof err); show("big0", ok, total, err);
    ok = script_storage_chunk_write("big.py", SCRIPT_BODY_MAX, a, 1, false, &total, err, sizeof err); show("big1", ok, total, err);
    ok = script_storage_chunk_write("big.py", SCRIPT_BODY_MAX, a, 1, false, &total, err, sizeof err); show("big2", ok, total, err);

    /* empty final */
    ok = script_storage_chunk_write("e.py", 0, a, 0, true, &total, err, sizeof err); show("empty", ok, total, err);
    ok = script_storage_chunk_write(".hidden.py", 0, a, 1, false, &total, err, sizeof err); show("badname", ok, total, err);
    return 0;
}
"""


def _run(tmp_path: Path) -> list[str]:
    out = compile_and_run(MAIN, cxx=True, sources=[SRC], include_dirs=INC,
                          defines=[f'SCRIPTS_BASE="{tmp_path}"'])
    return out.splitlines()


def test_sequential_chunks_commit_on_final(tmp_path):
    lines = _run(tmp_path)
    assert lines[0] == "c0 1 9 -"
    assert lines[3] == "listed 0"
    assert lines[4] == "final 1 18 -"
    assert lines[5] == "listed 1 up.py"
    assert (tmp_path / "up.py").read_text() == "print(1)\nprint(2)\n"
    assert not (tmp_path / ".upload.tmp").exists()


def test_wrong_offset_or_name_is_rejected(tmp_path):
    lines = _run(tmp_path)
    assert lines[1] == "bad_off 0 0 offset mismatch: expected 9"
    assert lines[2].startswith("bad_name 0 0 no upload in progress for other.py")


def test_offset_zero_restarts(tmp_path):
    lines = _run(tmp_path)
    assert lines[6] == "r0 1 4 -"
    assert lines[7] == "r_restart 1 9 -"
    assert (tmp_path / "r.py").read_text() == "print(2)\n"


def test_oversize_and_empty_and_bad_names(tmp_path):
    lines = _run(tmp_path)
    assert lines[8] == "big0 1 32768 -"
    assert lines[9] == "big1 0 0 script too large (max 32768)"
    assert lines[10].startswith("big2 0 0 no upload in progress"), "oversize must drop the temp file"
    assert lines[11] == "empty 0 0 empty script"
    assert lines[12] == "badname 0 0 invalid script name"
