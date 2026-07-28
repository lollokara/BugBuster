"""Every HATP_CMD_* the board callback (daq_board.c's s3_cmd_handler) knows how
to handle must actually be reachable from the S3 link — i.e. it must appear as
a `case HATP_CMD_*:` label in s3_link.c's handle_frame() allow-list switch.

handle_frame() ends in `default: send_error()`; a command implemented in
daq_board.c but missing from that switch is dead code that silently rejects
every request for it (HATP_RSP_ERROR) instead of ever reaching the handler.
This exact class of bug shipped once already: HATP_CMD_DAQ_WIFI_STREAM_RECYCLE
(0x79) was fully implemented in daq_board.c and declared in s3_link.h, but was
never added to handle_frame()'s case list, so the iOS recovery ladder's
rung-3 "recycle" escape hatch never actually reached the P4 -- it always hit
the allow-list's default branch and came back as an error. A test that only
checks "the handler function exists" or "the HTTP route exists" (as the
original test_daq_wifi_recycle.py did) cannot catch this: the handler was
present, just unreachable. This test parses the real dispatch surface instead
of searching for isolated strings.
"""
import re
from pathlib import Path

BOARD = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c").read_text()
S3LINKC = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.c").read_text()


def _extract_function_body(src: str, name: str) -> str:
    """Return the full braced body of the first function named `name`,
    found by balancing braces from its opening `{` (robust to reformatting,
    unlike a fixed line-range or single-line regex)."""
    m = re.search(re.escape(name) + r"\s*\([^;]*?\)\s*\{", src, re.DOTALL)
    assert m, f"could not locate function {name}() in source"
    start = m.end() - 1  # index of the opening brace
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise AssertionError(f"unbalanced braces while scanning {name}()")


def test_every_board_handled_command_is_reachable_from_s3_link_dispatch():
    board_body = _extract_function_body(BOARD, "s3_cmd_handler")
    link_body = _extract_function_body(S3LINKC, "handle_frame")

    board_cases = set(re.findall(r"case\s+(HATP_CMD_\w+)\s*:", board_body))
    link_cases = set(re.findall(r"case\s+(HATP_CMD_\w+)\s*:", link_body))

    assert board_cases, "sanity check: expected to find case labels in s3_cmd_handler"
    assert link_cases, "sanity check: expected to find case labels in handle_frame"

    unreachable = board_cases - link_cases
    assert not unreachable, (
        f"{sorted(unreachable)} are handled in daq_board.c's s3_cmd_handler but "
        "missing from s3_link.c's handle_frame() allow-list switch -- the S3 can "
        "never reach them (handle_frame falls through to `default: send_error()`), "
        "so the handler is dead code no matter how correct it is. Add each one to "
        "the appropriate case list in handle_frame()."
    )
